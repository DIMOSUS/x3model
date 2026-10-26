// x3model: X3 Terran Conflict / Albion Prelude bodies and scenes <-> FBX.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "archive.hpp"
#include "bob.hpp"
#include "bytes.hpp"

using namespace x3;

namespace {

int Usage() {
  fprintf(stderr,
          "usage:\n"
          "  x3model verify <game dir>      read and re-write every body of every catalogue, compare bytes\n");
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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return Usage();
  const std::string cmd = argv[1];
  try {
    if (cmd == "verify" && argc == 3) return CmdVerify(argv[2]);
  } catch (const std::exception& x) {
    fprintf(stderr, "error: %s\n", x.what());
    return 1;
  }
  return Usage();
}
