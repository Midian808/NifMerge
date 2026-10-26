#include "Export.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace nm {

namespace fs = std::filesystem;

namespace {

std::string Quote(const std::string& s) {
	std::string r = "\"";
	for (unsigned char c : s) {
		switch (c) {
			case '"': r += "\\\""; break;
			case '\\': r += "\\\\"; break;
			case '\n': r += "\\n"; break;
			case '\r': r += "\\r"; break;
			case '\t': r += "\\t"; break;
			default:
				if (c < 0x20) {
					char buf[8];
					snprintf(buf, sizeof buf, "\\u%04x", c);
					r += buf;
				}
				else
					r += static_cast<char>(c);
		}
	}
	return r + "\"";
}

std::string Num(double v) {
	char buf[32];
	snprintf(buf, sizeof buf, "%.6g", v);
	return buf;
}

std::string Arr(const float* v, int n) {
	std::string r = "[";
	for (int i = 0; i < n; i++) r += (i ? ", " : "") + Num(v[i]);
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
	o << "  \"fbx\": " << Quote(fs::u8path(fbxPath).filename().u8string()) << ",\n";
	o << "  \"metresPerGameUnit\": " << Num(kMetresPerUnit) << ",\n";

	o << "  \"humanoid\": [\n";
	bool first = true;
	for (auto& b : m.bones) {
		if (b.humanoid.empty()) continue;
		o << (first ? "" : ",\n") << "    {\"human\": " << Quote(b.humanoid) << ", \"bone\": " << Quote(b.name) << "}";
		first = false;
	}
	o << "\n  ],\n";

	o << "  \"materials\": [\n";
	for (size_t i = 0; i < m.materials.size(); i++) {
		const MaterialSlot& s = m.materials[i];
		o << "    {\n";
		o << "      \"slot\": " << i << ",\n";
		o << "      \"name\": " << Quote(s.name) << ",\n";
		o << "      \"sourceFile\": " << Quote(fs::u8path(s.sourceFile).filename().u8string()) << ",\n";
		o << "      \"shape\": " << Quote(s.shapeName) << ",\n";
		o << "      \"shaderBlock\": " << Quote(s.shaderBlock) << ",\n";
		o << "      \"shaderType\": " << s.shaderType << ",\n";
		o << "      \"shaderFlags1\": " << s.shaderFlags1 << ",\n";
		o << "      \"shaderFlags2\": " << s.shaderFlags2 << ",\n";
		o << "      \"textures\": [";
		for (size_t t = 0; t < s.textures.size(); t++) o << (t ? ", " : "") << Quote(s.textures[t]);
		o << "],\n";
		o << "      \"alphaProperty\": " << (s.hasAlphaProperty ? "true" : "false") << ",\n";
		o << "      \"alphaFlags\": " << s.alphaFlags << ",\n";
		o << "      \"alphaThreshold\": " << static_cast<int>(s.alphaThreshold) << ",\n";
		o << "      \"alpha\": " << Num(s.alpha) << ",\n";
		o << "      \"uvOffset\": " << Arr(s.uvOffset, 2) << ",\n";
		o << "      \"uvScale\": " << Arr(s.uvScale, 2) << ",\n";
		o << "      \"glossiness\": " << Num(s.glossiness) << ",\n";
		o << "      \"specularColor\": " << Arr(s.specularColor, 3) << ",\n";
		o << "      \"specularStrength\": " << Num(s.specularStrength) << ",\n";
		o << "      \"emissiveColor\": " << Arr(s.emissiveColor, 3) << ",\n";
		o << "      \"emissiveMultiple\": " << Num(s.emissiveMultiple) << ",\n";
		o << "      \"skinTintColor\": " << Arr(s.skinTintColor, 3) << ",\n";
		o << "      \"hairTintColor\": " << Arr(s.hairTintColor, 3) << ",\n";
		o << "      \"doubleSided\": " << (s.doubleSided ? "true" : "false") << ",\n";
		o << "      \"modelSpaceNormals\": " << (s.modelSpaceNormals ? "true" : "false") << ",\n";
		{
			Mat3 t = MsnTexelToUnityMesh(s.skinToRest);
			o << "      \"msnToMesh\": [";
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 3; c++) o << (r || c ? ", " : "") << Num(std::fabs(t.m[r][c]) < 1e-9 ? 0.0 : t.m[r][c]);
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
	for (auto& p : m.positions) o << "v " << Num(p.x) << " " << Num(p.y) << " " << Num(p.z) << "\n";
	for (auto& t : m.uvs) o << "vt " << Num(t[0]) << " " << Num(1.0 - t[1]) << "\n";
	for (auto& n : m.normals) o << "vn " << Num(n.x) << " " << Num(n.y) << " " << Num(n.z) << "\n";
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
