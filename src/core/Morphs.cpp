#include "Morphs.hpp"

#include "Tri.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>

namespace nm {

namespace fs = std::filesystem;

namespace {

// Acceptance limits on fit distances (game units), measured on real Skyrim SE head parts: correct
// pairings sit at median 0.11-0.24 / p90 0.25-0.51; wrong ones (male parts on a female head, a male
// mouth on a female mouth) fail p90 or max.
constexpr double kMaxMedian = 0.35, kMaxP90 = 0.6, kMaxIndexMax = 1.2, kMaxSurfaceMax = 0.8;
// Surface transfer. A slot vertex within kTrust of a same-facing mask surface takes the mask's motion
// directly. Every other vertex within kSpread is smoothed across the slot's own mesh: pulled with weight
// kPull towards its nearest-mask-point motion when that point faces the same way and lies within kOuter,
// towards zero beyond kOuter, and not pulled at all when no same-facing surface is near (it then simply
// follows its neighbours). Beyond kSpread nothing moves. Values measured on High Poly Head masks: lip
// surfaces the masks lack sit 0.3-0.95 away and well-fitting skin mostly under 0.25; pure smoothing (no
// pull) let sparsely anchored regions (the inside of the lips) collapse to one motion, pull 2 kept the
// lower lip moving with the fewest left/right disagreements, pull 4 froze it again.
constexpr double kTrust = 0.25, kOuter = 1.5, kPull = 2.0;
// The smoothing target fades from full strength at kFade to zero at kOuter, and smoothing continues to
// kSpread (target zero), so motion eases out across the mesh beyond the masks' edges even where the
// mesh is coarse, instead of stopping at an edge.
constexpr double kFade = 1.0, kSpread = 3.0;
// A vertex's nearest mask point must lie within this many triangle rings (across the mask's own
// surface) of its neighbours' points; otherwise it landed on another part of the mask (the upper lip
// for the lower lip's top, which nearly touch) and is smoothed instead.
constexpr int kContinuityRings = 3;
// Two masks overlap where most of one lies within this distance of the other.
constexpr double kOverlap = 1.0;
constexpr double kFar = 1e9;
// Two surfaces face the same way when their normals are within ~45 degrees (cosine 0.7). Measured on
// High Poly Head masks: at 0.34 (~70 degrees) the top edge of the lower lip still matched the upper
// lip's skin and stayed frozen on jaw-opening morphs; 0.7 freed it with the fewest left/right
// disagreements (0.8 behaved the same with slightly less direct mask motion).
constexpr double kFacing = 0.7;

enum Mode { kNone = 0, kSurface = 1, kWindow = 2, kIndex = 3 }; // higher is a stronger match
const char* ModeName(Mode m) {
	return m == kIndex ? "index" : m == kWindow ? "window" : m == kSurface ? "surface" : "";
}

std::string Lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

struct Stats {
	double median = kFar, p90 = kFar, max = kFar;
};
Stats Summarise(std::vector<double> d) {
	Stats s;
	if (d.empty()) return s;
	std::sort(d.begin(), d.end());
	s.median = d[d.size() / 2];
	s.p90 = d[std::min(d.size() - 1, d.size() * 9 / 10)];
	s.max = d.back();
	return s;
}

// Closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5), with barycentrics.
Vec3 ClosestOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c, double bary[3]) {
	Vec3 ab = b - a, ac = c - a, ap = p - a;
	double d1 = ab.dot(ap), d2 = ac.dot(ap);
	auto set = [&](double u, double v, double w) {
		bary[0] = u;
		bary[1] = v;
		bary[2] = w;
	};
	if (d1 <= 0 && d2 <= 0) return set(1, 0, 0), a;
	Vec3 bp = p - b;
	double d3 = ab.dot(bp), d4 = ac.dot(bp);
	if (d3 >= 0 && d4 <= d3) return set(0, 1, 0), b;
	double vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) {
		double v = d1 / (d1 - d3);
		return set(1 - v, v, 0), a + ab * v;
	}
	Vec3 cp = p - c;
	double d5 = ab.dot(cp), d6 = ac.dot(cp);
	if (d6 >= 0 && d5 <= d6) return set(0, 0, 1), c;
	double vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) {
		double w = d2 / (d2 - d6);
		return set(1 - w, 0, w), a + ac * w;
	}
	double va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
		double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		return set(0, 1 - w, w), b + (c - b) * w;
	}
	double denom = 1.0 / (va + vb + vc), v = vb * denom, w = vc * denom;
	return set(1 - v - w, v, w), a + ab * v + ac * w;
}

// Uniform grid over a triangle set for closest-point queries within a radius.
class TriangleGrid {
public:
	TriangleGrid(const std::vector<Vec3>& pts, const std::vector<std::array<uint32_t, 3>>& tris, double cell)
		: pts_(pts), tris_(tris), cell_(cell) {
		for (uint32_t t = 0; t < tris.size(); t++) {
			Vec3 lo = pts[tris[t][0]], hi = lo;
			for (int k = 1; k < 3; k++) {
				const Vec3& q = pts[tris[t][k]];
				lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
				hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
			}
			auto a = Cell(lo), b = Cell(hi);
			for (int64_t x = a[0]; x <= b[0]; x++)
				for (int64_t y = a[1]; y <= b[1]; y++)
					for (int64_t z = a[2]; z <= b[2]; z++) cells_[Key(x, y, z)].push_back(t);
		}
	}

	// Distance to the closest point on any triangle within `radius`, or kFar. With `facing`, only
	// triangles whose normal (times `normalSign`) points within ~45 degrees of it (kFacing) are considered.
	double Closest(const Vec3& p, double radius, uint32_t* triOut, double bary[3], const Vec3* facing = nullptr,
				   double normalSign = 1.0) const {
		double best = kFar;
		auto a = Cell(p - Vec3(radius, radius, radius)), b = Cell(p + Vec3(radius, radius, radius));
		for (int64_t x = a[0]; x <= b[0]; x++)
			for (int64_t y = a[1]; y <= b[1]; y++)
				for (int64_t z = a[2]; z <= b[2]; z++) {
					auto it = cells_.find(Key(x, y, z));
					if (it == cells_.end()) continue;
					for (uint32_t t : it->second) {
						if (facing && Normal(t).dot(*facing) * normalSign < kFacing) continue;
						double bc[3];
						Vec3 q = ClosestOnTriangle(p, pts_[tris_[t][0]], pts_[tris_[t][1]], pts_[tris_[t][2]], bc);
						double d = (q - p).length();
						if (d < best) {
							best = d;
							if (triOut) *triOut = t;
							if (bary) std::copy(bc, bc + 3, bary);
						}
					}
				}
		return best <= radius ? best : kFar;
	}

	Vec3 Normal(uint32_t t) const {
		const Vec3 &a = pts_[tris_[t][0]], &b = pts_[tris_[t][1]], &c = pts_[tris_[t][2]];
		return (b - a).cross(c - a).normalized();
	}

private:
	std::array<int64_t, 3> Cell(const Vec3& p) const {
		return {static_cast<int64_t>(std::floor(p.x / cell_)), static_cast<int64_t>(std::floor(p.y / cell_)),
				static_cast<int64_t>(std::floor(p.z / cell_))};
	}
	static uint64_t Key(int64_t x, int64_t y, int64_t z) {
		return (uint64_t(x & 0x1FFFFF) << 42) | (uint64_t(y & 0x1FFFFF) << 21) | uint64_t(z & 0x1FFFFF);
	}
	const std::vector<Vec3>& pts_;
	const std::vector<std::array<uint32_t, 3>>& tris_;
	double cell_;
	std::unordered_map<uint64_t, std::vector<uint32_t>> cells_;
};

// One material slot of the merged mesh as a standalone surface in the rest pose.
struct Slot {
	int index = 0;
	uint32_t first = 0, count = 0;
	std::vector<Vec3> pts;
	std::vector<std::array<uint32_t, 3>> tris; // local indices
	Mat3 rot;                                   // skinToRest
	std::unique_ptr<TriangleGrid> grid;
};

struct Candidate {
	Mode mode = kNone;
	int slot = -1;
	uint32_t start = 0;
	Vec3 t; // translation: rest = rot * tri + t
	Stats stats;
	bool sameTriangles = false; // index fit whose triangle list is the slot's, vertex for vertex
};

// The fit distances are within the acceptance limits.
bool WithinLimits(const Candidate& c) {
	if (c.mode == kNone) return false;
	double maxLimit = c.mode == kSurface ? kMaxSurfaceMax : kMaxIndexMax;
	return c.stats.median <= kMaxMedian && c.stats.p90 <= kMaxP90 && c.stats.max <= maxLimit;
}

// An index fit with the slot's own triangles is accepted however far the vertices are apart: identical
// faces prove vertex i is vertex i, and a sculpted FaceGen head moves its vertices well away from the
// TRI's base shape (median 0.41 on the user's High Poly Head) while keeping the base mesh's topology.
bool Accepted(const Candidate& c) { return (c.mode == kIndex && c.sameTriangles) || WithinLimits(c); }

bool Better(const Candidate& a, const Candidate& b) {
	bool aa = Accepted(a), ba = Accepted(b);
	if (aa != ba) return aa;
	if (aa && a.mode != b.mode) return a.mode > b.mode;
	if (a.mode == kNone || b.mode == kNone) return b.mode == kNone && a.mode != kNone;
	return a.stats.median < b.stats.median;
}

// Triangles as a sorted list, each rotated to start at its lowest index (winding kept).
std::vector<std::array<uint32_t, 3>> Faces(std::vector<std::array<uint32_t, 3>> f) {
	for (auto& t : f) std::rotate(t.begin(), std::min_element(t.begin(), t.end()), t.end());
	std::sort(f.begin(), f.end());
	return f;
}

// Index or window fit: translation is the mean offset over the corresponding vertices.
Candidate FitIndexed(const TriFile& tri, const Slot& s, uint32_t start, Mode mode) {
	Candidate c;
	c.mode = mode;
	c.slot = s.index;
	c.start = start;
	c.sameTriangles = mode == kIndex && !tri.triangles.empty() && Faces(tri.triangles) == Faces(s.tris);
	const size_t n = tri.vertices.size();
	Vec3 sum;
	for (size_t i = 0; i < n; i++) sum += s.pts[start + i] - s.rot * tri.vertices[i];
	c.t = sum * (1.0 / n);
	std::vector<double> d(n);
	for (size_t i = 0; i < n; i++) d[i] = (s.rot * tri.vertices[i] + c.t - s.pts[start + i]).length();
	c.stats = Summarise(d);
	return c;
}

Candidate FitWindow(const TriFile& tri, const Slot& s) {
	const size_t n = tri.vertices.size();
	Candidate best;
	if (n == 0 || n >= s.count) return best;
	// Coarse pass on every 4th vertex, then a full fit at the best start.
	double bestMedian = kFar;
	uint32_t bestStart = 0;
	for (uint32_t k = 0; k + n <= s.count; k++) {
		Vec3 sum;
		size_t m = 0;
		for (size_t i = 0; i < n; i += 4, m++) sum += s.pts[k + i] - s.rot * tri.vertices[i];
		Vec3 t = sum * (1.0 / m);
		std::vector<double> d;
		for (size_t i = 0; i < n; i += 4) d.push_back((s.rot * tri.vertices[i] + t - s.pts[k + i]).length());
		std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
		if (d[d.size() / 2] < bestMedian) {
			bestMedian = d[d.size() / 2];
			bestStart = k;
		}
	}
	return FitIndexed(tri, s, bestStart, kWindow);
}

// Surface fit. All TRIs of one head share one space, so when that space's origin is known (from exact
// matches) the mask is placed there and only its distance to the surface is measured. Letting a mask
// find its own offset would let it slide along a curved surface until even a wrong head's mask fits.
// Without a known origin the offset is refined from `guess` by iterating closest points.
Candidate FitSurface(const TriFile& tri, const Slot& s, const Vec3& guess, bool solveOffset) {
	Candidate c;
	c.mode = kSurface;
	c.slot = s.index;
	c.t = guess;
	const double radius = 3.0;
	std::vector<double> d(tri.vertices.size());
	for (int iter = 0; solveOffset && iter < 8; iter++) {
		Vec3 shift;
		size_t hits = 0;
		for (size_t i = 0; i < tri.vertices.size(); i++) {
			Vec3 p = s.rot * tri.vertices[i] + c.t;
			uint32_t t;
			double bc[3];
			d[i] = s.grid->Closest(p, radius, &t, bc);
			if (d[i] >= kFar) continue;
			const auto& f = s.tris[t];
			Vec3 q = s.pts[f[0]] * bc[0] + s.pts[f[1]] * bc[1] + s.pts[f[2]] * bc[2];
			shift += q - p;
			hits++;
		}
		if (hits < tri.vertices.size() / 2) {
			c.stats = Stats{};
			return c;
		}
		Vec3 step = shift * (1.0 / hits);
		c.t += step;
		if (step.length() < 1e-4) break;
	}
	for (size_t i = 0; i < tri.vertices.size(); i++) d[i] = std::min(s.grid->Closest(s.rot * tri.vertices[i] + c.t, radius, nullptr, nullptr), 9.0);
	c.stats = Summarise(d);
	return c;
}

std::string Fmt(const Stats& s) {
	char b[96];
	snprintf(b, sizeof b, "median %.3f, p90 %.3f, max %.3f", s.median, s.p90, s.max);
	return b;
}

} // namespace

std::vector<std::string> ExpandMorphPaths(const std::vector<std::string>& paths) {
	std::vector<std::string> out;
	for (const auto& p : paths) {
		fs::path fp = fs::u8path(p);
		if (fs::is_directory(fp)) {
			for (auto& e : fs::recursive_directory_iterator(fp))
				if (e.is_regular_file() && Lower(e.path().extension().u8string()) == ".tri") out.push_back(e.path().u8string());
		}
		else if (fs::is_regular_file(fp))
			out.push_back(fp.u8string());
		else
			throw Error("Morph file or folder not found: " + p);
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

void ApplyMorphs(MergedModel& m, const std::vector<std::string>& paths, const LogFn& log) {
	const std::vector<std::string> files = ExpandMorphPaths(paths);
	if (files.empty()) {
		log(LogLevel::Warning, "No .tri files found in the morph paths");
		return;
	}

	// Slots as standalone rest-pose surfaces.
	std::vector<Slot> slots(m.materials.size());
	for (size_t i = 0; i < slots.size(); i++) {
		Slot& s = slots[i];
		s.index = static_cast<int>(i);
		s.first = m.materialVertices[i].first;
		s.count = m.materialVertices[i].second;
		s.rot = m.materials[i].skinToRest;
		s.pts.assign(m.positions.begin() + s.first, m.positions.begin() + s.first + s.count);
	}
	for (size_t t = 0; t < m.tris.size(); t++) {
		Slot& s = slots[m.triMaterial[t]];
		s.tris.push_back({m.tris[t][0] - s.first, m.tris[t][1] - s.first, m.tris[t][2] - s.first});
	}
	for (auto& s : slots) s.grid = std::make_unique<TriangleGrid>(s.pts, s.tris, 1.0);

	std::vector<TriFile> tris;
	for (const auto& f : files) {
		try {
			tris.push_back(ReadTri(f));
		}
		catch (const std::exception& e) {
			log(LogLevel::Warning, std::string("Morphs: ") + e.what() + "; skipped");
			MorphSource src;
			src.file = f;
			src.outcome = std::string("unreadable: ") + e.what();
			m.morphSources.push_back(src);
		}
	}

	// Pass 1: index and window fits. The translations of those within the distance limits give the
	// TRI-space origin used to place surface masks; without any, start from the head bone. A fit accepted
	// only for its identical triangles does not count: its mean offset includes the head's sculpt
	// (0.25-0.3 from the shared origin on the user's High Poly Head).
	std::vector<Candidate> best(tris.size());
	std::vector<Vec3> anchors;
	for (size_t i = 0; i < tris.size(); i++) {
		for (const Slot& s : slots) {
			Candidate c;
			if (tris[i].vertices.size() == s.count) c = FitIndexed(tris[i], s, 0, kIndex);
			else if (tris[i].vertices.size() < s.count) c = FitWindow(tris[i], s);
			if (Better(c, best[i])) best[i] = c;
		}
		if (WithinLimits(best[i])) anchors.push_back(best[i].t);
	}
	Vec3 origin;
	const bool solveOffsets = anchors.empty();
	if (!anchors.empty()) {
		for (int k = 0; k < 3; k++) {
			std::vector<double> v;
			for (auto& a : anchors) v.push_back(k == 0 ? a.x : k == 1 ? a.y : a.z);
			std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
			(k == 0 ? origin.x : k == 1 ? origin.y : origin.z) = v[v.size() / 2];
		}
	}
	else {
		for (auto& b : m.bones)
			if (b.name == "NPC Head [Head]") origin = b.world.t;
		log(LogLevel::Warning, "Morphs: no .tri matches a head part's shape closely, so the head's TRI space is unknown; "
							   "surface masks are fitted independently and masks made for a different head may pass");
	}

	// Pass 2: surface fits for files without an accepted index or window match.
	for (size_t i = 0; i < tris.size(); i++) {
		if (Accepted(best[i])) continue;
		for (const Slot& s : slots) {
			if (s.tris.empty()) continue;
			Candidate c = FitSurface(tris[i], s, origin, solveOffsets);
			if (Better(c, best[i])) best[i] = c;
		}
	}

	// Same-named files are alternatives for the same part: keep the strongest, then densest.
	std::vector<std::string> outcome(tris.size());
	std::map<std::string, size_t> byName;
	for (size_t i = 0; i < tris.size(); i++) {
		if (!Accepted(best[i])) {
			outcome[i] = best[i].mode == kNone ? "rejected: no slot it fits"
											   : "rejected: best fit " + std::string(ModeName(best[i].mode)) + " on '" +
													 m.materials[best[i].slot].name + "' (" + Fmt(best[i].stats) + ") is outside the limits";
			continue;
		}
		std::string key = Lower(fs::u8path(tris[i].path).filename().u8string());
		auto it = byName.find(key);
		if (it == byName.end()) {
			byName[key] = i;
			continue;
		}
		size_t j = it->second;
		bool iWins = best[i].mode != best[j].mode ? best[i].mode > best[j].mode
												   : tris[i].vertices.size() > tris[j].vertices.size();
		size_t loser = iWins ? j : i, winner = iWins ? i : j;
		outcome[loser] = "superseded by " + tris[winner].path;
		it->second = winner;
	}

	// Character-creation morphs (`*chargen.tri`, the game's naming for a head part's chargen TRI) shape a
	// face rather than animate it, and a FaceGen head has them baked in already.
	for (size_t i = 0; i < tris.size(); i++)
		if (outcome[i].empty() && Lower(fs::u8path(tris[i].path).filename().u8string()).find("chargen") != std::string::npos)
			outcome[i] = "skipped: character-creation (chargen) morphs, already baked into a FaceGen head";

	// Sources are chosen per morph name. Files giving a part different names (an animation mod's EXPR_
	// morphs beside the game's own) all apply; where two sources give the same name to the same vertices,
	// the better one keeps it. A file left with no names is not applied.
	std::vector<std::set<std::string>> provides(tris.size());
	struct Drop {
		std::string reason;
		size_t count;
	};
	std::vector<std::vector<Drop>> drops(tris.size());
	for (size_t i = 0; i < tris.size(); i++)
		if (outcome[i].empty())
			for (const auto& morph : tris[i].morphs)
				if (std::any_of(morph.deltas.begin(), morph.deltas.end(), [](const Vec3& d) { return d.length() >= 1e-7; }))
					provides[i].insert(morph.name);
	auto drop = [&](size_t i, const std::set<std::string>& names, const std::string& reason) {
		size_t n = 0;
		for (auto it = provides[i].begin(); it != provides[i].end();)
			if (names.count(*it)) it = provides[i].erase(it), n++;
			else ++it;
		if (n) drops[i].push_back({reason, n});
	};
	auto shared = [&](size_t i, size_t j) {
		std::set<std::string> both;
		std::set_intersection(provides[i].begin(), provides[i].end(), provides[j].begin(), provides[j].end(),
							  std::inserter(both, both.end()));
		return both;
	};

	// Per slot, exact sources first: on overlapping vertices a name stays with the source whose triangles
	// are the slot's own, then the stronger mode, then the closer fit. A mask then gives no name an exact
	// source of its slot already gives.
	std::vector<std::vector<size_t>> exact(slots.size()), surface(slots.size());
	for (size_t i = 0; i < tris.size(); i++)
		if (outcome[i].empty()) (best[i].mode == kSurface ? surface : exact)[best[i].slot].push_back(i);
	for (size_t s = 0; s < slots.size(); s++) {
		auto& ex = exact[s];
		std::sort(ex.begin(), ex.end(), [&](size_t a, size_t b) {
			if (best[a].sameTriangles != best[b].sameTriangles) return best[a].sameTriangles;
			if (best[a].mode != best[b].mode) return best[a].mode > best[b].mode;
			return best[a].stats.median < best[b].stats.median;
		});
		for (size_t a = 0; a < ex.size(); a++)
			for (size_t b = 0; b < a; b++) {
				const size_t i = ex[a], j = ex[b];
				uint32_t a0 = best[i].start, a1 = a0 + uint32_t(tris[i].vertices.size());
				uint32_t b0 = best[j].start, b1 = b0 + uint32_t(tris[j].vertices.size());
				if (a0 < b1 && b0 < a1) drop(i, provides[j], "superseded by " + tris[j].path + " (same vertices)");
			}
		std::set<std::string> exactNames;
		for (size_t i : ex) exactNames.insert(provides[i].begin(), provides[i].end());
		for (size_t i : surface[s]) drop(i, exactNames, "skipped: '" + m.materials[s].name + "' already has an exact morph source");
	}

	// Two masks covering mostly the same part of a slot give a name they share from the better fit only.
	for (size_t s = 0; s < slots.size(); s++) {
		std::vector<size_t> live;
		for (size_t i : surface[s])
			if (!provides[i].empty()) live.push_back(i);
		std::sort(live.begin(), live.end(), [&](size_t a, size_t b) { return best[a].stats.median < best[b].stats.median; });
		std::vector<size_t> kept;
		for (size_t i : live) {
			std::vector<Vec3> pi;
			for (const Vec3& p : tris[i].vertices) pi.push_back(slots[s].rot * p + best[i].t);
			for (size_t k : kept) {
				const std::set<std::string> both = shared(i, k);
				if (both.empty()) continue;
				std::vector<Vec3> pk;
				for (const Vec3& p : tris[k].vertices) pk.push_back(slots[s].rot * p + best[k].t);
				TriangleGrid gk(pk, tris[k].triangles, 1.0);
				size_t near = 0;
				for (const Vec3& p : pi)
					if (gk.Closest(p, kOverlap, nullptr, nullptr) < kOverlap) near++;
				if (near * 2 > pi.size()) drop(i, both, "superseded by " + tris[k].path + " (covers the same region, better fit)");
			}
			if (!provides[i].empty()) kept.push_back(i);
		}
	}
	for (size_t i = 0; i < tris.size(); i++)
		if (outcome[i].empty() && provides[i].empty())
			outcome[i] = drops[i].empty() ? "skipped: none of its morphs move anything" : drops[i].front().reason;

	// Accumulate deltas per morph name and merged vertex.
	std::map<std::string, std::map<uint32_t, Vec3>> acc;
	auto add = [&](const std::string& name, uint32_t v, const Vec3& d) {
		if (d.length() < 1e-7) return;
		acc[name][v] += d;
	};
	for (size_t s = 0; s < slots.size(); s++) {
		for (size_t i : exact[s]) {
			if (!outcome[i].empty()) continue;
			const TriFile& t = tris[i];
			for (const auto& morph : t.morphs)
				if (provides[i].count(morph.name))
					for (size_t k = 0; k < t.vertices.size(); k++)
						add(morph.name, slots[s].first + best[i].start + uint32_t(k), slots[s].rot * morph.deltas[k]);
		}
		// Masks sharing morph names are one set (the left and right halves of a face), and a mask without a
		// name of its set does not move for it. Sets with different names (an animation mod's masks beside
		// the game's) are transferred separately: a mask says nothing about names it does not carry.
		std::vector<size_t> all;
		for (size_t i : surface[s])
			if (outcome[i].empty()) all.push_back(i);
		std::vector<int> family(all.size(), -1);
		std::vector<std::vector<size_t>> families;
		for (size_t a = 0; a < all.size(); a++) {
			if (family[a] >= 0) continue;
			family[a] = int(families.size());
			families.push_back({all[a]});
			for (size_t q = 0; q < families.back().size(); q++)
				for (size_t b = 0; b < all.size(); b++)
					if (family[b] < 0 && !shared(families.back()[q], all[b]).empty()) {
						family[b] = family[a];
						families.back().push_back(all[b]);
					}
		}
		for (const std::vector<size_t>& masks : families) {
			// Masks in the rest pose, then each slot vertex projected onto the nearest one.
			std::vector<std::vector<Vec3>> maskPts(masks.size());
			std::vector<std::unique_ptr<TriangleGrid>> maskGrids;
			for (size_t k = 0; k < masks.size(); k++) {
				const TriFile& t = tris[masks[k]];
				for (const Vec3& p : t.vertices) maskPts[k].push_back(slots[s].rot * p + best[masks[k]].t);
				maskGrids.push_back(std::make_unique<TriangleGrid>(maskPts[k], t.triangles, 1.0));
			}
			// Each mask's winding relative to the slot: the sign that makes its triangle normals agree with
			// the slot's vertex normals where the two surfaces coincide.
			std::vector<double> sign(masks.size(), 1.0);
			for (size_t k = 0; k < masks.size(); k++) {
				double vote = 0;
				for (uint32_t v = 0; v < slots[s].count; v++) {
					uint32_t tri;
					if (maskGrids[k]->Closest(slots[s].pts[v], 0.3, &tri, nullptr) < 0.3)
						vote += maskGrids[k]->Normal(tri).dot(m.normals[slots[s].first + v]);
				}
				sign[k] = vote < 0 ? -1.0 : 1.0;
			}
			// A vertex close to a mask takes the motion of the nearest mask point, preferring a surface facing
			// the same way when one is nearly as close: where two surfaces almost touch (closed lips) it then
			// follows its own side rather than the marginally closer other one.
			constexpr double kFacingSlack = 0.3;
			const uint32_t n = slots[s].count;
			std::vector<std::string> names;
			std::map<std::string, size_t> nameIndex;
			for (size_t k = 0; k < masks.size(); k++)
				for (const auto& morph : tris[masks[k]].morphs)
					if (provides[masks[k]].count(morph.name) && nameIndex.emplace(morph.name, names.size()).second)
						names.push_back(morph.name);
			std::vector<std::vector<Vec3>> field(names.size(), std::vector<Vec3>(n));
			std::vector<std::vector<Vec3>> nearest; // nearest-mask-point motion, the pull target
			std::vector<double> pull(n, 0.0);       // weight of that target per vertex
			enum class State { Outside, Sampled, Interpolated };
			std::vector<State> state(n, State::Outside);
			struct Where {
				size_t mask = 0;
				uint32_t tri = 0;
				Vec3 point;
			};
			std::vector<Where> where(n);
			size_t sampled = 0, interpolated = 0, discontinuous = 0;
			for (uint32_t v = 0; v < n; v++) {
				const Vec3& vn = m.normals[slots[s].first + v];
				struct Hit {
					double d = kFar;
					uint32_t tri = 0;
					size_t mask = 0;
					double bary[3] = {0, 0, 0};
				} any, facing;
				for (size_t k = 0; k < masks.size(); k++) {
					Hit h;
					h.mask = k;
					h.d = maskGrids[k]->Closest(slots[s].pts[v], kSpread, &h.tri, h.bary);
					if (h.d < any.d) any = h;
					Hit f;
					f.mask = k;
					f.d = maskGrids[k]->Closest(slots[s].pts[v], kOuter, &f.tri, f.bary, &vn, sign[k]);
					if (f.d < facing.d) facing = f;
				}
				if (any.d >= kSpread) continue; // far beyond the masks: does not move
				if (any.d >= kOuter) {
					// Beyond the masks' edge: smoothed towards zero, so motion eases out over the mesh.
					state[v] = State::Interpolated;
					pull[v] = kPull;
					interpolated++;
					continue;
				}
				// A match only counts when the mask surface faces the same way; otherwise the vertex lies on a
				// surface the mask lacks (the top of the lower lip faces up; the nearest mask skin is the upper
				// lip) and its nearest-point motion is not used at all.
				const bool facingMatch = facing.d < kFar && facing.d <= any.d + kFacingSlack;
				const Hit& hit = facingMatch ? facing : any;
				// The nearest-point motion is final for close vertices, and the pull target for the rest (faded
				// towards the masks' edges).
				const TriFile& t = tris[masks[hit.mask]];
				const auto& f = t.triangles[hit.tri];
				const double fade = hit.d <= kFade ? 1.0 : (kOuter - hit.d) / (kOuter - kFade);
				for (const auto& morph : t.morphs) {
					auto ni = nameIndex.find(morph.name);
					if (ni == nameIndex.end()) continue;
					Vec3 d = morph.deltas[f[0]] * hit.bary[0] + morph.deltas[f[1]] * hit.bary[1] + morph.deltas[f[2]] * hit.bary[2];
					field[ni->second][v] = slots[s].rot * d * fade;
				}
				where[v] = {hit.mask, hit.tri,
							maskPts[hit.mask][f[0]] * hit.bary[0] + maskPts[hit.mask][f[1]] * hit.bary[1] +
								maskPts[hit.mask][f[2]] * hit.bary[2]};
				if (facingMatch && hit.d <= kTrust) {
					state[v] = State::Sampled;
					sampled++;
				}
				else {
					state[v] = State::Interpolated;
					pull[v] = facingMatch ? kPull : 0.0;
					interpolated++;
				}
			}

			// Vertices not matched directly lie on surfaces the mask does not describe (the top and inside of a
			// lip), on skin the character's sliders moved away from the generic mask, or beyond the masks'
			// edge. Their nearest mask point can belong to a different surface (the upper lip for the lower
			// lip's top), so their motion is smoothed over the slot's own mesh instead, with directly matched
			// vertices fixed: each becomes (sum of neighbours + pull * target) / (neighbour count + pull). A
			// vertex then moves with the surface it is attached to, a region with few fixed neighbours keeps
			// local variation instead of taking one neighbour's motion wholesale, and the motion eases out
			// beyond the masks' edges.
			nearest = field;
			for (uint32_t v = 0; v < n; v++)
				if (pull[v] == 0.0 && state[v] == State::Interpolated)
					for (auto& f : field) f[v] = Vec3(); // no trusted guess: start from rest, let neighbours decide
			std::vector<std::vector<uint32_t>> nbr(n);
			for (const auto& f : slots[s].tris)
				for (int a = 0; a < 3; a++)
					for (int b = 0; b < 3; b++)
						if (a != b) nbr[f[a]].push_back(f[b]);
			{
				// Copies of a vertex split at a UV seam are one surface point.
				std::map<std::tuple<long long, long long, long long>, std::vector<uint32_t>> same;
				for (uint32_t v = 0; v < n; v++) {
					const Vec3& q = slots[s].pts[v];
					same[{std::llround(q.x * 1e4), std::llround(q.y * 1e4), std::llround(q.z * 1e4)}].push_back(v);
				}
				for (auto& [key, vs] : same)
					for (uint32_t a : vs)
						for (uint32_t b : vs)
							if (a != b) nbr[a].push_back(b);
			}
			for (auto& list : nbr) {
				std::sort(list.begin(), list.end());
				list.erase(std::unique(list.begin(), list.end()), list.end());
			}
			// Correspondence continuity. Mesh neighbours should land on nearby parts of the mask; a sampled
			// vertex whose mask point is far, across the mask's surface, from where most of its sampled
			// neighbours land took a different surface's motion and is smoothed instead. Distance across a
			// mask is measured in triangle rings; between the two halves (left/right masks) it falls back
			// to 3D distance against the neighbours' own spacing.
			{
				std::vector<std::vector<std::vector<uint32_t>>> triNbr(masks.size());
				for (size_t k = 0; k < masks.size(); k++) {
					const auto& T = tris[masks[k]].triangles;
					std::vector<std::vector<uint32_t>> byVert(tris[masks[k]].vertices.size());
					for (uint32_t t = 0; t < T.size(); t++)
						for (uint32_t c : T[t]) byVert[c].push_back(t);
					triNbr[k].resize(T.size());
					for (uint32_t t = 0; t < T.size(); t++) {
						for (uint32_t c : T[t])
							for (uint32_t o : byVert[c])
								if (o != t) triNbr[k][t].push_back(o);
						std::sort(triNbr[k][t].begin(), triNbr[k][t].end());
						triNbr[k][t].erase(std::unique(triNbr[k][t].begin(), triNbr[k][t].end()), triNbr[k][t].end());
					}
				}
				auto withinRings = [&](size_t k, uint32_t from, uint32_t to) {
					if (from == to) return true;
					std::vector<uint32_t> ring{from}, next;
					std::unordered_map<uint32_t, bool> seen{{from, true}};
					for (int r = 0; r < kContinuityRings; r++) {
						next.clear();
						for (uint32_t t : ring)
							for (uint32_t o : triNbr[k][t])
								if (seen.emplace(o, true).second) {
									if (o == to) return true;
									next.push_back(o);
								}
						ring.swap(next);
					}
					return false;
				};
				std::vector<uint32_t> demote;
				for (uint32_t v = 0; v < n; v++) {
					if (state[v] != State::Sampled) continue;
					int agree = 0, disagree = 0;
					for (uint32_t u : nbr[v]) {
						if (state[u] != State::Sampled) continue;
						bool ok = where[u].mask == where[v].mask
									  ? withinRings(where[v].mask, where[v].tri, where[u].tri)
									  : (where[u].point - where[v].point).length() <=
											2.0 * (slots[s].pts[u] - slots[s].pts[v]).length() + 0.1;
						(ok ? agree : disagree)++;
					}
					if (disagree > agree) demote.push_back(v);
				}
				for (uint32_t v : demote) {
					state[v] = State::Interpolated;
					sampled--;
					interpolated++;
					discontinuous++;
				}
			}

			std::vector<uint32_t> unknown;
			for (uint32_t v = 0; v < n; v++)
				if (state[v] == State::Interpolated && !nbr[v].empty()) unknown.push_back(v);
			int sweeps = 0;
			for (; sweeps < 5000; sweeps++) { // Gauss-Seidel
				double change = 0;
				for (uint32_t v : unknown) {
					const double inv = 1.0 / (nbr[v].size() + pull[v]);
					for (size_t k = 0; k < field.size(); k++) {
						auto& f = field[k];
						Vec3 sum = nearest[k][v] * pull[v];
						for (uint32_t u : nbr[v]) sum += f[u];
						Vec3 next = sum * inv;
						change = std::max(change, (next - f[v]).length());
						f[v] = next;
					}
				}
				if (change < 1e-7) break;
			}
			std::string files;
			for (size_t i : masks) files += (files.empty() ? "" : " + ") + fs::u8path(tris[i].path).filename().u8string();
			log(LogLevel::Info, "Morphs: '" + m.materials[s].name + "' (" + files + "): " + std::to_string(sampled) +
									" vertices take mask motion directly, " + std::to_string(interpolated) +
									" are smoothed across the mesh (" + std::to_string(discontinuous) +
									" of them landed on a different part of the mask than their neighbours; " +
									std::to_string(sweeps) + " sweeps)");
			if (sampled * 3 < sampled + interpolated)
				log(LogLevel::Warning, "Morphs: '" + m.materials[s].name + "': under a third of the vertices near its masks fit them "
									   "closely; most of its facial motion is smoothed, so these masks may not suit this head");

			for (size_t k = 0; k < names.size(); k++)
				for (uint32_t v = 0; v < n; v++)
					if (state[v] != State::Outside) add(names[k], slots[s].first + v, field[k][v]);
		}
	}

	for (auto& [name, verts] : acc) {
		BlendShape b;
		b.name = name;
		for (auto& [v, d] : verts) {
			b.vertices.push_back(v);
			b.deltas.push_back(d);
		}
		if (!b.vertices.empty()) m.blendShapes.push_back(std::move(b));
	}

	for (size_t i = 0; i < tris.size(); i++) {
		MorphSource src;
		src.file = tris[i].path;
		src.mode = ModeName(best[i].mode);
		src.windowStart = best[i].start;
		src.median = best[i].stats.median;
		src.p90 = best[i].stats.p90;
		src.max = best[i].stats.max;
		src.sameTriangles = best[i].sameTriangles;
		if (outcome[i].empty()) src.morphs.assign(provides[i].begin(), provides[i].end());
		if (best[i].slot >= 0) src.shape = m.materials[best[i].slot].name;
		src.outcome = outcome[i].empty() ? "applied" : outcome[i];
		const std::string file = fs::u8path(src.file).filename().u8string();
		if (outcome[i].empty()) {
			std::string where = src.mode == std::string("window")
									? " vertices " + std::to_string(src.windowStart) + "-" +
										  std::to_string(src.windowStart + tris[i].vertices.size() - 1)
									: "";
			char off[64];
			snprintf(off, sizeof off, ", offset (%.2f, %.2f, %.2f)", best[i].t.x, best[i].t.y, best[i].t.z);
			std::string elsewhere;
			for (const Drop& d : drops[i]) elsewhere += "; " + std::to_string(d.count) + " left out: " + d.reason;
			log(LogLevel::Info, "Morphs: " + file + " -> '" + src.shape + "'" + where + " by " + src.mode + " (" +
									(src.sameTriangles ? "identical triangles, " : "") + Fmt(best[i].stats) + off + "), " +
									std::to_string(src.morphs.size()) + " morphs" + elsewhere);
		}
		else {
			char off[64];
			snprintf(off, sizeof off, " [offset (%.2f, %.2f, %.2f)]", best[i].t.x, best[i].t.y, best[i].t.z);
			log(LogLevel::Info, "Morphs: " + file + " " + src.outcome + (best[i].mode != kNone ? off : ""));
		}
		m.morphSources.push_back(src);
	}
	log(LogLevel::Info, "Morphs: " + std::to_string(m.blendShapes.size()) + " blend shapes");
}

} // namespace nm
