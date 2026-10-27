// SkyAnim tests. Fixtures: samples/animationwork/{LE,SSE} (the user's real Skyrim LE and SE clips and
// skeletons; required). A missing fixture is a failure.
#include "anim/AnimExport.hpp"
#include "anim/Batch.hpp"
#include "anim/ClipPose.hpp"
#include "core/FbxCommon.hpp"
#include "core/Humanoid.hpp"
#include "core/NifIO.hpp"

#include <fbxsdk.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <sstream>
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
std::string Anim(const std::string& rel) { return kRoot + "/samples/animationwork/" + rel; }
std::string Tmp(const std::string& rel) {
	fs::create_directories(kRoot + "/.tmp/tests/anim");
	return kRoot + "/.tmp/tests/anim/" + rel;
}

void Quiet(LogLevel, const std::string&) {}

std::vector<std::pair<std::string, std::function<void()>>>& Registry() {
	static std::vector<std::pair<std::string, std::function<void()>>> r;
	return r;
}
struct Reg {
	Reg(const char* n, std::function<void()> f) { Registry().push_back({n, f}); }
};
#define TEST(name)                         \
	void name();                           \
	Reg reg_##name(#name, name);           \
	void name()

const double kDeg = 180.0 / 3.14159265358979323846;

std::string Read(const std::string& path) {
	std::ifstream f(fs::u8path(path), std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// The sample clips and what is known about each one (measured independently with the Python prototype).
struct SampleClip {
	const char* platform; // folder: LE or SSE
	const char* file;
	int frames, tracks;
	double duration;
	const char* rotation; // the one rotation encoding it uses
	size_t dropped;       // tracks with no bone in the platform's skeleton_female.hkx
	bool loops;
	std::vector<std::pair<double, std::string>> events;
};

const std::vector<SampleClip>& Samples() {
	static const std::vector<SampleClip> s = {
		{"LE", "mt_idle.hkx", 2301, 242, 76.6667, "rotation THREECOMP40", 127, true, {}},
		{"LE", "mt_walkforward.HKX", 49, 107, 1.6, "rotation THREECOMP40", 8, true, {{0.83396, ""}}},
		{"LE", "mt_runforward.HKX", 25, 107, 0.8, "rotation THREECOMP40", 8, true, {{0.0, "FootLeft"}, {0.4, "FootRight"}}},
		{"LE", "bow_release.hkx", 21, 107, 0.666667, "rotation THREECOMP40", 8, false, {{0.0333333, "arrowRelease"}, {0.1, "SoundPlay.WPNBowFire"}}},
		{"SSE", "mt_idle.hkx", 901, 99, 30.0, "rotation THREECOMP48", 0, true, {}},
		{"SSE", "mt_walkforward.hkx", 35, 99, 1.13333, "rotation THREECOMP48", 0, true, {{0.283333, "FootRight"}, {0.85, "FootLeft"}}},
		{"SSE", "mt_runforward.hkx", 21, 99, 0.666667, "rotation THREECOMP48", 0, true, {{0.283333, "FootRight"}, {0.616667, "FootLeft"}}},
	};
	return s;
}

std::string SkeletonHkx(const std::string& platform) { return Anim(platform + "/skeleton_female.hkx"); }
std::string SkeletonNif(const std::string& platform) { return Anim(platform + "/skeleton_female.nif"); }

struct Loaded {
	HkxClip clip;
	std::vector<HkxSkeleton> skeletons;
	const HkxSkeleton* body = nullptr;
	ClipBinding binding;
};

Loaded Load(const SampleClip& s) {
	Loaded l;
	l.clip = ReadClip(HkxPackfile::Load(Anim(std::string(s.platform) + "/" + s.file)));
	l.skeletons = ReadSkeletons(HkxPackfile::Load(SkeletonHkx(s.platform)));
	l.body = &BodySkeleton(l.skeletons, l.clip.skeletonName, SkeletonHkx(s.platform));
	l.binding = BindClip(l.clip, *l.body);
	return l;
}

// ---- Math ----------------------------------------------------------------------------------------

TEST(QuaternionMatrixRoundTrip) {
	std::mt19937 rng(7);
	std::normal_distribution<double> n(0, 1);
	double worst = 0;
	for (int i = 0; i < 2000; i++) {
		Quat q = Quat(n(rng), n(rng), n(rng), n(rng)).normalized();
		Mat3 m = q.toMat3();
		CHECK(std::fabs(m.det() - 1) < 1e-9, "det %g", m.det());
		worst = std::max(worst, Quat::fromMat3(m).angleTo(q));
	}
	CHECK(worst < 1e-7, "worst round-trip angle %g rad", worst);
	// toMat3 rotates like the skeleton's matrices: 90 degrees about Z takes +X to +Y.
	Vec3 v = Quat(0, 0, std::sqrt(0.5), std::sqrt(0.5)).toMat3() * Vec3(1, 0, 0);
	CHECK(std::fabs(v.x) < 1e-12 && std::fabs(v.y - 1) < 1e-12, "(%g, %g, %g)", v.x, v.y, v.z);
}

// ---- Packfile and skeletons ----------------------------------------------------------------------

TEST(PackfileLayouts) {
	for (auto [platform, ptr] : {std::pair<const char*, int>{"LE", 4}, {"SSE", 8}}) {
		HkxPackfile pf = HkxPackfile::Load(SkeletonHkx(platform));
		CHECK(pf.pointerSize() == ptr, "%s pointer size %d", platform, pf.pointerSize());
		CHECK(pf.version() == "hk_2010.2.0-r1", "%s", pf.version().c_str());
		CHECK(Classify(pf) == HkxKind::Skeleton, "%s skeleton classified", platform);
	}
	HkxPackfile character = HkxPackfile::Load(Anim("LE/defaultfemale.hkx"));
	CHECK(Classify(character) == HkxKind::Other, "defaultfemale.hkx is not a skeleton or clip");
	CHECK(character.rootClass() == "hkbCharacterData", "root %s", character.rootClass().c_str());
	// Not a packfile at all.
	bool threw = false;
	try {
		HkxPackfile::Load(SkeletonNif("LE"));
	} catch (const Error&) {
		threw = true;
	}
	CHECK(threw, "a .nif is rejected as a packfile");
}

TEST(SkeletonsLeAndSe) {
	auto le = ReadSkeletons(HkxPackfile::Load(SkeletonHkx("LE")));
	auto se = ReadSkeletons(HkxPackfile::Load(SkeletonHkx("SSE")));
	CHECK(le.size() == 2 && se.size() == 2, "%zu / %zu skeletons", le.size(), se.size());
	if (le.size() != 2 || se.size() != 2) return;
	CHECK(le[0].name == "NPC Root [Root]" && le[0].bones.size() == 115, "LE body '%s' %zu", le[0].name.c_str(), le[0].bones.size());
	CHECK(le[1].name == "Ragdoll_NPC COM [COM ]" && le[1].bones.size() == 18, "LE ragdoll %zu", le[1].bones.size());
	CHECK(se[0].bones.size() == 126, "SE body %zu bones", se[0].bones.size());
	// The SE skeleton is the LE one plus 11 appended bones.
	bool prefix = true;
	double poseDiff = 0;
	for (size_t i = 0; i < 115; i++) {
		prefix = prefix && le[0].bones[i] == se[0].bones[i] && le[0].parents[i] == se[0].parents[i];
		poseDiff = std::max(poseDiff, (le[0].referencePose[i].t - se[0].referencePose[i].t).length());
		poseDiff = std::max(poseDiff, le[0].referencePose[i].r.angleTo(se[0].referencePose[i].r));
	}
	CHECK(prefix && poseDiff < 1e-6, "LE bones are SE's first 115 (pose diff %g)", poseDiff);
	CHECK(se[0].bones[115] == "Belly", "%s", se[0].bones[115].c_str());
	// Known rest values: COM height and the calf length.
	const auto& com = le[0].referencePose[le[0].find("NPC COM [COM ]")];
	CHECK(std::fabs(com.t.z - 68.9113) < 1e-3 && std::fabs(com.t.y + 0.35) < 1e-3, "COM (%g, %g, %g)", com.t.x, com.t.y, com.t.z);
	CHECK(std::fabs(le[0].referencePose[le[0].find("npc l calf [lclf]")].t.z - 35.595) < 1e-3, "case-insensitive find");
}

// ---- Clips ---------------------------------------------------------------------------------------

TEST(SampleClipsDecode) {
	for (const auto& s : Samples()) {
		Loaded l = Load(s);
		const HkxClip& c = l.clip;
		const std::string name = std::string(s.platform) + "/" + s.file;
		CHECK(c.numFrames == s.frames && c.numTracks == s.tracks, "%s: %d frames, %d tracks", name.c_str(), c.numFrames, c.numTracks);
		CHECK(std::fabs(c.duration - s.duration) < 1e-3, "%s: duration %g", name.c_str(), c.duration);
		CHECK(std::fabs(c.frameDuration - 1.0 / 30) < 1e-6, "%s: frame duration %g", name.c_str(), c.frameDuration);
		CHECK(c.pointerSize == (std::string(s.platform) == "SSE" ? 8 : 4), "%s", name.c_str());
		CHECK(c.formats.count(s.rotation) && c.formats.count("position 16-bit"), "%s: formats", name.c_str());
		CHECK(c.unverifiedFormats.empty(), "%s: every encoding is a verified one", name.c_str());
		CHECK(!c.referenceFrameMoves, "%s: no root motion", name.c_str());
		CHECK(c.events.size() == s.events.size(), "%s: %zu events", name.c_str(), c.events.size());
		for (size_t i = 0; i < std::min(c.events.size(), s.events.size()); i++)
			CHECK(std::fabs(c.events[i].time - s.events[i].first) < 1e-4 && c.events[i].text == s.events[i].second,
				  "%s: event %zu = %g '%s'", name.c_str(), i, c.events[i].time, c.events[i].text.c_str());
		// Every decoded rotation is a unit quaternion and scale is a sane positive vector somewhere.
		double worstNorm = 0;
		for (auto& f : c.frames)
			for (auto& x : f) worstNorm = std::max(worstNorm, std::fabs(std::sqrt(x.r.dot(x.r)) - 1));
		CHECK(worstNorm < 1e-9, "%s: quaternion norm error %g", name.c_str(), worstNorm);
	}
}

// Bones the samples leave in their rest orientation (static tracks) must decode to the skeleton's
// reference pose: this pins the bit layout of each rotation encoding.
TEST(StaticTracksMatchReferencePose) {
	const char* restBones[] = {"SkirtBBone02", "SkirtBBone03", "SkirtFBone02", "SkirtFBone03", "NPC L Pauldron",
							   "WeaponSword", "WeaponMace", "WeaponBow", "NPC Head MagicNode [Hmag]", "NPCEyeBone"};
	for (const auto& s : Samples()) {
		Loaded l = Load(s);
		int checked = 0;
		for (int t = 0; t < l.clip.numTracks; t++) {
			int b = l.binding.trackToBone[t];
			if (b < 0) continue;
			bool listed = false;
			for (auto* r : restBones) listed = listed || l.body->bones[b] == r;
			if (!listed) continue;
			bool still = true; // only tracks the clip holds still (some clips animate a skirt or a pauldron)
			for (auto& f : l.clip.frames) still = still && f[t].r.angleTo(l.clip.frames[0][t].r) < 1e-6;
			if (!still) continue;
			double err = l.clip.frames[0][t].r.angleTo(l.body->referencePose[b].r) * kDeg;
			CHECK(err < 0.5, "%s/%s: %s is %.2f deg off its reference pose", s.platform, s.file, l.body->bones[b].c_str(), err);
			checked++;
		}
		CHECK(checked >= 6, "%s/%s: %d rest bones checked", s.platform, s.file, checked);
	}
}

// Rotations between frames stay continuous across block boundaries (mt_idle LE has 10 blocks).
TEST(BlockSeamsAreContinuous) {
	Loaded l = Load(Samples()[0]);
	const auto& f = l.clip.frames;
	double worstSeam = 0, worstStep = 0;
	for (size_t i = 1; i < f.size(); i++)
		for (int t = 0; t < l.clip.numTracks; t++) {
			double a = f[i - 1][t].r.angleTo(f[i][t].r) * kDeg;
			worstStep = std::max(worstStep, a);
			if (i % 255 == 0) worstSeam = std::max(worstSeam, a);
		}
	CHECK(worstStep < 3.0, "largest per-frame rotation step %.2f deg", worstStep);
	CHECK(worstSeam <= worstStep, "seam step %.2f", worstSeam);
}

// ---- Binding -------------------------------------------------------------------------------------

TEST(BindingModes) {
	for (const auto& s : Samples()) {
		Loaded l = Load(s);
		const bool se = std::string(s.platform) == "SSE";
		CHECK(l.binding.mode == (se ? ClipBinding::Mode::ByIndex : ClipBinding::Mode::ByName), "%s/%s: %s", s.platform, s.file,
			  l.binding.modeName());
		CHECK(l.binding.dropped.size() == s.dropped, "%s/%s: %zu dropped", s.platform, s.file, l.binding.dropped.size());
	}
	// Case-only differences bind: the LE clips name SHIELD / WEAPON / QUIVER, the skeleton Shield / Weapon / Quiver.
	Loaded walk = Load(Samples()[1]);
	for (const char* n : {"Shield", "Weapon", "Quiver"}) {
		int bone = walk.body->find(n);
		bool bound = std::find(walk.binding.trackToBone.begin(), walk.binding.trackToBone.end(), bone) != walk.binding.trackToBone.end();
		CHECK(bone >= 0 && bound, "%s is bound", n);
	}
	// Named tracks bind by name, not position: track 42 of the LE walk is SHIELD, bone 42 is Shield too,
	// but track 105+ (the breast and butt bones) must not land on bones 105+.
	for (int t = 0; t < walk.clip.numTracks; t++) {
		int b = walk.binding.trackToBone[t];
		if (b >= 0 && walk.clip.trackNames[t])
			CHECK(walk.body->find(*walk.clip.trackNames[t]) == b, "track %d bound to %s", t, walk.body->bones[b].c_str());
	}
}

// An explicit index map wins over names and positions, and is validated. No Skyrim sample carries
// one, so the clip is a real sample with a map attached.
TEST(ExplicitBinding) {
	Loaded l = Load(Samples()[4]); // SE idle, nameless
	HkxClip c = l.clip;
	c.trackToBone.clear();
	for (int t = 0; t < c.numTracks; t++) c.trackToBone.push_back(static_cast<int16_t>(c.numTracks - 1 - t));
	ClipBinding b = BindClip(c, *l.body);
	CHECK(b.mode == ClipBinding::Mode::Explicit && b.trackToBone[0] == c.numTracks - 1, "explicit map used");
	c.trackToBone[0] = 500;
	bool threw = false;
	try {
		BindClip(c, *l.body);
	} catch (const Error&) {
		threw = true;
	}
	CHECK(threw, "an out-of-range bone index is rejected");
	// Nameless tracks beyond the skeleton cannot bind.
	HkxClip big = l.clip;
	big.numTracks = 200;
	big.trackNames.resize(200);
	threw = false;
	try {
		BindClip(big, *l.body);
	} catch (const Error&) {
		threw = true;
	}
	CHECK(threw, "200 unnamed tracks on a 126-bone skeleton are rejected");
}

// ---- Real coverage fixtures (samples/animationwork/coverage: installed-mod clips, copied under
// neutral names, that use the encodings the walk/run/idle samples do not) --------------------------

std::string Coverage(const std::string& f) { return Anim("coverage/" + f); }
const HkxSkeleton& SeBody() {
	static const std::vector<HkxSkeleton> s = ReadSkeletons(HkxPackfile::Load(SkeletonHkx("SSE")));
	return s[0];
}

// A clip with both track names and an explicit index map, both naming the same bones on this skeleton:
// the index array is read correctly when every index lands on the bone of the track's own name.
TEST(CoverageExplicitBindingAgrees) {
	HkxClip c = ReadClip(HkxPackfile::Load(Coverage("explicit_binding_agrees.hkx")));
	CHECK(c.trackToBone.size() == size_t(c.numTracks) && c.numTracks == 68, "%zu indices for %d tracks", c.trackToBone.size(), c.numTracks);
	int same = 0;
	for (int t = 0; t < c.numTracks; t++)
		same += c.trackNames[t] && c.trackToBone[t] >= 0 && c.trackToBone[t] < int(SeBody().bones.size()) &&
				SeBody().find(*c.trackNames[t]) == c.trackToBone[t];
	CHECK(same == c.numTracks, "%d of %d indices name the track's bone", same, c.numTracks);
	ClipBinding b = BindClip(c, SeBody());
	CHECK(b.mode == ClipBinding::Mode::ByName && b.conflicts == 0 && b.dropped.empty(), "%s, %d conflicts, %zu dropped", b.modeName(),
		  b.conflicts, b.dropped.size());
	CHECK(c.unverifiedFormats.empty(), "explicit binding is a verified encoding");
}

// Authored for a skeleton variant with breast bones at the indices this skeleton uses for NPC L/R Item:
// binding by name keeps the breast tracks off the Item bones.
TEST(CoverageExplicitBindingOtherSkeleton) {
	HkxClip c = ReadClip(HkxPackfile::Load(Coverage("explicit_binding_other_skeleton.hkx")));
	ClipBinding b = BindClip(c, SeBody());
	CHECK(b.mode == ClipBinding::Mode::ByName && b.conflicts > 0, "%s, %d conflicts", b.modeName(), b.conflicts);
	CHECK(std::find(b.dropped.begin(), b.dropped.end(), "NPC L PreBreast") != b.dropped.end(), "NPC L PreBreast dropped, not bound");
	for (const char* item : {"NPC L Item01", "NPC L Item02", "NPC L Item03", "NPC R Item01", "NPC R Item02"}) {
		int bone = SeBody().find(item);
		CHECK(std::find(b.trackToBone.begin(), b.trackToBone.end(), bone) == b.trackToBone.end(), "%s is not driven", item);
	}
}

// Uncompressed (interleaved) clip: frame-major transforms. A wrong layout scrambles every track; the right
// one moves smoothly, holds still bones at the reference pose, and this cycle loops.
TEST(CoverageInterleaved) {
	HkxClip c = ReadClip(HkxPackfile::Load(Coverage("interleaved.hkx")));
	CHECK(c.animClass == "hkaInterleavedUncompressedAnimation" && c.numFrames == 41 && c.numTracks == 107, "%s %d x %d",
		  c.animClass.c_str(), c.numFrames, c.numTracks);
	CHECK(std::fabs(c.frameDuration - 1.0 / 30) < 1e-5, "frame duration %g", c.frameDuration);
	double step = 0;
	for (int f = 1; f < c.numFrames; f++)
		for (int t = 0; t < c.numTracks; t++) step = std::max(step, c.frames[f - 1][t].r.angleTo(c.frames[f][t].r) * kDeg);
	CHECK(step < 6.0, "largest per-frame step %.2f deg", step);
	ClipBinding b = BindClip(c, SeBody());
	int still = 0, atRest = 0;
	for (int t = 0; t < c.numTracks; t++) {
		int bone = b.trackToBone[t];
		if (bone < 0) continue;
		bool s = true;
		for (auto& f : c.frames) s = s && f[t].r.angleTo(c.frames[0][t].r) < 1e-6;
		if (!s) continue;
		still++;
		atRest += c.frames[0][t].r.angleTo(SeBody().referencePose[bone].r) * kDeg < 0.5;
	}
	CHECK(still >= 15 && atRest >= 15, "%d still bones, %d at the reference pose", still, atRest);
	// It is a mounted turn, not a cycle: the body (NPC COM, 2.3 units above the root) swings half round.
	PosedClip p = PoseClip(c, b, SeBody(), LoadSkeleton(SkeletonNif("SSE")));
	CHECK(!p.loops && std::fabs(p.loopError - 180.0) < 1.0, "not a loop (first/last differ by %.2f deg)", p.loopError);
}

// Float tracks drive skeleton float slots; the transform data around them must still decode exactly
// (DecodeSpline checks every block's end offset) and the clip exports with a note.
TEST(CoverageFloatTracks) {
	HkxClip c = ReadClip(HkxPackfile::Load(Coverage("float_tracks.hkx")));
	CHECK(c.numFloatTracks == 4 && c.numTracks == 99, "%d float tracks, %d transform tracks", c.numFloatTracks, c.numTracks);
	const fs::path out = fs::u8path(Tmp("coverage_out"));
	fs::remove_all(out);
	BatchRequest req;
	req.inputs = {Coverage("float_tracks.hkx"), Coverage("additive.hkx"), Coverage("xml_tagfile.hkx")};
	req.skeletonNif = SkeletonNif("SSE");
	req.skeletonHkx = SkeletonHkx("SSE");
	req.outDir = out.u8string();
	BatchResult r = RunBatch(req, Quiet);
	CHECK(r.items.size() == 3 && r.exported() == 1, "%zu clips, %zu exported", r.items.size(), r.exported());
	for (auto& it : r.items) {
		const std::string f = fs::u8path(it.clip).filename().u8string();
		if (f == "float_tracks.hkx") {
			bool noted = std::any_of(it.warnings.begin(), it.warnings.end(), [](auto& w) { return w.find("float tracks") != std::string::npos; });
			CHECK(it.error.empty() && noted, "float_tracks exported with a note: %s", it.error.c_str());
		} else if (f == "additive.hkx") {
			CHECK(it.error.find("additive clip") != std::string::npos, "additive refused: %s", it.error.c_str());
		} else {
			CHECK(it.error.find("Havok XML file") != std::string::npos, "XML tagfile named as such: %s", it.error.c_str());
		}
	}
}

// ---- Pose ----------------------------------------------------------------------------------------

TEST(PoseOnNifSkeleton) {
	for (const auto& s : Samples()) {
		Loaded l = Load(s);
		Skeleton nif = LoadSkeleton(SkeletonNif(s.platform));
		PosedClip p = PoseClip(l.clip, l.binding, *l.body, nif);
		const std::string name = std::string(s.platform) + "/" + s.file;
		CHECK(p.frames.size() == size_t(s.frames), "%s", name.c_str());
		CHECK(p.loops == s.loops, "%s: loops %d (error %.3f deg)", name.c_str(), p.loops, p.loopError);
		// Bone set: every Humanoid bone, the helper nodes above them, and names unique.
		std::map<std::string, int> index;
		for (size_t i = 0; i < p.bones.size(); i++) index[p.bones[i].name] = static_cast<int>(i);
		CHECK(index.size() == p.bones.size(), "%s: unique bone names", name.c_str());
		for (auto& h : HumanoidMap()) CHECK(index.count(h.skyrim), "%s: %s present", name.c_str(), h.skyrim);
		CHECK(index.count("CME LBody [LBody]") && index.count("NPC") && !index.count("skeleton_female.nif"), "%s: helper chain kept, file root not",
			  name.c_str());
		// The rest pose is the NIF's, rigid: exactly what NifMerge writes for the same skeleton.
		for (auto& b : p.bones) CHECK(b.world.maxDiff(nif.bones[b.skeletonIndex].world.rigid()) < 1e-9, "%s: rest of %s", name.c_str(), b.name.c_str());
		// Nodes the clip cannot reach (no driven ancestor) stay at the NIF rest pose in every frame.
		for (size_t i = 0; i < p.bones.size(); i++) {
			bool reach = false;
			for (int k = static_cast<int>(i); k >= 0; k = p.bones[k].parent) reach = reach || p.driven[k];
			if (reach) continue;
			for (auto& f : p.frames) CHECK(f[i].maxDiff(p.bones[i].world) < 1e-9, "%s: %s moved", name.c_str(), p.bones[i].name.c_str());
		}
		// Feet: the lowest toe point over the clip is on the ground (game units, z up).
		int lt = index["NPC L Toe0 [LToe]"], rt = index["NPC R Toe0 [RToe]"];
		double low = 1e9;
		for (auto& f : p.frames) low = std::min({low, f[lt].t.z, f[rt].t.z});
		if (std::string(s.file).find("bow") == std::string::npos)
			CHECK(low > -1.5 && low < 1.5, "%s: lowest toe at z %.2f", name.c_str(), low);
		// The head stays at standing height (the NIF puts it at about 120 units).
		int head = index["NPC Head [Head]"];
		for (auto& f : p.frames) CHECK(f[head].t.z > 100 && f[head].t.z < 130, "%s: head z %.1f", name.c_str(), f[head].t.z);
	}
}

// The game copies a Havok bone's local transform onto the NIF node of the same name; with the clip's
// frame replaced by the Havok reference pose, the posed bones must equal the Havok reference pose
// composed through the NIF's (identity) helper nodes.
TEST(PoseCopiesHavokLocals) {
	Loaded l = Load(Samples()[4]);
	HkxClip c = l.clip;
	c.numFrames = 1;
	c.frames.resize(1);
	for (int t = 0; t < c.numTracks; t++) {
		c.frames[0][t].t = l.body->referencePose[t].t;
		c.frames[0][t].r = l.body->referencePose[t].r;
	}
	Skeleton nif = LoadSkeleton(SkeletonNif("SSE"));
	PosedClip p = PoseClip(c, l.binding, *l.body, nif);
	// Havok world of the leg chain from its reference pose.
	std::vector<Xform> hw(l.body->bones.size());
	for (size_t i = 0; i < hw.size(); i++) {
		const auto& r = l.body->referencePose[i];
		Xform loc{r.r.toMat3() * r.s.x, r.t};
		hw[i] = l.body->parents[i] < 0 ? loc : hw[l.body->parents[i]] * loc;
	}
	for (const char* n : {"NPC Pelvis [Pelv]", "NPC L Thigh [LThg]", "NPC L Calf [LClf]", "NPC Spine2 [Spn2]", "NPC Head [Head]"}) {
		int out = -1;
		for (size_t i = 0; i < p.bones.size(); i++)
			if (p.bones[i].name == n) out = static_cast<int>(i);
		double d = p.frames[0][out].maxDiff(hw[l.body->find(n)].rigid());
		CHECK(d < 1e-4, "%s differs by %g", n, d);
	}
}

// ---- FBX -----------------------------------------------------------------------------------------

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

TEST(FbxRoundTrip) {
	for (size_t si : {size_t(1), size_t(5)}) { // LE walk, SE walk
		const SampleClip& s = Samples()[si];
		Loaded l = Load(s);
		Skeleton nif = LoadSkeleton(SkeletonNif(s.platform));
		PosedClip p = PoseClip(l.clip, l.binding, *l.body, nif);
		const std::string fbx = Tmp(std::string(s.platform) + "_walk.fbx");
		AnimExportInfo info{"walk", "src", SkeletonNif(s.platform), SkeletonHkx(s.platform), &l.clip, &l.binding};
		WriteAnimFbx(p, info, fbx);
		WriteAnimJson(p, info, fbx, Quiet);

		FbxRead in(fbx);
		CHECK(in.scene->GetSrcObjectCount<FbxAnimStack>() == 1, "one take");
		FbxAnimStack* stack = in.scene->GetSrcObject<FbxAnimStack>(0);
		CHECK(std::string(stack->GetName()) == "walk", "take name %s", stack->GetName());
		in.scene->SetCurrentAnimationStack(stack);
		CHECK(in.scene->GetGlobalSettings().GetSystemUnit() == FbxSystemUnit::m, "metres");
		std::map<std::string, FbxNode*> nodes;
		for (int i = 0; i < in.scene->GetNodeCount(); i++) nodes[in.scene->GetNode(i)->GetName()] = in.scene->GetNode(i);
		CHECK(nodes.size() == p.bones.size() + 1, "%zu nodes for %zu bones", nodes.size(), p.bones.size());
		double worstT = 0, worstR = 0;
		for (size_t f = 0; f < p.frames.size(); f++) {
			FbxTime t;
			t.SetSecondDouble(f * p.frameDuration);
			for (size_t b = 0; b < p.bones.size(); b++) {
				FbxNode* n = nodes[p.bones[b].name];
				if (!n) continue;
				FbxAMatrix got = n->EvaluateGlobalTransform(t);
				FbxAMatrix want = ToFbxMatrix(ToFbxXform(p.frames[f][b]));
				for (int k = 0; k < 3; k++) worstT = std::max(worstT, std::fabs(got.GetT()[k] - want.GetT()[k]));
				FbxQuaternion qg = got.GetQ(), qw = want.GetQ();
				double dot = std::fabs(qg[0] * qw[0] + qg[1] * qw[1] + qg[2] * qw[2] + qg[3] * qw[3]);
				worstR = std::max(worstR, 2 * std::acos(std::min(1.0, dot)) * kDeg);
			}
		}
		CHECK(worstT < 1e-4, "%s: worst bone position error %g m", s.platform, worstT);
		CHECK(worstR < 0.01, "%s: worst bone rotation error %g deg", s.platform, worstR);
		// Rest pose (no animation) equals the NIF bind pose.
		FbxNode* com = nodes["NPC COM [COM ]"];
		FbxVector4 rest = com->EvaluateLocalTranslation(FBXSDK_TIME_INFINITE);
		CHECK(com && std::fabs(com->LclTranslation.Get()[1] - 68.9113 * kMetresPerUnit) < 1e-5, "COM rest height %g", com->LclTranslation.Get()[1]);
		(void)rest;

		std::string json = Read(AnimSidecarPathFor(fbx));
		// The sidecar's rest pose is each bone's NIF bind pose relative to its parent, FBX space, exactly
		// (the FBX file's own Euler rest loses ~0.13 deg on bones at +-90 deg pitch, the skirt roots).
		std::map<std::string, Xform> restLocal;
		for (size_t b = 0; b < p.bones.size(); b++) {
			Xform w = ToFbxXform(p.bones[b].world);
			restLocal[p.bones[b].name] = p.bones[b].parent < 0 ? w : ToFbxXform(p.bones[p.bones[b].parent].world).inverse() * w;
		}
		size_t restEntries = 0;
		double worstRestT = 0, worstRestR = 0;
		for (size_t at = json.find("{\"bone\": "); at != std::string::npos; at = json.find("{\"bone\": ", at + 1)) {
			size_t nameEnd = json.find("\", \"t\": [", at);
			std::string bone = json.substr(at + 10, nameEnd - at - 10);
			double t[3], q[4];
			if (std::sscanf(json.c_str() + nameEnd, "\", \"t\": [%lf, %lf, %lf], \"r\": [%lf, %lf, %lf, %lf]", &t[0], &t[1], &t[2], &q[0], &q[1],
							&q[2], &q[3]) != 7 || !nodes.count(bone))
				continue;
			restEntries++;
			const Xform& want = restLocal[bone];
			worstRestT = std::max({worstRestT, std::fabs(want.t.x - t[0]), std::fabs(want.t.y - t[1]), std::fabs(want.t.z - t[2])});
			worstRestR = std::max(worstRestR, Quat::fromMat3(want.L).angleTo(Quat(q[0], q[1], q[2], q[3]).normalized()) * kDeg);
		}
		CHECK(restEntries == p.bones.size(), "%s: %zu rest entries for %zu bones", s.platform, restEntries, p.bones.size());
		CHECK(worstRestT < 1e-5 && worstRestR < 0.01, "%s: sidecar rest = NIF bind pose (%g m, %g deg)", s.platform, worstRestT, worstRestR);
		CHECK(json.find("\"formatVersion\": 2") != std::string::npos, "sidecar format version 2");
		CHECK(json.find("\"loop\": true") != std::string::npos, "%s: loop flag", s.platform);
		CHECK(json.find("\"human\": \"Hips\", \"bone\": \"NPC COM [COM ]\"") != std::string::npos, "humanoid map");
		const bool le = std::string(s.platform) == "LE";
		CHECK(json.find(le ? "\"platform\": \"LE\"" : "\"platform\": \"SE\"") != std::string::npos, "platform");
		if (le) CHECK(json.find("\"events\": []") != std::string::npos, "LE walk's empty-name event is left out");
		else CHECK(json.find("{\"time\": 0.283333, \"name\": \"FootRight\"}") != std::string::npos, "SE walk events");
	}
}

// ---- Batch ---------------------------------------------------------------------------------------

void CopyTo(const std::string& from, const fs::path& to) {
	fs::create_directories(to.parent_path());
	fs::copy_file(fs::u8path(from), to, fs::copy_options::overwrite_existing);
}

TEST(BatchFolder) {
	// A dropped folder: SE skeleton at the top, SE clips in a subfolder, LE clips in another, a second
	// mt_idle deeper down, the character file, and one truncated clip.
	const fs::path in = fs::u8path(Tmp("batch_in")), out = fs::u8path(Tmp("batch_out"));
	fs::remove_all(in);
	fs::remove_all(out);
	CopyTo(SkeletonNif("SSE"), in / "skeleton_female.nif");
	CopyTo(SkeletonHkx("SSE"), in / "skeleton_female.hkx");
	for (const char* f : {"mt_idle.hkx", "mt_walkforward.hkx", "mt_runforward.hkx"}) CopyTo(Anim(std::string("SSE/") + f), in / "se" / f);
	for (const char* f : {"mt_walkforward.HKX", "mt_runforward.HKX", "bow_release.hkx"}) CopyTo(Anim(std::string("LE/") + f), in / "le" / f);
	CopyTo(Anim("LE/mt_idle.hkx"), in / "le" / "female" / "mt_idle.hkx");
	CopyTo(Anim("LE/defaultfemale.hkx"), in / "characters" / "defaultfemale.hkx");
	{
		std::string clip = Read(Anim("SSE/mt_walkforward.hkx"));
		std::ofstream f(in / "se" / "broken.hkx", std::ios::binary);
		f << clip.substr(0, clip.size() / 2);
	}

	BatchRequest req;
	req.inputs = {in.u8string()};
	req.outDir = out.u8string();
	BatchResult r = RunBatch(req, Quiet);
	CHECK(r.skeletonNif == (in / "skeleton_female.nif").u8string(), "skeleton nif %s", r.skeletonNif.c_str());
	CHECK(r.skeletonHkx == (in / "skeleton_female.hkx").u8string(), "skeleton hkx %s", r.skeletonHkx.c_str());
	CHECK(r.items.size() == 8 && r.exported() == 7 && r.failed() == 1, "%zu clips, %zu exported", r.items.size(), r.exported());
	for (auto& it : r.items) {
		bool broken = fs::u8path(it.clip).filename() == "broken.hkx";
		CHECK(broken != it.error.empty(), "%s: %s", it.clip.c_str(), it.error.c_str());
	}
	CHECK(r.skipped.size() == 1 && r.skipped[0].second == "hkbCharacterData", "character file skipped");
	for (const char* f : {"se/mt_idle", "se/mt_walkforward", "se/mt_runforward", "le/mt_walkforward", "le/mt_runforward",
						  "le/bow_release", "le/female/mt_idle"}) {
		CHECK(fs::is_regular_file(out / (std::string(f) + ".fbx")) && fs::is_regular_file(out / (std::string(f) + ".anim.json")),
			  "%s written", f);
	}
	CHECK(!fs::exists(out / "se" / "broken.fbx"), "nothing written for the broken clip");
	std::string report = Read((out / "batch_report.json").u8string());
	CHECK(report.find("\"exported\": 7") != std::string::npos && report.find("\"failed\": 1") != std::string::npos, "report counts");
	CHECK(report.find("\"holds\": \"hkbCharacterData\"") != std::string::npos, "report lists the skipped file");
	// LE clips on the SE skeleton bind by name; the SE skeleton has every LE bone.
	std::string leWalk = Read((out / "le" / "mt_walkforward.anim.json").u8string());
	CHECK(leWalk.find("\"binding\": \"by name\"") != std::string::npos, "LE walk binds by name");

	// Two skeleton .nif files: the run stops before writing anything.
	fs::remove_all(out);
	CopyTo(SkeletonNif("LE"), in / "le" / "skeleton_female.nif");
	bool threw = false;
	try {
		RunBatch(req, Quiet);
	} catch (const Error& e) {
		threw = std::string(e.what()).find("More than one skeleton .nif") != std::string::npos;
	}
	CHECK(threw, "two skeleton .nif files are refused");
	CHECK(!fs::exists(out), "nothing was written");
	// ...unless the skeleton is named explicitly.
	req.skeletonNif = SkeletonNif("SSE");
	CHECK(RunBatch(req, Quiet).exported() == 7, "explicit skeleton settles it");
}

TEST(BatchCollisions) {
	// Two loose files with the same name land on the same output: both fail, nothing is overwritten.
	const fs::path out = fs::u8path(Tmp("collide_out"));
	fs::remove_all(out);
	BatchRequest req;
	req.inputs = {Anim("SSE/mt_idle.hkx"), Anim("LE/mt_idle.hkx"), Anim("SSE/mt_walkforward.hkx")};
	req.skeletonNif = SkeletonNif("SSE");
	req.skeletonHkx = SkeletonHkx("SSE");
	req.outDir = out.u8string();
	BatchResult r = RunBatch(req, Quiet);
	CHECK(r.exported() == 1 && r.failed() == 2, "%zu exported, %zu failed", r.exported(), r.failed());
	CHECK(!fs::exists(out / "mt_idle.fbx"), "no mt_idle.fbx written");
	for (auto& it : r.items)
		if (!it.error.empty()) CHECK(it.error.find("same file") != std::string::npos, "%s", it.error.c_str());
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
