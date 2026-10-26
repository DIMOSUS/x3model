#pragma once
// Text bodies (.bod, packed .pbd), read the way the engine's text loader Mesh::ParseTextBody 0x00483f20 builds
// its model (see docs/formats.md, "Text body"), into the same bob::Body a binary body reads into:
//   - values end at ';', '/' starts a comment to the end of the line (so '/! N: ... !/' blocks are ignored);
//   - each record (LOD) is normalised so its largest |coordinate| becomes 65536, its first value is the LOD value;
//   - faces of 3 or 4 corners, fanned; corners merge into points by smoothing group, position and UV;
//   - normals from the geometry (angle-weighted face normals per smoothing group, flat for group 0);
//   - no tangent records (the engine builds none for a text body).
// The normals are computed in floating point, not with the engine's fixed-point routine bit for bit.
#include <cstdint>
#include <string>
#include <vector>

#include "bob.hpp"

namespace x3::textbody {

// true for a text body (not a scene): has no "VER:" scene header and no "P n;" node lines
bool IsTextBody(const std::vector<uint8_t>& data);
bool IsTextScene(const std::vector<uint8_t>& data);
bob::Body Read(const std::vector<uint8_t>& data);

}  // namespace x3::textbody
