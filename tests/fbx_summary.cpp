// Prints a canonical, run-independent summary of an FBX file (FBX object IDs differ between runs,
// so files are compared through this). Used by tests/run_gui_test.py.
#include <fbxsdk.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

namespace {

// Order-dependent checksum of values rounded to 1e-6.
struct Sum {
	uint64_t h = 1469598103934665603ULL;
	void add(double v) {
		int64_t q = static_cast<int64_t>(std::llround(v * 1e6));
		for (int i = 0; i < 8; i++) {
			h ^= static_cast<uint8_t>(q >> (i * 8));
			h *= 1099511628211ULL;
		}
	}
};

} // namespace

int main(int argc, char** argv) {
	if (argc != 2) {
		std::printf("usage: fbx_summary <file.fbx>\n");
		return 2;
	}
	FbxManager* mgr = FbxManager::Create();
	FbxIOSettings* ios = FbxIOSettings::Create(mgr, IOSROOT);
	mgr->SetIOSettings(ios);
	FbxImporter* imp = FbxImporter::Create(mgr, "");
	if (!imp->Initialize(argv[1], -1, ios)) {
		std::printf("cannot open %s\n", argv[1]);
		return 1;
	}
	FbxScene* scene = FbxScene::Create(mgr, "");
	imp->Import(scene);
	imp->Destroy();

	std::function<void(FbxNode*, int)> walk = [&](FbxNode* n, int depth) {
		FbxDouble3 t = n->LclTranslation.Get(), r = n->LclRotation.Get();
		std::printf("%*s%s T(%.6f %.6f %.6f) R(%.4f %.4f %.4f)\n", depth * 2, "", n->GetName(), t[0], t[1], t[2], r[0],
					r[1], r[2]);
		if (FbxMesh* m = n->GetMesh()) {
			Sum pos, nrm, uv, mat, wts;
			for (int i = 0; i < m->GetControlPointsCount(); i++)
				for (int k = 0; k < 3; k++) pos.add(m->GetControlPointAt(i)[k]);
			if (auto* e = m->GetElementNormal())
				for (int i = 0; i < e->GetDirectArray().GetCount(); i++)
					for (int k = 0; k < 3; k++) nrm.add(e->GetDirectArray().GetAt(i)[k]);
			if (auto* e = m->GetElementUV())
				for (int i = 0; i < e->GetDirectArray().GetCount(); i++)
					for (int k = 0; k < 2; k++) uv.add(e->GetDirectArray().GetAt(i)[k]);
			if (auto* e = m->GetElementMaterial())
				for (int i = 0; i < e->GetIndexArray().GetCount(); i++) mat.add(e->GetIndexArray().GetAt(i));
			for (int i = 0; i < m->GetPolygonCount(); i++)
				for (int k = 0; k < 3; k++) mat.add(m->GetPolygonVertex(i, k));
			if (auto* skin = static_cast<FbxSkin*>(m->GetDeformer(0, FbxDeformer::eSkin))) {
				for (int c = 0; c < skin->GetClusterCount(); c++) {
					FbxCluster* cl = skin->GetCluster(c);
					std::printf("%*s  cluster %s -> %s points=%d\n", depth * 2, "", cl->GetName(), cl->GetLink()->GetName(),
								cl->GetControlPointIndicesCount());
					for (int i = 0; i < cl->GetControlPointIndicesCount(); i++) {
						wts.add(cl->GetControlPointIndices()[i]);
						wts.add(cl->GetControlPointWeights()[i]);
					}
				}
			}
			std::printf("%*s  mesh points=%d polys=%d pos=%016llx nrm=%016llx uv=%016llx tri=%016llx wts=%016llx\n",
						depth * 2, "", m->GetControlPointsCount(), m->GetPolygonCount(), (unsigned long long)pos.h,
						(unsigned long long)nrm.h, (unsigned long long)uv.h, (unsigned long long)mat.h,
						(unsigned long long)wts.h);
			for (int i = 0; i < n->GetMaterialCount(); i++)
				std::printf("%*s  material %d %s\n", depth * 2, "", i, n->GetMaterial(i)->GetName());
		}
		for (int i = 0; i < n->GetChildCount(); i++) walk(n->GetChild(i), depth + 1);
	};
	walk(scene->GetRootNode(), 0);
	mgr->Destroy();
	return 0;
}
