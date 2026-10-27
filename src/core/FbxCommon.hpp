#pragma once
// Skyrim -> FBX conversion and scene plumbing shared by every FBX writer, so models and animations
// written for the same skeleton use identical axes, units and bone transforms.
#include "Export.hpp"

#include <fbxsdk.h>

#include <string>

namespace nm {

// Rc maps Skyrim axes to FBX axes: (x, y, z) -> (-x, z, y).
inline const Mat3& FbxAxes() {
	static const Mat3 r = [] {
		Mat3 m = Mat3::zero();
		m.m[0][0] = -1;
		m.m[1][2] = 1;
		m.m[2][1] = 1;
		return m;
	}();
	return r;
}

inline Vec3 ToFbxPoint(const Vec3& p) { return (FbxAxes() * p) * kMetresPerUnit; }
inline Vec3 ToFbxDir(const Vec3& d) { return FbxAxes() * d; }
inline Xform ToFbxXform(const Xform& x) { return {FbxAxes() * x.L * FbxAxes().transposed(), ToFbxPoint(x.t)}; }

// FbxAMatrix uses the row-vector convention: rows 0..2 are the basis vectors, row 3 the translation.
inline FbxAMatrix ToFbxMatrix(const Xform& x) {
	FbxAMatrix m;
	m.SetIdentity();
	for (int i = 0; i < 3; i++) m.SetRow(i, FbxVector4(x.L.m[0][i], x.L.m[1][i], x.L.m[2][i], 0.0));
	m.SetRow(3, FbxVector4(x.t.x, x.t.y, x.t.z, 1.0));
	return m;
}

// Owns the FBX manager for one write; the scene is Y-up, right-handed, metres.
struct FbxSession {
	FbxManager* mgr = FbxManager::Create();
	FbxIOSettings* ios = nullptr;
	FbxScene* scene = nullptr;

	explicit FbxSession(const char* sceneName) {
		if (!mgr) throw Error("Could not initialise the FBX SDK");
		ios = FbxIOSettings::Create(mgr, IOSROOT);
		mgr->SetIOSettings(ios);
		scene = FbxScene::Create(mgr, sceneName);
		scene->GetGlobalSettings().SetAxisSystem(
			FbxAxisSystem(FbxAxisSystem::eYAxis, FbxAxisSystem::eParityOdd, FbxAxisSystem::eRightHanded));
		scene->GetGlobalSettings().SetSystemUnit(FbxSystemUnit::m);
	}
	~FbxSession() {
		if (mgr) mgr->Destroy();
	}
	FbxSession(const FbxSession&) = delete;
	FbxSession& operator=(const FbxSession&) = delete;

	// Writes the scene as binary FBX.
	void save(const std::string& fbxPath) {
		FbxExporter* exporter = FbxExporter::Create(mgr, "");
		int format = mgr->GetIOPluginRegistry()->GetNativeWriterFormat();
		if (!exporter->Initialize(fbxPath.c_str(), format, ios))
			throw Error("FBX export failed to open '" + fbxPath + "': " + exporter->GetStatus().GetErrorString());
		if (!exporter->Export(scene))
			throw Error("FBX export failed: " + std::string(exporter->GetStatus().GetErrorString()));
		exporter->Destroy();
	}
};

// One skeleton node under parent, positioned by its local transform (FBX space, scale-free).
inline FbxNode* AddBoneNode(FbxScene* scene, FbxNode* parent, const std::string& name, const Xform& local, bool root) {
	FbxAMatrix lm = ToFbxMatrix(local);
	FbxSkeleton* attr = FbxSkeleton::Create(scene, name.c_str());
	attr->SetSkeletonType(root ? FbxSkeleton::eRoot : FbxSkeleton::eLimbNode);
	attr->Size.Set(1.0);
	FbxNode* node = FbxNode::Create(scene, name.c_str());
	node->SetNodeAttribute(attr);
	node->LclTranslation.Set(lm.GetT());
	node->LclRotation.Set(lm.GetR());
	node->LclScaling.Set(FbxDouble3(1, 1, 1));
	parent->AddChild(node);
	return node;
}

} // namespace nm
