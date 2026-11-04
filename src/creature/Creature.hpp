#pragma once
// SkyCreature's whole export run, shared by the CLI and the GUI: a creature's skeleton (.nif + .hkx),
// its model parts and its animation clips go in as any mix of files and folders, sorted by content;
// out come one skinned model FBX and one FBX per clip, all rigged the same way for Unity.
#include "anim/Batch.hpp"
#include "core/Pipeline.hpp"

#include <string>
#include <vector>

namespace nm {

using CreatureRig = BatchRequest::Rig; // Auto: Humanoid when the skeleton maps onto one, else Generic

struct CreatureRequest {
	std::vector<std::string> inputs; // files and folders (folders are searched recursively for .hkx)
	std::vector<std::string> parts;  // model parts given explicitly, after those among the inputs
	std::string skeletonNif;         // override; empty: the shapeless .nif among the inputs
	std::string skeletonHkx;         // override; empty: the skeleton .hkx among the inputs, else beside the .nif
	CreatureRig rig = CreatureRig::Auto;
	std::string name;                // output name; empty: the first part's file name
	std::string outDir;
};

// The inputs sorted by content. A .nif given as a file is the skeleton when it holds no shapes, else a
// model part; .nif files inside folders are skipped (a creature's folder holds every variant of its
// model, which must not all merge into one).
struct CreatureInputs {
	std::vector<std::string> skeletonNifs;
	std::vector<std::string> parts;
	std::vector<std::string> skeletonHkxs;
	std::vector<std::string> clipInputs; // clip files and folders holding clips, as given
	size_t clips = 0;                    // clips found among them
	std::vector<std::pair<std::string, std::string>> skipped; // file, why
};
CreatureInputs ClassifyCreatureInputs(const std::vector<std::string>& inputs, const LogFn& log);

struct CreatureResult {
	std::string skeletonNif, skeletonHkx;
	Rig rig = Rig::Generic;
	RunResult model;
	bool hasClips = false;
	BatchResult clips; // when hasClips
	bool failed() const { return hasClips && clips.failed() > 0; }
};

// Throws when the skeleton or the model cannot be made: nothing is written then. A failing clip is
// reported in the result and the other clips are still exported.
CreatureResult RunCreature(const CreatureRequest& req, const LogFn& log);

} // namespace nm
