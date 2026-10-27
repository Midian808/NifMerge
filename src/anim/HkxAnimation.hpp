#pragma once
// Skyrim animation data read from .hkx packfiles: the Havok skeleton (skeleton.hkx) and animation
// clips, decoded to per-frame, per-track transforms.
#include "HkxPackfile.hpp"

#include <set>

namespace nm {

// Havok's hkQsTransform: translation, rotation, scale (Skyrim axes, game units).
struct HkTransform {
	Vec3 t;
	Quat r;
	Vec3 s{1, 1, 1};
};

struct HkxSkeleton {
	std::string name;
	std::vector<std::string> bones;
	std::vector<int> parents; // -1 for roots; parents precede children
	std::vector<HkTransform> referencePose;
	int find(const std::string& bone) const; // case-insensitive; -1 if absent
};

// The skeletons in a skeleton.hkx (Skyrim's holds the body and the ragdoll).
std::vector<HkxSkeleton> ReadSkeletons(const HkxPackfile& pf);

struct HkxEvent {
	double time = 0; // seconds
	std::string text;
	int track = 0;
};

struct HkxClip {
	std::string path;
	int pointerSize = 4;      // 4: Skyrim LE, 8: Skyrim SE
	std::string animClass;    // hkaSplineCompressedAnimation or hkaInterleavedUncompressedAnimation
	double duration = 0;      // seconds
	double frameDuration = 0; // seconds per frame
	int numFrames = 0;
	int numTracks = 0;
	int numFloatTracks = 0;
	std::vector<std::optional<std::string>> trackNames; // annotation track names; null in SE clips
	std::vector<HkxEvent> events;                       // ordered by time
	std::string skeletonName;                           // binding's originalSkeletonName
	std::vector<int16_t> trackToBone;                   // explicit binding; empty means track i -> bone i
	int blendHint = 0;                                  // 0 normal, 1 additive
	bool hasReferenceFrame = false;                     // extracted motion present
	bool referenceFrameMoves = false;                   // and not all zero
	std::set<std::string> formats;                      // encodings met, e.g. "rotation THREECOMP48"
	std::set<std::string> unverifiedFormats;            // the subset no real sample has confirmed
	std::vector<std::vector<HkTransform>> frames;       // [frame][track]
};

HkxClip ReadClip(const HkxPackfile& pf);

// What a .hkx holds, for sorting a batch of dropped files.
enum class HkxKind { Skeleton, Animation, Other };
HkxKind Classify(const HkxPackfile& pf);

} // namespace nm
