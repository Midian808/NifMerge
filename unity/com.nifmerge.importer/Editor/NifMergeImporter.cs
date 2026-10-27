using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;

namespace NifMerge
{
    // Finds NifMerge FBX files (an FBX with a NifMerge .materials.json beside it) and configures them:
    // Humanoid avatar with a T-pose reference, and one Standard material per slot remapped onto the model.
    public class NifMergeImporter : AssetPostprocessor
    {
        const string StampPrefix = "nifmerge:";
        const string ImporterVersion = "3";

        static void OnPostprocessAllAssets(string[] imported, string[] deleted, string[] moved, string[] movedFrom)
        {
            var models = new HashSet<string>();
            bool texturesChanged = false;
            string root = NormalizedRoot();
            foreach (string path in Concat(imported, moved))
            {
                string p = path.Replace('\\', '/');
                string lower = p.ToLowerInvariant();
                if (lower.EndsWith(".fbx")) models.Add(p);
                else if (lower.EndsWith(".materials.json"))
                {
                    string fbx = p.Substring(0, p.Length - ".materials.json".Length) + ".fbx";
                    if (File.Exists(fbx)) models.Add(fbx);
                }
                else if (root != null && lower.StartsWith(root.ToLowerInvariant() + "/")) texturesChanged = true;
            }
            foreach (string fbx in models) Configure(fbx, false);
            if (texturesChanged) RebuildAll(materialsOnly: true);
        }

        static IEnumerable<string> Concat(string[] a, string[] b)
        {
            foreach (var s in a) yield return s;
            foreach (var s in b) yield return s;
        }

        static string NormalizedRoot()
        {
            string r = NifMergeSettings.instance.texturesRoot;
            if (string.IsNullOrEmpty(r)) return null;
            return r.Replace('\\', '/').TrimEnd('/');
        }

        [MenuItem("Tools/NifMerge/Rebuild All Materials")]
        public static void RebuildAllMenu() => RebuildAll(materialsOnly: true);

        [MenuItem("Tools/NifMerge/Reconfigure Selected Models")]
        static void ReconfigureSelected()
        {
            foreach (var obj in Selection.objects)
            {
                string p = AssetDatabase.GetAssetPath(obj);
                if (p.ToLowerInvariant().EndsWith(".fbx")) Configure(p, true);
            }
        }

        public static void RebuildAll(bool materialsOnly = true)
        {
            foreach (string guid in AssetDatabase.FindAssets("t:Model"))
            {
                string p = AssetDatabase.GUIDToAssetPath(guid);
                if (!p.ToLowerInvariant().EndsWith(".fbx")) continue;
                if (materialsOnly)
                {
                    var sidecar = Sidecar.TryLoad(p, out _);
                    if (sidecar != null) BuildMaterials(p, sidecar);
                }
                else Configure(p, true);
            }
        }

        // Returns true when the model is NifMerge output.
        public static bool Configure(string fbxPath, bool force)
        {
            var sidecar = Sidecar.TryLoad(fbxPath, out string sidecarText);
            if (sidecar == null) return false;
            var importer = AssetImporter.GetAtPath(fbxPath) as ModelImporter;
            if (importer == null) return false;

            string stamp = StampPrefix + ImporterVersion + ":" +
                           Hash128.Compute(sidecarText) + ":" + Hash128.Compute(File.ReadAllBytes(fbxPath));
            if (!force && importer.userData == stamp) return true;

            var warnings = BuildMaterials(fbxPath, sidecar, out var materials);

            importer.materialImportMode = ModelImporterMaterialImportMode.ImportStandard;
            importer.materialLocation = ModelImporterMaterialLocation.InPrefab;
            foreach (var id in new List<AssetImporter.SourceAssetIdentifier>(importer.GetExternalObjectMap().Keys))
                if (id.type == typeof(Material)) importer.RemoveRemap(id);
            foreach (var (name, mat) in materials)
                importer.AddRemap(new AssetImporter.SourceAssetIdentifier(typeof(Material), name), mat);

            importer.useFileScale = true;
            importer.globalScale = 1f;
            importer.importNormals = ModelImporterNormals.Import;
            importer.importTangents = ModelImporterTangents.CalculateMikk;
            // Facial morphs from .tri files arrive as blend shapes; Unity derives their normals.
            importer.importBlendShapes = true;
            importer.importBlendShapeNormals = ModelImporterNormals.Calculate;
            importer.importAnimation = false;
            importer.optimizeGameObjects = false;

            importer.humanDescription = NifMergeHumanoid.Build(fbxPath, sidecar, warnings);
            importer.animationType = ModelImporterAnimationType.Human;
            importer.avatarSetup = ModelImporterAvatarSetup.CreateFromThisModel;
            importer.userData = stamp;
            importer.SaveAndReimport();

            NifMergeNormalBaker.Bake(fbxPath, sidecar, NormalizedRoot(), warnings);
            Report(fbxPath, warnings);
            return true;
        }

        static List<string> BuildMaterials(string fbxPath, Sidecar sidecar)
        {
            var warnings = BuildMaterials(fbxPath, sidecar, out _);
            NifMergeNormalBaker.Bake(fbxPath, sidecar, NormalizedRoot(), warnings);
            Report(fbxPath, warnings);
            return warnings;
        }

        static List<string> BuildMaterials(string fbxPath, Sidecar sidecar, out List<(string, Material)> materials)
        {
            var warnings = new List<string>();
            materials = NifMergeMaterials.Build(fbxPath, sidecar, NormalizedRoot(), warnings);
            return warnings;
        }

        // <fbx dir>/<fbx name>_Materials/ImportReport.txt holds the problems found by the last configuration.
        public static string ReportPathFor(string fbxPath) =>
            $"{Path.GetDirectoryName(fbxPath).Replace('\\', '/')}/{Path.GetFileNameWithoutExtension(fbxPath)}_Materials/ImportReport.txt";

        static void Report(string fbxPath, List<string> warnings)
        {
            File.WriteAllLines(ReportPathFor(fbxPath),
                               warnings.Count == 0 ? new[] { "No problems." } : warnings.ToArray());
            if (warnings.Count == 0)
                Debug.Log($"NifMerge: configured {fbxPath}");
            else
                Debug.LogWarning($"NifMerge: configured {fbxPath} with {warnings.Count} warning(s):\n  " +
                                 string.Join("\n  ", warnings));
        }
    }
}
