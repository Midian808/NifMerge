#pragma once
#include "Model.hpp"

namespace nm {

// Turns a flat list of nif paths into parts, pairing "<name>_0.nif" with "<name>_1.nif" when both
// are listed or the partner exists next to a listed file. Duplicates are removed.
std::vector<PartInput> PairPartFiles(const std::vector<std::string>& paths);

// Loads the skeleton and every part, applies the weight blend, re-poses everything onto the
// skeleton, and merges it into one mesh with one material slot per source shape.
MergedModel MergeModel(const MergeRequest& req, const LogFn& log);

} // namespace nm
