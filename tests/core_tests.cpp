// Core tests. Fixtures: tests/fixtures/pynifly (fetched by tests/fetch_fixtures.py) and samples/ (the user's real SE set,
// required: its FaceGen head is the case this tool exists for). A missing fixture is a failure.
#include "core/Export.hpp"
#include "core/Humanoid.hpp"
#include "core/Merge.hpp"
#include "core/Morphs.hpp"
#include "core/NifIO.hpp"
#include "core/Pipeline.hpp"
#include "core/Tri.hpp"

#include "NifFile.hpp"
#include <fbxsdk.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <tuple>
#include <cstdint>
#include <string>
#include <unordered_map>
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
std::string Fx(const std::string& rel) { return kRoot + "/tests/fixtures/pynifly/" + rel; }
std::string Sample(const std::string& rel) { return kRoot + "/samples/" + rel; }
std::string Tmp(const std::string& rel) {
	fs::create_directories(kRoot + "/.tmp/tests");
	return kRoot + "/.tmp/tests/" + rel;
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

std::string Lower(std::string s) {
	for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

bool Near(const Vec3& a, const Vec3& b, double eps) {
	return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

// Independent oracle: the game's skinning formula evaluated with nifly's own transform math and
// nifly's own skeleton world transforms. Returns max error between LoadPosedShapes and the oracle.
double MaxReposeError(const std::string& nifPath, const std::string& skelPath) {
	nifly::NifFile nif, skelNif;
	nif.Load(fs::u8path(nifPath));
	skelNif.Load(fs::u8path(skelPath));
	Skeleton skel = LoadSkeleton(skelPath);
	auto posed = LoadPosedShapes(nifPath, skel, Quiet);
	std::map<std::string, const PosedShape*> byName;
	for (auto& p : posed) byName[p.material.shapeName] = &p;

	double worst = 0;
	int shapesChecked = 0;
	for (nifly::NiShape* shape : nif.GetShapes()) {
		if (!shape->IsSkinned()) continue;
		auto it = byName.find(shape->name.get());
		CHECK(it != byName.end(), "shape '%s' missing from loader output", shape->name.get().c_str());
		if (it == byName.end()) continue;
		std::vector<nifly::Vector3> verts;
		if (auto* dyn = dynamic_cast<nifly::BSDynamicTriShape*>(shape))
			for (auto& d : dyn->dynamicData) verts.emplace_back(d.x, d.y, d.z);
		else
			nif.GetVertsForShape(shape, verts);
		std::vector<std::string> bones;
		nif.GetShapeBoneList(shape, bones);
		std::vector<nifly::Vector3> expected(verts.size());
		std::vector<float> sums(verts.size(), 0.f);
		for (uint32_t b = 0; b < bones.size(); b++) {
			nifly::MatTransform s2b, world;
			nif.GetShapeTransformSkinToBone(shape, b, s2b);
			std::string name = bones[b] == "Head" ? "NPC Head [Head]" : bones[b];
			CHECK(skelNif.GetNodeTransformToGlobal(name, world), "bone %s not in skeleton", name.c_str());
			std::unordered_map<uint16_t, float> w;
			nif.GetShapeBoneWeights(shape, b, w);
			for (auto& [v, wt] : w) {
				expected[v] += world.ApplyTransform(s2b.ApplyTransform(verts[v])) * wt;
				sums[v] += wt;
			}
		}
		const PosedShape& p = *it->second;
		CHECK(p.positions.size() == verts.size(), "vertex count");
		for (size_t v = 0; v < verts.size() && v < p.positions.size(); v++) {
			if (sums[v] <= 0) continue;
			nifly::Vector3 e = expected[v] / sums[v];
			Vec3 d = p.positions[v] - Vec3(e.x, e.y, e.z);
			worst = std::fmax(worst, std::fmax(std::fabs(d.x), std::fmax(std::fabs(d.y), std::fabs(d.z))));
		}
		shapesChecked++;
	}
	CHECK(shapesChecked > 0, "no skinned shapes checked in %s", nifPath.c_str());
	return worst;
}

Vec3 Centroid(const PosedShape& s) {
	Vec3 c;
	for (auto& p : s.positions) c += p;
	return c * (1.0 / s.positions.size());
}

const PosedShape* FindShape(const std::vector<PosedShape>& v, const std::string& contains) {
	for (auto& s : v)
		if (s.material.shapeName.find(contains) != std::string::npos) return &s;
	return nullptr;
}

// FaceGen head shape: the one using Skyrim's face shader (BSLSP_FACE).
const PosedShape* FindFaceShape(const std::vector<PosedShape>& v) {
	for (auto& s : v)
		if (s.material.shaderType == 4) return &s;
	return nullptr;
}

std::vector<std::string> SampleParts() {
	return {Sample("femalebody_0.nif"), Sample("femalehands_1.nif"), Sample("femalefeet_0.nif"), Sample("Head.nif")};
}

} // namespace

// Shapes with geometry across the sample parts (one per weight pair), and how many use model-space
// normal maps: what a merge of the samples must produce.
std::pair<size_t, int> SampleShapeCounts() {
	Skeleton skel = LoadSkeleton(Sample("skeleton_female.nif"));
	size_t shapes = 0;
	int msn = 0;
	for (auto& part : PairPartFiles(SampleParts()))
		for (auto& s : LoadPosedShapes(part.path0, skel, Quiet)) {
			shapes++;
			msn += s.material.modelSpaceNormals ? 1 : 0;
		}
	return {shapes, msn};
}

TEST(FixturesPresent) {
	for (auto& f : {Fx("skeleton_vanilla_le.nif"), Fx("femalebody_1_le.nif"), Fx("malebody_1_le.nif"), Fx("malehands_1_le.nif"),
					Fx("malefeet_1_le.nif"), Fx("malehead_le.nif"), Fx("daedriccuirass_1_le.nif"), Fx("eyeschild_le.nif"),
					Fx("eyeschild_le.tri"), Fx("facegen_se.nif"),
					Fx("skeleton_female_xpmse.nif"), Fx("skeleton_male_xpmse.nif"), Sample("skeleton_female.nif"),
					Sample("femalebody_0.nif"), Sample("femalebody_1.nif"), Sample("femalehands_0.nif"),
					Sample("femalehands_1.nif"), Sample("femalefeet_0.nif"), Sample("femalefeet_1.nif"),
					Sample("Head.nif")})
		CHECK(fs::exists(f), "missing fixture %s (run: python tests/fetch_fixtures.py; samples/ is your own set)", f.c_str());
}

TEST(SkeletonHumanoidStructure) {
	for (auto& path : {Sample("skeleton_female.nif"), Fx("skeleton_vanilla_le.nif")}) {
		Skeleton s = LoadSkeleton(path);
		for (auto& h : HumanoidMap())
			if (h.required) CHECK(s.find(h.skyrim) >= 0, "%s lacks %s", path.c_str(), h.skyrim);
		auto isAncestor = [&](int anc, int i) {
			for (; i >= 0; i = s.bones[i].parent)
				if (i == anc) return true;
			return false;
		};
		int com = s.find("NPC COM [COM ]");
		CHECK(isAncestor(com, s.find("NPC Spine [Spn0]")), "COM must be above Spine");
		CHECK(isAncestor(com, s.find("NPC L Thigh [LThg]")), "COM must be above Thigh");
		CHECK(!isAncestor(s.find("NPC Pelvis [Pelv]"), s.find("NPC Spine [Spn0]")), "Spine is not under Pelvis");
	}
}

TEST(ReposeMatchesGameFormula) {
	const std::string skel = Sample("skeleton_female.nif");
	for (auto& f : {"femalebody_0.nif", "femalebody_1.nif", "femalehands_0.nif", "femalehands_1.nif",
					"femalefeet_0.nif", "femalefeet_1.nif", "Head.nif"}) {
		double err = MaxReposeError(Sample(f), skel);
		CHECK(err < 1e-3, "%s: max error %g", f, err);
	}
	CHECK(MaxReposeError(Fx("facegen_se.nif"), Fx("skeleton_male_xpmse.nif")) < 1e-3, "facegen_se");
	CHECK(MaxReposeError(Fx("femalebody_1_le.nif"), Fx("skeleton_vanilla_le.nif")) < 1e-3, "LE body");
}

TEST(FaceGenPartsLandOnTheHead) {
	Skeleton skel = LoadSkeleton(Sample("skeleton_female.nif"));
	auto head = LoadPosedShapes(Sample("Head.nif"), skel, Quiet);
	Vec3 headBone = skel.bones[skel.find("NPC Head [Head]")].world.t;
	const PosedShape* eyes = FindShape(head, "Eyes");
	const PosedShape* mouth = FindShape(head, "Mouth");
	const PosedShape* face = FindFaceShape(head);
	CHECK(eyes && mouth && face, "expected eyes, mouth and FaceGen head shapes");
	if (!eyes || !mouth || !face) return;
	Vec3 e = Centroid(*eyes), mo = Centroid(*mouth);
	// Eyes: centred left/right, in front of and above the head bone, inside the head's bounds.
	CHECK(std::fabs(e.x) < 0.5, "eyes off-centre x=%f", e.x);
	CHECK(e.y > headBone.y + 3 && e.y < headBone.y + 15, "eyes depth y=%f (head bone y=%f)", e.y, headBone.y);
	CHECK(e.z > headBone.z && e.z < headBone.z + 15, "eyes height z=%f (head bone z=%f)", e.z, headBone.z);
	CHECK(mo.z < e.z, "mouth (z=%f) must be below the eyes (z=%f)", mo.z, e.z);
	Vec3 lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
	for (auto& p : face->positions) {
		lo = {std::fmin(lo.x, p.x), std::fmin(lo.y, p.y), std::fmin(lo.z, p.z)};
		hi = {std::fmax(hi.x, p.x), std::fmax(hi.y, p.y), std::fmax(hi.z, p.z)};
	}
	for (const Vec3& c : {e, mo})
		CHECK(c.x > lo.x && c.x < hi.x && c.y > lo.y && c.y < hi.y && c.z > lo.z && c.z < hi.z,
			  "part centroid (%f %f %f) outside the head's bounds", c.x, c.y, c.z);
}

TEST(PartPairing) {
	auto p = PairPartFiles({Sample("femalebody_0.nif")});
	CHECK(p.size() == 1 && p[0].path1, "listing _0 pairs with the existing _1");
	p = PairPartFiles({Sample("femalebody_0.nif"), Sample("femalebody_1.nif")});
	CHECK(p.size() == 1 && p[0].path1, "listing both yields one pair");
	p = PairPartFiles({Sample("femalebody_1.nif")});
	CHECK(p.size() == 1 && p[0].path1 && fs::u8path(p[0].path0).filename() == "femalebody_0.nif", "listing _1 pairs");
	p = PairPartFiles({Sample("Head.nif")});
	CHECK(p.size() == 1 && !p[0].path1, "Head.nif is single");
	p = PairPartFiles(SampleParts());
	CHECK(p.size() == 4, "four parts, got %zu", p.size());
}

TEST(WeightBlend) {
	Skeleton skel = LoadSkeleton(Sample("skeleton_female.nif"));
	auto s0 = LoadPosedShapes(Sample("femalebody_0.nif"), skel, Quiet);
	auto s1 = LoadPosedShapes(Sample("femalebody_1.nif"), skel, Quiet);
	for (double w : {0.0, 50.0, 100.0}) {
		MergeRequest req{Sample("skeleton_female.nif"), {{Sample("femalebody_0.nif"), Sample("femalebody_1.nif")}}, w};
		MergedModel m = MergeModel(req, Quiet);
		const double t = w / 100.0;
		size_t base = 0;
		double worst = 0;
		for (size_t s = 0; s < s0.size(); s++) {
			for (size_t v = 0; v < s0[s].positions.size(); v++) {
				Vec3 want = s0[s].positions[v] * (1 - t) + s1[s].positions[v] * t;
				Vec3 d = m.positions[base + v] - want;
				worst = std::fmax(worst, d.length());
			}
			base += s0[s].positions.size();
		}
		CHECK(worst < 1e-6, "weight %g: blend error %g", w, worst);
	}
	// The two files really differ, so the blend is exercised.
	double maxDelta = 0;
	for (size_t v = 0; v < s0[0].positions.size(); v++)
		maxDelta = std::fmax(maxDelta, (s0[0].positions[v] - s1[0].positions[v]).length());
	CHECK(maxDelta > 0.5, "_0 and _1 body differ by only %g units; blend untested", maxDelta);
}

TEST(MergeSamples) {
	MergeRequest req{Sample("skeleton_female.nif"), PairPartFiles(SampleParts()), 50};
	MergedModel m = MergeModel(req, Quiet);
	const size_t expected = SampleShapeCounts().first;
	CHECK(expected > 0 && m.materials.size() == expected, "%zu material slots expected, got %zu", expected,
		  m.materials.size());
	std::map<std::string, int> human;
	for (auto& b : m.bones)
		if (!b.humanoid.empty()) human[b.humanoid]++;
	for (auto& h : HumanoidMap()) CHECK(human[h.unity] == 1, "humanoid %s mapped %d times", h.unity, human[h.unity]);
	for (size_t i = 0; i < m.bones.size(); i++) {
		CHECK(m.bones[i].parent < static_cast<int>(i), "bone order");
		double s = m.bones[i].world.L.col(0).length();
		CHECK(std::fabs(s - 1) < 1e-6, "bone %s carries scale %f", m.bones[i].name.c_str(), s);
	}
	size_t badWeights = 0;
	for (auto& vw : m.weights) {
		double sum = 0;
		for (auto& [b, w] : vw) {
			if (b < 0 || b >= static_cast<int>(m.bones.size())) badWeights++;
			sum += w;
		}
		if (std::fabs(sum - 1) > 1e-4) badWeights++;
	}
	CHECK(badWeights == 0, "%zu invalid vertex weights", badWeights);
	CHECK(m.triMaterial.size() == m.tris.size(), "material per triangle");
	std::map<std::string, int> names;
	for (auto& mat : m.materials) CHECK(++names[mat.name] == 1, "duplicate material name %s", mat.name.c_str());
}

// Computed normals are unit length, and vertices that share a position across a seam inside the skin
// (UV seams, neck, wrists, ankles) share a normal, or Unity shows a shading line. Skin =
// model-space-normal-mapped slots, whose nifs store no vertex normals.
void CheckSeamNormals(const MergedModel& m, const char* tag) {
	size_t bad = 0;
	for (auto& n : m.normals)
		if (std::fabs(n.length() - 1) > 1e-6) bad++;
	CHECK(bad == 0, "%s: %zu normals are not unit length", tag, bad);
	std::map<std::tuple<long long, long long, long long>, std::vector<size_t>> byPos;
	std::vector<int> vertSlot(m.positions.size());
	for (size_t t = 0; t < m.tris.size(); t++)
		for (int k = 0; k < 3; k++) vertSlot[m.tris[t][k]] = m.triMaterial[t];
	auto skin = [&](int slot) { return m.materials[slot].modelSpaceNormals; };
	for (size_t v = 0; v < m.positions.size(); v++)
		if (skin(vertSlot[v]))
			byPos[{std::llround(m.positions[v].x * 1000), std::llround(m.positions[v].y * 1000),
				   std::llround(m.positions[v].z * 1000)}].push_back(v);
	size_t seamVerts = 0, crossPart = 0, mismatched = 0;
	for (auto& [pos, verts] : byPos) {
		if (verts.size() < 2) continue;
		seamVerts++;
		for (size_t i = 1; i < verts.size(); i++) {
			if (vertSlot[verts[i]] != vertSlot[verts[0]]) crossPart++;
			if ((m.normals[verts[i]] - m.normals[verts[0]]).length() > 1e-6) mismatched++;
		}
	}
	CHECK(seamVerts > 100, "%s: expected UV/part seams, found %zu", tag, seamVerts);
	CHECK(crossPart > 20, "%s: expected vertices shared across parts (neck/wrist/ankle), found %zu", tag, crossPart);
	CHECK(mismatched == 0, "%s: %zu seam vertices have differing normals", tag, mismatched);
}

TEST(ComputedNormalsAreSeamless) {
	CheckSeamNormals(MergeModel({Sample("skeleton_female.nif"), PairPartFiles(SampleParts()), 50}, Quiet), "plain");
}

TEST(ModelSpaceNormalMatrix) {
	// Unrotated skin space: texel (r, g, b) is Skyrim (r, b, g), which is Unity mesh (r, g, b).
	Mat3 t = MsnTexelToUnityMesh(Mat3());
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++) CHECK(std::fabs(t.m[r][c] - (r == c ? 1 : 0)) < 1e-12, "identity at %d,%d", r, c);
	// Skin space turned 90 degrees about Skyrim Z: red (skin +X) lands on Skyrim +Y, Unity mesh +Z.
	Mat3 rz = Mat3::zero();
	rz.m[0][1] = -1;
	rz.m[1][0] = 1;
	rz.m[2][2] = 1;
	Vec3 red = MsnTexelToUnityMesh(rz) * Vec3(1, 0, 0);
	CHECK(Near(red, Vec3(0, 0, 1), 1e-12), "rotated red -> (%f %f %f)", red.x, red.y, red.z);
	// The samples' skin parts are bound without rotation.
	MergedModel m = MergeModel({Sample("skeleton_female.nif"), PairPartFiles(SampleParts()), 50}, Quiet);
	int msn = 0;
	for (auto& s : m.materials) {
		if (!s.modelSpaceNormals) continue;
		msn++;
		double d = 0;
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++) d = std::fmax(d, std::fabs(s.skinToRest.m[r][c] - (r == c ? 1 : 0)));
		CHECK(d < 1e-3, "%s skinToRest deviates from identity by %g", s.name.c_str(), d);
	}
	const int expectedMsn = SampleShapeCounts().second;
	CHECK(expectedMsn > 0 && msn == expectedMsn, "%d model-space slots expected, got %d", expectedMsn, msn);
}

// ---- Facial morphs (.tri). samples/trifiles holds a mixed female/male, vanilla/High-Poly-Head set,
// including HPH/femalehead.tri, the full morph file of the High Poly Head mesh the sample head is sculpted
// from, the HPH chargen files, and HPH/EFA: an animation mod's EXPR_ morphs for the same parts (mouth, eyes,
// brows, and face masks for both the vanilla and the High Poly Head).

const MergedModel& MorphModel() {
	static MergedModel m = MergeModel(
		{Sample("skeleton_female.nif"), PairPartFiles(SampleParts()), 50, {Sample("trifiles")}}, Quiet);
	return m;
}

// The same set without the full head TRI, so the head takes its morphs from the HPH face masks.
const MergedModel& SurfaceModel() {
	static MergedModel m = [] {
		std::vector<std::string> files;
		for (auto& f : ExpandMorphPaths({Sample("trifiles")}))
			if (Lower(fs::u8path(f).filename().u8string()) != "femalehead.tri") files.push_back(f);
		return MergeModel({Sample("skeleton_female.nif"), PairPartFiles(SampleParts()), 50, files}, Quiet);
	}();
	return m;
}

// Morph sources keyed "<folder>/<file>", lower case.
std::map<std::string, const MorphSource*> SourcesByFile(const MergedModel& m) {
	std::map<std::string, const MorphSource*> by;
	for (auto& s : m.morphSources)
		by[Lower(fs::u8path(s.file).parent_path().filename().u8string() + "/" + fs::u8path(s.file).filename().u8string())] = &s;
	return by;
}

void ExpectApplied(const MergedModel& m, const std::string& key, const char* mode, int slot, uint32_t start) {
	auto by = SourcesByFile(m);
	auto it = by.find(Lower(key));
	CHECK(it != by.end(), "%s not reported", key.c_str());
	if (it == by.end()) return;
	const MorphSource& s = *it->second;
	CHECK(s.outcome == "applied", "%s: %s", key.c_str(), s.outcome.c_str());
	CHECK(s.mode == mode, "%s mode %s, expected %s", key.c_str(), s.mode.c_str(), mode);
	CHECK(slot >= 0 && s.shape == m.materials[slot].name, "%s applied to '%s'", key.c_str(), s.shape.c_str());
	CHECK(s.windowStart == start, "%s window start %u, expected %u", key.c_str(), s.windowStart, start);
}

const BlendShape* FindBlendShape(const MergedModel& m, const std::string& name) {
	for (auto& b : m.blendShapes)
		if (b.name == name) return &b;
	return nullptr;
}

int SlotOfVertex(const MergedModel& m, uint32_t v) {
	for (size_t i = 0; i < m.materialVertices.size(); i++)
		if (v >= m.materialVertices[i].first && v < m.materialVertices[i].first + m.materialVertices[i].second)
			return static_cast<int>(i);
	return -1;
}

int SlotByShader(const MergedModel& m, uint32_t type) {
	for (size_t i = 0; i < m.materials.size(); i++)
		if (m.materials[i].shaderType == type) return static_cast<int>(i);
	return -1;
}

TEST(TriFilesRead) {
	auto files = ExpandMorphPaths({Sample("trifiles")});
	CHECK(files.size() >= 6, "expected the sample .tri files, found %zu", files.size());
	for (auto& f : files) {
		TriFile t = ReadTri(f); // throws on a byte left over or missing
		CHECK(!t.vertices.empty() && !t.triangles.empty() && !t.morphs.empty(), "%s decoded empty", f.c_str());
		for (auto& m : t.morphs) CHECK(m.deltas.size() == t.vertices.size(), "%s/%s delta count", f.c_str(), m.name.c_str());
	}
	bool threw = false;
	try {
		ReadTri(Sample("Head.nif"));
	}
	catch (const Error&) {
		threw = true;
	}
	CHECK(threw, "a non-TRI file must be rejected");
}

TEST(MorphSourceDecisions) {
	const MergedModel& m = MorphModel();
	const auto by = SourcesByFile(m);
	auto expect = [&](const std::string& key, const char* mode, int slot, uint32_t start) { ExpectApplied(m, key, mode, slot, start); };
	const int head = SlotByShader(m, 4);
	int mouth = -1, eyes = -1, brows = -1;
	for (size_t i = 0; i < m.materials.size(); i++) {
		const std::string& n = m.materials[i].shapeName;
		if (n.find("Mouth") != std::string::npos) mouth = int(i);
		if (n.find("Eyes") != std::string::npos) eyes = int(i);
		if (n.find("Brow") != std::string::npos) brows = int(i);
	}
	expect("trifiles/MouthHumanF.tri", "index", mouth, 0);
	expect("trifiles/EyesFemaleLeft.tri", "window", eyes, 0);
	expect("trifiles/EyesFemaleRight.tri", "window", eyes, 88);
	expect("hph/femaleheadbrows.tri", "index", brows, 0);
	expect("hph/femalehead.tri", "index", head, 0);
	// EFA's EXPR_ files apply beside the game's own: their names differ.
	expect("efa/expr_mouthhumanf.tri", "index", mouth, 0);
	expect("efa/expr_eyesfemaleleft.tri", "window", eyes, 0);
	expect("efa/expr_eyesfemaleright.tri", "window", eyes, 88);
	expect("efa/femaleheadbrows_expr.tri", "index", brows, 0);
	expect("efa/femaleheadmaskleftside_expr.tri", "surface", head, 0);
	expect("efa/femaleheadmaskrightside_expr.tri", "surface", head, 0);
	auto expectNot = [&](const std::string& key, const char* outcomePrefix) {
		auto it = by.find(key);
		CHECK(it != by.end() && it->second->outcome.rfind(outcomePrefix, 0) == 0, "%s: %s (expected %s...)", key.c_str(),
			  it != by.end() ? it->second->outcome.c_str() : "not reported", outcomePrefix);
	};
	// The head has an exact source for every name of the game's face masks, so they give it nothing.
	expectNot("hph/femaleheadmaskleftside.tri", "skipped: '");
	expectNot("hph/femaleheadmaskrightside.tri", "skipped: '");
	// EFA's vanilla-head masks share their names with its High Poly Head masks and fit worse.
	expectNot("efa/expr_femaleheadmaskleftside.tri", "superseded by");
	expectNot("efa/expr_femaleheadmaskrightside.tri", "superseded by");
	// EFA's vanilla brows fit only as a surface, and the brows' exact EXPR_ source gives all its names.
	expectNot("efa/expr_femaleheadbrows.tri", "skipped: '");
	for (const char* key : {"hph/femaleheadbrowschargen.tri", "hph/femaleheadmaskleftsidechargen.tri",
							"hph/femaleheadmaskrightsidechargen.tri"})
		expectNot(key, "skipped: character-creation");
	// Without the full head TRI both mask sets carry the head, each with its own names.
	ExpectApplied(SurfaceModel(), "hph/femaleheadmaskleftside.tri", "surface", head, 0);
	ExpectApplied(SurfaceModel(), "hph/femaleheadmaskrightside.tri", "surface", head, 0);
	ExpectApplied(SurfaceModel(), "efa/femaleheadmaskleftside_expr.tri", "surface", head, 0);
	ExpectApplied(SurfaceModel(), "efa/femaleheadmaskrightside_expr.tri", "surface", head, 0);

	// No morph name comes from two sources on the same vertices: two exact sources only as windows on
	// different vertices, and never an exact source and a mask. Masks sharing a name are the halves of one
	// face (left and right), which the overlap rule keeps apart.
	std::map<std::pair<std::string, std::string>, std::vector<const MorphSource*>> givers;
	for (auto& s : m.morphSources)
		if (s.outcome == "applied")
			for (auto& n : s.morphs) givers[{s.shape, n}].push_back(&s);
	for (auto& [key, list] : givers) {
		bool clash = false;
		for (size_t a = 0; a < list.size(); a++)
			for (size_t b = 0; b < a; b++) {
				const bool maskA = list[a]->mode == "surface", maskB = list[b]->mode == "surface";
				if (maskA && maskB) continue;
				clash |= maskA || maskB || list[a]->mode != "window" || list[b]->mode != "window" ||
						 list[a]->windowStart == list[b]->windowStart;
			}
		CHECK(!clash, "'%s' given to '%s' by %zu sources", key.second.c_str(), key.first.c_str(), list.size());
	}
	size_t applied = 0, male = 0;
	for (auto& s : m.morphSources) {
		applied += s.outcome == "applied";
		std::string f = Lower(fs::u8path(s.file).filename().u8string());
		if (f.rfind("male", 0) == 0 || f.find("eyesmale") == 0 || f == "mouthhuman.tri") {
			male++;
			CHECK(s.outcome != "applied", "male/other-head file applied: %s", s.file.c_str());
		}
	}
	CHECK(applied == 11, "11 files applied, got %zu", applied);
	CHECK(male >= 9, "male files present in the sample set: %zu", male);
}

TEST(IndexMorphsAreExact) {
	const MergedModel& m = MorphModel();
	TriFile left = ReadTri(Sample("trifiles/EyesFemaleLeft.tri"));
	const BlendShape* blink = FindBlendShape(m, "BlinkLeft");
	CHECK(blink, "BlinkLeft exists");
	if (!blink) return;
	int eyes = -1;
	for (size_t i = 0; i < m.materials.size(); i++)
		if (m.materials[i].shapeName.find("Eyes") != std::string::npos) eyes = int(i);
	const uint32_t first = m.materialVertices[eyes].first;
	const TriMorph* src = nullptr;
	for (auto& t : left.morphs)
		if (t.name == "BlinkLeft") src = &t;
	CHECK(src, "BlinkLeft in EyesFemaleLeft.tri");
	if (!src) return;
	std::map<uint32_t, Vec3> got;
	for (size_t i = 0; i < blink->vertices.size(); i++) got[blink->vertices[i]] = blink->deltas[i];
	double worst = 0;
	size_t moved = 0;
	for (size_t k = 0; k < left.vertices.size(); k++) {
		Vec3 want = m.materials[eyes].skinToRest * src->deltas[k];
		Vec3 have = got.count(first + uint32_t(k)) ? got[first + uint32_t(k)] : Vec3();
		worst = std::fmax(worst, (have - want).length());
		moved += want.length() > 1e-6;
	}
	CHECK(moved > 10 && worst < 1e-9, "left-eye BlinkLeft reproduced exactly (%zu verts, error %g)", moved, worst);
	// The right eye's half must not move on BlinkLeft.
	size_t rightMoved = 0;
	for (uint32_t k = 88; k < 176; k++) rightMoved += got.count(first + k) ? got[first + k].length() > 1e-6 : 0;
	CHECK(rightMoved == 0, "BlinkLeft moved %zu right-eye vertices", rightMoved);
}

// Left/right asymmetry of a morph in the source masks themselves: for each vertex of one mask, the
// motion of the mirrored vertex in the other mask, mirrored back. Returns the p90 as a fraction of peak.
double SourceAsymmetryP90(const MergedModel& m, const std::string& morphName) {
	std::vector<TriFile> masks;
	for (auto& s : m.morphSources)
		if (s.outcome == "applied" && s.mode == "surface" &&
			std::find(s.morphs.begin(), s.morphs.end(), morphName) != s.morphs.end())
			masks.push_back(ReadTri(s.file));
	if (masks.size() != 2) return -1;
	auto deltas = [&](const TriFile& t) -> const std::vector<Vec3>* {
		for (auto& mo : t.morphs)
			if (mo.name == morphName) return &mo.deltas;
		return nullptr;
	};
	const std::vector<Vec3>* a = deltas(masks[0]);
	const std::vector<Vec3>* b = deltas(masks[1]);
	if (!a || !b) return -1;
	double peak = 0;
	for (auto& d : *a) peak = std::fmax(peak, d.length());
	for (auto& d : *b) peak = std::fmax(peak, d.length());
	std::vector<double> diff;
	for (size_t i = 0; i < masks[0].vertices.size(); i++) {
		if ((*a)[i].length() < 0.05 * peak) continue;
		const Vec3& q = masks[0].vertices[i];
		double best = 0.08;
		size_t j = SIZE_MAX;
		for (size_t k = 0; k < masks[1].vertices.size(); k++) {
			double dist = (masks[1].vertices[k] - Vec3(-q.x, q.y, q.z)).length();
			if (dist < best) best = dist, j = k;
		}
		if (j == SIZE_MAX) continue;
		Vec3 mirrored(-(*b)[j].x, (*b)[j].y, (*b)[j].z);
		diff.push_back(((*a)[i] - mirrored).length() / peak);
	}
	if (diff.empty()) return -1;
	std::sort(diff.begin(), diff.end());
	return diff[diff.size() * 9 / 10];
}

// No frozen patches around the mouth: jaw-opening morphs must move the whole lower lip, including its
// top and inner surfaces. A vertex is frozen when it moves less than a quarter as much as its busiest
// neighbour. Region: the head around the mouth mesh (its bounding box grown by one unit).
void CheckLipsNotFrozen(const MergedModel& m, const char* tag, const std::string& prefix = "") {
	const int head = SlotByShader(m, 4);
	const uint32_t first = m.materialVertices[head].first, count = m.materialVertices[head].second;
	std::vector<std::vector<uint32_t>> nbr(count);
	for (size_t t = 0; t < m.tris.size(); t++) {
		if (m.triMaterial[t] != head) continue;
		for (int a = 0; a < 3; a++)
			for (int b = 0; b < 3; b++)
				if (a != b) nbr[m.tris[t][a] - first].push_back(m.tris[t][b] - first);
	}
	int mouthSlot = -1;
	for (size_t i = 0; i < m.materials.size(); i++)
		if (m.materials[i].shapeName.find("Mouth") != std::string::npos) mouthSlot = int(i);
	CHECK(mouthSlot >= 0, "%s: mouth slot", tag);
	if (mouthSlot < 0) return;
	Vec3 lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
	for (uint32_t i = 0; i < m.materialVertices[mouthSlot].second; i++) {
		const Vec3& p = m.positions[m.materialVertices[mouthSlot].first + i];
		lo = {std::fmin(lo.x, p.x), std::fmin(lo.y, p.y), std::fmin(lo.z, p.z)};
		hi = {std::fmax(hi.x, p.x), std::fmax(hi.y, p.y), std::fmax(hi.z, p.z)};
	}
	lo = lo - Vec3(1, 1, 1);
	hi = hi + Vec3(1, 1, 1);
	for (const char* base : {"Aah", "BigAah", "Oh", "OohQ"}) {
		const std::string full = prefix + base;
		const char* name = full.c_str();
		const BlendShape* b = FindBlendShape(m, name);
		CHECK(b, "%s: %s exists", tag, name);
		if (!b) continue;
		std::vector<Vec3> d(count);
		for (size_t i = 0; i < b->vertices.size(); i++)
			if (SlotOfVertex(m, b->vertices[i]) == head) d[b->vertices[i] - first] = b->deltas[i];
		int frozen = 0, region = 0;
		for (uint32_t v = 0; v < count; v++) {
			const Vec3& p = m.positions[first + v];
			if (p.x < lo.x || p.y < lo.y || p.z < lo.z || p.x > hi.x || p.y > hi.y || p.z > hi.z) continue;
			region++;
			double busiest = 0;
			for (uint32_t u : nbr[v]) busiest = std::fmax(busiest, d[u].length());
			if (busiest >= 0.2 && d[v].length() < 0.25 * busiest) frozen++;
		}
		std::printf("    [lip] %s %s: %d frozen of %d head vertices around the mouth\n", tag, name, frozen, region);
		CHECK(region > 200 && frozen * 50 <= region, "%s %s: %d frozen vertices around the mouth (of %d)", tag, name, frozen,
			  region);
	}
}

TEST(SurfaceMorphTransfer) {
	const MergedModel& m = SurfaceModel();
	const int head = SlotByShader(m, 4);
	const uint32_t first = m.materialVertices[head].first, count = m.materialVertices[head].second;
	Vec3 headBone;
	for (auto& b : m.bones)
		if (b.name == "NPC Head [Head]") headBone = b.world.t;

	auto check = [&](const char* name, double sideSign, bool down, bool symmetric) {
		const BlendShape* b = FindBlendShape(m, name);
		CHECK(b, "%s exists", name);
		if (!b) return;
		// Where the morph acts most, over the whole model (blinks live mostly in the eye mesh).
		double peak = 0;
		size_t peakI = 0;
		for (size_t i = 0; i < b->vertices.size(); i++)
			if (b->deltas[i].length() > peak) peak = b->deltas[i].length(), peakI = i;
		CHECK(peak > 0.1, "%s peak %.3f", name, peak);
		const Vec3& pp = m.positions[b->vertices[peakI]];
		if (sideSign != 0) CHECK(pp.x * sideSign > 0.5, "%s peaks on the wrong side (x=%.2f)", name, pp.x);
		if (down) CHECK(b->deltas[peakI].z < 0, "%s peak moves up", name);

		std::vector<Vec3> d(count);
		double headPeak = 0;
		for (size_t i = 0; i < b->vertices.size(); i++)
			if (SlotOfVertex(m, b->vertices[i]) == head) {
				d[b->vertices[i] - first] = b->deltas[i];
				headPeak = std::fmax(headPeak, b->deltas[i].length());
			}
		// Left and right masks transfer consistently: a symmetric morph moves each vertex as the mirror of
		// its counterpart on the other side of the face (the head is close to symmetric).
		if (symmetric && headPeak > 0) {
			std::map<std::tuple<long long, long long, long long>, uint32_t> grid;
			auto key = [](double x, double y, double z) {
				return std::make_tuple(std::llround(x / 0.25), std::llround(y / 0.25), std::llround(z / 0.25));
			};
			for (uint32_t v = 0; v < count; v++) {
				const Vec3& q = m.positions[first + v];
				grid[key(q.x, q.y, q.z)] = v;
			}
			std::vector<double> diff;
			for (uint32_t v = 0; v < count; v++) {
				const Vec3& q = m.positions[first + v];
				if (d[v].length() < 0.05 * headPeak || std::fabs(q.x) < 0.3) continue;
				double bestDist = 0.08;
				uint32_t mirror = UINT32_MAX;
				for (int dx = -1; dx <= 1; dx++)
					for (int dy = -1; dy <= 1; dy++)
						for (int dz = -1; dz <= 1; dz++) {
							auto it = grid.find(std::make_tuple(std::get<0>(key(-q.x, q.y, q.z)) + dx,
																std::get<1>(key(-q.x, q.y, q.z)) + dy,
																std::get<2>(key(-q.x, q.y, q.z)) + dz));
							if (it == grid.end()) continue;
							double dist = (m.positions[first + it->second] - Vec3(-q.x, q.y, q.z)).length();
							const Vec3& nv = m.normals[first + v];
							if (Vec3(-nv.x, nv.y, nv.z).dot(m.normals[first + it->second]) < 0.7) continue;
							if (dist < bestDist) bestDist = dist, mirror = it->second;
						}
				if (mirror == UINT32_MAX) continue;
				Vec3 mirrored(-d[mirror].x, d[mirror].y, d[mirror].z);
				diff.push_back((d[v] - mirrored).length() / headPeak);
			}
			std::sort(diff.begin(), diff.end());
			CHECK(diff.size() > 100, "%s: %zu mirrored vertex pairs", name, diff.size());
			if (!diff.empty()) {
				double p90 = diff[diff.size() * 9 / 10], mx = diff.back();
				std::printf("    [sym] %s: %zu pairs, p50 %.3f p90 %.3f max %.3f (fraction of peak %.3f)\n", name, diff.size(),
							diff[diff.size() / 2], p90, mx, headPeak);
				size_t big = 0;
				for (double x : diff) big += x > 0.3;
				const double source = SourceAsymmetryP90(m, name);
				std::printf("    [sym] %s: source masks p90 %.3f; %zu pairs differ by more than 30%% of peak\n", name, source, big);
				// The transfer may add a little asymmetry (the shaped head is not perfectly symmetric) but must
				// track the source's own; at closed mouth corners 3D proximity cannot always tell the lips
				// apart, so a few pairs there may follow the other lip.
				CHECK(source >= 0 && p90 <= source + 0.05, "%s: transfer asymmetry p90 %.3f vs source %.3f", name, p90, source);
				CHECK(big * 100 <= diff.size(), "%s: %zu of %zu mirrored pairs disagree by more than 30%% of peak", name, big,
					  diff.size());
			}
		}
		// The back of the head (well behind the head bone) does not move.
		double back = 0;
		for (uint32_t v = 0; v < count; v++)
			if (m.positions[first + v].y < headBone.y - 4) back = std::fmax(back, d[v].length());
		CHECK(back < 1e-6, "%s moves the back of the head by %.3f", name, back);
	};
	for (const std::string prefix : {"", "EXPR_"}) { // the game's masks, then EFA's, transferred separately
		check((prefix + "BlinkLeft").c_str(), -1, true, false); // Skyrim +X is the character's right
		check((prefix + "BlinkRight").c_str(), +1, true, false);
		check((prefix + "Aah").c_str(), 0, true, true); // jaw opens downwards
		check((prefix + "MoodHappy").c_str(), 0, false, true);
		// The face masks do not describe the top and inside of the lower lip; it must still move.
		CheckLipsNotFrozen(m, "masks", prefix);
	}

	const BlendShape* aah = FindBlendShape(m, "Aah");
	if (aah) {
		size_t headMoved = 0;
		for (size_t i = 0; i < aah->vertices.size(); i++) headMoved += SlotOfVertex(m, aah->vertices[i]) == head;
		CHECK(headMoved > 200 && headMoved < count, "Aah transfers to part of the head, not all of it (%zu of %u)", headMoved, count);
	}
}

// Two exact sources for the same mouth with different names both apply, each reproduced exactly.
TEST(SourcesChosenPerMorphName) {
	const MergedModel& m = MorphModel();
	int mouth = -1;
	for (size_t i = 0; i < m.materials.size(); i++)
		if (m.materials[i].shapeName.find("Mouth") != std::string::npos) mouth = int(i);
	CHECK(mouth >= 0, "mouth slot");
	if (mouth < 0) return;
	const uint32_t first = m.materialVertices[mouth].first, count = m.materialVertices[mouth].second;
	for (const char* file : {"trifiles/MouthHumanF.tri", "trifiles/HPH/EFA/expr_mouthhumanf.tri"}) {
		TriFile tri = ReadTri(Sample(file));
		size_t compared = 0;
		for (auto& morph : tri.morphs) {
			const BlendShape* b = FindBlendShape(m, morph.name);
			double peak = 0;
			for (auto& d : morph.deltas) peak = std::fmax(peak, d.length());
			if (peak < 1e-7) continue;
			CHECK(b, "%s: %s is a blend shape", file, morph.name.c_str());
			if (!b) continue;
			std::vector<Vec3> got(count);
			for (size_t i = 0; i < b->vertices.size(); i++)
				if (SlotOfVertex(m, b->vertices[i]) == mouth) got[b->vertices[i] - first] = b->deltas[i];
			double worst = 0;
			for (uint32_t v = 0; v < count; v++) {
				Vec3 want = m.materials[mouth].skinToRest * morph.deltas[v];
				if (want.length() < 1e-7) want = Vec3();
				worst = std::fmax(worst, (got[v] - want).length());
			}
			CHECK(worst < 1e-9, "%s: %s reproduced on the mouth (error %g)", file, morph.name.c_str(), worst);
			compared++;
		}
		CHECK(compared >= 20, "%s: %zu morphs compared", file, compared);
	}
	// EXPR_Aah moves the face too, through EFA's masks, beside the game's Aah from femalehead.tri.
	const BlendShape* expr = FindBlendShape(m, "EXPR_Aah");
	size_t headMoved = 0;
	if (expr)
		for (uint32_t v : expr->vertices) headMoved += SlotOfVertex(m, v) == SlotByShader(m, 4);
	CHECK(headMoved > 200, "EXPR_Aah moves %zu head vertices", headMoved);
}

// HPH/femalehead.tri has the sample head's own triangles but not its shape: the FaceGen head is sculpted
// well beyond the distance limits. It is applied by index, exactly, as the game applies it.
TEST(SculptedHeadMorphsByIndex) {
	const MergedModel& m = MorphModel();
	const int head = SlotByShader(m, 4);
	const uint32_t first = m.materialVertices[head].first, count = m.materialVertices[head].second;
	auto by = SourcesByFile(m);
	auto it = by.find("hph/femalehead.tri");
	CHECK(it != by.end(), "femalehead.tri reported");
	if (it == by.end()) return;
	const MorphSource& src = *it->second;
	CHECK(src.sameTriangles, "femalehead.tri recognised by its triangles");
	CHECK(src.median > 0.35, "the head is sculpted beyond the distance limits (median %.3f), so this exercises the triangle rule",
		  src.median);

	// The masks are still fitted, at the shared origin set by the mouth and brows only: their fits equal
	// those of the mask-only set, unaffected by the sculpted head's own offset.
	auto surface = SourcesByFile(SurfaceModel());
	for (const char* key : {"hph/femaleheadmaskleftside.tri", "hph/femaleheadmaskrightside.tri"}) {
		auto a = by.find(key), b = surface.find(key);
		CHECK(a != by.end() && b != surface.end() && a->second->median == b->second->median &&
				  a->second->max == b->second->max,
			  "%s fits the same with and without femalehead.tri", key);
	}

	// Every morph reproduced exactly on every head vertex, and nothing else moves the head.
	TriFile tri = ReadTri(src.file);
	CHECK(tri.vertices.size() == count, "vertex counts %zu / %u", tri.vertices.size(), count);
	const Mat3& rot = m.materials[head].skinToRest;
	size_t compared = 0;
	for (auto& morph : tri.morphs) {
		double peak = 0;
		for (auto& d : morph.deltas) peak = std::fmax(peak, d.length());
		const BlendShape* b = FindBlendShape(m, morph.name);
		if (peak < 1e-7) {
			bool moves = false;
			if (b)
				for (size_t i = 0; i < b->vertices.size(); i++) moves |= SlotOfVertex(m, b->vertices[i]) == head;
			CHECK(!moves, "%s moves nothing in the TRI but moves the head", morph.name.c_str());
			continue;
		}
		CHECK(b, "%s is a blend shape", morph.name.c_str());
		if (!b) continue;
		std::vector<Vec3> got(count);
		for (size_t i = 0; i < b->vertices.size(); i++)
			if (SlotOfVertex(m, b->vertices[i]) == head) got[b->vertices[i] - first] = b->deltas[i];
		double worst = 0;
		for (uint32_t v = 0; v < count; v++) {
			Vec3 want = rot * morph.deltas[v];
			if (want.length() < 1e-7) want = Vec3();
			worst = std::fmax(worst, (got[v] - want).length());
		}
		CHECK(worst < 1e-9, "%s reproduced on the head (error %g)", morph.name.c_str(), worst);
		compared++;
	}
	CHECK(compared >= 40, "%zu head morphs compared", compared);
	CHECK(!FindBlendShape(m, "CreatedByKouLeifoh"), "the TRI's empty marker morph is not a blend shape");
	CheckLipsNotFrozen(m, "femalehead.tri");
	CheckLipsNotFrozen(m, "EFA masks beside femalehead.tri", "EXPR_");

	// The triangle rule needs the slot's triangles exactly: the same file with its first face's winding
	// reversed is a different mesh, judged by distance alone, and rejected.
	std::vector<char> bytes;
	{
		std::ifstream in(fs::u8path(src.file), std::ios::binary);
		bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}
	uint32_t numVerts = 0, numModVerts = 0;
	std::memcpy(&numVerts, bytes.data() + 8, 4);
	std::memcpy(&numModVerts, bytes.data() + 8 + 9 * 4, 4);
	const size_t face0 = 64 + size_t(numVerts + numModVerts) * 12; // header, vertices, modifier vertices
	std::swap_ranges(bytes.begin() + face0 + 4, bytes.begin() + face0 + 8, bytes.begin() + face0 + 8);
	const std::string patched = Tmp("femalehead_reversed_face.tri");
	{
		std::ofstream out(fs::u8path(patched), std::ios::binary);
		out.write(bytes.data(), std::streamsize(bytes.size()));
	}
	TriFile check = ReadTri(patched);
	CHECK(check.triangles[0][1] == tri.triangles[0][2] && check.triangles[0][2] == tri.triangles[0][1],
		  "patched file has the first face reversed");
	MergedModel alt = MergeModel({Sample("skeleton_female.nif"), PairPartFiles(SampleParts()), 50,
								  {Sample("trifiles/MouthHumanF.tri"), Sample("trifiles/HPH/femaleheadbrows.tri"), patched}},
								 Quiet);
	bool rejected = false;
	for (auto& s : alt.morphSources)
		if (fs::u8path(s.file).filename() == "femalehead_reversed_face.tri")
			rejected = s.outcome.rfind("rejected:", 0) == 0 && !s.sameTriangles;
	CHECK(rejected, "a head TRI with one face reversed is judged by distance and rejected");
}

TEST(BlendShapesRoundTripFbx) {
	RunOptions opt;
	opt.request = {Sample("skeleton_female.nif"), PairPartFiles(SampleParts()), 50, {Sample("trifiles")}};
	opt.fbxPath = Tmp("morphs.fbx");
	RunPipeline(opt, Quiet);
	const MergedModel& m = MorphModel();
	FbxManager* mgr = FbxManager::Create();
	FbxIOSettings* ios = FbxIOSettings::Create(mgr, IOSROOT);
	mgr->SetIOSettings(ios);
	FbxImporter* imp = FbxImporter::Create(mgr, "");
	CHECK(imp->Initialize(opt.fbxPath.c_str(), -1, ios), "importer init");
	FbxScene* scene = FbxScene::Create(mgr, "");
	imp->Import(scene);
	imp->Destroy();
	FbxMesh* mesh = nullptr;
	std::function<void(FbxNode*)> find = [&](FbxNode* n) {
		if (n->GetMesh()) mesh = n->GetMesh();
		for (int i = 0; i < n->GetChildCount(); i++) find(n->GetChild(i));
	};
	find(scene->GetRootNode());
	CHECK(mesh && mesh->GetDeformerCount(FbxDeformer::eBlendShape) == 1, "one blend shape deformer");
	if (!mesh || mesh->GetDeformerCount(FbxDeformer::eBlendShape) != 1) return;
	auto* blend = static_cast<FbxBlendShape*>(mesh->GetDeformer(0, FbxDeformer::eBlendShape));
	CHECK(blend->GetBlendShapeChannelCount() == int(m.blendShapes.size()), "%d channels, expected %zu",
		  blend->GetBlendShapeChannelCount(), m.blendShapes.size());
	// The SDK reads sparse shapes back as full shapes (one point per mesh vertex), so compare the offset
	// of every vertex: ours where the morph acts, zero elsewhere.
	double worst = 0;
	for (int c = 0; c < blend->GetBlendShapeChannelCount() && c < int(m.blendShapes.size()); c++) {
		FbxShape* shape = blend->GetBlendShapeChannel(c)->GetTargetShape(0);
		const BlendShape& b = m.blendShapes[c];
		CHECK(std::string(blend->GetBlendShapeChannel(c)->GetName()) == b.name, "channel %d name", c);
		std::vector<Vec3> want(mesh->GetControlPointsCount());
		for (size_t i = 0; i < b.vertices.size(); i++) {
			const Vec3& d = b.deltas[i];
			want[b.vertices[i]] = {-d.x * kMetresPerUnit, d.z * kMetresPerUnit, d.y * kMetresPerUnit};
		}
		const bool dense = shape->GetControlPointsCount() == mesh->GetControlPointsCount();
		CHECK(dense || shape->GetControlPointsCount() == int(b.vertices.size()), "%s point count %d", b.name.c_str(),
			  shape->GetControlPointsCount());
		for (int i = 0; i < shape->GetControlPointsCount(); i++) {
			int v = dense ? i : shape->GetControlPointIndices()[i];
			FbxVector4 base = mesh->GetControlPointAt(v), target = shape->GetControlPointAt(i);
			worst = std::fmax(worst, (Vec3(target[0] - base[0], target[1] - base[1], target[2] - base[2]) - want[v]).length());
		}
	}
	// The SDK zeroes shape offsets below about a micrometre; anything larger must survive exactly.
	CHECK(worst < 2e-6, "blend shape offsets survive the FBX round trip (error %g m)", worst);
	mgr->Destroy();

	MergedModel plain = MergeModel({Sample("skeleton_female.nif"), PairPartFiles(SampleParts()), 50}, Quiet);
	CHECK(plain.blendShapes.empty() && plain.morphSources.empty(), "no morphs without .tri input");
}

// ---- Outfit (samples/clothing): items replace the character's triangles in the body slots they take.

std::string Dress() { return Sample("clothing/Dress/01_LatexDress_0.nif"); }
std::string Boots() { return Sample("clothing/Boots/02_HighBoots_0.nif"); }
std::string Thong() { return Sample("clothing/Thong/03_String_0.nif"); }

MergedModel Wear(const std::vector<std::string>& items, const LogFn& log = Quiet,
				 const std::vector<std::string>& character = SampleParts()) {
	MergeRequest req{Sample("skeleton_female.nif"), PairPartFiles(character), 50};
	req.outfit = PairPartFiles(items);
	return MergeModel(req, log);
}

const MergedModel& PlainModel() {
	static MergedModel m = MergeModel({Sample("skeleton_female.nif"), PairPartFiles(SampleParts()), 50}, Quiet);
	return m;
}

int SlotOf(const MergedModel& m, const std::string& fileStart, const std::string& shape) {
	for (size_t i = 0; i < m.materials.size(); i++)
		if (Lower(fs::u8path(m.materials[i].sourceFile).filename().u8string()).rfind(Lower(fileStart), 0) == 0 &&
			m.materials[i].shapeName == shape)
			return int(i);
	return -1;
}

const PartReport* ReportOf(const MergedModel& m, const std::string& fileStart) {
	for (auto& p : m.parts)
		if (Lower(fs::u8path(p.file).filename().u8string()).rfind(Lower(fileStart), 0) == 0) return &p;
	return nullptr;
}

size_t SlotTriangles(const MergedModel& m, int slot) {
	return size_t(std::count(m.triMaterial.begin(), m.triMaterial.end(), slot));
}

// Every vertex of a character slot that is still present equals the same vertex merged without the
// outfit (position, normal, UV): the outfit only takes triangles away. `keptOf` lists, per vertex of
// the plain slot, whether it survives (in order); empty means all of them. Where the slot meets a
// shape the outfit replaced (`replaced`, a file name start) or the outfit's own surface, its vertices
// join the item's surface instead, so their normals follow it: there only positions and UVs are compared.
// Those seam vertices must be fewer than 1 in `seamShare` of the slot's (a low-poly slot has a larger share).
void ExpectCharacterUnchanged(const MergedModel& m, const std::string& fileStart, const std::string& shape,
							  const std::vector<bool>& keptOf = {}, const std::string& replaced = "",
							  const MergedModel& plain = PlainModel(), size_t seamShare = 20) {
	const int a = SlotOf(plain, fileStart, shape), b = SlotOf(m, fileStart, shape);
	CHECK(a >= 0 && b >= 0, "%s / %s present", fileStart.c_str(), shape.c_str());
	if (a < 0 || b < 0) return;
	auto [fa, ca] = plain.materialVertices[a];
	auto [fb, cb] = m.materialVertices[b];
	auto key = [](const Vec3& q) { return std::make_tuple(std::llround(q.x * 1000), std::llround(q.y * 1000), std::llround(q.z * 1000)); };
	std::set<std::tuple<long long, long long, long long>> seam; // positions shared with a replaced shape or the outfit
	if (!replaced.empty()) {
		for (size_t r = 0; r < plain.materials.size(); r++)
			if (Lower(fs::u8path(plain.materials[r].sourceFile).filename().u8string()).rfind(Lower(replaced), 0) == 0)
				for (uint32_t v = 0; v < plain.materialVertices[r].second; v++)
					seam.insert(key(plain.positions[plain.materialVertices[r].first + v]));
		for (size_t r = 0; r < m.materials.size(); r++)
			if (m.materials[r].role == "outfit")
				for (uint32_t v = 0; v < m.materialVertices[r].second; v++) seam.insert(key(m.positions[m.materialVertices[r].first + v]));
	}
	double pos = 0, nrm = 0, uv = 0;
	uint32_t j = 0;
	size_t atSeam = 0;
	for (uint32_t i = 0; i < ca; i++) {
		if (!keptOf.empty() && !keptOf[i]) continue;
		if (j >= cb) break;
		pos = std::fmax(pos, (plain.positions[fa + i] - m.positions[fb + j]).length());
		if (seam.count(key(plain.positions[fa + i]))) atSeam++;
		else nrm = std::fmax(nrm, (plain.normals[fa + i] - m.normals[fb + j]).length());
		uv = std::fmax(uv, std::fabs(plain.uvs[fa + i][0] - m.uvs[fb + j][0]) + std::fabs(plain.uvs[fa + i][1] - m.uvs[fb + j][1]));
		j++;
	}
	const size_t want = keptOf.empty() ? ca : size_t(std::count(keptOf.begin(), keptOf.end(), true));
	CHECK(cb == want && j == want, "%s / %s keeps %u vertices, expected %zu", fileStart.c_str(), shape.c_str(), cb, want);
	CHECK(pos < 1e-9 && nrm < 1e-9 && uv < 1e-9, "%s / %s unchanged by the outfit (position %g, normal %g, uv %g)",
		  fileStart.c_str(), shape.c_str(), pos, nrm, uv);
	CHECK(replaced.empty() || (atSeam > 0 && atSeam * seamShare < want), "%s / %s: %zu vertices on the seam with %s", fileStart.c_str(),
		  shape.c_str(), atSeam, replaced.c_str());
}

// Writes copies of a weight pair into .tmp/tests/<folder>, giving each triangle of each shape the slot
// `slotOf(shape, triangle, its three vertices in the _0 file)` returns. The _1 file gets the same
// assignment, as Outfit Studio writes it. Returns the copy's _0 path.
std::string Repartition(const std::string& src0, const std::string& folder,
						const std::function<uint16_t(const std::string&, const std::array<nifly::Vector3, 3>&)>& slotOf) {
	const fs::path dir = fs::u8path(Tmp(folder));
	fs::create_directories(dir);
	std::map<std::string, std::pair<std::vector<uint16_t>, std::vector<int>>> plan; // shape -> slots, triParts
	std::string out0;
	for (int k = 0; k < 2; k++) {
		std::string src = src0;
		src.replace(src.size() - 6, 2, k ? "_1" : "_0");
		nifly::NifFile nif;
		CHECK(nif.Load(fs::u8path(src)) == 0, "load %s", src.c_str());
		for (auto* shape : nif.GetShapes()) {
			const std::string name = shape->name.get();
			std::vector<nifly::Triangle> tris;
			shape->GetTriangles(tris);
			if (k == 0) {
				const auto* verts = nif.GetVertsForShape(shape);
				std::vector<uint16_t> ids;
				std::vector<int> triParts;
				for (auto& t : tris) {
					uint16_t slot = slotOf(name, {nifly::Vector3((*verts)[t.p1]), nifly::Vector3((*verts)[t.p2]), nifly::Vector3((*verts)[t.p3])});
					auto it = std::find(ids.begin(), ids.end(), slot);
					if (it == ids.end()) ids.push_back(slot), it = ids.end() - 1;
					triParts.push_back(int(it - ids.begin()));
				}
				plan[name] = {ids, triParts};
			}
			auto& [ids, triParts] = plan[name];
			nifly::NiVector<nifly::BSDismemberSkinInstance::PartitionInfo> info;
			for (uint16_t id : ids) {
				nifly::BSDismemberSkinInstance::PartitionInfo pi;
				pi.flags = nifly::PartitionFlags(nifly::PF_EDITOR_VISIBLE | nifly::PF_START_NET_BONESET);
				pi.partID = id;
				info.push_back(pi);
			}
			nif.SetShapePartitions(shape, info, triParts);
			nif.UpdateSkinPartitions(shape);
		}
		const fs::path dst = dir / fs::u8path(src).filename();
		CHECK(nif.Save(dst) == 0, "save %s", dst.u8string().c_str());
		if (k == 0) out0 = dst.u8string();
	}
	return out0;
}

TEST(OutfitDressReplacesTheBody) {
	MergedModel m = Wear({Dress()});
	for (const char* shape : {"3BA", "3BA_Vagina", "3BA_Anus"}) {
		CHECK(SlotOf(m, "femalebody", shape) < 0, "the character's %s is replaced", shape);
		const int copy = SlotOf(m, "01_LatexDress", shape);
		CHECK(copy >= 0 && m.materials[copy].role == "outfit", "the dress's own %s is worn", shape);
	}
	CHECK(SlotOf(m, "01_LatexDress", "Dress") >= 0, "the dress itself is present");
	const PartReport* body = ReportOf(m, "femalebody");
	CHECK(body && body->removed.size() == 3, "the body reports its three shapes removed");
	if (body)
		for (auto& r : body->removed)
			CHECK(r.slot == 32 && r.wholeShape && Lower(fs::u8path(r.by).filename().u8string()).rfind("01_latexdress", 0) == 0,
				  "%s removed whole by the dress in slot 32", r.shape.c_str());
	// Hands, feet and the whole head stay; the FaceGen eyes, mouth and brows carry slot 32 but are head parts.
	ExpectCharacterUnchanged(m, "femalehands", "artHands", {}, "femalebody");
	ExpectCharacterUnchanged(m, "femalefeet", "Feet", {}, "femalebody");
	for (size_t i = 0; i < PlainModel().materials.size(); i++) {
		const MaterialSlot& s = PlainModel().materials[i];
		if (Lower(fs::u8path(s.sourceFile).filename().u8string()) == "head.nif") ExpectCharacterUnchanged(m, "head.nif", s.shapeName);
	}
	// The dress's body copy is the dress file's own geometry, re-posed like any part.
	MergedModel alone = MergeModel({Sample("skeleton_female.nif"), PairPartFiles({Dress()}), 50}, Quiet);
	const int a = SlotOf(alone, "01_LatexDress", "3BA"), b = SlotOf(m, "01_LatexDress", "3BA");
	if (a >= 0 && b >= 0) {
		double worst = 0;
		for (uint32_t i = 0; i < alone.materialVertices[a].second; i++)
			worst = std::fmax(worst, (alone.positions[alone.materialVertices[a].first + i] - m.positions[m.materialVertices[b].first + i]).length());
		CHECK(alone.materialVertices[a].second == m.materialVertices[b].second && worst < 1e-9,
			  "the body copy equals the dress file re-posed (error %g)", worst);
	}
}

TEST(OutfitBootsReplaceTheFeet) {
	MergedModel m = Wear({Boots()});
	CHECK(SlotOf(m, "femalefeet", "Feet") < 0, "the feet are replaced");
	CHECK(SlotOf(m, "02_HighBoots", "Boots") >= 0, "the boots are worn");
	ExpectCharacterUnchanged(m, "femalebody", "3BA", {}, "femalefeet"); // the legs stay inside the boots
	ExpectCharacterUnchanged(m, "femalehands", "artHands");
	const PartReport* feet = ReportOf(m, "femalefeet");
	CHECK(feet && feet->removed.size() == 1 && feet->removed[0].slot == 37 && feet->removed[0].wholeShape,
		  "the feet report removal in slot 37");
}

TEST(OutfitThongIsAdded) {
	MergedModel m = Wear({Thong()});
	const int thong = SlotOf(m, "03_String", "0_Panty001");
	CHECK(thong >= 0 && m.materials[thong].role == "outfit" && m.materials[thong].slots == std::vector<uint16_t>{52},
		  "the thong is worn in slot 52");
	CHECK(m.materials.size() == PlainModel().materials.size() + 1, "nothing is removed");
	for (auto& p : m.parts) CHECK(p.removed.empty(), "%s loses nothing", p.file.c_str());
	ExpectCharacterUnchanged(m, "femalebody", "3BA");
}

TEST(OutfitAllThree) {
	std::vector<std::string> logLines;
	MergedModel m = Wear({Dress(), Boots(), Thong()}, [&](LogLevel, const std::string& s) {
		if (s.rfind("Outfit: ", 0) == 0) logLines.push_back(s);
	});
	CHECK(m.materials.size() == 14, "14 material slots, got %zu", m.materials.size());
	CHECK(SlotOf(m, "femalebody", "3BA") < 0 && SlotOf(m, "femalefeet", "Feet") < 0, "body and feet replaced");
	ExpectCharacterUnchanged(m, "femalehands", "artHands", {}, "femalebody");
	CheckSeamNormals(m, "dress, boots and thong");
	// The GUI's preview, read from the files alone, says exactly what the merge logs.
	auto preview = DescribeOutfit(PairPartFiles(SampleParts()), PairPartFiles({Dress(), Boots(), Thong()}));
	CHECK(preview == logLines, "GUI preview matches the merge log (%zu vs %zu lines)", preview.size(), logLines.size());
	for (auto& l : logLines) std::printf("    [outfit] %s\n", l.c_str());
	// The sidecar carries the decisions.
	RunOptions opt;
	opt.request = {Sample("skeleton_female.nif"), PairPartFiles(SampleParts()), 50};
	opt.request.outfit = PairPartFiles({Dress(), Boots(), Thong()});
	opt.fbxPath = Tmp("outfit.fbx");
	RunPipeline(opt, Quiet);
	std::ifstream f(fs::u8path(Tmp("outfit.materials.json")));
	std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	CHECK(json.find("{\"shape\": \"Feet\", \"slot\": 37, \"triangles\": 16288, \"wholeShape\": true, \"by\": \"02_HighBoots_0.nif\"}") !=
			  std::string::npos,
		  "sidecar records the feet replaced by the boots");
	CHECK(json.find("\"role\": \"outfit\"") != std::string::npos && json.find("\"slots\": [52]") != std::string::npos,
		  "sidecar materials carry role and slots");
}

TEST(OutfitTwoItemsInOneSlotIsAnError) {
	std::string msg;
	try {
		Wear({Dress(), Sample("femalebody_0.nif")});
	}
	catch (const Error& e) {
		msg = e.what();
	}
	CHECK(msg.find("01_LatexDress_0.nif") != std::string::npos && msg.find("femalebody_0.nif") != std::string::npos &&
			  msg.find("slot 32") != std::string::npos,
		  "error names both items and the slot: '%s'", msg.c_str());
}

// No helmet sample: the thong, re-partitioned to slot 31 (hair), stands in for one.
TEST(OutfitHelmetHidesOnlyTheHair) {
	const std::string helmet = Repartition(Thong(), "helmet", [](const std::string&, const std::array<nifly::Vector3, 3>&) { return uint16_t(31); });
	auto slots = ReadShapeSlots(helmet);
	CHECK(slots.size() == 1 && std::all_of(slots[0].triSlots.begin(), slots[0].triSlots.end(), [](uint16_t s) { return s == 31; }),
		  "stand-in written with slot 31");
	MergedModel m = Wear({helmet});
	size_t hairGone = 0;
	for (size_t i = 0; i < PlainModel().materials.size(); i++) {
		const MaterialSlot& s = PlainModel().materials[i];
		const std::string file = Lower(fs::u8path(s.sourceFile).filename().u8string());
		const bool hair = s.slots == std::vector<uint16_t>{31};
		if (hair) {
			CHECK(SlotOf(m, file, s.shapeName) < 0, "%s (slot 31) hidden under the helmet", s.shapeName.c_str());
			hairGone++;
		}
		else
			ExpectCharacterUnchanged(m, file, s.shapeName);
	}
	CHECK(hairGone == 2, "two hair shapes hidden, got %zu", hairGone);
}

// No sample shape spans two slots: the body's triangles below the knee become slot 38 (calves) in a copy,
// and the boots take 37 + 38 (the foot part 37, the shaft 38). Wearing them hides exactly those body
// triangles and the vertices only they use; the rest of the body is unchanged.
TEST(OutfitHidesPerPartition) {
	constexpr float kKnee = 33.0f; // nif space, Z up
	auto below = [&](const std::array<nifly::Vector3, 3>& v) { return v[0].z < kKnee && v[1].z < kKnee && v[2].z < kKnee; };
	const std::string body = Repartition(Sample("femalebody_0.nif"), "split_body", [&](const std::string& shape, const std::array<nifly::Vector3, 3>& v) {
		return uint16_t(shape == "3BA" && below(v) ? 38 : 32);
	});
	const std::string boots = Repartition(Boots(), "split_boots", [](const std::string&, const std::array<nifly::Vector3, 3>& v) {
		return uint16_t(v[0].z < 12 && v[1].z < 12 && v[2].z < 12 ? 37 : 38);
	});
	// Which body triangles are below the knee, in the order the merge sees them.
	nifly::NifFile nif;
	nif.Load(fs::u8path(Sample("femalebody_0.nif")));
	std::vector<bool> calf;
	for (auto* shape : nif.GetShapes())
		if (shape->name.get() == "3BA") {
			std::vector<nifly::Triangle> tris;
			shape->GetTriangles(tris);
			const auto* verts = nif.GetVertsForShape(shape);
			for (auto& t : tris) calf.push_back(below({nifly::Vector3((*verts)[t.p1]), nifly::Vector3((*verts)[t.p2]), nifly::Vector3((*verts)[t.p3])}));
		}
	const size_t calfTris = size_t(std::count(calf.begin(), calf.end(), true));
	CHECK(calfTris > 1000 && calfTris < calf.size() / 2, "%zu of %zu body triangles below the knee", calfTris, calf.size());

	std::vector<std::string> character = SampleParts();
	character[0] = body;
	MergedModel m = Wear({boots}, Quiet, character);
	const int b = SlotOf(m, "femalebody", "3BA");
	CHECK(b >= 0, "the body stays");
	if (b < 0) return;
	CHECK(SlotTriangles(m, b) == calf.size() - calfTris, "body keeps %zu triangles, expected %zu", SlotTriangles(m, b),
		  calf.size() - calfTris);
	CHECK(m.materials[b].slots == std::vector<uint16_t>{32}, "the kept body triangles are all slot 32");
	CHECK(SlotOf(m, "femalefeet", "Feet") < 0, "the feet (slot 37) are hidden whole");
	const PartReport* rep = ReportOf(m, "femalebody");
	CHECK(rep && rep->removed.size() == 1 && rep->removed[0].slot == 38 && !rep->removed[0].wholeShape &&
			  rep->removed[0].triangles == calfTris,
		  "the body reports %zu slot-38 triangles removed", calfTris);
	// Kept vertices: those a kept triangle uses, in order; each equals the plain merge's vertex.
	const MergedModel& plain = PlainModel();
	const int p = SlotOf(plain, "femalebody", "3BA");
	std::vector<bool> kept(plain.materialVertices[p].second, false);
	size_t t = 0;
	for (size_t i = 0; i < plain.tris.size(); i++)
		if (plain.triMaterial[i] == p) {
			if (!calf[t])
				for (uint32_t v : plain.tris[i]) kept[v - plain.materialVertices[p].first] = true;
			t++;
		}
	ExpectCharacterUnchanged(m, "femalebody", "3BA", kept);
}

TEST(MergeLegacySkyrim) {
	MergeRequest req{Fx("skeleton_vanilla_le.nif"), {{Fx("femalebody_1_le.nif"), std::nullopt}}, 50};
	MergedModel m = MergeModel(req, Quiet);
	CHECK(!m.positions.empty() && m.materials.size() == 1, "LE body merges");
}

TEST(MissingBoneIsAnError) {
	bool threw = false;
	try {
		// The vanilla LE skeleton has no XPMSE physics bones, which the sample body uses.
		MergeModel({Fx("skeleton_vanilla_le.nif"), {{Sample("femalebody_1.nif"), std::nullopt}}, 50}, Quiet);
	}
	catch (const Error& e) {
		threw = std::string(e.what()).find("not present in the skeleton") != std::string::npos;
	}
	CHECK(threw, "a bone missing from the skeleton must be a hard error");
}

// ---- Skyrim LE (user 12, stream 83). The PyNifly LE fixtures are meshes as the game shipped them. The
// backports are SE samples converted to LE by nifly's OptimizeFor, the conversion SSE NIF Optimizer runs
// (with its head-part option for a FaceGen head), written to .tmp/tests/le.

std::string LegacySkeleton() { return Fx("skeleton_vanilla_le.nif"); }
std::vector<std::string> LegacyParts() {
	return {Fx("malebody_1_le.nif"), Fx("malehands_1_le.nif"), Fx("malefeet_1_le.nif"), Fx("malehead_le.nif")};
}

const MergedModel& LegacyModel() {
	static MergedModel m = MergeModel({LegacySkeleton(), PairPartFiles(LegacyParts()), 50}, Quiet);
	return m;
}

bool IsLegacy(const std::string& path) {
	nifly::NifFile nif;
	return nif.Load(fs::u8path(path)) == 0 && nif.GetHeader().GetVersion().IsSK();
}

std::string BackportToLE(const std::string& src, const std::string& folder) {
	nifly::NifFile nif;
	CHECK(nif.Load(fs::u8path(src)) == 0 && nif.GetHeader().GetVersion().IsSSE(), "%s loads as SE", src.c_str());
	bool head = false;
	for (auto* s : nif.GetShapes()) head |= dynamic_cast<nifly::BSDynamicTriShape*>(s) != nullptr;
	nifly::OptOptions opt;
	opt.targetVersion = nifly::NiVersion::getSK();
	opt.headParts = head;
	opt.removeParallax = false;
	opt.fixBSXFlags = false;
	opt.fixShaderFlags = false;
	const nifly::OptResult r = nif.OptimizeFor(opt);
	CHECK(!r.versionMismatch && !r.dupesRenamed, "%s converts to LE", src.c_str());
	const fs::path dir = fs::u8path(Tmp("le/" + folder));
	fs::create_directories(dir);
	const fs::path dst = dir / fs::u8path(src).filename();
	CHECK(nif.Save(dst) == 0, "save %s", dst.u8string().c_str());
	CHECK(IsLegacy(dst.u8string()), "%s is LE", dst.u8string().c_str());
	return dst.u8string();
}

std::vector<PartInput> Backport(const std::vector<PartInput>& parts, const std::string& folder) {
	std::vector<PartInput> out;
	for (auto& p : parts)
		out.push_back({BackportToLE(p.path0, folder), p.path1 ? std::optional<std::string>(BackportToLE(*p.path1, folder)) : std::nullopt});
	return out;
}

// Copy of an LE nif in .tmp/tests/le/<folder> with every NiTriShape rewritten as NiTriStrips over the same
// vertices, as older LE meshes store them: two triangles sharing an edge form a four-point strip, whose
// second triangle the strip reads with reversed winding; the rest are three-point strips. Returns the
// copy's path and how many four-point strips it holds.
std::pair<std::string, size_t> StripifyLE(const std::string& src, const std::string& folder) {
	nifly::NifFile nif;
	CHECK(nif.Load(fs::u8path(src)) == 0 && nif.GetHeader().GetVersion().IsSK(), "%s loads as LE", src.c_str());
	nifly::NiHeader& hdr = nif.GetHeader();
	size_t quads = 0;
	for (auto* shape : nif.GetShapes()) {
		auto* triShape = dynamic_cast<nifly::NiTriShape*>(shape);
		auto* data = triShape ? dynamic_cast<nifly::NiTriShapeData*>(triShape->GetGeomData()) : nullptr;
		if (!data) continue;
		std::vector<nifly::Triangle> tris;
		data->GetTriangles(tris);
		std::map<std::pair<uint16_t, uint16_t>, std::vector<size_t>> byEdge; // directed edge -> triangles holding it
		for (size_t t = 0; t < tris.size(); t++) {
			const auto& x = tris[t];
			byEdge[{x.p1, x.p2}].push_back(t);
			byEdge[{x.p2, x.p3}].push_back(t);
			byEdge[{x.p3, x.p1}].push_back(t);
		}
		auto strips = std::make_unique<nifly::NiTriStripsData>();
		static_cast<nifly::NiTriBasedGeomData&>(*strips) = static_cast<const nifly::NiTriBasedGeomData&>(*data);
		std::vector<bool> used(tris.size(), false);
		for (size_t t = 0; t < tris.size(); t++) {
			if (used[t]) continue;
			used[t] = true;
			const auto& x = tris[t];
			std::vector<uint16_t> strip{x.p1, x.p2, x.p3};
			for (auto [a, b, c] : {std::array<uint16_t, 3>{x.p1, x.p2, x.p3}, std::array<uint16_t, 3>{x.p2, x.p3, x.p1},
								   std::array<uint16_t, 3>{x.p3, x.p1, x.p2}}) {
				// The neighbour across edge (b, c) holds it as (c, b): strip a b c d reads (a b c), then (b d c).
				auto it = byEdge.find({c, b});
				if (it == byEdge.end()) continue;
				auto n = std::find_if(it->second.begin(), it->second.end(), [&](size_t u) { return !used[u]; });
				if (n == it->second.end()) continue;
				const auto& y = tris[*n];
				const uint16_t d = y.p1 != b && y.p1 != c ? y.p1 : y.p2 != b && y.p2 != c ? y.p2 : y.p3;
				used[*n] = true;
				strip = {a, b, c, d};
				quads++;
				break;
			}
			uint16_t len = static_cast<uint16_t>(strip.size());
			strips->stripsInfo.stripLengths.push_back(len);
			strips->stripsInfo.points.push_back(strip);
		}
		auto stripShape = std::make_unique<nifly::NiTriStrips>();
		static_cast<nifly::NiTriBasedGeom&>(*stripShape) = static_cast<const nifly::NiTriBasedGeom&>(*triShape);
		hdr.ReplaceBlock(hdr.GetBlockID(data), std::move(strips));
		hdr.ReplaceBlock(hdr.GetBlockID(triShape), std::move(stripShape));
	}
	nif.LinkGeomData();
	const fs::path dir = fs::u8path(Tmp("le/" + folder));
	fs::create_directories(dir);
	const fs::path dst = dir / fs::u8path(src).filename();
	CHECK(nif.Save(dst) == 0, "save %s", dst.u8string().c_str());
	return {dst.u8string(), quads};
}

// The same character merged from two encodings of its files: everything Unity receives matches. Triangles
// are compared per material slot as sets, each rotated to start at its lowest vertex (winding kept), since
// a strip encoding lists them in another order. SSE NIF Optimizer's head-part option clears the
// packed-tangent shader flag (it breaks LE head parts), so that flag is not compared. Vertex weights are
// compared per bone (a bone missing on one side weighs 0). `tol` bounds position, normal and weight
// differences: exact for re-encodings of the same file, looser for a conversion that changes the data.
void ExpectSameModel(const MergedModel& a, const MergedModel& b, const char* tag, double tol = 1e-9) {
	auto file = [](const std::string& p) { return fs::u8path(p).filename().u8string(); };
	CHECK(a.bones.size() == b.bones.size(), "%s: %zu vs %zu bones", tag, a.bones.size(), b.bones.size());
	CHECK(a.materials.size() == b.materials.size(), "%s: %zu vs %zu material slots", tag, a.materials.size(), b.materials.size());
	CHECK(a.materialVertices == b.materialVertices, "%s: vertex ranges per slot differ", tag);
	if (a.materials.size() != b.materials.size() || a.materialVertices != b.materialVertices) return;
	const uint32_t flags2 = ~uint32_t(nifly::SLSF2_PACKED_TANGENT);
	for (size_t i = 0; i < a.materials.size(); i++) {
		const MaterialSlot &x = a.materials[i], &y = b.materials[i];
		double rot = 0;
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++) rot = std::fmax(rot, std::fabs(x.skinToRest.m[r][c] - y.skinToRest.m[r][c]));
		CHECK(x.name == y.name && x.shapeName == y.shapeName && file(x.sourceFile) == file(y.sourceFile) &&
				  x.shaderBlock == y.shaderBlock && x.shaderType == y.shaderType && x.shaderFlags1 == y.shaderFlags1 &&
				  (x.shaderFlags2 & flags2) == (y.shaderFlags2 & flags2) && x.textures == y.textures &&
				  x.hasAlphaProperty == y.hasAlphaProperty && x.alphaFlags == y.alphaFlags && x.alphaThreshold == y.alphaThreshold &&
				  x.glossiness == y.glossiness && x.specularStrength == y.specularStrength && x.emissiveMultiple == y.emissiveMultiple &&
				  x.alpha == y.alpha && x.doubleSided == y.doubleSided && x.modelSpaceNormals == y.modelSpaceNormals &&
				  x.vertexColors == y.vertexColors && x.vertexAlpha == y.vertexAlpha && x.role == y.role && x.slots == y.slots &&
				  rot < 1e-9,
			  "%s: material %s differs", tag, x.name.c_str());
	}
	CHECK(a.positions.size() == b.positions.size() && a.normals.size() == b.normals.size() && a.uvs.size() == b.uvs.size() &&
			  a.colors.size() == b.colors.size() && a.weights.size() == b.weights.size(),
		  "%s: vertex attribute counts differ", tag);
	if (a.positions.size() != b.positions.size() || a.normals.size() != b.normals.size() || a.uvs.size() != b.uvs.size() ||
		a.colors.size() != b.colors.size() || a.weights.size() != b.weights.size())
		return;
	double pos = 0, nrm = 0, uv = 0, col = 0, wt = 0;
	for (size_t v = 0; v < a.positions.size(); v++) {
		pos = std::fmax(pos, (a.positions[v] - b.positions[v]).length());
		nrm = std::fmax(nrm, (a.normals[v] - b.normals[v]).length());
		uv = std::fmax(uv, std::fabs(a.uvs[v][0] - b.uvs[v][0]) + std::fabs(a.uvs[v][1] - b.uvs[v][1]));
		if (!a.colors.empty())
			for (int k = 0; k < 4; k++) col = std::fmax(col, std::fabs(a.colors[v][k] - b.colors[v][k]));
		std::map<int, double> w;
		for (auto& [bone, x] : a.weights[v]) w[bone] += x;
		for (auto& [bone, x] : b.weights[v]) w[bone] -= x;
		for (auto& [bone, d] : w) wt = std::fmax(wt, std::fabs(d));
	}
	CHECK(pos < tol && nrm < tol && uv < 1e-9 && col < 1e-9 && wt < tol,
		  "%s: vertices differ (position %g, normal %g, uv %g, colour %g, weight %g)", tag, pos, nrm, uv, col, wt);
	auto trisBySlot = [](const MergedModel& m) {
		std::vector<std::vector<std::array<uint32_t, 3>>> out(m.materials.size());
		for (size_t t = 0; t < m.tris.size(); t++) {
			auto x = m.tris[t];
			std::rotate(x.begin(), std::min_element(x.begin(), x.end()), x.end());
			out[m.triMaterial[t]].push_back(x);
		}
		for (auto& s : out) std::sort(s.begin(), s.end());
		return out;
	};
	CHECK(a.tris.size() == b.tris.size() && trisBySlot(a) == trisBySlot(b), "%s: triangles differ (%zu vs %zu)", tag, a.tris.size(),
		  b.tris.size());
	CHECK(a.blendShapes.size() == b.blendShapes.size(), "%s: %zu vs %zu blend shapes", tag, a.blendShapes.size(), b.blendShapes.size());
	for (size_t i = 0; i < std::min(a.blendShapes.size(), b.blendShapes.size()); i++) {
		const BlendShape &x = a.blendShapes[i], &y = b.blendShapes[i];
		double d = 0;
		if (x.vertices == y.vertices)
			for (size_t k = 0; k < x.deltas.size(); k++) d = std::fmax(d, (x.deltas[k] - y.deltas[k]).length());
		CHECK(x.name == y.name && x.vertices == y.vertices && d < tol, "%s: blend shape %s differs (offset %g)", tag, x.name.c_str(), d);
	}
	CHECK(a.morphSources.size() == b.morphSources.size(), "%s: %zu vs %zu morph sources", tag, a.morphSources.size(),
		  b.morphSources.size());
	for (size_t i = 0; i < std::min(a.morphSources.size(), b.morphSources.size()); i++) {
		const MorphSource &x = a.morphSources[i], &y = b.morphSources[i];
		CHECK(file(x.file) == file(y.file) && x.shape == y.shape && x.mode == y.mode && x.windowStart == y.windowStart &&
				  x.sameTriangles == y.sameTriangles && x.morphs == y.morphs && x.outcome == y.outcome,
			  "%s: %s decided differently ('%s' vs '%s')", tag, file(x.file).c_str(), x.outcome.c_str(), y.outcome.c_str());
	}
	CHECK(a.parts.size() == b.parts.size(), "%s: %zu vs %zu parts", tag, a.parts.size(), b.parts.size());
	for (size_t i = 0; i < std::min(a.parts.size(), b.parts.size()); i++) {
		const PartReport &x = a.parts[i], &y = b.parts[i];
		bool same = file(x.file) == file(y.file) && x.role == y.role && x.slots == y.slots && x.head == y.head &&
					x.removed.size() == y.removed.size();
		for (size_t k = 0; same && k < x.removed.size(); k++) {
			const auto &r = x.removed[k], &s = y.removed[k];
			same = r.shape == s.shape && r.slot == s.slot && r.triangles == s.triangles && r.wholeShape == s.wholeShape && file(r.by) == file(s.by);
		}
		CHECK(same, "%s: part %s reported differently", tag, file(x.file).c_str());
	}
}

TEST(LegacyReposeMatchesGameFormula) {
	for (auto& f : LegacyParts()) CHECK(MaxReposeError(f, LegacySkeleton()) < 1e-3, "%s", f.c_str());
	for (auto& f : {Fx("daedriccuirass_1_le.nif"), Fx("eyeschild_le.nif")}) CHECK(MaxReposeError(f, LegacySkeleton()) < 1e-3, "%s", f.c_str());
}

TEST(MergeLegacyCharacter) {
	for (auto& f : LegacyParts()) CHECK(IsLegacy(f), "%s is LE", f.c_str());
	const MergedModel& m = LegacyModel();
	std::map<std::string, int> human;
	for (auto& b : m.bones)
		if (!b.humanoid.empty()) human[b.humanoid]++;
	for (auto& h : HumanoidMap()) CHECK(human[h.unity] == 1, "humanoid %s mapped %d times", h.unity, human[h.unity]);
	size_t badWeights = 0;
	for (auto& vw : m.weights) {
		double sum = 0;
		for (auto& [b, w] : vw) {
			if (b < 0 || b >= static_cast<int>(m.bones.size())) badWeights++;
			sum += w;
		}
		if (std::fabs(sum - 1) > 1e-4) badWeights++;
	}
	CHECK(badWeights == 0, "%zu invalid vertex weights", badWeights);
	CheckSeamNormals(m, "LE character");
	// Materials as the LE shader and texture-set blocks give them.
	const int body = SlotOf(m, "malebody_1_le", "MaleUnderwearBody:0"), head = SlotOf(m, "malehead_le", "MaleHeadIMF");
	const int hands = SlotOf(m, "malehands_1_le", "HandMaleBig3rd"), feet = SlotOf(m, "malefeet_1_le", "FootMale_Big");
	CHECK(body >= 0 && head >= 0 && hands >= 0 && feet >= 0, "LE body, head, hands and feet slots present");
	if (body < 0 || head < 0 || hands < 0 || feet < 0) return;
	const MaterialSlot &b = m.materials[body], &h = m.materials[head];
	CHECK(b.shaderBlock == "BSLightingShaderProperty" && b.shaderType == 5 && b.modelSpaceNormals && !b.textures.empty() &&
			  Lower(b.textures[0]) == "textures\\actors\\character\\male\\malebody_1.dds",
		  "LE body material: %s type %u texture '%s'", b.shaderBlock.c_str(), b.shaderType, b.textures.empty() ? "" : b.textures[0].c_str());
	CHECK(h.shaderType == 4 && h.modelSpaceNormals && !h.textures.empty() &&
			  Lower(h.textures[0]) == "textures\\actors\\character\\male\\malehead.dds",
		  "LE head material: type %u texture '%s'", h.shaderType, h.textures.empty() ? "" : h.textures[0].c_str());
	CHECK(b.slots == (std::vector<uint16_t>{32, 34, 38}) && m.materials[hands].slots == std::vector<uint16_t>{33} &&
			  m.materials[feet].slots == std::vector<uint16_t>{37} && m.materials[head].slots == (std::vector<uint16_t>{130, 143, 230}),
		  "LE dismember partitions give body 32/34/38, hands 33, feet 37, head 130/143/230");
	const PartReport* headPart = ReportOf(m, "malehead_le");
	CHECK(headPart && headPart->head, "the LE head's partitions mark it as a head part");
}

TEST(LegacyStripsMatchShapes) {
	auto [strips, quads] = StripifyLE(Fx("femalebody_1_le.nif"), "strips");
	nifly::NifFile nif;
	CHECK(nif.Load(fs::u8path(strips)) == 0, "load %s", strips.c_str());
	size_t stripShapes = 0;
	for (auto* s : nif.GetShapes()) stripShapes += dynamic_cast<nifly::NiTriStrips*>(s) != nullptr;
	CHECK(stripShapes == nif.GetShapes().size() && stripShapes > 0, "every shape of the copy is NiTriStrips");
	CHECK(quads > 1000, "%zu four-point strips, so the alternating winding is exercised", quads);
	CHECK(MaxReposeError(strips, LegacySkeleton()) < 1e-3, "LE strips body re-poses by the game formula");
	ExpectSameModel(MergeModel({LegacySkeleton(), {{strips, std::nullopt}}, 50}, Quiet),
					MergeModel({LegacySkeleton(), {{Fx("femalebody_1_le.nif"), std::nullopt}}, 50}, Quiet), "strips vs shapes");
}

TEST(LegacyOutfitCuirass) {
	MergeRequest req{LegacySkeleton(), PairPartFiles(LegacyParts()), 50};
	req.outfit = PairPartFiles({Fx("daedriccuirass_1_le.nif")});
	MergedModel m = MergeModel(req, Quiet);
	const int torso = SlotOf(m, "daedriccuirass_1_le", "TorsoLow:0"), glow = SlotOf(m, "daedriccuirass_1_le", "MaleTorsoGlow");
	CHECK(torso >= 0 && glow >= 0 && m.materials[torso].role == "outfit" && m.materials[glow].role == "outfit", "the cuirass is worn");
	CHECK(SlotOf(m, "malebody_1_le", "MaleUnderwearBody:0") < 0, "the LE body (slot 32) is replaced");
	const PartReport* body = ReportOf(m, "malebody_1_le");
	CHECK(body && !body->removed.empty(), "the LE body reports what the cuirass hid");
	if (body)
		for (auto& r : body->removed)
			CHECK(Lower(fs::u8path(r.by).filename().u8string()) == "daedriccuirass_1_le.nif", "%s removed by the cuirass", r.shape.c_str());
	// An LE effect shader keeps its texture outside a texture set.
	if (glow >= 0) {
		const MaterialSlot& g = m.materials[glow];
		CHECK(g.shaderBlock == "BSEffectShaderProperty" && !g.textures.empty() && Lower(g.textures[0]) == "textures\\effects\\vaportile02.dds" &&
				  g.hasAlphaProperty && g.alphaFlags == 4333,
			  "LE effect material: %s texture '%s' alpha %u", g.shaderBlock.c_str(), g.textures.empty() ? "" : g.textures[0].c_str(),
			  g.alphaFlags);
	}
	ExpectCharacterUnchanged(m, "malehands_1_le", "HandMaleBig3rd", {}, "malebody_1_le", LegacyModel());
	ExpectCharacterUnchanged(m, "malefeet_1_le", "FootMale_Big", {}, "malebody_1_le", LegacyModel(), 8); // 218 vertices, 22 at the ankle
	ExpectCharacterUnchanged(m, "malehead_le", "MaleHeadIMF", {}, "malebody_1_le", LegacyModel());
}

TEST(LegacyEyesMorphsByIndex) {
	MergedModel m = MergeModel({LegacySkeleton(), {{Fx("eyeschild_le.nif"), std::nullopt}}, 50, {Fx("eyeschild_le.tri")}}, Quiet);
	CHECK(m.morphSources.size() == 1 && m.morphSources[0].outcome == "applied" && m.morphSources[0].mode == "index",
		  "eyeschild_le.tri applied by index (%s)", m.morphSources.empty() ? "none" : m.morphSources[0].outcome.c_str());
	TriFile tri = ReadTri(Fx("eyeschild_le.tri"));
	CHECK(m.materials.size() == 1 && tri.vertices.size() == m.materialVertices[0].second, "TRI and LE eyes share their vertices");
	if (m.materials.size() != 1 || tri.vertices.size() != m.materialVertices[0].second) return;
	const Mat3& rot = m.materials[0].skinToRest;
	size_t compared = 0;
	for (auto& morph : tri.morphs) {
		double peak = 0;
		for (auto& d : morph.deltas) peak = std::fmax(peak, d.length());
		if (peak < 1e-7) continue;
		const BlendShape* b = FindBlendShape(m, morph.name);
		CHECK(b, "%s is a blend shape", morph.name.c_str());
		if (!b) continue;
		std::vector<Vec3> got(tri.vertices.size());
		for (size_t i = 0; i < b->vertices.size(); i++) got[b->vertices[i]] = b->deltas[i];
		double worst = 0;
		for (size_t v = 0; v < tri.vertices.size(); v++) {
			Vec3 want = rot * morph.deltas[v];
			if (want.length() < 1e-7) want = Vec3();
			worst = std::fmax(worst, (got[v] - want).length());
		}
		CHECK(worst < 1e-9, "%s reproduced on the LE eyes (error %g)", morph.name.c_str(), worst);
		compared++;
	}
	CHECK(compared > 0, "%zu LE eye morphs compared", compared);
}

// The game's own male head masks and brows on the game's own LE male head: each lies on the head's surface.
TEST(LegacyMaleHeadTakesMaleMasks) {
	MergedModel m = MergeModel({LegacySkeleton(), PairPartFiles(LegacyParts()), 50,
								{Sample("trifiles/MaleHeadmaskLeftside.tri"), Sample("trifiles/MaleHeadmaskRightside.tri"),
								 Sample("trifiles/MaleHeadbrows.tri")}},
							   Quiet);
	const int head = SlotOf(m, "malehead_le", "MaleHeadIMF");
	CHECK(head >= 0 && m.morphSources.size() == 3, "three sources for the LE head");
	if (head < 0) return;
	size_t headMoved = 0;
	for (auto& s : m.morphSources) {
		CHECK(s.outcome == "applied" && s.mode == "surface" && s.shape == m.materials[head].name && s.p90 < 0.01,
			  "%s on the LE male head: %s %s p90 %.4f", fs::u8path(s.file).filename().u8string().c_str(), s.outcome.c_str(), s.mode.c_str(), s.p90);
		for (auto& n : s.morphs)
			if (const BlendShape* b = FindBlendShape(m, n))
				for (uint32_t v : b->vertices) headMoved += SlotOfVertex(m, v) == head;
	}
	CHECK(headMoved > 100, "the masks move %zu LE head vertices", headMoved);
}

// The conversion itself moves the data a little: the LE skin carries the SE file's NiSkinData weights,
// which differ from the SE vertex weights the SE merge reads by up to about 2.5e-4 and leave out
// influences below 1e-4. Each LE file's own data is checked exactly by the game-formula re-pose.
constexpr double kBackportTol = 5e-4;

void ExpectBackportReposes(const MergeRequest& le) {
	std::vector<std::string> files;
	for (auto& list : {le.parts, le.outfit})
		for (auto& p : list) {
			files.push_back(p.path0);
			if (p.path1) files.push_back(*p.path1);
		}
	for (auto& f : files)
		if (IsLegacy(f)) CHECK(MaxReposeError(f, le.skeletonPath) < 1e-3, "%s re-poses by the game formula", f.c_str());
}

// The user's SE character, FaceGen head and outfit, all backported to LE: the merge is the SE merge.
TEST(BackportedCharacterMatchesSE) {
	const auto parts = PairPartFiles(SampleParts()), outfit = PairPartFiles({Dress(), Boots(), Thong()});
	MergeRequest se{Sample("skeleton_female.nif"), parts, 50, {Sample("trifiles")}, outfit};
	MergeRequest le{BackportToLE(Sample("skeleton_female.nif"), "skeleton"), Backport(parts, "character"), 50, {Sample("trifiles")},
					Backport(outfit, "outfit")};
	nifly::NifFile head;
	head.Load(fs::u8path(Tmp("le/character/Head.nif")));
	size_t dynamic = 0, shapes = 0;
	for (auto* s : head.GetShapes()) {
		dynamic += dynamic_cast<nifly::BSDynamicTriShape*>(s) != nullptr;
		shapes += dynamic_cast<nifly::NiTriShape*>(s) != nullptr;
	}
	CHECK(dynamic == 0 && shapes > 0, "the LE FaceGen head holds NiTriShapes only");
	ExpectBackportReposes(le);
	ExpectSameModel(MergeModel(le, Quiet), MergeModel(se, Quiet), "LE backport vs SE", kBackportTol);
}

// LE and SE files in one character: an LE head and dress with the SE body, hands, feet and boots.
TEST(MixedLegacyAndSpecialEdition) {
	auto parts = PairPartFiles(SampleParts());
	auto outfit = PairPartFiles({Dress(), Boots()});
	MergeRequest se{Sample("skeleton_female.nif"), parts, 50, {Sample("trifiles")}, outfit};
	MergeRequest mixed = se;
	for (auto& p : mixed.parts)
		if (fs::u8path(p.path0).filename() == "Head.nif") p = Backport({p}, "mixed")[0];
	mixed.outfit[0] = Backport({mixed.outfit[0]}, "mixed")[0];
	CHECK(IsLegacy(mixed.parts[3].path0) && IsLegacy(mixed.outfit[0].path0) && !IsLegacy(mixed.parts[0].path0) &&
			  !IsLegacy(mixed.outfit[1].path0),
		  "head and dress LE, the rest SE");
	ExpectBackportReposes(mixed);
	ExpectSameModel(MergeModel(mixed, Quiet), MergeModel(se, Quiet), "mixed LE/SE vs SE", kBackportTol);
}

TEST(FbxRoundTrip) {
	RunOptions opt;
	opt.request = {Sample("skeleton_female.nif"), PairPartFiles(SampleParts()), 50};
	opt.fbxPath = Tmp("roundtrip.fbx");
	RunResult r = RunPipeline(opt, Quiet);
	MergedModel m = MergeModel(opt.request, Quiet);
	CHECK(fs::exists(r.sidecarPath), "sidecar written");

	FbxManager* mgr = FbxManager::Create();
	FbxIOSettings* ios = FbxIOSettings::Create(mgr, IOSROOT);
	mgr->SetIOSettings(ios);
	FbxImporter* imp = FbxImporter::Create(mgr, "");
	CHECK(imp->Initialize(opt.fbxPath.c_str(), -1, ios), "importer init");
	FbxScene* scene = FbxScene::Create(mgr, "");
	CHECK(imp->Import(scene), "import");
	imp->Destroy();

	CHECK(scene->GetGlobalSettings().GetSystemUnit() == FbxSystemUnit::m, "metres");
	FbxNode* meshNode = nullptr;
	std::function<void(FbxNode*)> find = [&](FbxNode* n) {
		if (n->GetMesh()) meshNode = n;
		for (int i = 0; i < n->GetChildCount(); i++) find(n->GetChild(i));
	};
	find(scene->GetRootNode());
	CHECK(meshNode, "mesh node");
	if (!meshNode) return;
	// Unity names the model root after the file; no node may reuse that name.
	std::function<void(FbxNode*)> noStem = [&](FbxNode* n) {
		CHECK(std::string(n->GetName()) != "roundtrip", "node named like the file");
		for (int i = 0; i < n->GetChildCount(); i++) noStem(n->GetChild(i));
	};
	noStem(scene->GetRootNode());
	FbxMesh* mesh = meshNode->GetMesh();
	CHECK(mesh->GetControlPointsCount() == static_cast<int>(m.positions.size()), "control points");
	CHECK(mesh->GetPolygonCount() == static_cast<int>(m.tris.size()), "polygons");
	CHECK(meshNode->GetMaterialCount() == static_cast<int>(m.materials.size()), "materials %d",
		  meshNode->GetMaterialCount());
	for (int i = 0; i < meshNode->GetMaterialCount(); i++)
		CHECK(m.materials[i].name == meshNode->GetMaterial(i)->GetName(), "material %d name", i);

	auto* me = mesh->GetElementMaterial();
	size_t wrongMat = 0;
	for (int p = 0; p < mesh->GetPolygonCount(); p++)
		if (me->GetIndexArray().GetAt(p) != m.triMaterial[p]) wrongMat++;
	CHECK(wrongMat == 0, "%zu polygons with the wrong material", wrongMat);

	// Positions: (x, y, z) -> (-x, z, y) * metres-per-unit.
	double worst = 0;
	for (int v = 0; v < mesh->GetControlPointsCount(); v += 97) {
		FbxVector4 c = mesh->GetControlPointAt(v);
		const Vec3& p = m.positions[v];
		Vec3 want{-p.x * kMetresPerUnit, p.z * kMetresPerUnit, p.y * kMetresPerUnit};
		worst = std::fmax(worst, (Vec3(c[0], c[1], c[2]) - want).length());
	}
	CHECK(worst < 1e-6, "control point error %g", worst);

	// Every bone's global transform rebuilt from its local TRS must equal its cluster bind matrix,
	// i.e. the rest pose and the bind pose agree, so the mesh imports undeformed.
	auto* skin = static_cast<FbxSkin*>(mesh->GetDeformer(0, FbxDeformer::eSkin));
	CHECK(skin && skin->GetClusterCount() > 50, "skin clusters");
	double bindErr = 0;
	for (int c = 0; skin && c < skin->GetClusterCount(); c++) {
		FbxCluster* cl = skin->GetCluster(c);
		FbxAMatrix link;
		cl->GetTransformLinkMatrix(link);
		FbxAMatrix global = cl->GetLink()->EvaluateGlobalTransform();
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 4; j++) bindErr = std::fmax(bindErr, std::fabs(link[i][j] - global[i][j]));
	}
	CHECK(bindErr < 1e-5, "bind vs rest mismatch %g", bindErr);
	bool hasBindPose = false;
	for (int i = 0; i < scene->GetPoseCount(); i++) hasBindPose |= scene->GetPose(i)->IsBindPose();
	CHECK(hasBindPose, "bind pose present");
	mgr->Destroy();
}

int main() {
	for (auto& [name, fn] : Registry()) {
		g_test = name;
		int before = g_failures;
		try {
			fn();
		}
		catch (const std::exception& e) {
			g_failures++;
			std::printf("  FAIL [%s] threw: %s\n", name.c_str(), e.what());
		}
		std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name.c_str());
	}
	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
