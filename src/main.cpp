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

#include "archive.hpp"
#include "bob.hpp"
#include "body_fbx.hpp"
#include "bytes.hpp"
#include "derive.hpp"
#include "fbx_write.hpp"

using namespace x3;

namespace {

int Usage() {
  fprintf(stderr,
          "usage:\n"
          "  x3model verify <game dir>      read and re-write every body of every catalogue, compare bytes\n"
          "  x3model info <body>            print a body's sections, LODs and materials\n"
          "  x3model export <body> <out.fbx> [--no-textures] [--unit-mm N]\n"
          "                                 body -> FBX (textures copied next to it when the body comes from the game)\n"
          "  x3model import <in.fbx> <out.bob|out.pbb>   FBX -> body\n"
          "  x3model roundtrip-fbx <game dir> [filter]   every body -> FBX -> body, compare bytes\n"
          "  x3model derive-check <game dir> [filter]    recomputed tangents/bounds against the shipped ones\n"
          "\n"
          "<body> is a file (.bob/.pbb) or \"<game dir>|<path in the catalogues>\", e.g.\n"
          "  \"D:\\GAMES\\X3 Terran Conflict|objects/ships/argon/argon_M3.pbb\"\n");
  return 2;
}

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
    if (data.size() >= 4 && memcmp(data.data(), "CUT1", 4) == 0) {
      count["CUT1 (scene, skipped)"]++;
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
  for (auto& [k, v] : count) printf("%-26s %d\n", k.c_str(), v);
  return count.count("BOB1 DIFFERENT") ? 1 : 0;
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
  const bob::Body b = bob::Read(data);
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
  double unitMm = 1;
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] == "--no-textures") texDir.clear();
    else if (args[i] == "--unit-mm" && i + 1 < args.size()) unitMm = atof(args[++i].c_str());
    else if (in.empty()) in = args[i];
    else out = args[i];
  }
  if (in.empty() || out.empty()) return Usage();
  std::unique_ptr<Game> game;
  const size_t bar = in.find('|');
  if (bar != std::string::npos) game = std::make_unique<Game>(in.substr(0, bar));
  std::string label;
  const auto data = LoadInput(in, &label);
  const bob::Body body = bob::Read(data);
  ExportOptions opt;
  opt.unitCm = unitMm / 10.0;
  opt.textureDir = game ? texDir : "";
  std::vector<std::pair<std::string, std::string>> textures;
  fbx::Scene sc = BodyToFbx(body, Stem(out), opt, &textures);
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

// every body of the game: BOB1 -> FBX -> BOB1, compare bytes
int CmdRoundtripFbx(const std::string& dir, const std::string& filter) {
  Game game(dir);
  const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "x3model_rt.fbx";
  std::map<std::string, int> count;
  int shown = 0;
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
    try {
      const bob::Body b = bob::Read(data);
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

int main(int argc, char** argv) {
  if (argc < 2) return Usage();
  const std::string cmd = argv[1];
  try {
    if (cmd == "verify" && argc == 3) return CmdVerify(argv[2]);
    if (cmd == "info" && argc == 3) return CmdInfo(argv[2]);
    if (cmd == "export") return CmdExport(std::vector<std::string>(argv + 2, argv + argc));
    if (cmd == "import" && argc == 4) return CmdImport(argv[2], argv[3]);
    if (cmd == "roundtrip-fbx" && argc >= 3) return CmdRoundtripFbx(argv[2], argc > 3 ? argv[3] : "");
    if (cmd == "derive-check" && argc >= 3) return CmdDeriveCheck(argv[2], argc > 3 ? argv[3] : "");
  } catch (const std::exception& x) {
    fprintf(stderr, "error: %s\n", x.what());
    return 1;
  }
  return Usage();
}
