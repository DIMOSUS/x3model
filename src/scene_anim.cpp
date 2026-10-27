#include "scene_anim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace x3::anim {

namespace {

using i64 = int64_t;

int32_t I32(i64 v) { return static_cast<int32_t>(static_cast<uint32_t>(v)); }
// round16(a*b): IMUL; ADD 0x8000; ADC 0; SHRD 16
int32_t R16(i64 a, i64 b) { return I32((a * b + 0x8000) >> 16); }
i64 CDiv(i64 a, i64 b) { return a / b; }  // C division truncates toward zero
// Math::FixedDivide 0x00412450
int32_t FixDiv(i64 a, i64 b) { return b == 0 ? 0 : I32(CDiv(a * 65536, b)); }
float F32(double x) { return static_cast<float>(x); }
int32_t Trunc(double x) { return static_cast<int32_t>(x); }

constexpr double kTwoPiF = 6.2831854820251465;  // float 2*pi (Math::g_TwoPi)
constexpr i64 kPiFNum = 13176795;               // float pi = 13176795 / 2^22

struct Tables {
  int32_t sin[0x4001];
  int32_t atan[0x10001];
  Tables() {
    const double c = kTwoPiF / 65536;
    for (int k = 0; k <= 0x4000; k++) sin[k] = Trunc(std::sin(k * c) * 65536.0);
    const double q = std::atan2(1.0, 1.0) * 8.0;
    for (int i = 0; i <= 0x10000; i++) atan[i] = Trunc(std::atan(i / 65536.0) / q * 65536.0);
  }
};
const Tables& Tab() {
  static const Tables t;
  return t;
}
int32_t Sin16(i64 u) {
  const auto& t = Tab().sin;
  u &= 0xffff;
  if (u <= 0x4000) return t[u];
  if (u <= 0x8000) return t[0x8000 - u];
  if (u <= 0xc000) return -t[u - 0x8000];
  return -t[0x10000 - u];
}
int32_t Cos16(i64 u) {
  const auto& t = Tab().sin;
  u &= 0xffff;
  if (u <= 0x4000) return t[0x4000 - u];
  if (u <= 0x8000) return -t[u - 0x4000];
  if (u <= 0xc000) return -t[0xc000 - u];
  return t[u - 0xc000];
}

// Win32 MulDiv: a*b/c rounded half away from zero, -1 on overflow
i64 MulDiv(i64 a, i64 b, i64 c) {
  if (c == 0) return -1;
  if (c < 0) a = -a, c = -c;
  const i64 r = (a < 0) != (b < 0) ? CDiv(a * b - c / 2, c) : CDiv(a * b + c / 2, c);
  return r >= -2147483647 && r <= 2147483647 ? r : -1;
}
// Math::LerpInt 0x00469de0 (t from the segment start, dur = segment length)
i64 LerpInt(i64 t, i64 dur, i64 end, i64 start) {
  if (dur == 0) return CDiv(start + end, 2);
  if (t < 1) return start;
  if (dur <= t) return end;
  if (t < 0x10000 && dur < 0x10000) return CDiv(start + end, 2) + MulDiv(end - start, t * 2 - dur, dur * 2);
  return CDiv(start + end, 2) + CDiv((end - start) * (t - CDiv(dur, 2)), dur);
}

// Scene::FindKeyframeIndex 0x00496d30
template <class K>
size_t FindKey(const std::vector<K>& keys, i64 t) {
  size_t hi = keys.size() - 1;
  if (keys[hi].t <= t) return hi;
  if (!(keys[0].t < t)) return 0;
  size_t lo = 0, mid = 0;
  if (hi != 0) {
    for (;;) {
      if (lo + 1 == hi && t < keys[hi].t) return lo;
      mid = (hi + lo) / 2;
      if (t < keys[mid].t) hi = mid, mid = lo;
      lo = mid;
      if (mid == hi) break;
    }
  }
  return mid;
}

// Scene::TcbEaseCurve 0x0048cf60
int32_t Ease(i64 t, i64 ein, i64 eout) {
  const i64 s = ein + eout;
  if (s == 0) return I32(t);
  if (s > 0x10000) ein = FixDiv(ein, s), eout = FixDiv(eout, s);
  const i64 d = 0x20000 - ein - eout;
  const i64 k = d == 0 ? 0 : CDiv(i64(1) << 32, d);
  if (t < ein) return I32(CDiv(k * R16(t, t), ein));
  if (t < 0x10000 - eout) return R16(k, t * 2 - ein);
  if (eout == 0) return 0x10000;
  return I32(0x10000 - CDiv(k * R16(0x10000 - t, 0x10000 - t), eout));
}

using Q4 = std::array<int32_t, 4>;
// Math::QuatMultiplyFixed 0x004f2640(q2, q1): q1 (x) q2, then Math::QuatNormalizeFixed
Q4 QMulFixed(const int32_t* q2, const int32_t* q1) {
  const i64 w1 = q1[0], x1 = q1[1], y1 = q1[2], z1 = q1[3], w2 = q2[0], x2 = q2[1], y2 = q2[2], z2 = q2[3];
  i64 w = i64(R16(w1, w2)) - R16(z1, z2) - R16(y1, y2) - R16(x1, x2);
  i64 x = i64(R16(y1, z2)) - R16(z1, y2) + R16(x1, w2) + R16(w1, x2);
  i64 y = i64(R16(z1, x2)) - R16(x1, z2) + R16(y1, w2) + R16(w1, y2);
  i64 z = i64(R16(x1, y2)) - R16(y1, x2) + R16(z1, w2) + R16(w1, z2);
  if (w || x || y || z) {
    const double k = 1.0 / (std::sqrt(double(x) * x + double(y) * y + double(z) * z + double(w) * w) * 1.52587890625e-05);
    w = Trunc(w * k), x = Trunc(x * k), y = Trunc(y * k), z = Trunc(z * k);
  }
  return {I32(w), I32(x), I32(y), I32(z)};
}
// Math::AxisAngleToQuatFixed 0x004f2b50
Q4 AxisAngleQuat(i64 angle, i64 ax, i64 ay, i64 az) {
  if (ax == 0 && ay == 0 && az == 0) return {0x10000, 0, 0, 0};
  const i64 h = CDiv(angle, 2) & 0xffff;
  const i64 s = Sin16(h);
  return {Cos16(h), R16(ax, s), R16(ay, s), R16(az, s)};
}
// Math::QuatSlerp 0x004f2920: from -> to by u (16.16), short arc
Q4 Slerp(const int32_t* qf, const int32_t* qt, i64 u) {
  const double fu = u * 1.52587890625e-05;
  double f[4], g[4];
  for (int k = 0; k < 4; k++) f[k] = qf[k] * 1.52587890625e-05, g[k] = qt[k] * 1.52587890625e-05;
  const double dot = f[1] * g[1] + f[2] * g[2] + f[3] * g[3] + f[0] * g[0];
  bool neg = false;
  double c;
  if (dot > 1.0) c = 1.0;
  else if (dot < -1.0) c = 1.0, neg = true;
  else if (dot < 0.0) c = -dot, neg = true;
  else c = dot;
  const double th = std::acos(c), s = std::sin(th);
  if (s == 0.0) return {qf[0], qf[1], qf[2], qf[3]};
  const double r = 1.0 / s;
  const double k0 = std::sin((1.0 - fu) * th) * r;
  double k1 = std::sin(th * fu) * r;
  if (neg) k1 = -k1;
  Q4 out;
  for (int k = 0; k < 4; k++) out[k] = Trunc((k0 * f[k] + k1 * g[k]) * 65536.0);
  return out;
}
// Math::QuatToMatrixRows 0x004f2300
std::array<std::array<int32_t, 3>, 3> QuatMatrix(const Q4& q) {
  const i64 w = q[0], x = q[1], y = q[2], z = q[3];
  const i64 xx = R16(x, x), yy = R16(y, y), zz = R16(z, z), xy = R16(x, y), xz = R16(x, z), yz = R16(y, z);
  const i64 wx = R16(w, x), wy = R16(w, y), wz = R16(w, z), a = 0x8000 - zz;
  return {{{I32((a - yy) * 2), I32((xy - wz) * 2), I32((wy + xz) * 2)},
           {I32((wz + xy) * 2), I32((a - xx) * 2), I32((yz - wx) * 2)},
           {I32((xz - wy) * 2), I32((wx + yz) * 2), I32(((0x8000 - yy) - xx) * 2)}}};
}
// Math::AnglesToMatrix 0x004f0270: a = yaw (Y), b = pitch (X), c = roll (Z)
std::array<std::array<int32_t, 3>, 3> AnglesMatrix(i64 a, i64 b, i64 c) {
  const double k = 1.52587890625e-05 * kTwoPiF;
  const double A = (a & 0xffff) * k, B = (b & 0xffff) * k, C = (c & 0xffff) * k;
  const double ca = std::cos(A), sa = std::sin(A), cb = std::cos(B), sb = std::sin(B), cc = std::cos(C), sc = std::sin(C);
  const double F = 65536.0;
  return {{{Trunc((cc * ca + sc * sb * sa) * F), Trunc(sc * cb * F), Trunc((cc * sa - F32(sc * sb) * ca) * F)},
           {Trunc((F32(cc * sb) * sa - sc * ca) * F), Trunc(cc * cb * F), Trunc((-(sc * sa) - F32(cc * sb) * ca) * F)},
           {Trunc(-(sa * cb) * F), Trunc(F32(sb) * F), Trunc(F * cb * ca)}}};
}

void Scale(i64 k, const int32_t* v, int32_t* out) {
  for (int i = 0; i < 3; i++) out[i] = R16(v[i], k);
}
// Scene::ComputeTcbKeyTangents 0x0048f850
void TcbInner(const TrackKey& p, const TrackKey& n, TrackKey& k) {
  const i64 T = k.tcb[0], C = k.tcb[1], B = k.tcb[2];
  const i64 inA = R16(R16(0x10000 - T, 0x10000 - C), B + 0x10000), inB = R16(R16(0x10000 - T, C + 0x10000), 0x10000 - B);
  const i64 outA = R16(R16(0x10000 - T, C + 0x10000), B + 0x10000), outB = R16(R16(0x10000 - T, 0x10000 - C), 0x10000 - B);
  const i64 rIn = FixDiv(k.t - p.t, n.t - p.t), rOut = FixDiv(n.t - k.t, n.t - p.t);
  int32_t dIn[3], dOut[3], s1[3], s2[3], sum[3];
  for (int i = 0; i < 3; i++) dIn[i] = k.v[i] - p.v[i], dOut[i] = n.v[i] - k.v[i];
  Scale(inB, dOut, s1), Scale(inA, dIn, s2);
  for (int i = 0; i < 3; i++) sum[i] = s1[i] + s2[i];
  Scale(rIn, sum, k.a);
  Scale(outB, dOut, s1), Scale(outA, dIn, s2);
  for (int i = 0; i < 3; i++) sum[i] = s1[i] + s2[i];
  Scale(rOut, sum, k.b);
  k.b[3] = 0;
}
// Scene::ComputeTcbStartTangent 0x0048fb60 / ComputeTcbEndTangent 0x0048fcd0
void TcbEnd(const int32_t* D, const int32_t* d, i64 dtNear, i64 dtFar, i64 tension, int32_t* out) {
  const i64 den = dtFar * 2;
  const i64 r = den ? FixDiv(dtNear, den) : 0;
  const i64 c0 = 0x4000 - r, c2 = 0x10000 - tension;
  int32_t halfD[3], diff[3], s[3], cD[3], c2D[3];
  Scale(0x8000, D, halfD);
  for (int i = 0; i < 3; i++) diff[i] = d[i] - halfD[i];
  Scale(0x18000, diff, s);
  Scale(c0, D, cD);
  Scale(c2, cD, c2D);
  for (int i = 0; i < 3; i++) out[i] = halfD[i] + (c2D[i] + s[i]);
}

// Scene::InterpolatePositionKey 0x0048d0c0
std::array<int32_t, 3> InterpPos(const std::vector<TrackKey>& keys, size_t i, i64 t) {
  const TrackKey &k0 = keys[i], &k1 = keys[i + 1];
  const i64 s = FixDiv(t - k0.t, k1.t - k0.t);
  std::array<int32_t, 3> r;
  int32_t a[3], b[3];
  if (k0.f & 0x4000) {
    const i64 e = Ease(s, k0.tcb[4], k1.tcb[3]), e2 = R16(e, e), e3 = R16(e2, e);
    const i64 h00 = e3 * 2 + 0x10000 - e2 * 3, h01 = e2 * 3 - e3 * 2, h10 = e3 - e2 * 2 + e, h11 = e3 - e2;
    Scale(h01, k1.v, a), Scale(h00, k0.v, b);
    int32_t c[3], d[3];
    Scale(h10, k0.b, c), Scale(h11, k1.a, d);
    for (int j = 0; j < 3; j++) r[j] = d[j] + (c[j] + (a[j] + b[j]));
    return r;
  }
  Scale(s, k1.v, a), Scale(0x10000 - s, k0.v, b);
  for (int j = 0; j < 3; j++) r[j] = a[j] + b[j];
  return r;
}

Q4 LogKey(const TrackKey& k) {
  i64 a = k.v[0];
  if (a >= 0x8000) a -= 0x10000;
  const i64 m = CDiv(a * kPiFNum, i64(1) << 22);
  return {0, R16(k.a[0], m), R16(k.a[1], m), R16(k.a[2], m)};
}
Q4 QExp(const Q4& g) {
  const i64 m = static_cast<i64>(std::sqrt(double(F32(double(g[1]) * g[1] + double(g[2]) * g[2] + double(g[3]) * g[3]))));
  const double th = m * 1.52587890625e-05;
  const float sn = F32(std::sin(th));
  const int32_t w = Trunc(std::cos(th) * 65536.0);
  if (std::fabs(sn) < 0.001) return {w, g[1], g[2], g[3]};
  if (m == 0) return {w, 0, 0, 0};
  const i64 si = Trunc(sn * 65536.0);
  return {w, I32(CDiv(g[1] * si, m)), I32(CDiv(g[2] * si, m)), I32(CDiv(g[3] * si, m))};
}
// Scene::InterpolateRotationKeyTcb 0x0048d2a0
Q4 InterpRot(const std::vector<TrackKey>& keys, size_t i, i64 t) {
  const size_t n = keys.size();
  const TrackKey &k0 = keys[i], &k1 = keys[i + 1];
  t = std::min(std::max<i64>(t, 0), k1.t);
  const i64 s = FixDiv(t - k0.t, k1.t - k0.t);
  if (!(k0.f & 0x8000) && !(k1.f & 0x8000)) return Slerp(k0.b, k1.b, s);
  const Q4 L1 = LogKey(k1);
  Q4 L0, L2;
  i64 adj0, adj1;
  if (i < 1) {
    L0 = L1, adj0 = 0x10000;
  } else {
    L0 = LogKey(k0);
    const i64 dd = k1.t - keys[i - 1].t;
    const i64 r = dd == 0 ? 0 : I32(CDiv((k0.t - keys[i - 1].t) * 0x20000, dd));
    const i64 C = k0.tcb[1];
    adj0 = C - R16(0x20000 - r, C) + (0x20000 - r);
  }
  if (i + 1 < n - 1) {
    L2 = LogKey(keys[i + 2]);
    const i64 dd = keys[i + 2].t - k0.t;
    const i64 r = dd == 0 ? 0 : I32(CDiv((k1.t - k0.t) * 0x20000, dd));
    const i64 C = k1.tcb[1];
    adj1 = C - R16(r, C) + r;
  } else {
    L2 = L1, adj1 = 0x10000;
  }
  i64 T = k0.tcb[0], C = k0.tcb[1], B = k0.tcb[2];
  i64 wa = R16(CDiv(R16(R16(0x10000 - T, C + 0x10000), B + 0x10000), 2), adj0);
  i64 wb = R16(CDiv(R16(R16(0x10000 - T, 0x10000 - C), 0x10000 - B), 2), adj0);
  Q4 g;
  for (int j = 0; j < 4; j++) g[j] = I32(CDiv(i64(R16(wb - 0x10000, L1[j])) + R16(wa, L0[j]), 2));
  const Q4 ge = QExp(g);
  const Q4 qa = QMulFixed(ge.data(), k0.b);
  T = k1.tcb[0], C = k1.tcb[1], B = k1.tcb[2];
  wa = R16(CDiv(R16(R16(0x10000 - T, 0x10000 - C), B + 0x10000), 2), adj1);
  wb = R16(CDiv(R16(R16(0x10000 - T, C + 0x10000), 0x10000 - B), 2), adj1);
  Q4 h;
  for (int j = 0; j < 4; j++) h[j] = I32(CDiv(i64(R16(-wb, L2[j])) + R16(0x10000 - wa, L1[j]), 2));
  const Q4 he = QExp(h);
  const Q4 qb = QMulFixed(he.data(), k1.b);
  const i64 e = Ease(s, k0.tcb[4], k1.tcb[3]);
  const Q4 p = Slerp(k0.b, k1.b, e);
  const Q4 r = Slerp(qa.data(), qb.data(), e);
  return Slerp(p.data(), r.data(), i64(R16(e, 0x10000 - e)) * 2);
}

// the octant code of AnimateNode: the angle of (x, y) from +x towards +y in 16-bit turns
int32_t Atan16(i64 y, i64 x) {
  const auto& A = Tab().atan;
  if (y < 0) {
    const i64 a = -y;
    if (x < 0) {
      if (x < y) return A[FixDiv(a, -x)] + 0x8000;
      return 0xc000 - A[FixDiv(-x, a)];
    }
    if (a < x) return -A[FixDiv(a, x)] & 0xffff;
    return A[FixDiv(x, a)] + 0xc000;
  }
  if (x < 0) {
    const i64 b = -x;
    if (y < b) return 0x8000 - A[FixDiv(y, b)];
    return A[FixDiv(b, y)] + 0x4000;
  }
  if (y < x) return A[FixDiv(y, x)];
  if (x == 0 && y == 0) return 0;
  return 0x4000 - A[FixDiv(x, y)];
}

}  // namespace

Tracks Build(const scene::Part& part) {
  Tracks tr;
  i64 t = 0;
  static const char* kP[] = {"p_tension", "p_continuity", "p_bias", "p_ease_in", "p_ease_out"};
  static const char* kR[] = {"r_tension", "r_continuity", "r_bias", "r_ease_in", "r_ease_out"};
  for (auto& key : part.keys) {
    const uint32_t f = key.flags | 0x80;
    if (!(f & 0x10)) {
      TrackKey k;
      k.t = t, k.f = f;
      k.v[0] = key.Get("x"), k.v[1] = key.Get("y"), k.v[2] = key.Get("z");
      if (f & 0x4000)
        for (int i = 0; i < 5; i++) k.tcb[i] = key.Get(kP[i]);
      tr.pos.push_back(k);
    }
    if (!(f & 8)) {
      if (f & 0x20) {
        if (f & 2) {  // identity key, 0x20 cleared
          TrackKey k;
          k.t = t, k.f = f & ~0x20u;
          k.a[0] = -0x10000;
          tr.rot.push_back(k);
        }
      } else if (f & 2) {
        TrackKey k;
        k.t = t, k.f = f;
        k.v[0] = key.Get("angle");
        k.a[0] = I32(-i64(key.Get("ax"))), k.a[1] = I32(-i64(key.Get("ay"))), k.a[2] = I32(-i64(key.Get("az")));
        if (f & 0x8000)
          for (int i = 0; i < 5; i++) k.tcb[i] = key.Get(kR[i]);
        tr.rot.push_back(k);
      } else if (!(f & 0x200)) {
        TrackKey k;
        k.t = t, k.f = f;
        k.v[0] = key.Get("yaw"), k.v[1] = key.Get("pitch"), k.v[2] = key.Get("roll3");
        tr.rot.push_back(k);
      }
    } else {
      if (!(f & 0x40)) {
        TrackKey k;
        k.t = t, k.f = f;
        k.v[0] = key.Get("tx"), k.v[1] = key.Get("ty"), k.v[2] = key.Get("tz");
        tr.target.push_back(k);
      }
      if (!(f & 0x20)) tr.roll.push_back({t, f, {I32(-i64(key.Get("roll"))), 0, 0}});
    }
    if ((f & 0x200) && !(f & 0x400)) tr.colour.push_back({t, f, {key.Get("r"), key.Get("g"), key.Get("b")}});
    if ((f & 0x800) && !(f & 0x1000)) tr.fov.push_back({t, f, {I32(CDiv(i64(key.Get("fov")) * 65536, 0x6487e)), 0, 0}});
    t = I32(t + key.Get("dt"));
  }
  auto& pos = tr.pos;  // ComputeTcbTangentsForKeys; key 0 in = last out = 0
  const size_t n = pos.size();
  if (n == 2) {
    int32_t d[3];
    for (int i = 0; i < 3; i++) d[i] = pos[1].v[i] - pos[0].v[i];
    Scale(0x10000 - pos[0].tcb[0], d, pos[0].b);
    Scale(0x10000 - pos[1].tcb[0], d, pos[1].a);
  } else if (n >= 3) {
    for (size_t i = 1; i + 1 < n; i++) TcbInner(pos[i - 1], pos[i + 1], pos[i]);
    int32_t D[3], d[3];
    for (int i = 0; i < 3; i++) D[i] = pos[2].v[i] - pos[0].v[i], d[i] = pos[1].v[i] - pos[0].v[i];
    TcbEnd(D, d, pos[1].t - pos[0].t, pos[2].t - pos[0].t, pos[0].tcb[0], pos[0].b);
    const TrackKey &kl = pos[n - 1], &kp = pos[n - 2], &kq = pos[n - 3];
    for (int i = 0; i < 3; i++) D[i] = kl.v[i] - kq.v[i], d[i] = kl.v[i] - kp.v[i];
    TcbEnd(D, d, kl.t - kp.t, kl.t - kq.t, kl.tcb[0], pos[n - 1].a);
  }
  Q4 run = {0x10000, 0, 0, 0};  // BuildRotationKeyQuats: flag 2, not 4/0x20
  for (auto& k : tr.rot) {
    if ((k.f & 2) && !(k.f & 0x24)) {
      Q4 q = AxisAngleQuat(k.v[0], k.a[0], k.a[1], k.a[2]);
      if (!(k.f & 0x2000)) q = QMulFixed(q.data(), run.data());
      run = q;
      memcpy(k.b, q.data(), sizeof k.b);
    }
    k.f |= 4;
  }
  return tr;
}

State Evaluate(const Tracks& tr, int64_t tMs) {
  State out;
  const i64 t = std::max<i64>(tMs, 0);
  int32_t st[4] = {0, 0, 0, 0};
  if (!tr.pos.empty()) {
    const size_t i = FindKey(tr.pos, t);
    if (t == tr.pos[i].t || i == tr.pos.size() - 1) out.pos = std::array<int32_t, 3>{tr.pos[i].v[0], tr.pos[i].v[1], tr.pos[i].v[2]};
    else out.pos = InterpPos(tr.pos, i, t);
  }
  const std::array<int32_t, 3> p = out.pos.value_or(std::array<int32_t, 3>{0, 0, 0});
  if (!tr.rot.empty()) {
    const size_t i = FindKey(tr.rot, t);
    const TrackKey& k = tr.rot[i];
    if (t == k.t || i == tr.rot.size() - 1) {
      if (!(k.f & 2)) {
        st[0] = k.v[0] & 0xffff, st[1] = k.v[1] & 0xffff, st[2] = k.v[2] & 0xffff;
        out.m = AnglesMatrix(st[0], st[1], st[2]);
        out.anglesAreYawPitchRoll = true;
      } else {
        memcpy(st, k.b, sizeof st);
        out.m = QuatMatrix({st[0], st[1], st[2], st[3]});
      }
    } else if (!(k.f & 2) && !(tr.rot[i + 1].f & 2)) {
      const TrackKey& k1 = tr.rot[i + 1];
      for (int j = 0; j < 3; j++) st[j] = I32(LerpInt(t - k.t, k1.t - k.t, k1.v[j], k.v[j]) & 0xffff);
      out.m = AnglesMatrix(st[0], st[1], st[2]);
      out.anglesAreYawPitchRoll = true;
    } else {
      const Q4 q = InterpRot(tr.rot, i, t);
      memcpy(st, q.data(), sizeof st);
      out.m = QuatMatrix(q);
    }
  }
  if (!tr.target.empty()) {
    const size_t i = FindKey(tr.target, t);
    std::array<int32_t, 3> g;
    if (t == tr.target[i].t || i == tr.target.size() - 1) g = {tr.target[i].v[0], tr.target[i].v[1], tr.target[i].v[2]};
    else g = InterpPos(tr.target, i, t);
    const i64 u = i64(p[0]) - g[0], v = i64(g[2]) - p[2];
    const int32_t yaw = Atan16(u, v);
    const i64 hor = i64(R16(v, Cos16(yaw))) + R16(u, Sin16(yaw));
    const int32_t pitch = Atan16(i64(g[1]) - p[1], hor);
    st[0] = yaw & 0xffff, st[1] = pitch & 0xffff;
    out.m = AnglesMatrix(st[0], st[1], st[2]);
    out.anglesAreYawPitchRoll = true;
  }
  if (!tr.roll.empty()) {
    const size_t i = FindKey(tr.roll, t);
    const ValueKey& k0 = tr.roll[i];
    if (t == k0.t || i == tr.roll.size() - 1) {
      st[2] = k0.v[0] & 0xffff;
    } else {
      const ValueKey& k1 = tr.roll[i + 1];
      i64 d = (i64(k1.v[0]) - k0.v[0]) & 0xffff;  // shortest 16-bit arc
      if (d > 0x8000) d -= 0x10000;
      st[2] = I32(LerpInt(t - k0.t, k1.t - k0.t, d + k0.v[0], k0.v[0]));
    }
    out.m = AnglesMatrix(st[0], st[1], st[2]);
    out.anglesAreYawPitchRoll = true;
  }
  if (!tr.colour.empty()) {
    const size_t i = FindKey(tr.colour, t);
    std::array<int32_t, 3> c;
    for (int j = 0; j < 3; j++) {
      i64 v = tr.colour[i].v[j];
      if (!(t == tr.colour[i].t || i == tr.colour.size() - 1))
        v = LerpInt(t - tr.colour[i].t, tr.colour[i + 1].t - tr.colour[i].t, tr.colour[i + 1].v[j], v);
      c[j] = I32(((v * 0xff + 0x8000) >> 16) & 0xffff);
    }
    out.colour = c;
  }
  if (!tr.fov.empty()) {
    const size_t i = FindKey(tr.fov, t);
    i64 v = tr.fov[i].v[0];
    if (!(t == tr.fov[i].t || i == tr.fov.size() - 1))
      v = LerpInt(t - tr.fov[i].t, tr.fov[i + 1].t - tr.fov[i].t, tr.fov[i + 1].v[0], v);
    out.fov = I32(v);
  }
  memcpy(out.angles, st, sizeof st);
  return out;
}

int64_t Length(const Tracks& tr) {
  int64_t end = 0;
  for (auto* v : {&tr.pos, &tr.rot, &tr.target})
    if (!v->empty()) end = std::max(end, v->back().t);
  for (auto* v : {&tr.roll, &tr.colour, &tr.fov})
    if (!v->empty()) end = std::max(end, v->back().t);
  return end;
}

}  // namespace x3::anim
