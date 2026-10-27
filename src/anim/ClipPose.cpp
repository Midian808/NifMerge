#include "ClipPose.hpp"

#include "core/Humanoid.hpp"

#include <algorithm>
#include <cmath>

namespace nm {

namespace {

const double kLoopToleranceDeg = 0.5;

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

	PosedClip out;
	out.frameDuration = clip.frameDuration;

	// Output bone set (NifMerge's keepChain rule; node 0 is the file's container, not a bone).
	std::vector<bool> keep(N, false);
	auto keepChain = [&](int i) {
		for (; i > 0 && !keep[i]; i = nif.bones[i].parent) keep[i] = true;
	};
	for (size_t i = 1; i < N; i++)
		if (nifToHkx[i] >= 0) keepChain(static_cast<int>(i));
	for (auto& h : HumanoidMap())
		if (int i = nif.find(h.skyrim); i >= 0) keepChain(i);
	std::vector<int> outIndex(N, -1);
	for (size_t i = 1; i < N; i++) {
		if (!keep[i]) continue;
		OutBone b;
		b.name = nif.bones[i].name;
		b.parent = outIndex[nif.bones[i].parent];
		b.world = nif.bones[i].world.rigid();
		b.skeletonIndex = static_cast<int>(i);
		for (auto& h : HumanoidMap())
			if (b.name == h.skyrim) b.humanoid = h.unity;
		outIndex[i] = static_cast<int>(out.bones.size());
		out.bones.push_back(b);
		out.driven.push_back(nifToHkx[i] >= 0);
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
		for (auto& b : out.bones) row.push_back(world[b.skeletonIndex].rigid());
	}

	// Loop detection on the Humanoid bones the clip drives.
	auto first = havokLocals(0), last = havokLocals(clip.numFrames - 1);
	for (auto& h : HumanoidMap()) {
		int b = hkx.find(h.skyrim);
		if (b < 0 || std::find(binding.trackToBone.begin(), binding.trackToBone.end(), b) == binding.trackToBone.end()) continue;
		out.loopError = std::max(out.loopError, first[b].r.angleTo(last[b].r) * 180.0 / 3.14159265358979323846);
	}
	out.loops = clip.numFrames > 2 && out.loopError <= kLoopToleranceDeg;
	return out;
}

} // namespace nm
