using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;

namespace NifMerge
{
    // Finds SkyAnim and SkyCreature animation FBX files (an FBX with a SkyAnim .anim.json beside it) and
    // configures them: the rig the sidecar names (a creature's), else Humanoid (default) or Generic per the
    // project setting; the clip named after the source clip, its loop flag, Skyrim's annotation events as
    // animation events, and root motion (applied by the Animator) or in-place playback.
    public class SkyAnimImporter : AssetPostprocessor
    {
        const string StampPrefix = "skyanim:";
        const string ImporterVersion = "7";
        // Every Skyrim annotation (FootLeft, arrowRelease, SoundPlay.WPNBowFire, ...) calls this method with
        // the annotation text as its string parameter.
        public const string EventFunction = "OnSkyrimEvent";

        static void OnPostprocessAllAssets(string[] imported, string[] deleted, string[] moved, string[] movedFrom)
        {
            var clips = new HashSet<string>();
            foreach (string path in Concat(imported, moved))
            {
                string p = path.Replace('\\', '/');
                string lower = p.ToLowerInvariant();
                if (lower.EndsWith(".fbx")) clips.Add(p);
                else if (lower.EndsWith(".anim.json"))
                {
                    string fbx = p.Substring(0, p.Length - ".anim.json".Length) + ".fbx";
                    if (File.Exists(fbx)) clips.Add(fbx);
                }
            }
            foreach (string fbx in clips) Configure(fbx, false);
        }

        static IEnumerable<string> Concat(string[] a, string[] b)
        {
            foreach (var s in a) yield return s;
            foreach (var s in b) yield return s;
        }

        // Re-applies the current settings to every SkyAnim clip in the project.
        [MenuItem("Tools/NifMerge/Reconfigure All SkyAnim Clips")]
        public static void ReconfigureAll()
        {
            foreach (string guid in AssetDatabase.FindAssets("t:Model"))
            {
                string p = AssetDatabase.GUIDToAssetPath(guid);
                if (p.ToLowerInvariant().EndsWith(".fbx")) Configure(p, false);
            }
        }

        // The sidecar's rest pose in Unity's local space, by bone name.
        public static Dictionary<string, (Vector3 position, Quaternion rotation)> RestPose(AnimSidecar sidecar)
        {
            var rest = new Dictionary<string, (Vector3, Quaternion)>();
            foreach (var e in sidecar.rest ?? new RestEntry[0]) rest[e.bone] = (e.UnityPosition, e.UnityRotation);
            return rest;
        }

        // The clip's rig: the sidecar's when it names one (a creature's clips, or a skeleton that cannot be
        // Humanoid), else the project setting.
        public static NifMergeSettings.AnimationRig RigOf(AnimSidecar sidecar)
        {
            if (sidecar.rig == "humanoid") return NifMergeSettings.AnimationRig.Humanoid;
            if (sidecar.rig == "generic") return NifMergeSettings.AnimationRig.Generic;
            return NifMergeSettings.instance.animationRig;
        }

        // Root motion. Skyrim moves the whole actor by the clip's reference frame, and the FBX carries that motion
        // on its top-level bones. Unity moves the GameObject by root motion but would also play those bones'
        // curves, moving the creature twice as far; so each top-level bone's curves are rewritten without the
        // motion (the sidecar has it per frame), and a Generic clip gets the motion as its own root-motion curves
        // (MotionT / MotionQ, model space; a Humanoid clip's root motion is Unity's, from the body). With
        // Animator.applyRootMotion the creature moves by it; without, the clip plays in place.
        void OnPostprocessAnimation(GameObject root, AnimationClip clip)
        {
            var sc = AnimSidecar.TryLoad(assetPath, out _);
            if (sc == null || !sc.rootMotion) return;
            if (sc.motion == null || sc.motion.Length != sc.frames)
            {
                Debug.LogWarning($"SkyAnim: {assetPath}: the sidecar's root motion has {sc.motion?.Length ?? 0} frames for {sc.frames}; the clip moves its bones");
                return;
            }
            var motion = new Matrix4x4[sc.frames];
            for (int f = 0; f < sc.frames; f++) motion[f] = Matrix4x4.TRS(sc.motion[f].UnityPosition, sc.motion[f].UnityRotation, Vector3.one);
            float TimeOf(int f) => Mathf.Min(f / sc.fps, clip.length);

            // Top-level bones: their local transform is in model space, where the motion is.
            var top = new HashSet<string>();
            foreach (var b in AnimationUtility.GetCurveBindings(clip))
                if (b.type == typeof(Transform) && b.path.Length > 0 && b.path.IndexOf('/') < 0) top.Add(b.path);
            foreach (string path in top)
            {
                var bone = root.transform.Find(path);
                var bindings = AnimationUtility.GetCurveBindings(clip).Where(b => b.type == typeof(Transform) && b.path == path).ToArray();
                AnimationCurve Curve(string property) =>
                    bindings.Where(b => b.propertyName == property).Select(b => AnimationUtility.GetEditorCurve(clip, b)).FirstOrDefault();
                AnimationCurve[] pos = { Curve("m_LocalPosition.x"), Curve("m_LocalPosition.y"), Curve("m_LocalPosition.z") };
                AnimationCurve[] quat = { Curve("m_LocalRotation.x"), Curve("m_LocalRotation.y"), Curve("m_LocalRotation.z"), Curve("m_LocalRotation.w") };
                AnimationCurve[] euler = { Curve("localEulerAnglesRaw.x"), Curve("localEulerAnglesRaw.y"), Curve("localEulerAnglesRaw.z") };
                var fresh = new AnimationCurve[7];
                for (int k = 0; k < 7; k++) fresh[k] = new AnimationCurve();
                Quaternion previous = Quaternion.identity;
                for (int f = 0; f < sc.frames; f++)
                {
                    float t = TimeOf(f);
                    Vector3 p = bone.localPosition;
                    for (int k = 0; k < 3; k++)
                        if (pos[k] != null) p[k] = pos[k].Evaluate(t);
                    Quaternion q = bone.localRotation;
                    if (quat.All(c => c != null))
                        q = new Quaternion(quat[0].Evaluate(t), quat[1].Evaluate(t), quat[2].Evaluate(t), quat[3].Evaluate(t)).normalized;
                    else if (euler.All(c => c != null))
                        q = Quaternion.Euler(euler[0].Evaluate(t), euler[1].Evaluate(t), euler[2].Evaluate(t));
                    Matrix4x4 still = motion[f].inverse * Matrix4x4.TRS(p, q, Vector3.one);
                    AddKey(fresh, t, still.GetColumn(3), still.rotation, ref previous);
                }
                foreach (var b in bindings)
                    if (b.propertyName.StartsWith("m_LocalPosition") || b.propertyName.StartsWith("m_LocalRotation") ||
                        b.propertyName.StartsWith("localEulerAngles"))
                        AnimationUtility.SetEditorCurve(clip, b, null);
                string[] props = { "localPosition.x", "localPosition.y", "localPosition.z", "localRotation.x", "localRotation.y", "localRotation.z", "localRotation.w" };
                for (int k = 0; k < 7; k++) clip.SetCurve(path, typeof(Transform), props[k], Linear(fresh[k]));
            }

            if (RigOf(sc) == NifMergeSettings.AnimationRig.Generic)
            {
                var curves = new AnimationCurve[7];
                for (int k = 0; k < 7; k++) curves[k] = new AnimationCurve();
                Quaternion previous = Quaternion.identity;
                for (int f = 0; f < sc.frames; f++) AddKey(curves, TimeOf(f), motion[f].GetColumn(3), motion[f].rotation, ref previous);
                string[] names = { "MotionT.x", "MotionT.y", "MotionT.z", "MotionQ.x", "MotionQ.y", "MotionQ.z", "MotionQ.w" };
                for (int k = 0; k < 7; k++) clip.SetCurve("", typeof(Animator), names[k], Linear(curves[k]));
            }
        }

        // One key per curve: position x, y, z, then a quaternion kept on the same hemisphere as the last.
        static void AddKey(AnimationCurve[] c, float t, Vector3 p, Quaternion q, ref Quaternion previous)
        {
            if (Quaternion.Dot(previous, q) < 0) q = new Quaternion(-q.x, -q.y, -q.z, -q.w);
            previous = q;
            float[] v = { p.x, p.y, p.z, q.x, q.y, q.z, q.w };
            for (int k = 0; k < 7; k++) c[k].AddKey(new Keyframe(t, v[k]));
        }

        static AnimationCurve Linear(AnimationCurve c)
        {
            for (int i = 0; i < c.length; i++)
            {
                AnimationUtility.SetKeyLeftTangentMode(c, i, AnimationUtility.TangentMode.Linear);
                AnimationUtility.SetKeyRightTangentMode(c, i, AnimationUtility.TangentMode.Linear);
            }
            return c;
        }

        // Returns true when the FBX is SkyAnim output.
        public static bool Configure(string fbxPath, bool force)
        {
            var sidecar = AnimSidecar.TryLoad(fbxPath, out string sidecarText);
            if (sidecar == null) return false;
            var importer = AssetImporter.GetAtPath(fbxPath) as ModelImporter;
            if (importer == null) return false;

            var rig = RigOf(sidecar);
            string stamp = StampPrefix + ImporterVersion + ":" + rig + ":" +
                           Hash128.Compute(sidecarText) + ":" + Hash128.Compute(File.ReadAllBytes(fbxPath));
            if (!force && importer.userData == stamp) return true;

            var warnings = new List<string>();
            importer.useFileScale = true;
            importer.globalScale = 1f;
            importer.materialImportMode = ModelImporterMaterialImportMode.None;
            importer.importAnimation = true;
            importer.optimizeGameObjects = false;
            // Keyframe reduction errs in proportion to a curve's values. On a root-motion clip the top-level bones
            // carry the whole motion (29 m for the dragon's takeoff), so reduced curves would leave centimetres of
            // drift once the exact motion is taken back out (OnPostprocessAnimation): such clips keep every key.
            importer.animationCompression = sidecar.rootMotion ? ModelImporterAnimationCompression.Off
                                                               : ModelImporterAnimationCompression.KeyframeReduction;
            if (rig == NifMergeSettings.AnimationRig.Humanoid)
            {
                importer.humanDescription = NifMergeHumanoid.Build(fbxPath, sidecar.humanoid, warnings, RestPose(sidecar));
                importer.animationType = ModelImporterAnimationType.Human;
                importer.avatarSetup = ModelImporterAvatarSetup.CreateFromThisModel;
            }
            else
            {
                importer.animationType = ModelImporterAnimationType.Generic;
                importer.avatarSetup = ModelImporterAvatarSetup.NoAvatar; // root motion: OnPostprocessAnimation
                importer.motionNodeName = "";
            }
            // Unity resets clip settings when the animation type changes, so the rig is imported first and
            // the clip configured on top of it. The stamp is set first so the nested postprocess skips.
            importer.clipAnimations = new ModelImporterClipAnimation[0];
            importer.userData = stamp;
            importer.SaveAndReimport();
            importer = (ModelImporter)AssetImporter.GetAtPath(fbxPath);

            var takes = importer.defaultClipAnimations;
            if (takes.Length == 0)
            {
                warnings.Add("the FBX holds no animation take");
            }
            else
            {
                var clip = takes[0];
                clip.name = sidecar.clip;
                clip.loopTime = sidecar.loop;
                clip.loopPose = false; // the clip's own last frame already matches its first
                // A clip with root motion lets the Animator move the creature by it; any other clip plays in
                // place, its root kept where the clip has it.
                bool inPlace = !sidecar.rootMotion;
                clip.lockRootRotation = inPlace;
                clip.keepOriginalOrientation = true;
                clip.lockRootHeightY = inPlace;
                clip.keepOriginalPositionY = true;
                clip.lockRootPositionXZ = inPlace;
                clip.keepOriginalPositionXZ = true;
                var events = new List<AnimationEvent>();
                foreach (var e in sidecar.events ?? new AnimEventEntry[0])
                    events.Add(new AnimationEvent
                    {
                        time = sidecar.duration > 0 ? Mathf.Clamp01(e.time / sidecar.duration) : 0, // normalised
                        functionName = EventFunction,
                        stringParameter = e.name
                    });
                clip.events = events.ToArray();
                if (rig == NifMergeSettings.AnimationRig.Humanoid)
                {
                    // A Humanoid clip keeps only muscle curves unless its mask names other transforms; Skyrim
                    // skins forearms to twist bones and drives weapon, physics and helper nodes, so every
                    // transform stays in.
                    var mask = new AvatarMask();
                    for (int i = 0; i < (int)AvatarMaskBodyPart.LastBodyPart; i++) mask.SetHumanoidBodyPartActive((AvatarMaskBodyPart)i, true);
                    mask.AddTransformPath(AssetDatabase.LoadAssetAtPath<GameObject>(fbxPath).transform, true);
                    clip.maskType = ClipAnimationMaskType.CreateFromThisModel;
                    clip.ConfigureClipFromMask(mask);
                    Object.DestroyImmediate(mask);
                }
                importer.clipAnimations = new[] { clip };
            }
            if (sidecar.droppedTracks != null && sidecar.droppedTracks.Length > 0)
                warnings.Add($"{sidecar.droppedTracks.Length} tracks had no bone in the Havok skeleton and were left out");
            if (sidecar.unverifiedFormats != null)
                foreach (var f in sidecar.unverifiedFormats) warnings.Add($"uses {f}, which no real sample has verified yet");

            importer.userData = stamp;
            importer.SaveAndReimport();
            if (warnings.Count == 0) Debug.Log($"SkyAnim: configured {fbxPath} ({rig})");
            else Debug.LogWarning($"SkyAnim: configured {fbxPath} ({rig}) with {warnings.Count} warning(s):\n  " + string.Join("\n  ", warnings));
            return true;
        }
    }
}
