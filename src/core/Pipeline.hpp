#pragma once
// The whole Go! path, shared by the CLI and the GUI so both produce identical output.
#include "Model.hpp"

namespace nm {

struct RunOptions {
	MergeRequest request;
	std::string fbxPath;
	std::string objPath; // optional debug dump
};

struct RunResult {
	std::string fbxPath;
	std::string sidecarPath;
	size_t materials = 0, vertices = 0, triangles = 0, bones = 0;
};

RunResult RunPipeline(const RunOptions& opt, const LogFn& log);

} // namespace nm
