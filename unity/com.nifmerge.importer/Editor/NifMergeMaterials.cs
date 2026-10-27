using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;

namespace NifMerge
{
    // Builds one Built-in pipeline material per sidecar slot, in <fbx dir>/<fbx name>_Materials. Skin
    // slots (SkinTint and FaceGen shader types) use the package's Skyrim Skin shader, which reproduces
    // Skyrim's additive specular; every other slot uses Standard. Skyrim's BSLightingShader has no exact
    // Standard equivalent; each mapping below is stated where made.
    public static class NifMergeMaterials
    {
        public const string SkinShader = "NifMerge/Skyrim Skin", SkinFadeShader = "NifMerge/Skyrim Skin (Fade)";

        static readonly int MainTex = Shader.PropertyToID("_MainTex");
        static readonly int Color = Shader.PropertyToID("_Color");
        static readonly int BumpMap = Shader.PropertyToID("_BumpMap");
        static readonly int DetailAlbedoMap = Shader.PropertyToID("_DetailAlbedoMap");
        static readonly int EmissionMap = Shader.PropertyToID("_EmissionMap");
        static readonly int EmissionColor = Shader.PropertyToID("_EmissionColor");
        static readonly int Glossiness = Shader.PropertyToID("_Glossiness");
        static readonly int Metallic = Shader.PropertyToID("_Metallic");
        static readonly int SpecularHighlights = Shader.PropertyToID("_SpecularHighlights");
        static readonly int Cutoff = Shader.PropertyToID("_Cutoff");
        static readonly int SpecMap = Shader.PropertyToID("_SpecMap");
        static readonly int SkyrimSpecColor = Shader.PropertyToID("_SkyrimSpecColor");
        static readonly int Shininess = Shader.PropertyToID("_Shininess");

        public static bool IsSkin(MaterialEntry m) =>
            m.shaderType == MaterialEntry.TypeSkinTint || m.shaderType == MaterialEntry.TypeFaceGen;

        public static List<(string name, Material mat)> Build(string fbxPath, Sidecar sidecar, string texturesRoot,
                                                              List<string> warnings)
        {
            string dir = Path.GetDirectoryName(fbxPath).Replace('\\', '/');
            string stem = Path.GetFileNameWithoutExtension(fbxPath);
            string folder = EnsureFolder(dir, stem + "_Materials");
            var textures = IndexTextures(texturesRoot, warnings);
            var standard = FindShader("Standard");
            var skin = FindShader(SkinShader);
            var skinFade = FindShader(SkinFadeShader);

            var result = new List<(string, Material)>();
            foreach (var m in sidecar.materials)
            {
                // Skin that blends without alpha testing needs the Fade variant; Cutout is a keyword.
                var shader = !IsSkin(m) ? standard : !m.AlphaTest && m.AlphaBlend ? skinFade : skin;
                string matPath = $"{folder}/{SafeFileName(m.name)}.mat";
                var mat = AssetDatabase.LoadAssetAtPath<Material>(matPath);
                bool created = mat == null;
                if (created) mat = new Material(shader);
                else mat.shader = shader;
                Setup(mat, m, textures, folder, warnings);
                if (created) AssetDatabase.CreateAsset(mat, matPath);
                else EditorUtility.SetDirty(mat);
                result.Add((m.name, mat));
            }
            AssetDatabase.SaveAssets();
            return result;
        }

        static Shader FindShader(string name)
        {
            var shader = Shader.Find(name);
            if (shader == null) throw new System.InvalidOperationException($"NifMerge: the {name} shader is not available");
            return shader;
        }

        static void Setup(Material mat, MaterialEntry m, Dictionary<string, string> textures, string folder,
                          List<string> warnings)
        {
            string label = $"slot '{m.name}'";

            // Diffuse.
            var diffuse = Resolve(m.Texture(0), textures, label + " diffuse", warnings);
            mat.SetTexture(MainTex, diffuse);

            // Skyrim UV transform, adjusted for the V flip NifMerge applies (v' = 1 - v).
            float su = Get(m.uvScale, 0, 1), sv = Get(m.uvScale, 1, 1);
            float ou = Get(m.uvOffset, 0, 0), ov = Get(m.uvOffset, 1, 0);
            mat.SetTextureScale(MainTex, new Vector2(su, sv));
            mat.SetTextureOffset(MainTex, new Vector2(ou, 1f - sv - ov));

            // Tint. Hair-tint shaders multiply a grey diffuse by the tint colour at double strength.
            Color c = UnityEngine.Color.white;
            if (m.shaderType == MaterialEntry.TypeHairTint)
                c = new Color(Mathf.Clamp01(2 * Get(m.hairTintColor, 0, 0.5f)), Mathf.Clamp01(2 * Get(m.hairTintColor, 1, 0.5f)),
                              Mathf.Clamp01(2 * Get(m.hairTintColor, 2, 0.5f)));
            c.a = m.alpha;
            mat.SetColor(Color, c);

            // Normal map.
            mat.SetTexture(BumpMap, null);
            mat.DisableKeyword("_NORMALMAP");
            Texture2D normalSource = null;
            if (!string.IsNullOrEmpty(m.Texture(1)))
            {
                // Model-space maps (_msn) are converted by NifMergeNormalBaker once the mesh is imported.
                if (!m.modelSpaceNormals)
                {
                    var src = normalSource = Resolve(m.Texture(1), textures, label + " normal map", warnings);
                    if (src != null)
                    {
                        var converted = ConvertNormalMap(src, folder, warnings);
                        if (converted != null)
                        {
                            mat.SetTexture(BumpMap, converted);
                            mat.EnableKeyword("_NORMALMAP");
                        }
                    }
                }
            }

            // FaceGen tint (slot 6) as a x2 multiply detail map, the closest Standard equivalent.
            mat.SetTexture(DetailAlbedoMap, null);
            mat.DisableKeyword("_DETAIL_MULX2");
            if (m.shaderType == MaterialEntry.TypeFaceGen && !string.IsNullOrEmpty(m.Texture(6)))
            {
                var tint = Resolve(m.Texture(6), textures, label + " FaceGen tint", warnings);
                if (tint != null)
                {
                    mat.SetTexture(DetailAlbedoMap, tint);
                    mat.EnableKeyword("_DETAIL_MULX2");
                }
            }

            bool specular = (m.shaderFlags1 & 1) != 0;
            if (IsSkin(m))
            {
                SetupSkinSpecular(mat, m, specular, normalSource, textures, label, warnings);
            }
            else
            {
                // Specular: Blinn-Phong power -> Standard smoothness via roughness = sqrt(2 / (power + 2)).
                mat.SetFloat(Metallic, 0f);
                mat.SetFloat(Glossiness, specular ? 1f - Mathf.Sqrt(2f / (Mathf.Max(m.glossiness, 0f) + 2f)) : 0f);
                mat.SetFloat(SpecularHighlights, specular ? 1f : 0f);
                if (specular) mat.DisableKeyword("_SPECULARHIGHLIGHTS_OFF");
                else mat.EnableKeyword("_SPECULARHIGHLIGHTS_OFF");
            }

            // Emission.
            mat.SetTexture(EmissionMap, null);
            mat.SetColor(EmissionColor, UnityEngine.Color.black);
            mat.DisableKeyword("_EMISSION");
            mat.globalIlluminationFlags = MaterialGlobalIlluminationFlags.EmissiveIsBlack;
            bool ownEmit = (m.shaderFlags1 & MaterialEntry.SLSF1_OwnEmit) != 0;
            bool glowMap = (m.shaderFlags2 & MaterialEntry.SLSF2_GlowMap) != 0 && !string.IsNullOrEmpty(m.Texture(2));
            if (ownEmit || glowMap)
            {
                var e = new Color(Get(m.emissiveColor, 0, 0), Get(m.emissiveColor, 1, 0), Get(m.emissiveColor, 2, 0)) *
                        Mathf.Max(m.emissiveMultiple, 0f);
                mat.SetColor(EmissionColor, e);
                if (glowMap) mat.SetTexture(EmissionMap, Resolve(m.Texture(2), textures, label + " glow map", warnings));
                mat.EnableKeyword("_EMISSION");
                mat.globalIlluminationFlags = MaterialGlobalIlluminationFlags.RealtimeEmissive;
            }

            // Transparency from NiAlphaProperty. Skyrim can test and blend at once; Standard cannot, so
            // alpha testing (hair, lashes, brows) wins as Cutout, blending alone becomes Fade.
            if (m.AlphaTest) SetBlendMode(mat, 1, m.alphaThreshold / 255f);
            else if (m.AlphaBlend) SetBlendMode(mat, 2, 0.5f);
            else SetBlendMode(mat, 0, 0.5f);

            if (m.doubleSided)
                warnings.Add($"{label}: double-sided in Skyrim; the material renders front faces only");
        }

        // Skyrim's specular as the skin shader computes it: specular colour x strength, unclamped and
        // added on top of the diffuse, the Blinn-Phong exponent as is, and the mask. Model-space slots
        // take the mask from the _s map in slot 7 (red channel), tangent-space slots from the normal
        // map's alpha.
        static void SetupSkinSpecular(Material mat, MaterialEntry m, bool specular, Texture2D normalSource,
                                      Dictionary<string, string> textures, string label, List<string> warnings)
        {
            var s = new Color(Get(m.specularColor, 0, 1), Get(m.specularColor, 1, 1), Get(m.specularColor, 2, 1)) *
                    Mathf.Max(m.specularStrength, 0f);
            s.a = 1f;
            mat.SetColor(SkyrimSpecColor, specular ? s : UnityEngine.Color.black);
            mat.SetFloat(Shininess, Mathf.Max(m.glossiness, 0f));
            mat.SetTexture(SpecMap, null);
            mat.DisableKeyword("_SPECMAP_ALPHA");
            if (!specular) return;

            if (m.modelSpaceNormals)
            {
                if (string.IsNullOrEmpty(m.Texture(7)))
                {
                    warnings.Add($"{label}: no specular map in slot 7; specular is unmasked");
                    return;
                }
                var mask = Resolve(m.Texture(7), textures, label + " specular map", warnings);
                if (mask == null) return;
                ImportAsData(mask, label + " specular map", warnings);
                mat.SetTexture(SpecMap, AssetDatabase.LoadAssetAtPath<Texture2D>(AssetDatabase.GetAssetPath(mask)));
            }
            else if (normalSource != null)
            {
                mat.SetTexture(SpecMap, normalSource);
                mat.EnableKeyword("_SPECMAP_ALPHA");
            }
        }

        // A mask is data, so it is sampled without sRGB decoding, as Skyrim samples it. DDS files come
        // through IHVImageFormatImporter, which has the flag only as the serialized field m_sRGBTexture.
        static void ImportAsData(Texture2D tex, string what, List<string> warnings)
        {
            string path = AssetDatabase.GetAssetPath(tex);
            var importer = AssetImporter.GetAtPath(path);
            if (importer is TextureImporter ti)
            {
                if (!ti.sRGBTexture) return;
                ti.sRGBTexture = false;
                ti.SaveAndReimport();
                return;
            }
            var flag = importer != null ? new SerializedObject(importer).FindProperty("m_sRGBTexture") : null;
            if (flag == null)
            {
                warnings.Add($"{what} '{path}': sRGB decoding could not be switched off; the mask reads darker in Linear colour space");
                return;
            }
            var so = flag.serializedObject;
            if (!flag.boolValue) return;
            flag.boolValue = false;
            so.ApplyModifiedPropertiesWithoutUndo();
            importer.SaveAndReimport();
        }

        // Mirrors StandardShaderGUI.SetupMaterialWithBlendMode. mode: 0 Opaque, 1 Cutout, 2 Fade.
        static void SetBlendMode(Material mat, int mode, float cutoff)
        {
            mat.SetFloat("_Mode", mode);
            mat.SetFloat(Cutoff, cutoff);
            mat.DisableKeyword("_ALPHATEST_ON");
            mat.DisableKeyword("_ALPHABLEND_ON");
            mat.DisableKeyword("_ALPHAPREMULTIPLY_ON");
            switch (mode)
            {
                case 0:
                    mat.SetOverrideTag("RenderType", "");
                    mat.SetFloat("_SrcBlend", (float)BlendMode.One);
                    mat.SetFloat("_DstBlend", (float)BlendMode.Zero);
                    mat.SetFloat("_ZWrite", 1f);
                    mat.renderQueue = -1;
                    break;
                case 1:
                    mat.SetOverrideTag("RenderType", "TransparentCutout");
                    mat.SetFloat("_SrcBlend", (float)BlendMode.One);
                    mat.SetFloat("_DstBlend", (float)BlendMode.Zero);
                    mat.SetFloat("_ZWrite", 1f);
                    mat.EnableKeyword("_ALPHATEST_ON");
                    mat.renderQueue = (int)RenderQueue.AlphaTest;
                    break;
                default:
                    mat.SetOverrideTag("RenderType", "Transparent");
                    mat.SetFloat("_SrcBlend", (float)BlendMode.SrcAlpha);
                    mat.SetFloat("_DstBlend", (float)BlendMode.OneMinusSrcAlpha);
                    mat.SetFloat("_ZWrite", 0f);
                    mat.EnableKeyword("_ALPHABLEND_ON");
                    mat.renderQueue = (int)RenderQueue.Transparent;
                    break;
            }
        }

        // Skyrim tangent-space normal maps use the DirectX convention (green = down) and keep a specular
        // mask in alpha. Unity expects green = up and reads alpha as part of the normal on some
        // platforms, so a converted PNG copy (green flipped, alpha opaque) is written and imported as a
        // normal map. The GPU does the decode, so every DDS format Unity can import is supported.
        static Texture2D ConvertNormalMap(Texture2D src, string folder, List<string> warnings)
        {
            string srcPath = AssetDatabase.GetAssetPath(src);
            string outFolder = EnsureFolder(folder, "Textures");
            string outPath = $"{outFolder}/{Path.GetFileNameWithoutExtension(srcPath)}_unity_n.png";
            string stamp = "nifmerge-normal:" + AssetDatabase.GetAssetDependencyHash(srcPath);

            var existing = AssetImporter.GetAtPath(outPath) as TextureImporter;
            if (existing != null && existing.userData == stamp) return AssetDatabase.LoadAssetAtPath<Texture2D>(outPath);

            int w, h;
            try
            {
                var px = ReadRaw(src, out w, out h);
                for (int i = 0; i < px.Length; i++)
                {
                    px[i].g = (byte)(255 - px[i].g);
                    px[i].a = 255;
                }
                var tex = new Texture2D(w, h, TextureFormat.RGBA32, false, true);
                tex.SetPixels32(px);
                File.WriteAllBytes(outPath, tex.EncodeToPNG());
                Object.DestroyImmediate(tex);
            }
            catch (System.Exception e)
            {
                warnings.Add($"normal map '{srcPath}' could not be converted: {e.Message}");
                return null;
            }

            AssetDatabase.ImportAsset(outPath, ImportAssetOptions.ForceUpdate);
            var ti = (TextureImporter)AssetImporter.GetAtPath(outPath);
            ti.textureType = TextureImporterType.NormalMap;
            ti.sRGBTexture = false;
            ti.maxTextureSize = Mathf.Max(ti.maxTextureSize, Mathf.NextPowerOfTwo(Mathf.Max(w, h)));
            ti.userData = stamp;
            ti.SaveAndReimport();
            return AssetDatabase.LoadAssetAtPath<Texture2D>(outPath);
        }

        // The stored texel bytes of any imported texture (DDS included), decoded by the GPU. Reading
        // through a render target whose sRGB-ness matches the source returns the bytes unchanged.
        // Row 0 is v = 0.
        public static Color32[] ReadRaw(Texture2D src, out int w, out int h)
        {
            w = src.width;
            h = src.height;
            bool srgb = GraphicsFormatUtility.IsSRGBFormat(src.graphicsFormat);
            var rt = RenderTexture.GetTemporary(w, h, 0, RenderTextureFormat.ARGB32,
                                                srgb ? RenderTextureReadWrite.sRGB : RenderTextureReadWrite.Linear);
            var prev = RenderTexture.active;
            var read = new Texture2D(w, h, TextureFormat.RGBA32, false, !srgb);
            try
            {
                Graphics.Blit(src, rt);
                RenderTexture.active = rt;
                read.ReadPixels(new Rect(0, 0, w, h), 0, 0);
                read.Apply();
                return read.GetPixels32();
            }
            finally
            {
                RenderTexture.active = prev;
                RenderTexture.ReleaseTemporary(rt);
                Object.DestroyImmediate(read);
            }
        }

        // Lower-case path relative to the textures root -> asset path, for every texture under the root.
        public static Dictionary<string, string> IndexTextures(string root, List<string> warnings)
        {
            var map = new Dictionary<string, string>();
            if (string.IsNullOrEmpty(root) || !AssetDatabase.IsValidFolder(root))
            {
                warnings?.Add($"Skyrim textures root '{root}' does not exist (Project Settings > NifMerge Importer); " +
                             "no textures assigned");
                return map;
            }
            foreach (string guid in AssetDatabase.FindAssets("t:Texture2D", new[] { root }))
            {
                string p = AssetDatabase.GUIDToAssetPath(guid);
                map[p.Substring(root.Length + 1).ToLowerInvariant()] = p;
            }
            return map;
        }

        public static Texture2D Resolve(string nifPath, Dictionary<string, string> textures, string what, List<string> warnings)
        {
            if (string.IsNullOrEmpty(nifPath)) return null;
            string rel = nifPath.Replace('\\', '/').TrimStart('/').ToLowerInvariant();
            if (rel.StartsWith("data/")) rel = rel.Substring(5);
            if (rel.StartsWith("textures/")) rel = rel.Substring(9);
            if (textures.TryGetValue(rel, out string assetPath)) return AssetDatabase.LoadAssetAtPath<Texture2D>(assetPath);
            warnings.Add($"{what} '{nifPath}' not found under the textures root");
            return null;
        }

        internal static string EnsureFolder(string parent, string name)
        {
            string path = $"{parent}/{name}";
            if (!AssetDatabase.IsValidFolder(path)) AssetDatabase.CreateFolder(parent, name);
            return path;
        }

        internal static string SafeFileName(string s)
        {
            foreach (char ch in Path.GetInvalidFileNameChars()) s = s.Replace(ch, '_');
            return s;
        }

        static float Get(float[] a, int i, float fallback) => a != null && i < a.Length ? a[i] : fallback;
    }
}
