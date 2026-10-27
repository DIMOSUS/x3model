#pragma once
// Scene (CUT1 / text) <-> FBX.
//
// Export: a root node for the scene, one node per part (parent = the part whose P number is the part's F), its rest
// transform from the part's first key, its keys as FBX animation (times in seconds; the game counts milliseconds).
// The referenced bodies' LOD 0 geometry is added under each part as child mesh nodes marked x3_ref (shared between
// parts using the same body), so the scene can be seen and placed; those nodes are ignored on import. Everything
// needed to write the scene back exactly (part fields, raw keys) is kept in x3_* user properties; a part whose FBX
// transform and animation are unchanged gets its stored keys back; for an edited one the keys are merged one by one:
// unchanged keys stay, changed ones take the FBX pose in their own form, FBX keys between them become new keys.
#include <array>
#include <functional>
#include <string>
#include <vector>

#include "body_fbx.hpp"
#include "scene.hpp"

namespace x3 {

// resolves a part's body reference to the body file's bytes (empty: not found / not a body)
using BodyLoader = std::function<std::vector<uint8_t>(const std::string& body)>;

fbx::Scene SceneToFbx(const scene::Scene& sc, const std::string& name, const ExportOptions& opt, const BodyLoader& load,
                      std::vector<std::pair<std::string, std::string>>* textures);

struct SceneImportReport {
  int parts = 0, keptKeys = 0, rebuiltKeys = 0, newParts = 0;
  std::vector<std::string> notes;
};
scene::Scene FbxToScene(const std::string& fbxPath, SceneImportReport& report);

// true if the FBX holds a scene (a node with x3_kind = scene)
bool FbxIsScene(const std::string& fbxPath);

// for tests: per key, the pose the engine reaches there in FBX space (translation; rotation as w, x, y, z), and an
// FBX Euler XYZ rotation (degrees) as w, x, y, z
std::vector<std::array<double, 7>> KeyPosesFbx(const scene::Part& part);
std::array<double, 4> EulerQuat(const double euler[3]);

}  // namespace x3
