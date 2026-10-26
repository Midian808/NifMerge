# NifMerge

Merges Skyrim character NIFs (body / hands / feet `_0`+`_1` weight pairs, a head or in-game FaceGen
head, hair and other head parts) onto a skeleton NIF and writes **one skinned FBX that Unity imports
as a Humanoid**, with one material slot per source shape. A companion Unity package configures the
avatar and builds Standard-shader materials automatically.

Research, design decisions and open items: [docs/DEV_PLAN.md](docs/DEV_PLAN.md).

## Using it

1. Run `build\Release\NifMerge.exe` (or drag `.nif` files onto it).
2. Pick the skeleton (e.g. XPMSE `skeleton_female.nif`) and add the part NIFs. Listing either file of
   a `_0`/`_1` pair is enough, because the partner is found automatically.
3. Set the body weight (0 = `_0`, 100 = `_1`), choose the output `.fbx`, press **Go!**

Output: `<name>.fbx` plus `<name>.materials.json` (texture paths, shader settings, Humanoid map).
Copy both into your Unity project.

### Unity side

Add the package `unity/com.nifmerge.importer` (Package Manager → *Add package from disk…* →
`package.json`). From then on, importing a NifMerge FBX:

- sets **Animation Type = Humanoid**, mapping Skyrim bones to Unity's (Hips = `NPC COM [COM ]`), with
  a T-pose reference skeleton (the mesh keeps Skyrim's arms-down bind pose);
- creates `<name>_Materials/*.mat` (Standard shader) and remaps the model's slots onto them;
- writes `<name>_Materials/ImportReport.txt` listing anything it could not do.

Textures: put Skyrim's textures in **Assets/SkyrimTextures** (configurable under *Project Settings →
NifMerge Importer*), keeping the folder structure below `textures\`. Diffuse maps and FaceGen tints
are used directly as `.dds`. Normal maps are converted to PNGs the Standard shader can use:
- tangent-space maps (`_n`): green flipped from DirectX to Unity's convention, specular mask
  removed from alpha (`<name>_unity_n.png`);
- model-space maps (`_msn`, used by Skyrim's body, hands, feet and head skin): baked to tangent
  space over the imported mesh at full resolution (`<name>_unity_ts_n.png`). A 4K body map takes a
  few seconds.

*Tools → NifMerge* rebuilds materials after adding textures.

Known limit of the Standard shader, reported per slot in the import report: double-sided slots
render front faces only.

### Command line

```
NifMerge merge --skeleton <skeleton.nif> --part <part.nif> [--part ...] [--weight 0..100] --out <model.fbx> [--obj <debug.obj>]
NifMerge inspect <file.nif> [--skeleton <skeleton.nif>]
NifMerge gui [same options as merge] [--go] [--exit-when-done]
```

`inspect --skeleton` prints, for each skinned shape, the skin→world transform each of its bones
implies. When every bone agrees, the shape is bound consistently to that skeleton.

## Building

Requirements: Visual Studio 2022 (MSVC, bundled CMake), Autodesk FBX SDK 2020.3.x
(`-DFBXSDK_ROOT=...` if it is not installed at the default
`C:\Program Files\Autodesk\FBX\FBX SDK\2020.3.11`).

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Tests

Set `NIFMERGE_BUILD=<build dir>` to point the Python tests at a build tree other than `build`.

| Command | What it covers |
|---|---|
| `build\tests\Release\core_tests.exe` | Re-pose vs. an independent evaluation of the game's skinning formula (nifly math) on every sample and fixture; FaceGen part placement; weight blending; seam-continuous normals; merge invariants; FBX read-back (geometry, materials, bind pose = rest pose). |
| `python tests\run_gui_test.py` | Drives the real GUI (`gui --go`) and checks its output is identical to the CLI's, plus exit codes and a window screenshot. |
| `python tests\run_unity_harness.py` | Batch-mode Unity (6000.0 and 2022.3): automatic import → valid Humanoid avatar, every bone mapping, 12 Standard materials, texture resolution, normal-map conversion, model-space → tangent-space bake of the user's real `_msn` maps (round-trip, agreement with Unity's mesh normals, no added seams), orientation/scale, bind-pose skinning, T-pose reference, retargeting vs. a canonical T-pose rig, screenshots. |

The tests need two sets of Skyrim assets, and neither is committed, because the repository may not
redistribute them. A missing fixture fails the tests.

- `tests/fixtures/pynifly/`: run `python tests\fetch_fixtures.py` once. It downloads them from
  PyNifly's public repository at a pinned commit and verifies their SHA-256 hashes.
- `samples/`: your own Skyrim SE set: `skeleton_female.nif` (XPMSE), `femalebody_0/1.nif`,
  `femalehands_0/1.nif`, `femalefeet_0/1.nif`, `Head.nif`, and the `_msn` maps
  `femalebody_1_msn.dds`, `femalehands_1_msn.dds`, `femalehead_msn.dds`.

## Third-party code

`third_party/nifly` (NIF library from BodySlide/Outfit Studio, GPL-3.0) and `third_party/imgui`
(Dear ImGui v1.91.8, MIT) are vendored; see [third_party/README.md](third_party/README.md). Because
nifly is GPL-3.0, NifMerge binaries are GPL-3.0 when distributed.
