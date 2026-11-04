#include "ClipPose.hpp"

#include "core/Humanoid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace nm {

namespace {

const double kLoopToleranceDeg = 0.5; // a wrap this small always counts as a loop
const double kLoopStepFactor = 2.0;    // a wrap up to this many times the bone's largest frame step counts too

} // namespace

const char* ClipBinding::modeName() const {
	switch (mode) {
		case Mode::Explicit: return "explicit";
		case Mode::ByName: return "by name";
		default: return "by index";
	}
}

ClipBinding BindClip(const HkxClip& clip, const HkxSkeleton& skel) {
	ClipBinding b;
	const int bones = static_cast<int>(skel.bones.size());
	const bool hasMap = !clip.trackToBone.empty();
	if (hasMap && static_cast<int>(clip.trackToBone.size()) != clip.numTracks)
		throw Error(clip.path + ": its binding lists " + std::to_string(clip.trackToBone.size()) + " bones for " +
					std::to_string(clip.numTracks) + " tracks");
	auto mapped = [&](int t) {
		int i = clip.trackToBone[t];
		if (i < 0 || i >= bones)
			throw Error(clip.path + ": track " + std::to_string(t) + " is bound to bone " + std::to_string(i) +
						", the skeleton has " + std::to_string(bones));
		return i;
	};
	auto named = [&](int t) { return clip.trackNames[t] && !clip.trackNames[t]->empty(); };
	bool anyNamed = false;
	for (int t = 0; t < clip.numTracks; t++) anyNamed = anyNamed || named(t);

	if (anyNamed) {
		b.mode = ClipBinding::Mode::ByName;
		for (int t = 0; t < clip.numTracks; t++) {
			int i = -1;
			if (named(t)) {
				i = skel.find(*clip.trackNames[t]);
				if (hasMap && clip.trackToBone[t] != i) b.conflicts++;
			} else if (hasMap) {
				i = mapped(t);
			}
			b.trackToBone.push_back(i);
			if (i < 0) b.dropped.push_back(named(t) ? *clip.trackNames[t] : "track " + std::to_string(t));
		}
		return b;
	}
	if (hasMap) {
		b.mode = ClipBinding::Mode::Explicit;
		for (int t = 0; t < clip.numTracks; t++) b.trackToBone.push_back(mapped(t));
		return b;
	}
	b.mode = ClipBinding::Mode::ByIndex;
	if (clip.numTracks > bones)
		throw Error(clip.path + ": its " + std::to_string(clip.numTracks) + " unnamed tracks cannot bind to the " +
					std::to_string(bones) + "-bone skeleton '" + skel.name + "'");
	for (int t = 0; t < clip.numTracks; t++) b.trackToBone.push_back(t);
	return b;
}

const HkxSkeleton& BodySkeleton(const std::vector<HkxSkeleton>& skeletons, const std::string& wanted, const std::string& path) {
	for (auto& s : skeletons)
		if (!wanted.empty() && s.name == wanted) return s;
	for (auto& s : skeletons)
		if (s.name.rfind("Ragdoll", 0) != 0) return s;
	throw Error(path + ": holds no body skeleton");
}

PosedClip PoseClip(const HkxClip& clip, const ClipBinding& binding, const HkxSkeleton& hkx, const Skeleton& nif) {
	const size_t N = nif.bones.size();
	if (N < 2) throw Error(nif.path + ": skeleton has no bones");
	// An additive clip stores offsets the game layers onto whatever else is playing; it has no pose of
	// its own. Which side of the base pose Havok composes them on is not recorded in the file, and a
	// guess would export a wrong-but-plausible pose, so these clips are refused.
	if (clip.blendHint != 0)
		throw Error(clip.path + ": is an additive clip (blend hint " + std::to_string(clip.blendHint) +
					"), which only plays layered on another animation; it is not exported");

	// NIF node -> Havok bone of the same name (exact, then case-insensitive), and each node's local
	// rest transform and uniform scale.
	std::vector<int> nifToHkx(N, -1);
	std::vector<Xform> restLocal(N);
	std::vector<double> scale(N, 1.0);
	for (size_t i = 0; i < N; i++) {
		const auto& b = nif.bones[i];
		restLocal[i] = b.parent < 0 ? b.world : nif.bones[b.parent].world.inverse() * b.world;
		scale[i] = restLocal[i].L.col(0).length();
		if (i > 0) nifToHkx[i] = hkx.find(b.name);
	}

	// Havok bones the clip drives that the NIF has no node for cannot be shown. When that is every driven
	// bone below the root, the .nif is another skeleton (creature skeletons share only "NPC Root [Root]").
	PosedClip out;
	{
		std::set<int> hasNode(nifToHkx.begin(), nifToHkx.end());
		bool anyBelowRoot = false;
		std::set<int> seen;
		for (int b : binding.trackToBone) {
			if (b < 0 || !seen.insert(b).second) continue;
			if (!hasNode.count(b)) out.missingNodes.push_back(hkx.bones[b]);
			else if (hkx.parents[b] >= 0) anyBelowRoot = true;
		}
		if (!anyBelowRoot)
			throw Error(nif.path + ": none of the bones the clip animates below the root has a node in it (the .nif is not the skeleton of '" +
						hkx.name + "' in the .hkx)");
	}
	out.frameDuration = clip.frameDuration;
	out.skeletonBones = static_cast<int>(hkx.bones.size());

	// Output bone set (NifMerge's keepChain rule; node 0 is the file's container, not a bone).
	std::vector<bool> keep(N, false);
	auto keepChain = [&](int i) {
		for (; i > 0 && !keep[i]; i = nif.bones[i].parent) keep[i] = true;
	};
	for (size_t i = 1; i < N; i++)
		if (nifToHkx[i] >= 0) keepChain(static_cast<int>(i));
	for (auto& h : HumanoidMap())
		if (int i = FindHumanoidBone(nif, h); i >= 0) keepChain(i);
	std::vector<int> outIndex(N, -1);
	for (size_t i = 1; i < N; i++) {
		if (!keep[i]) continue;
		OutBone b;
		b.name = nif.bones[i].name;
		b.parent = outIndex[nif.bones[i].parent];
		b.world = nif.bones[i].world.rigid();
		b.skeletonIndex = static_cast<int>(i);
		for (auto& h : HumanoidMap())
			if (static_cast<int>(i) == FindHumanoidBone(nif, h)) b.humanoid = h.unity;
		outIndex[i] = static_cast<int>(out.bones.size());
		out.bones.push_back(b);
		out.driven.push_back(nifToHkx[i] >= 0);
	}

	// Root motion: the reference frame moves the whole actor, so it is composed over every bone's pose in
	// actor space (the file root's): W0 * M * W0^-1. The top-level bones carry it; locals below are unchanged.
	if (clip.referenceFrameMoves) {
		const Vec3 up = clip.referenceUp;
		if (std::fabs(up.x) > 1e-6 || std::fabs(up.y) > 1e-6 || std::fabs(up.z - 1.0) > 1e-6)
			throw Error(clip.path + ": its root motion's up axis is (" + std::to_string(up.x) + ", " + std::to_string(up.y) + ", " +
						std::to_string(up.z) + "), not +Z; only +Z is supported");
		if (clip.referenceSamples.empty()) throw Error(clip.path + ": its root motion has no samples");
		out.rootMotion = true;
		const Xform w0 = nif.bones[0].world;

		// One sample per frame in Skyrim's clips; otherwise sampled by time, linearly.
		const auto& smp = clip.referenceSamples;
		for (int f = 0; f < clip.numFrames; f++) {
			double u = smp.size() == static_cast<size_t>(clip.numFrames) || clip.numFrames < 2
						   ? f
						   : static_cast<double>(f) * (smp.size() - 1) / (clip.numFrames - 1);
			size_t a = std::min(static_cast<size_t>(u), smp.size() - 1), b = std::min(a + 1, smp.size() - 1);
			double k = u - static_cast<double>(a);
			std::array<double, 4> v;
			for (int c = 0; c < 4; c++) v[c] = smp[a][c] * (1 - k) + smp[b][c] * k;
			Xform m;
			const double cs = std::cos(v[3]), sn = std::sin(v[3]);
			m.L.m[0] = {cs, -sn, 0};
			m.L.m[1] = {sn, cs, 0};
			m.L.m[2] = {0, 0, 1};
			m.t = {v[0], v[1], v[2]};
			out.motion.push_back(w0 * m * w0.inverse());
		}
	}

	// Havok locals per frame: the reference pose, overridden by each bound track (scale ignored).
	auto havokLocals = [&](int f) {
		std::vector<HkTransform> local(hkx.referencePose);
		for (int t = 0; t < clip.numTracks; t++) {
			int b = binding.trackToBone[t];
			if (b < 0) continue;
			local[b].t = clip.frames[f][t].t;
			local[b].r = clip.frames[f][t].r;
		}
		return local;
	};

	out.frames.resize(clip.numFrames);
	std::vector<Xform> world(N);
	for (int f = 0; f < clip.numFrames; f++) {
		auto local = havokLocals(f);
		world[0] = nif.bones[0].world;
		for (size_t i = 1; i < N; i++) {
			Xform l = restLocal[i];
			if (int h = nifToHkx[i]; h >= 0) l = {local[h].r.toMat3() * scale[i], local[h].t};
			world[i] = nif.bones[i].parent < 0 ? l : world[nif.bones[i].parent] * l;
		}
		auto& row = out.frames[f];
		row.reserve(out.bones.size());
		for (auto& b : out.bones) row.push_back(out.rootMotion ? (out.motion[f] * world[b.skeletonIndex]).rigid() : world[b.skeletonIndex].rigid());
	}

	// Loop detection on every bone the clip drives (the root motion is separate and not part of it): the
	// clip loops when wrapping from its last frame to its first is no bigger a jump, on any bone, than the
	// bone's own motion between frames. Cycles are authored either with the last frame equal to the first
	// or one ordinary step before it; one-shot clips jump many steps.
	std::set<int> bound;
	for (int b : binding.trackToBone)
		if (b >= 0) bound.insert(b);
	out.drivenBones = static_cast<int>(bound.size());
	std::vector<std::vector<HkTransform>> locals;
	for (int f = 0; f < clip.numFrames; f++) locals.push_back(havokLocals(f));
	const double deg = 180.0 / 3.14159265358979323846;
	out.loops = clip.numFrames > 2;
	for (int b : bound) {
		double step = 0;
		for (int f = 0; f + 1 < clip.numFrames; f++) step = std::max(step, locals[f][b].r.angleTo(locals[f + 1][b].r) * deg);
		const double gap = locals.front()[b].r.angleTo(locals.back()[b].r) * deg;
		out.loopError = std::max(out.loopError, gap);
		if (gap > std::max(kLoopToleranceDeg, kLoopStepFactor * step)) out.loops = false;
	}
	return out;
}

} // namespace nm
