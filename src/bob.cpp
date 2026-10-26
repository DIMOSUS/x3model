#include "bob.hpp"

#include "bytes.hpp"

namespace x3::bob {

int ParamValueCount(uint16_t type) {
  switch (type) {
    case kLong: case kBool: case kFloat: return 1;
    case kFloat2: return 2;
    case kFloat3: return 3;
    case kFloat4: return 4;
    case kMatrix3: return 9;
    case kMatrix4: return 16;
    case kString: return -1;
  }
  throw FormatError("unknown effect parameter type " + std::to_string(type));
}

const char* ParamTypeName(uint16_t type) {
  static const char* names[] = {"LONG", "BOOL", "FLOAT", "FLOAT2", "FLOAT3", "FLOAT4", "MATRIX3", "MATRIX4", "STRING"};
  return type < 9 ? names[type] : "?";
}

const Section* Body::Find(SectionKind k) const {
  for (auto& s : sections)
    if (s.kind == k) return &s;
  return nullptr;
}
Section* Body::Find(SectionKind k) {
  for (auto& s : sections)
    if (s.kind == k) return &s;
  return nullptr;
}

bool IsBob(const std::vector<uint8_t>& d) { return d.size() >= 4 && memcmp(d.data(), "BOB1", 4) == 0; }

// ---------------------------------------------------------------- read

static Material ReadMaterial(BeReader& r, int ver) {
  Material m;
  m.index = r.U16();
  if (ver == 6) m.flags = r.U32();
  if (m.IsEffect(ver)) {
    m.technique = r.U16();
    m.effect = r.Str();
    const uint16_t n = r.U16();
    for (uint16_t i = 0; i < n; i++) {
      EffectParam p;
      p.name = r.Str();
      p.type = r.U16();
      const int k = ParamValueCount(p.type);
      if (k < 0)
        p.str = r.Str();
      else
        for (int j = 0; j < k; j++) p.ints.push_back(r.I32());
      m.params.push_back(std::move(p));
    }
    return m;
  }
  if (ver == 6)
    m.texture = r.Str();
  else
    m.textureId = r.U16();
  for (int i = 0; i < 14; i++) m.words[i] = r.U16();
  if (ver < 6) m.flagWord = r.U16();
  m.word2c = r.U16();
  const int nMaps = ver == 6 ? 5 : 3;
  // MAT6 order: (name, value) x3 then the two extra pairs; MAT3/5: (id, value) x3
  for (int i = 0; i < nMaps; i++) {
    Material::Map mp;
    if (ver == 6)
      mp.name = r.Str();
    else
      mp.id = r.U16();
    mp.value = r.U16();
    m.maps.push_back(mp);
  }
  return m;
}

static Lod ReadLod(BeReader& r) {
  Lod l;
  l.value = r.I32();
  l.flags = r.U32();
  if (r.PeekTag() == "BONE") {
    r.Expect("BONE");
    l.hasBones = true;
    const int32_t n = r.I32();
    for (int32_t i = 0; i < n; i++) l.bones.push_back(r.Str());
    r.Expect("/BON");
  }
  r.Expect("POIN");
  const int32_t np = r.I32();
  if (np < 0) throw FormatError("negative point count");
  l.points.resize(static_cast<size_t>(np));
  for (auto& p : l.points) {
    p.flags = r.U16();
    if (p.flags & kPos)
      for (auto& v : p.pos) v = r.I32();
    if (p.flags & kUv) {
      for (auto& v : p.uv) v = r.I32();
      if (p.flags & kUv2)
        for (auto& v : p.uv2) v = r.I32();
    }
    if (p.flags & kNormal)
      for (auto& v : p.normal) v = r.I32();
    if (p.flags & kSmooth) p.smooth = r.U32();
  }
  r.Expect("/POI");
  if (r.PeekTag() == "WEIG") {
    r.Expect("WEIG");
    l.hasWeights = true;
    const int32_t n = r.I32();
    if (n != np) throw FormatError("WEIG count differs from the point count");
    l.weights.resize(static_cast<size_t>(n));
    for (auto& w : l.weights) {
      const uint16_t k = r.U16();
      for (uint16_t j = 0; j < k; j++) {
        Weight x;
        x.bone = r.U16();
        x.weight = r.I32();
        w.push_back(x);
      }
    }
    r.Expect("/WEI");
  }
  r.Expect("PART");
  const int32_t nParts = r.I32();
  if (nParts < 0) throw FormatError("negative part count");
  for (int32_t i = 0; i < nParts; i++) {
    Part part;
    part.flags = r.U32();
    const uint16_t ng = r.U16();
    for (uint16_t g = 0; g < ng; g++) {
      Group grp;
      grp.material = r.I32();
      const int32_t nf = r.I32();
      if (nf < 0) throw FormatError("negative face count");
      grp.faces.resize(static_cast<size_t>(nf));
      for (auto& f : grp.faces) {
        for (auto& v : f.v) v = r.I32();
        f.word = r.I32();
      }
      if (part.flags & kPartHasExtra) {
        const int32_t nt = r.I32();
        if (nt < 0) throw FormatError("negative tangent count");
        grp.tangents.resize(static_cast<size_t>(nt));
        for (auto& t : grp.tangents) {
          t.point = r.I32();
          for (auto& v : t.vec) v = r.I32();
        }
      }
      part.groups.push_back(std::move(grp));
    }
    if (part.flags & kPartHasExtra)
      for (auto& v : part.bounds) v = r.I32();
    l.parts.push_back(std::move(part));
  }
  r.Expect("/PAR");
  return l;
}

Body Read(const std::vector<uint8_t>& data) {
  BeReader r(data);
  Body b;
  r.Expect("BOB1");
  for (;;) {
    const size_t at = r.pos();
    const std::string tag = r.Tag();
    if (tag == "/BOB") break;
    Section s;
    if (tag == "INFO") {
      s.kind = SectionKind::Info, s.str = r.Str(), r.Expect("/INF");
    } else if (tag == "NAME") {
      s.kind = SectionKind::Name, s.str = r.Str(), r.Expect("/NAM");
    } else if (tag == "VERS") {
      s.kind = SectionKind::Vers, s.value = r.U32(), r.Expect("/VER");
    } else if (tag == "SND1") {
      s.kind = SectionKind::Snd, s.value = r.U32(), r.Expect("/SND");
    } else if (tag == "MAT3" || tag == "MAT5" || tag == "MAT6") {
      s.kind = SectionKind::Mat;
      s.matVersion = tag[3] - '0';
      const int32_t n = r.I32();
      for (int32_t i = 0; i < n; i++) s.materials.push_back(ReadMaterial(r, s.matVersion));
      r.Expect("/MAT");
    } else if (tag == "BODY") {
      s.kind = SectionKind::Body;
      const uint16_t n = r.U16();
      for (uint16_t i = 0; i < n; i++) s.lods.push_back(ReadLod(r));
      r.Expect("/BOD");
    } else {
      throw FormatError("unknown section '" + tag + "' at " + std::to_string(at));
    }
    b.sections.push_back(std::move(s));
  }
  if (!r.atEnd()) throw FormatError(std::to_string(r.size() - r.pos()) + " bytes after /BOB");
  return b;
}

// ---------------------------------------------------------------- write

static void WriteMaterial(BeWriter& w, const Material& m, int ver) {
  w.U16(m.index);
  if (ver == 6) w.U32(m.flags);
  if (m.IsEffect(ver)) {
    w.U16(m.technique);
    w.Str(m.effect);
    w.U16(static_cast<uint16_t>(m.params.size()));
    for (auto& p : m.params) {
      w.Str(p.name);
      w.U16(p.type);
      if (ParamValueCount(p.type) < 0)
        w.Str(p.str);
      else
        for (int32_t v : p.ints) w.I32(v);
    }
    return;
  }
  if (ver == 6)
    w.Str(m.texture);
  else
    w.U16(m.textureId);
  for (uint16_t v : m.words) w.U16(v);
  if (ver < 6) w.U16(m.flagWord);
  w.U16(m.word2c);
  for (auto& mp : m.maps) {
    if (ver == 6)
      w.Str(mp.name);
    else
      w.U16(mp.id);
    w.U16(mp.value);
  }
}

static void WriteLod(BeWriter& w, const Lod& l) {
  w.I32(l.value);
  w.U32(l.flags);
  if (l.hasBones) {
    w.Tag("BONE");
    w.I32(static_cast<int32_t>(l.bones.size()));
    for (auto& s : l.bones) w.Str(s);
    w.Tag("/BON");
  }
  w.Tag("POIN");
  w.I32(static_cast<int32_t>(l.points.size()));
  for (auto& p : l.points) {
    w.U16(p.flags);
    if (p.flags & kPos)
      for (int32_t v : p.pos) w.I32(v);
    if (p.flags & kUv) {
      for (int32_t v : p.uv) w.I32(v);
      if (p.flags & kUv2)
        for (int32_t v : p.uv2) w.I32(v);
    }
    if (p.flags & kNormal)
      for (int32_t v : p.normal) w.I32(v);
    if (p.flags & kSmooth) w.U32(p.smooth);
  }
  w.Tag("/POI");
  if (l.hasWeights) {
    w.Tag("WEIG");
    w.I32(static_cast<int32_t>(l.weights.size()));
    for (auto& ws : l.weights) {
      w.U16(static_cast<uint16_t>(ws.size()));
      for (auto& x : ws) w.U16(x.bone), w.I32(x.weight);
    }
    w.Tag("/WEI");
  }
  w.Tag("PART");
  w.I32(static_cast<int32_t>(l.parts.size()));
  for (auto& part : l.parts) {
    w.U32(part.flags);
    w.U16(static_cast<uint16_t>(part.groups.size()));
    for (auto& g : part.groups) {
      w.I32(g.material);
      w.I32(static_cast<int32_t>(g.faces.size()));
      for (auto& f : g.faces) {
        for (int32_t v : f.v) w.I32(v);
        w.I32(f.word);
      }
      if (part.flags & kPartHasExtra) {
        w.I32(static_cast<int32_t>(g.tangents.size()));
        for (auto& t : g.tangents) {
          w.I32(t.point);
          for (int32_t v : t.vec) w.I32(v);
        }
      }
    }
    if (part.flags & kPartHasExtra)
      for (int32_t v : part.bounds) w.I32(v);
  }
  w.Tag("/PAR");
}

std::vector<uint8_t> Write(const Body& b) {
  BeWriter w;
  w.Tag("BOB1");
  for (auto& s : b.sections) {
    switch (s.kind) {
      case SectionKind::Info: w.Tag("INFO"), w.Str(s.str), w.Tag("/INF"); break;
      case SectionKind::Name: w.Tag("NAME"), w.Str(s.str), w.Tag("/NAM"); break;
      case SectionKind::Vers: w.Tag("VERS"), w.U32(s.value), w.Tag("/VER"); break;
      case SectionKind::Snd: w.Tag("SND1"), w.U32(s.value), w.Tag("/SND"); break;
      case SectionKind::Mat: {
        const char tag[5] = {'M', 'A', 'T', static_cast<char>('0' + s.matVersion), 0};
        w.Tag(tag);
        w.I32(static_cast<int32_t>(s.materials.size()));
        for (auto& m : s.materials) WriteMaterial(w, m, s.matVersion);
        w.Tag("/MAT");
        break;
      }
      case SectionKind::Body:
        w.Tag("BODY");
        w.U16(static_cast<uint16_t>(s.lods.size()));
        for (auto& l : s.lods) WriteLod(w, l);
        w.Tag("/BOD");
        break;
    }
  }
  w.Tag("/BOB");
  return w.b;
}

}  // namespace x3::bob
