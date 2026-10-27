#include "Merge.hpp"

#include "Humanoid.hpp"
#include "Morphs.hpp"
#include "NifIO.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <unordered_set>

namespace nm {

namespace fs = std::filesystem;

namespace {

std::string Lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

std::string FileName(const std::string& p) { return fs::u8path(p).filename().u8string(); }

// Blends the _0 and _1 versions of a part. Both are already re-posed onto the skeleton, so the
// blend happens in rest space exactly as the game blends the two meshes.
std::vector<PosedShape> BlendWeights(std::vector<PosedShape> s0, std::vector<PosedShape> s1, double weight,
									 const PartInput& part, const LogFn& log) {
	const double t = std::clamp(weight, 0.0, 100.0) / 100.0;
	const std::string label = FileName(part.path0) + " + " + FileName(*part.path1);
	if (s0.size() != s1.size())
		throw Error(label + ": the two weight files have different shape counts (" + std::to_string(s0.size()) +
					" vs " + std::to_string(s1.size()) + ")");

	// Match by shape name when names are unique, else by order.
	std::vector<size_t> match(s0.size());
	std::unordered_set<std::string> names0, names1;
	for (auto& s : s0) names0.insert(s.material.shapeName);
	for (auto& s : s1) names1.insert(s.material.shapeName);
	if (names0.size() == s0.size() && names0 == names1) {
		for (size_t i = 0; i < s0.size(); i++)
			for (size_t j = 0; j < s1.size(); j++)
				if (s1[j].material.shapeName == s0[i].material.shapeName) match[i] = j;
	}
	else {
		for (size_t i = 0; i < s0.size(); i++) match[i] = i;
	}

	std::vector<PosedShape> out;
	for (size_t i = 0; i < s0.size(); i++) {
		PosedShape& a = s0[i];
		PosedShape& b = s1[match[i]];
		const std::string sl = label + " / " + a.material.shapeName;
		if (a.positions.size() != b.positions.size())
			throw Error(sl + ": vertex counts differ between weight files (" + std::to_string(a.positions.size()) +
						" vs " + std::to_string(b.positions.size()) + ")");
		if (a.tris != b.tris) throw Error(sl + ": triangle lists differ between weight files");

		PosedShape r = t >= 0.5 ? b : a;
		const bool blendNormals = !a.normals.empty() && !b.normals.empty();
		if (!blendNormals) r.normals.clear(); // computed from the blended positions after merging
		size_t weightDiffs = 0;
		for (size_t v = 0; v < a.positions.size(); v++) {
			r.positions[v] = a.positions[v] * (1 - t) + b.positions[v] * t;
			if (blendNormals) {
				r.normals[v] = (a.normals[v] * (1 - t) + b.normals[v] * t).normalized();
				if (r.normals[v].length() == 0) r.normals[v] = (t >= 0.5 ? b : a).normals[v];
			}
			for (int k = 0; k < 2; k++) r.uvs[v][k] = static_cast<float>(a.uvs[v][k] * (1 - t) + b.uvs[v][k] * t);
			const auto& wa = a.weights[v];
			const auto& wb = b.weights[v];
			bool same = wa.size() == wb.size();
			for (size_t k = 0; same && k < wa.size(); k++)
				same = wa[k].first == wb[k].first && std::fabs(wa[k].second - wb[k].second) < 1e-3f;
			if (!same) weightDiffs++;
		}
		if (weightDiffs)
			log(LogLevel::Warning, sl + ": " + std::to_string(weightDiffs) +
									   " vertices are weighted differently in _0 and _1; using the " +
									   (t >= 0.5 ? "_1" : "_0") + " weights");
		out.push_back(std::move(r));
	}
	return out;
}

// Area-weighted smooth normals for vertices whose nif stored none. Faces are accumulated per
// position (not per vertex), so copies of a vertex split at a UV seam, or shared across a part
// seam (neck, wrists, ankles), all get the same normal and no shading seam appears. Only faces
// whose vertices all need normals contribute, so authored normals (hair, eyes) are left alone.
void ComputeMissingNormals(MergedModel& m, const std::vector<bool>& needs) {
	if (std::none_of(needs.begin(), needs.end(), [](bool b) { return b; })) return;
	struct Key {
		int64_t x, y, z;
		bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
	};
	struct KeyHash {
		size_t operator()(const Key& k) const {
			return std::hash<int64_t>()(k.x * 73856093LL ^ k.y * 19349663LL ^ k.z * 83492791LL);
		}
	};
	constexpr double kWeld = 1e-3; // game units
	auto key = [&](const Vec3& p) {
		return Key{std::llround(p.x / kWeld), std::llround(p.y / kWeld), std::llround(p.z / kWeld)};
	};
	std::unordered_map<Key, Vec3, KeyHash> acc;
	for (auto& t : m.tris) {
		if (!needs[t[0]] || !needs[t[1]] || !needs[t[2]]) continue;
		Vec3 n = (m.positions[t[1]] - m.positions[t[0]]).cross(m.positions[t[2]] - m.positions[t[0]]);
		for (int k = 0; k < 3; k++) acc[key(m.positions[t[k]])] += n;
	}
	for (size_t v = 0; v < m.positions.size(); v++)
		if (needs[v]) {
			auto it = acc.find(key(m.positions[v]));
			Vec3 n = it == acc.end() ? Vec3() : it->second.normalized();
			m.normals[v] = n.length() > 0 ? n : Vec3(0, 0, 1);
		}
}

} // namespace

std::vector<PartInput> PairPartFiles(const std::vector<std::string>& paths) {
	std::vector<PartInput> parts;
	std::set<std::string> seen; // lowercase canonical key per part
	auto exists = [](const std::string& p) { return fs::exists(fs::u8path(p)); };
	auto listed = [&](const std::string& p) {
		for (auto& q : paths)
			if (Lower(fs::u8path(q).lexically_normal().u8string()) == Lower(fs::u8path(p).lexically_normal().u8string()))
				return true;
		return false;
	};

	for (const std::string& p : paths) {
		fs::path fp = fs::u8path(p);
		std::string stem = fp.stem().u8string();
		std::string ext = fp.extension().u8string();
		PartInput part{p, std::nullopt};
		if (stem.size() > 2 && stem[stem.size() - 2] == '_' && (stem.back() == '0' || stem.back() == '1')) {
			std::string base = stem.substr(0, stem.size() - 2);
			std::string p0 = (fp.parent_path() / fs::u8path(base + "_0" + ext)).u8string();
			std::string p1 = (fp.parent_path() / fs::u8path(base + "_1" + ext)).u8string();
			bool has0 = exists(p0) || listed(p0), has1 = exists(p1) || listed(p1);
			if (has0 && has1) part = {p0, p1};
		}
		std::string key = Lower(fs::u8path(part.path0).lexically_normal().u8string());
		if (seen.insert(key).second) parts.push_back(part);
	}
	return parts;
}

MergedModel MergeModel(const MergeRequest& req, const LogFn& log) {
	if (req.skeletonPath.empty()) throw Error("No skeleton selected");
	if (req.parts.empty()) throw Error("No model parts selected");

	Skeleton skel = LoadSkeleton(req.skeletonPath);
	log(LogLevel::Info, "Skeleton: " + FileName(req.skeletonPath) + " (" + std::to_string(skel.bones.size()) + " nodes)");

	{
		std::string missing;
		for (auto& h : HumanoidMap())
			if (h.required && skel.find(h.skyrim) < 0) missing += "\n    " + std::string(h.unity) + " -> '" + h.skyrim + "'";
		if (!missing.empty()) throw Error("Skeleton lacks bones required for a Unity Humanoid:" + missing);
	}

	std::vector<PosedShape> shapes;
	for (const PartInput& part : req.parts) {
		std::vector<PosedShape> ps;
		if (part.path1) {
			log(LogLevel::Info, "Loading " + FileName(part.path0) + " + " + FileName(*part.path1) + " at weight " +
									std::to_string(static_cast<int>(req.weight)));
			ps = BlendWeights(LoadPosedShapes(part.path0, skel, log), LoadPosedShapes(*part.path1, skel, log),
							  req.weight, part, log);
		}
		else {
			log(LogLevel::Info, "Loading " + FileName(part.path0));
			ps = LoadPosedShapes(part.path0, skel, log);
		}
		for (auto& s : ps) shapes.push_back(std::move(s));
	}

	MergedModel m;

	// Output bones: every weighted bone, every Humanoid bone, and all their ancestors below the
	// skeleton file's own root node (which is just the file's container).
	std::vector<bool> keep(skel.bones.size(), false);
	auto keepChain = [&](int i) {
		for (; i > 0 && !keep[i]; i = skel.bones[i].parent) keep[i] = true;
	};
	for (auto& s : shapes)
		for (auto& vw : s.weights)
			for (auto& p : vw) {
				if (p.first == 0)
					throw Error(FileName(s.material.sourceFile) + " / " + s.material.shapeName +
								": weighted to the skeleton file's root node '" + skel.bones[0].name +
								"', which is not a bone");
				keepChain(p.first);
			}
	for (auto& h : HumanoidMap())
		if (int i = skel.find(h.skyrim); i >= 0) keepChain(i);

	std::vector<int> outIndex(skel.bones.size(), -1);
	for (size_t i = 1; i < skel.bones.size(); i++) {
		if (!keep[i]) continue;
		OutBone b;
		b.name = skel.bones[i].name;
		b.parent = outIndex[skel.bones[i].parent]; // -1 when the parent is the file root
		b.world = skel.bones[i].world.rigid();       // scale is baked into the re-posed vertices
		b.skeletonIndex = static_cast<int>(i);
		for (auto& h : HumanoidMap())
			if (b.name == h.skyrim) b.humanoid = h.unity;
		outIndex[i] = static_cast<int>(m.bones.size());
		m.bones.push_back(b);
	}

	bool anyColors = std::any_of(shapes.begin(), shapes.end(), [](auto& s) { return !s.colors.empty(); });
	std::vector<bool> needsNormal;
	std::unordered_set<std::string> usedNames;
	for (auto& s : shapes) {
		MaterialSlot mat = s.material;
		std::string name = mat.name.empty() ? "Material" : mat.name;
		for (int n = 2; usedNames.count(name); n++) name = mat.name + "_" + std::to_string(n);
		usedNames.insert(name);
		mat.name = name;
		const int slot = static_cast<int>(m.materials.size());
		m.materials.push_back(mat);

		const uint32_t base = static_cast<uint32_t>(m.positions.size());
		m.materialVertices.push_back({base, static_cast<uint32_t>(s.positions.size())});
		for (size_t v = 0; v < s.positions.size(); v++) {
			m.positions.push_back(s.positions[v]);
			m.normals.push_back(s.normals.empty() ? Vec3() : s.normals[v]);
			needsNormal.push_back(s.normals.empty());
			m.uvs.push_back(s.uvs[v]);
			if (anyColors) m.colors.push_back(s.colors.empty() ? std::array<float, 4>{1, 1, 1, 1} : s.colors[v]);
			VertexWeights vw = s.weights[v];
			for (auto& p : vw) p.first = outIndex[p.first];
			m.weights.push_back(std::move(vw));
		}
		for (auto& t : s.tris) {
			m.tris.push_back({t[0] + base, t[1] + base, t[2] + base});
			m.triMaterial.push_back(slot);
		}
	}

	ComputeMissingNormals(m, needsNormal);
	if (!req.morphPaths.empty()) ApplyMorphs(m, req.morphPaths, log);

	log(LogLevel::Info, "Merged " + std::to_string(m.materials.size()) + " shapes: " + std::to_string(m.positions.size()) +
							" vertices, " + std::to_string(m.tris.size()) + " triangles, " + std::to_string(m.bones.size()) +
							" bones");
	return m;
}

} // namespace nm
