# Skyrim NIF → Unity Humanoid merger — research findings & dev plan

Goal: a GUI tool that takes Skyrim character NIFs (body `_0`/`_1`, hands, feet, head / FaceGen head,
hair) plus a skeleton NIF, and on **Go!** writes one merged skinned model that Unity imports as a
Humanoid, with one material slot per source part.

---

## 1. Research findings

All findings below were verified by building a probe against the `nifly` library and running it on
the real Skyrim fixtures that ship in the PyNifly repo (vanilla and XPMSE skeletons,
`femalebody_1.nif`, an SE FaceGen head `facegen.nif`).

### 1.1 Libraries

| Library | What it is | Verdict |
|---|---|---|
| **nifly** (github.com/ousnius/nifly, C++17, GPL-3) | The NIF parser/writer underneath both BodySlide/Outfit Studio **and** PyNifly. Reads Skyrim LE (`NiTriShape`/`NiTriShapeData`/`NiSkinData`/`NiSkinPartition`), SE (`BSTriShape`, skinned vertex data in the skin partition) and SE FaceGen (`BSDynamicTriShape`). | **Use as the core reader.** It compiled cleanly with this machine's MSVC in one `cl` call, no deps. |
| **PyNifly** | Blender add-on: Python layer over a C wrapper DLL (`NiflyDLL`) around nifly. | Its inaccuracies are in the Blender import layer (heuristics guessing a per-shape transform), not in parsing. Useful as a reference for edge cases, not as a dependency. |
| **BodySlide / Outfit Studio** | Editor built on nifly. FBX export is `src/files/FBXWrangler.cpp`. | Root cause of its failures pinned below (§1.2). |

### 1.2 Why Outfit Studio's export is wrong, and why FaceGen eyes/teeth end up offset

**Outfit Studio's FBX export** (`FBXWrangler::AddNif` / `FBXWrangler::AddSkinning`):
- Moves each shape by **one** per-shape transform (`AnimInfo::GetTransformShapeToGlobal`, the inverse
  of the shape's global-to-skin), then sets every cluster's bind matrix to the joint's *current* global
  transform. It **never uses the per-bone skin-to-bone matrices**, which is where Skyrim actually
  stores each shape's bind pose.
- Creates **no materials** (material IDs lost), **no FBX bind pose** (`FbxPose`), and writes UVs and
  normals per control point (so UV seams and hard edges are lost).

**What a FaceGen head NIF actually contains** (measured on `SkyrimSE/meshes/facegen.nif`):
- The bone NiNodes in the file carry **translation only; their rotations are stripped**. `NPC R
  Clavicle` sits at identity rotation in the file; in the skeleton it is about (115°, −57°, 146°).
  PyNifly's code notes the same ("Facegen bone rotations are missing").
- The parts inside use **different skin spaces**: head, mouth and hair vertices are stored in world
  space (skin-to-bone = inverse of the bone's world transform). The eyes are stored relative to the
  head bone, and also carry a shape transform plus a global-to-skin that cancel each other out.
- Any tool that places parts using the file's own nodes, or the per-shape global-to-skin, therefore
  misplaces the eyes and mouth/teeth. That is exactly the offset you see.

**The correct transform** is the one the game uses. Every vertex goes into the skeleton's rest pose
by:

```
v_rest = Σ_i  w_i · ( SkeletonBoneWorld_i ∘ SkinToBone_i )( v )
```

(normals the same way, using the rotation part only, then renormalised). Measured: with the vanilla
skeleton, every bone of every FaceGen shape gives the identical skin→world transform, so this lands
each part exactly where the game draws it. It also ignores the file's broken node transforms
entirely. The body file binds consistently too. Against the XPMSE female skeleton it shows a constant
rigid offset of (0, −2.54, 3.29) units on every bone, which this transform absorbs automatically.

### 1.3 Skeleton structure → Unity Humanoid

The vanilla and XPMSE skeletons have the same core chain (XPMSE adds pass-through `CME …` nodes, and
Unity accepts extra intermediate transforms):

```
NPC Root [Root]
└─ NPC COM [COM ]
   ├─ NPC Pelvis [Pelv] ─ NPC L/R Thigh ─ Calf ─ Foot ─ Toe0
   └─ NPC Spine [Spn0] ─ Spine1 ─ Spine2
                          ├─ NPC Neck [Neck] ─ NPC Head [Head]
                          └─ NPC L/R Clavicle ─ UpperArm ─ Forearm ─ Hand ─ Finger00..42
```

**`NPC Spine` is a sibling of `NPC Pelvis`, not its child.** Unity requires both the legs and the
spine to sit under Hips, so **Hips must be `NPC COM [COM ]`**. The pelvis becomes an unmapped
intermediate bone.

| Unity Humanoid | Skyrim bone |
|---|---|
| Hips | `NPC COM [COM ]` |
| Spine / Chest / UpperChest | `NPC Spine [Spn0]` / `NPC Spine1 [Spn1]` / `NPC Spine2 [Spn2]` |
| Neck / Head | `NPC Neck [Neck]` / `NPC Head [Head]` |
| L/R Shoulder | `NPC L/R Clavicle [L/RClv]` |
| L/R UpperArm / LowerArm / Hand | `NPC L/R UpperArm` / `NPC L/R Forearm` / `NPC L/R Hand` |
| L/R UpperLeg / LowerLeg / Foot / Toes | `NPC L/R Thigh` / `NPC L/R Calf` / `NPC L/R Foot` / `NPC L/R Toe0` |
| Thumb / Index / Middle / Ring / Little (×3 each) | `Finger00-02` / `10-12` / `20-22` / `30-32` / `40-42` |
| Eyes / Jaw | none in the vanilla skeleton (eyes are skinned to the head) |

Bone names contain brackets and **significant trailing spaces** (`NPC COM [COM ]`, `NPC L Foot [Lft ]`).
They must be carried byte-exact.

### 1.4 Rest pose, units, axes

- **The rest/bind pose has the arms down at about 65° below horizontal, not a T-pose.** Left upper arm
  (−13.0, −5.2, 106.5) → forearm (−22.4, −5.1, 85.7). Unity's Humanoid avatar wants a T-pose
  *reference*, which its avatar configuration supplies ("Enforce T-Pose") without re-binding the mesh.
  The plan keeps the game's bind pose; re-binding to a T-pose would only add error.
- Skyrim is Z-up, 1 unit ≈ 1.428 cm (the head bone sits at about 120 units, so a figure is about 1.75 m).
  The output is converted to Unity's Y-up, metres.
- One XPMSE skeleton bone has a non-unit scale (`NPC R Hand` s = 0.852). The pipeline must handle
  scale in bone transforms, not assume it is 1.

### 1.5 Body weight, parts, materials

- Body, hands and feet come as `_0`/`_1` pairs with identical topology. The tool interpolates
  `v = lerp(v0, v1, weight/100)` for positions and normals, and refuses (with a clear error) any pair
  whose vertex/triangle counts differ. Heads and FaceGen heads have no weight variants.
- A FaceGen head NIF (`meshes\actors\character\facegendata\facegeom\<plugin>\<formid>.nif`)
  already holds head, mouth/teeth, eyes, brows, hair and beard as separate shapes. A plain head
  NIF plus separate head-part NIFs works the same way, since the tool merges any number of input
  NIFs.
- FaceGen bone naming: some FaceGen files name the head bone `Head` instead of `NPC Head [Head]`.
  The tool maps the known aliases and reports any bone it cannot find in the skeleton.
- Materials: each shape has a `BSLightingShaderProperty` (shader type: SkinTint for the body,
  FaceGen/FaceTint for the head, HairTint for hair; texture set: diffuse, normal, subsurface/glow,
  detail, environment, env mask, …) and optionally a `NiAlphaProperty` (hair, brows, lashes).
  **One shape becomes one submesh/material slot**, named deterministically, and a sidecar
  `<model>.materials.json` records each slot's texture paths, shader type, flags and alpha settings,
  so materials can be built in Unity.

### 1.6 The user's sample set (`samples/`, Skyrim SE, XPMSE female skeleton)

| File | Shapes (verts) | Binding vs `skeleton_female.nif` (XPMSE) |
|---|---|---|
| `femalebody_0/1.nif` | main body (18436) and two small detail shapes (1905, 201) | 61 bones identity. `NPC L/R Hand` carry the hand-scale offset (see below). Also uses non-Humanoid physics bones (`L Breast01`, `VaginaB1`, …), all of which exist in the skeleton. |
| `femalehands_0/1.nif` | hands (10652) | Forearm and twist bones identity. Hand and all finger bones: **scale 0.8517** plus a matching offset |
| `femalefeet_0/1.nif` | feet (8876) | all 6 bones identity |
| `Head.nif` (FaceGen, `BSDynamicTriShape`) | mouth (141), head (3832), hair (13708), hairline (13708), scalp (221), eyes (176), brows (371) | Head, hair, hairline and scalp are stored in world space. **Mouth, eyes and brows are stored relative to `NPC Head [Head]`** (skin→world = the head bone's position (0, −1.55, 120.34)). Every bone agrees within each shape. |

- The `_0`/`_1` pairs have matching vertex counts, so weight interpolation is valid.
- The FaceGen file shows the mixed-skin-space layout from §1.2 (mouth, eyes and brows stored relative
  to the head). The re-pose formula resolves it exactly.
- **XPMSE female hand scale.** The XPMSE female skeleton scales `NPC L/R Hand` by 0.8517, so in-game
  the hands render about 15% smaller than authored. Re-posing onto this skeleton reproduces the
  in-game hands. **Decision taken:** the output skeleton is written **scale-free**. The hand scale is
  baked into the hand and finger bone positions and the re-posed vertices, so the rest geometry is
  identical to in-game, but Unity gets no scaled bones (scaled bones interact badly with Humanoid
  retargeting and with child transforms).
- There are 12 material slots: body 3, hands 1, feet 1, head 7.
- **The six skin slots (the three body shapes, hands, feet and the FaceGen head shape) use
  model-space normal maps** (`SLSF1_MODEL_SPACE_NORMALS`; `_msn.dds` in slot 1, specular in
  slot 7) and therefore **store no vertex normals**. The tool computes area-weighted smooth normals
  after merging. They are welded by position, so UV seams and the neck, wrist and ankle part seams
  shade continuously. Unity's Standard shader only accepts tangent-space normal maps, so the
  Unity package converts these `_msn` maps (see §5).

### 1.7 This machine

- Visual Studio 2022 and VS 18 Community (MSVC 14.44 / 14.51) with bundled CMake; .NET 9 / 10 SDKs;
  Python 3.13.
- Unity editors 2017.4 → 6000.0.68 under `D:\UnityHubFuckery`. Batch-mode Unity can be used to
  **automatically verify** that the output imports as a valid Humanoid.

---

## 2. Architecture

**A single native Windows app, C++17**, with nifly vendored as source:

```
inputs (skeleton.nif, part NIFs, weight)
  → Load        nifly: shapes, verts (FaceGen BSDynamicTriShape: from the dynamic block), normals,
                UVs, tris, bone list, weights, skin-to-bone, shader/alpha
  → Resolve     bone-name aliases ("Head" → "NPC Head [Head]"); every used bone must exist in the
                skeleton (else hard error naming the bones)
  → Re-pose     v_rest = Σ w·(SkelWorld ∘ SkinToBone)(v) per vertex; stored normals by the
                inverse-transpose of the same blend
  → Weight      lerp the re-posed _0/_1 pairs (vertex and triangle lists must match)
  → Merge       one mesh, one submesh per shape; bone set = used bones + Humanoid-mapped bones + all
                their ancestors; vertex weights normalised
  → Normals     shapes that store none (model-space-normal-mapped skin) get area-weighted normals
                accumulated by position, so UV seams and the neck, wrist and ankle part seams shade
                continuously
  → Write FBX   Y-up metres ((x, y, z) → (-x, z, y) × 0.0142875); bone hierarchy (scale baked out,
                §1.6); one mesh node "<name>_Mesh" (never the file's name, which Unity gives the
                root); skin clusters (bind = skeleton world) + FbxPose bind pose; per-control-point
                UVs (V flipped) and normals; one material per submesh
  → Sidecar     <model>.materials.json: slots, texture paths, shader settings, Humanoid map
```

Vertices are **not** welded across parts: that would merge vertices belonging to different material
slots. Only the computed normals are shared across seams.

The same core (`RunPipeline`) runs from the **CLI** and from the **GUI**. `NifMerge gui … --go`
opens the GUI prefilled and presses Go!, which is how the GUI test drives it. Dropping `.nif` files
onto the exe opens the GUI with them.

### 2.1 Forks (decide before building)

| Fork | Options | Recommendation |
|---|---|---|
| **FBX writer** | **Decided: Autodesk FBX SDK 2020.3.11**, installed at `C:\Program Files\Autodesk\FBX\FBX SDK\2020.3.11` (x64 `libfbxsdk-md.lib` / `libfbxsdk.dll` present). The build finds it via CMake `FBXSDK_ROOT`. | — |
| **GUI toolkit** | **Decided: Dear ImGui** + native Win32 file dialogs, inside the single C++ exe. | — |

Output is FBX in both cases, because that is the format Unity's Humanoid importer (`ModelImporter`) is
built around. glTF imports through packages that do not support Humanoid avatar setup.

---

## 3. Milestones

| # | Deliverable | Done when |
|---|---|---|
| M0 | Repo scaffold: CMake, vendored nifly, CLI `inspect` command (tree, shapes, per-bone bind-consistency report: the probe used for this research, productionised) | builds from clean; `inspect` runs on all fixtures |
| M1 | Load + weight + re-pose + merge; debug OBJ dump for quick visual checks | FaceGen eyes/mouth land at the skeleton-predicted spots (numeric test, §4) |
| M2 | FBX writer: hierarchy, skin, bind pose, materials, UV/normal split, units/axes; materials sidecar | FBX round-trips through the FBX SDK reader with identical rest-pose positions |
| M3 | Unity verification harness (batch mode): import → Humanoid → assert avatar valid → render a screenshot | passes on every fixture set |
| M4 | GUI: skeleton picker, part list (add/remove; `_0`/`_1` auto-paired), weight slider, output path, **Go!**, log pane | the end-to-end GUI run produces an output identical to the CLI run |
| M5 | Unity editor package (`unity/com.nifmerge.importer`): an `AssetPostprocessor` that detects the tool's FBX (via the sidecar), sets `animationType = Human` with the Skyrim → Humanoid mapping from §1.3 and an enforced T-pose, and builds one material per slot from `<model>.materials.json`. Textures are resolved against a configurable Skyrim textures root inside the project (Unity imports `.dds` natively). Tangent-space normal maps get a converted PNG copy (DirectX → Unity green flip, alpha made opaque, because Skyrim stores a specular mask there) imported as a normal map, alpha-tested/blended slots (hair, brows) get cutout/blend set up from `NiAlphaProperty`, and missing textures are listed as warnings, not silently skipped. | the batch-mode harness (M3) imports with the package present: avatar valid + human, all 12 sample slots get materials, every texture found under the root is assigned |

## 4. Verification (tests exercise the real path)

- **Re-pose correctness:** for every shape, check that all its bones agree on skin→world (the
  consistency report), and that the merged rest-pose vertices equal the formula applied to the source
  NIF.
- **FaceGen placement:** eye-shape centroid and mouth-shape centroid against the head bone, matching
  the in-game placement derived from skin-to-bone (no file-node transforms used).
- **FBX integrity:** read the output back with the FBX SDK: clusters, bind pose, material count ==
  shape count, polygon material indices.
- **Re-pose oracle:** `core_tests` re-evaluates the game's skinning formula independently with
  nifly's own transform math and nifly's own skeleton world transforms, and compares it with the
  loader for every shape of every sample and fixture (max error < 1e-3 units).
- **Seam normals:** vertices sharing a position across UV seams and part seams must have identical
  normals.
- **GUI:** `tests/run_gui_test.py` drives the real GUI and compares its FBX (canonical summary via
  `fbx_summary`; FBX object IDs differ per run) and sidecar with the CLI's.
- **Unity, the real consumer:** `tests/run_unity_harness.py` runs batch-mode 6000.0.68f1 and
  2022.3.61f1. The FBX is placed in a project that has the package, and the import happens
  automatically, the way a user's does. The harness then checks: the avatar is valid and human;
  every Humanoid mapping is live; 12 Standard materials; textures resolved from the textures root;
  the tangent-space normal map is converted (green flipped, alpha opaque); the user's real `_msn`
  maps are baked to tangent space (see §5 for the checks);
  orientation and scale (head bone position, left hand on -X, eyes forward); bind-pose skinning
  equals the mesh; the reference skeleton has horizontal arms and vertical legs. **Retargeting:**
  at all-zero muscles, our avatar's limb angles must equal those of a synthetic humanoid built in an
  exact T-pose (Unity's zero-muscle pose is not itself a T-pose, so this is compared against a
  canonical rig rather than assumed). Screenshots: bind pose, reference T-pose, zero-muscle pose.
- **Fixtures:** the Skyrim/SkyrimSE NIFs in the PyNifly repo, **plus your own real set** (body
  `_0`/`_1`, hands, feet, an in-game FaceGen head, the skeleton), because the FaceGen offset bug is the
  thing this tool exists to fix.

### 2.2 Facial morphs (built)

Skyrim animates faces with per-head-part morph files (`.tri`, `FRTRI003`), not bones: the only face
node in the skeletons is `NPCEyeBone`, and the FaceGen head NIF references no `.tri` (the plugin's
head-part records do). Expression `.tri` files hold phonemes, expressions, blinks, brow movement and
eye looks. Chargen morphs are already baked into a FaceGen head.

**Reading.** `Tri.cpp` decodes difference morphs (scaled int16 offsets) and modifier morphs (absolute
positions turned into offsets), rejecting truncated or over-long files.

**Matching** (`Morphs.cpp`). A TRI's base mesh is compared with each material slot in the rest pose
(rest = skinToRest * tri + t):
- index: same vertex count, t = mean offset;
- window: fewer vertices, a contiguous run of the slot's vertices;
- surface: a lower-resolution mask on the slot's surface.

Acceptance limits (game units): median ≤ 0.35 and p90 ≤ 0.6 for all modes, plus max ≤ 1.2 (index,
window) or ≤ 0.8 (surface). Priority is index > window > surface. Same-named files are alternatives:
the stronger mode wins, then the denser mesh. Per slot there is one index source, windows do not
overlap, a slot with an exact source takes no surface mask, and overlapping masks keep the better
fit.

**One TRI space per head.** All of a head's TRIs share one origin, the median offset of the exact
matches. Surface masks are placed there rather than fitting their own offset: a free fit slides a
mask along the curved face until even another head's mask passes. Measured: the male masks fitted
freely reached median 0.11; at the shared origin they score 0.46–0.49 and are rejected. Only when no
TRI matches exactly is a mask's offset fitted (with a warning).

Decisions on the user's set (`samples/trifiles`, origin (0.00, −2.03, 120.17)):

| File | Slot | Mode | median / p90 / max |
|---|---|---|---|
| `MouthHumanF.tri` | mouth | index | 0.228 / 0.486 / 0.984 |
| `EyesFemaleLeft.tri` | eyes 0–87 | window | 0.106 / 0.374 / 0.893 |
| `EyesFemaleRight.tri` | eyes 88–175 | window | 0.105 / 0.376 / 0.897 |
| `HPH/femaleheadbrows.tri` | brows | index | 0.244 / 0.338 / 0.425 |
| `HPH/femaleheadmaskleftside.tri` | head | surface | 0.191 / 0.472 / 0.573 |
| `HPH/femaleheadmaskrightside.tri` | head | surface | 0.171 / 0.464 / 0.577 |

The vanilla `FemaleHead*` files are superseded by their HPH namesakes. All male files and the male
mouth are rejected; the vanilla `MaleHeadbrows.tri` misses the median limit by only 0.002 and would in
any case lose to the brows' exact source. Result: 47 blend shapes.

**Surface transfer.** Each slot vertex takes the motion of the nearest mask point, preferring a
same-facing mask triangle when it is within 0.3 of the nearest overall. Each mask's winding is
detected from the data. Full strength applies within 1.0 of the mask, fading to zero at 1.5. Where
closed lips nearly touch, a vertex therefore follows its own lip. At closed mouth corners 3D proximity
cannot always decide: 2 of 500 mirrored vertex pairs disagree on `Aah`. Two alternatives were measured
and rejected: UV transfer (the HPH masks have their own UV layout; no UV coincides with the head's)
and filling ambiguous vertices from mesh neighbours (7 pairs, worse). The HPH chargen `.tri` holds the
unshaped head the masks were authored on and could resolve the corners; the user did not supply it.

**FBX.** One `FbxBlendShape` with a channel per morph name. Shapes are written in full (one position
per mesh vertex): the SDK's sparse-index form lost most offsets on read-back. Offsets below about a
micrometre are zeroed by the SDK.

**Checks.**
- Core: TRI decoding; the decisions above; the left eye's `BlinkLeft` reproduced exactly with the
  right eye still; blinks peaking on the correct side and moving down; `Aah` opening downwards.
- Core, surface transfer: left/right consistency of `Aah` and `MoodHappy` measured against the source
  masks' own asymmetry (`Aah` p90 1.4% vs 0.6% in the source, `MoodHappy` 11.3% vs 8.7%); mouth-corner
  disagreements ≤ 1% of pairs; the back of the head untouched; full-shape FBX round trip within 2 µm.
- Unity: all 47 imported by name, zero weights leave the mesh at rest, blink side and direction,
  `Aah` below the eyes and moving down, every morph confined to head parts, face renders.

## 5. Open questions (asked one at a time)

Decided: the tool ships the Unity editor package (M5), and its materials target the
**Built-in render pipeline, Standard shader**. Alpha slots use the Standard shader's Cutout or Fade
rendering mode, chosen from the `NiAlphaProperty` test/blend flags.

### Model-space normal maps (built)

The skin slots' `_msn` maps (§1.6) are converted to tangent space by the Unity package
(`NifMergeNormalBaker`), because the Standard shader only takes tangent-space maps.

- **Channel convention (measured).** Measured on the user's `femalebody_1_msn.dds`,
  `femalehands_1_msn.dds` and `femalehead_msn.dds`: each decoded texel was compared with the
  geometric face normal for every 7th face, across all 48 signed axis permutations. The convention
  is **X = +R, Y = +B, Z = +G** in the shape's skin space, so Skyrim swaps green and blue. Mean
  cosine is 0.91 on all three maps; the next-best mapping scores 0.61 or lower.
- **Tool side.** Each slot records its skin→rest rotation: the average of the per-vertex rotations,
  with a warning if they spread by more than 2° on a model-space slot. The sidecar
  (`formatVersion` 2) carries `msnToMesh`, a 3x3 matrix that takes a decoded texel straight to
  Unity mesh space. For an unrotated skin space it is the identity, since Skyrim's (r, b, g) maps
  to Unity's (x, z, y).
- **Unity side.** After import, every texel covered by a slot's triangles is decoded (GPU blit, so
  any DDS format) and mapped through `msnToMesh`. It is then expressed in the tangent frame
  interpolated at the texel centre from the imported mesh's own normals and Mikk tangents, which is
  the basis the shader rebuilds. Texels within 16 of an island are converted from the source's own
  padding, using the frame of the nearest triangle at its closest point. Bilinear and mip sampling
  at island edges then mixes the same normals the source would. Texels further out are
  flood-filled, so low mips never mix in a flat background. The result is written at source resolution as `<msn>_unity_ts_n.png` and
  imported as a normal map. Slots sharing one `_msn` (the body and feet share
  `femalebody_1_msn.dds`) bake into one texture.
- **Checks (Unity harness, user's real maps).** Body, feet, hands and head:
  - a baked texel read back through the mesh frame reproduces the `_msn` normal within 8° on ≥ 98%
    of samples (measured: 100%, and 1415/1416 on the body);
  - the `_msn` normals agree with Unity's imported mesh normals (mean cosine 0.92 body, 0.98 feet,
    0.96 hands, 0.95 head), which validates the mapping against the real mesh;
  - tangent-space normals point outwards;
  - at UV seams, the bake adds no discontinuity beyond the source's own.
- **Source seams (measured, not a converter defect).** The user's `_msn` maps are themselves
  discontinuous across UV seams. Median / p90 angle between the two sides of a seam:

  | Slot | Seam vertex pairs | Source `_msn` | Baked |
  |---|---|---|---|
  | body | 396 | 1.26° / 3.74° | 1.22° / 3.58° |
  | feet (on the body map) | 736 | 7.44° / 53.2° | 6.39° / 38.6° |
  | hands | 562 | 3.96° / 8.67° | 3.33° / 7.26° |
  | head | 60 | 7.59° / 20.8° | 8.05° / 21.3° |

  The game samples the same texels and shows the same faint island outlines (around the breasts and
  down the centre line). The bake reproduces the source and does not correct the source art. It
  comes out lower where the source points into the surface, because those normals are clamped to the
  front hemisphere.
- **Morphed bodies.** A model-space map describes the shape it was baked for. On a
  BodySlide-morphed body (e.g. larger breasts) the map's normals and the geometry disagree, and the
  game lights the reference shape's normals. The bake keeps that behaviour: it converts the map's
  normals faithfully rather than re-deriving them from the morphed geometry.
