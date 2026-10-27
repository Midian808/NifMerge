#pragma once
#include "Model.hpp"

namespace nm {

Skeleton LoadSkeleton(const std::string& path);

// Loads every shape in a nif and re-poses it onto the skeleton's rest pose:
//   v_rest = sum_i w_i * (SkeletonBoneWorld_i * SkinToBone_i)(v)
// This is the transform the game itself applies, so the nif's own node transforms (stripped of
// rotation in FaceGen files) and per-shape global-to-skin transforms are never used for skinned shapes.
std::vector<PosedShape> LoadPosedShapes(const std::string& path, const Skeleton& skel, const LogFn& log);

// The body slot (dismember partition ID) of every triangle of every shape with geometry, without
// loading the rest. Shapes in file order, as LoadPosedShapes returns them.
struct ShapeSlots {
	std::string shape;
	std::vector<uint16_t> triSlots; // 0 when the shape has no partitions
};
std::vector<ShapeSlots> ReadShapeSlots(const std::string& path);

// Bone-name lookup with the known FaceGen alias ("Head" -> "NPC Head [Head]"). -1 if absent.
int ResolveBone(const Skeleton& skel, const std::string& name);

// Human-readable dump of a nif: node tree, shapes, and (with a skeleton) the per-bone
// skin->world transform each skinned shape implies, so binding disagreements are visible.
std::string InspectNif(const std::string& path, const Skeleton* skel);

} // namespace nm
