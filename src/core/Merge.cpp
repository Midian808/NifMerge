#include "Merge.hpp"

#include "Humanoid.hpp"
#include "Morphs.hpp"
#include "NifIO.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
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
		if (a.triSlots != b.triSlots) throw Error(sl + ": body slots (partitions) differ between weight files");

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
// Only faces marked in `faces` contribute, and only vertices marked in `assign` are given a normal.
void ComputeMissingNormals(MergedModel& m, const std::vector<bool>& needs, const std::vector<bool>& faces,
						   const std::vector<bool>& assign) {
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
	for (size_t f = 0; f < m.tris.size(); f++) {
		const auto& t = m.tris[f];
		if (!faces[f] || !needs[t[0]] || !needs[t[1]] || !needs[t[2]]) continue;
		Vec3 n = (m.positions[t[1]] - m.positions[t[0]]).cross(m.positions[t[2]] - m.positions[t[0]]);
		for (int k = 0; k < 3; k++) acc[key(m.positions[t[k]])] += n;
	}
	for (size_t v = 0; v < m.positions.size(); v++)
		if (needs[v] && assign[v]) {
			auto it = acc.find(key(m.positions[v]));
			Vec3 n = it == acc.end() ? Vec3() : it->second.normalized();
			m.normals[v] = n.length() > 0 ? n : Vec3(0, 0, 1);
		}
}

// Biped slots (30-61) are what armour takes; IDs of 100 and more mark head-part partitions.
constexpr uint16_t kFirstBipedSlot = 30, kLastBipedSlot = 61, kFirstHeadPartId = 100, kHairSlot = 31;
bool IsBipedSlot(uint16_t s) { return s >= kFirstBipedSlot && s <= kLastBipedSlot; }

std::string PartLabel(const PartInput& p) { return FileName(p.path0) + (p.path1 ? " + " + FileName(*p.path1) : ""); }

std::string SlotList(const std::set<uint16_t>& slots) {
	std::string r;
	for (uint16_t s : slots) r += (r.empty() ? "" : ", ") + std::to_string(s);
	return r;
}

// Drops the triangles marked in `drop`, then every vertex no remaining triangle uses, then every
// material slot left without triangles. Slots keep their order and stay contiguous; per-slot vertex
// ranges are recounted.
void RemoveTriangles(MergedModel& m, const std::vector<bool>& drop) {
	std::vector<bool> used(m.positions.size(), false), slotUsed(m.materials.size(), false);
	for (size_t t = 0; t < m.tris.size(); t++)
		if (!drop[t]) {
			for (uint32_t v : m.tris[t]) used[v] = true;
			slotUsed[m.triMaterial[t]] = true;
		}
	std::vector<uint32_t> remap(m.positions.size(), UINT32_MAX);
	std::vector<int> slotRemap(m.materials.size(), -1);
	MergedModel out;
	for (size_t slot = 0; slot < m.materialVertices.size(); slot++) {
		if (!slotUsed[slot]) continue;
		slotRemap[slot] = static_cast<int>(out.materials.size());
		out.materials.push_back(m.materials[slot]);
		auto [first, count] = m.materialVertices[slot];
		const uint32_t base = static_cast<uint32_t>(out.positions.size());
		for (uint32_t v = first; v < first + count; v++) {
			if (!used[v]) continue;
			remap[v] = static_cast<uint32_t>(out.positions.size());
			out.positions.push_back(m.positions[v]);
			out.normals.push_back(m.normals[v]);
			out.uvs.push_back(m.uvs[v]);
			if (!m.colors.empty()) out.colors.push_back(m.colors[v]);
			out.weights.push_back(m.weights[v]);
		}
		out.materialVertices.push_back({base, static_cast<uint32_t>(out.positions.size()) - base});
	}
	for (size_t t = 0; t < m.tris.size(); t++)
		if (!drop[t]) {
			out.tris.push_back({remap[m.tris[t][0]], remap[m.tris[t][1]], remap[m.tris[t][2]]});
			out.triMaterial.push_back(slotRemap[m.triMaterial[t]]);
		}
	m.materials = std::move(out.materials);
	m.positions = std::move(out.positions);
	m.normals = std::move(out.normals);
	m.uvs = std::move(out.uvs);
	m.colors = std::move(out.colors);
	m.weights = std::move(out.weights);
	m.tris = std::move(out.tris);
	m.triMaterial = std::move(out.triMaterial);
	m.materialVertices = std::move(out.materialVertices);
}

// A part's body slots: all the outfit rule needs.
struct PartSlots {
	PartInput input;
	bool outfit = false;
	std::vector<ShapeSlots> shapes;
};

struct OutfitPlan {
	std::vector<std::vector<std::vector<bool>>> hidden; // per part, shape, triangle: hidden by the outfit
	std::vector<PartReport> reports;                    // per part
	std::vector<std::string> lines;                     // per part: what an outfit item does; empty for the character
	std::vector<std::string> warnings;
};

// The outfit rule. Each item takes the biped slots (30-61) of its partitions; two items cannot take one
// slot. Character triangles in a taken slot are hidden, per partition. In a head (a part with head-part
// partitions) only the hair reacts: its eyes, mouth and brows carry slot 32 but are head parts, not
// armour, and a dress never removes them.
OutfitPlan PlanOutfit(const std::vector<PartSlots>& parts) {
	OutfitPlan plan;
	std::vector<std::set<uint16_t>> slots(parts.size());
	std::vector<bool> head(parts.size(), false);
	for (size_t i = 0; i < parts.size(); i++)
		for (auto& sh : parts[i].shapes)
			for (uint16_t slot : sh.triSlots) {
				if (slot) slots[i].insert(slot);
				if (slot >= kFirstHeadPartId) head[i] = true;
			}

	std::map<uint16_t, size_t> takenBy;
	std::vector<std::set<uint16_t>> biped(parts.size());
	for (size_t i = 0; i < parts.size(); i++) {
		if (!parts[i].outfit) continue;
		for (uint16_t slot : slots[i])
			if (IsBipedSlot(slot)) biped[i].insert(slot);
		for (uint16_t slot : biped[i]) {
			auto [it, fresh] = takenBy.emplace(slot, i);
			if (!fresh)
				throw Error("Outfit: " + PartLabel(parts[it->second].input) + " and " + PartLabel(parts[i].input) +
							" both take body slot " + std::to_string(slot) + "; only one item can be worn per slot");
		}
		if (biped[i].empty())
			plan.warnings.push_back("Outfit: " + PartLabel(parts[i].input) +
									" takes no body slot (30-61) in its partitions; it is added over the character");
	}

	std::map<size_t, std::vector<std::string>> replaces; // outfit part -> what it hides
	plan.hidden.resize(parts.size());
	for (size_t i = 0; i < parts.size(); i++) {
		const PartSlots& part = parts[i];
		PartReport report;
		report.file = part.input.path0;
		report.role = part.outfit ? "outfit" : "character";
		report.slots.assign(slots[i].begin(), slots[i].end());
		report.head = head[i];
		plan.hidden[i].resize(part.shapes.size());
		for (size_t k = 0; k < part.shapes.size(); k++) {
			const ShapeSlots& sh = part.shapes[k];
			auto& hide = plan.hidden[i][k];
			hide.assign(sh.triSlots.size(), false);
			if (part.outfit) continue;
			std::map<uint16_t, size_t> count;
			for (size_t t = 0; t < sh.triSlots.size(); t++) {
				const uint16_t slot = sh.triSlots[t];
				if (!takenBy.count(slot) || (head[i] && slot != kHairSlot)) continue;
				hide[t] = true;
				count[slot]++;
			}
			size_t total = 0;
			for (auto& [slot, n] : count) total += n;
			const bool whole = total == sh.triSlots.size();
			for (auto& [slot, n] : count) {
				const size_t by = takenBy[slot];
				report.removed.push_back({sh.shape, slot, n, whole, parts[by].input.path0});
				replaces[by].push_back(FileName(part.input.path0) + " / " + sh.shape +
									   (whole ? "" : " (" + std::to_string(n) + " triangles in slot " + std::to_string(slot) + ")"));
			}
		}
		plan.reports.push_back(std::move(report));
	}
	plan.lines.resize(parts.size());
	for (size_t i = 0; i < parts.size(); i++) {
		if (!parts[i].outfit) continue;
		std::string what;
		for (auto& r : replaces[i]) what += (what.empty() ? "" : ", ") + r;
		plan.lines[i] = "Outfit: " + PartLabel(parts[i].input) + (biped[i].empty() ? "" : " takes slot " + SlotList(biped[i])) +
						": " + (what.empty() ? "added over the character" : "replaces " + what);
	}
	return plan;
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

std::vector<std::string> DescribeOutfit(const std::vector<PartInput>& character, const std::vector<PartInput>& outfit) {
	std::vector<PartSlots> parts;
	for (auto& p : character) parts.push_back({p, false, ReadShapeSlots(p.path0)});
	for (auto& p : outfit) parts.push_back({p, true, ReadShapeSlots(p.path0)});
	OutfitPlan plan = PlanOutfit(parts);
	std::vector<std::string> lines(plan.lines.begin() + character.size(), plan.lines.end());
	return lines;
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

	struct Loaded {
		PartInput input;
		bool outfit = false;
		std::vector<PosedShape> shapes;
	};
	std::vector<Loaded> loaded;
	auto load = [&](const PartInput& part, bool outfit) {
		Loaded l{part, outfit};
		const std::string what = outfit ? "Wearing " : "Loading ";
		if (part.path1) {
			log(LogLevel::Info, what + PartLabel(part) + " at weight " + std::to_string(static_cast<int>(req.weight)));
			l.shapes = BlendWeights(LoadPosedShapes(part.path0, skel, log), LoadPosedShapes(*part.path1, skel, log),
									req.weight, part, log);
		}
		else {
			log(LogLevel::Info, what + PartLabel(part));
			l.shapes = LoadPosedShapes(part.path0, skel, log);
		}
		for (auto& s : l.shapes) s.material.role = outfit ? "outfit" : "character";
		loaded.push_back(std::move(l));
	};
	for (const PartInput& part : req.parts) load(part, false);
	for (const PartInput& part : req.outfit) load(part, true);

	std::vector<PartSlots> slotsIn;
	for (auto& l : loaded) {
		PartSlots ps{l.input, l.outfit};
		for (auto& sh : l.shapes) ps.shapes.push_back({sh.material.shapeName, sh.triSlots});
		slotsIn.push_back(std::move(ps));
	}
	OutfitPlan plan = PlanOutfit(slotsIn);
	for (auto& w : plan.warnings) log(LogLevel::Warning, w);
	for (auto& line : plan.lines)
		if (!line.empty()) log(LogLevel::Info, line);
	const auto& hidden = plan.hidden;
	MergedModel m;
	m.parts = std::move(plan.reports);

	// Every shape goes into the merge; hidden triangles stay until the normals are computed and are
	// removed afterwards (with their unused vertices, and shapes left with nothing).
	std::vector<PosedShape> shapes;
	std::vector<bool> dropTri, wholeHidden;
	for (size_t i = 0; i < loaded.size(); i++)
		for (size_t k = 0; k < loaded[i].shapes.size(); k++) {
			const auto& h = hidden[i][k];
			PosedShape& s = loaded[i].shapes[k];
			std::set<uint16_t> kept;
			for (size_t t = 0; t < s.tris.size(); t++)
				if (!h[t] && s.triSlots[t]) kept.insert(s.triSlots[t]);
			s.material.slots.assign(kept.begin(), kept.end());
			dropTri.insert(dropTri.end(), h.begin(), h.end());
			wholeHidden.push_back(std::all_of(h.begin(), h.end(), [](bool b) { return b; }));
			shapes.push_back(std::move(s));
		}

	// Output bones: every weighted bone, every Humanoid bone, and all their ancestors below the
	// skeleton file's own root node (which is just the file's container).
	std::vector<bool> keep(skel.bones.size(), false);
	auto keepChain = [&](int i) {
		for (; i > 0 && !keep[i]; i = skel.bones[i].parent) keep[i] = true;
	};
	size_t triBase = 0;
	for (auto& s : shapes) {
		std::vector<bool> used(s.positions.size(), false);
		for (size_t t = 0; t < s.tris.size(); t++)
			if (!dropTri[triBase + t])
				for (uint32_t v : s.tris[t]) used[v] = true;
		triBase += s.tris.size();
		for (size_t v = 0; v < s.weights.size(); v++) {
			if (!used[v]) continue;
			for (auto& p : s.weights[v]) {
				if (p.first == 0)
					throw Error(FileName(s.material.sourceFile) + " / " + s.material.shapeName +
								": weighted to the skeleton file's root node '" + skel.bones[0].name +
								"', which is not a bone");
				keepChain(p.first);
			}
		}
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
	std::vector<bool> needsNormal, replacedSlot;
	std::unordered_set<std::string> usedNames;
	for (size_t si = 0; si < shapes.size(); si++) {
		const PosedShape& s = shapes[si];
		MaterialSlot mat = s.material;
		std::string name = mat.name.empty() ? "Material" : mat.name;
		if (!wholeHidden[si]) { // a shape the outfit hides entirely leaves the model; it takes no name
			for (int n = 2; usedNames.count(name); n++) name = mat.name + "_" + std::to_string(n);
			usedNames.insert(name);
		}
		mat.name = name;
		const int slot = static_cast<int>(m.materials.size());
		m.materials.push_back(mat);
		replacedSlot.push_back(wholeHidden[si]);

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

	// Normals come from the surface as worn. A shape the outfit replaces entirely does not count: the
	// item's own surface (the dress's body copy) takes its place at the wrists, neck and ankles. A shape
	// only partly hidden counts in full: hiding a partition cuts no seam into the surface, so what stays
	// visible shades as it does without the outfit.
	std::vector<bool> faces(m.tris.size()), all(m.positions.size(), true);
	for (size_t t = 0; t < m.tris.size(); t++) faces[t] = !replacedSlot[m.triMaterial[t]];
	ComputeMissingNormals(m, needsNormal, faces, all);
	if (std::any_of(dropTri.begin(), dropTri.end(), [](bool b) { return b; })) RemoveTriangles(m, dropTri);
	if (!req.morphPaths.empty()) ApplyMorphs(m, req.morphPaths, log);

	log(LogLevel::Info, "Merged " + std::to_string(m.materials.size()) + " shapes: " + std::to_string(m.positions.size()) +
							" vertices, " + std::to_string(m.tris.size()) + " triangles, " + std::to_string(m.bones.size()) +
							" bones");
	return m;
}

} // namespace nm
