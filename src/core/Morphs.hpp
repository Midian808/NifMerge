#pragma once
#include "Model.hpp"

namespace nm {

// Expands files and folders (searched recursively) into a sorted list of .tri files.
std::vector<std::string> ExpandMorphPaths(const std::vector<std::string>& paths);

// Matches each .tri to the merged mesh by geometry and adds its morphs as blend shapes.
// A TRI's base mesh is compared with every material slot in the rest pose (rest = skinToRest * tri + t,
// with t solved per candidate):
//   index   - same vertex count, vertex i to vertex i
//   window  - fewer vertices, vertex i to vertex start + i (e.g. one eye of a two-eye mesh)
//   surface - a lower-resolution mask lying on the slot's surface; its morph is transferred to the
//             slot's vertices through the nearest mask triangle, fading to zero away from the mask
// Same-named files are alternatives: the stronger match wins (index > window > surface), then the
// denser mesh. Every decision is logged and recorded in m.morphSources.
void ApplyMorphs(MergedModel& m, const std::vector<std::string>& paths, const LogFn& log);

} // namespace nm
