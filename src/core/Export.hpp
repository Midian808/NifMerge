#pragma once
#include "Model.hpp"

namespace nm {

// Skyrim (Z-up, right-handed, character facing +Y, game units) -> FBX (Y-up, right-handed,
// character facing +Z, metres). (x, y, z) -> (-x, z, y) is a proper rotation (det +1), so triangle
// winding and handedness are preserved. 1 game unit = 1/64 yard = 0.0142875 m.
constexpr double kMetresPerUnit = 0.0142875;

// Maps a decoded model-space normal-map texel (r, g, b each remapped to [-1, 1]) to a normal in the
// space of the mesh Unity imports from our FBX. Skyrim _msn maps store X in red, Y in blue and Z in
// green (measured on body, hands and head maps); Unity mesh space is Skyrim (x, y, z) -> (x, z, y).
Mat3 MsnTexelToUnityMesh(const Mat3& skinToRest);

// Writes a binary FBX: bone hierarchy, one skinned mesh, one material per slot, bind pose.
void WriteFbx(const MergedModel& m, const std::string& fbxPath, const LogFn& log);

// Sidecar consumed by the Unity editor package: material slots, texture paths, shader settings,
// and the Humanoid bone mapping. Path convention: <fbx without extension>.materials.json
std::string SidecarPathFor(const std::string& fbxPath);
void WriteMaterialsJson(const MergedModel& m, const std::string& fbxPath, const std::string& jsonPath);

// Debug dump of the merged rest-pose geometry in Skyrim space.
void WriteObj(const MergedModel& m, const std::string& objPath);

} // namespace nm
