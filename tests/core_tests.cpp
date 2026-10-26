// Core tests. Fixtures: tests/fixtures/pynifly (fetched by tests/fetch_fixtures.py) and samples/ (the user's real SE set,
// required: its FaceGen head is the case this tool exists for). A missing fixture is a failure.
#include "core/Export.hpp"
#include "core/Humanoid.hpp"
#include "core/Merge.hpp"
#include "core/NifIO.hpp"
#include "core/Pipeline.hpp"

#include "NifFile.hpp"
#include <fbxsdk.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <tuple>
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
	for (auto& f : {Fx("skeleton_vanilla_le.nif"), Fx("femalebody_1_le.nif"), Fx("facegen_se.nif"),
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

TEST(ComputedNormalsAreSeamless) {
	MergeRequest req{Sample("skeleton_female.nif"), PairPartFiles(SampleParts()), 50};
	MergedModel m = MergeModel(req, Quiet);
	size_t bad = 0;
	for (auto& n : m.normals)
		if (std::fabs(n.length() - 1) > 1e-6) bad++;
	CHECK(bad == 0, "%zu normals are not unit length", bad);
	// Vertices that share a position across a seam inside the skin (UV seams, neck, wrists, ankles)
	// must share a normal, or Unity shows a shading line. Skin = model-space-normal-mapped slots, whose
	// nifs store no vertex normals.
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
	CHECK(seamVerts > 100, "expected UV/part seams, found %zu", seamVerts);
	CHECK(crossPart > 20, "expected vertices shared across parts (neck/wrist/ankle), found %zu", crossPart);
	CHECK(mismatched == 0, "%zu seam vertices have differing normals", mismatched);
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
