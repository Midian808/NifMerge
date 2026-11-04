// SkyCreature tests. Fixtures: samples/Creatures/{Wolf,Horse,Dragon,Draugr} (the user's vanilla Skyrim SE
// creature extracts: skeleton.nif, skeleton.hkx, model NIFs, clips; required). A missing fixture is a failure.
#include "anim/AnimExport.hpp"
#include "anim/ClipPose.hpp"
#include "core/FbxCommon.hpp"
#include "core/Humanoid.hpp"
#include "core/Merge.hpp"
#include "core/NifIO.hpp"
#include "creature/Creature.hpp"

#include <NifFile.hpp>
#include <fbxsdk.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace nm;

namespace {

int g_failures = 0, g_checks = 0;
std::string g_test;

#define CHECK(cond, ...)                                                                              \
	do {                                                                                              \
		g_checks++;                                                                                   \
		if (!(cond)) {                                                                                \
			g_failures++;                                                                             \
			std::printf("  FAIL [%s] %s:%d: %s ", g_test.c_str(), __FILE__, __LINE__, #cond);         \
			std::printf(__VA_ARGS__);                                                                 \
			std::printf("\n");                                                                        \
		}                                                                                             \
	} while (0)

const std::string kRoot = NIFMERGE_ROOT;
std::string Cr(const std::string& rel) { return kRoot + "/samples/Creatures/" + rel; }
std::string Tmp(const std::string& rel) {
	fs::create_directories(kRoot + "/.tmp/tests/creature");
	return kRoot + "/.tmp/tests/creature/" + rel;
}

void Quiet(LogLevel, const std::string&) {}

std::vector<std::pair<std::string, std::function<void()>>>& Registry() {
	static std::vector<std::pair<std::string, std::function<void()>>> r;
	return r;
}
struct Reg {
	Reg(const char* n, std::function<void()> f) { Registry().push_back({n, f}); }
};
#define TEST(name)               \
	void name();                 \
	Reg reg_##name(#name, name); \
	void name()

const double kDeg = 180.0 / 3.14159265358979323846;

std::string Read(const std::string& path) {
	std::ifstream f(fs::u8path(path), std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// The four sample creatures: folder, model parts, clip folder, and what each one is.
struct Creature {
	const char* folder;
	std::vector<const char*> parts;
	const char* anims;
	Rig rig;
	size_t havokBones;
	size_t shapes;
};

const std::vector<Creature>& Creatures() {
	static const std::vector<Creature> c = {
		{"Wolf", {"wolf.nif"}, "anims", Rig::Generic, 50, 2},
		{"Horse", {"horse.nif", "horsemane.nif", "saddle.nif"}, "animations", Rig::Generic, 53, 7},
		{"Dragon", {"dragon.nif"}, "anims", Rig::Generic, 84, 2},
		{"Draugr", {"draugrmale01.nif", "helmet01.nif"}, "animation", Rig::Humanoid, 84, 7},
	};
	return c;
}

const Creature& Find(const std::string& folder) {
	for (auto& c : Creatures())
		if (folder == c.folder) return c;
	throw Error("no creature " + folder);
}

// Every sample clip: does it loop, how many Havok bones it drives. Measured by hand on the decoded clips
// (tests/hkx_oracle): cycles wrap with a jump of at most one ordinary frame step on every bone; the takeoff
// and the phoneme do not come back to their first pose; the bite does (it ends where it started).
struct ClipFact {
	const char* creature;
	const char* clip;
	bool loops;
	int driven;
	bool rootMotion;
};

const std::vector<ClipFact>& Clips() {
	static const std::vector<ClipFact> c = {
		{"Wolf", "mt_idle_wolf", true, 50, false},      {"Wolf", "runforward", true, 50, false},
		{"Wolf", "trotforwardr", true, 50, false},      {"Wolf", "walkforward_wolf", true, 50, false},
		{"Wolf", "walkforwardl_wolf", true, 50, false}, {"Wolf", "wolf_idle_spice", true, 50, false},
		{"Horse", "idle", true, 53, false},             {"Horse", "idle_headshake", true, 19, false},
		{"Horse", "runforward", true, 53, false},       {"Horse", "trotforward", true, 53, false},
		{"Horse", "walkforward", true, 53, false},      {"Dragon", "ground_bite", true, 83, false},
		{"Dragon", "mtforwardground", true, 83, false}, {"Dragon", "mtidle_hover", true, 83, false},
		{"Dragon", "mttakeoffvertical", false, 83, true}, {"Dragon", "phoneme_bigaah", false, 7, false},
		{"Draugr", "1hmforward", true, 83, false},      {"Draugr", "1hmidle", true, 84, false},
		{"Draugr", "bowdrawnforward", true, 84, false}, {"Draugr", "mtforward", true, 83, false},
		{"Draugr", "mtidle", true, 84, false},
	};
	return c;
}

CreatureRequest RequestFor(const Creature& c, const std::string& out) {
	CreatureRequest r;
	r.inputs.push_back(Cr(std::string(c.folder) + "/skeleton.nif"));
	for (auto& p : c.parts) r.inputs.push_back(Cr(std::string(c.folder) + "/" + p));
	r.inputs.push_back(Cr(std::string(c.folder) + "/" + c.anims));
	r.outDir = out;
	return r;
}

// One SkyCreature run per creature, shared by the tests that read its output.
const CreatureResult& Run(const Creature& c) {
	static std::map<std::string, CreatureResult> done;
	auto it = done.find(c.folder);
	if (it != done.end()) return it->second;
	const std::string out = Tmp(std::string("run_") + c.folder);
	fs::remove_all(fs::u8path(out));
	return done[c.folder] = RunCreature(RequestFor(c, out), Quiet);
}

struct PosedFixture {
	HkxClip clip;
	std::vector<HkxSkeleton> skeletons;
	const HkxSkeleton* body = nullptr;
	ClipBinding binding;
	Skeleton nif;
};

PosedFixture LoadClip(const Creature& c, const std::string& clip) {
	PosedFixture p;
	p.clip = ReadClip(HkxPackfile::Load(Cr(std::string(c.folder) + "/" + c.anims + "/" + clip + ".hkx")));
	p.skeletons = ReadSkeletons(HkxPackfile::Load(Cr(std::string(c.folder) + "/skeleton.hkx")));
	p.body = &BodySkeleton(p.skeletons, p.clip.skeletonName, "skeleton.hkx");
	p.binding = BindClip(p.clip, *p.body);
	p.nif = LoadSkeleton(Cr(std::string(c.folder) + "/skeleton.nif"));
	return p;
}

struct FbxRead {
	FbxManager* mgr = FbxManager::Create();
	FbxScene* scene = nullptr;
	explicit FbxRead(const std::string& path) {
		mgr->SetIOSettings(FbxIOSettings::Create(mgr, IOSROOT));
		FbxImporter* imp = FbxImporter::Create(mgr, "");
		if (!imp->Initialize(path.c_str(), -1, mgr->GetIOSettings())) throw Error("cannot read back " + path);
		scene = FbxScene::Create(mgr, "read");
		imp->Import(scene);
		imp->Destroy();
	}
	~FbxRead() { mgr->Destroy(); }
};

// ---- Rig -----------------------------------------------------------------------------------------

TEST(RigDetection) {
	for (auto& c : Creatures()) {
		Skeleton s = LoadSkeleton(Cr(std::string(c.folder) + "/skeleton.nif"));
		auto missing = MissingHumanoidBones(s);
		CHECK(missing.empty() == (c.rig == Rig::Humanoid), "%s: %zu Humanoid bones missing", c.folder, missing.size());
	}
	// The vanilla draugr spells its left foot "[LLft ]"; the alias maps it, and the human skeleton keeps its own.
	const HumanoidBone* leftFoot = nullptr;
	for (auto& h : HumanoidMap())
		if (std::string(h.unity) == "LeftFoot") leftFoot = &h;
	CHECK(leftFoot && leftFoot->alias, "LeftFoot has an alias");
	Skeleton draugr = LoadSkeleton(Cr("Draugr/skeleton.nif"));
	Skeleton human = LoadSkeleton(kRoot + "/samples/skeleton_female.nif");
	if (leftFoot) {
		int d = FindHumanoidBone(draugr, *leftFoot), h = FindHumanoidBone(human, *leftFoot);
		CHECK(d >= 0 && draugr.bones[d].name == "NPC L Foot [LLft ]", "draugr left foot '%s'", d >= 0 ? draugr.bones[d].name.c_str() : "-");
		CHECK(h >= 0 && human.bones[h].name == "NPC L Foot [Lft ]", "human left foot '%s'", h >= 0 ? human.bones[h].name.c_str() : "-");
	}
	CHECK(MissingHumanoidBones(human).empty(), "the human skeleton still maps");
}

// ---- Models --------------------------------------------------------------------------------------

TEST(ModelsMerge) {
	for (auto& c : Creatures()) {
		MergeRequest req;
		req.skeletonPath = Cr(std::string(c.folder) + "/skeleton.nif");
		req.rig = c.rig;
		for (auto& p : c.parts) req.parts.push_back({Cr(std::string(c.folder) + "/" + p), std::nullopt});
		MergedModel m = MergeModel(req, Quiet);
		CHECK(m.rig == c.rig && m.materials.size() == c.shapes, "%s: %zu materials", c.folder, m.materials.size());
		// FBX reads ':' as a namespace separator (Unity would import "HorseBody:0" as "0"): names carry '_'.
		for (auto& mat : m.materials)
			CHECK(mat.name.find(':') == std::string::npos, "%s: material '%s' (shape '%s') has no ':'", c.folder, mat.name.c_str(),
				  mat.shapeName.c_str());
		if (std::string(c.folder) == "Horse")
			CHECK(m.materials[0].name == "HorseBody_0" && m.materials[0].shapeName == "HorseBody:0", "horse slot 0 '%s' for shape '%s'",
				  m.materials[0].name.c_str(), m.materials[0].shapeName.c_str());
		size_t human = std::count_if(m.bones.begin(), m.bones.end(), [](auto& b) { return !b.humanoid.empty(); });
		if (c.rig == Rig::Generic) CHECK(human == 0, "%s: Generic model maps no Humanoid bone (%zu)", c.folder, human);
		else CHECK(human >= 20, "%s: %zu Humanoid bones", c.folder, human);
		bool weightsOk = !m.weights.empty();
		for (auto& w : m.weights) {
			double sum = 0;
			for (auto& p : w) sum += p.second, weightsOk = weightsOk && p.first >= 0 && p.first < static_cast<int>(m.bones.size());
			weightsOk = weightsOk && std::fabs(sum - 1.0) < 1e-3;
		}
		CHECK(weightsOk, "%s: every vertex weighted to output bones, weights sum to 1", c.folder);
		double lo = 1e9, hi = -1e9;
		size_t lowest = 0;
		for (size_t v = 0; v < m.positions.size(); v++) {
			if (m.positions[v].z < lo) lowest = v;
			lo = std::min(lo, m.positions[v].z), hi = std::max(hi, m.positions[v].z);
		}
		std::printf("  INFO %s: %zu vertices, %zu bones, height %.1f .. %.1f units, lowest vertex on '%s'\n", c.folder, m.positions.size(),
					m.bones.size(), lo, hi, m.bones[m.weights[lowest][0].first].name.c_str());
		// Standing at its rest pose: the lowest vertex is on a foot, within 10 units of the floor (the
		// dragon's claws curl 9.3 units below its root).
		const std::string low = m.bones[m.weights[lowest][0].first].name;
		const bool onFoot = low.find("Toe") != std::string::npos || low.find("Hoof") != std::string::npos || low.find("Foot") != std::string::npos;
		CHECK(onFoot && lo > -10.0 && lo < 3.0 && hi > 40.0, "%s: model spans %.1f .. %.1f units, lowest on '%s'", c.folder, lo, hi, low.c_str());
	}
	// The draugr's Humanoid left foot is its "[LLft ]" bone.
	MergeRequest req;
	req.skeletonPath = Cr("Draugr/skeleton.nif");
	req.parts.push_back({Cr("Draugr/draugrmale01.nif"), std::nullopt});
	MergedModel m = MergeModel(req, Quiet);
	bool foot = false;
	for (auto& b : m.bones) foot = foot || (b.humanoid == "LeftFoot" && b.name == "NPC L Foot [LLft ]");
	CHECK(foot, "draugr LeftFoot -> NPC L Foot [LLft ]");
	// Humanoid asked for a creature that has no Humanoid skeleton: refused, naming the missing bones.
	MergeRequest wolf;
	wolf.skeletonPath = Cr("Wolf/skeleton.nif");
	wolf.parts.push_back({Cr("Wolf/wolf.nif"), std::nullopt});
	std::string error;
	try {
		MergeModel(wolf, Quiet);
	} catch (const Error& e) {
		error = e.what();
	}
	CHECK(error.find("Hips -> 'NPC COM [COM ]'") != std::string::npos, "wolf as Humanoid refused: %s", error.c_str());
}

// The horse's shapes are bound exactly at the skeleton's rest pose (every bone implies the identity
// skin -> world transform), so the re-posed vertices must equal the file's own vertex positions.
TEST(HorseRestPoseIsTheFilesGeometry) {
	MergeRequest req;
	req.skeletonPath = Cr("Horse/skeleton.nif");
	req.rig = Rig::Generic;
	req.parts.push_back({Cr("Horse/horse.nif"), std::nullopt});
	MergedModel m = MergeModel(req, Quiet);
	nifly::NifFile nif;
	CHECK(nif.Load(fs::u8path(Cr("Horse/horse.nif"))) == 0, "horse.nif loads");
	double worst = 0;
	size_t compared = 0;
	for (size_t s = 0; s < m.materials.size(); s++) {
		nifly::NiShape* shape = nullptr;
		for (auto* sh : nif.GetShapes())
			if (sh->name.get() == m.materials[s].shapeName) shape = sh;
		const std::vector<nifly::Vector3>* verts = shape ? nif.GetVertsForShape(shape) : nullptr;
		if (!verts) continue;
		auto [first, count] = m.materialVertices[s];
		CHECK(verts->size() == count, "%s: %zu vertices in the file, %u merged", m.materials[s].shapeName.c_str(), verts->size(), count);
		for (uint32_t v = 0; v < count && v < verts->size(); v++) {
			const Vec3& p = m.positions[first + v];
			const auto& q = (*verts)[v];
			worst = std::max({worst, std::fabs(p.x - q.x), std::fabs(p.y - q.y), std::fabs(p.z - q.z)});
			compared++;
		}
	}
	CHECK(compared > 4000 && worst < 0.05, "%zu vertices compared, worst %.4f units", compared, worst);
}

// ---- Clips ---------------------------------------------------------------------------------------

TEST(EveryClipExports) {
	for (auto& c : Creatures()) {
		const CreatureResult& r = Run(c);
		CHECK(r.rig == c.rig && r.hasClips, "%s: rig and clips", c.folder);
		CHECK(r.clips.failed() == 0, "%s: %zu clips failed", c.folder, r.clips.failed());
		CHECK(fs::exists(fs::u8path(r.model.fbxPath)) && fs::exists(fs::u8path(r.model.sidecarPath)), "%s: model written", c.folder);
		const std::string side = Read(r.model.sidecarPath);
		CHECK(side.find("\"generator\": \"SkyCreature\"") != std::string::npos && side.find("\"formatVersion\": 3") != std::string::npos,
			  "%s: model sidecar generator and version", c.folder);
		CHECK(side.find(std::string("\"rig\": \"") + RigName(c.rig) + "\"") != std::string::npos, "%s: model sidecar rig", c.folder);
	}
	for (auto& f : Clips()) {
		const CreatureResult& r = Run(Find(f.creature));
		const BatchResult::Item* item = nullptr;
		for (auto& i : r.clips.items)
			if (fs::u8path(i.clip).stem().u8string() == f.clip) item = &i;
		CHECK(item && item->error.empty(), "%s/%s exported", f.creature, f.clip);
		if (!item || !item->error.empty()) continue;
		const std::string json = Read(AnimSidecarPathFor(item->fbx));
		auto has = [&](const std::string& s) { return json.find(s) != std::string::npos; };
		CHECK(has(std::string("\"loop\": ") + (f.loops ? "true" : "false")), "%s/%s: loop %d", f.creature, f.clip, f.loops);
		CHECK(has("\"drivenBones\": " + std::to_string(f.driven) + ",") &&
				  has("\"skeletonBones\": " + std::to_string(Find(f.creature).havokBones) + ","),
			  "%s/%s: driven bones %d of %zu", f.creature, f.clip, f.driven, Find(f.creature).havokBones);
		CHECK(has(std::string("\"rootMotion\": ") + (f.rootMotion ? "true" : "false")), "%s/%s: root motion %d", f.creature, f.clip, f.rootMotion);
		CHECK(has(std::string("\"rig\": \"") + RigName(Find(f.creature).rig) + "\""), "%s/%s: clip rig = the model's", f.creature, f.clip);
	}
}

// The dragon's takeoff: the game moves the actor by the reference frame (rising 2048 units), and the bones
// are posed relative to it. Every bone's world transform must be the motion composed over the clip's own
// pose, the root bone must carry the rise, and the FBX must keep it.
TEST(DragonTakeoffRootMotion) {
	const Creature& dragon = Find("Dragon");
	PosedFixture f = LoadClip(dragon, "mttakeoffvertical");
	CHECK(f.clip.referenceFrameMoves && f.clip.referenceSamples.size() == static_cast<size_t>(f.clip.numFrames), "%zu samples, %d frames",
		  f.clip.referenceSamples.size(), f.clip.numFrames);
	CHECK(std::fabs(f.clip.referenceSamples.back()[2] - 2048.0) < 1e-3, "rises to %.3f", f.clip.referenceSamples.back()[2]);
	PosedClip p = PoseClip(f.clip, f.binding, *f.body, f.nif);
	HkxClip still = f.clip;
	still.referenceFrameMoves = false;
	PosedClip q = PoseClip(still, f.binding, *f.body, f.nif);
	CHECK(p.rootMotion && p.motion.size() == p.frames.size() && !q.rootMotion && q.motion.empty(), "root motion, %zu frames of it",
		  p.motion.size());
	double worst = 0;
	const Xform w0 = f.nif.bones[0].world;
	for (size_t fr = 0; fr < p.frames.size(); fr++) {
		const auto& s = f.clip.referenceSamples[fr];
		Xform m;
		const double cs = std::cos(s[3]), sn = std::sin(s[3]);
		m.L.m[0] = {cs, -sn, 0};
		m.L.m[1] = {sn, cs, 0};
		m.t = {s[0], s[1], s[2]};
		const Xform motion = w0 * m * w0.inverse();
		for (size_t b = 0; b < p.bones.size(); b++) worst = std::max(worst, p.frames[fr][b].maxDiff((motion * q.frames[fr][b]).rigid()));
	}
	CHECK(worst < 1e-6, "every bone = motion x pose (worst %g)", worst);
	int root = -1, com = -1;
	for (size_t b = 0; b < p.bones.size(); b++) {
		if (p.bones[b].name == "NPC Root [Root]") root = static_cast<int>(b);
		if (p.bones[b].name == "NPC COM") com = static_cast<int>(b);
	}
	CHECK(root >= 0 && com >= 0, "root and COM in the clip");
	if (root < 0 || com < 0) return;
	const double rise = p.frames.back()[root].t.z - p.frames.front()[root].t.z;
	CHECK(std::fabs(rise - 2048.0) < 1e-3, "root rises %.3f units", rise);
	// The body ends where it started relative to the root, i.e. 2048 units up in the world.
	const double comEnd = p.frames.back()[com].t.z - p.frames.back()[root].t.z;
	CHECK(std::fabs(comEnd - (q.frames.back()[com].t.z - q.frames.back()[root].t.z)) < 1e-6, "COM relative to the root unchanged by the motion");

	// FBX: the root node carries the rise (Y up, metres).
	const CreatureResult& r = Run(dragon);
	std::string fbx;
	for (auto& i : r.clips.items)
		if (fs::u8path(i.clip).stem().u8string() == "mttakeoffvertical") fbx = i.fbx;
	FbxRead in(fbx);
	in.scene->SetCurrentAnimationStack(in.scene->GetSrcObject<FbxAnimStack>(0));
	FbxNode* n = in.scene->FindNodeByName("NPC Root [Root]");
	CHECK(n != nullptr, "root node in the FBX");
	if (!n) return;
	FbxTime t0, t1;
	t1.SetSecondDouble((p.frames.size() - 1) * p.frameDuration);
	const double up = n->EvaluateGlobalTransform(t1).GetT()[1] - n->EvaluateGlobalTransform(t0).GetT()[1];
	CHECK(std::fabs(up - 2048.0 * kMetresPerUnit) < 1e-4, "FBX root rises %.4f m (29.26 expected)", up);
	// The sidecar carries the motion per frame for the Unity package, FBX space (Y up, metres).
	const std::string side = Read(AnimSidecarPathFor(fbx));
	size_t entries = 0;
	for (size_t at = side.find("{\"t\": ["); at != std::string::npos; at = side.find("{\"t\": [", at + 1)) entries++;
	const size_t last = side.rfind("{\"t\": [");
	double tx = 0, ty = 0, tz = 0;
	std::sscanf(side.c_str() + last, "{\"t\": [%lf, %lf, %lf]", &tx, &ty, &tz);
	CHECK(entries == p.frames.size() && std::fabs(ty - 2048.0 * kMetresPerUnit) < 1e-4 && std::fabs(tx) < 1e-6 && std::fabs(tz) < 1e-6,
		  "sidecar motion: %zu frames, last (%g, %g, %g) m", entries, tx, ty, tz);
	FbxNode* top = in.scene->GetRootNode()->GetChild(0);
	CHECK(top && std::string(top->GetName()) == "NPC Root [Root]", "the dragon's root bone is the top node");
}

// A clip without root motion leaves the root bone where the clip has it.
TEST(NoRootMotionWithoutReferenceFrame) {
	PosedFixture f = LoadClip(Find("Wolf"), "runforward");
	PosedClip p = PoseClip(f.clip, f.binding, *f.body, f.nif);
	CHECK(!p.rootMotion && p.motion.empty(), "no root motion");
}

// A skeleton.nif that is not the .hkx's skeleton is named as such, not exported as an empty clip.
TEST(MismatchedSkeletonNamed) {
	PosedFixture f = LoadClip(Find("Wolf"), "runforward");
	Skeleton horse = LoadSkeleton(Cr("Horse/skeleton.nif"));
	std::string error;
	try {
		PoseClip(f.clip, f.binding, *f.body, horse);
	} catch (const Error& e) {
		error = e.what();
	}
	CHECK(error.find("is not the skeleton of") != std::string::npos, "%s", error.c_str());
	// The right skeleton has a node for every bone the clip animates.
	CHECK(PoseClip(f.clip, f.binding, *f.body, f.nif).missingNodes.empty(), "wolf clip on the wolf skeleton: no missing nodes");
}

// ---- Inputs --------------------------------------------------------------------------------------

TEST(InputsSortedByContent) {
	CreatureInputs in = ClassifyCreatureInputs({Cr("Wolf/skeleton.nif"), Cr("Wolf/wolf.nif"), Cr("Wolf")}, Quiet);
	CHECK(in.skeletonNifs.size() == 1 && in.parts.size() == 1, "%zu skeletons, %zu parts", in.skeletonNifs.size(), in.parts.size());
	CHECK(in.skeletonHkxs.size() == 1 && fs::u8path(in.skeletonHkxs[0]).filename() == "skeleton.hkx", "the folder's skeleton.hkx");
	CHECK(in.clips == 6 && in.clipInputs.size() == 1, "%zu clips in %zu inputs", in.clips, in.clipInputs.size());
	size_t nifsSkipped = 0, behavior = 0;
	for (auto& [file, why] : in.skipped) {
		if (fs::u8path(file).extension() == ".nif") nifsSkipped++;
		if (fs::u8path(file).filename() == "wolf.hkx") behavior++;
	}
	CHECK(nifsSkipped == 6 && behavior == 1, "folder NIFs skipped %zu, behavior file skipped %zu", nifsSkipped, behavior);

	// Several Havok skeletons (the draugr folder holds three): the one named like the skeleton .nif.
	const std::string out = Tmp("skeletons");
	fs::remove_all(fs::u8path(out));
	CreatureRequest req;
	req.inputs = {Cr("Draugr/skeletons.nif"), Cr("Draugr/draugrskeleton.nif"), Cr("Draugr")};
	req.outDir = out;
	CreatureResult r = RunCreature(req, Quiet);
	CHECK(fs::u8path(r.skeletonHkx).filename() == "skeletons.hkx", "picked %s", r.skeletonHkx.c_str());
	CHECK(r.rig == Rig::Humanoid && fs::exists(fs::u8path(out) / "draugrskeleton.fbx"), "the skeleton enemy is written");

	// Two skeleton .nif files: the run stops before writing anything.
	const std::string stopped = Tmp("two_skeletons");
	fs::remove_all(fs::u8path(stopped));
	req.inputs = {Cr("Wolf/skeleton.nif"), Cr("Horse/skeleton.nif"), Cr("Wolf/wolf.nif")};
	req.outDir = stopped;
	std::string error;
	try {
		RunCreature(req, Quiet);
	} catch (const Error& e) {
		error = e.what();
	}
	CHECK(error.find("More than one skeleton .nif") != std::string::npos && !fs::exists(fs::u8path(stopped)), "%s", error.c_str());

	// No clips: the model alone.
	const std::string modelOnly = Tmp("model_only");
	fs::remove_all(fs::u8path(modelOnly));
	req.inputs = {Cr("Wolf/skeleton.nif"), Cr("Wolf/wolf.nif")};
	req.outDir = modelOnly;
	r = RunCreature(req, Quiet);
	CHECK(!r.hasClips && fs::exists(fs::u8path(modelOnly) / "wolf.fbx") && !fs::exists(fs::u8path(modelOnly) / "anims"), "model only");

	// Forced Generic on the draugr: the model and its clips are Generic.
	const std::string generic = Tmp("draugr_generic");
	fs::remove_all(fs::u8path(generic));
	req = RequestFor(Find("Draugr"), generic);
	req.rig = CreatureRig::Generic;
	r = RunCreature(req, Quiet);
	CHECK(r.rig == Rig::Generic && r.clips.failed() == 0, "draugr as Generic");
	CHECK(Read(r.model.sidecarPath).find("\"humanoid\": [\n\n  ]") != std::string::npos, "no Humanoid map in the Generic draugr's sidecar");
	CHECK(!r.clips.items.empty() && Read(AnimSidecarPathFor(r.clips.items[0].fbx)).find("\"rig\": \"generic\"") != std::string::npos,
		  "its clips are Generic");
}

// The model's bones and each clip's bones share their paths (that is how Generic clips bind in Unity):
// every bone both files have sits under the same chain of parent names.
TEST(ClipBonePathsMatchTheModel) {
	for (auto& c : Creatures()) {
		const CreatureResult& r = Run(c);
		auto paths = [](const std::string& fbx) {
			FbxRead in(fbx);
			std::map<std::string, std::string> out;
			for (int i = 0; i < in.scene->GetNodeCount(); i++) {
				FbxNode* n = in.scene->GetNode(i);
				if (!n->GetSkeleton()) continue;
				std::string p;
				for (FbxNode* a = n; a && a != in.scene->GetRootNode(); a = a->GetParent()) p = "/" + std::string(a->GetName()) + p;
				out[n->GetName()] = p;
			}
			return out;
		};
		auto model = paths(r.model.fbxPath);
		size_t shared = 0, differ = 0, absent = 0;
		std::string firstAbsent;
		for (auto& item : r.clips.items) {
			if (!item.error.empty()) continue;
			for (auto& [bone, path] : paths(item.fbx)) {
				auto it = model.find(bone);
				if (it == model.end()) {
					if (!absent++) firstAbsent = bone;
					continue;
				}
				shared++;
				differ += it->second != path;
			}
		}
		CHECK(shared > 0 && differ == 0, "%s: %zu shared bones, %zu with another path", c.folder, shared, differ);
		// The model keeps every bone its Havok skeleton animates, so every clip bone has one to drive.
		CHECK(absent == 0, "%s: %zu clip bones missing from the model (e.g. '%s')", c.folder, absent, firstAbsent.c_str());
	}
}

} // namespace

int main() {
	for (auto& [name, fn] : Registry()) {
		g_test = name;
		int before = g_failures;
		try {
			fn();
		} catch (const std::exception& e) {
			g_failures++;
			std::printf("  FAIL [%s] threw: %s\n", name.c_str(), e.what());
		}
		std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name.c_str());
	}
	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
