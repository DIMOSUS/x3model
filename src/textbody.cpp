#include "textbody.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>

#include "bytes.hpp"
#include "textlex.hpp"

namespace x3::textbody {

namespace {

// The engine's lexer: whitespace and '/'-comments skipped, values end at ';'; numbers through the engine's own
// parsers (textlex.hpp), and a malformed or missing number is an error, as it is for the engine.
class Lexer : public EngineNumbers<Lexer> {
 public:
  explicit Lexer(const std::vector<uint8_t>& d) : s_(reinterpret_cast<const char*>(d.data()), d.size()) {}

  size_t p = 0;
  bool err = false;
  int C(size_t k = 0) const { return p + k < s_.size() ? static_cast<unsigned char>(s_[p + k]) : 0; }
  void Comment() {
    while (C() != '\n' && C() != 0) p++;
  }

  void Skip() {
    for (;;) {
      const int c = C();
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') p++;
      else if (c == '/') Comment();
      else break;
    }
  }
  bool AtEnd() {
    Skip();
    return C() == 0;
  }
  // a tag label ("MATERIAL6:") at the cursor; consumed if it matches
  bool Tag(const char* label) {
    Skip();
    const size_t n = strlen(label);
    if (s_.compare(p, n, label) != 0) return false;
    p += n;
    return true;
  }
  bool PeekTag(const char* label) {
    Skip();
    return s_.compare(p, strlen(label), label) == 0;
  }
  std::string Value() {
    Skip();
    std::string v;
    while (C() != ';' && C() != 0) {
      if (C() == '/') {  // a comment inside a value: to the end of the line
        Comment();
        continue;
      }
      v += s_[p++];
    }
    if (C() == ';') p++;
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
    size_t b = 0;
    while (b < v.size() && (v[b] == ' ' || v[b] == '\t' || v[b] == '\r' || v[b] == '\n')) b++;
    return v.substr(b);
  }
  // the next value without consuming it
  std::string PeekValue() {
    const size_t save = p;
    std::string v = Value();
    p = save;
    return v;
  }
  int64_t Int() {
    Skip();
    const size_t at = p;
    const int32_t v = ParseInt();
    if (err) throw FormatError("bad or missing integer at byte " + std::to_string(at));
    return v;
  }
  int32_t Fixed() {
    Skip();
    const size_t at = p;
    const int32_t v = ParseFixed();
    if (err) throw FormatError("bad or missing number at byte " + std::to_string(at));
    return v;
  }
  uint32_t Binary() {  // part / LOD flags: digit string, radix 2
    const std::string v = Value();
    uint32_t r = 0;
    for (char c : v)
      if (c == '0' || c == '1') r = r << 1 | static_cast<uint32_t>(c - '0');
    return r;
  }
  size_t pos() const { return p; }

 private:
  std::string s_;
};

const char* kParamTypes[] = {"SPTYPE_LONG", "SPTYPE_BOOL", "SPTYPE_FLOAT", "SPTYPE_FLOAT2", "SPTYPE_FLOAT3",
                             "SPTYPE_FLOAT4", "SPTYPE_MATRIX3", "SPTYPE_MATRIX4", "SPTYPE_STRING"};

std::string NullToEmpty(const std::string& s) { return s == "NULL" ? "" : s; }

bob::Material ReadMaterial(Lexer& lx, int ver) {
  bob::Material m;
  m.index = static_cast<uint16_t>(lx.Int());
  if (ver == 6) {
    m.flags = static_cast<uint32_t>(lx.Int());
    if (m.flags & 0x02000000u) {  // effect material: verbatim (Material::ReadEffectMaterial)
      m.technique = static_cast<uint16_t>(lx.Int());
      m.effect = lx.Value();
      const int64_t n = lx.Int();
      for (int64_t i = 0; i < n; i++) {
        bob::EffectParam p;
        p.name = lx.Value();
        const std::string type = lx.Value();
        int t = -1;
        for (int k = 0; k < 9; k++)
          if (type == kParamTypes[k]) t = k;
        if (t < 0) throw FormatError("unknown effect parameter type " + type);
        p.type = static_cast<uint16_t>(t);
        if (p.type == bob::kString) {
          p.str = lx.Value();
        } else if (p.type == bob::kLong || p.type == bob::kBool) {
          p.ints.push_back(static_cast<int32_t>(lx.Int()));
        } else {
          for (int k = 0; k < bob::ParamValueCount(p.type); k++) p.ints.push_back(lx.Fixed());
        }
        m.params.push_back(std::move(p));
      }
      return m;
    }
    m.texture = NullToEmpty(lx.Value());
  } else {
    m.textureId = static_cast<uint16_t>(lx.Int());
  }
  // rgb x3 -> +0x04..+0x14, +0x16 = -1, transparency +0x18, self-illumination +0x1a, shininess +0x24, strength +0x26
  for (int i = 0; i < 9; i++) m.words[i] = static_cast<uint16_t>(lx.Int());
  m.words[9] = 0xffff;
  m.words[10] = static_cast<uint16_t>(lx.Int());
  m.words[11] = static_cast<uint16_t>(lx.Int());
  m.words[12] = static_cast<uint16_t>(lx.Int());
  m.words[13] = static_cast<uint16_t>(lx.Int());
  const bool blend = lx.Int() != 0, twoSided = lx.Int() != 0, wire = lx.Int() != 0;
  const uint32_t switches = (blend ? 2u : 0) | (twoSided ? 0x10u : 0) | (wire ? 8u : 0);
  if (ver == 6)
    m.flags |= switches;
  else
    m.flagWord = static_cast<uint16_t>(switches);
  m.word2c = static_cast<uint16_t>(lx.Int());
  const int nMaps = ver == 6 ? 5 : ver == 5 ? 3 : 2;
  for (int i = 0; i < nMaps; i++) {
    bob::Material::Map mp;
    if (ver == 6)
      mp.name = NullToEmpty(lx.Value());
    else
      mp.id = static_cast<uint16_t>(lx.Int());
    mp.value = static_cast<uint16_t>(lx.Int());
    m.maps.push_back(mp);
  }
  if (ver == 3) m.maps.push_back({});  // the binary MAT3 record has three map pairs
  return m;
}

struct V3d {
  double x, y, z;
};
V3d Sub(const int32_t* a, const int32_t* b) { return {double(a[0]) - b[0], double(a[1]) - b[1], double(a[2]) - b[2]}; }
V3d Cross(V3d a, V3d b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double Dot(V3d a, V3d b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

}  // namespace

bool IsTextScene(const std::vector<uint8_t>& d) {  // Scene::LoadTextCut requires "VER:" first
  Lexer lx(d);
  return lx.PeekTag("VER:");
}

bool IsTextBody(const std::vector<uint8_t>& d) {
  if (d.size() >= 4 && (memcmp(d.data(), "BOB1", 4) == 0 || memcmp(d.data(), "CUT1", 4) == 0)) return false;
  return !IsTextScene(d);
}

bob::Body Read(const std::vector<uint8_t>& data) {
  Lexer lx(data);
  bob::Body body;
  // INFO: the first '/#' comment (as the compilers do)
  {
    const std::string s(reinterpret_cast<const char*>(data.data()), std::min<size_t>(data.size(), 4096));
    const size_t at = s.find("/#");
    if (at != std::string::npos) {
      size_t e = s.find('\n', at);
      std::string info = s.substr(at + 2, e == std::string::npos ? std::string::npos : e - at - 2);
      while (!info.empty() && (info.back() == '\r' || info.back() == ' ')) info.pop_back();
      while (!info.empty() && info[0] == ' ') info.erase(0, 1);
      bob::Section sec;
      sec.kind = bob::SectionKind::Info;
      sec.str = info;
      body.sections.push_back(sec);
    }
  }
  if (lx.Tag("NAME:")) {
    bob::Section sec;
    sec.kind = bob::SectionKind::Name;
    sec.str = lx.Value();
    body.sections.push_back(sec);
  }
  if (lx.Tag("SOUND:"))  // u16; flags; i32; i32; fx; fx; fx; u16; u16; i32; i32 (kept out of the binary form)
    for (int i = 0; i < 11; i++) lx.Value();

  bob::Section mats;
  mats.kind = bob::SectionKind::Mat;
  int ver = 0;
  for (;;) {
    int v = 0;
    if (lx.Tag("MATERIAL6:")) v = 6;
    else if (lx.Tag("MATERIAL5:")) v = 5;
    else if (lx.Tag("MATERIAL3:")) v = 3;
    else if (lx.PeekTag("MATERIAL2:") || lx.PeekTag("MATERIAL:"))
      throw FormatError("MATERIAL/MATERIAL2 (X2-era materials) are not supported");
    else break;
    if (ver && v != ver) throw FormatError("mixed MATERIAL kinds");
    ver = v;
    mats.materials.push_back(ReadMaterial(lx, v));
  }
  if (ver) {
    mats.matVersion = ver;
    body.sections.push_back(mats);
  }
  body.textMat3 = ver == 3;
  // MATERIAL3 faces name the record's index field, the others the array position
  std::map<int, int> mat3Pos;
  if (ver == 3)
    for (size_t i = 0; i < mats.materials.size(); i++) mat3Pos[mats.materials[i].index] = static_cast<int>(i);

  bob::Section bodySec;
  bodySec.kind = bob::SectionKind::Body;
  while (!lx.AtEnd()) {
    if (lx.PeekTag("BONES:") || lx.PeekTag("BODIES:") || lx.PeekTag("INFO:")) {
      lx.Tag("BONES:"), lx.Tag("BODIES:"), lx.Tag("INFO:");
      lx.Value();
      continue;
    }
    bob::Lod lod;
    lod.value = static_cast<int32_t>(lx.Int());
    std::vector<std::array<int32_t, 3>> verts;
    for (;;) {
      const int64_t x = lx.Int(), y = lx.Int(), z = lx.Int();
      if (x == -1 && y == -1 && z == -1) break;
      if (lx.AtEnd()) throw FormatError("vertex list without its -1; -1; -1; end");
      verts.push_back({static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(z)});
    }
    if (lx.Tag("WEIGHTS:")) {  // per vertex (bone; weight;)* -1;  (kept: one shipped cut body)
      lx.Value();
      for (size_t i = 0; i < verts.size(); i++)
        for (;;) {
          const int64_t b = lx.Int();
          if (b == -1) break;
          lx.Value();
        }
    }
    // normalise: largest |coordinate| -> 65536, (v << 16) / m truncated
    int64_t m = 0;
    for (auto& v : verts)
      for (int32_t c : v) m = std::max<int64_t>(m, std::llabs(static_cast<int64_t>(c)));
    if (m == 0) throw FormatError("body record with all vertices at the origin (the engine refuses it)");
    for (auto& v : verts)
      for (auto& c : v) c = static_cast<int32_t>((static_cast<int64_t>(c) * 65536) / m);

    // parts
    struct Tri {
      int32_t point[3];
    };
    bool anyUv2 = false, negMat = false;
    std::vector<bob::Point> points;
    std::map<std::tuple<uint32_t, int32_t, int32_t, int32_t>, std::vector<int32_t>> byKey;  // (s, pos) -> points
    std::vector<V3d> accum;
    for (;;) {
      if (lx.AtEnd()) throw FormatError("body record ends inside its parts (no -99)");
      if (lx.PeekValue() == "-99") {  // no more parts
        lx.Value();
        lod.flags = lx.Binary();
        break;
      }
      bob::Part part;
      std::map<int, size_t> groupOf;
      for (;;) {
        int64_t mat = lx.Int();
        if (mat == -99) break;
        int32_t corner[4];
        int k = 0;
        for (; k < 4; k++) {
          const std::string tok = lx.PeekValue();
          if (!tok.empty() && tok[0] == '-') break;
          corner[k] = static_cast<int32_t>(lx.Int());
        }
        if (k < 3) throw FormatError("face with fewer than 3 corners");
        if (k == 4) {
          const std::string tok = lx.PeekValue();
          if (tok.empty() || tok[0] != '-') throw FormatError("face with more than 4 corners");
        }
        const uint32_t flags = static_cast<uint32_t>(-lx.Int());
        uint32_t smooth = 0;
        if (flags & 0x10) smooth = static_cast<uint32_t>(lx.Int());
        int32_t uv[4][2] = {}, uv2[4][2] = {};
        if (flags & 8)
          for (int i = 0; i < k; i++) uv[i][0] = lx.Fixed(), uv[i][1] = lx.Fixed();
        if (flags & 0x80) {
          anyUv2 = true;
          for (int i = 0; i < k; i++) uv2[i][0] = lx.Fixed(), uv2[i][1] = lx.Fixed();
        } else {
          for (int i = 0; i < k; i++) uv2[i][0] = uv[i][0], uv2[i][1] = uv[i][1];
        }
        if (ver == 3) {
          auto it = mat3Pos.find(static_cast<int>(mat));
          if (it == mat3Pos.end()) throw FormatError("MATERIAL3 index " + std::to_string(mat) + " not defined");
          mat = it->second;
        }
        if (mat < 0) negMat = true;
        // corners -> points
        int32_t pt[4];
        for (int i = 0; i < k; i++) {
          if (corner[i] < 0 || static_cast<size_t>(corner[i]) >= verts.size())
            throw FormatError("face corner " + std::to_string(corner[i]) + " out of range");
          const auto& pos = verts[corner[i]];
          int32_t found = -1;
          if (smooth) {
            auto& list = byKey[{smooth, pos[0], pos[1], pos[2]}];
            for (int32_t c : list) {
              const bob::Point& q = points[c];
              if (std::abs(q.uv[0] - uv[i][0]) < 2 && std::abs(q.uv[1] - uv[i][1]) < 2 &&
                  std::abs(q.uv2[0] - uv2[i][0]) < 2 && std::abs(q.uv2[1] - uv2[i][1]) < 2) {
                found = c;
                break;
              }
            }
            if (found < 0) list.push_back(static_cast<int32_t>(points.size()));
          }
          if (found < 0) {
            bob::Point p;
            p.flags = bob::kPos | bob::kUv | bob::kNormal | bob::kSmooth;
            for (int c = 0; c < 3; c++) p.pos[c] = pos[c];
            p.uv[0] = uv[i][0], p.uv[1] = uv[i][1], p.uv2[0] = uv2[i][0], p.uv2[1] = uv2[i][1];
            p.smooth = smooth;
            found = static_cast<int32_t>(points.size());
            points.push_back(p);
            accum.push_back({0, 0, 0});
          }
          pt[i] = found;
        }
        // fan (v0, v(i-1), v(i)); normals: angle-weighted unit face normal per corner, flat for group 0
        const int32_t word = static_cast<int32_t>(flags & ~1u);
        for (int i = 2; i < k; i++) {
          const int32_t t[3] = {pt[0], pt[i - 1], pt[i]};
          const V3d e1 = Sub(points[t[1]].pos, points[t[0]].pos), e2 = Sub(points[t[2]].pos, points[t[0]].pos);
          V3d n = Cross(e1, e2);
          const double len = std::sqrt(Dot(n, n));
          if (len > 0) n = {n.x / len, n.y / len, n.z / len};
          for (int c = 0; c < 3; c++) {
            const V3d a = Sub(points[t[(c + 1) % 3]].pos, points[t[c]].pos), b = Sub(points[t[(c + 2) % 3]].pos, points[t[c]].pos);
            const double la = std::sqrt(Dot(a, a)), lb = std::sqrt(Dot(b, b));
            const double w = smooth == 0 ? 1.0 : (la > 0 && lb > 0 ? std::acos(std::clamp(Dot(a, b) / (la * lb), -1.0, 1.0)) : 0.0);
            accum[t[c]] = {accum[t[c]].x + n.x * w, accum[t[c]].y + n.y * w, accum[t[c]].z + n.z * w};
          }
          if (!groupOf.count(static_cast<int>(mat))) {
            groupOf[static_cast<int>(mat)] = part.groups.size();
            bob::Group g;
            g.material = static_cast<int32_t>(mat);
            part.groups.push_back(g);
          }
          bob::Face f;
          for (int c = 0; c < 3; c++) f.v[c] = t[c];
          f.word = word;
          part.groups[groupOf[static_cast<int>(mat)]].faces.push_back(f);
        }
      }
      part.flags = lx.Binary();
      if (part.flags & 4) lx.Value();  // pivot vertex (a field the binary form has no place for)
      lod.parts.push_back(std::move(part));
    }
    for (size_t i = 0; i < points.size(); i++) {
      const V3d n = accum[i];
      const double len = std::sqrt(Dot(n, n));
      for (int c = 0; c < 3; c++)
        points[i].normal[c] = len > 0 ? static_cast<int32_t>(std::llround((&n.x)[c] / len * 65536.0)) : 0;
      if (anyUv2) points[i].flags |= bob::kUv2;
    }
    lod.points = std::move(points);
    if (negMat) lod.flags |= 1;
    if (anyUv2) lod.flags |= 0x20;
    bodySec.lods.push_back(std::move(lod));
  }
  if (bodySec.lods.empty()) throw FormatError("no body records");
  body.sections.push_back(std::move(bodySec));
  return body;
}

}  // namespace x3::textbody
