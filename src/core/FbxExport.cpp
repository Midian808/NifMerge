#include "Export.hpp"
#include "FbxCommon.hpp"

#include <filesystem>

namespace nm {

void WriteFbx(const MergedModel& m, const std::string& fbxPath, const LogFn& log) {
	if (m.positions.empty()) throw Error("Nothing to write: the merged model is empty");
	FbxSession session("NifMerge");
	FbxScene* scene = session.scene;
	FbxNode* root = scene->GetRootNode();

	// Bones.
	std::vector<FbxNode*> boneNodes(m.bones.size());
	std::vector<Xform> boneWorld(m.bones.size());
	for (size_t i = 0; i < m.bones.size(); i++) {
		const OutBone& b = m.bones[i];
		boneWorld[i] = ToFbxXform(b.world);
		Xform local = b.parent < 0 ? boneWorld[i] : boneWorld[b.parent].inverse() * boneWorld[i];
		boneNodes[i] = AddBoneNode(scene, b.parent < 0 ? root : boneNodes[b.parent], b.name, local, b.parent < 0);
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

	// Facial morphs: one channel per blend shape. Each shape holds a position for every mesh vertex
	// (the base position where the morph does not act); the SDK's sparse-index form does not round-trip.
	if (!m.blendShapes.empty()) {
		FbxBlendShape* blend = FbxBlendShape::Create(scene, (meshName + "_Morphs").c_str());
		for (const BlendShape& b : m.blendShapes) {
			FbxBlendShapeChannel* channel = FbxBlendShapeChannel::Create(scene, b.name.c_str());
			FbxShape* shape = FbxShape::Create(scene, b.name.c_str());
			shape->InitControlPoints(nv);
			FbxVector4* sp = shape->GetControlPoints();
			for (int v = 0; v < nv; v++) sp[v] = cp[v];
			for (size_t i = 0; i < b.vertices.size(); i++) {
				Vec3 p = ToFbxPoint(m.positions[b.vertices[i]] + b.deltas[i]);
				sp[b.vertices[i]] = FbxVector4(p.x, p.y, p.z, 0.0);
			}
			channel->AddTargetShape(shape, 100.0);
			blend->AddBlendShapeChannel(channel);
		}
		mesh->AddDeformer(blend);
	}

	FbxPose* pose = FbxPose::Create(scene, "BindPose");
	pose->SetIsBindPose(true);
	pose->Add(meshNode, FbxMatrix(meshGlobal));
	for (size_t i = 0; i < m.bones.size(); i++) pose->Add(boneNodes[i], FbxMatrix(ToFbxMatrix(boneWorld[i])));
	scene->AddPose(pose);

	session.save(fbxPath);

	log(LogLevel::Info, "Wrote " + fbxPath + " (" + std::to_string(m.materials.size()) + " materials, " +
							std::to_string(clusterCount) + " skinned bones of " + std::to_string(m.bones.size()) + ", " +
							std::to_string(m.blendShapes.size()) + " blend shapes)");
}

} // namespace nm
