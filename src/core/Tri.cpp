#include "Tri.hpp"

#include "Model.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace nm {

namespace {

struct Reader {
	const std::vector<char>& b;
	size_t o = 0;
	const std::string& path;

	void need(size_t n) const {
		if (o + n > b.size()) throw Error(path + ": truncated TRI file");
	}
	template <typename T> T get() {
		need(sizeof(T));
		T v;
		std::memcpy(&v, b.data() + o, sizeof(T));
		o += sizeof(T);
		return v;
	}
	void skip(size_t n) {
		need(n);
		o += n;
	}
	std::string name() {
		uint32_t len = get<uint32_t>();
		if (len > 4096) throw Error(path + ": implausible morph name length");
		need(len);
		std::string s(b.data() + o, b.data() + o + len);
		o += len;
		if (auto z = s.find('\0'); z != std::string::npos) s.resize(z);
		return s;
	}
};

} // namespace

TriFile ReadTri(const std::string& path) {
	std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
	if (!f) throw Error("Cannot open " + path);
	std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	Reader r{bytes, 0, path};
	r.need(8);
	if (std::memcmp(bytes.data(), "FRTRI003", 8) != 0) throw Error(path + ": not an FRTRI003 morph file");
	r.o = 8;

	const uint32_t numVerts = r.get<uint32_t>(), numTris = r.get<uint32_t>(), numQuads = r.get<uint32_t>();
	r.get<uint32_t>();
	r.get<uint32_t>();
	const uint32_t numUV = r.get<uint32_t>();
	r.get<uint32_t>(); // flags
	const uint32_t numMorphs = r.get<uint32_t>(), numMods = r.get<uint32_t>(), numModVerts = r.get<uint32_t>();
	for (int i = 0; i < 4; i++) r.get<uint32_t>();
	if (numVerts > 1000000 || numTris > 4000000 || numMorphs > 10000 || numMods > 10000)
		throw Error(path + ": implausible TRI header counts");

	TriFile t;
	t.path = path;
	auto vec3f = [&] {
		float x = r.get<float>(), y = r.get<float>(), z = r.get<float>();
		return Vec3(x, y, z);
	};
	t.vertices.reserve(numVerts);
	for (uint32_t i = 0; i < numVerts; i++) t.vertices.push_back(vec3f());
	std::vector<Vec3> modVerts;
	modVerts.reserve(numModVerts);
	for (uint32_t i = 0; i < numModVerts; i++) modVerts.push_back(vec3f());

	auto index = [&] {
		uint32_t v = r.get<uint32_t>();
		if (v >= numVerts) throw Error(path + ": face index out of range");
		return v;
	};
	for (uint32_t i = 0; i < numTris; i++) {
		uint32_t a = index(), b = index(), c = index();
		t.triangles.push_back({a, b, c});
	}
	for (uint32_t i = 0; i < numQuads; i++) {
		uint32_t a = index(), b = index(), c = index(), d = index();
		t.triangles.push_back({a, b, c});
		t.triangles.push_back({a, c, d});
	}
	r.skip(size_t(numUV) * 8);                          // UV coordinates
	r.skip(size_t(numTris) * 12 + size_t(numQuads) * 16); // UV faces

	for (uint32_t m = 0; m < numMorphs; m++) {
		TriMorph morph;
		morph.name = r.name();
		const float mult = r.get<float>();
		morph.deltas.resize(numVerts);
		for (uint32_t i = 0; i < numVerts; i++) {
			int16_t x = r.get<int16_t>(), y = r.get<int16_t>(), z = r.get<int16_t>();
			morph.deltas[i] = Vec3(x * double(mult), y * double(mult), z * double(mult));
		}
		t.morphs.push_back(std::move(morph));
	}
	size_t modVert = 0;
	for (uint32_t m = 0; m < numMods; m++) {
		TriMorph morph;
		morph.name = r.name();
		morph.deltas.assign(numVerts, Vec3());
		const uint32_t count = r.get<uint32_t>();
		for (uint32_t k = 0; k < count; k++) {
			uint32_t v = index();
			if (modVert >= modVerts.size()) throw Error(path + ": modifier morphs use more vertices than stored");
			morph.deltas[v] = modVerts[modVert++] - t.vertices[v];
		}
		t.morphs.push_back(std::move(morph));
	}
	if (r.o != bytes.size())
		throw Error(path + ": " + std::to_string(bytes.size() - r.o) + " unexpected bytes after the last morph");
	return t;
}

} // namespace nm
