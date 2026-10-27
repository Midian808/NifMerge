using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;

namespace NifMerge
{
    // Finds SkyAnim animation FBX files (an FBX with a SkyAnim .anim.json beside it) and configures them:
    // Humanoid (default) or Generic per the project setting, the clip named after the source clip, its
    // loop flag, Skyrim's annotation events as animation events, and in-place root handling.
    public class SkyAnimImporter : AssetPostprocessor
    {
        const string StampPrefix = "skyanim:";
        const string ImporterVersion = "4";
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

        // Returns true when the FBX is SkyAnim output.
        public static bool Configure(string fbxPath, bool force)
        {
            var sidecar = AnimSidecar.TryLoad(fbxPath, out string sidecarText);
            if (sidecar == null) return false;
            var importer = AssetImporter.GetAtPath(fbxPath) as ModelImporter;
            if (importer == null) return false;

            var rig = NifMergeSettings.instance.animationRig;
            string stamp = StampPrefix + ImporterVersion + ":" + rig + ":" +
                           Hash128.Compute(sidecarText) + ":" + Hash128.Compute(File.ReadAllBytes(fbxPath));
            if (!force && importer.userData == stamp) return true;

            var warnings = new List<string>();
            importer.useFileScale = true;
            importer.globalScale = 1f;
            importer.materialImportMode = ModelImporterMaterialImportMode.None;
            importer.importAnimation = true;
            importer.optimizeGameObjects = false;
            if (rig == NifMergeSettings.AnimationRig.Humanoid)
            {
                importer.humanDescription = NifMergeHumanoid.Build(fbxPath, sidecar.humanoid, warnings, RestPose(sidecar));
                importer.animationType = ModelImporterAnimationType.Human;
                importer.avatarSetup = ModelImporterAvatarSetup.CreateFromThisModel;
            }
            else
            {
                importer.animationType = ModelImporterAnimationType.Generic;
                importer.avatarSetup = ModelImporterAvatarSetup.NoAvatar;
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
                // Skyrim clips play in place (root motion is not exported): keep the root where the clip
                // has it instead of letting the Animator move the character.
                clip.lockRootRotation = true;
                clip.keepOriginalOrientation = true;
                clip.lockRootHeightY = true;
                clip.keepOriginalPositionY = true;
                clip.lockRootPositionXZ = true;
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
