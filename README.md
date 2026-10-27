# NifMerge

Merges Skyrim character NIFs (body / hands / feet `_0`+`_1` weight pairs, a head or in-game FaceGen
head, hair and other head parts) onto a skeleton NIF and writes **one skinned FBX that Unity imports
as a Humanoid**, with one material slot per source shape. A companion Unity package configures the
avatar and builds materials automatically (a Skyrim skin shader for skin, Standard for the rest).

Research, design decisions and open items: [docs/DEV_PLAN.md](docs/DEV_PLAN.md).

## Using it

1. Run `build\Release\NifMerge.exe` (or drag `.nif` / `.tri` files onto it).
   Files dragged onto the open window go to the list they are dropped on (Model parts, Outfit or
   Facial morphs). Dropped anywhere else, they are sorted by type: a skeleton to the skeleton field,
   other `.nif` files to the parts, `.tri` files and folders to the facial morphs.
2. Pick the skeleton (e.g. XPMSE `skeleton_female.nif`) and add the part NIFs. Listing either file of
   a `_0`/`_1` pair is enough, because the partner is found automatically.
3. Optionally add clothing or armour under *Outfit*, and facial morph files (`.tri`), or whole
   folders of them, under *Facial morphs*.
4. Set the body weight (0 = `_0`, 100 = `_1`), choose the output `.fbx`, press **Go!**

Output: `<name>.fbx` plus `<name>.materials.json` (texture paths, shader settings, Humanoid map,
blend-shape list). Copy both into your Unity project.

### Outfit

A character can be generated wearing clothing or armour: add the item NIFs (`_0`/`_1` pairs pair
automatically) under *Outfit*, or `--wear` on the command line. Each item is merged like any part and
**replaces the character's triangles in the body slots it takes**, as the game hides the naked body
under armour. Slots come from each shape's dismember partitions (Skyrim's biped slots, 30–61):
- a dress in slot 32 replaces the body (it carries its own copy of the body, which is used);
- boots in slot 37 replace the feet; the legs stay inside them;
- an item in a slot no character part uses (a thong in 52) is added on top.

Hiding is per partition: if a character shape is split into several slots, only the triangles in a
taken slot go. A head is the exception: only its hair (slot 31) reacts, so a helmet hides the hair,
but the eyes, mouth and brows (which FaceGen marks as slot 32) never disappear under a dress. Two
items taking one slot is an error. The GUI shows under each item what it replaces, and the log and
the sidecar (`parts`) record what was removed. Where the outfit replaces a shape entirely, the item's
surface takes its place in the shading, so the wrists, neck and ankles shade without seams.

### Facial morphs

Skyrim animates faces with morph targets stored in `.tri` files next to the head-part meshes:
phonemes for lip-sync, expressions, blinks, brow movement and eye looks. NifMerge turns each morph
into a blend shape on the merged mesh (e.g. `Aah`, `BlinkLeft`, `MoodHappy`, `LookLeft`). A name
used by several parts becomes one blend shape that moves them all.

Point it at the expression `.tri` files for your head parts. A folder of mixed files is fine: each
file is matched to a head part by its geometry, and the log (and `morphSources` in the sidecar) says
what was applied where and why the rest were not:
- **index**: same vertex count as a part, vertex for vertex (e.g. a mouth). A file with the part's
  own triangles is used however much the part has been sculpted (e.g. the High Poly Head's
  `femalehead.tri` on a FaceGen head);
- **window**: a run of a part's vertices (e.g. one eye of a two-eye mesh);
- **surface**: a lower-resolution mask lying on a part's surface (e.g. High Poly Head face masks).
  Vertices close to a same-facing mask surface take its motion directly. Surfaces the mask does not
  describe (the top and inside of the lips) and the band around the mask's edge are smoothed across
  the part's own mesh, so they move with the skin they are attached to and ease out at the edge.

Files for a different head (another sex, another head mesh) fail the fit and are skipped.
Same-named files in different folders are alternatives, and the stronger, denser match wins.
Otherwise morphs are chosen by name. Files giving a part different morphs all apply, e.g. an
animation mod's `EXPR_` morphs beside the game's own. Where two files give the same morph to the
same part, the closer match keeps it: a full-mesh file over a mask, and the better-fitting of two
masks covering the same area.
Character-creation (`*chargen.tri`) files are skipped: a FaceGen head already has them baked in.

### Unity side

Add the package `unity/com.nifmerge.importer` (Package Manager → *Add package from disk…* →
`package.json`). From then on, importing a NifMerge FBX:

- sets **Animation Type = Humanoid**, mapping Skyrim bones to Unity's (Hips = `NPC COM [COM ]`), with
  a T-pose reference skeleton (the mesh keeps Skyrim's arms-down bind pose);
- creates `<name>_Materials/*.mat` and remaps the model's slots onto them. Skin slots use the
  package's *NifMerge/Skyrim Skin* shader, which adds Skyrim's specular (colour × strength, masked by
  the `_s` map, glossiness as the Blinn-Phong exponent) on top of the diffuse as the game does. Every
  other slot uses Standard;
- writes `<name>_Materials/ImportReport.txt` listing anything it could not do.

Textures: put Skyrim's textures in **Assets/SkyrimTextures** (configurable under *Project Settings →
NifMerge Importer*), keeping the folder structure below `textures\`. Diffuse maps and FaceGen tints
are used directly as `.dds`. Skin specular maps (`_s`) are used as `.dds` too, with sRGB decoding
switched off, because they are masks. Normal maps are converted to PNGs Unity's shaders can use:
- tangent-space maps (`_n`): green flipped from DirectX to Unity's convention, specular mask
  removed from alpha (`<name>_unity_n.png`);
- model-space maps (`_msn`, used by Skyrim's body, hands, feet and head skin): baked to tangent
  space over the imported mesh at full resolution (`<name>_unity_ts_n.png`). A 4K body map takes a
  few seconds.

*Tools → NifMerge* rebuilds materials after adding textures.

Known limits, the first reported per slot in the import report:
- double-sided slots render front faces only;
- the skin shader does not reproduce Skyrim's rim, soft or back lighting (the `_sk` map), so skin is
  darker at silhouettes than in game;
- non-skin slots ignore Skyrim's specular mask and strength.

### Command line

```
NifMerge merge --skeleton <skeleton.nif> --part <part.nif> [--part ...] [--wear <item.nif> ...]
               [--weight 0..100] [--tri <file.tri | folder> ...] --out <model.fbx> [--obj <debug.obj>]
NifMerge inspect <file.nif> [--skeleton <skeleton.nif>]
NifMerge gui [same options as merge] [--go] [--exit-when-done] [--write-layout <file>]
```

`--write-layout` is for the GUI test: it writes where the drop sections and Go! are, in window pixels.

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
| `build\tests\Release\core_tests.exe` | Re-pose vs. an independent evaluation of the game's skinning formula (nifly math) on every sample and fixture; FaceGen part placement; weight blending; seam-continuous normals; merge invariants; FBX read-back (geometry, materials, bind pose = rest pose); TRI decoding; morph-to-part matching decisions; exact reproduction of matched morphs; surface transfer (left/right consistency against the source masks' own, locality); blend shapes surviving the FBX round trip; outfit: each sample item replacing or adding exactly what its slots say, the head's eyes, mouth and brows kept, everything left visible unchanged, seam-continuous normals when dressed, two items in one slot rejected, and (with nifly-written stand-ins) a helmet hiding only the hair and per-partition hiding of a body split into two slots. |
| `python tests\run_gui_test.py` | Drives the real GUI (`gui --go`, the character wearing the sample outfit) and checks its output is identical to the CLI's, plus exit codes and a window screenshot. Then builds the same character by real drag and drop (`gui_input`: OLE drags, as from Explorer, onto each section, and a real click on Go!) and checks that output is identical too, which only holds if every file landed in the section it was dropped on. It moves the mouse, so leave it alone while it runs. |
| `python tests\run_unity_harness.py` | Batch-mode Unity (6000.0 and 2022.3): automatic import → valid Humanoid avatar, every bone mapping, 12 materials (Skyrim Skin on skin slots, Standard elsewhere), skin specular (the `_s` mask assigned and imported as data, colour × strength and glossiness carried over, and a rendered quad matching Skyrim's specular formula at two light angles, in Gamma and Linear colour space), texture resolution, normal-map conversion, model-space → tangent-space bake of the user's real `_msn` maps (round-trip, agreement with Unity's mesh normals, no added seams), orientation/scale, bind-pose skinning, T-pose reference, retargeting vs. a canonical T-pose rig, blend shapes (all imported, at rest when zeroed, blinks on the correct side, `Aah` opens the mouth, head-only), screenshots including face close-ups. A second model wears the sample outfit: Humanoid, the replaced parts absent, every material built, the items' normal maps converted and alpha modes set, the dress's body copy baked from its `_msn` map. |

The tests need two sets of Skyrim assets, and neither is committed, because the repository may not
redistribute them. A missing fixture fails the tests.

- `tests/fixtures/pynifly/`: run `python tests\fetch_fixtures.py` once. It downloads them from
  PyNifly's public repository at a pinned commit and verifies their SHA-256 hashes.
- `samples/`: your own Skyrim SE set: `skeleton_female.nif` (XPMSE), `femalebody_0/1.nif`,
  `femalehands_0/1.nif`, `femalefeet_0/1.nif`, `Head.nif`, the `_msn` maps
  `femalebody_1_msn.dds`, `femalehands_1_msn.dds`, `femalehead_msn.dds`, and `trifiles/`: expression
  `.tri` files for the head parts (female and male, vanilla and in `trifiles/HPH` High Poly Head),
  and `clothing/`: an outfit as `_0`/`_1` pairs: `Dress/01_LatexDress` (slot 32, with its own body
  copy), `Boots/02_HighBoots` (slot 37) and `Thong/03_String` (slot 52).

## Third-party code

`third_party/nifly` (NIF library from BodySlide/Outfit Studio, GPL-3.0) and `third_party/imgui`
(Dear ImGui v1.91.8, MIT) are vendored; see [third_party/README.md](third_party/README.md). Because
nifly is GPL-3.0, NifMerge binaries are GPL-3.0 when distributed.
