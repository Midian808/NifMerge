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
  shade continuously. Unity's shaders only accept tangent-space normal maps, so the
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
inputs (skeleton.nif, part NIFs, outfit item NIFs, weight)
  → Load        nifly: shapes, verts (FaceGen BSDynamicTriShape: from the dynamic block), normals,
                UVs, tris, bone list, weights, skin-to-bone, shader/alpha
  → Resolve     bone-name aliases ("Head" → "NPC Head [Head]"); every used bone must exist in the
                skeleton (else hard error naming the bones)
  → Re-pose     v_rest = Σ w·(SkelWorld ∘ SkinToBone)(v) per vertex; stored normals by the
                inverse-transpose of the same blend
  → Weight      lerp the re-posed _0/_1 pairs (vertex and triangle lists must match)
  → Outfit      items take the body slots of their partitions; character triangles in a taken slot
                are hidden (per partition; a head's hair only), §2.3
  → Merge       one mesh, one submesh per shape; bone set = used bones + Humanoid-mapped bones + all
                their ancestors; vertex weights normalised
  → Normals     shapes that store none (model-space-normal-mapped skin) get area-weighted normals
                accumulated by position, so UV seams and the neck, wrist and ankle part seams shade
                continuously; hidden triangles are removed afterwards
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
  every Humanoid mapping is live; 12 materials (Skyrim Skin on skin slots, Standard elsewhere);
  skin specular (see §5); textures resolved from the textures root;
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
window) or ≤ 0.8 (surface). An index fit whose triangle list is the slot's own, index for index, is
accepted at any distance: identical faces prove the vertex order, and a sculpted FaceGen head keeps
its base mesh's topology while moving its vertices well away from the base shape (the user's head
sits a median 0.42, max 0.92, from the High Poly Head's `femalehead.tri`, and fails the distance
limits). Priority is index > window > surface.

**Choosing sources.** Same-named files in different folders are alternatives for one part: the
stronger mode wins, then the denser mesh. Files named `*chargen.tri` (character-creation sliders,
already baked into a FaceGen head) are skipped. Everything else is decided **per morph name**, so
files giving one part different names all apply. The case is an animation mod (EFA), whose `EXPR_`
files for the mouth, eyes, brows and face masks sit beside the game's own. Where two sources give the
same name to the same vertices, one keeps it:
- between exact sources (index, window) on overlapping vertices: the one with the slot's own
  triangles, then the stronger mode, then the closer fit;
- a mask gives no name that an exact source of its slot gives;
- two masks covering mostly the same area: the better fit.

A file left with no names is not applied, with the reason of the first source that took them. Morphs
that move nothing (`femalehead.tri`'s `CreatedByKouLeifoh` marker) give no name. Masks sharing names
are transferred together as one set (the left and right halves of a face: a half without a name does
not move for it); sets with different names are transferred separately, since a mask says nothing
about names it does not carry. The sidecar's `morphSources` lists each applied file's names and
whether it matched by its triangles (`sameTriangles`).

**One TRI space per head.** All of a head's TRIs share one origin, the median offset of the exact
matches within the distance limits. A match accepted only for its triangles does not count: its
offset includes the sculpt (0.25–0.3 from the shared origin for `femalehead.tri`). Surface masks are
placed at the origin rather than fitting their own offset: a free fit slides a mask along the curved
face until even another head's mask passes. Measured: the male masks fitted freely reached median
0.11; at the shared origin they are rejected. Only when no TRI matches within the limits is a mask's
offset fitted (with a warning).

Decisions on the user's set (`samples/trifiles`, origin (0.00, −2.11, 120.17)):

| File | Slot | Mode | median / p90 / max | Outcome |
|---|---|---|---|---|
| `MouthHumanF.tri` | mouth | index | 0.228 / 0.486 / 0.984 | applied, 21 morphs |
| `EyesFemaleLeft.tri` | eyes 0–87 | window | 0.106 / 0.374 / 0.893 | applied, 22 morphs |
| `EyesFemaleRight.tri` | eyes 88–175 | window | 0.105 / 0.376 / 0.897 | applied, 22 morphs |
| `HPH/femaleheadbrows.tri` | brows | index | 0.244 / 0.338 / 0.425 | applied, 44 morphs |
| `HPH/femalehead.tri` | head | index (same triangles) | 0.420 / 0.557 / 0.918 | applied, 44 morphs |
| `HPH/femaleheadmask{left,right}side.tri` | head | surface | 0.151 / 0.438 / 0.560, 0.150 / 0.434 / 0.551 | skipped: `femalehead.tri` gives every name |
| `HPH/*chargen.tri` | | | | skipped: character creation |
| `EFA/expr_mouthhumanf.tri` | mouth | index | 0.228 / 0.486 / 0.984 | applied, 21 `EXPR_` morphs |
| `EFA/expr_eyesfemale{left,right}.tri` | eyes | window | as the game's eyes | applied, 22 `EXPR_` morphs each |
| `EFA/femaleheadbrows_expr.tri` | brows | index | 0.161 / 0.244 / 0.296 | applied, 29 `EXPR_` morphs |
| `EFA/femaleheadmask{left,right}side_expr.tri` | head | surface | 0.067 / 0.196 / 0.372, 0.071 / 0.203 / 0.415 | applied, 37 `EXPR_` morphs each |
| `EFA/expr_femaleheadmask*.tri` (vanilla head) | head | surface | 0.079 / 0.216 / 0.324, 0.078 / 0.203 / 0.374 | superseded by the closer HPH `_expr` masks |
| `EFA/expr_femaleheadbrows.tri` (vanilla brows) | brows | surface | 0.067 / 0.147 / 0.331 | skipped: the brows' exact `EXPR_` source |

The vanilla `FemaleHead*` files are superseded by their HPH namesakes. The male files and the male
mouth are rejected, except the brows: the HPH `maleheadbrows.tri` fits the female brows within the
limits (median 0.332) but gives no name, since the brows' exact source gives them all. Result: 93
blend shapes, 47 of the game's names and 46 `EXPR_` ones.

**Surface transfer.** The masks cover the face's skin but not every surface of the head: the top and
inside of the lower lip are missing. Those head vertices lie 0.3–0.95 from every mask point, and the
nearest one is on the upper lip, which barely moves when the jaw opens. A nearest-point-only transfer
therefore left 47 / 51 / 13 head vertices around the mouth frozen on `Aah` / `BigAah` / `Oh` (the
static band the user saw). The transfer is now:

- **Direct match.** A vertex takes the mask's motion at its nearest point only when that point is
  within 0.25 and on a mask surface **facing the same way** (normals within ~45°, cosine 0.7; each
  mask's winding is detected from the data). A vertex whose nearest point lies on a different part of
  the mask than its neighbours' points (more than 3 triangle rings away across the mask) is not a
  direct match either.
- **Smoothing.** Every other vertex within 3.0 of a mask is smoothed over the head's own mesh, with
  direct matches fixed: each becomes (Σ neighbours + pull × target) / (neighbour count + pull).
  - Same-facing nearest points within 1.5 are pulled towards with weight 2. Their target fades from
    full strength at 1.0 to zero at 1.5, and beyond 1.5 the target is zero.
  - Vertices with no same-facing surface near (the lower lip's top and inside) are not pulled, and
    simply follow the surface they are attached to.
  - UV-seam copies count as neighbours.

Measured choices. The trust distance 0.25 left the fewest frozen lip vertices; 0.30 doubled them. Pure
smoothing (no pull) collapsed the inside of the lips, which touches well-fitting skin at a single
vertex, into one motion (33 / 74 left/right disagreements on `Aah` / `MoodHappy`). Pull 2 balanced
that; pull 4 froze the lip again. A same-facing cosine of 0.34 still let the lower lip's top edge
match the upper lip; 0.7 freed it, and 0.8 behaved the same. Three alternatives were measured and
rejected:
- anchoring to the mouth mesh: it is static where it touches the lips;
- UV transfer: the HPH masks have their own UV layout, and no UV coincides with the head's;
- reshaping the masks with the HPH chargen sliders fitted to this head
  (`femaleheadmask*sidechargen.tri`): the fit tightens (median 0.179 → 0.127, p90 0.469 → 0.333), but
  the masks still have no lower-lip top or inside.

Result on the user's set with the masks carrying the head (`femalehead.tri` left out): **0 frozen head
vertices around the mouth** on `Aah`, `BigAah`, `Oh` and `OohQ`, and on their `EXPR_` counterparts
from EFA's masks. Left/right disagreements over 30% of peak: 2 of 421 pairs on `Aah`, 3 of 691 on
`MoodHappy`, 3 of 469 on `EXPR_Aah` and 8 of 834 on `EXPR_MoodHappy`. `MoodHappy`'s p90 is 0.062,
below its source masks' own 0.087. With `femalehead.tri` the head takes the game's names exactly, by
index, and the transfer serves only EFA's `EXPR_` masks.

**FBX.** One `FbxBlendShape` with a channel per morph name. Shapes are written in full (one position
per mesh vertex): the SDK's sparse-index form lost most offsets on read-back. Offsets below about a
micrometre are zeroed by the SDK.

**Unity import: positions only.** The importer imports the blend shapes with no blend-shape normals
(`importBlendShapeNormals = None`). That is how Skyrim morphs work: a FaceGen head's per-frame data
is positions, its normals stay static, and skin lighting comes from the model-space normal map, which
the bake reproduces exactly when the vertex frames do not turn. Calculated blend-shape normals would
turn the lighting with the deformation, which the game does not do. They also make Unity split head
vertices (3,832 → 3,919), changing the frames the normal-map bake was made for: the head's seam check
failed with them (median 9.29° against the source's 7.72°) and passes without them.

**Checks.**
- Core: TRI decoding; the decisions above; no name given twice to the same vertices; the left eye's
  `BlinkLeft` reproduced exactly with the right eye still; blinks peaking on the correct side and
  moving down; `Aah` opening downwards.
- Core, per-name sources: `MouthHumanF.tri` and EFA's `expr_mouthhumanf.tri` both reproduced exactly
  on the one mouth; `EXPR_Aah` moving the face through EFA's masks beside `femalehead.tri`.
- Core, identical triangles: every morph of `femalehead.tri` reproduced exactly on every head vertex;
  the empty marker morph not a blend shape; the masks' fits unchanged by its offset; the same file
  with one face reversed judged by distance and rejected.
- Core, surface transfer (the set without `femalehead.tri`, so both the game's and EFA's masks carry
  the head): left/right consistency of `Aah` and `MoodHappy` and their `EXPR_` counterparts measured against the source
  masks' own asymmetry (p90 within 0.05 of the source's, disagreements over 30% of peak ≤ 1% of
  pairs); no frozen vertices around the mouth on `Aah`, `BigAah`, `Oh` and `OohQ` (≤ 2% of the head
  vertices there); the back of the head untouched; full-shape FBX round trip within 2 µm.
- Unity: all 93 imported by name, zero weights leave the mesh at rest, blink side and direction,
  `Aah` below the eyes and moving down, every morph confined to head parts, face renders.

### 2.3 Clothing (built)

**Goal.** A character is generated wearing an outfit; nothing is swapped at runtime. Items are merged
like any other part, except that an item replaces the character's own triangles in the body slots it
takes, the way the game hides the naked body under armour.

**What the NIFs say** (measured on `samples/clothing`; the `_0` and `_1` files agree):

| File | Slots (dismember partitions) | Holds | In game |
|---|---|---|---|
| `femalebody` | 32 | body + its genitals | the naked body |
| `femalehands` | 33 | hands | |
| `femalefeet` | 37 | feet | |
| Dress | 32 | the dress **and its own copy of the body** (all three body shapes) | replaces the body |
| Boots | 37 | boots only, no foot inside | replace the feet; the body's legs stay inside them |
| Thong | 52 | the thong | added over the body |
| FaceGen `Head.nif` | eyes, mouth, brows 32; hair, hairline 31; head 130/143/230; scalp 131/141 | head parts | not armour: hidden only through hair |

- The dress's body copy has the body's triangles and vertex order. 7,740 chest vertices (heights
  87.6–107.5) are reshaped by up to 2.1 units to fit the dress, 690 of them visible at its edges, and
  its UVs differ by at most 0.004. The game shows this copy, so the merge uses it, not the character's body.
- The boots sit about 1.1 units outside the legs (heights 0.2–55.9), weighted to the thigh, calf and
  foot bones. The thong sits 0.2–1.2 outside the hips.
- Slots 30–61 are biped slots. The head's 130 and above are dismemberment IDs, which no item takes.
- The plugin's armour records are the real authority on an item's slots; the NIF's partitions must
  match them (community guides: a mismatch makes the armour invisible). No plugin is read, so the
  NIF's slots stand in for them.

**The rule** (`PlanOutfit` in `Merge.cpp`, the one implementation behind the merge and the GUI preview).
1. Inputs are two lists: the **character** (body, hands, feet, head) and the **outfit** (items).
   Separate lists are required: the body and the dress both declare slot 32, and nothing in either
   file says which one is the skin.
2. Every outfit item is merged. It takes the biped slots (30–61) of its partitions. An item with
   none is added, with a warning.
3. **Hiding is per partition** (decided; no source found states the engine's own rule). In a
   character shape, the triangles in a taken slot are removed, with
   the vertices no remaining triangle uses. A shape left with no triangles leaves the model and its
   material slot, and takes no part in material naming (the dress's body copy is `3BA`, not `3BA_2`).
   Every sample shape has a single partition, so the dress removes `femalebody`'s three shapes whole
   and the boots remove `femalefeet`. Hands stay.
4. **Head exception.** A part with a head-part partition (an ID of 100 or more) is a head. Only its
   hair (slot 31) reacts to items: a helmet taking 31 hides the hair. Its eyes, mouth and brows carry
   slot 32 but are head parts, not armour, so a dress never removes them.
5. Two outfit items taking one slot is an error naming both. An item taking a slot no character part
   uses is simply added (the thong).
6. **Normals come from the surface as worn.** A shape the outfit replaces entirely does not count: the
   item's own surface takes its place. Measured: with your body counted, the dress's body copy and
   the hands differed at one wrist vertex per side (normals 0.85 against 0.88 in X), because the copy
   and your body are not identical there. A shape only partly hidden counts in full: hiding a partition
   cuts no seam into the surface, so what stays visible shades exactly as without the outfit. Hidden
   triangles are removed after the normals are computed.
7. Weight pairs (whose `_0`/`_1` partitions must agree), re-posing, materials and morphs are unchanged:
   items are parts. The dress's body copy is skin with a model-space normal map, handled like the body.

Result on the sample set, wearing all three: 14 material slots. The dress replaces `femalebody`'s
`3BA`, `3BA_Vagina` and `3BA_Anus`; the boots replace `Feet`; the thong is added; the hands and the
whole head are kept.

**Surfaces.**
- Core: `MergeRequest::outfit`. Each triangle's slot is read from nifly's `GetShapePartitions`
  while loading (`ReadShapeSlots` reads only the slots). Each material records its `role` (character
  or outfit) and `slots`. `MergedModel::parts` records each input file's role, slots, whether it is a
  head, and every removal (shape, slot, triangles, whole shape, by which item).
- CLI: `--wear <nif>` (repeatable, paired like `--part`); `gui … --go` takes it too.
- GUI: an **Outfit** list with add, remove, clear and pairing. Under each item, a dimmed line from
  `DescribeOutfit` (the same rule, read from the files' slots alone) says what it replaces or that it
  is added; a slot clash shows in red before Go!.
- Sidecar (still formatVersion 2; the fields are additions): `parts`, and `role` and `slots` per
  material. The Unity package's sidecar mirror has the same fields. Its importer is unchanged: the
  items are Standard materials with tangent-space normal maps and alpha, which it already handles.
- The test harnesses find the body as the skin-tinted slot in body slot 32 (the character's own or an
  outfit's body copy), no longer by file name.

**Tests** (all on the real sample files):
- Core:
  - Dress: the character's three body shapes are removed whole, and the dress and its body copy are
    present. The body copy equals the dress file re-posed. Hands, feet and every head shape are
    unchanged, including the FaceGen eyes, mouth and brows. Vertices where hands or feet meet the
    item's surface get their normals from it, so only their positions and UVs are compared.
  - Boots: the feet are removed in slot 37; the body (legs inside the boots) and hands are unchanged.
  - Thong: added in slot 52, nothing removed.
  - All three together: 14 slots, and seam-continuous normals at the wrists and neck. The GUI preview
    equals the merge log. The sidecar records the removals and each material's role and slots.
  - Two slot-32 items (the dress, plus the body file given as an item): an error naming both and the
    slot.
  - Helmet: no helmet sample, so the thong is re-partitioned to slot 31 with nifly into `.tmp/tests`.
    Wearing it hides exactly the two hair shapes and leaves every other shape unchanged.
  - Per partition: no sample shape spans two slots, so a copy of the body has its triangles below the
    knee re-partitioned to slot 38, and a copy of the boots takes 37 + 38. Wearing them removes
    exactly those triangles and their unused vertices, keeps the rest of the body unchanged
    (positions, normals, UVs), and removes the feet whole.
- GUI test: the character wears all three items, and the GUI's FBX and sidecar equal the CLI's.
- Unity (6000.0.68 and 2022.3.61): a second model wears all three items. Checks: Humanoid valid; the
  replaced parts absent; every material a `.mat` with its expected shader; the items' diffuse assigned, normal maps
  converted and alpha modes set; the dress's body copy baked from `femalebody_1_msn.dds`; bind-pose
  skinning exact; `outfit_bind_pose.png` rendered.

**Test data gap.** The hair rule is tested only with a re-partitioned stand-in. A real helmet NIF in
`samples/clothing` would let the test use real data.

## 5. Open questions (asked one at a time)

Decided: the tool ships the Unity editor package (M5), and its materials target the
**Built-in render pipeline**. Skin slots (SkinTint and FaceGen shader types) use the package's own
**Skyrim Skin** shader (see *Skin specular* below); every other slot uses the **Standard** shader.
Alpha slots use Cutout or Fade, chosen from the `NiAlphaProperty` test/blend flags: Standard's
rendering modes, or for skin the `_ALPHATEST_ON` keyword or the *Skyrim Skin (Fade)* variant.

### Model-space normal maps (built)

The skin slots' `_msn` maps (§1.6) are converted to tangent space by the Unity package
(`NifMergeNormalBaker`), because Unity's shaders only take tangent-space maps.

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

### Skin specular (built)

Skyrim lights skin with a Blinn-Phong specular added on top of the diffuse. It has no energy
conservation and no N·L factor (vanilla code path, read from Community Shaders' `LightingEval.hlsli`).
Per light:

- diffuse = albedo · saturate(N·L) · light colour
- specular = pow(saturate(N·H), glossiness) · specular colour · specular strength · mask · light colour

The light colour includes the shadow. The mask is the `_s` map's red channel on model-space slots
(slot 7) and the normal map's alpha on tangent-space slots. The sample skin has strength 3,
colour white and glossiness 30.

Standard cannot express this. Its specular colour is a Fresnel reflectance that is taken away from the
diffuse. Matching Skyrim's peak at glossiness 30 needs F0 = 8·strength/(n + 2) = 0.75, which would
leave a quarter of the diffuse. So skin uses the package's **Skyrim Skin** shader
(`unity/com.nifmerge.importer/Shaders`). It is a Built-in surface shader with that lighting function,
`_SkyrimSpecColor` = colour × strength (HDR, unclamped), `_Shininess` = glossiness, `_SpecMap` = the
mask, and ambient light on the diffuse only. The importer switches the `_s` map's sRGB decoding off,
because the mask is data.

**Checks (Unity harness).** Body, head and the dress's body copy use the shader, with the `_s` map
assigned, non-sRGB, and colour × strength and glossiness carried over. A flat quad with the body
material, lit along its normal and then at 40°, is rendered with and without specular, in Gamma
and in Linear colour space. The difference is compared with the formula, using the albedo and mask
read from the stored texels. In Linear, a mask that was still sRGB-decoded would read about a third
of its value (0.39 → 0.13), so this checks the import setting through the rendered result.

**Not reproduced (known gaps):**
- Rim, soft and back lighting. They read the slot-2 `_sk` map (the samples' skin sets
  `SLSF2_Rim_Lighting`). Without them, skin is darker at silhouettes than in game.
- Gamma-space lighting. Skyrim adds light in gamma space; Unity's Linear projects add it in linear
  space, so mid-tone falloff and the size of the highlight differ somewhat.
- Non-skin specular. The Standard path ignores the specular mask and strength. It also maps
  glossiness to smoothness as 1 − sqrt(2/(n + 2)). Unity's own Blinn-Phong conversion
  (`PerceptualRoughnessToSpecPower`, n = 2/r⁴ − 2 for perceptual roughness r) gives
  1 − (2/(n + 2))^¼, so the current mapping is too smooth (0.75 instead of 0.5 at n = 30).
