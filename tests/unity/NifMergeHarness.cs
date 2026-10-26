// Batch-mode verification of a NifMerge model imported through the com.nifmerge.importer package.
// Run by tests/run_unity_harness.py:  Unity -batchmode -projectPath <p> -executeMethod NifMergeHarness.Run
// It checks the state the automatic import produced; it never configures the model itself.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using NifMerge;
using UnityEditor;
using UnityEngine;

public static class NifMergeHarness
{
    const string Fbx = "Assets/NifMergeTest/Character.fbx";
    static readonly List<string> Lines = new List<string>();
    static int failures, checks;

    static void Check(bool ok, string what)
    {
        checks++;
        if (!ok) failures++;
        Lines.Add((ok ? "PASS " : "FAIL ") + what);
    }

    public static void Run()
    {
        string outDir = Environment.GetEnvironmentVariable("NIFMERGE_HARNESS_OUT") ?? "HarnessOut";
        Directory.CreateDirectory(outDir);
        int code = 1;
        try
        {
            AssetDatabase.Refresh();
            RunChecks(outDir);
            code = failures == 0 ? 0 : 1;
        }
        catch (Exception e)
        {
            Lines.Add("FAIL exception: " + e);
            failures++;
        }
        Lines.Add($"Unity {Application.unityVersion}: {checks} checks, {failures} failures");
        File.WriteAllLines(Path.Combine(outDir, "harness_result.txt"), Lines);
        EditorApplication.Exit(code);
    }

    static void RunChecks(string outDir)
    {
        var sidecar = Sidecar.TryLoad(Fbx, out _);
        Check(sidecar != null, "sidecar loads");
        var importer = AssetImporter.GetAtPath(Fbx) as ModelImporter;
        Check(importer != null, "FBX imported");
        if (sidecar == null || importer == null) return;

        // ---- Automatic configuration happened on import.
        Check(importer.userData != null && importer.userData.StartsWith("nifmerge:"), "importer stamped by NifMerge");
        Check(importer.animationType == ModelImporterAnimationType.Human, "animation type is Human");
        var avatar = AssetDatabase.LoadAllAssetsAtPath(Fbx).OfType<Avatar>().FirstOrDefault();
        Check(avatar != null && avatar.isValid && avatar.isHuman, "avatar is valid and human");
        string reportPath = NifMergeImporter.ReportPathFor(Fbx);
        Check(File.Exists(reportPath), "import report written");
        var warnings = File.Exists(reportPath) ? File.ReadAllLines(reportPath).ToList() : new List<string>();
        Lines.Add($"INFO importer warnings ({warnings.Count}):");
        foreach (var w in warnings) Lines.Add("INFO   " + w);

        var prefab = AssetDatabase.LoadAssetAtPath<GameObject>(Fbx);
        var go = UnityEngine.Object.Instantiate(prefab);
        go.transform.SetPositionAndRotation(Vector3.zero, Quaternion.identity);
        try
        {
            var animator = go.GetComponent<Animator>();
            Check(animator != null && animator.avatar == avatar, "Animator carries the avatar");
            if (animator == null) return;
            animator.enabled = false;

            // ---- Every sidecar humanoid mapping is live in the avatar.
            var names = HumanTrait.BoneName;
            foreach (var h in sidecar.humanoid)
            {
                int idx = Array.IndexOf(names, h.human);
                Transform t = idx >= 0 ? animator.GetBoneTransform((HumanBodyBones)idx) : null;
                Check(t != null && t.name == h.bone, $"humanoid {h.human} -> '{h.bone}'");
            }

            // ---- Materials.
            var smr = go.GetComponentInChildren<SkinnedMeshRenderer>();
            Check(smr != null, "SkinnedMeshRenderer present");
            if (smr == null) return;
            var mats = smr.sharedMaterials;
            Check(mats.Length == sidecar.materials.Length, $"{mats.Length} material slots == {sidecar.materials.Length}");
            Check(smr.sharedMesh.subMeshCount == sidecar.materials.Length, "one submesh per slot");
            string matFolder = "Assets/NifMergeTest/Character_Materials";
            for (int i = 0; i < Math.Min(mats.Length, sidecar.materials.Length); i++)
            {
                string path = mats[i] ? AssetDatabase.GetAssetPath(mats[i]) : "";
                Check(path == $"{matFolder}/{sidecar.materials[i].name}.mat", $"slot {i} uses {sidecar.materials[i].name}.mat (got '{path}')");
                Check(mats[i] && mats[i].shader.name == "Standard", $"slot {i} uses the Standard shader");
            }
            // Slots are found by role (see Roles), never by the sample's own shape names.
            int bodySlot = Roles.Body(sidecar), hairSlot = Roles.Hair(sidecar), headSlot = Roles.Head(sidecar);
            Check(bodySlot >= 0 && hairSlot >= 0 && headSlot >= 0, "body, hair and FaceGen head slots found");
            if (bodySlot < 0 || hairSlot < 0 || headSlot < 0) return;

            var body = mats[bodySlot];
            Check(SameTexture(TexPath(body, "_MainTex"), sidecar.materials[bodySlot].Texture(0)),
                  "body diffuse assigned from the textures root");
            Check(body.GetFloat("_Mode") == 0, "body is opaque");

            var hair = mats[hairSlot];
            var hairEntry = sidecar.materials[hairSlot];
            Check(hair.GetFloat("_Mode") == 1, "hair is Cutout");
            Check(Mathf.Abs(hair.GetFloat("_Cutoff") - hairEntry.alphaThreshold / 255f) < 1e-4, "hair cutoff = alpha threshold");
            Check(SameTexture(TexPath(hair, "_MainTex"), hairEntry.Texture(0)), "hair diffuse assigned");
            string nPath = TexPath(hair, "_BumpMap");
            string nWant = $"Textures/{Path.GetFileNameWithoutExtension(hairEntry.Texture(1).Replace('\\', '/'))}_unity_n.png";
            Check(nPath.EndsWith(nWant), $"hair normal map converted (got '{nPath}')");
            var nImp = AssetImporter.GetAtPath(nPath) as TextureImporter;
            Check(nImp != null && nImp.textureType == TextureImporterType.NormalMap, "converted map imported as a normal map");
            if (File.Exists(nPath))
            {
                var png = new Texture2D(2, 2, TextureFormat.RGBA32, false, true);
                png.LoadImage(File.ReadAllBytes(nPath));
                Color32 p = png.GetPixels32()[0];
                // Source texel (R132, G65, B255, A100 after BC quantisation): green flipped, alpha forced opaque.
                Check(Math.Abs(p.g - 190) <= 3 && p.a == 255 && Math.Abs(p.r - 132) <= 3, $"normal texel green flipped, alpha opaque (got {p})");
            }

            CheckModelSpaceNormals(smr, sidecar, mats, warnings);
            Check(SameTexture(TexPath(mats[headSlot], "_DetailAlbedoMap"), sidecar.materials[headSlot].Texture(6)),
                  "FaceGen tint assigned as detail map");

            // ---- Orientation and scale: Skyrim (x, y, z) units -> Unity (x, z, y) metres, facing +Z.
            const float k = 0.0142875f;
            Transform headBone = animator.GetBoneTransform(HumanBodyBones.Head);
            Vector3 headWant = new Vector3(0f, 120.344f * k, -1.548f * k);
            Check((headBone.position - headWant).magnitude < 2e-3f, $"head bone at {headBone.position:F4}, expected {headWant:F4}");
            Check(animator.GetBoneTransform(HumanBodyBones.LeftHand).position.x < -0.1f, "left hand on the character's left (-X)");
            int eyes = Array.FindIndex(sidecar.materials, m => m.shape.Contains("Eyes"));
            Vector3 eyeCentre = SubmeshCentroid(smr.sharedMesh, eyes);
            Check(eyeCentre.z > headBone.position.z + 0.03f && eyeCentre.y > headBone.position.y, $"eyes in front of and above the head bone ({eyeCentre:F4})");

            // ---- Skinning: at the bind pose the skinned result equals the mesh.
            var baked = new Mesh();
            smr.BakeMesh(baked);
            var a = baked.vertices;
            var b = smr.sharedMesh.vertices;
            float worst = 0;
            for (int i = 0; i < a.Length; i++) worst = Mathf.Max(worst, (a[i] - b[i]).magnitude);
            Check(worst < 1e-4f, $"bind pose skins to the rest mesh (max error {worst:G3})");
            UnityEngine.Object.DestroyImmediate(baked);

            Render(go, Path.Combine(outDir, "bind_pose.png"));

            // ---- The avatar's reference skeleton is a T-pose (arms out along X, legs straight down).
            var hd = importer.humanDescription;
            var refPose = hd.skeleton.ToDictionary(b => b.name, b => b);
            var savedLocal = go.GetComponentsInChildren<Transform>(true).ToDictionary(t => t, t => (t.localPosition, t.localRotation));
            foreach (var t in go.GetComponentsInChildren<Transform>(true))
                if (t != go.transform && refPose.TryGetValue(t.name, out var sb)) t.localRotation = sb.rotation;
            Check(Mathf.Abs(LimbDrop(animator, HumanBodyBones.LeftUpperArm, HumanBodyBones.LeftLowerArm)) < 1f &&
                  Mathf.Abs(LimbDrop(animator, HumanBodyBones.RightLowerArm, HumanBodyBones.RightHand)) < 1f,
                  "reference skeleton: arms horizontal");
            Check(LimbDrop(animator, HumanBodyBones.LeftUpperLeg, HumanBodyBones.LeftLowerLeg) > 89f &&
                  LimbDrop(animator, HumanBodyBones.RightLowerLeg, HumanBodyBones.RightFoot) > 89f,
                  "reference skeleton: legs straight down");
            Render(go, Path.Combine(outDir, "reference_t_pose.png"));
            foreach (var kv in savedLocal) { kv.Key.localPosition = kv.Value.Item1; kv.Key.localRotation = kv.Value.Item2; }

            // ---- Retargeting matches a canonical T-pose rig: the same muscle values must produce the
            // same limb angles on our avatar as on a synthetic humanoid built in an exact T-pose.
            float armBind = LimbDrop(animator, HumanBodyBones.LeftUpperArm, HumanBodyBones.LeftLowerArm);
            var ours = MuscleZeroAngles(avatar, go.transform, animator);
            var canonRoot = BuildCanonicalRig(out var canonAvatar, out var canonAnimator);
            try
            {
                Check(canonAvatar.isValid && canonAvatar.isHuman, "canonical T-pose rig builds a valid avatar");
                var canon = MuscleZeroAngles(canonAvatar, canonRoot.transform, canonAnimator);
                Check(armBind > 30f, $"bind pose has the arms down ({armBind:F1} deg below horizontal)");
                for (int i = 0; i < ours.Length; i++)
                    Check(Mathf.Abs(ours[i] - canon[i]) < 4f,
                          $"retarget limb angle {i}: ours {ours[i]:F1} deg vs canonical {canon[i]:F1} deg");
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(canonRoot);
            }
            Render(go, Path.Combine(outDir, "muscle_zero_pose.png"));
        }
        finally
        {
            UnityEngine.Object.DestroyImmediate(go);
        }
    }

    // Degrees the limb segment points below horizontal.
    static float LimbDrop(Animator a, HumanBodyBones from, HumanBodyBones to)
    {
        Vector3 d = a.GetBoneTransform(to).position - a.GetBoneTransform(from).position;
        return Mathf.Asin(Mathf.Clamp(-d.y / d.magnitude, -1f, 1f)) * Mathf.Rad2Deg;
    }

    // Limb angles (below horizontal) with every muscle at 0: left/right upper arm, forearm, thigh, shin.
    static float[] MuscleZeroAngles(Avatar avatar, Transform root, Animator a)
    {
        var handler = new HumanPoseHandler(avatar, root);
        var pose = new HumanPose();
        handler.GetHumanPose(ref pose);
        for (int i = 0; i < pose.muscles.Length; i++) pose.muscles[i] = 0f;
        pose.bodyRotation = Quaternion.identity;
        handler.SetHumanPose(ref pose);
        return new[]
        {
            LimbDrop(a, HumanBodyBones.LeftUpperArm, HumanBodyBones.LeftLowerArm),
            LimbDrop(a, HumanBodyBones.RightUpperArm, HumanBodyBones.RightLowerArm),
            LimbDrop(a, HumanBodyBones.LeftLowerArm, HumanBodyBones.LeftHand),
            LimbDrop(a, HumanBodyBones.RightLowerArm, HumanBodyBones.RightHand),
            LimbDrop(a, HumanBodyBones.LeftUpperLeg, HumanBodyBones.LeftLowerLeg),
            LimbDrop(a, HumanBodyBones.RightUpperLeg, HumanBodyBones.RightLowerLeg),
            LimbDrop(a, HumanBodyBones.LeftLowerLeg, HumanBodyBones.LeftFoot),
            LimbDrop(a, HumanBodyBones.RightLowerLeg, HumanBodyBones.RightFoot),
        };
    }

    // A minimal humanoid in an exact T-pose (facing +Z), used as the retargeting reference.
    static GameObject BuildCanonicalRig(out Avatar avatar, out Animator animator)
    {
        var root = new GameObject("CanonicalRig");
        var bones = new List<(string human, Transform t)>();
        Transform Add(string human, Transform parent, Vector3 worldPos)
        {
            var t = new GameObject(human).transform;
            t.SetParent(parent ? parent : root.transform, false);
            t.position = worldPos;
            bones.Add((human, t));
            return t;
        }
        var hips = Add("Hips", null, new Vector3(0, 1.0f, 0));
        var spine = Add("Spine", hips, new Vector3(0, 1.1f, 0));
        var chest = Add("Chest", spine, new Vector3(0, 1.3f, 0));
        var neck = Add("Neck", chest, new Vector3(0, 1.5f, 0));
        Add("Head", neck, new Vector3(0, 1.6f, 0));
        foreach (var (side, sx) in new[] { ("Left", -1f), ("Right", 1f) })
        {
            var sh = Add(side + "Shoulder", chest, new Vector3(0.05f * sx, 1.45f, 0));
            var ua = Add(side + "UpperArm", sh, new Vector3(0.17f * sx, 1.45f, 0));
            var la = Add(side + "LowerArm", ua, new Vector3(0.45f * sx, 1.45f, 0));
            Add(side + "Hand", la, new Vector3(0.70f * sx, 1.45f, 0));
            var ul = Add(side + "UpperLeg", hips, new Vector3(0.1f * sx, 0.95f, 0));
            var ll = Add(side + "LowerLeg", ul, new Vector3(0.1f * sx, 0.52f, 0));
            var ft = Add(side + "Foot", ll, new Vector3(0.1f * sx, 0.08f, 0));
            Add(side + "Toes", ft, new Vector3(0.1f * sx, 0.0f, 0.12f));
        }
        var hd = new HumanDescription
        {
            human = bones.Select(b => { var hb = new HumanBone { humanName = b.human, boneName = b.human }; hb.limit.useDefaultValues = true; return hb; }).ToArray(),
            skeleton = root.GetComponentsInChildren<Transform>().Select(t => new SkeletonBone { name = t.name, position = t.localPosition, rotation = t.localRotation, scale = t.localScale }).ToArray(),
            upperArmTwist = 0.5f, lowerArmTwist = 0.5f, upperLegTwist = 0.5f, lowerLegTwist = 0.5f,
            armStretch = 0.05f, legStretch = 0.05f, feetSpacing = 0f, hasTranslationDoF = false
        };
        avatar = AvatarBuilder.BuildHumanAvatar(root, hd);
        animator = root.AddComponent<Animator>();
        animator.avatar = avatar;
        animator.enabled = false;
        return root;
    }

    // Model-space (_msn) maps are baked to tangent space. For each slot, sampled triangle centroids are
    // checked two ways: (1) the baked texel, read back through the mesh's tangent frame, reproduces the
    // _msn normal; (2) the _msn normal agrees with the imported mesh's own normal, which proves the
    // channel/axis mapping against Unity's mesh, not against our own assumptions.
    static void CheckModelSpaceNormals(SkinnedMeshRenderer smr, Sidecar sidecar, Material[] mats, List<string> warnings)
    {
        Check(!warnings.Any(w => w.Contains("not supported by the Standard")), "no model-space map left unconverted");
        var mesh = smr.sharedMesh;
        var normals = mesh.normals;
        var tangents = mesh.tangents;
        var uvs = mesh.uv;
        var index = NifMergeMaterials.IndexTextures("Assets/SkyrimTextures", null);
        var bakedBySource = new Dictionary<string, Texture>();
        int converted = 0;
        for (int slot = 0; slot < sidecar.materials.Length; slot++)
        {
            var entry = sidecar.materials[slot];
            if (!entry.modelSpaceNormals || string.IsNullOrEmpty(entry.Texture(1))) continue;
            string shape = $"slot {slot} ({Roles.Describe(entry)})";
            var srcTex = NifMergeMaterials.Resolve(entry.Texture(1), index, shape, new List<string>());
            if (srcTex == null)
            {
                // The driver only supplies the _msn maps present in samples/; the rest must be reported.
                Check(warnings.Any(w => w.Contains(entry.Texture(1)) && w.Contains("not found")),
                      $"{shape}: missing _msn texture reported");
                continue;
            }
            converted++;
            var bump = mats[slot].GetTexture("_BumpMap");
            string path = bump ? AssetDatabase.GetAssetPath(bump) : "";
            Check(path.EndsWith("_unity_ts_n.png") && mats[slot].IsKeywordEnabled("_NORMALMAP"),
                  $"{shape}: model-space map baked to tangent space (got '{path}')");
            var imp = AssetImporter.GetAtPath(path) as TextureImporter;
            Check(imp != null && imp.textureType == TextureImporterType.NormalMap, $"{shape}: baked map imported as a normal map");
            string key = entry.Texture(1).ToLowerInvariant();
            if (bakedBySource.TryGetValue(key, out var shared))
                Check(bump == shared, $"{shape}: shares the baked map of the other slot using the same _msn");
            else
                bakedBySource[key] = bump;
            if (imp == null) continue;

            var src = NifMergeMaterials.ReadRaw(srcTex, out int w, out int h);
            var png = new Texture2D(2, 2, TextureFormat.RGBA32, false, true);
            png.LoadImage(File.ReadAllBytes(path));
            Check(png.width == w && png.height == h, $"{shape}: baked map keeps the source resolution {w}x{h}");
            var baked = png.GetPixels32();
            int[] tri = mesh.GetTriangles(slot);
            int samples = 0, roundTripOk = 0, subTexel = 0;
            double geo = 0, tsz = 0;
            for (int t = 0; t < tri.Length; t += 3 * 25)
            {
                int a = tri[t], b = tri[t + 1], c = tri[t + 2];
                Vector2 uv = (uvs[a] + uvs[b] + uvs[c]) / 3f;
                int x = Mathf.Clamp((int)(uv.x * w), 0, w - 1), y = Mathf.Clamp((int)(uv.y * h), 0, h - 1);
                // Evaluate the frame at the texel centre, as the shader does. A texel whose centre lies
                // outside this (sub-texel) triangle belongs to a neighbour and is not this triangle's sample.
                var centre = new Vector2((x + 0.5f) / w, (y + 0.5f) / h);
                if (!Barycentric(uvs[a], uvs[b], uvs[c], centre, out float l0, out float l1, out float l2))
                {
                    subTexel++;
                    continue;
                }
                Vector3 n = (normals[a] * l0 + normals[b] * l1 + normals[c] * l2).normalized;
                Vector3 tg = (Vector3)tangents[a] * l0 + (Vector3)tangents[b] * l1 + (Vector3)tangents[c] * l2;
                tg = (tg - n * Vector3.Dot(n, tg)).normalized;
                Vector3 bt = Vector3.Cross(n, tg) * (tangents[a].w < 0 ? -1f : 1f);
                Vector3 expected = NifMergeNormalBaker.MeshNormal(src[y * w + x], entry.msnToMesh);
                if (Vector3.Dot(expected, n) < 0.05f) continue; // clamped to the front hemisphere by design
                Vector3 ts = NifMergeNormalBaker.Decode(baked[y * w + x]);
                Vector3 recon = (ts.x * tg + ts.y * bt + ts.z * n).normalized;
                samples++;
                if (Vector3.Dot(recon, expected) > 0.99f) roundTripOk++;
                geo += Vector3.Dot(expected, n);
                tsz += ts.z;
            }
            SeamFidelity(shape, mesh, slot, entry, src, baked, w, h);
            UnityEngine.Object.DestroyImmediate(png);
            Check(samples > 20 && subTexel < samples, $"{shape}: {samples} samples ({subTexel} sub-texel triangles skipped)");
            if (samples == 0) continue;
            Check(roundTripOk >= samples * 0.98, $"{shape}: baked texel reproduces the _msn normal ({roundTripOk}/{samples} within 8 deg)");
            Check(geo / samples > 0.85, $"{shape}: _msn normals agree with the mesh normals (mean cos {geo / samples:F3})");
            Check(tsz / samples > 0.85, $"{shape}: tangent-space normals point outwards (mean z {tsz / samples:F3})");
        }
        Check(converted > 0, $"{converted} model-space slots converted from the supplied _msn maps");
    }

    // Finds slots by what they are, using only the sidecar's shader data and Skyrim's standard part
    // file names. Must match the role rules in tests/run_unity_harness.py.
    static class Roles
    {
        static int Find(Sidecar s, Func<MaterialEntry, bool> f) => Array.FindIndex(s.materials, m => f(m));
        static bool From(MaterialEntry m, string part) => m.sourceFile.ToLowerInvariant().StartsWith(part);
        public static int Body(Sidecar s) => Find(s, m => From(m, "femalebody"));
        public static int Head(Sidecar s) => Find(s, m => m.shaderType == MaterialEntry.TypeFaceGen);
        public static int Hair(Sidecar s) =>
            Find(s, m => m.shaderType == MaterialEntry.TypeHairTint && m.AlphaTest && !string.IsNullOrEmpty(m.Texture(1)));
        public static string Describe(MaterialEntry m) =>
            m.shaderType == MaterialEntry.TypeFaceGen ? "FaceGen head" : $"from {m.sourceFile}";
    }

    // An assigned texture asset is the one a nif texture path names (relative to the textures root).
    static bool SameTexture(string assetPath, string nifPath)
    {
        if (string.IsNullOrEmpty(assetPath) || string.IsNullOrEmpty(nifPath)) return false;
        string rel = nifPath.Replace('\\', '/').ToLowerInvariant();
        if (rel.StartsWith("textures/")) rel = rel.Substring("textures/".Length);
        return assetPath.ToLowerInvariant().EndsWith("/" + rel);
    }

    // At UV seams (vertices sharing a position with different UVs) the two sides sample different
    // texels. Skyrim's own _msn is not perfectly continuous there, and the game shows that; the bake
    // must not add discontinuity of its own beyond 8-bit rounding.
    static void SeamFidelity(string shape, Mesh mesh, int slot, MaterialEntry entry, Color32[] src, Color32[] baked, int w, int h)
    {
        var V = mesh.vertices; var N = mesh.normals; var T = mesh.tangents; var UV = mesh.uv;
        var byPos = new Dictionary<Vector3Int, List<int>>();
        foreach (int i in new HashSet<int>(mesh.GetTriangles(slot)))
        {
            var k = Vector3Int.RoundToInt(V[i] * 1e5f);
            if (!byPos.TryGetValue(k, out var l)) byPos[k] = l = new List<int>();
            l.Add(i);
        }
        Vector3 Shaded(int k)
        {
            var ts = NifMergeNormalBaker.Decode(Bilinear(baked, w, h, UV[k]));
            Vector3 t = ((Vector3)T[k] - N[k] * Vector3.Dot(N[k], T[k])).normalized;
            Vector3 bt = Vector3.Cross(N[k], t) * (T[k].w < 0 ? -1f : 1f);
            return (ts.x * t + ts.y * bt + ts.z * N[k]).normalized;
        }
        var srcAng = new List<float>();
        var bakAng = new List<float>();
        foreach (var l in byPos.Values)
            for (int a = 0; a < l.Count; a++)
                for (int b = a + 1; b < l.Count; b++)
                {
                    int i = l[a], j = l[b];
                    if ((UV[i] - UV[j]).sqrMagnitude < 1e-10f) continue;
                    srcAng.Add(Vector3.Angle(NifMergeNormalBaker.MeshNormal(Bilinear(src, w, h, UV[i]), entry.msnToMesh),
                                             NifMergeNormalBaker.MeshNormal(Bilinear(src, w, h, UV[j]), entry.msnToMesh)));
                    bakAng.Add(Vector3.Angle(Shaded(i), Shaded(j)));
                }
        if (srcAng.Count < 10)
        {
            Lines.Add($"INFO {shape}: {srcAng.Count} UV-seam vertex pairs, seam fidelity not measured");
            return;
        }
        srcAng.Sort();
        bakAng.Sort();
        float P(List<float> v, float q) => v[Mathf.Min(v.Count - 1, (int)(v.Count * q))];
        Check(P(bakAng, 0.5f) <= P(srcAng, 0.5f) + 0.75f && P(bakAng, 0.9f) <= P(srcAng, 0.9f) + 1.5f,
              $"{shape}: bake adds no seams ({srcAng.Count} pairs; source median/p90 {P(srcAng, 0.5f):F2}/{P(srcAng, 0.9f):F2} deg, " +
              $"baked {P(bakAng, 0.5f):F2}/{P(bakAng, 0.9f):F2} deg)");
    }

    static Color32 Bilinear(Color32[] px, int w, int h, Vector2 uv)
    {
        float fx = uv.x * w - 0.5f, fy = uv.y * h - 0.5f;
        int x0 = Mathf.FloorToInt(fx), y0 = Mathf.FloorToInt(fy);
        float tx = fx - x0, ty = fy - y0;
        Vector4 acc = Vector4.zero;
        for (int dy = 0; dy < 2; dy++)
            for (int dx = 0; dx < 2; dx++)
            {
                var c = px[Mathf.Clamp(y0 + dy, 0, h - 1) * w + Mathf.Clamp(x0 + dx, 0, w - 1)];
                acc += new Vector4(c.r, c.g, c.b, c.a) * ((dx == 0 ? 1 - tx : tx) * (dy == 0 ? 1 - ty : ty));
            }
        return new Color32((byte)Mathf.RoundToInt(acc.x), (byte)Mathf.RoundToInt(acc.y), (byte)Mathf.RoundToInt(acc.z), 255);
    }

    static bool Barycentric(Vector2 a, Vector2 b, Vector2 c, Vector2 p, out float l0, out float l1, out float l2)
    {
        float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        l0 = l1 = l2 = 0;
        if (Mathf.Abs(area) < 1e-14f) return false;
        l0 = ((c.x - b.x) * (p.y - b.y) - (c.y - b.y) * (p.x - b.x)) / area;
        l1 = ((a.x - c.x) * (p.y - c.y) - (a.y - c.y) * (p.x - c.x)) / area;
        l2 = 1f - l0 - l1;
        return l0 >= 0 && l1 >= 0 && l2 >= 0;
    }

    static string TexPath(Material m, string prop)
    {
        var t = m ? m.GetTexture(prop) : null;
        return t ? AssetDatabase.GetAssetPath(t) : "";
    }

    static Vector3 SubmeshCentroid(Mesh mesh, int sub)
    {
        var v = mesh.vertices;
        var idx = mesh.GetIndices(sub);
        Vector3 c = Vector3.zero;
        foreach (int i in idx) c += v[i];
        return idx.Length > 0 ? c / idx.Length : c;
    }

    // Renders the current pose. The skinned mesh is baked into a static snapshot first: edit-mode
    // batch rendering does not re-skin a SkinnedMeshRenderer when its bones move.
    static void Render(GameObject go, string path)
    {
        EditorSettings.asyncShaderCompilation = false;
        var smr = go.GetComponentInChildren<SkinnedMeshRenderer>();
        var snapshot = new Mesh();
        smr.BakeMesh(snapshot);
        var snapGo = new GameObject("HarnessSnapshot");
        snapGo.transform.SetPositionAndRotation(smr.transform.position, smr.transform.rotation);
        snapGo.AddComponent<MeshFilter>().sharedMesh = snapshot;
        snapGo.AddComponent<MeshRenderer>().sharedMaterials = smr.sharedMaterials;
        smr.enabled = false;
        var camGo = new GameObject("HarnessCamera");
        var lightGo = new GameObject("HarnessLight");
        var rt = new RenderTexture(768, 1024, 24, RenderTextureFormat.ARGB32);
        try
        {
            var light = lightGo.AddComponent<Light>();
            light.type = LightType.Directional;
            light.intensity = 1.1f;
            lightGo.transform.rotation = Quaternion.Euler(30f, 200f, 0f);
            RenderSettings.ambientLight = new Color(0.45f, 0.45f, 0.5f);

            var cam = camGo.AddComponent<Camera>();
            cam.clearFlags = CameraClearFlags.SolidColor;
            cam.backgroundColor = new Color(0.22f, 0.23f, 0.26f);
            cam.fieldOfView = 30f;
            cam.transform.position = new Vector3(0f, 0.95f, 4.6f); // in front: the character faces +Z
            cam.transform.LookAt(new Vector3(0f, 0.92f, 0f));
            cam.targetTexture = rt;
            cam.Render();

            var prev = RenderTexture.active;
            RenderTexture.active = rt;
            var tex = new Texture2D(rt.width, rt.height, TextureFormat.RGB24, false);
            tex.ReadPixels(new Rect(0, 0, rt.width, rt.height), 0, 0);
            tex.Apply();
            RenderTexture.active = prev;
            File.WriteAllBytes(path, tex.EncodeToPNG());
            UnityEngine.Object.DestroyImmediate(tex);
            Lines.Add("INFO screenshot " + Path.GetFullPath(path));
        }
        finally
        {
            smr.enabled = true;
            UnityEngine.Object.DestroyImmediate(snapGo);
            UnityEngine.Object.DestroyImmediate(snapshot);
            UnityEngine.Object.DestroyImmediate(camGo);
            UnityEngine.Object.DestroyImmediate(lightGo);
            rt.Release();
            UnityEngine.Object.DestroyImmediate(rt);
        }
    }
}
