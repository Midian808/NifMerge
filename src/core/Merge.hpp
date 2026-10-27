#pragma once
#include "Model.hpp"

namespace nm {

// Turns a flat list of nif paths into parts, pairing "<name>_0.nif" with "<name>_1.nif" when both
// are listed or the partner exists next to a listed file. Duplicates are removed.
std::vector<PartInput> PairPartFiles(const std::vector<std::string>& paths);

// What wearing each outfit item does to the character, read from the files' body slots only (no
// skeleton needed): one line per item, as MergeModel logs it. Throws when two items take one slot.
std::vector<std::string> DescribeOutfit(const std::vector<PartInput>& character, const std::vector<PartInput>& outfit);

// Loads the skeleton and every part, applies the weight blend, re-poses everything onto the
// skeleton, and merges it into one mesh with one material slot per source shape. Outfit items replace
// the character's triangles in the body slots they take (see PlanOutfit in Merge.cpp).
MergedModel MergeModel(const MergeRequest& req, const LogFn& log);

} // namespace nm
