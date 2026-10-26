#pragma once
// Big-endian stream primitives of the game's body/scene files (Body::Read* in X3AP.exe: every binary value is
// big-endian; strings are NUL-terminated; sections open with a 4-byte tag and close with "/" + 3 letters).
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace x3 {

struct FormatError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class BeReader {
 public:
  BeReader(const uint8_t* data, size_t size) : p_(data), n_(size) {}
  explicit BeReader(const std::vector<uint8_t>& v) : p_(v.data()), n_(v.size()) {}

  size_t pos() const { return pos_; }
  size_t size() const { return n_; }
  bool atEnd() const { return pos_ >= n_; }
  const uint8_t* data() const { return p_; }

  uint8_t U8() {
    Need(1);
    return p_[pos_++];
  }
  uint16_t U16() {
    Need(2);
    uint16_t v = static_cast<uint16_t>(p_[pos_] << 8 | p_[pos_ + 1]);
    pos_ += 2;
    return v;
  }
  uint32_t U32() {
    Need(4);
    uint32_t v = static_cast<uint32_t>(p_[pos_]) << 24 | static_cast<uint32_t>(p_[pos_ + 1]) << 16 |
                 static_cast<uint32_t>(p_[pos_ + 2]) << 8 | p_[pos_ + 3];
    pos_ += 4;
    return v;
  }
  int32_t I32() { return static_cast<int32_t>(U32()); }
  std::string Str() {
    const size_t start = pos_;
    while (pos_ < n_ && p_[pos_]) pos_++;
    if (pos_ >= n_) throw FormatError("unterminated string at " + std::to_string(start));
    std::string s(reinterpret_cast<const char*>(p_ + start), pos_ - start);
    pos_++;
    return s;
  }
  // the next 4 bytes as a tag, without consuming them
  std::string PeekTag() const {
    if (pos_ + 4 > n_) return {};
    return std::string(reinterpret_cast<const char*>(p_ + pos_), 4);
  }
  std::string Tag() {
    Need(4);
    std::string t(reinterpret_cast<const char*>(p_ + pos_), 4);
    pos_ += 4;
    return t;
  }
  void Expect(const char* tag) {
    const size_t at = pos_;
    std::string t = Tag();
    if (t != tag) throw FormatError("expected '" + std::string(tag) + "' at " + std::to_string(at) + ", got '" + t + "'");
  }

 private:
  void Need(size_t k) const {
    if (pos_ + k > n_) throw FormatError("unexpected end of data at " + std::to_string(pos_));
  }
  const uint8_t* p_;
  size_t n_;
  size_t pos_ = 0;
};

class BeWriter {
 public:
  std::vector<uint8_t> b;
  void U8(uint8_t v) { b.push_back(v); }
  void U16(uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
  }
  void U32(uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(v >> s));
  }
  void I32(int32_t v) { U32(static_cast<uint32_t>(v)); }
  void Str(const std::string& s) {
    b.insert(b.end(), s.begin(), s.end());
    b.push_back(0);
  }
  void Tag(const char* t) { b.insert(b.end(), t, t + 4); }
};

}  // namespace x3
