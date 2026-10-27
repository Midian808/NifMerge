#pragma once
// Skyrim / FaceGen head morph files (.tri, "FRTRI003"): a base mesh plus named morphs.
#include "Math.hpp"

#include <array>
#include <string>
#include <vector>

namespace nm {

struct TriMorph {
	std::string name;
	std::vector<Vec3> deltas; // one per base vertex, in the file's own space
};

struct TriFile {
	std::string path;
	std::vector<Vec3> vertices;                    // base mesh
	std::vector<std::array<uint32_t, 3>> triangles; // base mesh faces (quads are split)
	std::vector<TriMorph> morphs;                  // difference morphs and modifier morphs, as deltas
};

// Reads an FRTRI003 file. Difference morphs are stored as scaled int16 offsets; modifier morphs list
// vertex indices whose absolute positions follow in the mod-vertex block and are turned into deltas.
// Throws nm::Error on anything malformed, including trailing or missing bytes.
TriFile ReadTri(const std::string& path);

} // namespace nm
