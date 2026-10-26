#include "derive.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <vector>

namespace x3 {

namespace {
struct V3 {
  double x = 0, y = 0, z = 0;
};
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Norm(V3 a) {
  const double l = std::sqrt(Dot(a, a));
  return l > 0 ? a * (1.0 / l) : V3{};
}
int32_t Fix(double v) { return static_cast<int32_t>(std::llround(v * 65536.0)); }
}  // namespace

void DeriveTangents(const bob::Lod& lod, bob::Part& part) {
  const size_t n = lod.points.size();
  std::vector<V3> tan(n), bin(n);
  for (auto& g : part.groups)
    for (auto& f : g.faces) {
      const bob::Point* p[3];
      bool ok = true;
      for (int k = 0; k < 3; k++) {
        ok &= f.v[k] >= 0 && static_cast<size_t>(f.v[k]) < n;
        if (ok) p[k] = &lod.points[f.v[k]];
      }
      if (!ok) continue;
      const V3 a{double(p[0]->pos[0]), double(p[0]->pos[1]), double(p[0]->pos[2])};
      const V3 e1 = V3{double(p[1]->pos[0]), double(p[1]->pos[1]), double(p[1]->pos[2])} - a;
      const V3 e2 = V3{double(p[2]->pos[0]), double(p[2]->pos[1]), double(p[2]->pos[2])} - a;
      const double du1 = (p[1]->uv[0] - p[0]->uv[0]) / 65536.0, dv1 = (p[1]->uv[1] - p[0]->uv[1]) / 65536.0;
      const double du2 = (p[2]->uv[0] - p[0]->uv[0]) / 65536.0, dv2 = (p[2]->uv[1] - p[0]->uv[1]) / 65536.0;
      const double det = du1 * dv2 - du2 * dv1;
      if (std::fabs(det) < 1e-20) continue;
      const double r = 1.0 / det;
      const V3 t = (e1 * dv2 - e2 * dv1) * r;
      const V3 b = (e2 * du1 - e1 * du2) * r;
      for (int k = 0; k < 3; k++) tan[f.v[k]] = tan[f.v[k]] + t, bin[f.v[k]] = bin[f.v[k]] + b;
    }
  for (auto& g : part.groups) {
    g.tangents.clear();
    std::vector<char> seen(n, 0);
    std::vector<int32_t> pts;
    for (auto& f : g.faces)
      for (int32_t v : f.v)
        if (v >= 0 && static_cast<size_t>(v) < n && !seen[v]) seen[v] = 1, pts.push_back(v);
    std::sort(pts.begin(), pts.end());
    for (int32_t v : pts) {
      const bob::Point& p = lod.points[v];
      const V3 nrm = Norm({p.normal[0] / 65536.0, p.normal[1] / 65536.0, p.normal[2] / 65536.0});
      V3 t = Norm(tan[v] - nrm * Dot(nrm, tan[v]));
      if (Dot(t, t) == 0) t = Norm(Cross(std::fabs(nrm.y) < 0.9 ? V3{0, 1, 0} : V3{1, 0, 0}, nrm));
      V3 b = Norm(bin[v] - nrm * Dot(nrm, bin[v]) - t * Dot(t, bin[v]));
      if (Dot(b, b) == 0) b = Cross(nrm, t);
      bob::TangentRecord rec;
      rec.point = v;
      rec.vec[0] = Fix(t.x), rec.vec[1] = Fix(t.y), rec.vec[2] = Fix(t.z);
      rec.vec[3] = Fix(b.x), rec.vec[4] = Fix(b.y), rec.vec[5] = Fix(b.z);
      g.tangents.push_back(rec);
    }
  }
}

void DeriveBounds(const bob::Lod& lod, bob::Part& part) {
  const size_t n = lod.points.size();
  std::vector<char> used(n, 0);
  for (auto& g : part.groups)
    for (auto& f : g.faces)
      for (int32_t v : f.v)
        if (v >= 0 && static_cast<size_t>(v) < n) used[v] = 1;
  long long sum[3] = {0, 0, 0}, count = 0;
  int32_t mn[3] = {INT32_MAX, INT32_MAX, INT32_MAX}, mx[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
  for (size_t v = 0; v < n; v++) {
    if (!used[v]) continue;
    count++;
    for (int k = 0; k < 3; k++) {
      sum[k] += lod.points[v].pos[k];
      mn[k] = std::min(mn[k], lod.points[v].pos[k]);
      mx[k] = std::max(mx[k], lod.points[v].pos[k]);
    }
  }
  if (!count) {
    part.bounds.fill(0);
    return;
  }
  int32_t c[3];
  for (int k = 0; k < 3; k++) c[k] = static_cast<int32_t>(sum[k] / count);
  long long r = 0;
  for (size_t v = 0; v < n; v++)
    if (used[v])
      for (int k = 0; k < 3; k++) r = std::max(r, std::llabs(static_cast<long long>(lod.points[v].pos[k]) - c[k]));
  part.bounds[0] = c[0], part.bounds[1] = c[1], part.bounds[2] = c[2];
  part.bounds[3] = static_cast<int32_t>(r);
  for (int k = 0; k < 3; k++) {
    part.bounds[4 + k] = static_cast<int32_t>((static_cast<long long>(mn[k]) + mx[k]) / 2);
    part.bounds[7 + k] = static_cast<int32_t>((static_cast<long long>(mx[k]) - mn[k]) / 2);
  }
}

}  // namespace x3
