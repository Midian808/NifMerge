using System.Collections.Generic;
using UnityEditor;
using UnityEngine;

namespace NifMerge
{
    // Builds the HumanDescription for a NifMerge model. The mesh keeps Skyrim's bind pose (arms down);
    // the avatar's reference skeleton is that pose with the limbs straightened into a T-pose, the
    // same thing the avatar configurator's "Enforce T-Pose" produces.
    public static class NifMergeHumanoid
    {
        public static HumanDescription Build(string fbxPath, Sidecar sidecar, List<string> warnings)
        {
            var prefab = AssetDatabase.LoadAssetAtPath<GameObject>(fbxPath);
            if (prefab == null) throw new System.InvalidOperationException("NifMerge: cannot load " + fbxPath);
            var inst = Object.Instantiate(prefab);
            inst.name = prefab.name;
            inst.hideFlags = HideFlags.HideAndDontSave;
            try
            {
                inst.transform.SetPositionAndRotation(Vector3.zero, Quaternion.identity);
                var byName = new Dictionary<string, Transform>();
                foreach (var t in inst.GetComponentsInChildren<Transform>(true))
                    if (!byName.ContainsKey(t.name)) byName.Add(t.name, t);

                var humanToBone = new Dictionary<string, Transform>();
                var human = new List<HumanBone>();
                foreach (var h in sidecar.humanoid)
                {
                    if (!byName.TryGetValue(h.bone, out var t))
                    {
                        warnings.Add($"Humanoid bone '{h.human}' -> '{h.bone}' is not in the model");
                        continue;
                    }
                    humanToBone[h.human] = t;
                    var hb = new HumanBone { humanName = h.human, boneName = h.bone };
                    hb.limit.useDefaultValues = true;
                    human.Add(hb);
                }

                ApplyTPose(humanToBone);

                var skeleton = new List<SkeletonBone>();
                foreach (var t in inst.GetComponentsInChildren<Transform>(true))
                    skeleton.Add(new SkeletonBone
                    {
                        name = t.name,
                        position = t.localPosition,
                        rotation = t.localRotation,
                        scale = t.localScale
                    });

                return new HumanDescription
                {
                    human = human.ToArray(),
                    skeleton = skeleton.ToArray(),
                    upperArmTwist = 0.5f,
                    lowerArmTwist = 0.5f,
                    upperLegTwist = 0.5f,
                    lowerLegTwist = 0.5f,
                    armStretch = 0.05f,
                    legStretch = 0.05f,
                    feetSpacing = 0f,
                    hasTranslationDoF = false
                };
            }
            finally
            {
                Object.DestroyImmediate(inst);
            }
        }

        // Unity's T-pose: facing +Z, legs straight down, arms and fingers straight out along X
        // (left arm towards -X). Bones are rotated parent-first so children follow.
        static void ApplyTPose(Dictionary<string, Transform> b)
        {
            foreach (string side in new[] { "Left", "Right" })
            {
                Vector3 outward = side == "Left" ? Vector3.left : Vector3.right;
                Align(b, side + "UpperLeg", side + "LowerLeg", Vector3.down);
                Align(b, side + "LowerLeg", side + "Foot", Vector3.down);

                Align(b, side + "UpperArm", side + "LowerArm", outward);
                Align(b, side + "LowerArm", side + "Hand", outward);
                Align(b, side + "Hand", side + " Middle Proximal", outward);
                foreach (string finger in new[] { "Index", "Middle", "Ring", "Little" })
                {
                    Align(b, $"{side} {finger} Proximal", $"{side} {finger} Intermediate", outward);
                    Align(b, $"{side} {finger} Intermediate", $"{side} {finger} Distal", outward);
                }
            }
        }

        // Rotates 'bone' about its own position so the direction to 'child' becomes 'target'.
        static void Align(Dictionary<string, Transform> b, string bone, string child, Vector3 target)
        {
            if (!b.TryGetValue(bone, out var t) || !b.TryGetValue(child, out var c)) return;
            Vector3 dir = c.position - t.position;
            if (dir.sqrMagnitude < 1e-10f) return;
            t.rotation = Quaternion.FromToRotation(dir.normalized, target) * t.rotation;
        }
    }
}
