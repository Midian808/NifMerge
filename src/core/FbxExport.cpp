#include "Export.hpp"

#include <fbxsdk.h>

#include <filesystem>

namespace nm {

namespace {

// Rc maps Skyrim axes to FBX axes: (x, y, z) -> (-x, z, y).
const Mat3 kRc = [] {
	Mat3 r = Mat3::zero();
	r.m[0][0] = -1;
	r.m[1][2] = 1;
	r.m[2][1] = 1;
	return r;
}();

Vec3 ToFbxPoint(const Vec3& p) { return (kRc * p) * kMetresPerUnit; }
Vec3 ToFbxDir(const Vec3& d) { return kRc * d; }
Xform ToFbxXform(const Xform& x) { return {kRc * x.L * kRc.transposed(), ToFbxPoint(x.t)}; }

// FbxAMatrix uses the row-vector convention: rows 0..2 are the basis vectors, row 3 the translation.
FbxAMatrix ToFbxMatrix(const Xform& x) {
	FbxAMatrix m;
	m.SetIdentity();
	for (int i = 0; i < 3; i++) m.SetRow(i, FbxVector4(x.L.m[0][i], x.L.m[1][i], x.L.m[2][i], 0.0));
	m.SetRow(3, FbxVector4(x.t.x, x.t.y, x.t.z, 1.0));
	return m;
}

struct FbxSession {
	FbxManager* mgr = FbxManager::Create();
	~FbxSession() {
		if (mgr) mgr->Destroy();
	}
};

} // namespace

void WriteFbx(const MergedModel& m, const std::string& fbxPath, const LogFn& log) {
	if (m.positions.empty()) throw Error("Nothing to write: the merged model is empty");
	FbxSession session;
	FbxManager* mgr = session.mgr;
	if (!mgr) throw Error("Could not initialise the FBX SDK");
	FbxIOSettings* ios = FbxIOSettings::Create(mgr, IOSROOT);
	mgr->SetIOSettings(ios);

	FbxScene* scene = FbxScene::Create(mgr, "NifMerge");
	scene->GetGlobalSettings().SetAxisSystem(
		FbxAxisSystem(FbxAxisSystem::eYAxis, FbxAxisSystem::eParityOdd, FbxAxisSystem::eRightHanded));
	scene->GetGlobalSettings().SetSystemUnit(FbxSystemUnit::m);
	FbxNode* root = scene->GetRootNode();

	// Bones.
	std::vector<FbxNode*> boneNodes(m.bones.size());
	std::vector<Xform> boneWorld(m.bones.size());
	for (size_t i = 0; i < m.bones.size(); i++) {
		const OutBone& b = m.bones[i];
		boneWorld[i] = ToFbxXform(b.world);
		Xform local = b.parent < 0 ? boneWorld[i] : boneWorld[b.parent].inverse() * boneWorld[i];
		FbxAMatrix lm = ToFbxMatrix(local);

		FbxSkeleton* attr = FbxSkeleton::Create(scene, b.name.c_str());
		attr->SetSkeletonType(b.parent < 0 ? FbxSkeleton::eRoot : FbxSkeleton::eLimbNode);
		attr->Size.Set(1.0);
		FbxNode* node = FbxNode::Create(scene, b.name.c_str());
		node->SetNodeAttribute(attr);
		node->LclTranslation.Set(lm.GetT());
		node->LclRotation.Set(lm.GetR());
		node->LclScaling.Set(FbxDouble3(1, 1, 1));
		(b.parent < 0 ? root : boneNodes[b.parent])->AddChild(node);
		boneNodes[i] = node;
	}

	// Mesh.
	// Unity names the model's root object after the file, so the mesh node must not reuse that name:
	// the Humanoid avatar matches transforms by name.
	const std::string meshName = std::filesystem::u8path(fbxPath).stem().u8string() + "_Mesh";
	FbxMesh* mesh = FbxMesh::Create(scene, meshName.c_str());
	const int nv = static_cast<int>(m.positions.size());
	mesh->InitControlPoints(nv);
	FbxVector4* cp = mesh->GetControlPoints();
	for (int v = 0; v < nv; v++) {
		Vec3 p = ToFbxPoint(m.positions[v]);
		cp[v] = FbxVector4(p.x, p.y, p.z, 0.0);
	}

	// Skyrim shapes are already split wherever UVs or normals differ, so per-control-point
	// attributes are exact.
	FbxGeometryElementNormal* normals = mesh->CreateElementNormal();
	normals->SetMappingMode(FbxGeometryElement::eByControlPoint);
	normals->SetReferenceMode(FbxGeometryElement::eDirect);
	for (int v = 0; v < nv; v++) {
		Vec3 n = ToFbxDir(m.normals[v]);
		normals->GetDirectArray().Add(FbxVector4(n.x, n.y, n.z, 0.0));
	}

	FbxGeometryElementUV* uvs = mesh->CreateElementUV("UVMap");
	uvs->SetMappingMode(FbxGeometryElement::eByControlPoint);
	uvs->SetReferenceMode(FbxGeometryElement::eDirect);
	for (int v = 0; v < nv; v++) // NIF UVs have their origin top-left; FBX bottom-left.
		uvs->GetDirectArray().Add(FbxVector2(m.uvs[v][0], 1.0 - m.uvs[v][1]));

	if (!m.colors.empty()) {
		FbxGeometryElementVertexColor* colors = mesh->CreateElementVertexColor();
		colors->SetMappingMode(FbxGeometryElement::eByControlPoint);
		colors->SetReferenceMode(FbxGeometryElement::eDirect);
		for (auto& c : m.colors) colors->GetDirectArray().Add(FbxColor(c[0], c[1], c[2], c[3]));
	}

	FbxGeometryElementMaterial* matElem = mesh->CreateElementMaterial();
	matElem->SetMappingMode(FbxGeometryElement::eByPolygon);
	matElem->SetReferenceMode(FbxGeometryElement::eIndexToDirect);
	for (size_t t = 0; t < m.tris.size(); t++) {
		mesh->BeginPolygon(m.triMaterial[t]);
		for (int k = 0; k < 3; k++) mesh->AddPolygon(static_cast<int>(m.tris[t][k]));
		mesh->EndPolygon();
	}

	FbxNode* meshNode = FbxNode::Create(scene, meshName.c_str());
	meshNode->SetNodeAttribute(mesh);
	root->AddChild(meshNode);
	for (auto& slot : m.materials) {
		FbxSurfacePhong* mat = FbxSurfacePhong::Create(scene, slot.name.c_str());
		mat->Diffuse.Set(FbxDouble3(1, 1, 1));
		meshNode->AddMaterial(mat);
	}

	// Skin.
	FbxSkin* skin = FbxSkin::Create(scene, (meshName + "_Skin").c_str());
	std::vector<FbxCluster*> clusters(m.bones.size(), nullptr);
	FbxAMatrix meshGlobal;
	meshGlobal.SetIdentity();
	for (int v = 0; v < nv; v++) {
		for (auto& [bone, w] : m.weights[v]) {
			if (!clusters[bone]) {
				FbxCluster* c = FbxCluster::Create(scene, m.bones[bone].name.c_str());
				c->SetLink(boneNodes[bone]);
				c->SetLinkMode(FbxCluster::eNormalize);
				c->SetTransformMatrix(meshGlobal);
				c->SetTransformLinkMatrix(ToFbxMatrix(boneWorld[bone]));
				clusters[bone] = c;
			}
			clusters[bone]->AddControlPointIndex(v, w);
		}
	}
	int clusterCount = 0;
	for (auto* c : clusters)
		if (c) {
			skin->AddCluster(c);
			clusterCount++;
		}
	mesh->AddDeformer(skin);

	FbxPose* pose = FbxPose::Create(scene, "BindPose");
	pose->SetIsBindPose(true);
	pose->Add(meshNode, FbxMatrix(meshGlobal));
	for (size_t i = 0; i < m.bones.size(); i++) pose->Add(boneNodes[i], FbxMatrix(ToFbxMatrix(boneWorld[i])));
	scene->AddPose(pose);

	FbxExporter* exporter = FbxExporter::Create(mgr, "");
	int format = mgr->GetIOPluginRegistry()->GetNativeWriterFormat();
	if (!exporter->Initialize(fbxPath.c_str(), format, ios))
		throw Error("FBX export failed to open '" + fbxPath + "': " + exporter->GetStatus().GetErrorString());
	if (!exporter->Export(scene))
		throw Error("FBX export failed: " + std::string(exporter->GetStatus().GetErrorString()));
	exporter->Destroy();

	log(LogLevel::Info, "Wrote " + fbxPath + " (" + std::to_string(m.materials.size()) + " materials, " +
							std::to_string(clusterCount) + " skinned bones of " + std::to_string(m.bones.size()) + ")");
}

} // namespace nm
