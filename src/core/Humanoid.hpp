#pragma once
// Skyrim skeleton bone -> Unity Humanoid bone mapping. Unity names are HumanTrait.BoneName strings.
// Skyrim names are byte-exact, including the significant trailing spaces inside brackets.
#include <string>
#include <vector>

namespace nm {

struct HumanoidBone {
	const char* unity;
	const char* skyrim;
	bool required;
};

inline const std::vector<HumanoidBone>& HumanoidMap() {
	static const std::vector<HumanoidBone> map = {
		// NPC Spine is a sibling of NPC Pelvis under NPC COM, so COM is the only valid Hips.
		{"Hips", "NPC COM [COM ]", true},
		{"Spine", "NPC Spine [Spn0]", true},
		{"Chest", "NPC Spine1 [Spn1]", false},
		{"UpperChest", "NPC Spine2 [Spn2]", false},
		{"Neck", "NPC Neck [Neck]", false},
		{"Head", "NPC Head [Head]", true},
		{"LeftShoulder", "NPC L Clavicle [LClv]", false},
		{"RightShoulder", "NPC R Clavicle [RClv]", false},
		{"LeftUpperArm", "NPC L UpperArm [LUar]", true},
		{"RightUpperArm", "NPC R UpperArm [RUar]", true},
		{"LeftLowerArm", "NPC L Forearm [LLar]", true},
		{"RightLowerArm", "NPC R Forearm [RLar]", true},
		{"LeftHand", "NPC L Hand [LHnd]", true},
		{"RightHand", "NPC R Hand [RHnd]", true},
		{"LeftUpperLeg", "NPC L Thigh [LThg]", true},
		{"RightUpperLeg", "NPC R Thigh [RThg]", true},
		{"LeftLowerLeg", "NPC L Calf [LClf]", true},
		{"RightLowerLeg", "NPC R Calf [RClf]", true},
		{"LeftFoot", "NPC L Foot [Lft ]", true},
		{"RightFoot", "NPC R Foot [Rft ]", true},
		{"LeftToes", "NPC L Toe0 [LToe]", false},
		{"RightToes", "NPC R Toe0 [RToe]", false},
		{"Left Thumb Proximal", "NPC L Finger00 [LF00]", false},
		{"Left Thumb Intermediate", "NPC L Finger01 [LF01]", false},
		{"Left Thumb Distal", "NPC L Finger02 [LF02]", false},
		{"Left Index Proximal", "NPC L Finger10 [LF10]", false},
		{"Left Index Intermediate", "NPC L Finger11 [LF11]", false},
		{"Left Index Distal", "NPC L Finger12 [LF12]", false},
		{"Left Middle Proximal", "NPC L Finger20 [LF20]", false},
		{"Left Middle Intermediate", "NPC L Finger21 [LF21]", false},
		{"Left Middle Distal", "NPC L Finger22 [LF22]", false},
		{"Left Ring Proximal", "NPC L Finger30 [LF30]", false},
		{"Left Ring Intermediate", "NPC L Finger31 [LF31]", false},
		{"Left Ring Distal", "NPC L Finger32 [LF32]", false},
		{"Left Little Proximal", "NPC L Finger40 [LF40]", false},
		{"Left Little Intermediate", "NPC L Finger41 [LF41]", false},
		{"Left Little Distal", "NPC L Finger42 [LF42]", false},
		{"Right Thumb Proximal", "NPC R Finger00 [RF00]", false},
		{"Right Thumb Intermediate", "NPC R Finger01 [RF01]", false},
		{"Right Thumb Distal", "NPC R Finger02 [RF02]", false},
		{"Right Index Proximal", "NPC R Finger10 [RF10]", false},
		{"Right Index Intermediate", "NPC R Finger11 [RF11]", false},
		{"Right Index Distal", "NPC R Finger12 [RF12]", false},
		{"Right Middle Proximal", "NPC R Finger20 [RF20]", false},
		{"Right Middle Intermediate", "NPC R Finger21 [RF21]", false},
		{"Right Middle Distal", "NPC R Finger22 [RF22]", false},
		{"Right Ring Proximal", "NPC R Finger30 [RF30]", false},
		{"Right Ring Intermediate", "NPC R Finger31 [RF31]", false},
		{"Right Ring Distal", "NPC R Finger32 [RF32]", false},
		{"Right Little Proximal", "NPC R Finger40 [RF40]", false},
		{"Right Little Intermediate", "NPC R Finger41 [RF41]", false},
		{"Right Little Distal", "NPC R Finger42 [RF42]", false},
	};
	return map;
}

} // namespace nm
