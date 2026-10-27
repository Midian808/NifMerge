using System.Collections.Generic;
using UnityEditor;
using UnityEngine;

namespace NifMerge
{
    // Per-project settings, stored in ProjectSettings/NifMergeSettings.asset.
    [FilePath("ProjectSettings/NifMergeSettings.asset", FilePathAttribute.Location.ProjectFolder)]
    public class NifMergeSettings : ScriptableSingleton<NifMergeSettings>
    {
        [Tooltip("Project folder that mirrors Skyrim's Data/textures folder. A nif texture path " +
                 "'textures\\actors\\x.dds' resolves to '<this folder>/actors/x.dds'.")]
        public string texturesRoot = "Assets/SkyrimTextures";

        public enum AnimationRig { Humanoid, Generic }

        [Tooltip("How SkyAnim animation FBX files import. Humanoid: retargets onto any Humanoid character " +
                 "(non-human bones such as twist and physics bones follow when the character has them). " +
                 "Generic: exact playback on a Generic-rigged character (an Animator without a Humanoid avatar) " +
                 "built from the same skeleton.")]
        public AnimationRig animationRig = AnimationRig.Humanoid;

        public void Save() => Save(true);
    }

    static class NifMergeSettingsProvider
    {
        [SettingsProvider]
        public static SettingsProvider Create()
        {
            return new SettingsProvider("Project/NifMerge Importer", SettingsScope.Project)
            {
                label = "NifMerge Importer",
                guiHandler = _ =>
                {
                    var s = NifMergeSettings.instance;
                    EditorGUI.BeginChangeCheck();
                    s.texturesRoot = EditorGUILayout.TextField(
                        new GUIContent("Skyrim textures root", "Project folder that mirrors Skyrim's Data/textures"),
                        s.texturesRoot);
                    s.animationRig = (NifMergeSettings.AnimationRig)EditorGUILayout.EnumPopup(
                        new GUIContent("SkyAnim animations import as", "Humanoid (retargetable) or Generic (exact)"),
                        s.animationRig);
                    if (EditorGUI.EndChangeCheck())
                    {
                        s.Save();
                        SkyAnimImporter.ReconfigureAll();
                    }
                    EditorGUILayout.HelpBox(
                        "Copy (or extract) the textures your models use into this folder, keeping Skyrim's folder " +
                        "structure below 'textures'. Then use Tools > NifMerge > Rebuild All Materials.",
                        MessageType.Info);
                    if (GUILayout.Button("Rebuild All Materials")) NifMergeImporter.RebuildAll();
                },
                keywords = new HashSet<string>(new[] { "NifMerge", "Skyrim", "textures", "SkyAnim", "animation" })
            };
        }
    }
}
