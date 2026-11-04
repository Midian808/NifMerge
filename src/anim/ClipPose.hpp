#pragma once
// Binds a clip's tracks to the Havok skeleton and poses the NIF skeleton with it, the way the game
// does: each Havok bone's local transform is copied onto the NIF node of the same name and composed
// through the NIF hierarchy (XPMSE's identity "CME" nodes exist so that this still works).
#include "HkxAnimation.hpp"

namespace nm {

struct ClipBinding {
	enum class Mode { Explicit, ByName, ByIndex };
	Mode mode = Mode::ByIndex;
	std::vector<int> trackToBone;        // Havok skeleton bone per track; -1 when the track is dropped
	std::vector<std::string> dropped;    // tracks with no bone in the skeleton (by name, or "track N")
	int conflicts = 0;                   // named tracks whose explicit index points at a differently named bone
	const char* modeName() const;
};

// Named tracks bind by name (case-insensitive): a name holds on any skeleton variant, while an explicit
// index only holds on the skeleton the clip was authored for. Nameless tracks use the clip's explicit
// index map when it has one, else track i -> bone i.
ClipBinding BindClip(const HkxClip& clip, const HkxSkeleton& skel);

// The body skeleton in a skeleton.hkx: the one the clip names, else the first that isn't a ragdoll.
const HkxSkeleton& BodySkeleton(const std::vector<HkxSkeleton>& skeletons, const std::string& wanted, const std::string& path);

struct PosedClip {
	std::vector<OutBone> bones;              // output skeleton: rest = NIF bind pose, rigid (scale baked out)
	std::vector<bool> driven;                // per output bone: posed from the Havok skeleton
	std::vector<std::vector<Xform>> frames;  // [frame][bone] rigid world transforms, game units, Skyrim axes
	double frameDuration = 0;
	bool loops = false;                      // on every driven bone, last -> first is no bigger than a frame step (or 0.5 degrees)
	double loopError = 0;                    // largest first/last rotation difference over the driven bones, degrees
	bool rootMotion = false;                 // the clip's reference frame moves, and the frames include it
	std::vector<Xform> motion;               // when rootMotion: the actor's motion per frame (game units, Skyrim axes)
	int drivenBones = 0;                     // Havok bones the clip has a track for
	std::vector<std::string> missingNodes;   // driven Havok bones the NIF has no node for (not exported)
	int skeletonBones = 0;                   // bones in the Havok skeleton
};

// Output bones are the NIF nodes driven by a Havok bone, every Humanoid bone, and all their ancestors
// below the NIF file root (NifMerge's rule, so bone paths match its characters). Animated scale is
// ignored; each NIF node keeps its own scale, which is then baked out like NifMerge's rest pose.
// A moving reference frame (root motion) is composed over the whole pose: the game moves the whole actor
// by it, and the bones are relative to it.
PosedClip PoseClip(const HkxClip& clip, const ClipBinding& binding, const HkxSkeleton& hkx, const Skeleton& nif);

} // namespace nm
