#include "scene.hpp"

#include <cmath>
#include <cstring>

#include "bob.hpp"
#include "bytes.hpp"
#include "textbody.hpp"

namespace x3::scene {

std::vector<Field> KeyLayout(uint32_t f) {
  static const char* tcbP[] = {"p_tension", "p_continuity", "p_bias", "p_ease_in", "p_ease_out"};
  static const char* tcbR[] = {"r_tension", "r_continuity", "r_bias", "r_ease_in", "r_ease_out"};
  std::vector<Field> o;
  if (!(f & 0x10)) {  // position
    o.push_back({"x", kInt}), o.push_back({"y", kInt}), o.push_back({"z", kInt});
    if (f & 0x4000)
      for (auto n : tcbP) o.push_back({n, kFixed});
  }
  if (!(f & 8)) {  // object rotation
    if (f & 0x20) {
      // no rotation values (with 0x2: an identity key)
    } else if (f & 2) {  // axis-angle: angle in turns, axis stored negated
      o.push_back({"angle", kFixed}), o.push_back({"ax", kFixed}), o.push_back({"ay", kFixed}), o.push_back({"az", kFixed});
      if (f & 0x8000)
        for (auto n : tcbR) o.push_back({n, kFixed});
    } else if (!(f & 0x200)) {  // three angles (turns)
      o.push_back({"yaw", kFixed}), o.push_back({"pitch", kFixed}), o.push_back({"roll3", kFixed});
    }
  } else {  // target mode (camera)
    if (!(f & 0x40)) o.push_back({"tx", kInt}), o.push_back({"ty", kInt}), o.push_back({"tz", kInt});
    if (!(f & 0x20)) o.push_back({"roll", kFixed});
  }
  if ((f & 0x200) && !(f & 0x400)) o.push_back({"r", kFixed}), o.push_back({"g", kFixed}), o.push_back({"b", kFixed});
  if ((f & 0x800) && !(f & 0x1000)) o.push_back({"fov", kFixed});
  o.push_back({"dt", kInt}), o.push_back({"extra", kInt});
  return o;
}

int32_t Key::Get(const char* name) const {
  const auto l = KeyLayout(flags);
  for (size_t i = 0; i < l.size() && i < values.size(); i++)
    if (strcmp(l[i].name, name) == 0) return values[i];
  return 0;
}
bool Key::Has(const char* name) const {
  for (auto& f : KeyLayout(flags))
    if (strcmp(f.name, name) == 0) return true;
  return false;
}

std::vector<int64_t> KeyTimes(const Part& p) {
  std::vector<int64_t> t;
  int64_t now = 0;
  for (auto& k : p.keys) {
    t.push_back(now);
    now += k.Get("dt");
  }
  return t;
}

bool IsCut1(const std::vector<uint8_t>& d) { return d.size() >= 4 && memcmp(d.data(), "CUT1", 4) == 0; }

// ---------------------------------------------------------------- CUT1

Scene ReadCut1(const std::vector<uint8_t>& data) {
  BeReader r(data);
  Scene sc;
  sc.binary = true;
  r.Expect("CUT1");
  if (r.PeekTag() == "INFO") {
    r.Expect("INFO");
    sc.info = r.Str();
    r.Expect("/INF");
  }
  sc.version = r.I32();
  const int32_t n = r.I32();
  for (int32_t i = 0; i < n; i++) {
    r.Expect("PATH");
    Part p;
    p.index = r.U32();
    p.body = sc.version < 6 ? std::to_string(r.I32()) : r.Str();
    p.mode = r.I32();
    p.parent = r.I32();
    p.flags = r.U32();
    if (r.PeekTag() == "NAME") {
      r.Expect("NAME");
      p.name = r.Str();
      r.Expect("/NAM");
    }
    if (r.PeekTag() == "CONS") {
      r.Expect("CONS");
      std::vector<std::pair<int32_t, int32_t>> c;
      const int32_t m = r.I32();
      for (int32_t k = 0; k < m; k++) {
        const int32_t a = r.I32();
        c.push_back({a, r.I32()});
      }
      p.constraints = c;
      r.Expect("/CON");
    }
    if (r.PeekTag() == "BOB1") {
      // the engine parses the body, then scans byte by byte for the first "/BOB" after the tag
      const size_t start = r.pos();
      const char* s = reinterpret_cast<const char*>(data.data());
      size_t e = std::string::npos;
      for (size_t k = start + 4; k + 4 <= data.size(); k++)
        if (memcmp(s + k, "/BOB", 4) == 0) {
          e = k;
          break;
        }
      if (e == std::string::npos) throw FormatError("embedded body without /BOB");
      p.embedded.assign(data.begin() + start, data.begin() + e + 4);
      r.Seek(e + 4);
    }
    const int32_t nn = r.I32();
    if (nn) {
      r.Expect("NOTE");
      for (int32_t k = 0; k < nn; k++) {
        const int32_t t = r.I32();
        p.notes.push_back({t, r.Str()});
      }
      r.Expect("/NOT");
    }
    const int32_t nk = r.I32();
    if (nk) {
      r.Expect("STAT");
      for (int32_t k = 0; k < nk; k++) {
        Key key;
        key.flags = r.U32();
        for (size_t f = 0; f < KeyLayout(key.flags).size(); f++) key.values.push_back(r.I32());
        p.keys.push_back(std::move(key));
      }
      r.Expect("/STA");
    }
    r.Expect("/PAT");
    sc.parts.push_back(std::move(p));
  }
  r.Expect("/CUT");
  sc.tail.assign(data.begin() + r.pos(), data.end());
  return sc;
}

std::vector<uint8_t> WriteCut1(const Scene& sc) {
  BeWriter w;
  const int32_t v = sc.binary ? sc.version : 6;
  w.Tag("CUT1");
  // INFO is required: Scene::LoadBinaryCut reads it first and fails without it (text scenes have none)
  w.Tag("INFO"), w.Str(sc.info ? *sc.info : std::string("x3model")), w.Tag("/INF");
  w.I32(v);
  w.I32(static_cast<int32_t>(sc.parts.size()));
  for (auto& p : sc.parts) {
    w.Tag("PATH");
    w.U32(p.index);
    if (v < 6)
      w.I32(static_cast<int32_t>(std::strtol(p.body.c_str(), nullptr, 10)));
    else
      w.Str(p.body);
    w.I32(p.mode), w.I32(p.parent), w.U32(p.flags);
    if (p.name) w.Tag("NAME"), w.Str(*p.name), w.Tag("/NAM");
    if (p.constraints) {
      w.Tag("CONS");
      w.I32(static_cast<int32_t>(p.constraints->size()));
      for (auto& [a, b] : *p.constraints) w.I32(a), w.I32(b);
      w.Tag("/CON");
    }
    if (!p.embedded.empty()) {
      w.b.insert(w.b.end(), p.embedded.begin(), p.embedded.end());
    } else if (!p.inlineText.empty()) {  // text 'L' body -> embedded BOB1
      const std::vector<uint8_t> text(p.inlineText.begin(), p.inlineText.end());
      const auto bytes = bob::Write(textbody::Read(text));
      w.b.insert(w.b.end(), bytes.begin(), bytes.end());
    }
    w.I32(static_cast<int32_t>(p.notes.size()));
    if (!p.notes.empty()) {
      w.Tag("NOTE");
      for (auto& [t, s] : p.notes) w.I32(t), w.Str(s);
      w.Tag("/NOT");
    }
    w.I32(static_cast<int32_t>(p.keys.size()));
    if (!p.keys.empty()) {
      w.Tag("STAT");
      for (auto& k : p.keys) {
        w.U32(k.flags);
        for (int32_t x : k.values) w.I32(x);
      }
      w.Tag("/STA");
    }
    w.Tag("/PAT");
  }
  w.Tag("/CUT");
  w.b.insert(w.b.end(), sc.tail.begin(), sc.tail.end());
  return w.b;
}

// ---------------------------------------------------------------- text (Scene::LoadTextCut)

namespace {

// A copy of the engine's text primitives: Body::TextSkipWhitespace 0x004e98a0, TextParseInt 0x004eb930,
// TextParseFixed16 0x004ec490, TextReadToken 0x004e9930, TextReadCharInSet 0x004ea160. NUL ends the buffer.
class Tr {
 public:
  explicit Tr(const std::vector<uint8_t>& d) : d_(d) { d_.push_back(0); }
  bool err = false;
  size_t p = 0;
  int C(size_t k = 0) const { return p + k < d_.size() ? d_[p + k] : 0; }
  void Comment() {
    while (C() != '\n' && C() != 0) p++;
  }
  void SkipWs() {
    for (;;) {
      const int c = C();
      if (c == 9 || c == 10 || c == 13 || c == 32) p++;
      else if (c == '/') Comment();
      else return;
    }
  }
  int32_t ParseInt() {
    int base = 0;
    uint32_t val = 0;
    bool neg = false;
    for (;;) {
      const int c = C();
      if (c == 0 || c == ';') {
        if (!base) return err = true, 0;
        if (c == ';') p++;
        return static_cast<int32_t>(neg ? 0u - val : val);
      }
      if (c == 9 || c == 10 || c == 13 || c == 32) { p++; continue; }
      if (c == '#') {
        if (base) return err = true, 0;
        while (C() != ';' && C() != 0) p++;
        if (C() == ';') p++;
        return 0;
      }
      if (c == '%') {
        if (base) return err = true, 0;
        base = 2, p++;
        continue;
      }
      if (c == '-') {
        if (base || neg) return err = true, 0;
        neg = true, p++;
        continue;
      }
      if (c == '/') { Comment(); continue; }
      if (c == '0' && base == 0 && (C(1) == 'x' || C(1) == 'X')) {
        base = 16, p += 2;
        continue;
      }
      if (base == 2) {
        if (neg || (c != '0' && c != '1')) return err = true, 0;
        val = val * 2 + static_cast<uint32_t>(c - '0');
      } else if (base == 16) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return err = true, 0;
        if (neg) return err = true, 0;
        val = val * 16 + static_cast<uint32_t>(d);
      } else {
        if (c < '0' || c > '9') return err = true, 0;
        base = 10;
        val = val * 10 + static_cast<uint32_t>(c - '0');
      }
      p++;
    }
  }
  int32_t ParseFixed() {
    double val = 0, scale = 0;
    bool neg = false, started = false;
    for (;;) {
      const int c = C();
      if (c == 0 || c == ';') {
        if (!started) return err = true, 0;
        if (c == ';') p++;
        if (neg) val = -val;
        const double x = val * 65536.0;
        if (!(x > -2147483649.0 && x < 2147483648.0)) return INT32_MIN;
        return static_cast<int32_t>(x);  // _ftol: truncation toward zero
      }
      if (c == 9 || c == 10 || c == 13 || c == 32) { p++; continue; }
      if (c == '#') {
        if (started) return err = true, 0;
        for (;;) {  // stops at the ';' (not consumed)
          if (C() == 0) return 0;
          p++;
          if (C() == ';') return 0;
        }
      }
      if (c == '-') {
        if (started) return err = true, 0;
        neg = true, p++;
        continue;
      }
      if (c == '.') {
        if (scale != 0.0) return err = true, 0;
        scale = 1.0, p++;
        continue;
      }
      if (c == '/') { Comment(); continue; }
      if (c < '0' || c > '9') return err = true, 0;
      started = true;
      if (scale == 0.0) {
        val = (static_cast<double>(c) + val * 10.0) - 48.0;
      } else {
        scale *= 0.1;
        val = static_cast<double>(c - '0') * scale + val;
      }
      p++;
    }
  }
  std::string Token() {
    std::string o;
    for (;;) {
      const int c = C();
      if (c == 0 || c == ';') {
        if (c == ';') p++;
        return o;
      }
      if (c == 9 || c == 10 || c == 13) { p++; continue; }
      if (c == '/') { Comment(); continue; }
      o += static_cast<char>(c);
      p++;
    }
  }
  int32_t I32() {
    if (err) return 0;
    SkipWs();
    return ParseInt();
  }
  int32_t Fixed() {
    if (err) return 0;
    SkipWs();
    return ParseFixed();
  }
  std::string String() {
    if (err) return "";
    SkipWs();
    return Token();
  }
  int CharIn(const char* set) {
    if (err) return 0;
    SkipWs();
    const int c = C();
    if (c && strchr(set, c)) p++;
    return c;
  }
  const std::vector<uint8_t>& data() const { return d_; }

 private:
  std::vector<uint8_t> d_;
};

}  // namespace

bool IsTextScene(const std::vector<uint8_t>& d) {
  Tr r(d);
  r.SkipWs();
  return r.C() == 'V' && r.C(1) == 'E' && r.C(2) == 'R' && r.C(3) == ':';
}

Scene ReadText(const std::vector<uint8_t>& data) {
  Tr r(data);
  Scene sc;
  sc.binary = false;
  r.SkipWs();
  if (!(r.C() == 'V' && r.C(1) == 'E' && r.C(2) == 'R' && r.C(3) == ':')) throw FormatError("text scene without VER:");
  r.p += 4;
  sc.version = r.I32();
  if (r.err) throw FormatError("bad VER: value");
  static const char* kHeader = "CFENKMLTclbjfsp";
  int c = r.CharIn("P");
  while (c == 'P' && !r.err) {
    Part p;
    p.index = static_cast<uint32_t>(r.I32());
    if (r.CharIn("B") != 'B') {  // the engine keeps the parts read so far
      r.err = true;
      break;
    }
    p.body = r.String();
    for (;;) {
      c = r.CharIn(kHeader);
      if (!c || r.err || !strchr(kHeader, c) || c == 'E') break;
      switch (c) {
        case 'C': p.mode = r.I32(); break;
        case 'F': p.parent = r.I32(); break;
        case 'K': {
          const int32_t n = r.I32();
          std::vector<std::pair<int32_t, int32_t>> cons;
          for (int32_t i = 0; i < n; i++) {
            const int32_t a = r.I32();
            cons.push_back({a, r.Fixed()});
          }
          p.constraints = cons;
          break;
        }
        case 'L': {
          const auto& d = r.data();
          if (r.CharIn("{") != '{') {  // up to the first '{', which starts the first key
            size_t e = r.p;
            while (e < d.size() && d[e] != '{' && d[e] != 0) e++;
            p.inlineText.assign(d.begin() + r.p, d.begin() + e);
            r.p = e;
          } else {
            size_t e = r.p;
            while (e < d.size() && d[e] != '}' && d[e] != 0) e++;
            if (e >= d.size() || d[e] != '}') throw FormatError("inline body without }");
            p.inlineText.assign(d.begin() + r.p, d.begin() + e);
            r.p = e + 1;
          }
          break;
        }
        case 'M':
          for (int i = 0; i < 9; i++) r.Fixed();  // read and discarded by the engine
          break;
        case 'N': p.name = r.String(); break;
        case 'T': {
          int32_t v = r.I32();
          while (v != -1 && !r.err) {
            if (p.notes.size() > 99) throw FormatError("more than 100 notes");
            std::string s = r.String();
            p.notes.push_back({v, s});
            v = r.I32();
          }
          break;
        }
        case 'b': break;
        case 'c': p.flags |= 1; break;
        case 'f': p.flags |= 0x20; break;
        case 'j': p.flags = (p.flags & ~0x40u) | 0x80; break;
        case 'l':
          p.flags |= 2;
          if (r.CharIn("d") == 'd') p.flags |= 4;
          else if (r.CharIn("p") == 'p') p.flags |= 8;
          else if (r.CharIn("s") == 's') p.flags |= 0x10;
          break;
        case 'p': p.flags |= 0x100; break;
        case 's': p.flags |= 0x200; break;
      }
    }
    if (r.err) throw FormatError("part P " + std::to_string(p.index) + ": header parse error");
    for (;;) {
      c = r.CharIn("{");
      if (c != '{' || r.err) break;
      Key k;
      k.flags = sc.version < 2 ? 1u : static_cast<uint32_t>(r.I32());
      for (auto& f : KeyLayout(k.flags)) k.values.push_back(f.kind == kInt ? r.I32() : r.Fixed());
      if (r.err) throw FormatError("part P " + std::to_string(p.index) + ": key parse error");
      if (r.CharIn("}") != '}') throw FormatError("part P " + std::to_string(p.index) + ": key not closed by }");
      p.keys.push_back(std::move(k));
    }
    sc.parts.push_back(std::move(p));
    c = r.CharIn("P");
  }
  return sc;
}

Scene Read(const std::vector<uint8_t>& data) { return IsCut1(data) ? ReadCut1(data) : ReadText(data); }

}  // namespace x3::scene
