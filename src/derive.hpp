#pragma once
// Data the engine needs per part that an edited mesh has to get recomputed: the per-group tangent frame records
// and the part's 10 bound integers (both present when the part flags have 0x10000000).
#include "bob.hpp"

namespace x3 {

// tangent frame records of every group of the part (points of the LOD; the part's faces)
void DeriveTangents(const bob::Lod& lod, bob::Part& part);
// the 10 bound integers: centre (3), L-infinity radius, AABB centre (3), AABB half extents (3)
void DeriveBounds(const bob::Lod& lod, bob::Part& part);

}  // namespace x3
