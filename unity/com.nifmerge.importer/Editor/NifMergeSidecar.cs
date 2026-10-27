using System;
using System.IO;
using UnityEngine;

namespace NifMerge
{
    // Mirror of the <model>.materials.json sidecar written by NifMerge (formatVersion 2).
    [Serializable]
    public class Sidecar
    {
        public string generator;
        public int formatVersion;
        public string fbx;
        public float metresPerGameUnit;
        public HumanoidEntry[] humanoid;
        public string[] blendShapes; // facial morph names, as imported onto the mesh
        public PartEntry[] parts;    // each input file, character or outfit, and what the outfit removed
        public MaterialEntry[] materials;

        public const string Generator = "NifMerge";
        public const int FormatVersion = 2;

        public static string PathFor(string fbxPath)
        {
            string dir = Path.GetDirectoryName(fbxPath) ?? "";
            return Path.Combine(dir, Path.GetFileNameWithoutExtension(fbxPath) + ".materials.json").Replace('\\', '/');
        }

        // Null when the FBX is not NifMerge output.
        public static Sidecar TryLoad(string fbxPath, out string text)
        {
            text = null;
            string path = PathFor(fbxPath);
            if (!File.Exists(path)) return null;
            text = File.ReadAllText(path);
            var s = JsonUtility.FromJson<Sidecar>(text);
            if (s == null || s.generator != Generator) return null;
            if (s.formatVersion != FormatVersion)
            {
                Debug.LogError($"NifMerge: {path} has formatVersion {s.formatVersion}; this importer reads version " +
                               $"{FormatVersion}. Re-export the model with the matching NifMerge.");
                return null;
            }
            return s;
        }
    }

    [Serializable]
    public class HumanoidEntry
    {
        public string human;
        public string bone;
    }

    [Serializable]
    public class PartEntry
    {
        public string file;
        public string role; // "character" or "outfit"
        public bool head;   // a head: only its hair reacts to outfit items
        public int[] slots; // body slots (dismember partition IDs) across its shapes
        public RemovalEntry[] removed;
    }

    [Serializable]
    public class RemovalEntry
    {
        public string shape;
        public int slot;
        public int triangles;
        public bool wholeShape;
        public string by; // the outfit file that takes the slot
    }

    [Serializable]
    public class MaterialEntry
    {
        public int slot;
        public string name;
        public string sourceFile;
        public string shape;
        public string role; // "character" or "outfit"
        public int[] slots; // body slots of its triangles
        public string shaderBlock;
        public long shaderType;
        public long shaderFlags1;
        public long shaderFlags2;
        public string[] textures;
        public bool alphaProperty;
        public int alphaFlags;
        public int alphaThreshold;
        public float alpha;
        public float[] uvOffset;
        public float[] uvScale;
        public float glossiness;
        public float[] specularColor;
        public float specularStrength;
        public float[] emissiveColor;
        public float emissiveMultiple;
        public float[] skinTintColor;
        public float[] hairTintColor;
        public bool doubleSided;
        public bool modelSpaceNormals;
        // Row-major 3x3: decoded _msn texel (each channel remapped to [-1, 1]) -> normal in Unity mesh space.
        public float[] msnToMesh;
        public bool vertexColors;
        public bool vertexAlpha;

        public string Texture(int i) => textures != null && i < textures.Length ? textures[i] : "";

        // Skyrim shader types (BSLightingShaderProperty).
        public const int TypeFaceGen = 4, TypeSkinTint = 5, TypeHairTint = 6;
        // Shader flags.
        public const long SLSF1_OwnEmit = 1L << 22, SLSF2_GlowMap = 1L << 6;
        // NiAlphaProperty flags: bit 0 blending, bit 9 alpha testing.
        public bool AlphaBlend => alphaProperty && (alphaFlags & 1) != 0;
        public bool AlphaTest => alphaProperty && (alphaFlags & (1 << 9)) != 0;
    }
}
