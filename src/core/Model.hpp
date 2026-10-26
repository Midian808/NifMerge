#pragma once
// Data model shared by the loader, merger and writers. All geometry is in Skyrim space
// (Z-up, game units) until the FBX writer converts it.
#include "Math.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nm {

enum class LogLevel { Info, Warning, Error };
using LogFn = std::function<void(LogLevel, const std::string&)>;

struct Error : std::runtime_error {
	using std::runtime_error::runtime_error;
};

struct Skeleton {
	struct Bone {
		std::string name;
		int parent = -1;
		Xform world; // rest pose in game units, as authored (may carry scale)
	};
	std::string path;
	std::vector<Bone> bones; // parents always precede children
	std::unordered_map<std::string, int> index;

	int find(const std::string& name) const {
		auto it = index.find(name);
		return it == index.end() ? -1 : it->second;
	}
};

struct MaterialSlot {
	std::string name;       // unique material name in the output
	std::string sourceFile; // nif the shape came from (the _1 file for weight pairs)
	std::string shapeName;
	std::string shaderBlock; // e.g. BSLightingShaderProperty, BSEffectShaderProperty, or empty
	uint32_t shaderType = 0;
	uint32_t shaderFlags1 = 0;
	uint32_t shaderFlags2 = 0;
	std::vector<std::string> textures; // texture set slots in nif order
	bool hasAlphaProperty = false;
	uint16_t alphaFlags = 0;
	uint8_t alphaThreshold = 0;
	float uvOffset[2] = {0, 0};
	float uvScale[2] = {1, 1};
	float glossiness = 0;
	float specularColor[3] = {1, 1, 1};
	float specularStrength = 0;
	float emissiveColor[3] = {0, 0, 0};
	float emissiveMultiple = 0;
	float alpha = 1;
	float skinTintColor[3] = {1, 1, 1};
	float hairTintColor[3] = {1, 1, 1};
	bool doubleSided = false;
	bool modelSpaceNormals = false;
	Mat3 skinToRest; // skin space (where a model-space normal map lives) -> rest pose, Skyrim axes
	bool vertexColors = false;
	bool vertexAlpha = false;
};

using VertexWeights = std::vector<std::pair<int, float>>; // (bone index, weight), sorted by weight desc

// One shape, already re-posed onto the skeleton's rest pose. Bone indices refer to the Skeleton.
struct PosedShape {
	MaterialSlot material;
	std::vector<Vec3> positions;
	std::vector<Vec3> normals; // empty when the nif stores none; computed after merging
	std::vector<std::array<float, 2>> uvs;
	std::vector<std::array<float, 4>> colors; // empty when the shape has none
	std::vector<std::array<uint32_t, 3>> tris;
	std::vector<VertexWeights> weights;
};

struct PartInput {
	std::string path0;                // the only file, or the _0 file of a weight pair
	std::optional<std::string> path1; // the _1 file of a weight pair
};

struct MergeRequest {
	std::string skeletonPath;
	std::vector<PartInput> parts;
	double weight = 50.0; // Skyrim NPC weight, 0..100
};

struct OutBone {
	std::string name;
	int parent = -1;    // index into MergedModel::bones
	Xform world;        // rigid (scale-free) rest transform, game units
	int skeletonIndex = -1;
	std::string humanoid; // Unity HumanBodyBones name, or empty
};

struct MergedModel {
	std::vector<OutBone> bones; // parents precede children
	std::vector<MaterialSlot> materials;
	std::vector<Vec3> positions;
	std::vector<Vec3> normals;
	std::vector<std::array<float, 2>> uvs;
	std::vector<std::array<float, 4>> colors; // empty when no input shape had colors
	std::vector<std::array<uint32_t, 3>> tris;
	std::vector<int> triMaterial;
	std::vector<VertexWeights> weights; // bone indices into bones
};

} // namespace nm
