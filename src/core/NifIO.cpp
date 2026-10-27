#include "NifIO.hpp"

#include "NifFile.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <sstream>
#include <unordered_map>

namespace nm {

using namespace nifly;

namespace {

Xform ToXform(const MatTransform& m) {
	Xform x;
	for (int r = 0; r < 3; r++) {
		const Vector3& row = m.rotation[r];
		x.L.m[r][0] = row.x * m.scale;
		x.L.m[r][1] = row.y * m.scale;
		x.L.m[r][2] = row.z * m.scale;
	}
	x.t = {m.translation.x, m.translation.y, m.translation.z};
	return x;
}

void LoadNifOrThrow(NifFile& nif, const std::string& path) {
	if (!std::filesystem::exists(std::filesystem::u8path(path)))
		throw Error("File not found: " + path);
	if (nif.Load(std::filesystem::u8path(path)) != 0 || !nif.IsValid())
		throw Error("Not a readable NIF file: " + path);
}

// World transform of a block in its own nif: its transform composed with every ancestor's.
Xform NifWorld(const NifFile& nif, NiAVObject* obj) {
	Xform w = ToXform(obj->GetTransformToParent());
	for (NiNode* p = nif.GetParentNode(obj); p; p = nif.GetParentNode(p))
		w = ToXform(p->GetTransformToParent()) * w;
	return w;
}

MaterialSlot ReadMaterial(NifFile& nif, NiShape* shape, const std::string& path) {
	MaterialSlot m;
	m.sourceFile = path;
	m.shapeName = shape->name.get();
	m.name = m.shapeName;

	if (NiShader* shader = nif.GetShader(shape)) {
		m.shaderBlock = shader->GetBlockName();
		m.shaderType = shader->GetShaderType();
		if (auto* bss = dynamic_cast<BSShaderProperty*>(shader)) {
			m.shaderFlags1 = bss->shaderFlags1;
			m.shaderFlags2 = bss->shaderFlags2;
		}
		Vector2 off = shader->GetUVOffset(), sc = shader->GetUVScale();
		m.uvOffset[0] = off.u;
		m.uvOffset[1] = off.v;
		m.uvScale[0] = sc.u;
		m.uvScale[1] = sc.v;
		m.glossiness = shader->GetGlossiness();
		Vector3 spec = shader->GetSpecularColor();
		m.specularColor[0] = spec.x;
		m.specularColor[1] = spec.y;
		m.specularColor[2] = spec.z;
		m.specularStrength = shader->GetSpecularStrength();
		Color4 em = shader->GetEmissiveColor();
		m.emissiveColor[0] = em.r;
		m.emissiveColor[1] = em.g;
		m.emissiveColor[2] = em.b;
		m.emissiveMultiple = shader->GetEmissiveMultiple();
		m.alpha = shader->GetAlpha();
		m.doubleSided = shader->IsDoubleSided();
		m.modelSpaceNormals = shader->IsModelSpace();
		m.vertexColors = shader->HasVertexColors();
		m.vertexAlpha = shader->HasVertexAlpha();
		if (auto* bsl = dynamic_cast<BSLightingShaderProperty*>(shader)) {
			m.skinTintColor[0] = bsl->skinTintColor.x;
			m.skinTintColor[1] = bsl->skinTintColor.y;
			m.skinTintColor[2] = bsl->skinTintColor.z;
			m.hairTintColor[0] = bsl->hairTintColor.x;
			m.hairTintColor[1] = bsl->hairTintColor.y;
			m.hairTintColor[2] = bsl->hairTintColor.z;
		}
		if (auto* texSet = nif.GetHeader().GetBlock(shader->TextureSetRef())) {
			for (auto& t : texSet->textures) m.textures.push_back(t.get());
		}
		else {
			// Effect shaders and pre-Skyrim texturing keep their paths elsewhere; GetTextureSlot knows where.
			for (uint32_t i = 0; i < 10; i++) {
				std::string tex;
				nif.GetTextureSlot(shape, tex, i);
				m.textures.push_back(tex);
			}
			while (!m.textures.empty() && m.textures.back().empty()) m.textures.pop_back();
		}
	}
	if (NiAlphaProperty* a = nif.GetAlphaProperty(shape)) {
		m.hasAlphaProperty = true;
		m.alphaFlags = a->flags;
		m.alphaThreshold = a->threshold;
	}
	return m;
}


} // namespace

int ResolveBone(const Skeleton& skel, const std::string& name) {
	int i = skel.find(name);
	if (i < 0 && name == "Head") i = skel.find("NPC Head [Head]");
	return i;
}

Skeleton LoadSkeleton(const std::string& path) {
	NifFile nif;
	LoadNifOrThrow(nif, path);
	Skeleton skel;
	skel.path = path;

	std::vector<std::pair<NiNode*, int>> stack{{nif.GetRootNode(), -1}};
	if (!stack[0].first) throw Error("Skeleton has no root node: " + path);
	while (!stack.empty()) {
		auto [node, parent] = stack.back();
		stack.pop_back();
		Skeleton::Bone b;
		b.name = node->name.get();
		b.parent = parent;
		Xform local = ToXform(node->GetTransformToParent());
		b.world = parent < 0 ? local : skel.bones[parent].world * local;
		int idx = static_cast<int>(skel.bones.size());
		skel.bones.push_back(b);
		skel.index.emplace(b.name, idx); // first occurrence wins
		auto children = nif.GetChildren<NiNode>(node);
		for (auto it = children.rbegin(); it != children.rend(); ++it) stack.push_back({*it, idx});
	}
	if (skel.bones.size() < 2) throw Error("Skeleton contains no bones: " + path);
	return skel;
}

// Body slot of each triangle, from the shape's dismember partitions (0 when it has none).
std::vector<uint16_t> TriangleSlots(NifFile& nif, NiShape* shape, size_t triangles, const std::string& label) {
	std::vector<uint16_t> slots(triangles, 0);
	NiVector<BSDismemberSkinInstance::PartitionInfo> partitions;
	std::vector<int> triParts;
	if (!nif.GetShapePartitions(shape, partitions, triParts)) return slots;
	if (triParts.size() != triangles)
		throw Error(label + ": its partitions list " + std::to_string(triParts.size()) + " triangles, the shape has " +
					std::to_string(triangles));
	for (size_t t = 0; t < triParts.size(); t++)
		if (triParts[t] >= 0 && static_cast<uint32_t>(triParts[t]) < partitions.size()) slots[t] = partitions[triParts[t]].partID;
	return slots;
}

std::vector<ShapeSlots> ReadShapeSlots(const std::string& path) {
	NifFile nif;
	LoadNifOrThrow(nif, path);
	const std::string file = std::filesystem::u8path(path).filename().u8string();
	std::vector<ShapeSlots> out;
	for (NiShape* shape : nif.GetShapes()) {
		std::vector<Triangle> tris;
		shape->GetTriangles(tris);
		if (tris.empty()) continue;
		const std::string name = shape->name.get();
		out.push_back({name, TriangleSlots(nif, shape, tris.size(), file + " / " + name)});
	}
	return out;
}

std::vector<PosedShape> LoadPosedShapes(const std::string& path, const Skeleton& skel, const LogFn& log) {
	NifFile nif;
	LoadNifOrThrow(nif, path);
	std::vector<PosedShape> out;
	const std::string file = std::filesystem::u8path(path).filename().u8string();

	for (NiShape* shape : nif.GetShapes()) {
		PosedShape s;
		s.material = ReadMaterial(nif, shape, path);
		const std::string label = file + " / " + s.material.shapeName;

		// Vertex positions. FaceGen heads (BSDynamicTriShape) keep the real, morphed positions in
		// their dynamic data block; the static vertex data is not authoritative for them.
		std::vector<Vector3> verts;
		if (auto* dyn = dynamic_cast<BSDynamicTriShape*>(shape); dyn && !dyn->dynamicData.empty()) {
			for (auto& d : dyn->dynamicData) verts.emplace_back(d.x, d.y, d.z);
		}
		else if (auto* v = nif.GetVertsForShape(shape)) {
			verts = *v;
		}
		std::vector<Triangle> tris;
		shape->GetTriangles(tris);
		if (verts.empty() || tris.empty()) {
			log(LogLevel::Warning, label + ": no geometry, skipped");
			continue;
		}
		const size_t nv = verts.size();

		for (auto& t : tris) {
			if (t.p1 >= nv || t.p2 >= nv || t.p3 >= nv) throw Error(label + ": triangle index out of range");
			s.tris.push_back({t.p1, t.p2, t.p3});
		}
		s.triSlots = TriangleSlots(nif, shape, s.tris.size(), label);
		if (auto* uv = nif.GetUvsForShape(shape); uv && uv->size() == nv) {
			for (auto& u : *uv) s.uvs.push_back({u.u, u.v});
		}
		else {
			log(LogLevel::Warning, label + ": no UVs, using (0,0)");
			s.uvs.assign(nv, {0.f, 0.f});
		}
		if (shape->HasVertexColors()) {
			if (auto* c = nif.GetColorsForShape(shape); c && c->size() == nv)
				for (auto& col : *c) s.colors.push_back({col.r, col.g, col.b, col.a});
		}
		const std::vector<Vector3>* srcNormals = nif.GetNormalsForShape(shape);
		if (srcNormals && srcNormals->size() != nv) srcNormals = nullptr;

		// Per-vertex blended transform and weights.
		std::vector<Xform> vxf(nv);
		s.weights.assign(nv, {});

		if (shape->IsSkinned()) {
			std::vector<std::string> boneNames;
			nif.GetShapeBoneList(shape, boneNames);
			std::vector<std::string> missing;
			std::vector<int> skelIdx(boneNames.size(), -1);
			std::vector<Xform> K(boneNames.size());
			for (uint32_t i = 0; i < boneNames.size(); i++) {
				skelIdx[i] = ResolveBone(skel, boneNames[i]);
				if (skelIdx[i] < 0) {
					missing.push_back(boneNames[i]);
					continue;
				}
				MatTransform s2b;
				nif.GetShapeTransformSkinToBone(shape, i, s2b);
				K[i] = skel.bones[skelIdx[i]].world * ToXform(s2b);
			}
			if (!missing.empty()) {
				std::string msg = label + ": bones not present in the skeleton (" + skel.path + "):";
				for (auto& m : missing) msg += "\n    '" + m + "'";
				throw Error(msg);
			}
			for (uint32_t i = 0; i < boneNames.size(); i++) {
				std::unordered_map<uint16_t, float> w;
				nif.GetShapeBoneWeights(shape, i, w);
				for (auto& [vi, wt] : w)
					if (vi < nv && wt > 0.f) s.weights[vi].push_back({static_cast<int>(i), wt});
			}

			size_t unweighted = 0;
			int fallbackBone = 0;
			for (size_t v = 0; v < nv; v++) {
				auto& vw = s.weights[v];
				double sum = 0;
				for (auto& p : vw) sum += p.second;
				if (sum <= 0) {
					// The game would collapse such a vertex; keep it on the shape's first bone instead.
					unweighted++;
					vw = {{fallbackBone, 1.f}};
					sum = 1;
				}
				Xform M{Mat3::zero(), Vec3()};
				for (auto& p : vw) {
					double wn = p.second / sum;
					M.L += K[p.first].L * wn;
					M.t += K[p.first].t * wn;
					p.second = static_cast<float>(wn);
				}
				vxf[v] = M;
				// Re-index weights from shape-bone index to skeleton index.
				for (auto& p : vw) p.first = skelIdx[p.first];
				std::sort(vw.begin(), vw.end(), [](auto& a, auto& b) { return a.second > b.second; });
			}
			if (unweighted)
				log(LogLevel::Warning, label + ": " + std::to_string(unweighted) +
										   " vertices had no bone weights; bound to '" + boneNames[fallbackBone] + "'");
		}
		else {
			// Unskinned: rigidly attach to the nearest ancestor node that the skeleton has, placed
			// where the skeleton puts that node; otherwise to the skeleton's top bone.
			Xform shapeWorld = NifWorld(nif, shape);
			int bone = -1;
			Xform M = shapeWorld;
			for (NiNode* p = nif.GetParentNode(shape); p; p = nif.GetParentNode(p)) {
				int si = ResolveBone(skel, p->name.get());
				if (si > 0) {
					bone = si;
					M = skel.bones[si].world * NifWorld(nif, p).inverse() * shapeWorld;
					break;
				}
			}
			if (bone < 0) {
				bone = skel.find("NPC Root [Root]");
				if (bone < 0) bone = 1;
			}
			log(LogLevel::Info, label + ": unskinned, attached rigidly to '" + skel.bones[bone].name + "'");
			for (size_t v = 0; v < nv; v++) {
				vxf[v] = M;
				s.weights[v] = {{bone, 1.f}};
			}
		}

		s.positions.resize(nv);
		for (size_t v = 0; v < nv; v++) s.positions[v] = vxf[v].point({verts[v].x, verts[v].y, verts[v].z});

		// Rotation from the shape's skin space (where a model-space normal map lives) to the rest pose:
		// the average of the per-vertex rotations. Consistently bound shapes have one rotation.
		{
			Mat3 sum = Mat3::zero();
			for (auto& x : vxf) sum += x.L.orthonormalized();
			s.material.skinToRest = sum.orthonormalized();
			double worst = 0;
			for (auto& x : vxf) {
				Mat3 d = s.material.skinToRest.transposed() * x.L.orthonormalized();
				double c = std::clamp((d.m[0][0] + d.m[1][1] + d.m[2][2] - 1.0) / 2.0, -1.0, 1.0);
				worst = std::max(worst, std::acos(c) * 57.29577951308232);
			}
			if (s.material.modelSpaceNormals && worst > 2.0)
				log(LogLevel::Warning, label + ": skin space rotates by up to " + std::to_string(worst) +
										   " degrees across the shape; its model-space normal map is converted with the average rotation");
		}
		if (srcNormals) {
			s.normals.resize(nv);
			for (size_t v = 0; v < nv; v++) {
				const Vector3& n = (*srcNormals)[v];
				// Normals transform by the inverse-transpose of the linear part (exact under scale).
				s.normals[v] = (vxf[v].L.inverse().transposed() * Vec3(n.x, n.y, n.z)).normalized();
			}
		}
		else {
			// Model-space-normal-mapped skin (body, hands, feet, FaceGen head) stores no vertex normals.
			// They are computed after merging, so smoothing can cross UV seams and part seams.
			log(LogLevel::Info, label + ": no vertex normals stored (model-space normal map: " +
									(s.material.modelSpaceNormals ? "yes" : "no") + "); computing smooth normals");
		}
		out.push_back(std::move(s));
	}
	if (out.empty()) throw Error("No usable shapes in " + path);
	return out;
}

std::string InspectNif(const std::string& path, const Skeleton* skel) {
	NifFile nif;
	LoadNifOrThrow(nif, path);
	std::ostringstream os;
	auto ver = nif.GetHeader().GetVersion();
	os << "== " << path << "  (user " << ver.User() << ", stream " << ver.Stream() << ")\n";

	std::function<void(NiNode*, int)> tree = [&](NiNode* n, int depth) {
		os << std::string(depth * 2, ' ') << n->name.get() << " [" << n->GetBlockName() << "]\n";
		for (auto* c : nif.GetChildren<NiNode>(n)) tree(c, depth + 1);
		for (auto* c : nif.GetChildren<NiShape>(n))
			os << std::string(depth * 2 + 2, ' ') << "<shape> " << c->name.get() << " [" << c->GetBlockName()
			   << "] verts=" << c->GetNumVertices() << (c->IsSkinned() ? " skinned" : "") << "\n";
	};
	os << "-- node tree\n";
	tree(nif.GetRootNode(), 0);

	os << "-- shapes\n";
	for (NiShape* s : nif.GetShapes()) {
		MaterialSlot m = ReadMaterial(nif, s, path);
		os << "SHAPE '" << m.shapeName << "' [" << s->GetBlockName() << "] shader=" << m.shaderBlock
		   << " type=" << m.shaderType << " modelSpaceNormals=" << m.modelSpaceNormals
		   << " alpha=" << (m.hasAlphaProperty ? std::to_string(m.alphaFlags) : "none") << "\n";
		for (size_t i = 0; i < m.textures.size(); i++)
			if (!m.textures[i].empty()) os << "    tex[" << i << "] " << m.textures[i] << "\n";
		if (!s->IsSkinned() || !skel) continue;
		std::vector<std::string> bones;
		nif.GetShapeBoneList(s, bones);
		std::map<std::string, int> distinct;
		for (uint32_t i = 0; i < bones.size(); i++) {
			int si = ResolveBone(*skel, bones[i]);
			if (si < 0) {
				os << "    bone '" << bones[i] << "' MISSING from skeleton\n";
				continue;
			}
			MatTransform s2b;
			nif.GetShapeTransformSkinToBone(s, i, s2b);
			Xform k = skel->bones[si].world * ToXform(s2b);
			char key[160];
			snprintf(key, sizeof key, "T=(%.2f %.2f %.2f) S=%.4f", k.t.x + 0.0, k.t.y + 0.0, k.t.z + 0.0,
					 k.L.col(0).length());
			distinct[key]++;
		}
		os << "    skin->world implied by its " << bones.size() << " bones (count: transform):\n";
		for (auto& [k, c] : distinct) os << "      " << c << ": " << k << "\n";
	}
	return os.str();
}

} // namespace nm
