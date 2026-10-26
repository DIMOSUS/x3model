#pragma once
// BOB1: the game's binary body (.bob, packed .pbb). Read and written without loss: every field is kept as it is in
// the file, so Write(Read(x)) == x byte for byte. Layout as parsed by Mesh::ParseBobBody 0x00481aa0 in X3AP.exe
// (see docs/formats.md):
//
//   BOB1 { INFO cstr /INF | NAME cstr /NAM | VERS u32 /VER | SND1 u32 /SND
//        | MAT3|MAT5|MAT6 i32 n material*n /MAT | BODY u16 nLod lod*nLod /BOD } /BOB
//   lod  := i32 value  u32 flags  [BONE i32 n cstr*n /BON]  POIN i32 n point*n /POI
//           [WEIG i32 n (u16 k (u16 bone, i32 weight)*k)*n /WEI]  PART i32 n part*n /PAR
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace x3::bob {

// effect parameter types (SPTYPE_*, name table 0x0054f0a0)
enum ParamType : uint16_t {
  kLong = 0, kBool = 1, kFloat = 2, kFloat2 = 3, kFloat3 = 4, kFloat4 = 5, kMatrix3 = 6, kMatrix4 = 7, kString = 8,
};
int ParamValueCount(uint16_t type);  // number of i32 values; -1 for kString
const char* ParamTypeName(uint16_t type);

struct EffectParam {
  std::string name;
  uint16_t type = 0;
  std::vector<int32_t> ints;  // LONG/BOOL as is, FLOAT* / MATRIX* as 16.16 fixed
  std::string str;            // STRING
};

struct Material {
  uint16_t index = 0;
  uint32_t flags = 0;  // MAT6 only
  bool IsEffect(int version) const { return version == 6 && (flags & 0x02000000u); }
  // effect material (MAT6 with flags & 0x02000000): Material::ReadEffectMaterial 0x00470490
  uint16_t technique = 0;
  std::string effect;  // "argon.fx"
  std::vector<EffectParam> params;
  // classic material
  std::string texture;   // MAT6
  uint16_t textureId = 0;  // MAT3/MAT5
  std::array<uint16_t, 14> words{};  // colours/intensities (+0x04..+0x1a) and +0x24, +0x26
  uint16_t flagWord = 0;   // MAT3/MAT5
  uint16_t word2c = 0;     // +0x2c
  struct Map {
    std::string name;  // MAT6
    uint16_t id = 0;   // MAT3/MAT5
    uint16_t value = 0;
  };
  std::vector<Map> maps;  // 3 for MAT3/MAT5, 5 for MAT6
};

enum PointFlag : uint16_t { kPos = 1, kUv = 2, kUv2 = 4, kNormal = 8, kSmooth = 16 };

struct Point {
  uint16_t flags = 0;
  int32_t pos[3]{};     // body units (scaled by 1/65536 * LOD 0 value for metres in practice)
  int32_t uv[2]{};      // 16.16
  int32_t uv2[2]{};     // 16.16, only with kUv2
  int32_t normal[3]{};  // 16.16 unit vector
  uint32_t smooth = 0;  // smoothing group of the source face
};

struct Face {
  int32_t v[3]{};
  int32_t word = 0;  // stored as int16 by the engine
};

// per-group tangent frame record of a part with flags & 0x10000000: point index + two 16.16 vectors
struct TangentRecord {
  int32_t point = 0;
  int32_t vec[6]{};
};

struct Group {
  int32_t material = 0;  // index into the body's material array (negative allowed)
  std::vector<Face> faces;
  std::vector<TangentRecord> tangents;  // only if the part has flags & 0x10000000
};

constexpr uint32_t kPartHasExtra = 0x10000000u;  // tangent records + 10 bounds ints present

struct Part {
  uint32_t flags = 0;
  std::vector<Group> groups;
  std::array<int32_t, 10> bounds{};  // only if flags & kPartHasExtra
};

struct Weight {
  uint16_t bone = 0;
  int32_t weight = 0;
};

struct Lod {
  int32_t value = 0;   // LOD 0: object scale; LOD >= 1: switch threshold
  uint32_t flags = 0;  // 0x20: points carry a second UV set (set by the engine; kept as read)
  bool hasBones = false;
  std::vector<std::string> bones;
  std::vector<Point> points;
  bool hasWeights = false;
  std::vector<std::vector<Weight>> weights;  // per point
  std::vector<Part> parts;
};

enum class SectionKind { Info, Name, Vers, Snd, Mat, Body };

struct Section {
  SectionKind kind = SectionKind::Info;
  std::string str;           // Info, Name
  uint32_t value = 0;        // Vers, Snd
  int matVersion = 6;        // Mat: 3, 5, 6
  std::vector<Material> materials;
  std::vector<Lod> lods;     // Body
};

struct Body {
  std::vector<Section> sections;  // in file order

  // convenience access (first section of the kind, or null)
  const Section* Find(SectionKind k) const;
  Section* Find(SectionKind k);
};

bool IsBob(const std::vector<uint8_t>& data);
Body Read(const std::vector<uint8_t>& data);
std::vector<uint8_t> Write(const Body& body);

}  // namespace x3::bob
