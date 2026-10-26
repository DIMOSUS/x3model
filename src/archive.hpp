#pragma once
// The game's catalogues (.cat index + .dat data) and packed files.
//  .cat: every byte XOR (0xDB + i); text, first line the .dat name, then "path size" lines, members back to back.
//  .dat: every byte XOR 0x33.
//  packed members (.pck, .pbb, .pbd, ...): gzip, either plain or with every byte XOR key, key = first byte ^ 0xC8
//  (Archive::ResourceRead in X3AP.exe).
// Mount order as X3AP.exe: NN.cat from 01 up to the first missing number, then addon\NN.cat the same way; a later
// catalogue wins, and a loose file in the game folder wins over every catalogue.
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace x3 {

struct CatEntry {
  std::string path;  // as written in the catalogue (forward slashes)
  uint64_t offset = 0, size = 0;
  int mount = 0;     // index into Game::mounts
};

class Game {
 public:
  explicit Game(const std::string& dir);
  struct Mount {
    std::string cat, dat;  // full paths
    std::string label;     // "01.cat", "addon/04.cat"
  };
  std::vector<Mount> mounts;
  // every member of every catalogue, in mount order
  std::vector<CatEntry> entries;
  // lower-cased, forward-slash path -> index into entries of the winning (highest) mount
  std::map<std::string, size_t> winners;

  const std::string& dir() const { return dir_; }
  // raw member bytes (the .dat XOR removed, still packed if the member is packed)
  std::vector<uint8_t> ReadRaw(const CatEntry& e) const;
  // member bytes unpacked (gzip / XOR-keyed gzip removed when present)
  std::vector<uint8_t> Read(const CatEntry& e) const;
  // the winning member for a path (loose file first); false if none
  bool Find(const std::string& path, std::vector<uint8_t>& out, std::string* where = nullptr) const;
  static std::string Key(const std::string& path);

 private:
  std::string dir_;
};

// Remove a gzip / XOR-keyed gzip wrapper if present; otherwise return the input unchanged.
std::vector<uint8_t> Unpack(const std::vector<uint8_t>& data);
// Wrap in plain gzip (stored deflate blocks; the game inflates any valid gzip stream).
std::vector<uint8_t> PackGzip(const std::vector<uint8_t>& data);
// Write a catalogue holding the given members (path inside, bytes as they should be read back).
void WriteCatalog(const std::string& catPath, const std::vector<std::pair<std::string, std::vector<uint8_t>>>& members);

// UTF-8 std::string <-> std::filesystem::path (C++20 made u8string a char8_t string)
inline std::filesystem::path Path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
inline std::string Utf8(const std::filesystem::path& p) {
  const std::u8string u = p.u8string();
  return std::string(u.begin(), u.end());
}

std::vector<uint8_t> ReadFile(const std::string& path);
void WriteFile(const std::string& path, const std::vector<uint8_t>& data);

}  // namespace x3
