#include "Export.hpp"
#include "TextUtil.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace nm {

namespace fs = std::filesystem;

namespace {

std::string Arr(const float* v, int n) {
	std::string r = "[";
	for (int i = 0; i < n; i++) r += (i ? ", " : "") + JsonNum(v[i]);
	return r + "]";
}

void WriteFile(const std::string& path, const std::string& text) {
	std::ofstream f(fs::u8path(path), std::ios::binary);
	if (!f) throw Error("Cannot write " + path);
	f << text;
	if (!f) throw Error("Write failed: " + path);
}

} // namespace

Mat3 MsnTexelToUnityMesh(const Mat3& skinToRest) {
	Mat3 swapYZ = Mat3::zero(); // used twice: texel (r, g, b) -> Skyrim (r, b, g); Skyrim -> Unity mesh
	swapYZ.m[0][0] = 1;
	swapYZ.m[1][2] = 1;
	swapYZ.m[2][1] = 1;
	return swapYZ * skinToRest * swapYZ;
}

std::string SidecarPathFor(const std::string& fbxPath) {
	fs::path p = fs::u8path(fbxPath);
	return (p.parent_path() / fs::u8path(p.stem().u8string() + ".materials.json")).u8string();
}

void WriteMaterialsJson(const MergedModel& m, const std::string& fbxPath, const std::string& jsonPath) {
	std::ostringstream o;
	o << "{\n";
	o << "  \"generator\": \"NifMerge\",\n";
	o << "  \"formatVersion\": 2,\n";
	o << "  \"fbx\": " << JsonQuote(fs::u8path(fbxPath).filename().u8string()) << ",\n";
	o << "  \"metresPerGameUnit\": " << JsonNum(kMetresPerUnit) << ",\n";

	o << "  \"humanoid\": [\n";
	bool first = true;
	for (auto& b : m.bones) {
		if (b.humanoid.empty()) continue;
		o << (first ? "" : ",\n") << "    {\"human\": " << JsonQuote(b.humanoid) << ", \"bone\": " << JsonQuote(b.name) << "}";
		first = false;
	}
	o << "\n  ],\n";

	o << "  \"blendShapes\": [";
	for (size_t i = 0; i < m.blendShapes.size(); i++) o << (i ? ", " : "") << JsonQuote(m.blendShapes[i].name);
	o << "],\n";

	o << "  \"morphSources\": [\n";
	for (size_t i = 0; i < m.morphSources.size(); i++) {
		const MorphSource& s = m.morphSources[i];
		o << "    {\"file\": " << JsonQuote(fs::u8path(s.file).filename().u8string()) << ", \"shape\": " << JsonQuote(s.shape)
		  << ", \"mode\": " << JsonQuote(s.mode) << ", \"windowStart\": " << s.windowStart << ", \"median\": " << JsonNum(s.median)
		  << ", \"p90\": " << JsonNum(s.p90) << ", \"max\": " << JsonNum(s.max)
		  << ", \"sameTriangles\": " << (s.sameTriangles ? "true" : "false") << ", \"morphs\": [";
		for (size_t k = 0; k < s.morphs.size(); k++) o << (k ? ", " : "") << JsonQuote(s.morphs[k]);
		o << "], \"outcome\": " << JsonQuote(s.outcome) << "}"
		  << (i + 1 < m.morphSources.size() ? ",\n" : "\n");
	}
	o << "  ],\n";

	o << "  \"parts\": [\n";
	for (size_t i = 0; i < m.parts.size(); i++) {
		const PartReport& r = m.parts[i];
		o << "    {\"file\": " << JsonQuote(fs::u8path(r.file).filename().u8string()) << ", \"role\": " << JsonQuote(r.role)
		  << ", \"head\": " << (r.head ? "true" : "false") << ", \"slots\": [";
		for (size_t k = 0; k < r.slots.size(); k++) o << (k ? ", " : "") << r.slots[k];
		o << "], \"removed\": [";
		for (size_t k = 0; k < r.removed.size(); k++) {
			const auto& d = r.removed[k];
			o << (k ? ", " : "") << "{\"shape\": " << JsonQuote(d.shape) << ", \"slot\": " << d.slot
			  << ", \"triangles\": " << d.triangles << ", \"wholeShape\": " << (d.wholeShape ? "true" : "false")
			  << ", \"by\": " << JsonQuote(fs::u8path(d.by).filename().u8string()) << "}";
		}
		o << "]}" << (i + 1 < m.parts.size() ? ",\n" : "\n");
	}
	o << "  ],\n";

	o << "  \"materials\": [\n";
	for (size_t i = 0; i < m.materials.size(); i++) {
		const MaterialSlot& s = m.materials[i];
		o << "    {\n";
		o << "      \"slot\": " << i << ",\n";
		o << "      \"name\": " << JsonQuote(s.name) << ",\n";
		o << "      \"sourceFile\": " << JsonQuote(fs::u8path(s.sourceFile).filename().u8string()) << ",\n";
		o << "      \"shape\": " << JsonQuote(s.shapeName) << ",\n";
		o << "      \"role\": " << JsonQuote(s.role) << ",\n";
		o << "      \"slots\": [";
		for (size_t k = 0; k < s.slots.size(); k++) o << (k ? ", " : "") << s.slots[k];
		o << "],\n";
		o << "      \"shaderBlock\": " << JsonQuote(s.shaderBlock) << ",\n";
		o << "      \"shaderType\": " << s.shaderType << ",\n";
		o << "      \"shaderFlags1\": " << s.shaderFlags1 << ",\n";
		o << "      \"shaderFlags2\": " << s.shaderFlags2 << ",\n";
		o << "      \"textures\": [";
		for (size_t t = 0; t < s.textures.size(); t++) o << (t ? ", " : "") << JsonQuote(s.textures[t]);
		o << "],\n";
		o << "      \"alphaProperty\": " << (s.hasAlphaProperty ? "true" : "false") << ",\n";
		o << "      \"alphaFlags\": " << s.alphaFlags << ",\n";
		o << "      \"alphaThreshold\": " << static_cast<int>(s.alphaThreshold) << ",\n";
		o << "      \"alpha\": " << JsonNum(s.alpha) << ",\n";
		o << "      \"uvOffset\": " << Arr(s.uvOffset, 2) << ",\n";
		o << "      \"uvScale\": " << Arr(s.uvScale, 2) << ",\n";
		o << "      \"glossiness\": " << JsonNum(s.glossiness) << ",\n";
		o << "      \"specularColor\": " << Arr(s.specularColor, 3) << ",\n";
		o << "      \"specularStrength\": " << JsonNum(s.specularStrength) << ",\n";
		o << "      \"emissiveColor\": " << Arr(s.emissiveColor, 3) << ",\n";
		o << "      \"emissiveMultiple\": " << JsonNum(s.emissiveMultiple) << ",\n";
		o << "      \"skinTintColor\": " << Arr(s.skinTintColor, 3) << ",\n";
		o << "      \"hairTintColor\": " << Arr(s.hairTintColor, 3) << ",\n";
		o << "      \"doubleSided\": " << (s.doubleSided ? "true" : "false") << ",\n";
		o << "      \"modelSpaceNormals\": " << (s.modelSpaceNormals ? "true" : "false") << ",\n";
		{
			Mat3 t = MsnTexelToUnityMesh(s.skinToRest);
			o << "      \"msnToMesh\": [";
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 3; c++) o << (r || c ? ", " : "") << JsonNum(std::fabs(t.m[r][c]) < 1e-9 ? 0.0 : t.m[r][c]);
			o << "],\n";
		}
		o << "      \"vertexColors\": " << (s.vertexColors ? "true" : "false") << ",\n";
		o << "      \"vertexAlpha\": " << (s.vertexAlpha ? "true" : "false") << "\n";
		o << "    }" << (i + 1 < m.materials.size() ? "," : "") << "\n";
	}
	o << "  ]\n}\n";
	WriteFile(jsonPath, o.str());
}

void WriteObj(const MergedModel& m, const std::string& objPath) {
	std::ostringstream o;
	o << "# NifMerge rest-pose dump, Skyrim space (Z-up, game units)\n";
	for (auto& p : m.positions) o << "v " << JsonNum(p.x) << " " << JsonNum(p.y) << " " << JsonNum(p.z) << "\n";
	for (auto& t : m.uvs) o << "vt " << JsonNum(t[0]) << " " << JsonNum(1.0 - t[1]) << "\n";
	for (auto& n : m.normals) o << "vn " << JsonNum(n.x) << " " << JsonNum(n.y) << " " << JsonNum(n.z) << "\n";
	int current = -1;
	for (size_t t = 0; t < m.tris.size(); t++) {
		if (m.triMaterial[t] != current) {
			current = m.triMaterial[t];
			o << "g " << m.materials[current].name << "\nusemtl " << m.materials[current].name << "\n";
		}
		o << "f";
		for (int k = 0; k < 3; k++) {
			uint32_t i = m.tris[t][k] + 1;
			o << " " << i << "/" << i << "/" << i;
		}
		o << "\n";
	}
	WriteFile(objPath, o.str());
}

} // namespace nm
