#pragma once
// The engine's number parsers for text bodies and text scenes, copied exactly: TextParseInt 0x004eb930 and
// TextParseFixed16 0x004ec490 (Body::Text*). Both read up to and including ';', skip whitespace and '/'-comments
// inside the value, and set the error flag where the engine does. Self provides C(k) (the byte k ahead of the
// cursor, 0 at the end), Comment() (skip to the end of the line), and the members p (cursor) and err.
#include <cstdint>

namespace x3 {

template <class Self>
struct EngineNumbers {
  int32_t ParseInt() {
    Self& s = static_cast<Self&>(*this);
    int base = 0;
    uint32_t val = 0;
    bool neg = false;
    for (;;) {
      const int c = s.C();
      if (c == 0 || c == ';') {
        if (!base) return s.err = true, 0;
        if (c == ';') s.p++;
        return static_cast<int32_t>(neg ? 0u - val : val);
      }
      if (c == 9 || c == 10 || c == 13 || c == 32) { s.p++; continue; }
      if (c == '#') {
        if (base) return s.err = true, 0;
        while (s.C() != ';' && s.C() != 0) s.p++;
        if (s.C() == ';') s.p++;
        return 0;
      }
      if (c == '%') {
        if (base) return s.err = true, 0;
        base = 2, s.p++;
        continue;
      }
      if (c == '-') {
        if (base || neg) return s.err = true, 0;
        neg = true, s.p++;
        continue;
      }
      if (c == '/') { s.Comment(); continue; }
      if (c == '0' && base == 0 && (s.C(1) == 'x' || s.C(1) == 'X')) {
        base = 16, s.p += 2;
        continue;
      }
      if (base == 2) {
        if (neg || (c != '0' && c != '1')) return s.err = true, 0;
        val = val * 2 + static_cast<uint32_t>(c - '0');
      } else if (base == 16) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return s.err = true, 0;
        if (neg) return s.err = true, 0;
        val = val * 16 + static_cast<uint32_t>(d);
      } else {
        if (c < '0' || c > '9') return s.err = true, 0;
        base = 10;
        val = val * 10 + static_cast<uint32_t>(c - '0');
      }
      s.p++;
    }
  }

  int32_t ParseFixed() {
    Self& s = static_cast<Self&>(*this);
    double val = 0, scale = 0;
    bool neg = false, started = false;
    for (;;) {
      const int c = s.C();
      if (c == 0 || c == ';') {
        if (!started) return s.err = true, 0;
        if (c == ';') s.p++;
        if (neg) val = -val;
        const double x = val * 65536.0;
        if (!(x > -2147483649.0 && x < 2147483648.0)) return INT32_MIN;
        return static_cast<int32_t>(x);  // _ftol: truncation toward zero
      }
      if (c == 9 || c == 10 || c == 13 || c == 32) { s.p++; continue; }
      if (c == '#') {
        if (started) return s.err = true, 0;
        for (;;) {  // stops at the ';' (not consumed)
          if (s.C() == 0) return 0;
          s.p++;
          if (s.C() == ';') return 0;
        }
      }
      if (c == '-') {
        if (started) return s.err = true, 0;
        neg = true, s.p++;
        continue;
      }
      if (c == '.') {
        if (scale != 0.0) return s.err = true, 0;
        scale = 1.0, s.p++;
        continue;
      }
      if (c == '/') { s.Comment(); continue; }
      if (c < '0' || c > '9') return s.err = true, 0;
      started = true;
      if (scale == 0.0) {
        val = (static_cast<double>(c) + val * 10.0) - 48.0;
      } else {
        scale *= 0.1;
        val = static_cast<double>(c - '0') * scale + val;
      }
      s.p++;
    }
  }
};

}  // namespace x3
