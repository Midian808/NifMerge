#pragma once
// SkyAnim's whole export run, shared by the CLI and the GUI: any mix of files and folders goes in,
// is sorted by content (skeleton.nif, skeleton .hkx, clips, other), and every clip is exported on its
// own so one bad clip never stops the rest.
#include "core/Model.hpp"

#include <string>
#include <vector>

namespace nm {

struct BatchRequest {
	std::vector<std::string> inputs; // files and folders (folders are searched recursively)
	std::string skeletonNif;         // override; empty: taken from the inputs
	std::string skeletonHkx;         // override; empty: taken from the inputs, else beside the .nif
	std::string outDir;
};

struct BatchInputs {
	struct Clip {
		std::string path;
		std::string relDir; // folder below the dropped folder it was found in; empty for loose files
	};
	std::vector<std::string> nifs;
	std::vector<std::string> skeletons; // .hkx files holding a skeleton
	std::vector<Clip> clips;
	std::vector<std::pair<std::string, std::string>> skipped; // file, what it holds
};

// Expands folders and sorts every file by what it contains.
BatchInputs ClassifyInputs(const std::vector<std::string>& inputs, const LogFn& log);

struct BatchResult {
	struct Item {
		std::string clip;    // source .hkx
		std::string fbx;     // output, empty when failed
		std::string error;   // why it failed, empty when exported
		std::vector<std::string> warnings;
	};
	std::string skeletonNif, skeletonHkx, reportPath;
	std::vector<Item> items;
	std::vector<std::pair<std::string, std::string>> skipped;
	size_t exported() const;
	size_t failed() const;
};

// Throws when the skeleton cannot be settled (missing, ambiguous, unreadable): no clip is written then.
BatchResult RunBatch(const BatchRequest& req, const LogFn& log);

} // namespace nm
