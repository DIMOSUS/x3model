#include "scene_fbx.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include "archive.hpp"
#include "bob.hpp"
#include "bytes.hpp"
#include "scene_anim.hpp"
#include "textbody.hpp"
#include "ufbx.h"
#include "util.hpp"

namespace x3 {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFix = 65536.0;

struct Q {
  double w = 1, x = 0, y = 0, z = 0;
};
Q Mul(Q a, Q b) {  // Math::QuatMultiplyFixed operand order: a = the accumulated one
  return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y + a.y * b.w + a.z * b.x - a.x * b.z, a.w * b.z + a.z * b.w + a.x * b.y - a.y * b.x};
}
Q Normalised(Q q) {
  const double l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  return l > 0 ? Q{q.w / l, q.x / l, q.y / l, q.z / l} : Q{};
}

// The game is left-handed and FBX right-handed (Z mirrored). A rotation R of the game becomes M R M in FBX
// (M = diag(1, 1, -1)); for a quaternion that negates x and y. Whether the engine applies its key rotation to row
// vectors (the Direct3D habit: the effective rotation is the transpose) was settled on the scene
// stations/docks/dock9portsscene: with row vectors its five docking arms point at the hub in a regular five-fold
// pattern, with column vectors they scatter (docs/formats.md, "Scenes").
constexpr bool RowVectors() { return true; }
Q GameToFbx(Q q) {
  if (RowVectors()) q = {q.w, -q.x, -q.y, -q.z};  // effective rotation = transpose = conjugate
  return {q.w, -q.x, -q.y, q.z};
}
Q FbxToGame(Q q) {
  q = {q.w, -q.x, -q.y, q.z};
  if (RowVectors()) q = {q.w, -q.x, -q.y, -q.z};
  return q;
}

// FBX Euler XYZ (degrees; the matrix is Rz * Ry * Rx for column vectors) <-> quaternion
void QuatToEuler(Q q, double out[3]) {
  q = Normalised(q);
  const double m00 = 1 - 2 * (q.y * q.y + q.z * q.z), m10 = 2 * (q.x * q.y + q.w * q.z), m20 = 2 * (q.x * q.z - q.w * q.y);
  const double m21 = 2 * (q.y * q.z + q.w * q.x), m22 = 1 - 2 * (q.x * q.x + q.y * q.y);
  const double m01 = 2 * (q.x * q.y - q.w * q.z), m11 = 1 - 2 * (q.x * q.x + q.z * q.z);
  const double sy = -m20;
  double rx, ry, rz;
  if (std::fabs(sy) < 0.9999999) {
    ry = std::asin(std::clamp(sy, -1.0, 1.0));
    rx = std::atan2(m21, m22);
    rz = std::atan2(m10, m00);
  } else {  // gimbal lock: rz = 0, then R = Ry * Rx and m01 = sin(ry) sin(rx), m11 = cos(rx)
    ry = sy > 0 ? kPi / 2 : -kPi / 2;
    rx = std::atan2(sy > 0 ? m01 : -m01, m11);
    rz = 0;
  }
  out[0] = rx * 180 / kPi, out[1] = ry * 180 / kPi, out[2] = rz * 180 / kPi;
}

// Per key: the engine's rotation quaternion (Scene::BuildRotationKeyQuats: axis-angle keys, axis stored negated,
// half angle from the 16-bit turn, composed with the previous key unless flag 0x2000), or the previous one for keys
// of another kind. Three-angle keys (no flag 2) are taken as yaw (Y), pitch (X), roll (Z) - an inference.
// one key applied to the rotation accumulated so far: the rotation after this key
Q StepRotation(const Q& run, const scene::Key& k) {
  const uint32_t f = k.flags;
  if (f & 8) return run;  // target mode: the rotation comes from the target
  Q q;
  if (f & 2) {
    if (!(f & 0x20)) {
      const double ax = -k.Get("ax") / kFix, ay = -k.Get("ay") / kFix, az = -k.Get("az") / kFix;
      if (ax != 0 || ay != 0 || az != 0) {
        const double h = ((k.Get("angle") / 2) & 0xffff) / kFix * 2 * kPi;  // half angle, 16-bit turns
        const double s = std::sin(h);
        q = {std::cos(h), ax * s, ay * s, az * s};
      }
    }
    if (!(f & 0x2000)) q = Mul(run, q);
  } else if (!(f & 0x20) && !(f & 0x200) && k.Has("yaw")) {
    auto turn = [](int32_t v) { return (v & 0xffff) / kFix * 2 * kPi; };
    const double a = turn(k.Get("yaw")), b = turn(k.Get("pitch")), c = turn(k.Get("roll3"));
    const Q qy{std::cos(a / 2), 0, std::sin(a / 2), 0}, qx{std::cos(b / 2), std::sin(b / 2), 0, 0},
        qz{std::cos(c / 2), 0, 0, std::sin(c / 2)};
    q = Mul(Mul(qy, qx), qz);
  } else {
    q = run;
  }
  return q;
}
std::vector<Q> EngineQuats(const scene::Part& p) {
  std::vector<Q> out;
  Q run;
  for (auto& k : p.keys) out.push_back(run = StepRotation(run, k));
  return out;
}

struct Pose {
  double t[3];
  Q r;
};
// the rotation of an engine matrix (rows, 16.16; the standard matrix of the quaternion, as QuatToMatrixRows makes it)
Q MatrixQuat(const std::array<std::array<int32_t, 3>, 3>& mi) {
  double m[3][3];
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++) m[r][c] = mi[r][c] / kFix;
  const double tr = m[0][0] + m[1][1] + m[2][2];
  Q q;
  if (tr > 0) {
    const double s = std::sqrt(tr + 1) * 2;
    q = {s / 4, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s};
  } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
    const double s = std::sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
    q = {(m[2][1] - m[1][2]) / s, s / 4, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s};
  } else if (m[1][1] > m[2][2]) {
    const double s = std::sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
    q = {(m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, s / 4, (m[1][2] + m[2][1]) / s};
  } else {
    const double s = std::sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
    q = {(m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, s / 4};
  }
  return Normalised(q);
}

bool OwnRotation(uint32_t f) { return !(f & 8) && ((f & 2) || !(f & 0x20)) && !(f & 0x200 && !(f & 2)); }

// Per key: the engine's position (game units, FBX axes) and rotation (FBX) at the key's time. A key with its own
// position / axis-angle has its value (rotations composed in doubles); a key without one does not hold the previous
// value: the engine interpolates the track there (linear or TCB between the keys that have one), as it does for the
// orientation of a part with target or roll keys (cameras) at every time - those come from the engine evaluator.
std::vector<Pose> KeyPoses(const scene::Part& p) {
  const auto qs = EngineQuats(p);
  std::vector<Pose> out;
  const anim::Tracks tr = anim::Build(p);
  const bool aimed = !tr.target.empty() || !tr.roll.empty();
  const auto times = scene::KeyTimes(p);
  bool ordered = true;  // (keys out of time order: the previous value, as the export cannot show more)
  for (size_t i = 1; i < times.size(); i++) ordered &= times[i] >= times[i - 1];
  double pos[3] = {0, 0, 0};
  for (size_t i = 0; i < p.keys.size(); i++) {
    const auto& k = p.keys[i];
    const anim::State st = ordered ? anim::Evaluate(tr, times[i]) : anim::State{};
    if (k.Has("x")) pos[0] = k.Get("x"), pos[1] = k.Get("y"), pos[2] = k.Get("z");
    else if (st.pos) pos[0] = (*st.pos)[0], pos[1] = (*st.pos)[1], pos[2] = (*st.pos)[2];
    Q q = Normalised(qs[i]);  // (file axes are not exactly unit)
    if ((aimed || !OwnRotation(k.flags)) && st.m) q = MatrixQuat(*st.m);
    out.push_back({{pos[0], pos[1], -pos[2]}, GameToFbx(q)});
  }
  return out;
}

Q EulerToQuat(const double e[3]) {  // inverse of QuatToEuler: R = Rz * Ry * Rx
  auto axis = [](int k, double deg) {
    const double h = deg * kPi / 360;
    Q q{std::cos(h), 0, 0, 0};
    (&q.x)[k] = std::sin(h);
    return q;
  };
  return Mul(Mul(axis(2, e[2]), axis(1, e[1])), axis(0, e[0]));
}

// What the export writes for a part: the rest pose, and (when the part moves and its key times are in order) one
// linear curve key per stored key, Euler angles unwrapped from key to key.
struct ExportedAnim {
  double restT[3] = {0, 0, 0}, restE[3] = {0, 0, 0};
  bool moves = false;
  std::vector<double> time;                  // seconds
  std::vector<std::array<double, 3>> t, e;  // translation, Euler degrees
  // the curves' value at time s (seconds); the rest pose for a static part
  void Eval(double s, double outT[3], double outE[3]) const {
    if (!moves) {
      for (int k = 0; k < 3; k++) outT[k] = restT[k], outE[k] = restE[k];
      return;
    }
    size_t i = 0;
    while (i + 1 < time.size() && time[i + 1] <= s) i++;
    double f = 0;
    if (i + 1 < time.size() && s > time[i] && time[i + 1] > time[i]) f = (s - time[i]) / (time[i + 1] - time[i]);
    const size_t j = std::min(i + 1, time.size() - 1);
    for (int k = 0; k < 3; k++) outT[k] = t[i][k] + (t[j][k] - t[i][k]) * f, outE[k] = e[i][k] + (e[j][k] - e[i][k]) * f;
  }
};
ExportedAnim ExportAnim(const std::vector<Pose>& poses, const std::vector<int64_t>& keyTimes) {
  ExportedAnim a;
  if (poses.empty()) return a;
  for (int k = 0; k < 3; k++) a.restT[k] = poses[0].t[k];
  QuatToEuler(poses[0].r, a.restE);
  for (size_t i = 1; i < poses.size() && !a.moves; i++)
    for (int k = 0; k < 3; k++)
      a.moves |= poses[i].t[k] != poses[0].t[k] || std::fabs(poses[i].r.w - poses[0].r.w) > 1e-9 ||
                 std::fabs(poses[i].r.x - poses[0].r.x) > 1e-9 || std::fabs(poses[i].r.y - poses[0].r.y) > 1e-9 ||
                 std::fabs(poses[i].r.z - poses[0].r.z) > 1e-9;
  // keys out of time order (a negative dt; one shipped part) cannot be an FBX curve: rest pose only, the stored
  // keys stay in x3_keys
  for (size_t i = 1; i < keyTimes.size(); i++) a.moves &= keyTimes[i] >= keyTimes[i - 1];
  if (!a.moves) return a;
  double prev[3] = {a.restE[0], a.restE[1], a.restE[2]};
  for (size_t i = 0; i < poses.size(); i++) {
    double e[3];
    QuatToEuler(poses[i].r, e);
    for (int k = 0; k < 3; k++) {  // unwrap: continue from the previous key
      while (e[k] - prev[k] > 180) e[k] -= 360;
      while (e[k] - prev[k] < -180) e[k] += 360;
      prev[k] = e[k];
    }
    a.time.push_back(keyTimes[i] / 1000.0);
    a.t.push_back({poses[i].t[0], poses[i].t[1], poses[i].t[2]});
    a.e.push_back({e[0], e[1], e[2]});
  }
  return a;
}

// The baked form (export --bake-anim, a preview: DCCs show FBX keys linearly): the keys, plus between them samples
// at fps of the engine's motion (TCB, squad, camera targets) from the engine evaluator.
ExportedAnim BakeAnim(const scene::Part& p, const std::vector<Pose>& poses, const std::vector<int64_t>& keyTimes, double fps) {
  ExportedAnim a = ExportAnim(poses, keyTimes);
  if (!a.moves || fps <= 0) return a;
  const anim::Tracks tr = anim::Build(p);
  const int64_t end = anim::Length(tr);
  ExportedAnim b = a;
  b.time.clear(), b.t.clear(), b.e.clear();
  size_t j = 0;
  double prev[3] = {a.restE[0], a.restE[1], a.restE[2]};
  auto add = [&](double s, const double t[3], double e[3]) {
    for (int k = 0; k < 3; k++) {
      while (e[k] - prev[k] > 180) e[k] -= 360;
      while (e[k] - prev[k] < -180) e[k] += 360;
      prev[k] = e[k];
    }
    b.time.push_back(s), b.t.push_back({t[0], t[1], t[2]}), b.e.push_back({e[0], e[1], e[2]});
  };
  for (int64_t f = 0;; f++) {
    const double s = f / fps;
    // the keys up to this sample, as exported
    for (; j < a.time.size() && a.time[j] <= s + 1e-6; j++) {
      double e[3] = {a.e[j][0], a.e[j][1], a.e[j][2]};
      add(a.time[j], a.t[j].data(), e);
    }
    if (s * 1000 > end) break;
    if (!b.time.empty() && std::fabs(b.time.back() - s) < 1e-6) continue;
    const anim::State st = anim::Evaluate(tr, std::llround(s * 1000));
    double t[3] = {0, 0, 0}, e[3];
    if (st.pos) t[0] = (*st.pos)[0], t[1] = (*st.pos)[1], t[2] = -double((*st.pos)[2]);
    QuatToEuler(GameToFbx(st.m ? MatrixQuat(*st.m) : Q{}), e);
    add(s, t, e);
  }
  return b;
}

std::string KeysBlob(const scene::Part& p) {
  std::vector<uint8_t> b;
  PutI32(b, static_cast<int32_t>(p.keys.size()));
  for (auto& k : p.keys) {
    PutI32(b, static_cast<int32_t>(k.flags));
    for (int32_t v : k.values) PutI32(b, v);
  }
  return Base64(b);
}
std::vector<scene::Key> KeysFromBlob(const std::string& s) {
  const auto b = Unbase64(s);
  size_t at = 0;
  const int32_t n = GetI32(b, at);
  if (n < 0 || static_cast<size_t>(n) > b.size() / 12) throw FormatError("x3_keys does not decode");
  std::vector<scene::Key> keys(static_cast<size_t>(n));
  for (auto& k : keys) {
    k.flags = static_cast<uint32_t>(GetI32(b, at));
    for (size_t i = 0; i < scene::KeyLayout(k.flags).size(); i++) k.values.push_back(GetI32(b, at));
  }
  if (at != b.size()) throw FormatError("x3_keys does not decode");
  return keys;
}

bob::Body ParseBody(const std::vector<uint8_t>& d) {
  if (bob::IsBob(d)) return bob::Read(d);
  return textbody::Read(d);
}

}  // namespace

// ================================================================ export

fbx::Scene SceneToFbx(const scene::Scene& sc, const std::string& name, const ExportOptions& opt, const BodyLoader& load,
                      std::vector<std::pair<std::string, std::string>>* textures) {
  fbx::Scene out;
  out.unitCm = opt.unitCm;
  out.creator = "x3model (X3 scene " + name + ")";
  fbx::Node root;
  root.name = name;
  root.props.push_back(fbx::Prop::Str("x3_kind", "scene"));
  root.props.push_back(fbx::Prop::Int("x3_binary", sc.binary));
  root.props.push_back(fbx::Prop::Int("x3_version", sc.version));
  if (sc.info) root.props.push_back(fbx::Prop::Str("x3_info", *sc.info));
  root.props.push_back(fbx::Prop::Dbl("x3_unit_mm", opt.unitCm * 10));
  if (opt.bakeFps > 0) root.props.push_back(fbx::Prop::Dbl("x3_bake_fps", opt.bakeFps));
  out.nodes.push_back(root);

  std::map<uint32_t, int> nodeOfPart;
  std::set<std::string> usedNames;
  // body geometry, shared: body key -> (mesh, material slots) per LOD 0 part
  std::map<std::string, std::vector<std::pair<int, std::vector<int>>>> geometry;
  double animEnd = 0;

  for (size_t pi = 0; pi < sc.parts.size(); pi++) {
    const scene::Part& p = sc.parts[pi];
    fbx::Node n;
    std::string nm = p.name ? *p.name : "P" + std::to_string(p.index) + "_" + p.body;
    for (char& c : nm)
      if (c == '\\' || c == '/') c = '_';
    if (!usedNames.insert(nm).second) nm += "#" + std::to_string(p.index), usedNames.insert(nm);
    n.name = nm;
    auto it = nodeOfPart.find(static_cast<uint32_t>(p.parent));
    n.parent = p.parent >= 0 && it != nodeOfPart.end() ? it->second : 0;
    n.props.push_back(fbx::Prop::Int("x3_part", p.index));
    n.props.push_back(fbx::Prop::Int("x3_order", static_cast<int64_t>(pi)));
    n.props.push_back(fbx::Prop::Str("x3_body", p.body));
    n.props.push_back(fbx::Prop::Int("x3_mode", p.mode));
    n.props.push_back(fbx::Prop::Int("x3_parent", p.parent));
    n.props.push_back(fbx::Prop::Int("x3_flags", p.flags));
    if (p.name) n.props.push_back(fbx::Prop::Str("x3_name", *p.name));
    if (p.constraints) {
      std::string s = "n";
      for (auto& [a, b] : *p.constraints) s += " " + std::to_string(a) + ":" + std::to_string(b);
      n.props.push_back(fbx::Prop::Str("x3_constraints", s));
    }
    if (!p.notes.empty()) {
      std::string s;
      for (auto& [t, txt] : p.notes) s += std::to_string(t) + " " + txt + "\n";
      n.props.push_back(fbx::Prop::Str("x3_notes", s));
    }
    if (!p.embedded.empty()) n.props.push_back(fbx::Prop::Str("x3_embedded", Base64(p.embedded)));
    if (!p.inlineText.empty()) n.props.push_back(fbx::Prop::Str("x3_inline", p.inlineText));
    n.props.push_back(fbx::Prop::Str("x3_keys", KeysBlob(p)));

    // rest pose, and every key a curve key when the part moves
    const ExportedAnim anim = BakeAnim(p, KeyPoses(p), scene::KeyTimes(p), opt.bakeFps);
    for (int k = 0; k < 3; k++) n.translation[k] = anim.restT[k], n.rotation[k] = anim.restE[k];
    for (size_t i = 0; i < anim.time.size(); i++) {
      for (int k = 0; k < 3; k++) {
        n.animT[k].time.push_back(anim.time[i]), n.animT[k].value.push_back(anim.t[i][k]);
        n.animR[k].time.push_back(anim.time[i]), n.animR[k].value.push_back(anim.e[i][k]);
      }
      animEnd = std::max(animEnd, anim.time[i]);
    }
    const int nodeIndex = static_cast<int>(out.nodes.size());
    nodeOfPart[p.index] = nodeIndex;
    const int parentNode = n.parent;
    out.nodes.push_back(n);

    // a camera's target (keys with flag 8): a node beside the camera (the target is in the camera's parent space),
    // with a key at every target key; moving it moves the target keys
    const anim::Tracks tracks = anim::Build(p);
    if (!tracks.target.empty()) {
      fbx::Node tn;
      tn.name = nm + "_target";
      tn.parent = parentNode;
      tn.props.push_back(fbx::Prop::Int("x3_target_of", static_cast<int64_t>(pi)));
      const auto& t0 = tracks.target[0];
      tn.translation[0] = t0.v[0], tn.translation[1] = t0.v[1], tn.translation[2] = -double(t0.v[2]);
      bool moves = false;
      for (auto& k : tracks.target) moves |= k.v[0] != t0.v[0] || k.v[1] != t0.v[1] || k.v[2] != t0.v[2];
      for (size_t i = 1; i < tracks.target.size(); i++) moves &= tracks.target[i].t >= tracks.target[i - 1].t;
      if (moves)
        for (auto& k : tracks.target)
          for (int c = 0; c < 3; c++) {
            tn.animT[c].time.push_back(k.t / 1000.0);
            tn.animT[c].value.push_back(c == 2 ? -double(k.v[2]) : k.v[c]);
            animEnd = std::max(animEnd, k.t / 1000.0);
          }
      out.nodes.push_back(tn);
    }

    // the body's LOD 0 geometry under the part (x3_ref: not part of the scene data)
    std::string key;
    std::vector<uint8_t> bytes;
    if (!p.embedded.empty()) key = "embedded:" + std::to_string(pi), bytes = p.embedded;
    else if (!p.inlineText.empty()) key = "inline:" + std::to_string(pi), bytes.assign(p.inlineText.begin(), p.inlineText.end());
    else if (!(p.flags & 0x80) && load) key = Game::Key(p.body);  // (0x80: the body names a scene)
    if (key.empty()) continue;
    if (!geometry.count(key)) {
      auto& slots = geometry[key];
      try {
        if (bytes.empty()) bytes = load(p.body);
        if (!bytes.empty()) {
          const bob::Body body = ParseBody(bytes);
          std::string stem = p.body;
          const size_t sl = stem.find_last_of("/\\");
          if (sl != std::string::npos) stem = stem.substr(sl + 1);
          const fbx::Scene sub = BodyToFbx(body, stem, opt, textures);
          const int matBase = static_cast<int>(out.materials.size());
          for (auto& m : sub.materials) out.materials.push_back(m);
          for (auto& sn : sub.nodes) {
            if (sn.mesh < 0 || sn.parent < 0 || sub.nodes[sn.parent].name.find("_LOD0") == std::string::npos) continue;
            std::vector<int> ms;
            for (int s : sn.materials) ms.push_back(matBase + s);
            slots.push_back({static_cast<int>(out.meshes.size()), ms});
            out.meshes.push_back(sub.meshes[sn.mesh]);
          }
        }
      } catch (const std::exception&) {
        slots.clear();
      }
    }
    int k = 0;
    for (auto& [mesh, mats] : geometry[key]) {
      fbx::Node g;
      g.name = nm + "_geo" + std::to_string(k++);
      g.parent = nodeIndex;
      g.mesh = mesh;
      g.materials = mats;
      g.props.push_back(fbx::Prop::Str("x3_ref", p.body));
      out.nodes.push_back(g);
    }
  }
  out.animEnd = animEnd;
  return out;
}

// ================================================================ import

namespace {

std::string PStr(const ufbx_props& props, const char* name) {
  const ufbx_prop* p = ufbx_find_prop(&props, name);
  return p ? std::string(p->value_str.data, p->value_str.length) : "";
}
bool PHas(const ufbx_props& props, const char* name) { return ufbx_find_prop(&props, name) != nullptr; }
long long PInt(const ufbx_props& props, const char* name, long long def = 0) {
  const ufbx_prop* p = ufbx_find_prop(&props, name);
  if (!p) return def;
  if (p->type == UFBX_PROP_STRING) return std::stoll(std::string(p->value_str.data, p->value_str.length));
  return p->value_int ? p->value_int : static_cast<long long>(std::llround(p->value_real));
}

ufbx_scene* Load(const std::string& path) {
  ufbx_load_opts o{};
  o.target_axes = ufbx_axes_right_handed_y_up;
  o.target_unit_meters = 0.001;
  o.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
  ufbx_error err{};
  ufbx_scene* s = ufbx_load_file(path.c_str(), &o, &err);
  if (!s) {
    char b[1024];
    ufbx_format_error(b, sizeof b, &err);
    throw std::runtime_error(std::string("FBX load failed: ") + b);
  }
  return s;
}

// slackT / slackA: extra tolerance in game units (per axis) / radians (motion within the timing error of a DCC that
// bakes, the error of its curve simplification)
bool Close(const Pose& a, const ufbx_transform& b, const double* slackT, double slackA) {
  // FBX curve values are float32, and Blender keeps them in float32 metres under a 0.001 scale: far from the origin
  // (some scenes reach 3e8) that is several units (measured up to 2.2e-7 of the largest coordinate)
  const double big = std::max({std::fabs(a.t[0]), std::fabs(a.t[1]), std::fabs(a.t[2])});
  for (int k = 0; k < 3; k++)
    if (std::fabs(a.t[k] - (&b.translation.x)[k]) > std::max(0.501, big * 4e-7) + (slackT ? slackT[k] : 0)) return false;
  const double dot = std::min(1.0, std::fabs(a.r.w * b.rotation.w + a.r.x * b.rotation.x + a.r.y * b.rotation.y + a.r.z * b.rotation.z));
  return 2 * std::acos(dot) < 1e-4 + slackA;  // 1e-4 rad = 0.006 degrees; float32 Euler angles are far finer
}
bool Close(const Pose& a, const ufbx_transform& b) { return Close(a, b, nullptr, 0); }
Pose PoseOf(const double t[3], const double e[3]) { return {{t[0], t[1], t[2]}, Normalised(EulerToQuat(e))}; }
Pose PoseOf(const ufbx_transform& f) {
  return {{f.translation.x, f.translation.y, f.translation.z}, Normalised({f.rotation.w, f.rotation.x, f.rotation.y, f.rotation.z})};
}
bool SameRotation(Q a, Q b) {
  a = Normalised(a), b = Normalised(b);
  return std::fabs(a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z) > 1 - 1e-9;
}

// key fields by name, and a key built from them for a flag word (fields the word adds start at 0)
std::map<std::string, int32_t> Fields(const scene::Key& k) {
  std::map<std::string, int32_t> m;
  const auto l = scene::KeyLayout(k.flags);
  for (size_t i = 0; i < l.size() && i < k.values.size(); i++) m[l[i].name] = k.values[i];
  return m;
}
scene::Key MakeKey(uint32_t flags, const std::map<std::string, int32_t>& m) {
  scene::Key k;
  k.flags = flags;
  for (auto& f : scene::KeyLayout(flags)) {
    auto it = m.find(f.name);
    k.values.push_back(it == m.end() ? 0 : it->second);
  }
  return k;
}
// an engine rotation as the axis-angle fields (angle in turns, axis stored negated)
void SetAxisAngle(std::map<std::string, int32_t>& m, Q g) {
  g = Normalised(g);
  if (g.w < 0) g = {-g.w, -g.x, -g.y, -g.z};
  const double half = std::acos(std::clamp(g.w, -1.0, 1.0)), sn = std::sin(half);
  double ax = 0, ay = 0, az = 0;
  if (sn > 1e-9) ax = g.x / sn, ay = g.y / sn, az = g.z / sn;
  m["angle"] = static_cast<int32_t>(std::llround(2 * half / (2 * kPi) * kFix));
  m["ax"] = static_cast<int32_t>(std::llround(-ax * kFix));
  m["ay"] = static_cast<int32_t>(std::llround(-ay * kFix));
  m["az"] = static_cast<int32_t>(std::llround(-az * kFix));
}

// A part's keys after the FBX: every stored key the FBX shows unchanged stays as it is; a changed one gets the FBX
// pose in its own form (its flags, TCB, colour, FOV, extra kept; a relative rotation stays relative); an FBX key
// where no stored key is and the keys so far do not give the FBX pose becomes a new key, with the flags and
// parameters of the key before it and an absolute rotation. Relative keys after a changed one are encoded again so
// their own orientation does not move. "at" is the part's FBX pose at a time (seconds), "frame" the FBX frame length.
struct MergeResult {
  std::vector<scene::Key> keys;
  int changed = 0, added = 0;
};
MergeResult MergeKeys(const std::vector<scene::Key>& stored, const std::function<ufbx_transform(double)>& at,
                      const std::vector<double>& fbxTimes, const std::array<std::vector<double>, 6>& channels,
                      double frame, bool needKey, bool fromBlender, double bakeFps) {
  scene::Part probe;
  probe.keys = stored;
  const auto poses = KeyPoses(probe);
  const auto times = scene::KeyTimes(probe);
  const auto eq = EngineQuats(probe);
  const ExportedAnim exported = BakeAnim(probe, poses, times, bakeFps);
  bool ordered = true;
  for (size_t i = 1; i < times.size(); i++) ordered &= times[i] >= times[i - 1];

  double et[3], ee[3];
  // the motion of the curves within 2 ms of t: Blender's baked frames are off our times (measured: about 0.4 ms; its
  // curve simplification adds to it)
  // a camera (target or roll keys): its orientation comes from them, the FBX rotation is not compared
  const anim::Tracks engine = anim::Build(probe);
  const bool aimed = !engine.target.empty() || !engine.roll.empty();
  auto slack = [&](const ExportedAnim& a, double t, double* sT, double& sA) {
    sT[0] = sT[1] = sT[2] = 0;
    sA = aimed ? 10.0 : 0.0;
    if (!a.moves || !fromBlender) return;  // other writers keep the key times: no timing error to allow for
    double t0[3], e0[3], t1[3], e1[3];
    a.Eval(t - 0.002, t0, e0), a.Eval(t + 0.002, t1, e1);
    for (int k = 0; k < 3; k++) sT[k] = std::fabs(t1[k] - t0[k]);
    const Q q0 = Normalised(EulerToQuat(e0)), q1 = Normalised(EulerToQuat(e1));
    sA = aimed ? 10.0 : 2 * std::acos(std::min(1.0, std::fabs(q0.w * q1.w + q0.x * q1.x + q0.y * q1.y + q0.z * q1.z)));
  };
  // Blender bakes every frame and then, channel by channel, drops the frames a line through their neighbours gives
  // within 0.1% of the values (bake_anim_simplify_factor 1, the default): where a channel's keys are sparser than a
  // frame, its curve is only that close (measured on objects/cut/04705: up to 0.3% once drops add up)
  auto sparse = [&](const std::vector<double>& ts, double t) {
    if (ts.empty()) return false;
    auto it = std::lower_bound(ts.begin(), ts.end(), t - 1e-6);
    const bool on = it != ts.end() && *it <= t + 1e-6;
    const double next = on ? (it + 1 != ts.end() ? *(it + 1) : 1e30) : (it != ts.end() ? *it : 1e30);
    const double prev = it != ts.begin() ? *(it - 1) : -1e30;
    return next - t > 1.5 * frame || t - prev > 1.5 * frame;
  };
  auto blenderSlack = [&](double t, const Pose& p, double* sT, double& sA) {
    if (!fromBlender) return;
    for (int k = 0; k < 3; k++)
      if (sparse(channels[k], t)) sT[k] += 5e-3 * std::fabs(p.t[k]);
    for (int k = 0; k < 3; k++)
      if (sparse(channels[3 + k], t)) sA += 5e-3 * kPi / 3;
  };
  // the exported curves show f at time ts: their value, any key at ts (keys sharing a time), or - for a DCC that
  // bakes to frames - the line between their values at the frames around ts
  auto shows = [&](double ts, const ufbx_transform& f) {
    double sT[3], sA;
    slack(exported, ts, sT, sA);
    blenderSlack(ts, PoseOf(f), sT, sA);
    exported.Eval(ts, et, ee);
    if (Close(PoseOf(et, ee), f, sT, sA)) return true;
    for (size_t j = 0; j < exported.time.size(); j++)
      if (exported.time[j] == ts && Close(PoseOf(exported.t[j].data(), exported.e[j].data()), f)) return true;
    if (!exported.moves || !fromBlender) return false;
    const double f0 = std::floor(ts / frame) * frame, f1 = f0 + frame, w = (ts - f0) / frame;
    double t0[3], e0[3], t1[3], e1[3];
    exported.Eval(f0, t0, e0), exported.Eval(f1, t1, e1);
    for (int k = 0; k < 3; k++) et[k] = t0[k] + (t1[k] - t0[k]) * w, ee[k] = e0[k] + (e1[k] - e0[k]) * w;
    return Close(PoseOf(et, ee), f, sT, sA);
  };
  // a DCC that bakes whole frames ends at the last one: a key after it (within a frame) is checked there
  const double fbxEnd = fbxTimes.empty() ? 0 : fbxTimes.back();
  auto checkTime = [&](double ts) { return !fbxTimes.empty() && ts > fbxEnd && ts - fbxEnd < frame ? fbxEnd : ts; };

  // The FBX pose exactly at a changed key's time ts. A DCC that bakes to frames has no sample there: the key is a
  // corner of its (linear) curve, which the two frames before it (after the key before, at prevTs) or the two
  // after it (before the key after, at nextTs) lie on - the line through them ends at the corner.
  auto poseAtKey = [&](double ts, double prevTs, double nextTs) -> Pose {
    auto it = std::lower_bound(fbxTimes.begin(), fbxTimes.end(), ts - 1e-4);
    if (fbxTimes.empty() || (it != fbxTimes.end() && *it <= ts + 1e-4)) return PoseOf(at(ts));
    double a = 0, b = 0;
    if (it - fbxTimes.begin() >= 2 && *(it - 2) > prevTs + 1e-4) a = *(it - 2), b = *(it - 1);
    else if (fbxTimes.end() - it >= 2 && *(it + 1) < nextTs - 1e-4) a = *(it + 1), b = *it;
    else return PoseOf(at(ts));
    const Pose pa = PoseOf(at(a)), pb = PoseOf(at(b));
    const double s = (ts - a) / (b - a);
    Pose out;
    for (int k = 0; k < 3; k++) out.t[k] = pa.t[k] + (pb.t[k] - pa.t[k]) * s;
    // rotation: pa (x) (pa^-1 (x) pb)^s
    Q d = Mul(Q{pa.r.w, -pa.r.x, -pa.r.y, -pa.r.z}, pb.r);
    if (d.w < 0) d = {-d.w, -d.x, -d.y, -d.z};
    const double half = std::acos(std::clamp(d.w, -1.0, 1.0)), sn = std::sin(half);
    Q p{1, 0, 0, 0};
    if (sn > 1e-12) {
      const double h = half * s, k = std::sin(h) / sn;
      p = {std::cos(h), d.x * k, d.y * k, d.z * k};
    }
    out.r = Normalised(Mul(pa.r, p));
    return out;
  };

  struct K {
    int64_t ms;
    Pose pose;  // FBX space
    int src;    // stored key, -1 for a new one
    bool changed;
  };
  std::vector<K> ks;
  for (size_t i = 0; i < stored.size(); i++) {
    // keys out of time order: only the rest pose is in the FBX
    const bool check = ordered || i == 0;
    const double ts = checkTime(times[i] / 1000.0);
    const ufbx_transform f = at(check ? ts : 0.0);
    const bool same = !check || shows(ts, f);
    if (!same && getenv("X3_SCENE_DEBUG"))
      fprintf(stderr, "  key %zu at %lld ms: stored t %.1f %.1f %.1f q %.5f %.5f %.5f %.5f, FBX t %.1f %.1f %.1f q %.5f %.5f %.5f %.5f\n", i,
              static_cast<long long>(times[i]), poses[i].t[0], poses[i].t[1], poses[i].t[2], poses[i].r.w, poses[i].r.x,
              poses[i].r.y, poses[i].r.z, f.translation.x, f.translation.y, f.translation.z, f.rotation.w, f.rotation.x,
              f.rotation.y, f.rotation.z);
    const double prevTs = i ? times[i - 1] / 1000.0 : -1e30, nextTs = i + 1 < times.size() ? times[i + 1] / 1000.0 : 1e30;
    Pose pose = poses[i];
    bool changed = !same;
    if (changed) {
      pose = check ? poseAtKey(ts, prevTs, nextTs) : PoseOf(f);
      // a key next to an edited one: the frames around it straddle the edit, but its corner is where it was
      ufbx_transform corner{};
      corner.translation = {pose.t[0], pose.t[1], pose.t[2]};
      corner.rotation = {pose.r.x, pose.r.y, pose.r.z, pose.r.w};
      double sT[3], sA;
      slack(exported, ts, sT, sA);
      blenderSlack(ts, pose, sT, sA);
      if (Close(poses[i], corner, sT, sA)) pose = poses[i], changed = false;
    }
    ks.push_back({times[i], pose, static_cast<int>(i), changed});
  }
  int added = 0;
  if (ordered) {
    std::vector<double> candidates = fbxTimes;
    candidates.insert(candidates.begin(), 0.0);
    const int64_t tol = std::max<int64_t>(1, std::llround(frame * 500));  // half a frame, in ms
    for (double t : candidates) {
      if (t < 0) continue;
      const int64_t ms = std::llround(t * 1000);
      bool near = false;
      for (auto& k : ks) near |= std::llabs(k.ms - ms) <= tol;
      if (near) continue;
      const ufbx_transform f = at(t);
      // a baked export: the samples between keys are the engine's motion; where the FBX still shows them, no key
      if (bakeFps > 0 && exported.moves && shows(t, f)) continue;
      std::vector<Pose> ps;
      std::vector<int64_t> ts;
      for (auto& k : ks) ps.push_back(k.pose), ts.push_back(k.ms);
      // (no keys at all: the engine's pose is the origin; a new part gets its key at 0 anyway)
      if (!ks.empty() || !(needKey && t == 0.0)) {
        const ExportedAnim sofar = ExportAnim(ps, ts);
        double sT[3], sA;
        slack(sofar, t, sT, sA);
        blenderSlack(t, PoseOf(f), sT, sA);
        sofar.Eval(t, et, ee);
        if (Close(PoseOf(et, ee), f, sT, sA)) continue;
      }
      if (getenv("X3_SCENE_DEBUG") && added < 3)
        fprintf(stderr, "  new key at %.4f s: FBX t %.1f %.1f %.1f, keys so far give %.1f %.1f %.1f\n", t, f.translation.x,
                f.translation.y, f.translation.z, et[0], et[1], et[2]);
      auto pos = std::find_if(ks.begin(), ks.end(), [&](const K& k) { return k.ms > ms; });
      ks.insert(pos, K{ms, PoseOf(f), -1, true});
      added++;
    }
  }

  MergeResult out;
  out.added = added;
  Q run;
  for (size_t j = 0; j < ks.size(); j++) {
    const K& k = ks[j];
    scene::Key key;
    if (k.src >= 0 && !k.changed) {
      key = stored[k.src];
      // its own rotation after earlier changes; if that moved, encode it again for its stored orientation (a key
      // without one, like one without a position, is interpolated by the engine and holds nothing)
      if (!aimed && OwnRotation(key.flags) && !SameRotation(StepRotation(run, key), eq[k.src])) {
        auto m = Fields(key);
        uint32_t fl = (key.flags & ~0x20u) | 2;
        SetAxisAngle(m, fl & 0x2000 ? eq[k.src] : Mul(Q{run.w, -run.x, -run.y, -run.z}, Normalised(eq[k.src])));
        key = MakeKey(fl, m);
      }
    } else {
      // a changed key keeps its own form; a new one takes the one before it (or a plain absolute key)
      const scene::Key* base = k.src >= 0 ? &stored[k.src] : nullptr;
      scene::Key prevOut;
      if (!base && !out.keys.empty()) prevOut = out.keys.back(), base = &prevOut;
      uint32_t fl = base ? base->flags : 0x2002;
      auto m = base ? Fields(*base) : std::map<std::string, int32_t>{};
      if (k.src < 0) fl = (fl & ~0x10u) | 0x2000;  // a new key: own position, absolute rotation
      // a key without its own position gets one only if the FBX shows another than the engine gives there
      const double want[3] = {k.pose.t[0], k.pose.t[1], -k.pose.t[2]};
      if ((fl & 0x10) && k.src >= 0) {
        bool moved = false;
        for (int c = 0; c < 3; c++) moved |= std::fabs(k.pose.t[c] - poses[k.src].t[c]) > 0.5;
        if (moved) fl &= ~0x10u;
      }
      if (!(fl & 0x10)) {
        m["x"] = static_cast<int32_t>(std::llround(want[0]));
        m["y"] = static_cast<int32_t>(std::llround(want[1]));
        m["z"] = static_cast<int32_t>(std::llround(want[2]));
      }
      if (!(fl & 8) && !aimed) {
        const Q g = Normalised(FbxToGame(k.pose.r));
        const bool keep = k.src >= 0 && (OwnRotation(fl) ? SameRotation(StepRotation(run, MakeKey(fl, m)), g)
                                                          : SameRotation(FbxToGame(poses[k.src].r), g));
        if (!keep) {
          fl = (fl & ~0x20u) | 2;
          SetAxisAngle(m, fl & 0x2000 ? g : Mul(Q{run.w, -run.x, -run.y, -run.z}, g));
        }
      }
      key = MakeKey(fl, m);
      if (k.src >= 0) out.changed++;
    }
    run = StepRotation(run, key);
    out.keys.push_back(std::move(key));
  }
  // times: with new keys, every dt from the absolute times; extra (unused by the engine) of the new keys and of a
  // former last key follows the common "next key" pattern
  if (added) {
    const auto layoutIndex = [](const scene::Key& k, const char* name) {
      const auto l = scene::KeyLayout(k.flags);
      for (size_t i = 0; i < l.size(); i++)
        if (strcmp(l[i].name, name) == 0) return i;
      return l.size();
    };
    for (size_t j = 0; j < out.keys.size(); j++) {
      scene::Key& key = out.keys[j];
      const bool last = j + 1 == out.keys.size();
      key.values[layoutIndex(key, "dt")] = last ? -1 : static_cast<int32_t>(ks[j + 1].ms - ks[j].ms);
      int32_t& extra = key.values[layoutIndex(key, "extra")];
      if (ks[j].src < 0 || (extra == -1 && !last)) extra = last ? -1 : static_cast<int32_t>(j + 1);
    }
  }
  return out;
}

// The animation stack that holds a node's own animation. One stack (ours, Max, Maya, Blender with "All Actions"
// off) is the scene's; Blender's default writes every action applied to every object as stacks "object|action" -
// the node's own is the one whose action is named after it (an imported "N|..." or a new "NAction"), else the only
// stack that animates it, else the first stack.
using StackCache = std::map<const ufbx_node*, const ufbx_anim_stack*>;
const ufbx_anim_stack* StackFor(const ufbx_scene* s, const ufbx_node* n, StackCache& cache) {
  auto it = cache.find(n);
  if (it != cache.end()) return it->second;
  const std::string name(n->name.data, n->name.length);
  std::vector<const ufbx_anim_stack*> having;
  for (size_t i = 0; i < s->anim_stacks.count; i++) {
    const ufbx_anim_stack* st = s->anim_stacks.data[i];
    bool has = false;
    for (size_t l = 0; l < st->layers.count && !has; l++)
      for (size_t k = 0; k < st->layers.data[l]->anim_props.count && !has; k++)
        has = st->layers.data[l]->anim_props.data[k].element == &n->element;
    if (has) having.push_back(st);
  }
  const ufbx_anim_stack* pick = s->anim_stacks.count ? s->anim_stacks.data[0] : nullptr;
  if (having.size() == 1) pick = having[0];
  for (int pass = 0; pass < 2 && having.size() > 1; pass++)
    for (const ufbx_anim_stack* st : having) {
      const std::string sn(st->name.data, st->name.length);
      if (sn.compare(0, name.size() + 1, name + "|") != 0) continue;
      const std::string action = sn.substr(name.size() + 1);
      if (pass == 0 ? action.compare(0, name.size() + 1, name + "|") == 0 : action == name + "Action") {
        pick = st, pass = 2;
        break;
      }
    }
  return cache[n] = pick;
}

// times (seconds) of the translation / rotation keys of a node and its ancestors up to (not including) stop
std::vector<double> AnimTimes(const ufbx_scene* s, const ufbx_node* n, const ufbx_node* stop, StackCache& cache) {
  std::set<double> t;
  for (const ufbx_node* a = n; a && a != stop; a = a->parent) {
    const ufbx_anim_stack* st = StackFor(s, a, cache);
    if (!st) continue;
    for (size_t li = 0; li < st->layers.count; li++) {
      const ufbx_anim_layer* layer = st->layers.data[li];
      for (size_t pi = 0; pi < layer->anim_props.count; pi++) {
        const ufbx_anim_prop& ap = layer->anim_props.data[pi];
        if (ap.element != &a->element) continue;
        const std::string prop(ap.prop_name.data, ap.prop_name.length);
        if (prop != "Lcl Translation" && prop != "Lcl Rotation" && prop != "Lcl Scaling") continue;
        for (int c = 0; c < 3; c++)
          if (const ufbx_anim_curve* cv = ap.anim_value->curves[c])
            for (size_t k = 0; k < cv->keyframes.count; k++) t.insert(cv->keyframes.data[k].time);
      }
    }
  }
  return std::vector<double>(t.begin(), t.end());
}

// key times (seconds) per channel of the node's own curves: Lcl Translation x, y, z, then Lcl Rotation x, y, z
std::array<std::vector<double>, 6> ChannelTimes(const ufbx_scene* s, const ufbx_node* n, StackCache& cache) {
  std::array<std::vector<double>, 6> out;
  const ufbx_anim_stack* st = StackFor(s, n, cache);
  if (!st) return out;
  for (size_t li = 0; li < st->layers.count; li++)
    for (size_t pi = 0; pi < st->layers.data[li]->anim_props.count; pi++) {
      const ufbx_anim_prop& ap = st->layers.data[li]->anim_props.data[pi];
      if (ap.element != &n->element) continue;
      const std::string prop(ap.prop_name.data, ap.prop_name.length);
      const int base = prop == "Lcl Translation" ? 0 : prop == "Lcl Rotation" ? 3 : -1;
      if (base < 0) continue;
      for (int c = 0; c < 3; c++)
        if (const ufbx_anim_curve* cv = ap.anim_value->curves[c])
          for (size_t k = 0; k < cv->keyframes.count; k++) out[base + c].push_back(cv->keyframes.data[k].time);
    }
  for (auto& v : out) std::sort(v.begin(), v.end());
  return out;
}

// The part's transform at time t (seconds) relative to stop (its nearest part ancestor, or null for the scene's
// top): the product of the local transforms of the node and of every node between, so grouping nodes, a scaled
// scene root (Blender's unit scale) and the unit conversion of ufbx all count. Translation in game units.
ufbx_transform Relative(const ufbx_scene* s, const ufbx_node* n, const ufbx_node* stop, double t, double unitMm,
                        StackCache& cache) {
  ufbx_matrix m = ufbx_identity_matrix;
  for (const ufbx_node* a = n; a && a != stop; a = a->parent) {
    const ufbx_anim_stack* st = StackFor(s, a, cache);
    const ufbx_transform lt = st ? ufbx_evaluate_transform(st->anim, a, t) : a->local_transform;
    const ufbx_matrix l = ufbx_transform_to_matrix(&lt);
    m = ufbx_matrix_mul(&l, &m);
  }
  ufbx_transform r = ufbx_matrix_to_transform(&m);
  r.translation.x /= unitMm, r.translation.y /= unitMm, r.translation.z /= unitMm;
  return r;
}

}  // namespace

std::vector<std::array<double, 7>> KeyPosesFbx(const scene::Part& part) {
  std::vector<std::array<double, 7>> out;
  for (auto& ps : KeyPoses(part)) out.push_back({ps.t[0], ps.t[1], ps.t[2], ps.r.w, ps.r.x, ps.r.y, ps.r.z});
  return out;
}
std::array<double, 4> EulerQuat(const double euler[3]) {
  const Q q = Normalised(EulerToQuat(euler));
  return {q.w, q.x, q.y, q.z};
}

bool FbxIsScene(const std::string& path) {
  ufbx_scene* s = Load(path);
  bool yes = false;
  for (size_t i = 0; i < s->nodes.count; i++) yes |= PStr(s->nodes.data[i]->props, "x3_kind") == "scene";
  ufbx_free_scene(s);
  return yes;
}

scene::Scene FbxToScene(const std::string& path, SceneImportReport& rep) {
  ufbx_scene* s = Load(path);
  struct Free {
    ufbx_scene* s;
    ~Free() { ufbx_free_scene(s); }
  } freeScene{s};
  const ufbx_node* root = nullptr;
  for (size_t i = 0; i < s->nodes.count; i++)
    if (PStr(s->nodes.data[i]->props, "x3_kind") == "scene") root = s->nodes.data[i];
  if (!root) throw std::runtime_error("no scene root (a node with x3_kind = scene) in " + path);

  scene::Scene sc;
  sc.binary = PInt(root->props, "x3_binary", 1) != 0;
  sc.version = static_cast<int32_t>(PInt(root->props, "x3_version", 6));
  if (PHas(root->props, "x3_info")) sc.info = PStr(root->props, "x3_info");
  if (!sc.binary) sc.version = 6, sc.binary = true;  // written as CUT1
  if (!sc.info) sc.info = "x3model";
  double unitMm = 1.0;
  if (const ufbx_prop* up = ufbx_find_prop(&root->props, "x3_unit_mm"))
    if (up->value_real > 0) unitMm = up->value_real;
  const double frame = s->settings.frames_per_second > 0 ? 1.0 / s->settings.frames_per_second : 1.0 / 30;
  StackCache stacks;
  double bakeFps = 0;
  if (const ufbx_prop* bp = ufbx_find_prop(&root->props, "x3_bake_fps")) bakeFps = bp->value_real;
  const bool fromBlender = s->metadata.exporter == UFBX_EXPORTER_BLENDER_BINARY || s->metadata.exporter == UFBX_EXPORTER_BLENDER_ASCII;
  if (getenv("X3_SCENE_DEBUG")) rep.notes.push_back(std::string("written by Blender: ") + (fromBlender ? "yes" : "no"));
  if (getenv("X3_SCENE_DEBUG"))
    for (size_t i = 0; i < s->anim_stacks.count; i++) {
      const ufbx_anim_stack* st = s->anim_stacks.data[i];
      std::set<const ufbx_element*> els;
      for (size_t l = 0; l < st->layers.count; l++)
        for (size_t k = 0; k < st->layers.data[l]->anim_props.count; k++) els.insert(st->layers.data[l]->anim_props.data[k].element);
      rep.notes.push_back("stack '" + std::string(st->name.data, st->name.length) + "': " + std::to_string(st->layers.count) +
                          " layers, " + std::to_string(els.size()) + " elements");
    }

  // part nodes: x3_part (stored) or x3_body without x3_part (new), in stored order, new ones after; a copy made in
  // the DCC carries the x3_order of its original - the first node keeps it, the copies become new parts (part
  // numbers themselves repeat in some shipped scenes)
  struct Item {
    long long order;
    const ufbx_node* node;
    bool stored;
  };
  std::vector<Item> items;
  long long maxIndex = -1;
  std::set<long long> taken;
  for (size_t i = 0; i < s->nodes.count; i++) {
    const ufbx_node* n = s->nodes.data[i];
    if (PHas(n->props, "x3_ref")) continue;
    if (PHas(n->props, "x3_part") && taken.insert(PInt(n->props, "x3_order", -1 - static_cast<long long>(i))).second) {
      items.push_back({PInt(n->props, "x3_order", 1LL << 40), n, true});
      maxIndex = std::max(maxIndex, PInt(n->props, "x3_part"));
    } else if (PHas(n->props, "x3_part") || PHas(n->props, "x3_body")) {
      items.push_back({(1LL << 41) + static_cast<long long>(i), n, false});
    }
  }
  std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.order < b.order; });
  std::map<long long, const ufbx_node*> targetOf;  // part (x3_order) -> its camera target node
  for (size_t i = 0; i < s->nodes.count; i++)
    if (PHas(s->nodes.data[i]->props, "x3_target_of"))
      targetOf.emplace(PInt(s->nodes.data[i]->props, "x3_target_of"), s->nodes.data[i]);
  std::map<const ufbx_node*, uint32_t> indexOf;
  for (auto& it : items)
    indexOf[it.node] = it.stored ? static_cast<uint32_t>(PInt(it.node->props, "x3_part")) : static_cast<uint32_t>(++maxIndex);
  // the nearest ancestor that is a part
  auto anchorOf = [&](const ufbx_node* n) -> const ufbx_node* {
    for (const ufbx_node* a = n->parent; a; a = a->parent)
      if (indexOf.count(a)) return a;
    return nullptr;
  };
  // parents before their children (the engine looks a parent up among the parts built before it)
  {
    std::vector<Item> sorted;
    std::set<const ufbx_node*> done;
    std::function<void(const Item&)> emit = [&](const Item& it) {
      if (done.count(it.node)) return;
      done.insert(it.node);
      if (const ufbx_node* a = anchorOf(it.node))
        for (auto& o : items)
          if (o.node == a) emit(o);
      sorted.push_back(it);
    };
    for (auto& it : items) emit(it);
    items = std::move(sorted);
  }

  for (auto& it : items) {
    const ufbx_node* n = it.node;
    const ufbx_props& pp = n->props;
    scene::Part p;
    p.index = indexOf[n];
    p.body = PStr(pp, "x3_body");
    if (!it.stored) std::replace(p.body.begin(), p.body.end(), '/', '\\');
    p.mode = static_cast<int32_t>(PInt(pp, "x3_mode", 0));
    p.flags = static_cast<uint32_t>(PInt(pp, "x3_flags", 0x40));
    if (PHas(pp, "x3_name")) p.name = PStr(pp, "x3_name");
    const ufbx_node* anchor = anchorOf(n);
    p.parent = anchor ? static_cast<int32_t>(indexOf[anchor]) : -1;
    if (PHas(pp, "x3_constraints")) {
      std::vector<std::pair<int32_t, int32_t>> c;
      std::istringstream in(PStr(pp, "x3_constraints"));
      std::string tok;
      in >> tok;  // "n"
      while (in >> tok) {
        const size_t colon = tok.find(':');
        if (colon == std::string::npos) throw FormatError("part P" + std::to_string(p.index) + ": bad x3_constraints entry '" + tok + "'");
        c.push_back({std::stoi(tok.substr(0, colon)), std::stoi(tok.substr(colon + 1))});
      }
      p.constraints = c;
    }
    if (PHas(pp, "x3_notes")) {
      std::istringstream in(PStr(pp, "x3_notes"));
      std::string line;
      while (std::getline(in, line)) {
        if (line.empty()) continue;
        const size_t sp = line.find(' ');
        p.notes.push_back({std::stoi(line.substr(0, sp)), sp == std::string::npos ? "" : line.substr(sp + 1)});
      }
    }
    if (PHas(pp, "x3_embedded")) p.embedded = Unbase64(PStr(pp, "x3_embedded"));
    if (PHas(pp, "x3_inline")) p.inlineText = PStr(pp, "x3_inline");

    const auto fbxTimes = AnimTimes(s, n, anchor, stacks);
    auto at = [&](double t) { return Relative(s, n, anchor, t, unitMm, stacks); };
    // keys: the stored ones merged key by key with what the FBX shows (a new part: keys from the FBX)
    std::vector<scene::Key> stored;
    if (it.stored && PHas(pp, "x3_keys")) {
      try {
        stored = KeysFromBlob(PStr(pp, "x3_keys"));
      } catch (const std::exception&) {
        rep.notes.push_back("part P" + std::to_string(p.index) + ": x3_keys does not decode, keys taken from the FBX");
      }
    }
    const MergeResult merged =
        MergeKeys(stored, at, fbxTimes, ChannelTimes(s, n, stacks), frame, !it.stored, fromBlender, bakeFps);
    p.keys = merged.keys;
    // a camera's target node (x3_target_of): its position at each target key is the key's target
    int targetsMoved = 0;
    if (auto tit = targetOf.find(PInt(pp, "x3_order", -1)); it.stored && tit != targetOf.end()) {
      const ufbx_node* tn = tit->second;
      const ufbx_node* tanchor = anchorOf(tn);
      const auto ttimes = AnimTimes(s, tn, tanchor, stacks);
      const auto tchan = ChannelTimes(s, tn, stacks);
      auto tat = [&](double t) { return Relative(s, tn, tanchor, t, unitMm, stacks).translation; };
      const auto kt = scene::KeyTimes(p);
      double prevTs = -1e30;
      for (size_t i = 0; i < p.keys.size(); i++) {
        scene::Key& k = p.keys[i];
        if (!(k.flags & 8) || (k.flags & 0x40)) continue;
        const double ts = kt[i] / 1000.0;
        // (baked by Blender: the corner between the frames before it, as for the part's own keys)
        ufbx_vec3 v = tat(ts);
        auto itT = std::lower_bound(ttimes.begin(), ttimes.end(), ts - 1e-4);
        const bool sampled = itT != ttimes.end() && *itT <= ts + 1e-4;
        if (fromBlender && !sampled && itT - ttimes.begin() >= 2 && *(itT - 2) > prevTs + 1e-4) {
          const double a = *(itT - 2), b = *(itT - 1), f = (ts - a) / (b - a);
          const ufbx_vec3 va = tat(a), vb = tat(b);
          v = {va.x + (vb.x - va.x) * f, va.y + (vb.y - va.y) * f, va.z + (vb.z - va.z) * f};
        }
        prevTs = ts;
        const double want[3] = {v.x, v.y, -v.z}, have[3] = {double(k.Get("tx")), double(k.Get("ty")), double(k.Get("tz"))};
        // Blender: the timing slack and, where its simplification thinned a channel, 0.5% (as for the parts)
        double slackT[3] = {0, 0, 0};
        if (fromBlender) {
          const ufbx_vec3 v0 = tat(ts - 0.002), v1 = tat(ts + 0.002);
          const double d[3] = {std::fabs(v1.x - v0.x), std::fabs(v1.y - v0.y), std::fabs(v1.z - v0.z)};
          for (int c = 0; c < 3; c++) {
            slackT[c] = d[c];
            const auto& ch = tchan[c];
            auto ci = std::lower_bound(ch.begin(), ch.end(), ts - 1e-6);
            const double next = ci != ch.end() ? *ci : 1e30, prev = ci != ch.begin() ? *(ci - 1) : -1e30;
            if (!ch.empty() && (next - ts > 1.5 * frame || ts - prev > 1.5 * frame)) slackT[c] += 5e-3 * std::fabs(have[c]);
          }
        }
        const double big = std::max({std::fabs(have[0]), std::fabs(have[1]), std::fabs(have[2])});
        bool moved = false;
        for (int c = 0; c < 3; c++) moved |= std::fabs(want[c] - have[c]) > std::max(0.501, big * 4e-7) + slackT[c];
        if (!moved) continue;
        auto m = Fields(k);
        m["tx"] = static_cast<int32_t>(std::llround(want[0]));
        m["ty"] = static_cast<int32_t>(std::llround(want[1]));
        m["tz"] = static_cast<int32_t>(std::llround(want[2]));
        k = MakeKey(k.flags, m);
        targetsMoved++;
      }
      if (targetsMoved)
        rep.notes.push_back("part P" + std::to_string(p.index) + ": " + std::to_string(targetsMoved) + " target keys moved");
    }
    bool kept = merged.changed == 0 && merged.added == 0 && targetsMoved == 0 && !stored.empty();
    if (!kept && !targetsMoved && !stored.empty() && p.keys.size() == stored.size()) {  // re-encoded relative keys only
      kept = true;
      for (size_t i = 0; kept && i < stored.size(); i++)
        kept = p.keys[i].flags == stored[i].flags && p.keys[i].values == stored[i].values;
    }
    if (it.stored && !kept)
      rep.notes.push_back("part P" + std::to_string(p.index) + ": " + std::to_string(merged.changed) + " of " +
                          std::to_string(stored.size()) + " keys changed, " + std::to_string(merged.added) + " added");
    rep.parts++;
    if (kept) rep.keptKeys++;
    else rep.rebuiltKeys++;
    if (!it.stored) {
      rep.newParts++;
      rep.notes.push_back("new part P" + std::to_string(p.index) + " (" + std::string(n->name.data, n->name.length) + "), body " + p.body);
    }
    sc.parts.push_back(std::move(p));
  }
  return sc;
}

}  // namespace x3
