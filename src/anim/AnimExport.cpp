#include "AnimExport.hpp"

#include "core/FbxCommon.hpp"
#include "core/TextUtil.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace nm {

namespace fs = std::filesystem;

namespace {

// Each bone's rest transform relative to its parent, FBX space (Y-up, metres): the NIF bind pose,
// exactly as NifMerge writes it.
std::vector<Xform> FbxRestLocals(const PosedClip& posed) {
	std::vector<Xform> world(posed.bones.size()), local(posed.bones.size());
	for (size_t i = 0; i < posed.bones.size(); i++) {
		const OutBone& b = posed.bones[i];
		world[i] = ToFbxXform(b.world);
		local[i] = b.parent < 0 ? world[i] : world[b.parent].inverse() * world[i];
	}
	return local;
}

} // namespace

void WriteAnimFbx(const PosedClip& posed, const AnimExportInfo& info, const std::string& fbxPath) {
	const size_t B = posed.bones.size(), F = posed.frames.size();
	if (B == 0 || F == 0) throw Error("Nothing to write: the clip has no bones or frames");
	FbxSession session("SkyAnim");
	FbxScene* scene = session.scene;

	// Rest skeleton: the NIF bind pose, exactly as NifMerge writes it.
	std::vector<FbxNode*> nodes(B);
	std::vector<Xform> rest(B);
	const std::vector<Xform> restLocal = FbxRestLocals(posed);
	for (size_t i = 0; i < B; i++) {
		const OutBone& b = posed.bones[i];
		rest[i] = ToFbxXform(b.world);
		nodes[i] = AddBoneNode(scene, b.parent < 0 ? scene->GetRootNode() : nodes[b.parent], b.name, restLocal[i], b.parent < 0);
	}

	// Frame rate: Skyrim clips are 30 fps; anything else gets a custom rate.
	const double fps = 1.0 / posed.frameDuration;
	if (std::fabs(fps - 30.0) < 1e-3) {
		scene->GetGlobalSettings().SetTimeMode(FbxTime::eFrames30);
	} else {
		scene->GetGlobalSettings().SetTimeMode(FbxTime::eCustom);
		scene->GetGlobalSettings().SetCustomFrameRate(fps);
	}
	auto timeOf = [&](size_t f) {
		FbxTime t;
		t.SetSecondDouble(static_cast<double>(f) * posed.frameDuration);
		return t;
	};
	FbxAnimStack* stack = FbxAnimStack::Create(scene, info.clipName.c_str());
	FbxAnimLayer* layer = FbxAnimLayer::Create(scene, "Base Layer");
	stack->AddMember(layer);
	FbxTimeSpan span(timeOf(0), timeOf(F - 1));
	stack->SetLocalTimeSpan(span);
	stack->SetReferenceTimeSpan(span);
	scene->GetGlobalSettings().SetTimelineDefaultTimeSpan(span);
	scene->SetCurrentAnimationStack(stack);

	// Local transforms per frame (FBX space). A bone gets curves when the Havok skeleton drives it or
	// when its local transform changes anyway (a helper node under a driven, scaled parent).
	std::vector<std::vector<FbxAMatrix>> local(B, std::vector<FbxAMatrix>(F));
	std::vector<Xform> world(B);
	std::vector<bool> keyed(posed.driven);
	for (size_t f = 0; f < F; f++) {
		for (size_t i = 0; i < B; i++) world[i] = ToFbxXform(posed.frames[f][i]);
		for (size_t i = 0; i < B; i++) {
			const int p = posed.bones[i].parent;
			Xform l = p < 0 ? world[i] : world[p].inverse() * world[i];
			local[i][f] = ToFbxMatrix(l);
			if (!keyed[i]) {
				Xform r = p < 0 ? rest[i] : rest[p].inverse() * rest[i];
				keyed[i] = l.maxDiff(r) > 1e-6;
			}
		}
	}
	for (size_t i = 0; i < B; i++) {
		if (!keyed[i]) continue;
		FbxAnimCurve* t[3];
		FbxAnimCurve* r[3];
		const char* comp[3] = {FBXSDK_CURVENODE_COMPONENT_X, FBXSDK_CURVENODE_COMPONENT_Y, FBXSDK_CURVENODE_COMPONENT_Z};
		for (int k = 0; k < 3; k++) {
			t[k] = nodes[i]->LclTranslation.GetCurve(layer, comp[k], true);
			r[k] = nodes[i]->LclRotation.GetCurve(layer, comp[k], true);
			t[k]->KeyModifyBegin();
			r[k]->KeyModifyBegin();
		}
		for (size_t f = 0; f < F; f++) {
			FbxVector4 T = local[i][f].GetT(), R = local[i][f].GetR();
			for (int k = 0; k < 3; k++) {
				int key = t[k]->KeyAdd(timeOf(f));
				t[k]->KeySet(key, timeOf(f), static_cast<float>(T[k]), FbxAnimCurveDef::eInterpolationLinear);
				key = r[k]->KeyAdd(timeOf(f));
				r[k]->KeySet(key, timeOf(f), static_cast<float>(R[k]), FbxAnimCurveDef::eInterpolationLinear);
			}
		}
		for (int k = 0; k < 3; k++) {
			t[k]->KeyModifyEnd();
			r[k]->KeyModifyEnd();
		}
		// Euler angles from per-frame matrices can jump by 360 degrees; unroll keeps them continuous.
		FbxAnimCurveFilterUnroll unroll;
		unroll.Apply(r, 3);
	}
	session.save(fbxPath);
}

std::string AnimSidecarPathFor(const std::string& fbxPath) {
	fs::path p = fs::u8path(fbxPath);
	return (p.parent_path() / (p.stem().u8string() + ".anim.json")).u8string();
}

void WriteAnimJson(const PosedClip& posed, const AnimExportInfo& info, const std::string& fbxPath, const LogFn& log) {
	const HkxClip& clip = *info.clip;
	auto list = [](const auto& items) {
		std::string r = "[";
		bool first = true;
		for (auto& s : items) {
			r += (first ? "" : ", ") + JsonQuote(s);
			first = false;
		}
		return r + "]";
	};

	std::ostringstream o;
	o << "{\n";
	o << "  \"generator\": \"SkyAnim\",\n";
	o << "  \"formatVersion\": 2,\n";
	o << "  \"fbx\": " << JsonQuote(fs::u8path(fbxPath).filename().u8string()) << ",\n";
	o << "  \"clip\": " << JsonQuote(info.clipName) << ",\n";
	o << "  \"source\": " << JsonQuote(info.sourceClip) << ",\n";
	o << "  \"platform\": " << JsonQuote(clip.pointerSize == 8 ? "SE" : "LE") << ",\n";
	o << "  \"skeletonNif\": " << JsonQuote(info.skeletonNif) << ",\n";
	o << "  \"skeletonHkx\": " << JsonQuote(info.skeletonHkx) << ",\n";
	o << "  \"frames\": " << posed.frames.size() << ",\n";
	o << "  \"fps\": " << JsonNum(1.0 / posed.frameDuration) << ",\n";
	o << "  \"duration\": " << JsonNum((posed.frames.size() - 1) * posed.frameDuration) << ",\n";
	o << "  \"loop\": " << (posed.loops ? "true" : "false") << ",\n";
	o << "  \"loopErrorDegrees\": " << JsonNum(posed.loopError) << ",\n";
	o << "  \"rootMotion\": false,\n";
	o << "  \"events\": [";
	bool first = true;
	for (auto& e : clip.events) {
		if (e.text.empty()) {
			log(LogLevel::Warning, info.clipName + ": event at " + JsonNum(e.time) + " s has no name and is left out");
			continue;
		}
		o << (first ? "\n" : ",\n") << "    {\"time\": " << JsonNum(e.time) << ", \"name\": " << JsonQuote(e.text) << "}";
		first = false;
	}
	o << (first ? "],\n" : "\n  ],\n");
	o << "  \"binding\": " << JsonQuote(info.binding->modeName()) << ",\n";
	o << "  \"droppedTracks\": " << list(info.binding->dropped) << ",\n";
	o << "  \"formats\": " << list(clip.formats) << ",\n";
	o << "  \"unverifiedFormats\": " << list(clip.unverifiedFormats) << ",\n";
	o << "  \"humanoid\": [";
	first = true;
	for (auto& b : posed.bones) {
		if (b.humanoid.empty()) continue;
		o << (first ? "\n" : ",\n") << "    {\"human\": " << JsonQuote(b.humanoid) << ", \"bone\": " << JsonQuote(b.name) << "}";
		first = false;
	}
	o << (first ? "],\n" : "\n  ],\n");
	// Unity gives an animated FBX's model the pose of its first frame, not the rest pose; the importer
	// restores this rest pose before building a Humanoid avatar, so the clip's avatar matches the
	// character's. Local to the parent, FBX space: translation in metres, rotation as a quaternion.
	o << "  \"rest\": [";
	const std::vector<Xform> rest = FbxRestLocals(posed);
	for (size_t i = 0; i < rest.size(); i++) {
		Quat q = Quat::fromMat3(rest[i].L);
		o << (i ? ",\n" : "\n") << "    {\"bone\": " << JsonQuote(posed.bones[i].name) << ", \"t\": [" << JsonNum(rest[i].t.x) << ", "
		  << JsonNum(rest[i].t.y) << ", " << JsonNum(rest[i].t.z) << "], \"r\": [" << JsonNum(q.x) << ", " << JsonNum(q.y) << ", "
		  << JsonNum(q.z) << ", " << JsonNum(q.w) << "]}";
	}
	o << (rest.empty() ? "]\n" : "\n  ]\n");
	o << "}\n";

	const std::string path = AnimSidecarPathFor(fbxPath);
	std::ofstream f(fs::u8path(path), std::ios::binary);
	if (!f) throw Error("Cannot write " + path);
	f << o.str();
	if (!f) throw Error("Write failed: " + path);
}

} // namespace nm
