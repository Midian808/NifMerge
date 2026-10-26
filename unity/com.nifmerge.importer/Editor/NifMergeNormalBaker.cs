using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;
using UnityEngine.Experimental.Rendering;

namespace NifMerge
{
    // Converts Skyrim model-space normal maps (_msn) into tangent-space normal maps for the Standard
    // shader. Every texel covered by a slot's triangles is re-expressed in that texel's tangent frame,
    // interpolated from the imported mesh's own normals and (Mikk) tangents, so the result matches the
    // basis the shader reconstructs. Slots sharing one _msn texture (Skyrim body and feet) are baked
    // into one output texture.
    public static class NifMergeNormalBaker
    {
        const string Version = "3";

        public static void Bake(string fbxPath, Sidecar sidecar, string texturesRoot, List<string> warnings)
        {
            var slots = Enumerable.Range(0, sidecar.materials.Length)
                                  .Where(i => sidecar.materials[i].modelSpaceNormals &&
                                              !string.IsNullOrEmpty(sidecar.materials[i].Texture(1)))
                                  .ToList();
            if (slots.Count == 0) return;

            var prefab = AssetDatabase.LoadAssetAtPath<GameObject>(fbxPath);
            var smr = prefab ? prefab.GetComponentInChildren<SkinnedMeshRenderer>() : null;
            var mesh = smr ? smr.sharedMesh : null;
            if (mesh == null || mesh.subMeshCount != sidecar.materials.Length)
            {
                warnings.Add("model-space normal maps not converted: the imported mesh does not have one submesh per slot");
                return;
            }
            var tangents = mesh.tangents;
            if (tangents.Length != mesh.vertexCount)
            {
                warnings.Add("model-space normal maps not converted: the imported mesh has no tangents");
                return;
            }

            string dir = Path.GetDirectoryName(fbxPath).Replace('\\', '/');
            string matFolder = $"{dir}/{Path.GetFileNameWithoutExtension(fbxPath)}_Materials";
            var textures = NifMergeMaterials.IndexTextures(texturesRoot, null);

            // Group slots by the _msn texture they use.
            var groups = new Dictionary<string, List<int>>();
            foreach (int i in slots)
            {
                var src = NifMergeMaterials.Resolve(sidecar.materials[i].Texture(1), textures,
                                                    $"slot '{sidecar.materials[i].name}' model-space normal map", warnings);
                if (src == null) continue;
                string key = AssetDatabase.GetAssetPath(src);
                if (!groups.TryGetValue(key, out var list)) groups[key] = list = new List<int>();
                list.Add(i);
            }

            var normals = mesh.normals;
            var uvs = mesh.uv;
            foreach (var g in groups)
            {
                var src = AssetDatabase.LoadAssetAtPath<Texture2D>(g.Key);
                string outFolder = NifMergeMaterials.EnsureFolder(matFolder, "Textures");
                string outPath = $"{outFolder}/{Path.GetFileNameWithoutExtension(g.Key)}_unity_ts_n.png";
                string stamp = "nifmerge-msn:" + Version + ":" + AssetDatabase.GetAssetDependencyHash(g.Key) + ":" +
                               AssetDatabase.GetAssetDependencyHash(fbxPath) + ":" +
                               string.Join(",", g.Value.Select(i => i + "=" + string.Join(" ", sidecar.materials[i].msnToMesh)));

                var existing = AssetImporter.GetAtPath(outPath) as TextureImporter;
                if (existing == null || existing.userData != stamp)
                {
                    var raw = NifMergeMaterials.ReadRaw(src, out int w, out int h);
                    var result = Convert(raw, w, h, mesh, normals, tangents, uvs, g.Value, sidecar, out int covered);
                    if (covered == 0)
                    {
                        warnings.Add($"model-space normal map '{g.Key}': the slots' UVs cover no texels; not converted");
                        continue;
                    }
                    var tex = new Texture2D(w, h, TextureFormat.RGBA32, false, true);
                    tex.SetPixels32(result);
                    File.WriteAllBytes(outPath, tex.EncodeToPNG());
                    Object.DestroyImmediate(tex);
                    AssetDatabase.ImportAsset(outPath, ImportAssetOptions.ForceUpdate);
                    var ti = (TextureImporter)AssetImporter.GetAtPath(outPath);
                    ti.textureType = TextureImporterType.NormalMap;
                    ti.sRGBTexture = false;
                    ti.maxTextureSize = Mathf.Max(ti.maxTextureSize, Mathf.NextPowerOfTwo(Mathf.Max(w, h)));
                    ti.userData = stamp;
                    ti.SaveAndReimport();
                }

                var converted = AssetDatabase.LoadAssetAtPath<Texture2D>(outPath);
                foreach (int i in g.Value)
                {
                    var mat = AssetDatabase.LoadAssetAtPath<Material>(
                        $"{matFolder}/{NifMergeMaterials.SafeFileName(sidecar.materials[i].name)}.mat");
                    if (mat == null) continue;
                    mat.SetTexture("_BumpMap", converted);
                    mat.EnableKeyword("_NORMALMAP");
                    EditorUtility.SetDirty(mat);
                }
            }
            AssetDatabase.SaveAssets();
        }

        // Pixel (x, y) of the output corresponds to pixel (x, y) of the source: both are addressed by the
        // same mesh UVs. Row 0 is v = 0.
        //   1. Texels whose centre a triangle covers are converted with the frame interpolated there.
        //   2. Texels within BandTexels of an island are converted from the source's own padding, with
        //      the frame of the nearest triangle (evaluated at the closest point on it). Bilinear and
        //      mip sampling at island edges then mixes the same normals the source would.
        //   3. Everything further out is flood-filled from its neighbours.
        public static Color32[] Convert(Color32[] src, int w, int h, Mesh mesh, Vector3[] normals, Vector4[] tangents,
                                        Vector2[] uvs, List<int> slots, Sidecar sidecar, out int covered)
        {
            var dst = new Color32[w * h];
            var filled = new bool[w * h];
            covered = 0;
            foreach (int slot in slots)
            {
                var m = sidecar.materials[slot].msnToMesh;
                int[] idx = mesh.GetTriangles(slot);
                for (int t = 0; t < idx.Length; t += 3)
                {
                    int a = idx[t], b = idx[t + 1], c = idx[t + 2];
                    Vector2 pa = Pixel(uvs[a], w, h), pb = Pixel(uvs[b], w, h), pc = Pixel(uvs[c], w, h);
                    float area = Edge(pa, pb, pc);
                    if (Mathf.Abs(area) < 1e-12f) continue;
                    Bounds(pa, pb, pc, 0, w, h, out int x0, out int x1, out int y0, out int y1);
                    for (int y = y0; y <= y1; y++)
                        for (int x = x0; x <= x1; x++)
                        {
                            var p = new Vector2(x, y);
                            float l0 = Edge(pb, pc, p) / area, l1 = Edge(pc, pa, p) / area, l2 = Edge(pa, pb, p) / area;
                            const float eps = -1e-4f;
                            if (l0 < eps || l1 < eps || l2 < eps) continue;
                            int i = y * w + x;
                            if (!ConvertTexel(src[i], m, normals, tangents, a, b, c, l0, l1, l2, out dst[i])) continue;
                            if (!filled[i]) covered++;
                            filled[i] = true;
                        }
                }
            }

            var bandDist = new float[w * h];
            for (int i = 0; i < bandDist.Length; i++) bandDist[i] = float.MaxValue;
            var inBand = new bool[w * h];
            foreach (int slot in slots)
            {
                var m = sidecar.materials[slot].msnToMesh;
                int[] idx = mesh.GetTriangles(slot);
                for (int t = 0; t < idx.Length; t += 3)
                {
                    int a = idx[t], b = idx[t + 1], c = idx[t + 2];
                    Vector2 pa = Pixel(uvs[a], w, h), pb = Pixel(uvs[b], w, h), pc = Pixel(uvs[c], w, h);
                    float area = Edge(pa, pb, pc);
                    if (Mathf.Abs(area) < 1e-12f) continue;
                    Bounds(pa, pb, pc, BandTexels, w, h, out int x0, out int x1, out int y0, out int y1);
                    for (int y = y0; y <= y1; y++)
                        for (int x = x0; x <= x1; x++)
                        {
                            int i = y * w + x;
                            if (filled[i]) continue;
                            var p = new Vector2(x, y);
                            Vector2 q = ClosestPointOnTriangle(p, pa, pb, pc);
                            float d = (q - p).sqrMagnitude;
                            if (d > BandTexels * BandTexels || d >= bandDist[i]) continue;
                            float l0 = Edge(pb, pc, q) / area, l1 = Edge(pc, pa, q) / area, l2 = 1f - l0 - l1;
                            if (!ConvertTexel(src[i], m, normals, tangents, a, b, c, l0, l1, l2, out dst[i])) continue;
                            bandDist[i] = d;
                            inBand[i] = true;
                        }
                }
            }
            for (int i = 0; i < filled.Length; i++)
                if (inBand[i]) filled[i] = true;

            Dilate(dst, filled, w, h);
            var flat = Encode(Vector3.forward);
            for (int i = 0; i < dst.Length; i++)
                if (!filled[i]) dst[i] = flat;
            return dst;
        }

        const int BandTexels = 16;

        // Converts one source texel with the tangent frame interpolated from triangle (a, b, c).
        static bool ConvertTexel(Color32 texel, float[] m, Vector3[] normals, Vector4[] tangents, int a, int b, int c,
                                 float l0, float l1, float l2, out Color32 result)
        {
            result = default;
            Vector3 n = (normals[a] * l0 + normals[b] * l1 + normals[c] * l2).normalized;
            Vector3 tg = (Vector3)tangents[a] * l0 + (Vector3)tangents[b] * l1 + (Vector3)tangents[c] * l2;
            tg = (tg - n * Vector3.Dot(n, tg)).normalized;
            if (n == Vector3.zero || tg == Vector3.zero) return false;
            Vector3 bt = Vector3.Cross(n, tg) * (tangents[a].w < 0 ? -1f : 1f);
            Vector3 ms = MeshNormal(texel, m);
            var ts = new Vector3(Vector3.Dot(ms, tg), Vector3.Dot(ms, bt), Vector3.Dot(ms, n));
            if (ts.z < 0.05f) ts.z = 0.05f; // keep the normal in the front hemisphere
            result = Encode(ts.normalized);
            return true;
        }

        static void Bounds(Vector2 a, Vector2 b, Vector2 c, int pad, int w, int h, out int x0, out int x1, out int y0, out int y1)
        {
            x0 = Mathf.Max(0, Mathf.FloorToInt(Mathf.Min(a.x, Mathf.Min(b.x, c.x))) - pad);
            x1 = Mathf.Min(w - 1, Mathf.CeilToInt(Mathf.Max(a.x, Mathf.Max(b.x, c.x))) + pad);
            y0 = Mathf.Max(0, Mathf.FloorToInt(Mathf.Min(a.y, Mathf.Min(b.y, c.y))) - pad);
            y1 = Mathf.Min(h - 1, Mathf.CeilToInt(Mathf.Max(a.y, Mathf.Max(b.y, c.y))) + pad);
        }

        // Closest point to p on triangle abc (Ericson, Real-Time Collision Detection, 5.1.5), in 2D.
        static Vector2 ClosestPointOnTriangle(Vector2 p, Vector2 a, Vector2 b, Vector2 c)
        {
            Vector2 ab = b - a, ac = c - a, ap = p - a;
            float d1 = Vector2.Dot(ab, ap), d2 = Vector2.Dot(ac, ap);
            if (d1 <= 0 && d2 <= 0) return a;
            Vector2 bp = p - b;
            float d3 = Vector2.Dot(ab, bp), d4 = Vector2.Dot(ac, bp);
            if (d3 >= 0 && d4 <= d3) return b;
            float vc = d1 * d4 - d3 * d2;
            if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
            Vector2 cp = p - c;
            float d5 = Vector2.Dot(ab, cp), d6 = Vector2.Dot(ac, cp);
            if (d6 >= 0 && d5 <= d6) return c;
            float vb = d5 * d2 - d1 * d6;
            if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
            float va = d3 * d6 - d5 * d4;
            if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) return b + (c - b) * ((d4 - d3) / (d4 - d3 + d5 - d6));
            float denom = 1f / (va + vb + vc);
            return a + ab * (vb * denom) + ac * (vc * denom);
        }

        // Decoded texel -> normal in mesh space through the sidecar's matrix (row-major 3x3).
        public static Vector3 MeshNormal(Color32 c, float[] m)
        {
            float r = c.r / 255f * 2f - 1f, g = c.g / 255f * 2f - 1f, b = c.b / 255f * 2f - 1f;
            return new Vector3(m[0] * r + m[1] * g + m[2] * b, m[3] * r + m[4] * g + m[5] * b,
                               m[6] * r + m[7] * g + m[8] * b).normalized;
        }

        public static Color32 Encode(Vector3 ts) =>
            new Color32((byte)Mathf.RoundToInt((ts.x * 0.5f + 0.5f) * 255f), (byte)Mathf.RoundToInt((ts.y * 0.5f + 0.5f) * 255f),
                        (byte)Mathf.RoundToInt((ts.z * 0.5f + 0.5f) * 255f), 255);

        public static Vector3 Decode(Color32 c) =>
            new Vector3(c.r / 255f * 2f - 1f, c.g / 255f * 2f - 1f, c.b / 255f * 2f - 1f).normalized;

        // Texel-centre coordinates: texel (x, y) covers UV [x/w, (x+1)/w).
        static Vector2 Pixel(Vector2 uv, int w, int h) => new Vector2(uv.x * w - 0.5f, uv.y * h - 0.5f);

        static float Edge(Vector2 a, Vector2 b, Vector2 p) => (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);

        // Fills every texel still uncovered from its nearest filled neighbours, ring by ring outwards, so
        // low mip levels never mix in a flat background.
        static void Dilate(Color32[] px, bool[] filled, int w, int h)
        {
            var frontier = new List<int>();
            var queued = new bool[px.Length];
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                {
                    int i = y * w + x;
                    if (filled[i] || !HasFilledNeighbour(filled, x, y, w, h)) continue;
                    frontier.Add(i);
                    queued[i] = true;
                }
            var next = new List<int>();
            while (frontier.Count > 0)
            {
                foreach (int i in frontier)
                {
                    int x = i % w, y = i / w, n = 0;
                    Vector3 sum = Vector3.zero;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++)
                        {
                            int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                            int j = ny * w + nx;
                            if (!filled[j]) continue;
                            sum += Decode(px[j]);
                            n++;
                        }
                    px[i] = Encode(n > 0 ? sum.normalized : Vector3.forward);
                }
                next.Clear();
                foreach (int i in frontier) filled[i] = true;
                foreach (int i in frontier)
                {
                    int x = i % w, y = i / w;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++)
                        {
                            int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                            int j = ny * w + nx;
                            if (filled[j] || queued[j]) continue;
                            queued[j] = true;
                            next.Add(j);
                        }
                }
                var t = frontier;
                frontier = next;
                next = t;
            }
        }

        static bool HasFilledNeighbour(bool[] filled, int x, int y, int w, int h)
        {
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < w && ny < h && filled[ny * w + nx]) return true;
                }
            return false;
        }
    }
}
