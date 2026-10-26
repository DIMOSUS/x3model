#pragma once
// Body (BOB1) <-> FBX.
//
// Export: a root node for the body, one child node per LOD ("LOD0", ...), and under each LOD one mesh node per part.
// Geometry is in game units with Z negated (the game is left-handed; FBX right-handed, Y up), normals and two UV
// sets per vertex, one FBX material per body material. Everything the FBX geometry cannot carry (effect material
// parameters, point flags and smoothing values, face words, part flags and bounds, tangent records, LOD values) is
// kept in user properties named x3_*; Blender imports and re-exports them as custom properties. An unedited FBX
// imports back to the same body byte for byte; an edited one gets the derived data recomputed.
#include <string>
#include <vector>

#include "bob.hpp"
#include "fbx_write.hpp"

namespace x3 {

class Game;

struct ExportOptions {
  double unitCm = 0.1;           // one game unit in the FBX: 0.1 cm = 1 mm
  std::string textureDir;        // textures referenced relative to the .fbx in this folder ("" = not referenced)
  const Game* game = nullptr;    // to copy the textures out of the catalogues
  std::string outDir;            // where the .fbx goes (for the texture copies)
};

// body -> FBX scene; name is the body's file stem; textures to copy are added to *textures as (game path, out path)
fbx::Scene BodyToFbx(const bob::Body& body, const std::string& name, const ExportOptions& opt,
                     std::vector<std::pair<std::string, std::string>>* textures);

struct ImportReport {
  int lods = 0, parts = 0, points = 0, faces = 0;
  bool exact = true;              // every part matched its stored data (unedited)
  std::vector<std::string> notes;
};

// FBX file -> body
bob::Body FbxToBody(const std::string& fbxPath, ImportReport& report);

// In-memory: the texture file a material parameter names (basename, without folders)
std::string TextureBase(const std::string& path);

}  // namespace x3
