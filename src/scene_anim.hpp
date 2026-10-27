#pragma once
// The engine's animation of a scene part, as Scene::AnimateNode 0x0048dc20 computes it from the tracks that
// Scene::CommitCutArrays 0x004901a0 prepares (RETools projects/x3ap/model/scene_format.md section 9; a C++ copy of the
// reference evaluate_tracks() in x3scene.py): positions linear or Hermite with Kochanek-Bartels tangents and the TCB
// ease curve, rotations slerp or squad, camera targets with a roll track, all in 16.16 fixed point. The engine's
// x87 steps are done in doubles, so a value can be 1/65536 off in rare cases.
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "scene.hpp"

namespace x3::anim {

struct TrackKey {  // one 0x50-byte engine key of a position, rotation or target track
  int64_t t = 0;
  uint32_t f = 0;
  int32_t tcb[5] = {0, 0, 0, 0, 0};  // tension, continuity, bias, ease in, ease out (0 unless read)
  int32_t v[3] = {0, 0, 0};          // position / target / angle (v[0]) / three angles
  int32_t a[3] = {0, 0, 0};          // incoming tangent / axis (negated)
  int32_t b[4] = {0, 0, 0, 0};       // outgoing tangent / quaternion w, x, y, z
};
struct ValueKey {  // roll, FOV (one value) or colour (three)
  int64_t t = 0;
  uint32_t f = 0;
  int32_t v[3] = {0, 0, 0};
};
struct Tracks {
  std::vector<TrackKey> pos, rot, target;
  std::vector<ValueKey> roll, colour, fov;
};
Tracks Build(const scene::Part& part);

struct State {
  std::optional<std::array<int32_t, 3>> pos;              // engine units, parent space
  std::optional<std::array<std::array<int32_t, 3>, 3>> m;  // rows, 16.16, applied to row vectors
  // node+0x200..+0x20c afterwards: yaw, pitch, roll (16-bit turns) after a target/roll/three-angle track,
  // else the quaternion w, x, y, z (16.16)
  int32_t angles[4] = {0, 0, 0, 0};
  bool anglesAreYawPitchRoll = false;
  std::optional<int32_t> fov;                       // turns, 16.16
  std::optional<std::array<int32_t, 3>> colour;     // 0..255
};
State Evaluate(const Tracks& tracks, int64_t tMs);
int64_t Length(const Tracks& tracks);  // the largest last-key time of the tracks (Scene::GetAnimationLength)

}  // namespace x3::anim
