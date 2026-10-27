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
	bool loops = false;                      // first and last frames agree on every Humanoid bone
	double loopError = 0;                    // largest first/last rotation difference there, degrees
};

// Output bones are the NIF nodes driven by a Havok bone, every Humanoid bone, and all their ancestors
// below the NIF file root (NifMerge's rule, so bone paths match its characters). Animated scale is
// ignored; each NIF node keeps its own scale, which is then baked out like NifMerge's rest pose.
PosedClip PoseClip(const HkxClip& clip, const ClipBinding& binding, const HkxSkeleton& hkx, const Skeleton& nif);

} // namespace nm
