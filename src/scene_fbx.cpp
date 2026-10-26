#include "scene_fbx.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

#include "archive.hpp"
#include "bob.hpp"
#include "bytes.hpp"
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
// vectors (the Direct3D habit: the effective rotation is the transpose) is settled by the scene check in the docs
// ("Scenes", rotation convention): kRowVectors.
constexpr bool kRowVectors = true;
Q GameToFbx(Q q) {
  if (kRowVectors) q = {q.w, -q.x, -q.y, -q.z};  // effective rotation = transpose = conjugate
  return {q.w, -q.x, -q.y, q.z};
}
Q FbxToGame(Q q) {
  q = {q.w, -q.x, -q.y, q.z};
  if (kRowVectors) q = {q.w, -q.x, -q.y, -q.z};
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
std::vector<Q> EngineQuats(const scene::Part& p) {
  std::vector<Q> out;
  Q run;
  for (auto& k : p.keys) {
    const uint32_t f = k.flags;
    if (f & 8) {
      out.push_back(run);
      continue;
    }
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
    run = q;
    out.push_back(q);
  }
  return out;
}

struct Pose {
  double t[3];
  Q r;
};
// per key: position (game units, FBX axes) and rotation (FBX), carrying values of keys that omit them
std::vector<Pose> KeyPoses(const scene::Part& p) {
  const auto qs = EngineQuats(p);
  std::vector<Pose> out;
  double pos[3] = {0, 0, 0};
  for (size_t i = 0; i < p.keys.size(); i++) {
    const auto& k = p.keys[i];
    if (k.Has("x")) pos[0] = k.Get("x"), pos[1] = k.Get("y"), pos[2] = k.Get("z");
    out.push_back({{pos[0], pos[1], -pos[2]}, GameToFbx(Normalised(qs[i]))});  // (file axes are not exactly unit)
  }
  return out;
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
  std::vector<scene::Key> keys(static_cast<size_t>(std::max(GetI32(b, at), 0)));
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

    const auto poses = KeyPoses(p);
    if (!poses.empty()) {
      for (int k = 0; k < 3; k++) n.translation[k] = poses[0].t[k];
      QuatToEuler(poses[0].r, n.rotation);
    }
    // animation: every key a curve key when the part moves
    bool moves = false;
    for (size_t i = 1; i < poses.size() && !moves; i++)
      for (int k = 0; k < 3; k++)
        moves |= poses[i].t[k] != poses[0].t[k] || std::fabs(poses[i].r.w - poses[0].r.w) > 1e-9 ||
                 std::fabs(poses[i].r.x - poses[0].r.x) > 1e-9 || std::fabs(poses[i].r.y - poses[0].r.y) > 1e-9 ||
                 std::fabs(poses[i].r.z - poses[0].r.z) > 1e-9;
    // keys out of time order (a negative dt; one shipped part) cannot be an FBX curve: rest pose only, the stored
    // keys stay in x3_keys
    const auto keyTimes = scene::KeyTimes(p);
    for (size_t i = 1; i < keyTimes.size(); i++) moves &= keyTimes[i] >= keyTimes[i - 1];
    if (moves) {
      const auto& times = keyTimes;
      double prev[3] = {n.rotation[0], n.rotation[1], n.rotation[2]};
      for (size_t i = 0; i < poses.size(); i++) {
        const double t = times[i] / 1000.0;
        double e[3];
        QuatToEuler(poses[i].r, e);
        for (int k = 0; k < 3; k++) {  // unwrap: continue from the previous key
          while (e[k] - prev[k] > 180) e[k] -= 360;
          while (e[k] - prev[k] < -180) e[k] += 360;
          prev[k] = e[k];
          n.animT[k].time.push_back(t), n.animT[k].value.push_back(poses[i].t[k]);
          n.animR[k].time.push_back(t), n.animR[k].value.push_back(e[k]);
        }
        animEnd = std::max(animEnd, t);
      }
    }
    const int nodeIndex = static_cast<int>(out.nodes.size());
    nodeOfPart[p.index] = nodeIndex;
    out.nodes.push_back(n);

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

bool Close(const Pose& a, const ufbx_transform& b) {
  for (int k = 0; k < 3; k++)
    if (std::fabs(a.t[k] - (&b.translation.x)[k]) > 0.5) return false;
  const double dot = a.r.w * b.rotation.w + a.r.x * b.rotation.x + a.r.y * b.rotation.y + a.r.z * b.rotation.z;
  return std::fabs(dot) > 1 - 1e-5;  // FBX curves are float32
}

// times (seconds) of the node's own translation / rotation keys
std::vector<double> AnimTimes(const ufbx_scene* s, const ufbx_node* n) {
  std::set<double> t;
  for (size_t li = 0; li < s->anim_layers.count; li++) {
    const ufbx_anim_layer* layer = s->anim_layers.data[li];
    for (size_t pi = 0; pi < layer->anim_props.count; pi++) {
      const ufbx_anim_prop& ap = layer->anim_props.data[pi];
      if (ap.element != &n->element) continue;
      const std::string prop(ap.prop_name.data, ap.prop_name.length);
      if (prop != "Lcl Translation" && prop != "Lcl Rotation") continue;
      for (int c = 0; c < 3; c++)
        if (const ufbx_anim_curve* cv = ap.anim_value->curves[c])
          for (size_t k = 0; k < cv->keyframes.count; k++) t.insert(cv->keyframes.data[k].time);
    }
  }
  return std::vector<double>(t.begin(), t.end());
}

}  // namespace

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

  // part nodes: x3_part (stored) or x3_body without x3_part (new), in stored order, new ones after
  struct Item {
    long long order;
    const ufbx_node* node;
  };
  std::vector<Item> items;
  long long maxIndex = -1;
  for (size_t i = 0; i < s->nodes.count; i++) {
    const ufbx_node* n = s->nodes.data[i];
    if (PHas(n->props, "x3_ref")) continue;
    if (PHas(n->props, "x3_part")) {
      items.push_back({PInt(n->props, "x3_order", 1LL << 40), n});
      maxIndex = std::max(maxIndex, PInt(n->props, "x3_part"));
    } else if (PHas(n->props, "x3_body")) {
      items.push_back({(1LL << 41) + static_cast<long long>(i), n});
    }
  }
  std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.order < b.order; });
  std::map<const ufbx_node*, uint32_t> indexOf;
  for (auto& it : items) {
    const ufbx_node* n = it.node;
    indexOf[n] = PHas(n->props, "x3_part") ? static_cast<uint32_t>(PInt(n->props, "x3_part"))
                                            : static_cast<uint32_t>(++maxIndex);
  }

  for (auto& it : items) {
    const ufbx_node* n = it.node;
    const ufbx_props& pp = n->props;
    scene::Part p;
    p.index = indexOf[n];
    p.body = PStr(pp, "x3_body");
    p.mode = static_cast<int32_t>(PInt(pp, "x3_mode", 0));
    p.flags = static_cast<uint32_t>(PInt(pp, "x3_flags", 0x40));
    if (PHas(pp, "x3_name")) p.name = PStr(pp, "x3_name");
    // parent: the nearest ancestor that is a part
    p.parent = -1;
    for (const ufbx_node* a = n->parent; a; a = a->parent)
      if (indexOf.count(a)) {
        p.parent = static_cast<int32_t>(indexOf[a]);
        break;
      }
    if (PHas(pp, "x3_constraints")) {
      std::vector<std::pair<int32_t, int32_t>> c;
      std::istringstream in(PStr(pp, "x3_constraints"));
      std::string tok;
      in >> tok;  // "n"
      while (in >> tok) {
        const size_t colon = tok.find(':');
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

    // keys: stored ones while the FBX still shows them, else rebuilt from the FBX transform and animation
    bool kept = false;
    if (PHas(pp, "x3_keys")) {
      std::vector<scene::Key> stored;
      try {
        stored = KeysFromBlob(PStr(pp, "x3_keys"));
      } catch (...) {
      }
      scene::Part probe;
      probe.keys = stored;
      const auto poses = KeyPoses(probe);
      const auto times = scene::KeyTimes(probe);
      const auto fbxTimes = AnimTimes(s, n);
      bool same = true;
      if (poses.empty()) {
        same = Close(Pose{{0, 0, 0}, Q{}}, n->local_transform);
      } else if (fbxTimes.empty()) {  // static in the FBX: every stored key must be the rest pose
        bool ordered = true;
        for (size_t i = 1; i < times.size(); i++) ordered &= times[i] >= times[i - 1];
        if (ordered)
          for (auto& ps : poses) same &= Close(ps, n->local_transform);
        else  // written without animation (keys out of time order): the rest pose is what can be checked
          same = Close(poses[0], n->local_transform);
      } else {
        for (size_t i = 0; i < poses.size() && same; i++) {
          const ufbx_transform ev = ufbx_evaluate_transform(s->anim, n, times[i] / 1000.0);
          same = Close(poses[i], ev);
          for (size_t j = 0; j < poses.size() && !same; j++)  // keys sharing a time: the curve holds one of them
            if (j != i && times[j] == times[i]) same = Close(poses[j], ev);
          if (!same && getenv("X3_SCENE_DEBUG")) {
            char b[400];
            snprintf(b, sizeof b, "P%u key %zu t=%lld: stored t(%.3f %.3f %.3f) q(%.5f %.5f %.5f %.5f) evaluated t(%.3f %.3f %.3f) q(%.5f %.5f %.5f %.5f)",
                     p.index, i, static_cast<long long>(times[i]), poses[i].t[0], poses[i].t[1], poses[i].t[2], poses[i].r.w,
                     poses[i].r.x, poses[i].r.y, poses[i].r.z, ev.translation.x, ev.translation.y, ev.translation.z,
                     ev.rotation.w, ev.rotation.x, ev.rotation.y, ev.rotation.z);
            rep.notes.push_back(b);
          }
        }
      }
      if (same) {
        p.keys = stored, kept = true;
      } else if (getenv("X3_SCENE_DEBUG")) {
        const ufbx_transform& t = n->local_transform;
        char b[400];
        snprintf(b, sizeof b, "P%u: %zu keys, %zu FBX times; rest stored t(%.3f %.3f %.3f) q(%.5f %.5f %.5f %.5f) fbx t(%.3f %.3f %.3f) q(%.5f %.5f %.5f %.5f)",
                 p.index, poses.size(), fbxTimes.size(), poses.empty() ? 0 : poses[0].t[0], poses.empty() ? 0 : poses[0].t[1],
                 poses.empty() ? 0 : poses[0].t[2], poses.empty() ? 1 : poses[0].r.w, poses.empty() ? 0 : poses[0].r.x,
                 poses.empty() ? 0 : poses[0].r.y, poses.empty() ? 0 : poses[0].r.z, t.translation.x, t.translation.y,
                 t.translation.z, t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z);
        rep.notes.push_back(b);
      }
    }
    if (!kept) {
      std::vector<double> times = AnimTimes(s, n);
      if (times.empty()) times.push_back(0.0);
      for (size_t i = 0; i < times.size(); i++) {
        const ufbx_transform tr = times.size() == 1 ? n->local_transform : ufbx_evaluate_transform(s->anim, n, times[i]);
        const Q g = Normalised(FbxToGame({tr.rotation.w, tr.rotation.x, tr.rotation.y, tr.rotation.z}));
        scene::Key k;
        k.flags = 0x2002;  // position + absolute axis-angle rotation
        const Q q = g.w < 0 ? Q{-g.w, -g.x, -g.y, -g.z} : g;
        const double half = std::acos(std::clamp(q.w, -1.0, 1.0)), sn = std::sin(half);
        const double angleTurns = 2 * half / (2 * kPi);
        double ax = 0, ay = 0, az = 0;
        if (sn > 1e-9) ax = q.x / sn, ay = q.y / sn, az = q.z / sn;
        const int32_t dt = i + 1 < times.size() ? static_cast<int32_t>(std::llround((times[i + 1] - times[i]) * 1000)) : -1;
        k.values = {static_cast<int32_t>(std::llround(tr.translation.x)), static_cast<int32_t>(std::llround(tr.translation.y)),
                    static_cast<int32_t>(std::llround(-tr.translation.z)),
                    static_cast<int32_t>(std::llround(angleTurns * kFix)),
                    static_cast<int32_t>(std::llround(-ax * kFix)), static_cast<int32_t>(std::llround(-ay * kFix)),
                    static_cast<int32_t>(std::llround(-az * kFix)), dt, i + 1 < times.size() ? static_cast<int32_t>(i + 1) : -1};
        p.keys.push_back(k);
      }
      if (PHas(pp, "x3_part")) rep.notes.push_back("part P" + std::to_string(p.index) + ": keys rebuilt from the FBX");
    }
    rep.parts++;
    if (kept) rep.keptKeys++;
    else rep.rebuiltKeys++;
    if (!PHas(pp, "x3_part")) {
      rep.newParts++;
      rep.notes.push_back("new part P" + std::to_string(p.index) + " (" + std::string(n->name.data, n->name.length) + "), body " + p.body);
    }
    sc.parts.push_back(std::move(p));
  }
  return sc;
}

}  // namespace x3
