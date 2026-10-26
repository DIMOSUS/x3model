// Binary FBX 7.4 (fbx_write.hpp). The file: the 27-byte header ('Kaydara FBX Binary  ', 0x1A 0x00, version 7400), a
// tree of node records {u32 end offset, u32 property count, u32 property bytes, u8 name length, name, properties,
// children, a 13-byte null record after children}, a null record after the last root node, the footer. Properties are
// typed: C (bool), I (int32), L (int64), D (double), S (string: u32 length + bytes), arrays d/i/f/l (u32 count,
// u32 encoding 0 = raw, u32 bytes, data). Object names are stored as 'name\0\1Class'. Written the way Blender's own
// exporter writes them: GlobalSettings, Documents, Definitions (counts), Objects, Connections.
#include "fbx_write.hpp"

#include "archive.hpp"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace x3::fbx {
namespace {

struct Out {
  std::vector<uint8_t> b;
  struct Open {
    size_t start, propsStart, propsEnd = 0;
    uint32_t props = 0;
    bool children = false;
  };
  std::vector<Open> stack;

  void U8(uint8_t v) { b.push_back(v); }
  void U32(uint32_t v) { Bytes(&v, 4); }
  void I64(int64_t v) { Bytes(&v, 8); }
  void F64(double v) { Bytes(&v, 8); }
  void Bytes(const void* p, size_t n) { b.insert(b.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); }
  void Patch32(size_t at, uint32_t v) { memcpy(b.data() + at, &v, 4); }
  void Null() {
    for (int i = 0; i < 13; i++) U8(0);
  }
  void Begin(const char* name) {
    if (!stack.empty()) {
      Open& p = stack.back();
      if (!p.children) p.propsEnd = b.size(), p.children = true;
    }
    Open o;
    o.start = b.size();
    U32(0), U32(0), U32(0);
    const size_t n = strlen(name);
    U8(static_cast<uint8_t>(n));
    Bytes(name, n);
    o.propsStart = b.size();
    stack.push_back(o);
  }
  void End() {
    Open o = stack.back();
    stack.pop_back();
    if (!o.children) o.propsEnd = b.size();
    if (o.children || !o.props) Null();
    Patch32(o.start, static_cast<uint32_t>(b.size()));
    Patch32(o.start + 4, o.props);
    Patch32(o.start + 8, static_cast<uint32_t>(o.propsEnd - o.propsStart));
  }
  void Prop() { stack.back().props++; }
  void C(bool v) { Prop(), U8('C'), U8(v ? 1 : 0); }
  void I(int32_t v) { Prop(), U8('I'), U32(static_cast<uint32_t>(v)); }
  void L(int64_t v) { Prop(), U8('L'), I64(v); }
  void D(double v) { Prop(), U8('D'), F64(v); }
  void S(const std::string& v) { Prop(), U8('S'), U32(static_cast<uint32_t>(v.size())), Bytes(v.data(), v.size()); }
  template <typename T>
  void Arr(char code, const std::vector<T>& v) {
    Prop(), U8(static_cast<uint8_t>(code)), U32(static_cast<uint32_t>(v.size())), U32(0), U32(static_cast<uint32_t>(v.size() * sizeof(T)));
    if (!v.empty()) Bytes(v.data(), v.size() * sizeof(T));
  }
  void Dv(const std::vector<double>& v) { Arr('d', v); }
  void Iv(const std::vector<int32_t>& v) { Arr('i', v); }
  void Fv(const std::vector<float>& v) { Arr('f', v); }
  void Lv(const std::vector<int64_t>& v) { Arr('l', v); }
  void NodeI(const char* name, int32_t v) { Begin(name), I(v), End(); }
  void NodeL(const char* name, int64_t v) { Begin(name), L(v), End(); }
  void NodeS(const char* name, const std::string& v) { Begin(name), S(v), End(); }
  void NodeC(const char* name, bool v) { Begin(name), C(v), End(); }
  // Properties70 entries
  void PI(const char* name, const char* type, const char* label, const char* flags, int32_t v) {
    Begin("P"), S(name), S(type), S(label), S(flags), I(v), End();
  }
  void PL(const char* name, const char* type, const char* label, const char* flags, int64_t v) {
    Begin("P"), S(name), S(type), S(label), S(flags), L(v), End();
  }
  void PD(const char* name, const char* type, const char* label, const char* flags, double v) {
    Begin("P"), S(name), S(type), S(label), S(flags), D(v), End();
  }
  void PS(const char* name, const char* type, const char* label, const char* flags, const std::string& v) {
    Begin("P"), S(name), S(type), S(label), S(flags), S(v), End();
  }
  void PD3(const char* name, const char* type, const char* label, const char* flags, const double* v) {
    Begin("P"), S(name), S(type), S(label), S(flags), D(v[0]), D(v[1]), D(v[2]), End();
  }
  void User(const fbx::Prop& p) {
    switch (p.kind) {
      case fbx::Prop::kInt:
        if (p.i >= INT32_MIN && p.i <= INT32_MAX)
          PI(p.name.c_str(), "int", "Integer", "AU", static_cast<int32_t>(p.i));
        else
          PD(p.name.c_str(), "double", "Number", "AU", static_cast<double>(p.i));
        break;
      case fbx::Prop::kDouble: PD(p.name.c_str(), "double", "Number", "AU", p.d); break;
      case fbx::Prop::kString: PS(p.name.c_str(), "KString", "", "U", p.s); break;
    }
  }
};

std::string Obj(const char* cls, const std::string& name) { return name + '\0' + '\x01' + cls; }

std::string Sanitized(const std::string& s) {
  std::string r = s;
  for (char& c : r)
    if (c == ':' || c == '\0' || c == '\x01') c = '_';
  return r;
}

constexpr int64_t kTicksPerSecond = 46186158000LL;  // FBX time unit

}  // namespace

bool Write(const std::string& path, const Scene& sc, std::string& error) {
  Out o;
  o.Bytes("Kaydara FBX Binary  ", 21);
  o.U8(0x1A), o.U8(0x00);
  o.U32(7400);

  o.Begin("FBXHeaderExtension");
  o.NodeI("FBXHeaderVersion", 1003);
  o.NodeI("FBXVersion", 7400);
  o.NodeS("Creator", sc.creator);
  o.End();

  const bool animated = sc.animEnd > 0;
  o.Begin("GlobalSettings");
  o.NodeI("Version", 1000);
  o.Begin("Properties70");
  o.PI("UpAxis", "int", "Integer", "", 1), o.PI("UpAxisSign", "int", "Integer", "", 1);
  o.PI("FrontAxis", "int", "Integer", "", 2), o.PI("FrontAxisSign", "int", "Integer", "", 1);
  o.PI("CoordAxis", "int", "Integer", "", 0), o.PI("CoordAxisSign", "int", "Integer", "", 1);
  o.PI("OriginalUpAxis", "int", "Integer", "", -1), o.PI("OriginalUpAxisSign", "int", "Integer", "", 1);
  o.PD("UnitScaleFactor", "double", "Number", "", sc.unitCm);
  o.PD("OriginalUnitScaleFactor", "double", "Number", "", sc.unitCm);
  o.PI("TimeMode", "enum", "", "", 14);  // custom frame rate
  o.PD("CustomFrameRate", "double", "Number", "", sc.fps);
  o.PL("TimeSpanStart", "KTime", "Time", "", 0);
  o.PL("TimeSpanStop", "KTime", "Time", "", static_cast<int64_t>(sc.animEnd * kTicksPerSecond));
  o.End();
  o.End();

  int64_t next = 1000000;
  const int64_t docId = next++;
  o.Begin("Documents");
  o.NodeI("Count", 1);
  o.Begin("Document"), o.L(docId), o.S(""), o.S("Scene");
  o.Begin("Properties70");
  o.Begin("P"), o.S("SourceObject"), o.S("object"), o.S(""), o.S(""), o.End();
  o.PS("ActiveAnimStackName", "KString", "", "", animated ? "Take 001" : "");
  o.End();
  o.Begin("RootNode"), o.L(0), o.End();
  o.End();
  o.End();
  o.Begin("References");
  o.End();

  // counts for Definitions
  size_t textures = 0, curveNodes = 0, curves = 0;
  for (auto& m : sc.materials) textures += m.textures.size();
  for (auto& n : sc.nodes) {
    bool t = false, r = false;
    for (int k = 0; k < 3; k++) {
      if (!n.animT[k].time.empty()) curves++, t = true;
      if (!n.animR[k].time.empty()) curves++, r = true;
    }
    curveNodes += t + r;
  }
  o.Begin("Definitions");
  o.NodeI("Version", 100);
  o.NodeI("Count", static_cast<int32_t>(1 + sc.nodes.size() + sc.meshes.size() + sc.materials.size() + textures * 2 +
                                        (animated ? 2 + curveNodes + curves : 0)));
  auto def = [&](const char* type, size_t n) {
    if (n) o.Begin("ObjectType"), o.S(type), o.NodeI("Count", static_cast<int32_t>(n)), o.End();
  };
  def("GlobalSettings", 1);
  def("Model", sc.nodes.size());
  def("Geometry", sc.meshes.size());
  def("Material", sc.materials.size());
  def("Texture", textures);
  def("Video", textures);
  if (animated) {
    def("AnimationStack", 1), def("AnimationLayer", 1), def("AnimationCurveNode", curveNodes), def("AnimationCurve", curves);
  }
  o.End();

  std::vector<int64_t> nodeIds(sc.nodes.size()), meshIds(sc.meshes.size()), matIds(sc.materials.size());
  struct Conn {
    int64_t child, parent;
    std::string prop;  // empty: OO
  };
  std::vector<Conn> conns;

  o.Begin("Objects");
  for (size_t mi = 0; mi < sc.meshes.size(); mi++) {
    const Mesh& m = sc.meshes[mi];
    const int64_t gid = meshIds[mi] = next++;
    std::vector<int32_t> poly;
    poly.reserve(m.triangles.size());
    for (size_t i = 0; i < m.triangles.size(); i++) poly.push_back(i % 3 == 2 ? ~m.triangles[i] : m.triangles[i]);
    o.Begin("Geometry"), o.L(gid), o.S(Obj("Geometry", Sanitized(m.name))), o.S("Mesh");
    o.Begin("Vertices"), o.Dv(m.positions), o.End();
    o.Begin("PolygonVertexIndex"), o.Iv(poly), o.End();
    o.NodeI("GeometryVersion", 124);
    // normals and UVs are per control point; written ByPolygonVertex (the mapping every importer reads), the UVs
    // IndexToDirect with the control point index as the index
    if (!m.normals.empty()) {
      std::vector<double> nv;
      nv.reserve(m.triangles.size() * 3);
      for (int32_t v : m.triangles) nv.push_back(m.normals[v * 3]), nv.push_back(m.normals[v * 3 + 1]), nv.push_back(m.normals[v * 3 + 2]);
      o.Begin("LayerElementNormal"), o.I(0);
      o.NodeI("Version", 101), o.NodeS("Name", "");
      o.NodeS("MappingInformationType", "ByPolygonVertex"), o.NodeS("ReferenceInformationType", "Direct");
      o.Begin("Normals"), o.Dv(nv), o.End();
      o.End();
    }
    for (int s = 0; s < 2; s++) {
      if (m.uv[s].empty()) continue;
      o.Begin("LayerElementUV"), o.I(s);
      o.NodeI("Version", 101), o.NodeS("Name", s ? "UVMap2" : "UVMap");
      o.NodeS("MappingInformationType", "ByPolygonVertex"), o.NodeS("ReferenceInformationType", "IndexToDirect");
      o.Begin("UV"), o.Dv(m.uv[s]), o.End();
      o.Begin("UVIndex"), o.Iv(m.triangles), o.End();
      o.End();
    }
    o.Begin("LayerElementMaterial"), o.I(0);
    o.NodeI("Version", 101), o.NodeS("Name", "");
    o.NodeS("MappingInformationType", "ByPolygon"), o.NodeS("ReferenceInformationType", "IndexToDirect");
    o.Begin("Materials"), o.Iv(m.triangleMaterial), o.End();
    o.End();
    for (int layer = 0; layer < 2; layer++) {
      if (layer == 1 && m.uv[1].empty()) break;
      o.Begin("Layer"), o.I(layer);
      o.NodeI("Version", 100);
      if (layer == 0) {
        if (!m.normals.empty()) o.Begin("LayerElement"), o.NodeS("Type", "LayerElementNormal"), o.NodeI("TypedIndex", 0), o.End();
        o.Begin("LayerElement"), o.NodeS("Type", "LayerElementMaterial"), o.NodeI("TypedIndex", 0), o.End();
      }
      if (!m.uv[layer].empty()) o.Begin("LayerElement"), o.NodeS("Type", "LayerElementUV"), o.NodeI("TypedIndex", layer), o.End();
      o.End();
    }
    o.End();
  }

  for (size_t ni = 0; ni < sc.nodes.size(); ni++) {
    const Node& n = sc.nodes[ni];
    const int64_t id = nodeIds[ni] = next++;
    o.Begin("Model"), o.L(id), o.S(Obj("Model", Sanitized(n.name))), o.S(n.mesh >= 0 ? "Mesh" : "Null");
    o.NodeI("Version", 232);
    o.Begin("Properties70");
    o.PI("RotationOrder", "enum", "", "", 0);  // XYZ
    o.PD3("Lcl Translation", "Lcl Translation", "", "A", n.translation);
    o.PD3("Lcl Rotation", "Lcl Rotation", "", "A", n.rotation);
    o.PD3("Lcl Scaling", "Lcl Scaling", "", "A", n.scaling);
    if (n.hidden) o.PD("Visibility", "Visibility", "", "A", 0.0);
    for (auto& p : n.props) o.User(p);
    o.End();
    o.NodeC("Shading", true);
    o.NodeS("Culling", "CullingOff");
    o.End();
    conns.push_back({id, n.parent >= 0 ? -1 - n.parent : 0, ""});  // parent patched below
    if (n.mesh >= 0) conns.push_back({meshIds[n.mesh], id, ""});
  }
  for (auto& c : conns)
    if (c.parent < 0) c.parent = nodeIds[-1 - c.parent];
  for (size_t ni = 0; ni < sc.nodes.size(); ni++)
    for (int mat : sc.nodes[ni].materials) conns.push_back({-2 - mat, nodeIds[ni], ""});  // material patched below

  for (size_t mi = 0; mi < sc.materials.size(); mi++) {
    const Material& m = sc.materials[mi];
    const int64_t id = matIds[mi] = next++;
    o.Begin("Material"), o.L(id), o.S(Obj("Material", Sanitized(m.name))), o.S("");
    o.NodeI("Version", 102);
    o.NodeS("ShadingModel", "phong");
    o.NodeI("MultiLayer", 0);
    o.Begin("Properties70");
    o.PD3("DiffuseColor", "Color", "", "A", m.diffuse);
    const double zero[3] = {0, 0, 0};
    o.PD3("SpecularColor", "Color", "", "A", zero);
    for (auto& p : m.props) o.User(p);
    o.End();
    o.End();
    for (auto& t : m.textures) {
      const int64_t tid = next++, vid = next++;
      std::string base = t.file;
      const size_t sl = base.find_last_of("/\\");
      if (sl != std::string::npos) base = base.substr(sl + 1);
      o.Begin("Video"), o.L(vid), o.S(Obj("Video", Sanitized(base))), o.S("Clip");
      o.NodeS("Type", "Clip");
      o.Begin("Properties70"), o.PS("Path", "KString", "XRefUrl", "", t.file), o.End();
      o.NodeI("UseMipMap", 0);
      o.NodeS("Filename", t.file);
      o.NodeS("RelativeFilename", t.file);
      o.End();
      o.Begin("Texture"), o.L(tid), o.S(Obj("Texture", Sanitized(base))), o.S("");
      o.NodeS("Type", "TextureVideoClip");
      o.NodeI("Version", 202);
      o.NodeS("TextureName", Obj("Texture", Sanitized(base)));
      o.Begin("Properties70"), o.End();
      o.NodeS("Media", Obj("Video", Sanitized(base)));
      o.NodeS("FileName", t.file);
      o.NodeS("RelativeFilename", t.file);
      o.Begin("ModelUVTranslation"), o.D(0), o.D(0), o.End();
      o.Begin("ModelUVScaling"), o.D(1), o.D(1), o.End();
      o.NodeS("Texture_Alpha_Source", "None");
      o.Begin("Cropping"), o.I(0), o.I(0), o.I(0), o.I(0), o.End();
      o.End();
      conns.push_back({vid, tid, ""});
      conns.push_back({tid, id, t.channel});
    }
  }
  for (auto& c : conns)
    if (c.child <= -2) c.child = matIds[-2 - c.child];

  // animation: one stack, one layer, a curve node per animated property, a curve per animated component
  if (animated) {
    const int64_t stack = next++, layer = next++;
    o.Begin("AnimationStack"), o.L(stack), o.S(Obj("AnimStack", "Take 001")), o.S("");
    o.Begin("Properties70");
    o.PL("LocalStop", "KTime", "Time", "", static_cast<int64_t>(sc.animEnd * kTicksPerSecond));
    o.PL("ReferenceStop", "KTime", "Time", "", static_cast<int64_t>(sc.animEnd * kTicksPerSecond));
    o.End();
    o.End();
    o.Begin("AnimationLayer"), o.L(layer), o.S(Obj("AnimLayer", "BaseLayer")), o.S(""), o.End();
    conns.push_back({layer, stack, ""});
    for (size_t ni = 0; ni < sc.nodes.size(); ni++) {
      const Node& n = sc.nodes[ni];
      for (int which = 0; which < 2; which++) {
        const Curve* cv = which ? n.animR : n.animT;
        if (cv[0].time.empty() && cv[1].time.empty() && cv[2].time.empty()) continue;
        const int64_t cn = next++;
        const char* prop = which ? "Lcl Rotation" : "Lcl Translation";
        const double* rest = which ? n.rotation : n.translation;
        o.Begin("AnimationCurveNode"), o.L(cn), o.S(Obj("AnimCurveNode", which ? "R" : "T")), o.S("");
        o.Begin("Properties70");
        o.PD("d|X", "Number", "", "A", rest[0]), o.PD("d|Y", "Number", "", "A", rest[1]), o.PD("d|Z", "Number", "", "A", rest[2]);
        o.End();
        o.End();
        conns.push_back({cn, layer, ""});
        conns.push_back({cn, nodeIds[ni], prop});
        static const char* comp[3] = {"d|X", "d|Y", "d|Z"};
        for (int k = 0; k < 3; k++) {
          const Curve& c = cv[k];
          if (c.time.empty()) continue;
          const int64_t cid = next++;
          std::vector<int64_t> kt;
          std::vector<float> kv;
          for (size_t i = 0; i < c.time.size(); i++)
            kt.push_back(static_cast<int64_t>(std::llround(c.time[i] * kTicksPerSecond))), kv.push_back(static_cast<float>(c.value[i]));
          o.Begin("AnimationCurve"), o.L(cid), o.S(Obj("AnimCurve", "")), o.S("");
          o.Begin("Default"), o.D(rest[k]), o.End();
          o.NodeI("KeyVer", 4009);
          o.Begin("KeyTime"), o.Lv(kt), o.End();
          o.Begin("KeyValueFloat"), o.Fv(kv), o.End();
          o.Begin("KeyAttrFlags"), o.Iv(std::vector<int32_t>{4}), o.End();  // linear interpolation
          o.Begin("KeyAttrDataFloat"), o.Iv(std::vector<int32_t>{0, 0, 0, 0}), o.End();
          o.Begin("KeyAttrRefCount"), o.Iv(std::vector<int32_t>{static_cast<int32_t>(kt.size())}), o.End();
          o.End();
          conns.push_back({cid, cn, comp[k]});
        }
      }
    }
  }
  o.End();  // Objects

  o.Begin("Connections");
  for (auto& c : conns) {
    o.Begin("C"), o.S(c.prop.empty() ? "OO" : "OP"), o.L(c.child), o.L(c.parent);
    if (!c.prop.empty()) o.S(c.prop);
    o.End();
  }
  o.End();

  if (animated) {
    o.Begin("Takes");
    o.NodeS("Current", "Take 001");
    o.Begin("Take"), o.S("Take 001");
    o.NodeS("FileName", "Take_001.tak");
    o.Begin("LocalTime"), o.L(0), o.L(static_cast<int64_t>(sc.animEnd * kTicksPerSecond)), o.End();
    o.Begin("ReferenceTime"), o.L(0), o.L(static_cast<int64_t>(sc.animEnd * kTicksPerSecond)), o.End();
    o.End();
    o.End();
  }

  o.Null();
  static const uint8_t footerId[16] = {0xfa, 0xbc, 0xab, 0x09, 0xd0, 0xc8, 0xd4, 0x66, 0xb1, 0x76, 0xfb, 0x83, 0x1c, 0xf7, 0x26, 0x7e};
  static const uint8_t footerMagic[16] = {0xf8, 0x5a, 0x8c, 0x6a, 0xde, 0xf5, 0xd9, 0x7e, 0xec, 0xe9, 0x0c, 0xe3, 0x75, 0x8f, 0x29, 0x0b};
  o.Bytes(footerId, 16);
  o.U32(0);
  size_t pad = ((o.b.size() + 15) & ~static_cast<size_t>(15)) - o.b.size();
  if (!pad) pad = 16;
  for (size_t i = 0; i < pad; i++) o.U8(0);
  o.U32(7400);
  for (int i = 0; i < 120; i++) o.U8(0);
  o.Bytes(footerMagic, 16);
  if (o.b.size() >= 0xFFFFFFFFull) {  // FBX 7.4 keeps node offsets in 32 bits
    error = "the FBX would be 4 GB or larger";
    return false;
  }

  try {
    WriteFile(path, o.b);
  } catch (const std::exception& x) {
    error = x.what();
    return false;
  }
  return true;
}

}  // namespace x3::fbx
