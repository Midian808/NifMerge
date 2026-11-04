#include "Creature.hpp"

#include "anim/ClipPose.hpp"
#include "anim/HkxAnimation.hpp"
#include "core/Humanoid.hpp"
#include "core/Merge.hpp"
#include "core/NifIO.hpp"

#include <algorithm>
#include <filesystem>

namespace nm {

namespace fs = std::filesystem;

namespace {

std::string LowerExt(const fs::path& p) {
	std::string e = p.extension().u8string();
	for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return e;
}

std::string Name(const std::string& path) { return fs::u8path(path).filename().u8string(); }

bool Same(const std::string& a, const std::string& b) {
	return fs::u8path(a).lexically_normal() == fs::u8path(b).lexically_normal();
}

void AddUnique(std::vector<std::string>& list, const std::string& p) {
	for (auto& e : list)
		if (Same(e, p)) return;
	list.push_back(p);
}

// What a .hkx holds; an unreadable file counts as a clip, which then fails with its reason.
HkxKind KindOf(const std::string& path) {
	try {
		return Classify(HkxPackfile::Load(path));
	} catch (const std::exception&) {
		return HkxKind::Animation;
	}
}

std::string Choose(const std::vector<std::string>& found, const std::string& what) {
	std::string names;
	for (auto& f : found) names += "\n  " + f;
	throw Error("More than one " + what + " in the selection; choose one:" + names);
}

} // namespace

CreatureInputs ClassifyCreatureInputs(const std::vector<std::string>& inputs, const LogFn& log) {
	CreatureInputs in;
	for (auto& input : inputs) {
		const fs::path p = fs::u8path(input);
		std::error_code ec;
		if (fs::is_directory(p, ec)) {
			std::vector<fs::path> files;
			for (auto& e : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec))
				if (e.is_regular_file(ec)) files.push_back(e.path());
			std::sort(files.begin(), files.end());
			size_t clipsHere = 0;
			for (auto& f : files) {
				const std::string ext = LowerExt(f), path = f.u8string();
				if (ext == ".nif") {
					in.skipped.push_back({path, "a .nif inside a folder (give model parts and the skeleton as files)"});
				} else if (ext == ".hkx") {
					switch (KindOf(path)) {
						case HkxKind::Skeleton: AddUnique(in.skeletonHkxs, path); break;
						case HkxKind::Animation: clipsHere++; break;
						default: in.skipped.push_back({path, "neither a skeleton nor an animation"});
					}
				}
			}
			if (clipsHere) {
				AddUnique(in.clipInputs, input);
				in.clips += clipsHere;
			}
		} else if (fs::is_regular_file(p, ec)) {
			const std::string ext = LowerExt(p);
			if (ext == ".nif") {
				// Shapes with geometry make a model part; a skeleton file has none.
				if (ReadShapeSlots(input).empty()) AddUnique(in.skeletonNifs, input);
				else AddUnique(in.parts, input);
			} else if (ext == ".hkx") {
				switch (KindOf(input)) {
					case HkxKind::Skeleton: AddUnique(in.skeletonHkxs, input); break;
					case HkxKind::Animation:
						AddUnique(in.clipInputs, input);
						in.clips++;
						break;
					default: in.skipped.push_back({input, "neither a skeleton nor an animation"});
				}
			} else {
				in.skipped.push_back({input, "not a .nif, .hkx or folder"});
			}
		} else {
			throw Error("Not found: " + input);
		}
	}
	for (auto& [file, why] : in.skipped) log(LogLevel::Info, "Skipped " + file + ": " + why);
	return in;
}

CreatureResult RunCreature(const CreatureRequest& req, const LogFn& log) {
	if (req.outDir.empty()) throw Error("No output folder given");
	CreatureInputs in = ClassifyCreatureInputs(req.inputs, log);
	for (auto& p : req.parts) AddUnique(in.parts, p);
	CreatureResult res;

	// Skeleton: the override, else the one shapeless .nif among the inputs.
	if (!req.skeletonNif.empty()) res.skeletonNif = req.skeletonNif;
	else if (in.skeletonNifs.size() == 1) res.skeletonNif = in.skeletonNifs[0];
	else if (in.skeletonNifs.size() > 1) Choose(in.skeletonNifs, "skeleton .nif");
	if (res.skeletonNif.empty()) throw Error("No skeleton .nif given (a .nif without shapes, e.g. skeleton.nif)");
	if (in.parts.empty()) throw Error("No model parts given (the creature's .nif files with shapes, given as files)");

	// Havok skeleton, needed for the clips: the override, else the one among the inputs (several: the one
	// named like the .nif, beside it), else the one beside the .nif.
	const fs::path nifPath = fs::u8path(res.skeletonNif);
	const std::vector<fs::path> beside = {nifPath.parent_path() / (nifPath.stem().u8string() + ".hkx"),
										  nifPath.parent_path() / "skeleton.hkx"};
	if (!req.skeletonHkx.empty()) {
		res.skeletonHkx = req.skeletonHkx;
	} else if (in.skeletonHkxs.size() == 1) {
		res.skeletonHkx = in.skeletonHkxs[0];
	} else if (in.skeletonHkxs.size() > 1) {
		for (auto& h : in.skeletonHkxs)
			if (Same(h, beside[0].u8string())) res.skeletonHkx = h;
		if (res.skeletonHkx.empty()) Choose(in.skeletonHkxs, "skeleton .hkx");
	} else {
		for (auto& c : beside)
			if (fs::is_regular_file(c)) {
				res.skeletonHkx = c.u8string();
				break;
			}
	}
	res.hasClips = in.clips > 0;
	if (res.hasClips && res.skeletonHkx.empty())
		throw Error("No skeleton .hkx given for the animations, and none beside " + res.skeletonNif);

	Skeleton skel = LoadSkeleton(res.skeletonNif);

	// Every node the Havok skeleton animates stays in the model, so each clip's curves have a bone to drive
	// (attachment and effect nodes, the tongue, ...).
	std::vector<std::string> keepBones;
	if (!res.skeletonHkx.empty()) {
		std::vector<HkxSkeleton> skeletons = ReadSkeletons(HkxPackfile::Load(res.skeletonHkx));
		if (skeletons.empty()) throw Error(res.skeletonHkx + ": holds no skeleton");
		const HkxSkeleton& body = BodySkeleton(skeletons, "", res.skeletonHkx);
		for (size_t i = 1; i < skel.bones.size(); i++) {
			const int h = body.find(skel.bones[i].name);
			if (h < 0) continue;
			keepBones.push_back(skel.bones[i].name);
		}
	}

	// Rig: Humanoid when every required Humanoid bone is there (or when asked, which fails otherwise).
	{
		const std::vector<std::string> missing = MissingHumanoidBones(skel);
		if (req.rig == CreatureRig::Humanoid) res.rig = Rig::Humanoid; // MergeModel names what is missing
		else if (req.rig == CreatureRig::Generic) res.rig = Rig::Generic;
		else res.rig = missing.empty() ? Rig::Humanoid : Rig::Generic;
		log(LogLevel::Info, std::string("Rig: ") + (res.rig == Rig::Humanoid ? "Humanoid" : "Generic") +
								(req.rig == CreatureRig::Auto
									 ? (missing.empty() ? " (the skeleton has every Humanoid bone)"
														: " (the skeleton has no Humanoid " + missing.front().substr(0, missing.front().find(' ')) + ")")
									 : " (as chosen)"));
	}

	// The model. A failure here stops the run before any clip is written.
	const std::string name = !req.name.empty() ? req.name : fs::u8path(in.parts.front()).stem().u8string();
	RunOptions opt;
	opt.request.skeletonPath = res.skeletonNif;
	opt.request.rig = res.rig;
	opt.request.parts = PairPartFiles(in.parts);
	opt.request.keepBones = keepBones;
	opt.fbxPath = (fs::u8path(req.outDir) / fs::u8path(name + ".fbx")).u8string();
	opt.generator = "SkyCreature";
	res.model = RunPipeline(opt, log);

	// The clips, rigged like the model.
	if (res.hasClips) {
		BatchRequest batch;
		batch.inputs = in.clipInputs;
		batch.skeletonNif = res.skeletonNif;
		batch.skeletonHkx = res.skeletonHkx;
		batch.outDir = (fs::u8path(req.outDir) / "anims").u8string();
		batch.rig = res.rig == Rig::Humanoid ? CreatureRig::Humanoid : CreatureRig::Generic;
		res.clips = RunBatch(batch, log);
	} else {
		log(LogLevel::Info, "No animation clips given; wrote the model only");
	}
	return res;
}

} // namespace nm
