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
using UnityEngine.Animations;
using UnityEngine.Playables;

public static class NifMergeHarness
{
    const string Fbx = "Assets/NifMergeTest/Character.fbx";
    const string OutfitFbx = "Assets/NifMergeTest/Outfit.fbx"; // the same character wearing the sample outfit
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
            CheckOutfit(outDir);
            CheckAnimations(outDir);
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
                string want = ExpectedShader(sidecar.materials[i]);
                Check(mats[i] && mats[i].shader.name == want, $"slot {i} uses the {want} shader (got '{(mats[i] ? mats[i].shader.name : "")}')");
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
            CheckSkinSpecular(mats[bodySlot], sidecar.materials[bodySlot], "body");
            CheckSkinSpecular(mats[headSlot], sidecar.materials[headSlot], "head");
            // Both colour spaces: the scratch project starts in Gamma, users' projects are often Linear.
            var colorSpace = PlayerSettings.colorSpace;
            foreach (var cs in new[] { ColorSpace.Gamma, ColorSpace.Linear })
            {
                PlayerSettings.colorSpace = cs;
                MeasureSkinSpecular(mats[bodySlot], sidecar.materials[bodySlot]);
            }
            PlayerSettings.colorSpace = colorSpace;

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
            CheckFacialMorphs(go, smr, sidecar, outDir);

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

    // The character wearing the sample outfit (dress, boots, thong), imported automatically like any model:
    // configured as a Humanoid, the replaced body parts absent, every material built, the dress's
    // body copy baked from its model-space map, and the items' tangent-space maps converted.
    static void CheckOutfit(string outDir)
    {
        var sidecar = Sidecar.TryLoad(OutfitFbx, out _);
        var importer = AssetImporter.GetAtPath(OutfitFbx) as ModelImporter;
        Check(sidecar != null && importer != null, "outfit: FBX imported with its sidecar");
        if (sidecar == null || importer == null) return;
        Check(importer.userData != null && importer.userData.StartsWith("nifmerge:") &&
              importer.animationType == ModelImporterAnimationType.Human, "outfit: configured as a Humanoid");
        var avatar = AssetDatabase.LoadAllAssetsAtPath(OutfitFbx).OfType<Avatar>().FirstOrDefault();
        Check(avatar != null && avatar.isValid && avatar.isHuman, "outfit: avatar is valid and human");
        string reportPath = NifMergeImporter.ReportPathFor(OutfitFbx);
        var warnings = File.Exists(reportPath) ? File.ReadAllLines(reportPath).ToList() : new List<string>();
        Lines.Add($"INFO outfit importer warnings ({warnings.Count}):");
        foreach (var w in warnings) Lines.Add("INFO   " + w);

        // What the outfit replaced, as the sidecar records it, is absent from the model.
        var removed = sidecar.parts.Where(p => p.role == "character").SelectMany(p => p.removed).ToList();
        Check(removed.Any(r => r.slot == 32 && r.wholeShape) && removed.Any(r => r.slot == 37 && r.wholeShape),
              $"outfit: the sidecar records the body (32) and feet (37) replaced ({removed.Count} removals)");
        foreach (var r in removed.Where(r => r.wholeShape))
            Check(!sidecar.materials.Any(m => m.role == "character" && m.shape == r.shape),
                  $"outfit: character '{r.shape}' replaced by {r.by} is not in the model");

        var prefab = AssetDatabase.LoadAssetAtPath<GameObject>(OutfitFbx);
        var go = UnityEngine.Object.Instantiate(prefab);
        go.transform.SetPositionAndRotation(Vector3.zero, Quaternion.identity);
        try
        {
            var animator = go.GetComponent<Animator>();
            if (animator) animator.enabled = false;
            var smr = go.GetComponentInChildren<SkinnedMeshRenderer>();
            Check(smr != null, "outfit: SkinnedMeshRenderer present");
            if (smr == null) return;
            var mats = smr.sharedMaterials;
            Check(mats.Length == sidecar.materials.Length && smr.sharedMesh.subMeshCount == mats.Length,
                  $"outfit: {mats.Length} material slots == {sidecar.materials.Length}");
            for (int i = 0; i < Math.Min(mats.Length, sidecar.materials.Length); i++)
            {
                var e = sidecar.materials[i];
                string path = mats[i] ? AssetDatabase.GetAssetPath(mats[i]) : "";
                string wantShader = ExpectedShader(e);
                Check(path == $"Assets/NifMergeTest/Outfit_Materials/{e.name}.mat" && mats[i].shader.name == wantShader,
                      $"outfit: slot {i} ({e.shape}, {e.role}) uses a {wantShader} {e.name}.mat (got '{path}', {(mats[i] ? mats[i].shader.name : "")})");
                if (e.role != "outfit" || e.modelSpaceNormals) continue;
                // An item: diffuse from the textures root, tangent-space map converted, alpha mode from NiAlphaProperty.
                Check(SameTexture(TexPath(mats[i], "_MainTex"), e.Texture(0)), $"outfit: {e.shape} diffuse assigned");
                string n = TexPath(mats[i], "_BumpMap");
                Check(n.EndsWith($"{Path.GetFileNameWithoutExtension(e.Texture(1).Replace('\\', '/'))}_unity_n.png"),
                      $"outfit: {e.shape} normal map converted (got '{n}')");
                float want = e.AlphaTest ? 1 : e.AlphaBlend ? 2 : 0;
                Check(mats[i].GetFloat("_Mode") == want, $"outfit: {e.shape} rendering mode {mats[i].GetFloat("_Mode")}, expected {want}");
            }
            int body = Roles.Body(sidecar);
            Check(body >= 0 && sidecar.materials[body].role == "outfit", "outfit: the body is the dress's own body copy");
            if (body >= 0) CheckSkinSpecular(mats[body], sidecar.materials[body], "outfit: dress's body copy");
            CheckModelSpaceNormals(smr, sidecar, mats, warnings);

            var baked = new Mesh();
            smr.BakeMesh(baked);
            var a = baked.vertices;
            var b = smr.sharedMesh.vertices;
            float worst = 0;
            for (int i = 0; i < a.Length; i++) worst = Mathf.Max(worst, (a[i] - b[i]).magnitude);
            Check(worst < 1e-4f, $"outfit: bind pose skins to the rest mesh (max error {worst:G3})");
            UnityEngine.Object.DestroyImmediate(baked);
            Render(go, Path.Combine(outDir, "outfit_bind_pose.png"));
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
            // Standard samples the map only with _NORMALMAP; the skin shader always samples it.
            bool sampled = mats[slot].shader.name != "Standard" || mats[slot].IsKeywordEnabled("_NORMALMAP");
            Check(path.EndsWith("_unity_ts_n.png") && sampled, $"{shape}: model-space map baked to tangent space (got '{path}')");
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
        // The body is the skin-tinted slot in body slot 32: the character's own, or an outfit's body copy.
        public static int Body(Sidecar s) =>
            Find(s, m => m.shaderType == MaterialEntry.TypeSkinTint && m.slots != null && Array.IndexOf(m.slots, 32) >= 0);
        public static int Head(Sidecar s) => Find(s, m => m.shaderType == MaterialEntry.TypeFaceGen);
        public static int Hair(Sidecar s) =>
            Find(s, m => m.shaderType == MaterialEntry.TypeHairTint && m.AlphaTest && !string.IsNullOrEmpty(m.Texture(1)));
        public static string Describe(MaterialEntry m) =>
            m.shaderType == MaterialEntry.TypeFaceGen ? "FaceGen head" : $"from {m.sourceFile}";
    }

    // An assigned texture asset is the one a nif texture path names (relative to the textures root).
    // Skin (SkinTint and FaceGen shader types) uses the package's Skyrim Skin shader, the rest Standard.
    static string ExpectedShader(MaterialEntry e)
    {
        bool skin = e.shaderType == MaterialEntry.TypeSkinTint || e.shaderType == MaterialEntry.TypeFaceGen;
        if (!skin) return "Standard";
        return !e.AlphaTest && e.AlphaBlend ? "NifMerge/Skyrim Skin (Fade)" : "NifMerge/Skyrim Skin";
    }

    // A skin slot's specular inputs: the _s map (slot 7) as the mask, imported as data, and Skyrim's
    // specular colour x strength and glossiness carried over unchanged.
    static void CheckSkinSpecular(Material mat, MaterialEntry e, string what)
    {
        foreach (var name in new[] { "NifMerge/Skyrim Skin", "NifMerge/Skyrim Skin (Fade)" })
        {
            var sh = Shader.Find(name);
            Check(sh != null && !ShaderUtil.ShaderHasError(sh), $"{what}: shader '{name}' compiles");
        }
        Check(mat.shader.name == "NifMerge/Skyrim Skin", $"{what}: uses the Skyrim Skin shader");
        Check(e.modelSpaceNormals && !string.IsNullOrEmpty(e.Texture(7)), $"{what}: the sidecar names an _s map in slot 7");
        Check(SameTexture(TexPath(mat, "_SpecMap"), e.Texture(7)), $"{what}: specular mask is the slot-7 _s map (got '{TexPath(mat, "_SpecMap")}')");
        // The import setting itself: a Gamma project never creates sRGB formats, so the texture's format
        // cannot tell. MeasureSkinSpecular checks the sampled value in Linear.
        var mask = mat.GetTexture("_SpecMap") as Texture2D;
        var maskImporter = mask ? AssetImporter.GetAtPath(AssetDatabase.GetAssetPath(mask)) : null;
        bool data = maskImporter is TextureImporter ti ? !ti.sRGBTexture
                  : maskImporter && new SerializedObject(maskImporter).FindProperty("m_sRGBTexture") is SerializedProperty p && !p.boolValue;
        Check(data, $"{what}: specular mask imported without sRGB decoding");
        Check(!mat.IsKeywordEnabled("_SPECMAP_ALPHA"), $"{what}: mask read from the red channel");
        Color want = new Color(e.specularColor[0], e.specularColor[1], e.specularColor[2]) * e.specularStrength;
        Color got = mat.GetColor("_SkyrimSpecColor");
        Check((e.shaderFlags1 & 1) != 0 && Mathf.Abs(got.r - want.r) < 1e-4f && Mathf.Abs(got.g - want.g) < 1e-4f &&
              Mathf.Abs(got.b - want.b) < 1e-4f, $"{what}: specular colour x strength {got} == {want}");
        Check(Mathf.Abs(mat.GetFloat("_Shininess") - e.glossiness) < 1e-4f, $"{what}: glossiness {mat.GetFloat("_Shininess")} == {e.glossiness}");
    }

    // Renders a flat quad with the imported body material, the camera on the quad's normal, and checks the
    // pixel against Skyrim's formula, linear in the light colour Lc:
    //   diffuse  = albedo * N.L * Lc
    //   specular = pow(N.H, glossiness) * specular colour x strength * mask * Lc
    // Each light angle is rendered with and without specular; the difference is the specular term.
    // Albedo and mask come from the textures' stored texels, so this also checks how each is decoded.
    static void MeasureSkinSpecular(Material imported, MaterialEntry e)
    {
        var mat = new Material(imported);
        mat.SetTexture("_BumpMap", null); // flat normal, so N.L and N.H are exact
        var quad = GameObject.CreatePrimitive(PrimitiveType.Quad); // faces -Z
        quad.GetComponent<MeshRenderer>().sharedMaterial = mat;
        var at = new Vector3(0f, -50f, 0f); // away from the character, which stands at the origin
        quad.transform.position = at;
        var camGo = new GameObject("HarnessSpecCamera");
        var lightGo = new GameObject("HarnessSpecLight");
        var rt = new RenderTexture(16, 16, 24, RenderTextureFormat.ARGBFloat, RenderTextureReadWrite.Linear);
        var saved = (RenderSettings.ambientMode, RenderSettings.ambientLight, RenderSettings.ambientProbe);
        try
        {
            RenderSettings.ambientMode = UnityEngine.Rendering.AmbientMode.Flat;
            RenderSettings.ambientLight = Color.black;
            RenderSettings.ambientProbe = new UnityEngine.Rendering.SphericalHarmonicsL2();
            var light = lightGo.AddComponent<Light>();
            light.type = LightType.Directional;
            light.intensity = 0.25f;
            light.shadows = LightShadows.None;
            var cam = camGo.AddComponent<Camera>();
            cam.clearFlags = CameraClearFlags.SolidColor;
            cam.backgroundColor = Color.black;
            cam.fieldOfView = 10f;
            cam.transform.position = at + new Vector3(0f, 0f, -2f);
            cam.transform.LookAt(at);
            cam.targetTexture = rt;

            bool linear = QualitySettings.activeColorSpace == ColorSpace.Linear;
            string space = linear ? "Linear" : "Gamma";
            var diffuse = NifMergeMaterials.ReadRaw((Texture2D)mat.GetTexture("_MainTex"), out _, out _)[0];
            var maskTexel = NifMergeMaterials.ReadRaw((Texture2D)mat.GetTexture("_SpecMap"), out _, out _)[0];
            float albedo = linear ? Mathf.GammaToLinearSpace(diffuse.g / 255f) : diffuse.g / 255f;
            float mask = maskTexel.r / 255f; // data: never decoded
            Color specColor = mat.GetColor("_SkyrimSpecColor");

            float Pixel(float angle, bool spec)
            {
                // The light travels along +Z rotated by angle about Y; the quad's normal is -Z.
                lightGo.transform.rotation = Quaternion.Euler(0f, angle, 0f);
                mat.SetColor("_SkyrimSpecColor", spec ? specColor : Color.black);
                cam.Render();
                var prev = RenderTexture.active;
                RenderTexture.active = rt;
                var tex = new Texture2D(rt.width, rt.height, TextureFormat.RGBAFloat, false, true);
                tex.ReadPixels(new Rect(0, 0, rt.width, rt.height), 0, 0);
                tex.Apply();
                RenderTexture.active = prev;
                float g = tex.GetPixel(rt.width / 2, rt.height / 2).g;
                UnityEngine.Object.DestroyImmediate(tex);
                return g;
            }

            float d0 = Pixel(0f, false), s0 = Pixel(0f, true) - d0;
            float lc = d0 / albedo; // N.L = 1
            float want0 = specColor.g * mask * lc; // N.H = 1
            Lines.Add($"INFO skin specular ({space}): albedo {albedo:F4}, mask {mask:F4}, Lc {lc:F4}, diffuse {d0:F4}, specular {s0:F4}");
            Check(lc > 0.01f && Mathf.Abs(s0 - want0) <= 0.03f * want0 + 1e-3f,
                  $"skin specular ({space}) at N.H = 1: {s0:F4} == colour x strength x mask x Lc = {want0:F4}");
            const float angle = 40f;
            float d1 = Pixel(angle, false), s1 = Pixel(angle, true) - d1;
            float wantD1 = albedo * lc * Mathf.Cos(angle * Mathf.Deg2Rad);
            float wantS1 = want0 * Mathf.Pow(Mathf.Cos(angle * 0.5f * Mathf.Deg2Rad), e.glossiness);
            Check(Mathf.Abs(d1 - wantD1) <= 0.03f * wantD1 + 1e-3f, $"skin diffuse ({space}) at N.L = cos {angle}: {d1:F4} == {wantD1:F4}");
            Check(Mathf.Abs(s1 - wantS1) <= 0.03f * wantS1 + 1e-3f,
                  $"skin specular ({space}) falls off as pow(N.H, {e.glossiness}): {s1:F4} == {wantS1:F4}");
        }
        finally
        {
            (RenderSettings.ambientMode, RenderSettings.ambientLight, RenderSettings.ambientProbe) = saved;
            UnityEngine.Object.DestroyImmediate(quad);
            UnityEngine.Object.DestroyImmediate(camGo);
            UnityEngine.Object.DestroyImmediate(lightGo);
            UnityEngine.Object.DestroyImmediate(mat);
            rt.Release();
            UnityEngine.Object.DestroyImmediate(rt);
        }
    }

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
    // Facial morphs (.tri) imported as blend shapes: present by name, confined to the head, and moving
    // the way their names say. Face close-ups are rendered for a look.
    static void CheckFacialMorphs(GameObject go, SkinnedMeshRenderer smr, Sidecar sidecar, string outDir)
    {
        var mesh = smr.sharedMesh;
        var names = sidecar.blendShapes ?? new string[0];
        Check(names.Length > 0, $"sidecar lists {names.Length} blend shapes");
        Check(mesh.blendShapeCount == names.Length, $"{mesh.blendShapeCount} blend shapes imported, sidecar lists {names.Length}");
        foreach (var n in names) Check(mesh.GetBlendShapeIndex(n) >= 0, $"blend shape '{n}' imported");
        if (mesh.blendShapeCount == 0) return;

        // Submesh of each vertex, to tell which parts a morph moves.
        var slotOf = new int[mesh.vertexCount];
        for (int sm = 0; sm < mesh.subMeshCount; sm++)
            foreach (int v in mesh.GetIndices(sm)) slotOf[v] = sm;
        bool IsHeadPart(int slot)
        {
            var e = sidecar.materials[slot];
            return e.sourceFile.ToLowerInvariant() == sidecar.materials[Roles.Head(sidecar)].sourceFile.ToLowerInvariant();
        }

        var rest = mesh.vertices;
        Vector3[] Posed(string morph, float weight)
        {
            for (int i = 0; i < mesh.blendShapeCount; i++) smr.SetBlendShapeWeight(i, 0f);
            if (morph != null) smr.SetBlendShapeWeight(mesh.GetBlendShapeIndex(morph), weight);
            var baked = new Mesh();
            smr.BakeMesh(baked);
            var v = baked.vertices;
            UnityEngine.Object.DestroyImmediate(baked);
            return v;
        }
        var neutral = Posed(null, 0f);
        float worstRest = 0;
        for (int i = 0; i < rest.Length; i++) worstRest = Mathf.Max(worstRest, (neutral[i] - rest[i]).magnitude);
        Check(worstRest < 1e-4f, $"all morphs at 0 leave the mesh at rest (max {worstRest:G3} m)");

        (int vertex, Vector3 delta, bool onlyHead) Peak(string morph)
        {
            var p = Posed(morph, 100f);
            int best = -1;
            float bestLen = 0;
            bool onlyHead = true;
            for (int i = 0; i < p.Length; i++)
            {
                float len = (p[i] - neutral[i]).magnitude;
                if (len > 1e-5f && !IsHeadPart(slotOf[i])) onlyHead = false;
                if (len > bestLen) bestLen = len; else continue;
                best = i;
            }
            return (best, best >= 0 ? p[best] - neutral[best] : Vector3.zero, onlyHead);
        }
        foreach (var (morph, side) in new[] { ("BlinkLeft", -1f), ("BlinkRight", 1f) })
        {
            if (mesh.GetBlendShapeIndex(morph) < 0) continue;
            var (v, d, onlyHead) = Peak(morph);
            Check(v >= 0 && d.magnitude > 0.002f, $"{morph} moves the mesh (peak {d.magnitude * 1000:F1} mm)");
            if (v < 0) continue;
            // Unity: the character faces +Z, so its left is -X.
            Check(neutral[v].x * side > 0.005f && d.y < 0f, $"{morph}: peak on the character's {(side < 0 ? "left" : "right")} and moving down " +
                  $"(x {neutral[v].x:F3}, dy {d.y * 1000:F1} mm)");
            Check(onlyHead, $"{morph} moves only head parts");
        }
        if (mesh.GetBlendShapeIndex("Aah") >= 0)
        {
            var (v, d, onlyHead) = Peak("Aah");
            var eyes = Array.FindIndex(sidecar.materials, mm => mm.shape.Contains("Eyes"));
            Vector3 eyeCentre = SubmeshCentroid(mesh, eyes);
            Check(v >= 0 && d.y < -0.003f && neutral[v].y < eyeCentre.y - 0.03f,
                  $"Aah opens the mouth: peak below the eyes moving down ({d.y * 1000:F1} mm)");
            Check(onlyHead, "Aah moves only head parts");
        }

        RenderFace(go, smr, Path.Combine(outDir, "face_neutral.png"), null);
        RenderFace(go, smr, Path.Combine(outDir, "face_aah.png"), new[] { "Aah" });
        RenderFace(go, smr, Path.Combine(outDir, "face_blink.png"), new[] { "BlinkLeft", "BlinkRight" });
        RenderFace(go, smr, Path.Combine(outDir, "face_happy.png"), new[] { "MoodHappy" });
        RenderFace(go, smr, Path.Combine(outDir, "face_bigaah.png"), new[] { "BigAah" });
        RenderFace(go, smr, Path.Combine(outDir, "face_oh.png"), new[] { "Oh" });
        RenderFace(go, smr, Path.Combine(outDir, "face_oohq.png"), new[] { "OohQ" });
        for (int i = 0; i < mesh.blendShapeCount; i++) smr.SetBlendShapeWeight(i, 0f);
    }

    // ---- SkyAnim clips (Assets/NifMergeTest/Anim, Humanoid by default; AnimGeneric reconfigured as Generic).
    const string AnimDir = "Assets/NifMergeTest/Anim/";
    const string GenericDir = "Assets/NifMergeTest/AnimGeneric/";

    // Plays a clip on a model's Animator through a PlayableGraph, the way the runtime does (an Animator with a
    // Humanoid avatar retargets Humanoid clips and applies Generic clips' transform curves directly).
    sealed class ClipPlayer : IDisposable
    {
        PlayableGraph graph;
        AnimationClipPlayable playable;

        public ClipPlayer(GameObject go, AnimationClip clip)
        {
            var animator = go.GetComponent<Animator>();
            if (animator == null) animator = go.AddComponent<Animator>(); // Unity objects compare to null with ==, not ??
            animator.applyRootMotion = false;
            animator.cullingMode = AnimatorCullingMode.AlwaysAnimate; // no camera sees the model in batch mode
            graph = PlayableGraph.Create("HarnessClip");
            graph.SetTimeUpdateMode(DirectorUpdateMode.Manual);
            playable = AnimationClipPlayable.Create(graph, clip);
            playable.SetApplyFootIK(false);
            AnimationPlayableOutput.Create(graph, "pose", animator).SetSourcePlayable(playable);
        }

        public void Sample(float t)
        {
            playable.SetTime(t);
            graph.Evaluate();
        }

        public void Dispose() => graph.Destroy();
    }

    static AnimationClip ClipOf(string fbx) =>
        AssetDatabase.LoadAllAssetsAtPath(fbx).OfType<AnimationClip>().FirstOrDefault(c => !c.name.StartsWith("__preview__"));

    static Dictionary<string, Transform> ByName(GameObject go)
    {
        var d = new Dictionary<string, Transform>();
        foreach (var t in go.GetComponentsInChildren<Transform>(true))
            if (!d.ContainsKey(t.name)) d.Add(t.name, t);
        return d;
    }

    static void CheckAnimations(string outDir)
    {
        // Automatic Humanoid import of every clip.
        foreach (string name in new[] { "mt_walkforward", "mt_runforward", "mt_idle", "bow_release" })
        {
            string fbx = AnimDir + name + ".fbx";
            var sc = AnimSidecar.TryLoad(fbx, out _);
            var imp = AssetImporter.GetAtPath(fbx) as ModelImporter;
            Check(sc != null && imp != null, $"{name}: SkyAnim clip and sidecar imported");
            if (sc == null || imp == null) continue;
            Check(imp.userData != null && imp.userData.StartsWith("skyanim:"), $"{name}: importer stamped by SkyAnim");
            Check(imp.animationType == ModelImporterAnimationType.Human, $"{name}: imported as Humanoid");
            var avatar = AssetDatabase.LoadAllAssetsAtPath(fbx).OfType<Avatar>().FirstOrDefault();
            Check(avatar != null && avatar.isValid && avatar.isHuman, $"{name}: avatar is valid and human");
            var clip = ClipOf(fbx);
            Check(clip != null && clip.name == sc.clip, $"{name}: clip named '{clip?.name}'");
            if (clip == null) continue;
            Check(clip.humanMotion, $"{name}: humanoid clip");
            Check(Mathf.Abs(clip.length - sc.duration) < 0.002f, $"{name}: length {clip.length} s (sidecar {sc.duration})");
            Check(AnimationUtility.GetAnimationClipSettings(clip).loopTime == sc.loop, $"{name}: loop time {sc.loop}");
            var events = AnimationUtility.GetAnimationEvents(clip);
            bool eventsOk = events.Length == sc.events.Length;
            for (int i = 0; eventsOk && i < events.Length; i++)
                eventsOk = Mathf.Abs(events[i].time - sc.events[i].time) < 0.002f && events[i].functionName == SkyAnimImporter.EventFunction &&
                           events[i].stringParameter == sc.events[i].name;
            Check(eventsOk, $"{name}: {events.Length} events = " + string.Join(", ", events.Select(e => $"{e.stringParameter}@{e.time:0.000}")));
            var bindings = AnimationUtility.GetCurveBindings(clip);
            Check(bindings.Any(b => b.path.EndsWith("NPC L ForearmTwist1 [LLt1]")), $"{name}: forearm twist bone animated alongside the muscles");
        }
        var walkSc = AnimSidecar.TryLoad(AnimDir + "mt_walkforward.fbx", out _);
        Check(walkSc != null && walkSc.events.Length == 2 && walkSc.events[0].name == "FootRight", "SE walk carries FootRight/FootLeft");
        var bowSc = AnimSidecar.TryLoad(AnimDir + "bow_release.fbx", out _);
        Check(bowSc != null && bowSc.platform == "LE" && bowSc.binding == "by name" && !bowSc.loop, "LE bow binds by name, does not loop");

        // The sidecar's rest pose, converted to Unity space the way the importer does, puts every bone where the
        // NifMerge character's rest pose has it (the clip models themselves arrive in their first frame's pose).
        var restCharacter = UnityEngine.Object.Instantiate(AssetDatabase.LoadAssetAtPath<GameObject>(Fbx));
        try
        {
            var chRest = ByName(restCharacter);
            foreach (string name in new[] { "mt_walkforward", "bow_release" })
            {
                var sc = AnimSidecar.TryLoad(AnimDir + name + ".fbx", out _);
                var model = UnityEngine.Object.Instantiate(AssetDatabase.LoadAssetAtPath<GameObject>(AnimDir + name + ".fbx"));
                try
                {
                    var bones = ByName(model);
                    float before = 0f, after = 0f;
                    foreach (var kv in bones)
                        if (chRest.TryGetValue(kv.Key, out var c) && kv.Key.StartsWith("NPC ")) before = Mathf.Max(before, (c.position - kv.Value.position).magnitude);
                    foreach (var kv in SkyAnimImporter.RestPose(sc))
                    {
                        bones[kv.Key].localPosition = kv.Value.position;
                        bones[kv.Key].localRotation = kv.Value.rotation;
                    }
                    int compared = 0;
                    foreach (var kv in bones)
                        if (kv.Value != model.transform && chRest.TryGetValue(kv.Key, out var c))
                        {
                            after = Mathf.Max(after, (c.position - kv.Value.position).magnitude);
                            compared++;
                        }
                    Check(sc.rest.Length == bones.Count - 1, $"{name}: sidecar rest covers every bone ({sc.rest.Length})");
                    bool humanCovered = sc.humanoid.All(h => chRest.ContainsKey(h.bone) && bones.ContainsKey(h.bone));
                    Check(humanCovered && after < 1e-3f,
                          $"{name}: sidecar rest pose = the character's rest pose ({compared} bones, worst {after:0.00000} m; the model's own default pose was {before:0.000} m off)");
                }
                finally
                {
                    UnityEngine.Object.DestroyImmediate(model);
                }
            }
        }
        finally
        {
            UnityEngine.Object.DestroyImmediate(restCharacter);
        }

        // Generic: reconfigure the copies through the same entry point the settings page uses.
        var settings = NifMergeSettings.instance;
        settings.animationRig = NifMergeSettings.AnimationRig.Generic;
        foreach (string name in new[] { "mt_walkforward", "bow_release" }) SkyAnimImporter.Configure(GenericDir + name + ".fbx", false);
        settings.animationRig = NifMergeSettings.AnimationRig.Humanoid;

        var character = AssetDatabase.LoadAssetAtPath<GameObject>(Fbx);
        foreach (string name in new[] { "mt_walkforward", "bow_release" })
        {
            string fbx = GenericDir + name + ".fbx";
            var imp = AssetImporter.GetAtPath(fbx) as ModelImporter;
            var clip = ClipOf(fbx);
            Check(imp != null && imp.animationType == ModelImporterAnimationType.Generic && clip != null && !clip.humanMotion,
                  $"{name}: Generic copy imported as Generic");
            if (clip == null) continue;
            var sc = AnimSidecar.TryLoad(fbx, out _);

            // Every curve of a bone the character has binds on the character (bone paths match NifMerge's).
            var ch = UnityEngine.Object.Instantiate(character);
            var anim = UnityEngine.Object.Instantiate(AssetDatabase.LoadAssetAtPath<GameObject>(fbx));
            try
            {
                var chBones = ByName(ch);
                var animBones = ByName(anim);
                int bound = 0, missing = 0;
                foreach (var b in AnimationUtility.GetCurveBindings(clip))
                {
                    string bone = b.path.Substring(b.path.LastIndexOf('/') + 1);
                    if (!chBones.ContainsKey(bone)) continue; // the character's skin uses fewer bones
                    if (ch.transform.Find(b.path) != null) bound++;
                    else missing++;
                }
                Check(bound > 0 && missing == 0, $"{name}: {bound} curves bind on the character, {missing} do not");

                // Sampled on the character, every shared bone lands where the clip's own skeleton puts it.
                float worst = 0f, lowestToe = float.MaxValue, headLow = float.MaxValue, headHigh = float.MinValue;
                var humanFrames = new List<Dictionary<string, Vector3>>();
                var ownFrames = new List<Dictionary<string, Vector3>>(); // the clip's own skeleton, Generic
                var times = new List<float>();
                // A Generic clip plays on a Generic rig: an Animator without a Humanoid avatar (one with an avatar
                // drives the human bones through muscles only and ignores their transform curves).
                var chAnimator = ch.GetComponent<Animator>();
                var chAvatar = chAnimator.avatar;
                chAnimator.avatar = null;
                using (var onCharacter = new ClipPlayer(ch, clip))
                using (var onOwn = new ClipPlayer(anim, clip))
                for (int f = 0; f < sc.frames; f += Math.Max(1, sc.frames / 12))
                {
                    float t = f / sc.fps;
                    onCharacter.Sample(t);
                    onOwn.Sample(t);
                    foreach (var kv in animBones)
                        if (kv.Value != anim.transform && chBones.TryGetValue(kv.Key, out var c))
                            worst = Mathf.Max(worst, (c.position - kv.Value.position).magnitude);
                    lowestToe = Mathf.Min(lowestToe, chBones["NPC L Toe0 [LToe]"].position.y, chBones["NPC R Toe0 [RToe]"].position.y);
                    float head = chBones["NPC Head [Head]"].position.y;
                    headLow = Mathf.Min(headLow, head);
                    headHigh = Mathf.Max(headHigh, head);
                    var snap = new Dictionary<string, Vector3>();
                    foreach (var h in sc.humanoid) snap[h.bone] = chBones[h.bone].position;
                    humanFrames.Add(snap);
                    ownFrames.Add(sc.humanoid.ToDictionary(h => h.bone, h => animBones[h.bone].position));
                    times.Add(t);
                    if (name == "mt_walkforward" && humanFrames.Count <= 3)
                        Render(ch, Path.Combine(outDir, $"walk_generic_{f:00}.png"), new Vector3(4.6f, 0.95f, 0f), new Vector3(0f, 0.92f, 0f), 30f, 768, 1024);
                }
                chAnimator.avatar = chAvatar;
                chAnimator.Rebind();
                Check(worst < 1e-4f, $"{name}: Generic playback on the character matches the clip's skeleton (worst {worst:0.000000} m)");
                Check(headLow > 1.4f && headHigh < 1.9f, $"{name}: head stays at standing height ({headLow:0.00}..{headHigh:0.00} m)");
                if (name == "mt_walkforward")
                    Check(lowestToe > -0.03f && lowestToe < 0.03f, $"{name}: planted toe at ground level ({lowestToe:0.000} m)");

                // The Humanoid clip of the same source, retargeted through the character's avatar.
                var humanClip = ClipOf(AnimDir + name + ".fbx");
                float worstHuman = 0f, hipsTravel = 0f;
                string worstBone = "";
                var hips = chBones["NPC COM [COM ]"];
                Vector3 hips0 = Vector3.zero;
                using (var player = new ClipPlayer(ch, humanClip))
                {
                    for (int i = 0; i < times.Count; i++)
                    {
                        player.Sample(times[i]);
                        if (i == 0) hips0 = hips.position;
                        hipsTravel = Mathf.Max(hipsTravel, Vector3.ProjectOnPlane(hips.position - hips0, Vector3.up).magnitude);
                        foreach (var h in sc.humanoid)
                        {
                            if (h.human.Contains("Proximal") || h.human.Contains("Intermediate") || h.human.Contains("Distal")) continue;
                            float d = (chBones[h.bone].position - humanFrames[i][h.bone]).magnitude;
                            if (d > worstHuman)
                            {
                                worstHuman = d;
                                worstBone = h.bone;
                            }
                        }
                        if (name == "mt_walkforward" && i < 3)
                            Render(ch, Path.Combine(outDir, $"walk_humanoid_{i}.png"), new Vector3(4.6f, 0.95f, 0f), new Vector3(0f, 0.92f, 0f), 30f, 768, 1024);
                    }
                }
                // Unity's import of a Humanoid clip loses a little (keyframe reduction): measured as the clip on its
                // own model against the same clip played Generic there. Retargeting onto the character, whose
                // avatar matches the clip's (both built from the rest pose), may add at most 1 cm to that.
                float worstOwn = 0f;
                var own = UnityEngine.Object.Instantiate(AssetDatabase.LoadAssetAtPath<GameObject>(AnimDir + name + ".fbx"));
                try
                {
                    var ownBones = ByName(own);
                    using (var player = new ClipPlayer(own, humanClip))
                        for (int i = 0; i < times.Count; i++)
                        {
                            player.Sample(times[i]);
                            foreach (var h in sc.humanoid)
                                if (!h.human.Contains("Proximal") && !h.human.Contains("Intermediate") && !h.human.Contains("Distal"))
                                    worstOwn = Mathf.Max(worstOwn, (ownBones[h.bone].position - ownFrames[i][h.bone]).magnitude);
                        }
                }
                finally
                {
                    UnityEngine.Object.DestroyImmediate(own);
                }
                Lines.Add($"INFO {name}: Humanoid vs Generic: on the character up to {worstHuman:0.000} m (at {worstBone}), on its own model {worstOwn:0.000} m");
                Check(worstHuman <= worstOwn + 0.01f,
                      $"{name}: Humanoid retarget onto the character adds at most 1 cm to the import's own loss ({worstHuman:0.000} vs {worstOwn:0.000} m)");
                Check(hipsTravel < 0.15f, $"{name}: plays in place (hips travel {hipsTravel:0.000} m)");
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(ch);
                UnityEngine.Object.DestroyImmediate(anim);
            }
        }
    }

    static void RenderFace(GameObject go, SkinnedMeshRenderer smr, string path, string[] morphs)
    {
        for (int i = 0; i < smr.sharedMesh.blendShapeCount; i++) smr.SetBlendShapeWeight(i, 0f);
        foreach (var m in morphs ?? new string[0])
        {
            int i = smr.sharedMesh.GetBlendShapeIndex(m);
            if (i >= 0) smr.SetBlendShapeWeight(i, 100f);
        }
        var head = go.GetComponent<Animator>().GetBoneTransform(HumanBodyBones.Head).position + new Vector3(0f, 0.02f, 0f);
        Render(go, path, head + new Vector3(0f, 0f, 0.6f), head, 24f, 768, 768);
    }

    static void Render(GameObject go, string path)
    {
        Render(go, path, new Vector3(0f, 0.95f, 4.6f), new Vector3(0f, 0.92f, 0f), 30f, 768, 1024);
    }

    static void Render(GameObject go, string path, Vector3 camPos, Vector3 target, float fov, int width, int height)
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
        var rt = new RenderTexture(width, height, 24, RenderTextureFormat.ARGB32);
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
            cam.fieldOfView = fov;
            cam.transform.position = camPos; // in front: the character faces +Z
            cam.transform.LookAt(target);
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
