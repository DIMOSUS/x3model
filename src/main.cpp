// x3model: X3 Terran Conflict / Albion Prelude bodies and scenes <-> FBX.
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <cmath>
#include <filesystem>
#include <memory>
#include <sstream>

#include "archive.hpp"
#include "bob.hpp"
#include "body_fbx.hpp"
#include "bytes.hpp"
#include "derive.hpp"
#include "fbx_write.hpp"
#include "scene.hpp"
#include "scene_anim.hpp"
#include "scene_fbx.hpp"
#include "textbody.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace x3;

namespace {

int Usage() {
  fprintf(stderr,
          "usage:\n"
          "  x3model verify <game dir>      read and re-write every body and scene of every catalogue, compare bytes\n"
          "  x3model info <body>            print a body's sections, LODs and materials, or a scene's parts\n"
          "  x3model export <body> <out.fbx> [--no-textures] [--unit-mm N] [--bake-anim [fps]]\n"
          "                                 body or scene -> FBX (textures copied next to it when it comes from the game)\n"
          "  x3model import <in.fbx> <out.bob|out.pbb>   FBX -> body, or scene (CUT1) for a scene FBX\n"
          "  x3model pack <out.cat> [--force] <path inside>=<file> ...   write a catalogue (e.g. addon\\07.cat);\n"
          "                                 an existing catalogue is replaced only with --force\n"
          "  x3model roundtrip-fbx <game dir> [filter]     every body -> FBX -> body, compare bytes\n"
          "  x3model roundtrip-scenes <game dir> [filter]  every scene -> FBX -> scene, compare bytes\n"
          "  x3model derive-check <game dir> [filter]      recomputed tangents/bounds against the shipped ones\n"
          "\n"
          "<body> is a file (.bob/.pbb/.bod/.pbd) or \"<game dir>|<path in the catalogues>\", e.g.\n"
          "  \"D:\\GAMES\\X3 Terran Conflict|objects/ships/argon/argon_M3.pbb\"\n");
  return 2;
}

bob::Body ReadAnyBody(const std::vector<uint8_t>& data);

bool EndsWith(const std::string& s, const char* suf) {
  const size_t n = strlen(suf);
  return s.size() >= n && Game::Key(s).compare(s.size() - n, n, suf) == 0;
}

int CmdVerify(const std::string& dir) {
  Game game(dir);
  std::map<std::string, int> count;
  int shown = 0;
  for (auto& e : game.entries) {
    if (!EndsWith(e.path, ".pbb") && !EndsWith(e.path, ".bob")) continue;
    std::vector<uint8_t> data;
    try {
      data = game.Read(e);
    } catch (const std::exception& x) {
      count["unpack error"]++;
      if (shown++ < 20) printf("%s %s: %s\n", game.mounts[e.mount].label.c_str(), e.path.c_str(), x.what());
      continue;
    }
    if (scene::IsCut1(data)) {
      try {
        const scene::Scene sc = scene::ReadCut1(data);
        count[scene::WriteCut1(sc) == data ? "CUT1 byte-identical" : "CUT1 DIFFERENT"]++;
      } catch (const std::exception& x) {
        count["CUT1 read error"]++;
        if (shown++ < 20) printf("%s %s: %s\n", game.mounts[e.mount].label.c_str(), e.path.c_str(), x.what());
      }
      continue;
    }
    if (!bob::IsBob(data)) {
      count["not BOB1"]++;
      continue;
    }
    try {
      const bob::Body b = bob::Read(data);
      if (bob::Write(b) == data)
        count["BOB1 byte-identical"]++;
      else {
        count["BOB1 DIFFERENT"]++;
        if (shown++ < 20) printf("%s %s: re-written bytes differ\n", game.mounts[e.mount].label.c_str(), e.path.c_str());
      }
    } catch (const std::exception& x) {
      count["BOB1 read error"]++;
      if (shown++ < 20) printf("%s %s: %s\n", game.mounts[e.mount].label.c_str(), e.path.c_str(), x.what());
    }
  }
  // text members (.pbd/.bod): bodies are read like the engine's text loader, scenes are counted
  std::map<std::string, int> tcount;
  for (auto& e : game.entries) {
    if (!EndsWith(e.path, ".pbd") && !EndsWith(e.path, ".bod")) continue;
    std::vector<uint8_t> data;
    try {
      data = game.Read(e);
    } catch (...) {
      tcount["text: unpack error"]++;
      continue;
    }
    if (scene::IsTextScene(data)) {
      try {
        const scene::Scene sc = scene::ReadText(data);
        // text -> CUT1 -> tree must describe the same parts and keys
        const scene::Scene back = scene::ReadCut1(scene::WriteCut1(sc));
        bool same = back.parts.size() == sc.parts.size();
        for (size_t i = 0; same && i < sc.parts.size(); i++) {
          const auto &a = sc.parts[i], &b = back.parts[i];
          same = a.index == b.index && a.body == b.body && a.mode == b.mode && a.parent == b.parent &&
                 a.flags == b.flags && a.name == b.name && a.constraints == b.constraints && a.notes == b.notes &&
                 a.keys.size() == b.keys.size();
          for (size_t k = 0; same && k < a.keys.size(); k++)
            same = a.keys[k].flags == b.keys[k].flags && a.keys[k].values == b.keys[k].values;
        }
        tcount[same ? "text scene read, same as CUT1" : "text scene: CUT1 DIFFERENT"]++;
      } catch (const std::exception& x) {
        tcount["text scene refused"]++;
        if (shown++ < 30) printf("%s %s: %s\n", game.mounts[e.mount].label.c_str(), e.path.c_str(), x.what());
      }
      continue;
    }
    try {
      textbody::Read(data);
      tcount["text body read"]++;
    } catch (const std::exception& x) {
      std::string why = x.what();
      tcount["text body refused: " + why.substr(0, 60)]++;
      if (shown++ < 30) printf("%s %s: %s\n", game.mounts[e.mount].label.c_str(), e.path.c_str(), x.what());
    }
  }
  for (auto& [k, v] : count) printf("%-26s %d\n", k.c_str(), v);
  for (auto& [k, v] : tcount) printf("%-26s %d\n", k.c_str(), v);
  // (read errors and refusals are listed, not failed on: the shipped game has a few)
  return count.count("BOB1 DIFFERENT") || count.count("CUT1 DIFFERENT") || tcount.count("text scene: CUT1 DIFFERENT") ? 1 : 0;
}

// Loads a body or scene given as a file path, or as a path inside the game's catalogues ("game:objects/ships/x").
std::vector<uint8_t> LoadInput(const std::string& spec, std::string* label) {
  const size_t c = spec.find('|');
  if (c != std::string::npos) {  // "<game dir>|<path inside>"
    Game game(spec.substr(0, c));
    std::string inner = spec.substr(c + 1), where;
    std::vector<uint8_t> out;
    if (!game.Find(inner, out, &where)) throw std::runtime_error("not found in the game: " + inner);
    if (label) *label = where + ":" + inner;
    return out;
  }
  if (label) *label = spec;
  return Unpack(ReadFile(spec));
}

int CmdInfo(const std::string& spec) {
  std::string label;
  const auto data = LoadInput(spec, &label);
  if (scene::IsCut1(data) || scene::IsTextScene(data)) {
    const scene::Scene sc = scene::Read(data);
    printf("%s\n%s scene, version %d, %zu parts%s%s\n", label.c_str(), sc.binary ? "CUT1" : "text", sc.version,
           sc.parts.size(), sc.info ? ", INFO " : "", sc.info ? sc.info->c_str() : "");
    const char* only = getenv("X3_PART");
    for (auto& p : sc.parts) {
      if (only && std::to_string(p.index) != only) continue;
      printf("  P%u body '%s' C %d F %d flags %x%s%s%s, %zu keys\n", p.index, p.body.c_str(), p.mode, p.parent, p.flags,
             p.name ? " name " : "", p.name ? p.name->c_str() : "", p.embedded.empty() && p.inlineText.empty() ? "" : " (embedded body)",
             p.keys.size());
      if (only)
        for (auto& k : p.keys) {
          printf("    {%#x;", k.flags);
          for (int32_t v : k.values) printf(" %d;", v);
          printf(" }\n");
        }
    }
    return 0;
  }
  const bob::Body b = ReadAnyBody(data);
  printf("%s\n", label.c_str());
  for (auto& s : b.sections) {
    switch (s.kind) {
      case bob::SectionKind::Info: printf("INFO %s\n", s.str.c_str()); break;
      case bob::SectionKind::Name: printf("NAME %s\n", s.str.c_str()); break;
      case bob::SectionKind::Vers: printf("VERS %u\n", s.value); break;
      case bob::SectionKind::Snd: printf("SND1 %u\n", s.value); break;
      case bob::SectionKind::Mat:
        printf("MAT%d %zu materials\n", s.matVersion, s.materials.size());
        for (auto& m : s.materials) {
          if (m.IsEffect(s.matVersion)) {
            printf("  %u effect %s tech %u flags %08x, %zu params:", m.index, m.effect.c_str(), m.technique, m.flags,
                   m.params.size());
            for (auto& p : m.params)
              if (p.type == bob::kString && !p.str.empty()) printf(" %s=%s", p.name.c_str(), p.str.c_str());
            printf("\n");
            if (getenv("X3_PARAMS"))
              for (auto& p : m.params) {
                printf("      %s %s", p.name.c_str(), bob::ParamTypeName(p.type));
                if (p.type == bob::kString) printf(" %s", p.str.c_str());
                else
                  for (int32_t v : p.ints) printf(" %d", v);
                printf("\n");
              }
          } else {
            printf("  %u classic tex '%s' flags %08x\n", m.index, m.texture.c_str(), m.flags);
          }
        }
        break;
      case bob::SectionKind::Body:
        printf("BODY %zu LOD\n", s.lods.size());
        for (size_t li = 0; li < s.lods.size(); li++) {
          const auto& l = s.lods[li];
          int32_t mn[3] = {INT32_MAX, INT32_MAX, INT32_MAX}, mx[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
          std::map<int, int> pf;
          for (auto& p : l.points) {
            pf[p.flags]++;
            for (int k = 0; k < 3; k++) mn[k] = std::min(mn[k], p.pos[k]), mx[k] = std::max(mx[k], p.pos[k]);
          }
          size_t faces = 0, groups = 0, tang = 0;
          std::map<uint32_t, int> partFlags;
          for (auto& part : l.parts) {
            partFlags[part.flags]++;
            groups += part.groups.size();
            for (auto& g : part.groups) faces += g.faces.size(), tang += g.tangents.size();
          }
          printf("  LOD %zu value %d flags %08x: %zu points, %zu parts, %zu groups, %zu faces, %zu tangent records\n", li,
                 l.value, l.flags, l.points.size(), l.parts.size(), groups, faces, tang);
          printf("    bounds x %d..%d y %d..%d z %d..%d\n", mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
          printf("    point flags:");
          for (auto& [k, v] : pf) printf(" %02x x%d", k, v);
          printf("; part flags:");
          for (auto& [k, v] : partFlags) printf(" %08x x%d", k, v);
          printf("\n");
          if (!l.parts.empty() && !l.parts[0].groups.empty()) {
            const auto& p0 = l.parts[0];
            printf("    part 0 bounds:");
            for (int32_t v : p0.bounds) printf(" %d", v);
            const auto& g0 = p0.groups[0];
            if (!g0.faces.empty())
              printf("\n    group 0 material %d, face 0 (%d %d %d word %d)", g0.material, g0.faces[0].v[0],
                     g0.faces[0].v[1], g0.faces[0].v[2], g0.faces[0].word);
            if (!g0.tangents.empty()) {
              printf(", tangent 0: point %d", g0.tangents[0].point);
              for (int32_t v : g0.tangents[0].vec) printf(" %d", v);
            }
            printf("\n");
          }
        }
        break;
    }
  }
  return 0;
}

// a binary (BOB1) or text body
bob::Body ReadAnyBody(const std::vector<uint8_t>& data) {
  if (bob::IsBob(data)) return bob::Read(data);
  if (textbody::IsTextBody(data)) return textbody::Read(data);
  throw FormatError("not a body (a scene?)");
}

// A scene part's body ("ships\argon\argon_M3"): objects/<name> with the engine's extension order; among the
// catalogues the highest one holding the stem wins, then pbb > bob > pbd > bod inside it.
// types/BodyData: "id; shadow; collision; explosion; filename;" (filename blank: v\%05d)
std::string BodyDataName(const Game& game, int id) {
  static std::map<int, std::string> names;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    for (const char* path : {"addon/types/BodyData.pck", "types/BodyData.pck"}) {  // the addon's table replaces the other
      std::vector<uint8_t> d;
      if (!game.Find(path, d)) continue;
      std::string text(d.begin(), d.end()), line;
      std::istringstream in(text);
      while (std::getline(in, line)) {
        if (line.empty() || line[0] == '/') continue;
        std::vector<std::string> f;
        size_t at = 0;
        while (at <= line.size()) {
          const size_t e = line.find(';', at);
          std::string v = line.substr(at, e == std::string::npos ? std::string::npos : e - at);
          while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
          while (!v.empty() && v[0] == ' ') v.erase(0, 1);
          f.push_back(v);
          if (e == std::string::npos) break;
          at = e + 1;
        }
        if (f.size() >= 5 && !f[4].empty()) names[atoi(f[0].c_str())] = f[4];
      }
      break;
    }
  }
  auto it = names.find(id);
  if (it != names.end()) return it->second;
  char b[16];
  snprintf(b, sizeof b, "v\\%05d", id);
  return b;
}

std::vector<uint8_t> LoadGameBody(const Game& game, const std::string& bodyRef) {
  static const char* exts[] = {".pbb", ".bob", ".pbd", ".bod"};
  std::string body = bodyRef;
  if (!body.empty() && std::all_of(body.begin(), body.end(), [](char c) { return isdigit(static_cast<unsigned char>(c)); }))
    body = BodyDataName(game, atoi(body.c_str()));  // a body id
  std::replace(body.begin(), body.end(), '\\', '/');
  // a loose file in the game folder comes before every catalogue (Archive::ResolveName), in extension order
  for (const char* x : exts) {
    const std::filesystem::path loose = Path(game.dir()) / Path("objects/" + body + x);
    if (std::filesystem::is_regular_file(loose)) {
      std::vector<uint8_t> d = Unpack(ReadFile(Utf8(loose)));
      if (scene::IsCut1(d) || scene::IsTextScene(d)) return {};
      return d;
    }
  }
  const CatEntry* best = nullptr;
  for (const char* x : exts) {
    auto it = game.winners.find(Game::Key("objects/" + body + x));
    if (it == game.winners.end()) continue;
    const CatEntry& e = game.entries[it->second];
    if (!best || e.mount > best->mount) best = &e;
  }
  if (!best) return {};
  std::vector<uint8_t> d = game.Read(*best);
  if (scene::IsCut1(d) || scene::IsTextScene(d)) return {};  // a nested scene, not a body
  return d;
}

std::string Stem(const std::string& path) {
  std::string b = TextureBase(path);
  const size_t dot = b.rfind('.');
  return dot == std::string::npos ? b : b.substr(0, dot);
}

// The texture a material names, out of the catalogues: dds/<stem>.dds|.pck, then tex/<stem>.jpg|.tga
bool FindTexture(const Game& game, const std::string& stem, std::vector<uint8_t>& out, std::string& ext) {
  static const char* tries[][2] = {{"dds/", ".dds"}, {"dds/", ".pck"}, {"tex/", ".jpg"}, {"tex/", ".tga"},
                                   {"tex/", ".pck"}, {"textures/", ".jpg"}, {"dds/", ".jpg"}};
  for (auto& t : tries)
    if (game.Find(std::string(t[0]) + stem + t[1], out)) {
      ext = std::string(t[1]) == ".pck" ? (out.size() >= 4 && memcmp(out.data(), "DDS ", 4) == 0 ? ".dds" : ".jpg") : t[1];
      if (ext == ".jpg" && out.size() >= 3 && out[0] == 0 && out[1] == 0 && out[2] == 2) ext = ".tga";
      return true;
    }
  return false;
}

int CmdExport(const std::vector<std::string>& args) {
  std::string in, out, texDir = "textures";
  double unitMm = 1, bakeFps = 0;
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] == "--no-textures") {
      texDir.clear();
    } else if (args[i] == "--bake-anim") {
      bakeFps = 30;
      if (i + 1 < args.size() && isdigit(static_cast<unsigned char>(args[i + 1][0]))) bakeFps = atof(args[++i].c_str());
      if (!(bakeFps > 0)) throw std::runtime_error("--bake-anim needs a positive frame rate");
    } else if (args[i] == "--unit-mm") {
      if (i + 1 >= args.size()) return Usage();
      char* end = nullptr;
      unitMm = strtod(args[++i].c_str(), &end);
      if (*end || !(unitMm > 0)) throw std::runtime_error("--unit-mm needs a positive number, not " + args[i]);
    } else if (in.empty()) {
      in = args[i];
    } else if (out.empty()) {
      out = args[i];
    } else {
      return Usage();
    }
  }
  if (in.empty() || out.empty()) return Usage();
  std::unique_ptr<Game> game;
  const size_t bar = in.find('|');
  if (bar != std::string::npos) game = std::make_unique<Game>(in.substr(0, bar));
  std::string label;
  const auto data = LoadInput(in, &label);
  ExportOptions opt;
  opt.unitCm = unitMm / 10.0;
  opt.bakeFps = bakeFps;
  opt.textureDir = game ? texDir : "";
  std::vector<std::pair<std::string, std::string>> textures;
  fbx::Scene sc;
  if (scene::IsCut1(data) || scene::IsTextScene(data)) {
    const scene::Scene s = scene::Read(data);
    BodyLoader loader;
    if (game) loader = [&](const std::string& body) { return LoadGameBody(*game, body); };
    sc = SceneToFbx(s, Stem(out), opt, loader, &textures);
  } else {
    sc = BodyToFbx(ReadAnyBody(data), Stem(out), opt, &textures);
  }
  const std::filesystem::path outDir = Path(out).parent_path();
  int copied = 0, missing = 0;
  for (auto& [stem, rel] : textures) {
    std::vector<uint8_t> tex;
    std::string ext;
    if (!FindTexture(*game, stem, tex, ext)) {
      missing++;
      fprintf(stderr, "texture not found in the game: %s\n", stem.c_str());
      continue;
    }
    std::string file = rel.substr(0, rel.size() - 4) + ext;
    if (ext != ".dds")
      for (auto& m : sc.materials)
        for (auto& t : m.textures)
          if (t.file == rel) t.file = file;
    WriteFile(Utf8(outDir / Path(file)), tex);
    copied++;
  }
  std::string err;
  if (!fbx::Write(out, sc, err)) throw std::runtime_error(err);
  printf("%s -> %s: %zu nodes, %zu meshes, %zu materials, %d textures copied%s\n", label.c_str(), out.c_str(),
         sc.nodes.size(), sc.meshes.size(), sc.materials.size(), copied,
         missing ? (", " + std::to_string(missing) + " missing").c_str() : "");
  return 0;
}

int CmdImport(const std::string& in, const std::string& out) {
  if (FbxIsScene(in)) {
    SceneImportReport rep;
    const scene::Scene sc = FbxToScene(in, rep);
    std::vector<uint8_t> bytes = scene::WriteCut1(sc);
    if (EndsWith(out, ".pbb")) bytes = PackGzip(bytes);
    WriteFile(out, bytes);
    printf("%s -> %s: scene, %d parts (%d with their stored keys, %d rebuilt, %d new)\n", in.c_str(), out.c_str(),
           rep.parts, rep.keptKeys, rep.rebuiltKeys, rep.newParts);
    for (auto& n : rep.notes) printf("  %s\n", n.c_str());
    return 0;
  }
  ImportReport rep;
  const bob::Body body = FbxToBody(in, rep);
  std::vector<uint8_t> bytes = bob::Write(body);
  if (EndsWith(out, ".pbb")) bytes = PackGzip(bytes);
  WriteFile(out, bytes);
  printf("%s -> %s: %d LOD, %d parts, %d points, %d faces; %s\n", in.c_str(), out.c_str(), rep.lods, rep.parts,
         rep.points, rep.faces, rep.exact ? "stored data used (unedited)" : "derived data recomputed (edited)");
  for (auto& n : rep.notes) printf("  %s\n", n.c_str());
  return 0;
}

// --perturb: in every animated part with 3+ keys the middle FBX key is edited (even parts: x + 100; odd parts:
// Euler Y + 10 degrees); after import that key must show the edit, every other key its old pose, nothing added
struct PerturbStats {
  int parts = 0, ok = 0, keysSame = 0, keysReencoded = 0;
};
// the FBX node of each part (by x3_order; body geometry nodes sit between them)
std::vector<size_t> PartNodes(const fbx::Scene& f, size_t parts) {
  std::vector<size_t> out(parts, 0);
  for (size_t i = 0; i < f.nodes.size(); i++)
    for (auto& p : f.nodes[i].props)
      if (p.name == "x3_order" && p.i >= 0 && static_cast<size_t>(p.i) < parts) out[p.i] = i;
  return out;
}
// a camera's target node: kind 2, the middle target key moved x + 100
int TargetNode(const fbx::Scene& f, size_t pi) {
  for (size_t i = 0; i < f.nodes.size(); i++)
    for (auto& p : f.nodes[i].props)
      if (p.name == "x3_target_of" && p.i == static_cast<int64_t>(pi)) return static_cast<int>(i);
  return -1;
}
void PerturbAndCheck(const scene::Scene& sc, fbx::Scene& f, std::vector<std::pair<size_t, int>>& edits) {
  const auto nodeOf = PartNodes(f, sc.parts.size());
  for (size_t pi = 0; pi < sc.parts.size(); pi++) {
    const int tn = TargetNode(f, pi);
    if (tn >= 0 && f.nodes[tn].animT[0].time.size() >= 3) {
      auto& c = f.nodes[tn].animT[0];
      const size_t mid = c.time.size() / 2;
      if (c.time[mid] != c.time[mid - 1] && c.time[mid] != c.time[mid + 1]) {
        c.value[mid] += 100;
        edits.push_back({pi, 2});
        continue;
      }
    }
    fbx::Node& n = f.nodes[nodeOf[pi]];
    const auto& tt = n.animT[0].time;
    if (tt.size() < 3) continue;
    const size_t mid = tt.size() / 2;
    if (tt[mid] == tt[mid - 1] || tt[mid] == tt[mid + 1]) continue;
    int kind = static_cast<int>(edits.size() % 2);
    if (sc.parts[pi].keys[mid].flags & 8) kind = 0;  // target mode: the rotation comes from the target
    if (kind == 0) n.animT[0].value[mid] += 100;
    else n.animR[1].value[mid] += 10;
    edits.push_back({pi, kind});
  }
}
std::string CheckPerturbed(const scene::Part& before, const scene::Part& after, int kind, const fbx::Node& n, PerturbStats& st) {
  if (after.keys.size() != before.keys.size())
    return std::to_string(before.keys.size()) + " keys became " + std::to_string(after.keys.size());
  if (kind == 2) {  // exactly the middle target key moved by 100 in x
    std::vector<size_t> tk;
    for (size_t j = 0; j < before.keys.size(); j++)
      if ((before.keys[j].flags & 8) && !(before.keys[j].flags & 0x40)) tk.push_back(j);
    const size_t mid = tk[tk.size() / 2];
    for (size_t j = 0; j < before.keys.size(); j++) {
      scene::Key want = before.keys[j];
      if (j == mid) {
        const auto l = scene::KeyLayout(want.flags);
        for (size_t i = 0; i < l.size(); i++)
          if (strcmp(l[i].name, "tx") == 0) want.values[i] += 100;
      }
      if (after.keys[j].flags != want.flags || after.keys[j].values != want.values)
        return "key " + std::to_string(j) + (j == mid ? " (the edited target)" : "") + " differs";
      if (j != mid) st.keysSame++;
    }
    return "";
  }
  const size_t mid = n.animT[0].time.size() / 2;
  const auto a = KeyPosesFbx(before), b = KeyPosesFbx(after);
  // (keys without their own position / rotation are interpolated by the engine: an edit next to them moves them;
  // a camera's orientation follows its target)
  bool aimed = false;
  for (auto& k : before.keys) aimed |= (k.flags & 8) != 0;
  auto ownRot = [](uint32_t f) { return !(f & 8) && ((f & 2) || !(f & 0x20)) && !(f & 0x200 && !(f & 2)); };
  for (size_t j = 0; j < a.size(); j++) {
    if (j != mid && !before.keys[j].Has("x") && (aimed || !ownRot(before.keys[j].flags))) continue;
    std::array<double, 7> want = a[j];
    if (j == mid && kind == 0) want[0] += 100;
    if (j == mid && kind == 1) {
      const double e[3] = {n.animR[0].value[mid], n.animR[1].value[mid], n.animR[2].value[mid]};
      const auto q = EulerQuat(e);
      for (int k = 0; k < 4; k++) want[3 + k] = q[k];
    }
    for (int k = 0; k < 3 && (j == mid || before.keys[j].Has("x")); k++)
      if (std::fabs(want[k] - b[j][k]) > std::max(0.5, std::fabs(want[k]) * 1.2e-7)) {  // (FBX curves are float32)
        char m[200];
        snprintf(m, sizeof m, "key %zu of %zu (mid %zu) position %.1f %.1f %.1f, want %.1f %.1f %.1f, flags %#x->%#x", j,
                 a.size(), mid, b[j][0], b[j][1], b[j][2], want[0], want[1], want[2], before.keys[j].flags, after.keys[j].flags);
        return m;
      }
    const double dot = want[3] * b[j][3] + want[4] * b[j][4] + want[5] * b[j][5] + want[6] * b[j][6];
    if (!aimed && (j == mid || ownRot(before.keys[j].flags)) && std::fabs(dot) < 1 - 1e-8)
      return "key " + std::to_string(j) + " rotation off (dot " + std::to_string(dot) + ")";
    if (j != mid) {
      if (after.keys[j].flags == before.keys[j].flags && after.keys[j].values == before.keys[j].values) st.keysSame++;
      else st.keysReencoded++;
    }
  }
  return "";
}

// every scene of the game: scene -> FBX -> scene, compare the CUT1 bytes
int CmdRoundtripScenes(const std::string& dir, const std::string& filter, bool perturb, bool bake) {
  Game game(dir);
  const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "x3model_rt_scene.fbx";
  std::map<std::string, int> count;
  PerturbStats pst;
  int shown = 0;
  for (auto& [key, idx] : game.winners) {
    const CatEntry& e = game.entries[idx];
    if (!EndsWith(e.path, ".pbb") && !EndsWith(e.path, ".bob") && !EndsWith(e.path, ".pbd") && !EndsWith(e.path, ".bod")) continue;
    if (!filter.empty() && key.find(Game::Key(filter)) == std::string::npos) continue;
    std::vector<uint8_t> data;
    try {
      data = game.Read(e);
    } catch (...) {
      continue;
    }
    if (!scene::IsCut1(data) && !scene::IsTextScene(data)) continue;
    try {
      const scene::Scene sc = scene::Read(data);
      const auto want = scene::WriteCut1(sc);
      ExportOptions opt;
      if (bake) opt.bakeFps = 30;
      fbx::Scene f = SceneToFbx(sc, Stem(e.path), opt, nullptr, nullptr);
      std::vector<std::pair<size_t, int>> edits;
      if (perturb) {
        PerturbAndCheck(sc, f, edits);
        if (edits.empty()) continue;
      }
      std::string err;
      if (!fbx::Write(Utf8(tmp), f, err)) throw std::runtime_error(err);
      SceneImportReport rep;
      const scene::Scene back = FbxToScene(Utf8(tmp), rep);
      if (perturb) {
        const auto nodeOf = PartNodes(f, sc.parts.size());
        for (auto& [pi, kind] : edits) {
          pst.parts++;
          const std::string why = CheckPerturbed(sc.parts[pi], back.parts[pi], kind, f.nodes[nodeOf[pi]], pst);
          if (why.empty()) pst.ok++;
          else if (shown++ < 15)
            printf("%s P%u (%s): %s\n", e.path.c_str(), sc.parts[pi].index, kind == 2 ? "target" : kind ? "rotation" : "position", why.c_str());
        }
        continue;
      }
      const auto got = scene::WriteCut1(back);
      if (got == want) {
        count["identical"]++;
      } else {
        count["DIFFERENT"]++;
        if (shown++ < 15) {
          size_t at = 0;
          while (at < got.size() && at < want.size() && got[at] == want[at]) at++;
          printf("%s: differs at byte %zu of %zu/%zu (%d parts rebuilt)", e.path.c_str(), at, want.size(), got.size(), rep.rebuiltKeys);
          if (!rep.notes.empty()) printf(" - %s", rep.notes[0].c_str());
          printf("\n");
        }
      }
    } catch (const std::exception& x) {
      count["error"]++;
      if (shown++ < 15) printf("%s: %s\n", e.path.c_str(), x.what());
    }
  }
  if (perturb) {
    printf("edited parts %d, as expected %d; other keys byte-identical %d, re-encoded (same pose) %d\n", pst.parts,
           pst.ok, pst.keysSame, pst.keysReencoded);
    return pst.ok == pst.parts ? 0 : 1;
  }
  for (auto& [k, v] : count) printf("%-12s %d\n", k.c_str(), v);
  return count.size() == 1 && count.count("identical") ? 0 : 1;
}

// the engine's animation of every part of a scene, sampled: "t P x y z m00 .. m22" per line (16.16 matrix; for
// checking scene_anim against the reference x3scene.py evaluate_tracks)
int CmdAnimSample(const std::string& spec, int stepMs) {
  const scene::Scene sc = scene::Read(LoadInput(spec, nullptr));
  for (auto& p : sc.parts) {
    const anim::Tracks tr = anim::Build(p);
    const int64_t end = anim::Length(tr);
    for (int64_t t = 0;; t = std::min<int64_t>(t + stepMs, end)) {
      const anim::State s = anim::Evaluate(tr, t);
      printf("%lld %u", static_cast<long long>(t), p.index);
      if (s.pos) printf(" %d %d %d", (*s.pos)[0], (*s.pos)[1], (*s.pos)[2]);
      else printf(" - - -");
      for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
          if (s.m) printf(" %d", (*s.m)[r][c]);
          else printf(" -");
      printf("\n");
      if (t >= end) break;
    }
  }
  return 0;
}

// a catalogue (addon\NN.cat/.dat) holding the given files: "<path inside>=<file>"
int CmdPack(const std::vector<std::string>& args) {
  if (args.size() < 2) return Usage();
  const bool force = std::find(args.begin(), args.end(), "--force") != args.end();
  {
    const std::filesystem::path cat = Path(args[0]);
    const std::filesystem::path dat = cat.parent_path() / (Utf8(cat.stem()) + ".dat");
    if (!force && (std::filesystem::exists(cat) || std::filesystem::exists(dat)))
      throw std::runtime_error(args[0] + " (or its .dat) exists: pick the next free number, or pass --force to replace it");
  }
  std::vector<std::pair<std::string, std::vector<uint8_t>>> members;
  for (size_t i = 1; i < args.size(); i++) {
    if (args[i] == "--force") continue;
    const size_t eq = args[i].find('=');
    if (eq == std::string::npos) throw std::runtime_error("expected <path inside>=<file>: " + args[i]);
    std::string inner = args[i].substr(0, eq);
    std::replace(inner.begin(), inner.end(), '\\', '/');
    members.push_back({inner, ReadFile(args[i].substr(eq + 1))});
    printf("  %s (%zu bytes)\n", inner.c_str(), members.back().second.size());
  }
  WriteCatalog(args[0], members);
  printf("%s: %zu members\n", args[0].c_str(), members.size());
  return 0;
}

// every body of the game: BOB1 -> FBX -> BOB1, compare bytes
int CmdRoundtripFbx(const std::string& dir, const std::string& filter) {
  Game game(dir);
  const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "x3model_rt.fbx";
  std::map<std::string, int> count;
  int shown = 0;
  for (auto& [key, idx] : game.winners) {
    const CatEntry& e = game.entries[idx];
    const bool text = EndsWith(e.path, ".pbd") || EndsWith(e.path, ".bod");
    if (!EndsWith(e.path, ".pbb") && !EndsWith(e.path, ".bob") && !text) continue;
    if (!filter.empty() && key.find(Game::Key(filter)) == std::string::npos) continue;
    std::vector<uint8_t> data;
    try {
      data = game.Read(e);
    } catch (...) {
      continue;
    }
    if (text ? !textbody::IsTextBody(data) : !bob::IsBob(data)) continue;
    try {
      const bob::Body b = ReadAnyBody(data);
      if (text) data = bob::Write(b);  // a text body compares as its binary form
      ExportOptions opt;
      const fbx::Scene sc = BodyToFbx(b, Stem(e.path), opt, nullptr);
      std::string err;
      if (!fbx::Write(Utf8(tmp), sc, err)) throw std::runtime_error(err);
      ImportReport rep;
      const bob::Body back = FbxToBody(Utf8(tmp), rep);
      const auto bytes = bob::Write(back);
      if (bytes == data) {
        count["identical"]++;
      } else {
        count[rep.exact ? "DIFFERENT (stored path)" : "DIFFERENT (derived path)"]++;
        if (shown++ < 15) {
          size_t at = 0;
          while (at < bytes.size() && at < data.size() && bytes[at] == data[at]) at++;
          printf("%s: differs at byte %zu of %zu/%zu\n", e.path.c_str(), at, data.size(), bytes.size());
        }
      }
    } catch (const std::exception& x) {
      count["error"]++;
      if (shown++ < 15) printf("%s: %s\n", e.path.c_str(), x.what());
    }
  }
  for (auto& [k, v] : count) printf("%-28s %d\n", k.c_str(), v);
  return count.size() == 1 && count.count("identical") ? 0 : 1;
}

// how close DeriveTangents / DeriveBounds come to the shipped data
int CmdDeriveCheck(const std::string& dir, const std::string& filter) {
  Game game(dir);
  long long parts = 0, boundsEqual = 0, recs = 0, recsSameSet = 0, groupsSameOrder = 0, groups = 0;
  double tErr = 0, bErr = 0;
  long long tCount = 0;
  long long boundsField[10] = {};
  for (auto& [key, idx] : game.winners) {
    const CatEntry& e = game.entries[idx];
    if (!EndsWith(e.path, ".pbb") && !EndsWith(e.path, ".bob")) continue;
    if (!filter.empty() && key.find(Game::Key(filter)) == std::string::npos) continue;
    std::vector<uint8_t> data;
    try {
      data = game.Read(e);
    } catch (...) {
      continue;
    }
    if (!bob::IsBob(data)) continue;
    bob::Body b;
    try {
      b = bob::Read(data);
    } catch (...) {
      continue;
    }
    const bob::Section* bs = b.Find(bob::SectionKind::Body);
    if (!bs) continue;
    for (auto& lod : bs->lods)
      for (auto& part : lod.parts) {
        if (!(part.flags & bob::kPartHasExtra)) continue;
        parts++;
        bob::Part d = part;
        DeriveBounds(lod, d);
        DeriveTangents(lod, d);
        if (getenv("X3_DERIVE_DUMP") && parts == 1) {
          printf("%s\n  bounds stored :", e.path.c_str());
          for (int32_t v : part.bounds) printf(" %d", v);
          printf("\n  bounds derived:");
          for (int32_t v : d.bounds) printf(" %d", v);
          printf("\n");
          const auto& a = part.groups[0].tangents;
          std::map<int32_t, const bob::TangentRecord*> mine;
          for (auto& r : d.groups[0].tangents) mine[r.point] = &r;
          for (size_t i = 0; i < a.size() && i < 8; i++) {
            const bob::Point& p = lod.points[a[i].point];
            printf("  rec %zu point %d n(%d %d %d) uv(%d %d)\n    stored  T(%d %d %d) B(%d %d %d)\n", i, a[i].point,
                   p.normal[0], p.normal[1], p.normal[2], p.uv[0], p.uv[1], a[i].vec[0], a[i].vec[1], a[i].vec[2],
                   a[i].vec[3], a[i].vec[4], a[i].vec[5]);
            if (auto* m = mine[a[i].point])
              printf("    derived T(%d %d %d) B(%d %d %d)\n", m->vec[0], m->vec[1], m->vec[2], m->vec[3], m->vec[4], m->vec[5]);
          }
          for (size_t fi = 0; fi < 3 && fi < part.groups[0].faces.size(); fi++) {
            const auto& f = part.groups[0].faces[fi];
            printf("  face %zu:", fi);
            for (int32_t v : f.v) {
              const bob::Point& p = lod.points[v];
              printf(" [%d pos %d %d %d uv %d %d]", v, p.pos[0], p.pos[1], p.pos[2], p.uv[0], p.uv[1]);
            }
            printf("\n");
          }
          printf("  stored order:");
          for (size_t i = 0; i < a.size() && i < 30; i++) printf(" %d", a[i].point);
          printf("\n");
        }
        boundsEqual += d.bounds == part.bounds;
        for (int k = 0; k < 10; k++) boundsField[k] += d.bounds[k] == part.bounds[k];
        for (size_t gi = 0; gi < part.groups.size(); gi++) {
          const auto& a = part.groups[gi].tangents;
          const auto& c = d.groups[gi].tangents;
          groups++;
          bool sameOrder = a.size() == c.size();
          for (size_t i = 0; sameOrder && i < a.size(); i++) sameOrder = a[i].point == c[i].point;
          groupsSameOrder += sameOrder;
          std::map<int32_t, const bob::TangentRecord*> byPoint;
          for (auto& r : c) byPoint[r.point] = &r;
          for (auto& r : a) {
            recs++;
            auto it = byPoint.find(r.point);
            if (it == byPoint.end()) continue;
            recsSameSet++;
            double dt = 0, db = 0;
            for (int k = 0; k < 3; k++) {
              dt += (r.vec[k] - it->second->vec[k]) / 65536.0 * (r.vec[k] - it->second->vec[k]) / 65536.0;
              db += (r.vec[3 + k] - it->second->vec[3 + k]) / 65536.0 * (r.vec[3 + k] - it->second->vec[3 + k]) / 65536.0;
            }
            tErr += std::sqrt(dt), bErr += std::sqrt(db), tCount++;
          }
        }
      }
  }
  printf("parts with stored extra data: %lld; bounds identical: %lld\n  per field equal:", parts, boundsEqual);
  for (long long v : boundsField) printf(" %lld", v);
  printf("\ngroups: %lld, record point order identical: %lld\n", groups, groupsSameOrder);
  printf("tangent records: %lld, same point present: %lld, mean |dT| %.4f, mean |dB| %.4f\n", recs, recsSameSet,
         tCount ? tErr / tCount : 0.0, tCount ? bErr / tCount : 0.0);
  return 0;
}

}  // namespace

// Windows hands main() its arguments in the ANSI code page; every path here is UTF-8
int Main(int argc, const std::vector<std::string>& args) {
  std::vector<const char*> p;
  for (auto& a : args) p.push_back(a.c_str());
  const char* const* argv = p.data();
  if (argc < 2) return Usage();
  const std::string cmd = argv[1];
  try {
    if (cmd == "verify" && argc == 3) return CmdVerify(argv[2]);
    if (cmd == "info" && argc == 3) return CmdInfo(argv[2]);
    if (cmd == "export") return CmdExport(std::vector<std::string>(argv + 2, argv + argc));
    if (cmd == "import" && argc == 4) return CmdImport(argv[2], argv[3]);
    if (cmd == "roundtrip-scenes" && argc >= 3) {
      std::string filter;
      bool perturb = false, bake = false;
      for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--perturb") == 0) perturb = true;
        else if (strcmp(argv[i], "--bake") == 0) bake = true;
        else filter = argv[i];
      }
      return CmdRoundtripScenes(argv[2], filter, perturb, bake);
    }
    if (cmd == "anim-sample" && argc >= 3) return CmdAnimSample(argv[2], argc > 3 ? atoi(argv[3]) : 100);
    if (cmd == "pack") return CmdPack(std::vector<std::string>(argv + 2, argv + argc));
    if (cmd == "roundtrip-fbx" && argc >= 3) return CmdRoundtripFbx(argv[2], argc > 3 ? argv[3] : "");
    if (cmd == "derive-check" && argc >= 3) return CmdDeriveCheck(argv[2], argc > 3 ? argv[3] : "");
  } catch (const std::exception& x) {
    fprintf(stderr, "error: %s\n", x.what());
    return 1;
  }
  return Usage();
}

int wmain(int argc, wchar_t** wargv) {
  SetConsoleOutputCP(CP_UTF8);
  std::vector<std::string> args;
  for (int i = 0; i < argc; i++) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), n, nullptr, nullptr);
    args.push_back(std::move(s));
  }
  return Main(argc, args);
}
