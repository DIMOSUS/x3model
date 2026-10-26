#include "body_fbx.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

#include "archive.hpp"
#include "bytes.hpp"
#include "derive.hpp"
#include "ufbx.h"
#include "util.hpp"

namespace x3 {

namespace {

constexpr double kFix = 65536.0;

// The game is left-handed (Direct3D), FBX right-handed: Z is negated both ways. With the mirror, the game's corner
// order (a, b, c) faces away from the stored normals in FBX terms (measured in Blender: 10645 of 10647 faces of
// argon_M3 against their normals), so faces are written (a, c, b) and read back the same way.
inline double Mirror(int axis, double v) { return axis == 2 ? -v : v; }
constexpr int kCorner[3] = {0, 2, 1};  // FBX corner k = game corner kCorner[k] (its own inverse)

// Positions are stored normalised (the largest coordinate about 65536) and the LOD 0 value is the object's size,
// so one stored unit is value/65536 game units. A body with a non-positive LOD 0 value (one broken shipped asset)
// keeps its raw numbers.
inline double PositionScale(int32_t lod0Value) { return lod0Value > 0 ? lod0Value / kFix : 1.0; }

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string MaterialLabel(const bob::Material& m, int ver, size_t i) {
  std::string base;
  if (m.IsEffect(ver)) {
    base = m.effect;
    for (auto& p : m.params)
      if (p.name == "t_DiffuseTexture" && !p.str.empty()) base = TextureBase(p.str);
  } else {
    base = TextureBase(m.texture);
  }
  const size_t dot = base.rfind('.');
  if (dot != std::string::npos) base = base.substr(0, dot);
  return "m" + std::to_string(i) + "_" + (base.empty() ? "material" : base);
}

std::string ParamsText(const bob::Material& m) {
  std::string s;
  for (auto& p : m.params) {
    s += p.name + " " + bob::ParamTypeName(p.type);
    if (p.type == bob::kString) {
      s += " " + p.str;
    } else if (p.type == bob::kLong || p.type == bob::kBool) {
      s += " " + std::to_string(p.ints[0]);
    } else {
      for (int32_t v : p.ints) s += " " + Num(v / kFix);
    }
    s += "\n";
  }
  return s;
}

std::vector<bob::EffectParam> ParseParams(const std::string& text) {
  std::vector<bob::EffectParam> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    std::istringstream ls(line);
    bob::EffectParam p;
    std::string type;
    ls >> p.name >> type;
    int t = -1;
    for (int k = 0; k < 9; k++)
      if (type == bob::ParamTypeName(static_cast<uint16_t>(k))) t = k;
    if (t < 0) throw FormatError("unknown parameter type '" + type + "' in: " + line);
    p.type = static_cast<uint16_t>(t);
    if (p.type == bob::kString) {
      const size_t at = line.find(type) + type.size();
      p.str = at < line.size() ? line.substr(at + 1) : "";
    } else if (p.type == bob::kLong || p.type == bob::kBool) {
      long long v = 0;
      ls >> v;
      p.ints.push_back(static_cast<int32_t>(v));
    } else {
      const int n = bob::ParamValueCount(p.type);
      for (int k = 0; k < n; k++) {
        double v = 0;
        ls >> v;
        p.ints.push_back(static_cast<int32_t>(std::llround(v * kFix)));
      }
    }
    out.push_back(std::move(p));
  }
  return out;
}

std::string ClassicText(const bob::Material& m, int ver) {
  std::string s;
  if (ver == 6)
    s += "texture " + m.texture + "\n";
  else
    s += "textureid " + std::to_string(m.textureId) + "\n";
  s += "words";
  for (uint16_t w : m.words) s += " " + std::to_string(w);
  s += "\nflagword " + std::to_string(m.flagWord) + "\nword2c " + std::to_string(m.word2c) + "\n";
  for (auto& mp : m.maps) s += "map " + std::to_string(mp.id) + " " + std::to_string(mp.value) + " " + mp.name + "\n";
  return s;
}

void ParseClassic(const std::string& text, bob::Material& m) {
  std::istringstream in(text);
  std::string line;
  m.maps.clear();
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream ls(line);
    std::string key;
    ls >> key;
    if (key == "texture") {
      m.texture = line.size() > 8 ? line.substr(8) : "";
    } else if (key == "textureid") {
      ls >> m.textureId;
    } else if (key == "words") {
      for (auto& w : m.words) ls >> w;
    } else if (key == "flagword") {
      ls >> m.flagWord;
    } else if (key == "word2c") {
      ls >> m.word2c;
    } else if (key == "map") {
      bob::Material::Map mp;
      ls >> mp.id >> mp.value;
      std::string rest;
      std::getline(ls, rest);
      if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
      mp.name = rest;
      m.maps.push_back(mp);
    }
  }
}

const char* ChannelOf(const std::string& param) {
  if (param == "t_DiffuseTexture") return "DiffuseColor";
  if (param == "t_BumpTexture") return "NormalMap";
  if (param == "t_SpecularTexture") return "SpecularColor";
  if (param == "t_LightMapTexture") return "EmissiveColor";
  return nullptr;
}

}  // namespace

std::string TextureBase(const std::string& path) {
  const size_t sl = path.find_last_of("/\\");
  return sl == std::string::npos ? path : path.substr(sl + 1);
}

// ================================================================ export

fbx::Scene BodyToFbx(const bob::Body& body, const std::string& name, const ExportOptions& opt,
                     std::vector<std::pair<std::string, std::string>>* textures) {
  fbx::Scene sc;
  sc.unitCm = opt.unitCm;
  sc.creator = "x3model (X3 body " + name + ")";

  fbx::Node root;
  root.name = name;
  root.props.push_back(fbx::Prop::Str("x3_kind", "body"));
  std::string order;
  for (auto& s : body.sections) {
    static const char* names[] = {"INFO", "NAME", "VERS", "SND1", "MAT", "BODY"};
    order += std::string(order.empty() ? "" : " ") + names[static_cast<int>(s.kind)];
    switch (s.kind) {
      case bob::SectionKind::Info: root.props.push_back(fbx::Prop::Str("x3_info", s.str)); break;
      case bob::SectionKind::Name: root.props.push_back(fbx::Prop::Str("x3_name", s.str)); break;
      case bob::SectionKind::Vers: root.props.push_back(fbx::Prop::Int("x3_vers", s.value)); break;
      case bob::SectionKind::Snd: root.props.push_back(fbx::Prop::Int("x3_snd", s.value)); break;
      case bob::SectionKind::Mat: root.props.push_back(fbx::Prop::Int("x3_mat_version", s.matVersion)); break;
      default: break;
    }
  }
  root.props.push_back(fbx::Prop::Str("x3_sections", order));
  sc.nodes.push_back(root);

  // materials
  std::set<std::string> copied;
  if (const bob::Section* ms = body.Find(bob::SectionKind::Mat)) {
    for (size_t i = 0; i < ms->materials.size(); i++) {
      const bob::Material& m = ms->materials[i];
      fbx::Material fm;
      fm.name = MaterialLabel(m, ms->matVersion, i);
      fm.props.push_back(fbx::Prop::Int("x3_index", m.index));
      fm.props.push_back(fbx::Prop::Int("x3_flags", m.flags));
      auto addTexture = [&](const std::string& channel, const std::string& path) {
        if (opt.textureDir.empty() || path.empty() || Lower(path).find("none_") != std::string::npos) return;
        std::string base = TextureBase(path);
        const size_t dot = base.rfind('.');
        const std::string stem = dot == std::string::npos ? base : base.substr(0, dot);
        fm.textures.push_back({channel, opt.textureDir + "/" + stem + ".dds"});
        if (textures && copied.insert(stem).second) textures->push_back({stem, opt.textureDir + "/" + stem + ".dds"});
      };
      if (m.IsEffect(ms->matVersion)) {
        fm.props.push_back(fbx::Prop::Str("x3_kind", "effect"));
        fm.props.push_back(fbx::Prop::Int("x3_technique", m.technique));
        fm.props.push_back(fbx::Prop::Str("x3_effect", m.effect));
        fm.props.push_back(fbx::Prop::Str("x3_params", ParamsText(m)));
        for (auto& p : m.params)
          if (p.type == bob::kString)
            if (const char* ch = ChannelOf(p.name)) addTexture(ch, p.str);
      } else {
        fm.props.push_back(fbx::Prop::Str("x3_kind", "classic"));
        fm.props.push_back(fbx::Prop::Str("x3_classic", ClassicText(m, ms->matVersion)));
        addTexture("DiffuseColor", m.texture);
      }
      sc.materials.push_back(std::move(fm));
    }
  }
  std::vector<int> allSlots(sc.materials.size());
  for (size_t i = 0; i < allSlots.size(); i++) allSlots[i] = static_cast<int>(i);

  const bob::Section* bs = body.Find(bob::SectionKind::Body);
  if (!bs) return sc;
  const double scale = bs->lods.empty() ? 1.0 : PositionScale(bs->lods[0].value);
  root.props.push_back(fbx::Prop::Int("x3_lods", static_cast<int64_t>(bs->lods.size())));
  sc.nodes[0].props = root.props;

  for (size_t li = 0; li < bs->lods.size(); li++) {
    const bob::Lod& lod = bs->lods[li];
    fbx::Node ln;
    ln.name = name + "_LOD" + std::to_string(li);
    ln.parent = 0;
    ln.hidden = li > 0;
    ln.props.push_back(fbx::Prop::Int("x3_lod", static_cast<int64_t>(li)));
    ln.props.push_back(fbx::Prop::Int("x3_value", lod.value));
    ln.props.push_back(fbx::Prop::Int("x3_flags", lod.flags));
    ln.props.push_back(fbx::Prop::Int("x3_points", static_cast<int64_t>(lod.points.size())));
    if (lod.hasBones) {
      std::string b;
      for (auto& s : lod.bones) b += s + "\n";
      ln.props.push_back(fbx::Prop::Str("x3_bones", b));
    }
    if (lod.hasWeights) {
      std::vector<uint8_t> blob;
      for (auto& ws : lod.weights) {
        PutI32(blob, static_cast<int32_t>(ws.size()));
        for (auto& w : ws) PutI32(blob, w.bone), PutI32(blob, w.weight);
      }
      ln.props.push_back(fbx::Prop::Str("x3_weights", Base64(blob)));
    }
    const int lodNode = static_cast<int>(sc.nodes.size());
    sc.nodes.push_back(ln);

    // which points each part uses (points are shared by the LOD's parts); unreferenced points go to part 0
    std::vector<int> owner(lod.points.size(), -1);
    for (size_t pi = 0; pi < lod.parts.size(); pi++)
      for (auto& g : lod.parts[pi].groups)
        for (auto& f : g.faces)
          for (int32_t v : f.v)
            if (v >= 0 && static_cast<size_t>(v) < owner.size() && owner[v] < 0) owner[v] = static_cast<int>(pi);
    for (auto& o : owner)
      if (o < 0) o = 0;

    for (size_t pi = 0; pi < lod.parts.size(); pi++) {
      const bob::Part& part = lod.parts[pi];
      // control points of this part: the points it owns plus the shared points it references, in point order
      std::vector<char> used(lod.points.size(), 0);
      for (size_t v = 0; v < owner.size(); v++)
        if (owner[v] == static_cast<int>(pi)) used[v] = 1;
      for (auto& g : part.groups)
        for (auto& f : g.faces)
          for (int32_t v : f.v)
            if (v >= 0 && static_cast<size_t>(v) < used.size()) used[v] = 1;
      std::vector<int32_t> globalOf, localOf(lod.points.size(), -1);
      for (size_t v = 0; v < used.size(); v++)
        if (used[v]) localOf[v] = static_cast<int32_t>(globalOf.size()), globalOf.push_back(static_cast<int32_t>(v));

      fbx::Mesh mesh;
      mesh.name = name + "_LOD" + std::to_string(li) + "_part" + std::to_string(pi);
      bool anyUv2 = false;
      for (int32_t g : globalOf) anyUv2 |= (lod.points[g].flags & bob::kUv2) != 0;
      std::vector<uint8_t> pointBlob;
      for (int32_t g : globalOf) {
        const bob::Point& p = lod.points[g];
        for (int k = 0; k < 3; k++) mesh.positions.push_back(Mirror(k, p.pos[k] * scale));
        for (int k = 0; k < 3; k++) mesh.normals.push_back(Mirror(k, p.normal[k] / kFix));
        mesh.uv[0].push_back(p.uv[0] / kFix), mesh.uv[0].push_back(1.0 - p.uv[1] / kFix);
        if (anyUv2) mesh.uv[1].push_back(p.uv2[0] / kFix), mesh.uv[1].push_back(1.0 - p.uv2[1] / kFix);
        PutI32(pointBlob, p.flags), PutI32(pointBlob, static_cast<int32_t>(p.smooth));
      }
      std::string groups;
      std::vector<uint8_t> faceWords, tangents;
      bool wordsAllOne = true;
      for (size_t gi = 0; gi < part.groups.size(); gi++) {
        const bob::Group& g = part.groups[gi];
        groups += (groups.empty() ? "" : " ") + std::to_string(g.material) + ":" + std::to_string(g.faces.size());
        const int slot = g.material >= 0 && static_cast<size_t>(g.material) < sc.materials.size() ? g.material : 0;
        for (auto& f : g.faces) {
          for (int k = 0; k < 3; k++) {
            const int32_t v = f.v[kCorner[k]];
            mesh.triangles.push_back(v >= 0 && static_cast<size_t>(v) < localOf.size() ? localOf[v] : 0);
          }
          mesh.triangleMaterial.push_back(slot);
          PutI32(faceWords, f.word);
          wordsAllOne &= f.word == 1;
        }
        PutI32(tangents, static_cast<int32_t>(g.tangents.size()));
        for (auto& t : g.tangents) {
          PutI32(tangents, t.point);
          for (int32_t v : t.vec) PutI32(tangents, v);
        }
      }
      fbx::Node pn;
      pn.name = mesh.name;
      pn.parent = lodNode;
      pn.hidden = li > 0;
      pn.mesh = static_cast<int>(sc.meshes.size());
      pn.materials = allSlots;
      pn.props.push_back(fbx::Prop::Int("x3_part", static_cast<int64_t>(pi)));
      pn.props.push_back(fbx::Prop::Int("x3_flags", part.flags));
      pn.props.push_back(fbx::Prop::Str("x3_groups", groups));
      pn.props.push_back(fbx::Prop::Int("x3_vertex_count", static_cast<int64_t>(globalOf.size())));
      pn.props.push_back(fbx::Prop::Int("x3_face_count", static_cast<int64_t>(mesh.triangleMaterial.size())));
      bool identity = globalOf.size() == lod.points.size();
      for (size_t i = 0; identity && i < globalOf.size(); i++) identity = globalOf[i] == static_cast<int32_t>(i);
      if (!identity) {
        std::vector<uint8_t> blob;
        for (int32_t g : globalOf) PutI32(blob, g);
        pn.props.push_back(fbx::Prop::Str("x3_point_index", Base64(blob)));
      }
      pn.props.push_back(fbx::Prop::Str("x3_point_data", Base64(pointBlob)));
      if (!wordsAllOne) pn.props.push_back(fbx::Prop::Str("x3_face_words", Base64(faceWords)));
      if (part.flags & bob::kPartHasExtra) {
        pn.props.push_back(fbx::Prop::Str("x3_tangents", Base64(tangents)));
        pn.props.push_back(fbx::Prop::Str("x3_bounds", JoinInts(part.bounds.data(), part.bounds.size())));
      }
      sc.meshes.push_back(std::move(mesh));
      sc.nodes.push_back(std::move(pn));
    }
  }
  return sc;
}

// ================================================================ import

namespace {

std::string PropStr(const ufbx_props& props, const char* name, const char* def = "") {
  const ufbx_prop* p = ufbx_find_prop(&props, name);
  return p ? std::string(p->value_str.data, p->value_str.length) : def;
}
bool HasProp(const ufbx_props& props, const char* name) { return ufbx_find_prop(&props, name) != nullptr; }
long long PropInt(const ufbx_props& props, const char* name, long long def = 0) {
  const ufbx_prop* p = ufbx_find_prop(&props, name);
  if (!p) return def;
  if (p->type == UFBX_PROP_STRING) return std::stoll(std::string(p->value_str.data, p->value_str.length));
  return p->value_int ? p->value_int : static_cast<long long>(std::llround(p->value_real));
}

std::string Str(const ufbx_string& s) { return std::string(s.data, s.length); }

void CollectMeshNodes(ufbx_node* n, std::vector<ufbx_node*>& out) {
  if (n->mesh) out.push_back(n);
  for (size_t i = 0; i < n->children.count; i++) CollectMeshNodes(n->children.data[i], out);
}

bob::Material DefaultEffectMaterial(const ufbx_material* um, uint16_t index) {
  bob::Material m;
  m.index = index;
  m.flags = 0x02000000;
  m.technique = 1;
  m.effect = "standard_lighting.fx";
  auto add = [&](const char* name, uint16_t type, int32_t v) {
    bob::EffectParam p;
    p.name = name, p.type = type, p.ints = {v};
    m.params.push_back(p);
  };
  auto addStr = [&](const char* name, const std::string& v) {
    bob::EffectParam p;
    p.name = name, p.type = bob::kString, p.str = v;
    m.params.push_back(p);
  };
  add("g_EnableFog", bob::kBool, 0);
  std::string diff;
  if (um && um->fbx.diffuse_color.texture) diff = TextureBase(Str(um->fbx.diffuse_color.texture->filename));
  addStr("t_DiffuseTexture", diff);
  addStr("effectfile", "standard_lighting.fx");
  return m;
}

}  // namespace

bob::Body FbxToBody(const std::string& fbxPath, ImportReport& rep) {
  ufbx_load_opts o{};
  o.target_axes = ufbx_axes_right_handed_y_up;
  o.target_unit_meters = 0.001;  // game units (1 unit = 1 mm in our files); a re-export in metres is scaled back
  o.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
  o.generate_missing_normals = true;
  ufbx_error err{};
  ufbx_scene* scene = ufbx_load_file(fbxPath.c_str(), &o, &err);
  if (!scene) {
    char b[1024];
    ufbx_format_error(b, sizeof b, &err);
    throw std::runtime_error(std::string("FBX load failed: ") + b);
  }
  struct Free {
    ufbx_scene* s;
    ~Free() { ufbx_free_scene(s); }
  } freeScene{scene};

  // the body root: the node marked x3_kind = body, else the scene root
  ufbx_node* root = scene->root_node;
  for (size_t i = 0; i < scene->nodes.count; i++)
    if (PropStr(scene->nodes.data[i]->props, "x3_kind") == "body") root = scene->nodes.data[i];

  bob::Body body;
  const ufbx_props& rp = root->props;
  // materials: those carrying x3_index in index order, then any other material used by a mesh
  std::vector<const ufbx_material*> mats;
  for (size_t i = 0; i < scene->materials.count; i++)
    if (HasProp(scene->materials.data[i]->props, "x3_index")) mats.push_back(scene->materials.data[i]);
  std::stable_sort(mats.begin(), mats.end(), [](const ufbx_material* a, const ufbx_material* b) {
    // the FBX material names start with m<i>_, which keeps their original order
    auto num = [](const ufbx_material* m) {
      const std::string n = Str(m->name);
      return n.size() > 1 && n[0] == 'm' ? atoi(n.c_str() + 1) : 1 << 30;
    };
    return num(a) < num(b);
  });
  const int matVersion = static_cast<int>(PropInt(rp, "x3_mat_version", 6));
  std::map<const ufbx_material*, int> matIndex;
  for (size_t i = 0; i < mats.size(); i++) matIndex[mats[i]] = static_cast<int>(i);
  for (size_t i = 0; i < scene->materials.count; i++)
    if (!matIndex.count(scene->materials.data[i])) {
      matIndex[scene->materials.data[i]] = static_cast<int>(mats.size());
      mats.push_back(scene->materials.data[i]);
      rep.notes.push_back("new material " + Str(scene->materials.data[i]->name) + ": standard_lighting.fx template");
    }

  bob::Section matSec;
  matSec.kind = bob::SectionKind::Mat;
  matSec.matVersion = matVersion;
  for (size_t i = 0; i < mats.size(); i++) {
    const ufbx_props& mp = mats[i]->props;
    if (!HasProp(mp, "x3_index")) {
      matSec.materials.push_back(DefaultEffectMaterial(mats[i], static_cast<uint16_t>(i)));
      continue;
    }
    bob::Material m;
    m.index = static_cast<uint16_t>(PropInt(mp, "x3_index"));
    m.flags = static_cast<uint32_t>(PropInt(mp, "x3_flags"));
    if (PropStr(mp, "x3_kind") == "effect") {
      m.technique = static_cast<uint16_t>(PropInt(mp, "x3_technique"));
      m.effect = PropStr(mp, "x3_effect");
      m.params = ParseParams(PropStr(mp, "x3_params"));
    } else {
      ParseClassic(PropStr(mp, "x3_classic"), m);
    }
    matSec.materials.push_back(std::move(m));
  }

  // LOD nodes: children (any depth) of the root with x3_lod, else LOD0 = everything
  std::vector<std::pair<int, ufbx_node*>> lodNodes;
  for (size_t i = 0; i < scene->nodes.count; i++) {
    ufbx_node* n = scene->nodes.data[i];
    if (HasProp(n->props, "x3_lod")) lodNodes.push_back({static_cast<int>(PropInt(n->props, "x3_lod")), n});
  }
  std::sort(lodNodes.begin(), lodNodes.end(), [](auto& a, auto& b) { return a.first < b.first; });
  if (lodNodes.empty()) lodNodes.push_back({0, root});

  bob::Section bodySec;
  bodySec.kind = bob::SectionKind::Body;
  double scale = 0;  // game units per 1/65536 of a position (LOD 0 value / 65536)
  for (auto& [lodIndex, ln] : lodNodes) {
    bob::Lod lod;
    const ufbx_props& lp = ln->props;
    lod.value = static_cast<int32_t>(PropInt(lp, "x3_value", 0));
    lod.flags = static_cast<uint32_t>(PropInt(lp, "x3_flags", 0));
    if (HasProp(lp, "x3_bones")) {
      lod.hasBones = true;
      std::istringstream in(PropStr(lp, "x3_bones"));
      std::string s;
      while (std::getline(in, s))
        if (!s.empty()) lod.bones.push_back(s);
    }
    std::vector<ufbx_node*> partNodes;
    CollectMeshNodes(ln, partNodes);
    std::stable_sort(partNodes.begin(), partNodes.end(), [](ufbx_node* a, ufbx_node* b) {
      return PropInt(a->props, "x3_part", 1 << 30) < PropInt(b->props, "x3_part", 1 << 30);
    });
    const long long storedPoints = PropInt(lp, "x3_points", -1);

    // geometry of every part in game units (Z mirrored back)
    struct Corner {
      int cp;  // control point of the part
      double pos[3], nrm[3], uv[2], uv2[2];
    };
    struct PartGeo {
      ufbx_node* node;
      std::vector<std::array<Corner, 3>> tris;
      std::vector<int> triMat;  // body material index
      int cpCount;
      bool hasUv2;
    };
    std::vector<PartGeo> geos;
    double maxAbs = 0;
    for (ufbx_node* pn : partNodes) {
      const ufbx_mesh* mesh = pn->mesh;
      PartGeo g;
      g.node = pn;
      g.cpCount = static_cast<int>(mesh->num_vertices);
      const ufbx_uv_set* uvSet[2] = {nullptr, nullptr};
      for (size_t s = 0; s < mesh->uv_sets.count; s++) {
        const std::string nm = Str(mesh->uv_sets.data[s].name);
        if (nm == "UVMap2" && !uvSet[1]) uvSet[1] = &mesh->uv_sets.data[s];
      }
      if (mesh->uv_sets.count) uvSet[0] = &mesh->uv_sets.data[0];
      if (!uvSet[1] && mesh->uv_sets.count > 1) uvSet[1] = &mesh->uv_sets.data[1];
      if (uvSet[1] == uvSet[0]) uvSet[1] = nullptr;
      g.hasUv2 = uvSet[1] != nullptr;
      const ufbx_matrix& m = pn->geometry_to_world;
      const ufbx_matrix nm = ufbx_matrix_for_normals(&m);
      std::vector<uint32_t> tri(mesh->max_face_triangles * 3);
      for (size_t fi = 0; fi < mesh->faces.count; fi++) {
        const ufbx_face face = mesh->faces.data[fi];
        int bodyMat = 0;
        if (mesh->face_material.count) {
          const uint32_t slot = mesh->face_material.data[fi];
          if (slot < pn->materials.count && matIndex.count(pn->materials.data[slot])) bodyMat = matIndex[pn->materials.data[slot]];
        }
        const uint32_t nt = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
        for (uint32_t t = 0; t < nt; t++) {
          std::array<Corner, 3> c;
          for (int k = 0; k < 3; k++) {
            const uint32_t ix = tri[t * 3 + k];
            Corner& cr = c[k];
            cr.cp = static_cast<int>(mesh->vertex_indices.data[ix]);
            const ufbx_vec3 p = ufbx_transform_position(&m, ufbx_get_vertex_vec3(&mesh->vertex_position, ix));
            // (not renormalised here: the game's 16.16 normals are not exactly unit length, and an unedited
            // file must give them back as they were; the generic path below normalises)
            const ufbx_vec3 n = ufbx_transform_direction(&nm, ufbx_get_vertex_vec3(&mesh->vertex_normal, ix));
            cr.pos[0] = p.x, cr.pos[1] = p.y, cr.pos[2] = -p.z;
            cr.nrm[0] = n.x, cr.nrm[1] = n.y, cr.nrm[2] = -n.z;
            cr.uv[0] = cr.uv[1] = cr.uv2[0] = cr.uv2[1] = 0;
            if (uvSet[0]) {
              const ufbx_vec2 uv = ufbx_get_vertex_vec2(&uvSet[0]->vertex_uv, ix);
              cr.uv[0] = uv.x, cr.uv[1] = 1.0 - uv.y;
            }
            if (uvSet[1]) {
              const ufbx_vec2 uv = ufbx_get_vertex_vec2(&uvSet[1]->vertex_uv, ix);
              cr.uv2[0] = uv.x, cr.uv2[1] = 1.0 - uv.y;
            }
            for (double v : cr.pos) maxAbs = std::max(maxAbs, std::fabs(v));
          }
          g.tris.push_back(c);
          g.triMat.push_back(bodyMat);
        }
      }
      geos.push_back(std::move(g));
    }
    if (lodIndex == lodNodes.front().first) {
      if (HasProp(lp, "x3_value"))
        scale = PositionScale(lod.value);
      else {  // a new body: positions normalised to 65536, the LOD 0 value is the size
        lod.value = static_cast<int32_t>(std::ceil(std::max(maxAbs, 1.0)));
        scale = lod.value / kFix;
        rep.exact = false;
        rep.notes.push_back("LOD 0 value (object size) computed: " + std::to_string(lod.value));
      }
    } else if (!HasProp(lp, "x3_value")) {
      lod.value = 0;
      rep.notes.push_back("LOD " + std::to_string(lodIndex) + " has no switch threshold (x3_value); 0 used");
    }

    auto toInt = [](double v) { return static_cast<int32_t>(std::llround(v)); };
    // points: exact path = every control point has one attribute set and the stored data matches in count
    bool exact = storedPoints >= 0;
    std::string why = exact ? "" : "no stored point count";
    for (auto& g : geos) {
      const ufbx_props& pp = g.node->props;
      if (PropInt(pp, "x3_vertex_count", -1) != g.cpCount)
        exact = false, why = Str(g.node->name) + ": " + std::to_string(g.cpCount) + " vertices, stored " +
                              std::to_string(PropInt(pp, "x3_vertex_count", -1));
      else if (PropInt(pp, "x3_face_count", -1) != static_cast<long long>(g.tris.size()))
        exact = false, why = Str(g.node->name) + ": " + std::to_string(g.tris.size()) + " triangles, stored " +
                              std::to_string(PropInt(pp, "x3_face_count", -1));
    }
    lod.points.clear();
    std::vector<std::vector<int32_t>> partPointOf(geos.size());  // per part: control point -> LOD point
    if (exact) {
      lod.points.resize(static_cast<size_t>(storedPoints));
      std::vector<char> filled(lod.points.size(), 0);
      for (size_t gi = 0; gi < geos.size() && exact; gi++) {
        auto& g = geos[gi];
        const ufbx_props& pp = g.node->props;
        std::vector<int32_t> globalOf(g.cpCount);
        if (HasProp(pp, "x3_point_index")) {
          const auto blob = Unbase64(PropStr(pp, "x3_point_index"));
          size_t at = 0;
          for (auto& v : globalOf) v = GetI32(blob, at);
        } else {
          for (int i = 0; i < g.cpCount; i++) globalOf[i] = i;
        }
        const auto pd = Unbase64(PropStr(pp, "x3_point_data"));
        if (pd.size() != static_cast<size_t>(g.cpCount) * 8) {
          exact = false, why = Str(g.node->name) + ": stored point data does not match the vertices";
          break;
        }
        // one attribute set per control point
        std::vector<const Corner*> seen(g.cpCount, nullptr);
        for (auto& t : g.tris)
          for (auto& c : t) {
            if (!exact) break;
            const Corner*& s = seen[c.cp];
            if (!s) {
              s = &c;
              continue;
            }
            // equal up to the DCC's own precision: positions and UVs within float32 rounding; normals within 1e-3
            // of their length (Blender keeps custom normals in its own encoding, about 1e-4 off; some shipped
            // normals are far from unit length)
            auto near = [](double a, double b) { return std::fabs(a - b) <= std::max(2.0, std::fabs(a) * 2e-6); };
            const double nlen = std::sqrt(s->nrm[0] * s->nrm[0] + s->nrm[1] * s->nrm[1] + s->nrm[2] * s->nrm[2]);
            for (int k = 0; k < 3 && exact; k++)
              exact = std::fabs(s->nrm[k] - c.nrm[k]) <= std::max(1e-3 * nlen, 2.0 / kFix) &&
                      near(s->pos[k] / scale, c.pos[k] / scale);
            for (int k = 0; k < 2 && exact; k++)
              exact = near(s->uv[k] * kFix, c.uv[k] * kFix) && near(s->uv2[k] * kFix, c.uv2[k] * kFix);
            if (!exact) {
              char b[256];
              snprintf(b, sizeof b, " (n %.6g %.6g %.6g / %.6g %.6g %.6g, uv %.6g %.6g / %.6g %.6g)", s->nrm[0],
                       s->nrm[1], s->nrm[2], c.nrm[0], c.nrm[1], c.nrm[2], s->uv[0], s->uv[1], c.uv[0], c.uv[1]);
              why = Str(g.node->name) + ": vertex " + std::to_string(c.cp) + " has different normals/UVs per face" + b;
            }
          }
        if (!exact) break;
        size_t at = 0;
        for (int cp = 0; cp < g.cpCount; cp++) {
          const int32_t gl = globalOf[cp];
          if (gl < 0 || static_cast<size_t>(gl) >= lod.points.size()) {
            exact = false;
            break;
          }
          const uint16_t flags = static_cast<uint16_t>(GetI32(pd, at));
          const uint32_t smooth = static_cast<uint32_t>(GetI32(pd, at));
          if (filled[gl]) continue;
          filled[gl] = 1;
          bob::Point& p = lod.points[gl];
          p.flags = flags, p.smooth = smooth;
          if (const Corner* c = seen[cp]) {
            for (int k = 0; k < 3; k++) p.pos[k] = toInt(c->pos[k] / scale), p.normal[k] = toInt(c->nrm[k] * kFix);
            for (int k = 0; k < 2; k++) p.uv[k] = toInt(c->uv[k] * kFix), p.uv2[k] = toInt(c->uv2[k] * kFix);
          } else {  // a point no face uses: the control point itself
            const ufbx_vec3 v = ufbx_transform_position(&g.node->geometry_to_world, g.node->mesh->vertices.data[cp]);
            p.pos[0] = toInt(v.x / scale), p.pos[1] = toInt(v.y / scale), p.pos[2] = toInt(-v.z / scale);
          }
        }
        partPointOf[gi] = globalOf;
      }
      for (size_t i = 0; exact && i < filled.size(); i++)
        if (!filled[i]) exact = false, why = "point " + std::to_string(i) + " belongs to no part";
    }
    if (!exact) {
      rep.exact = false;
      rep.notes.push_back("LOD " + std::to_string(lodIndex) + ": rebuilt from the geometry (" + why + ")");
      lod.points.clear();
      // generic: a point per distinct (control point, normal, uv, uv2), parts after each other
      for (size_t gi = 0; gi < geos.size(); gi++) {
        auto& g = geos[gi];
        std::map<std::array<int32_t, 8>, int32_t> key;
        partPointOf[gi].clear();
        for (auto& t : g.tris)
          for (auto& c : t) {
            std::array<int32_t, 8> k = {c.cp, toInt(c.nrm[0] * 4096), toInt(c.nrm[1] * 4096), toInt(c.nrm[2] * 4096),
                                        toInt(c.uv[0] * kFix), toInt(c.uv[1] * kFix), toInt(c.uv2[0] * kFix), toInt(c.uv2[1] * kFix)};
            if (key.count(k)) continue;
            key[k] = static_cast<int32_t>(lod.points.size());
            bob::Point p;
            p.flags = bob::kPos | bob::kUv | bob::kNormal | bob::kSmooth | (g.hasUv2 ? bob::kUv2 : 0);
            double len = std::sqrt(c.nrm[0] * c.nrm[0] + c.nrm[1] * c.nrm[1] + c.nrm[2] * c.nrm[2]);
            if (len <= 0) len = 1;
            for (int i = 0; i < 3; i++) p.pos[i] = toInt(c.pos[i] / scale), p.normal[i] = toInt(c.nrm[i] / len * kFix);
            for (int i = 0; i < 2; i++) p.uv[i] = toInt(c.uv[i] * kFix), p.uv2[i] = toInt(c.uv2[i] * kFix);
            p.smooth = 1;
            lod.points.push_back(p);
          }
        // per corner: its point
        std::vector<int32_t> cornerPoint;
        for (auto& t : g.tris)
          for (auto& c : t) {
            std::array<int32_t, 8> k = {c.cp, toInt(c.nrm[0] * 4096), toInt(c.nrm[1] * 4096), toInt(c.nrm[2] * 4096),
                                        toInt(c.uv[0] * kFix), toInt(c.uv[1] * kFix), toInt(c.uv2[0] * kFix), toInt(c.uv2[1] * kFix)};
            cornerPoint.push_back(key[k]);
          }
        partPointOf[gi] = std::move(cornerPoint);
      }
      if (std::any_of(geos.begin(), geos.end(), [](auto& g) { return g.hasUv2; })) lod.flags |= 0x20;
    }

    // parts
    for (size_t gi = 0; gi < geos.size(); gi++) {
      auto& g = geos[gi];
      const ufbx_props& pp = g.node->props;
      bob::Part part;
      part.flags = static_cast<uint32_t>(PropInt(pp, "x3_flags", exact ? 0 : 0x30000001));
      if (!exact) part.flags |= bob::kPartHasExtra;
      // groups: stored (material:count) runs when the face count is unchanged, else one group per material
      std::vector<std::pair<int32_t, int32_t>> runs;
      if (exact) {
        std::istringstream in(PropStr(pp, "x3_groups"));
        std::string tok;
        long long total = 0;
        while (in >> tok) {
          const size_t c = tok.find(':');
          runs.push_back({std::stoi(tok.substr(0, c)), std::stoi(tok.substr(c + 1))});
          total += runs.back().second;
        }
        if (total != static_cast<long long>(g.tris.size())) runs.clear();
      }
      // game corner k of a triangle = FBX corner kCorner[k]
      auto pointOf = [&](size_t tri, int k) -> int32_t {
        const int c = kCorner[k];
        if (exact) return partPointOf[gi][g.tris[tri][c].cp];
        return partPointOf[gi][tri * 3 + c];
      };
      if (!runs.empty()) {
        size_t t = 0;
        for (auto& [mat, count] : runs) {
          bob::Group grp;
          grp.material = mat;
          for (int32_t i = 0; i < count; i++, t++) {
            bob::Face f;
            for (int k = 0; k < 3; k++) f.v[k] = pointOf(t, k);
            f.word = 1;
            grp.faces.push_back(f);
          }
          part.groups.push_back(std::move(grp));
        }
      } else {
        std::map<int, size_t> groupOf;
        for (size_t t = 0; t < g.tris.size(); t++) {
          const int mat = g.triMat[t];
          if (!groupOf.count(mat)) groupOf[mat] = part.groups.size(), part.groups.push_back(bob::Group{mat, {}, {}});
          bob::Face f;
          for (int k = 0; k < 3; k++) f.v[k] = pointOf(t, k);
          f.word = 1;
          part.groups[groupOf[mat]].faces.push_back(f);
        }
      }
      if (exact && HasProp(pp, "x3_face_words")) {
        const auto blob = Unbase64(PropStr(pp, "x3_face_words"));
        size_t at = 0;
        for (auto& grp : part.groups)
          for (auto& f : grp.faces) f.word = GetI32(blob, at);
      }
      // tangent records and bounds: stored, or derived
      bool haveExtra = false;
      if (exact && (part.flags & bob::kPartHasExtra) && HasProp(pp, "x3_tangents")) {
        const auto blob = Unbase64(PropStr(pp, "x3_tangents"));
        size_t at = 0;
        for (auto& grp : part.groups) {
          const int32_t n = GetI32(blob, at);
          grp.tangents.resize(static_cast<size_t>(std::max(n, 0)));
          for (auto& t : grp.tangents) {
            t.point = GetI32(blob, at);
            for (auto& v : t.vec) v = GetI32(blob, at);
          }
        }
        const auto b = SplitInts(PropStr(pp, "x3_bounds"));
        for (size_t i = 0; i < part.bounds.size() && i < b.size(); i++) part.bounds[i] = static_cast<int32_t>(b[i]);
        haveExtra = at == blob.size() && b.size() == part.bounds.size();
      }
      if ((part.flags & bob::kPartHasExtra) && !haveExtra) {
        DeriveTangents(lod, part);
        DeriveBounds(lod, part);
      }
      lod.parts.push_back(std::move(part));
      rep.parts++;
      rep.faces += static_cast<int>(g.tris.size());
    }
    if (exact && HasProp(lp, "x3_weights")) {
      lod.hasWeights = true;
      const auto blob = Unbase64(PropStr(lp, "x3_weights"));
      size_t at = 0;
      lod.weights.resize(lod.points.size());
      for (auto& ws : lod.weights) {
        const int32_t n = GetI32(blob, at);
        for (int32_t i = 0; i < n; i++) {
          bob::Weight w;
          w.bone = static_cast<uint16_t>(GetI32(blob, at));
          w.weight = GetI32(blob, at);
          ws.push_back(w);
        }
      }
    }
    rep.points += static_cast<int>(lod.points.size());
    rep.lods++;
    bodySec.lods.push_back(std::move(lod));
  }

  // sections in the stored order
  std::istringstream order(PropStr(rp, "x3_sections", "INFO MAT BODY"));
  std::string tag;
  while (order >> tag) {
    bob::Section s;
    if (tag == "INFO") {
      s.kind = bob::SectionKind::Info;
      s.str = PropStr(rp, "x3_info", "x3model");
    } else if (tag == "NAME") {
      s.kind = bob::SectionKind::Name, s.str = PropStr(rp, "x3_name");
    } else if (tag == "VERS") {
      s.kind = bob::SectionKind::Vers, s.value = static_cast<uint32_t>(PropInt(rp, "x3_vers"));
    } else if (tag == "SND1") {
      s.kind = bob::SectionKind::Snd, s.value = static_cast<uint32_t>(PropInt(rp, "x3_snd"));
    } else if (tag == "MAT") {
      s = matSec;
    } else if (tag == "BODY") {
      s = bodySec;
    } else {
      continue;
    }
    body.sections.push_back(std::move(s));
  }
  return body;
}

}  // namespace x3
