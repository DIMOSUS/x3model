#pragma once
// Small helpers: base64 for the opaque per-element data kept in FBX user properties, number lists in text form.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace x3 {

inline std::string Base64(const std::vector<uint8_t>& d) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  o.reserve((d.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < d.size(); i += 3) {
    const uint32_t v = d[i] << 16 | d[i + 1] << 8 | d[i + 2];
    o += t[v >> 18], o += t[v >> 12 & 63], o += t[v >> 6 & 63], o += t[v & 63];
  }
  if (i + 1 == d.size()) {
    const uint32_t v = d[i] << 16;
    o += t[v >> 18], o += t[v >> 12 & 63], o += "==";
  } else if (i + 2 == d.size()) {
    const uint32_t v = d[i] << 16 | d[i + 1] << 8;
    o += t[v >> 18], o += t[v >> 12 & 63], o += t[v >> 6 & 63], o += '=';
  }
  return o;
}

inline std::vector<uint8_t> Unbase64(const std::string& s) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::vector<uint8_t> o;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : s) {
    const int v = val(c);
    if (v < 0) continue;
    acc = acc << 6 | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      o.push_back(static_cast<uint8_t>(acc >> bits));
    }
  }
  return o;
}

// little-endian packing of int32 arrays for base64 blobs
inline void PutI32(std::vector<uint8_t>& b, int32_t v) {
  for (int s = 0; s < 32; s += 8) b.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> s));
}
inline int32_t GetI32(const std::vector<uint8_t>& b, size_t& at) {
  if (at + 4 > b.size()) return at = b.size(), 0;
  const uint32_t v = b[at] | b[at + 1] << 8 | b[at + 2] << 16 | static_cast<uint32_t>(b[at + 3]) << 24;
  at += 4;
  return static_cast<int32_t>(v);
}

inline std::string JoinInts(const int32_t* v, size_t n) {
  std::string s;
  for (size_t i = 0; i < n; i++) s += (i ? " " : "") + std::to_string(v[i]);
  return s;
}
inline std::vector<long long> SplitInts(const std::string& s) {
  std::vector<long long> o;
  std::istringstream in(s);
  long long v;
  while (in >> v) o.push_back(v);
  return o;
}

// shortest text that reads back to the same double
inline std::string Num(double v) {
  char b[40];
  for (int p = 6; p <= 17; p++) {
    snprintf(b, sizeof b, "%.*g", p, v);
    if (strtod(b, nullptr) == v) break;
  }
  return b;
}

}  // namespace x3
