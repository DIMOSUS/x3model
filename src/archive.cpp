#include "archive.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <map>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "bytes.hpp"
#include "ufbx.h"

namespace fs = std::filesystem;

namespace x3 {

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream f(Path(path), std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

void WriteFile(const std::string& path, const std::vector<uint8_t>& data) {
  const fs::path p = Path(path);
  if (p.has_parent_path()) fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  if (!f) throw std::runtime_error("cannot write " + path);
  f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  f.close();
  if (!f) throw std::runtime_error("write failed: " + path);
}

std::string Game::Key(const std::string& path) {
  std::string k = path;
  for (char& c : k) {
    if (c == '\\') c = '/';
    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  }
  return k;
}

static std::vector<std::pair<std::string, uint64_t>> ReadIndex(const std::string& cat) {
  std::vector<uint8_t> b = ReadFile(cat);
  for (size_t i = 0; i < b.size(); i++) b[i] ^= static_cast<uint8_t>(0xDB + i);
  std::vector<std::pair<std::string, uint64_t>> out;
  std::string text(b.begin(), b.end());
  size_t pos = 0;
  bool first = true;
  while (pos < text.size()) {
    size_t e = text.find('\n', pos);
    if (e == std::string::npos) e = text.size();
    std::string line = text.substr(pos, e - pos);
    pos = e + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    if (first) {  // the .dat name
      first = false;
      continue;
    }
    const size_t sp = line.rfind(' ');
    if (sp == std::string::npos) throw FormatError("bad catalogue line in " + cat + ": " + line);
    std::string path = line.substr(0, sp);
    std::replace(path.begin(), path.end(), '\\', '/');
    const std::string num = line.substr(sp + 1);
    uint64_t size = 0;
    const auto [end, ec] = std::from_chars(num.data(), num.data() + num.size(), size);
    if (ec != std::errc() || end != num.data() + num.size() || num.empty())
      throw FormatError("bad member size in " + cat + ": " + line);
    out.emplace_back(path, size);
  }
  return out;
}

Game::Game(const std::string& dir) : dir_(dir) {
  auto mountSeries = [&](const std::string& sub) {
    for (int i = 1; i < 100; i++) {
      char name[16];
      snprintf(name, sizeof name, "%02d.cat", i);
      fs::path cat = Path(dir) / Path(sub) / name;
      if (!fs::exists(cat)) break;
      Mount m;
      m.cat = Utf8(cat);
      m.dat = Utf8(cat.parent_path() / (Utf8(cat.stem()) + ".dat"));
      m.label = sub.empty() ? name : sub + "/" + name;
      mounts.push_back(m);
    }
  };
  mountSeries("");
  mountSeries("addon");
  for (size_t mi = 0; mi < mounts.size(); mi++) {
    uint64_t off = 0;
    for (auto& [path, size] : ReadIndex(mounts[mi].cat)) {
      CatEntry e;
      e.path = path;
      e.offset = off;
      e.size = size;
      e.mount = static_cast<int>(mi);
      off += size;
      winners[Key(path)] = entries.size();
      entries.push_back(e);
    }
  }
}

std::vector<uint8_t> Game::ReadRaw(const CatEntry& e) const {
  std::ifstream f(Path(mounts[e.mount].dat), std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + mounts[e.mount].dat);
  f.seekg(static_cast<std::streamoff>(e.offset));
  std::vector<uint8_t> b(e.size);
  f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(e.size));
  if (!f || static_cast<uint64_t>(f.gcount()) != e.size)
    throw FormatError(mounts[e.mount].dat + ": member " + e.path + " is truncated");
  for (auto& c : b) c ^= 0x33;
  return b;
}

std::vector<uint8_t> Game::Read(const CatEntry& e) const { return Unpack(ReadRaw(e)); }

bool Game::Find(const std::string& path, std::vector<uint8_t>& out, std::string* where) const {
  const fs::path loose = Path(dir_) / Path(path);
  if (fs::is_regular_file(loose)) {
    out = Unpack(ReadFile(Utf8(loose)));
    if (where) *where = "loose";
    return true;
  }
  auto it = winners.find(Key(path));
  if (it == winners.end()) return false;
  const CatEntry& e = entries[it->second];
  out = Read(e);
  if (where) *where = mounts[e.mount].label;
  return true;
}

// ---------------------------------------------------------------- gzip

static uint32_t Crc32(const std::vector<uint8_t>& d);

static std::vector<uint8_t> Gunzip(const uint8_t* p, size_t n) {
  if (n < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) throw FormatError("not a gzip stream");
  const uint8_t flg = p[3];
  size_t pos = 10;
  if (flg & 4) {  // FEXTRA
    const size_t xlen = p[pos] | p[pos + 1] << 8;
    pos += 2 + xlen;
  }
  if (flg & 8)
    while (pos < n && p[pos++]) {
    }
  if (flg & 16)
    while (pos < n && p[pos++]) {
    }
  if (flg & 2) pos += 2;
  if (pos + 8 > n) throw FormatError("truncated gzip stream");
  const uint32_t isize = p[n - 4] | p[n - 3] << 8 | p[n - 2] << 16 | static_cast<uint32_t>(p[n - 1]) << 24;
  // deflate expands at most ~1032:1; a larger ISIZE is a corrupt trailer, not a reason to allocate gigabytes
  if (isize > (n - 8 - pos) * 1032 + 64) throw FormatError("gzip size field is impossible for the stream");
  std::vector<uint8_t> out(isize);
  ufbx_inflate_input in{};
  in.total_size = n - 8 - pos;
  in.data = p + pos;
  in.data_size = in.total_size;
  in.no_header = true;
  in.no_checksum = true;
  ufbx_inflate_retain retain;
  retain.initialized = false;
  const ptrdiff_t got = ufbx_inflate(out.data(), out.size(), &in, &retain);
  if (got < 0 || static_cast<size_t>(got) != isize) throw FormatError("gzip inflate failed");
  const uint32_t crc = p[n - 8] | p[n - 7] << 8 | p[n - 6] << 16 | static_cast<uint32_t>(p[n - 5]) << 24;
  if (Crc32(out) != crc) throw FormatError("gzip CRC mismatch");
  return out;
}

std::vector<uint8_t> Unpack(const std::vector<uint8_t>& d) {
  if (d.size() >= 3 && d[0] == 0x1f && d[1] == 0x8b) return Gunzip(d.data(), d.size());
  if (d.size() >= 4) {
    const uint8_t key = d[0] ^ 0xC8;
    if ((d[1] ^ key) == 0x1f && (d[2] ^ key) == 0x8b) {
      std::vector<uint8_t> t(d.begin() + 1, d.end());
      for (auto& c : t) c ^= key;
      return Gunzip(t.data(), t.size());
    }
  }
  return d;
}

static uint32_t Crc32(const std::vector<uint8_t>& d) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (uint8_t b : d) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

std::vector<uint8_t> PackGzip(const std::vector<uint8_t>& d) {
  std::vector<uint8_t> o = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
  size_t pos = 0;
  do {
    const size_t len = std::min<size_t>(65535, d.size() - pos);
    const bool last = pos + len == d.size();
    o.push_back(last ? 1 : 0);
    o.push_back(static_cast<uint8_t>(len));
    o.push_back(static_cast<uint8_t>(len >> 8));
    o.push_back(static_cast<uint8_t>(~len));
    o.push_back(static_cast<uint8_t>(~len >> 8));
    o.insert(o.end(), d.begin() + pos, d.begin() + pos + len);
    pos += len;
  } while (pos < d.size());
  const uint32_t crc = Crc32(d), sz = static_cast<uint32_t>(d.size());
  for (int s = 0; s < 32; s += 8) o.push_back(static_cast<uint8_t>(crc >> s));
  for (int s = 0; s < 32; s += 8) o.push_back(static_cast<uint8_t>(sz >> s));
  return o;
}

void WriteCatalog(const std::string& catPath, const std::vector<std::pair<std::string, std::vector<uint8_t>>>& members) {
  const fs::path cat = Path(catPath);
  const fs::path dat = cat.parent_path() / (Utf8(cat.stem()) + ".dat");
  std::string index = Utf8(dat.filename()) + "\n";
  std::vector<uint8_t> data;
  std::map<std::string, int> seen;
  for (auto& [path, bytes] : members) {
    if (path.empty() || path.find_first_of("\r\n") != std::string::npos)
      throw std::runtime_error("bad member path '" + path + "'");
    if (seen[Game::Key(path)]++) throw std::runtime_error("member " + path + " given twice");
    index += path + " " + std::to_string(bytes.size()) + "\n";
    for (uint8_t c : bytes) data.push_back(c ^ 0x33);
  }
  // the game keeps member offsets and sizes as int
  if (data.size() >= 0x80000000u) throw std::runtime_error("catalogue data reaches 2 GB");
  std::vector<uint8_t> ib(index.begin(), index.end());
  for (size_t i = 0; i < ib.size(); i++) ib[i] ^= static_cast<uint8_t>(0xDB + i);
  // write both under temporary names, then put the .dat in place before the .cat that indexes it
  const fs::path catTmp = Path(Utf8(cat) + ".tmp"), datTmp = Path(Utf8(dat) + ".tmp");
  WriteFile(Utf8(datTmp), data);
  WriteFile(Utf8(catTmp), ib);
  fs::rename(datTmp, dat);
  fs::rename(catTmp, cat);
}

}  // namespace x3
