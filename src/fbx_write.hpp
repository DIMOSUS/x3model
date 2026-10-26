#pragma once
// A writer of binary FBX 7.4 (the format Blender, 3ds Max and Maya read; ufbx reads it back). Grown from the
// MW4M model dump writer (mw4hook src/fbx_write.cpp): the same node encoding, extended with a node hierarchy,
// two UV sets, per-polygon materials, several texture channels per material, user properties (Properties70
// entries flagged "U", which Blender imports as custom properties) and animation curves.
//
// Conventions of what this writer produces: right-handed, Y up, Z front; lengths in the file's unit, declared by
// unitCm (the length of one unit in centimetres, FBX UnitScaleFactor).
#include <cstdint>
#include <string>
#include <vector>

namespace x3::fbx {

struct Prop {
  enum Kind { kInt, kDouble, kString } kind = kString;
  std::string name;
  int64_t i = 0;
  double d = 0;
  std::string s;
  static Prop Int(std::string n, int64_t v) { Prop p; p.kind = kInt, p.name = std::move(n), p.i = v; return p; }
  static Prop Dbl(std::string n, double v) { Prop p; p.kind = kDouble, p.name = std::move(n), p.d = v; return p; }
  static Prop Str(std::string n, std::string v) { Prop p; p.kind = kString, p.name = std::move(n), p.s = std::move(v); return p; }
};

struct Texture {
  std::string channel;  // material property it connects to: DiffuseColor, NormalMap, SpecularColor, EmissiveColor, ...
  std::string file;     // relative to the .fbx (or absolute), forward slashes
};

struct Material {
  std::string name;
  double diffuse[3] = {1, 1, 1};
  std::vector<Texture> textures;
  std::vector<Prop> props;  // user properties
};

struct Mesh {
  std::string name;
  std::vector<double> positions;   // 3 per control point
  std::vector<double> normals;     // 3 per control point, or empty
  std::vector<double> uv[2];       // 2 per control point (v up: 0 at the bottom), or empty
  std::vector<int32_t> triangles;  // 3 control point indices per triangle, counter-clockwise front
  std::vector<int32_t> triangleMaterial;  // per triangle, index into the owning node's material slots
};

struct Curve {
  std::vector<double> time;   // seconds
  std::vector<double> value;
};

struct Node {
  std::string name;
  int parent = -1;  // index into Scene::nodes, -1 = the scene root
  double translation[3] = {0, 0, 0};
  double rotation[3] = {0, 0, 0};  // Euler degrees, XYZ order
  double scaling[3] = {1, 1, 1};
  int mesh = -1;                   // index into Scene::meshes
  std::vector<int> materials;      // material slots (indices into Scene::materials)
  std::vector<Prop> props;         // user properties
  // animation of Lcl Translation / Lcl Rotation (x, y, z); empty curves are not written
  Curve animT[3], animR[3];
  bool hidden = false;
};

struct Scene {
  double unitCm = 0.1;       // one file unit in centimetres (0.1 = millimetres)
  std::string creator = "x3model";
  std::vector<Node> nodes;
  std::vector<Mesh> meshes;
  std::vector<Material> materials;
  double animEnd = 0;        // seconds; 0 = no animation stack
  double fps = 30;
};

// false with the reason in error
bool Write(const std::string& path, const Scene& scene, std::string& error);

}  // namespace x3::fbx
