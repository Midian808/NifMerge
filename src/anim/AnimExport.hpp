#pragma once
// Writes a posed clip as a skeleton-only FBX with one animation take, plus the <clip>.anim.json
// sidecar the Unity package reads (events, loop flag, Humanoid map, provenance).
#include "ClipPose.hpp"

namespace nm {

struct AnimExportInfo {
	std::string clipName;     // take name and Unity clip name
	std::string sourceClip;   // the .hkx it came from
	std::string skeletonNif;
	std::string skeletonHkx;
	const HkxClip* clip = nullptr;
	const ClipBinding* binding = nullptr;
};

void WriteAnimFbx(const PosedClip& posed, const AnimExportInfo& info, const std::string& fbxPath);

// <fbx without extension>.anim.json
std::string AnimSidecarPathFor(const std::string& fbxPath);
void WriteAnimJson(const PosedClip& posed, const AnimExportInfo& info, const std::string& fbxPath, const LogFn& log);

} // namespace nm
