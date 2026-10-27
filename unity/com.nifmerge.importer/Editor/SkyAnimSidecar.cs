using System;
using System.IO;
using UnityEngine;

namespace NifMerge
{
    // Mirror of the <clip>.anim.json sidecar written by SkyAnim (formatVersion 2).
    [Serializable]
    public class AnimSidecar
    {
        public string generator;
        public int formatVersion;
        public string fbx;
        public string clip;        // clip name
        public string source;      // the .hkx it came from
        public string platform;    // "LE" or "SE"
        public int frames;
        public float fps;
        public float duration;     // seconds
        public bool loop;
        public bool rootMotion;    // always false: clips play in place
        public AnimEventEntry[] events;
        public string binding;     // "by name", "by index" or "explicit"
        public string[] droppedTracks;
        public string[] unverifiedFormats;
        public HumanoidEntry[] humanoid;
        public RestEntry[] rest;   // every bone's rest transform relative to its parent, FBX space

        public const string Generator = "SkyAnim";
        public const int FormatVersion = 2;

        public static string PathFor(string fbxPath)
        {
            string dir = Path.GetDirectoryName(fbxPath) ?? "";
            return Path.Combine(dir, Path.GetFileNameWithoutExtension(fbxPath) + ".anim.json").Replace('\\', '/');
        }

        // Null when the FBX is not SkyAnim output.
        public static AnimSidecar TryLoad(string fbxPath, out string text)
        {
            text = null;
            string path = PathFor(fbxPath);
            if (!File.Exists(path)) return null;
            text = File.ReadAllText(path);
            var s = JsonUtility.FromJson<AnimSidecar>(text);
            if (s == null || s.generator != Generator) return null;
            if (s.formatVersion != FormatVersion)
            {
                Debug.LogError($"SkyAnim: {path} has formatVersion {s.formatVersion}; this importer reads version " +
                               $"{FormatVersion}. Re-export the clip with the matching SkyAnim.");
                return null;
            }
            return s;
        }
    }

    [Serializable]
    public class RestEntry
    {
        public string bone;
        public float[] t; // translation, metres, FBX axes (Y up, right-handed)
        public float[] r; // rotation quaternion x, y, z, w, FBX axes

        // Unity imports FBX by mirroring X: a position (x, y, z) becomes (-x, y, z) and a rotation
        // (x, y, z, w) becomes (x, -y, -z, w).
        public Vector3 UnityPosition => new Vector3(-t[0], t[1], t[2]);
        public Quaternion UnityRotation => new Quaternion(r[0], -r[1], -r[2], r[3]);
    }

    [Serializable]
    public class AnimEventEntry
    {
        public float time; // seconds
        public string name;
    }
}
