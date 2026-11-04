# SkyCreature: Skyrim creatures → Unity

Goal: take a Skyrim creature (its `skeleton.nif`, its Havok `skeleton.hkx`, one or more model NIFs and
its `.hkx` animation clips) and write one skinned FBX plus one FBX per clip that Unity imports and plays.
Creatures whose skeleton maps onto Unity's Humanoid (the draugr) import as Humanoid; every other creature
(wolf, horse, dragon …) imports as **Generic**, and its clips play on it by bone path.

Status: built (milestones C0–C4, §4); the open items are in §6.

Sources of the findings: the user's vanilla SE extracts in `samples/Creatures/{Wolf,Horse,Dragon,Draugr}`
(each with its clips and a flat `textures/` folder; local only, like all of `samples/`).

---

## 1. Findings the design rests on

### 1.1 Skeletons

| Creature | Body `hkaSkeleton` | Bones | Ragdoll | NIF skeleton |
|---|---|---|---|---|
| Wolf | `NPC Root [Root]` | 50 (`Canine_*`) | `Ragdoll_Canine_COM` | `Canine_` node above `NPC Root [Root]` |
| Horse | `NPC Root [Root]` | 53 (`Horse*`) | `Ragdoll_Horse_COM01` | |
| Dragon | `NPC Root [Root]` | 84 (`NPC COM`, `NPC LLegThigh` …, wing fingers) | `Ragdoll_NPC COM01` | extra NIF-only nodes (`NPC RFootstep` …) |
| Draugr | `NPC Root [Root]` | 84 (human `NPC … [xxx]` names) | `Ragdoll_NPC COM [COM ]` | 93 nodes |

- Every file is Skyrim SE (64-bit). SkyAnim's reader handles all of them unchanged; the body skeleton is
  the one the clip's binding names.
- **Draugr** has every bone Unity's Humanoid needs except one spelling: the vanilla left foot is
  `NPC L Foot [LLft ]`, the human skeleton's is `NPC L Foot [Lft ]` (the right foot is `[Rft ]` on both).
- Wolf, horse and dragon have no Humanoid mapping (four-legged, or wings for arms).

### 1.2 Clips

- All 21 sample clips decode with SkyAnim's existing decoder: spline-compressed, THREECOMP40, 16-bit
  positions, nameless tracks bound by index or by an explicit index map, events kept.
- **Root motion is real for creatures.** `mttakeoffvertical` (dragon) carries an
  `hkaDefaultAnimatedReferenceFrame` (up (0,0,1), forward (0,1,0), one sample per frame, 121 samples)
  rising 0 → 2048 units (29.3 m). The bones themselves come back down: `NPC COM` local Z is 163 at frame 0,
  822 at frame 60 and 0 at frame 120, so the game's pose is *reference frame × bone pose* and the clip
  ends with the root at flight height, where `mtidle_hover` takes over. Each sample is (x, y, z, w):
  translation in game units and a rotation of w radians about the up axis.
- **Partial clips:** `idle_headshake` (horse, 19 tracks of 53) and `phoneme_bigaah` (dragon, 7 of 84)
  animate part of the body; the game layers them on another clip. Bones without a track hold the rest
  pose (SkyAnim's existing rule) and get no curves, so in Unity they layer the same way.
- **Loop flags.** SkyAnim measured loops on Humanoid bones only, so every creature clip came out looping
  with an error of exactly 0°. Measured on every driven bone instead, real cycles do not all end on their
  first pose: the wolf's trot is 4.1° off on its tail, the dragon's walk 5.1° on a thigh, the horse's run
  1.1° on its mane. In each case the jump from the last frame back to the first is at most 1.06 times
  that bone's largest frame-to-frame step (the cycle ends one ordinary step before its start). The clips
  that do not loop jump 30× (the phoneme) and 80× (the takeoff); the human bow release 15×. The dragon's
  bite ends in its first pose, so by pose it cannot be told from a loop.

### 1.3 Models

- Creature models are several shapes and often several files: horse = body + mane + saddle/harness; wolf
  = body + fur; dragon = body + wings; draugr variants carry their armour inside the body file, with
  helmets, hair and beards as separate files; the skeleton enemy is `draugrskeleton.nif` on `skeletons.nif`.
- Every shape checked (horse body/mane/saddle, wolf ×4, dragon, Paarthurnax, draugr male/female, skeleton
  enemy) binds to its skeleton through NifMerge's per-bone re-posing (DEV_PLAN §1.2). At rest every model
  stands on its feet; the dragon's claws curl 9.3 units below its root. A few wolf bones are
  bound up to 2.6 units off the skeleton's rest pose, and one skeleton-enemy clavicle shape has two bones
  disagreeing by about 16 units; the per-bone transform puts each vertex where the game draws it.
- Shaders seen: `BSLightingShaderProperty` types 0 (default) and 1 (environment map: horse eyes,
  Paarthurnax), and `BSEffectShaderProperty` (fire wolf, the magic draugr, the `fx*` eye glows). Model
  NIFs need no new reader code.
- Textures resolve by game path under the Unity textures root (`actors/wolf/wolf.dds`); the samples'
  `textures/` folders are flat and hold every body texture.

### 1.4 Unity

- A Generic clip binds by transform path. A clip FBX whose bone paths equal the model's plays on the model
  (SkyAnim's Generic check already proves this for characters), so clips stay separate files (**decided
  2026-11-04**).
- Generic root motion from a *motion node* is applied twice: the GameObject moves by it and the node keeps
  playing it (the dragon ended 58.5 m up, not 29.26 m; ANIM_DESIGN §1.6). The same motion as the clip's own
  `MotionT` / `MotionQ` curves, with the bones free of it, is exact, and plays in place without
  `applyRootMotion`.
- Unity refuses Skyrim's DDS files when their mip chain stops early ("Invalid mipCount: 11 provided, 12
  expected"): every creature texture in the samples does (the dragon's 4096×2048 maps have 11 of 13 levels).
- FBX reads `:` as a namespace separator: Unity imports a material named `HorseBody:0` as `0`, so the
  package's remap by name missed every slot of a shape named that way (horse body, dragon).

---

## 2. Decisions

| # | Fork | Choice and reason |
|---|---|---|
| C1 | **Where it lives** | A third exe `SkyCreature` in this repo, on `nifmerge_core` (NIF loading, re-posing, merging, materials, FBX) and `skyanim_core` (HKX decoding, binding, pose, clip FBX, batch). Its own code is the run that ties them together, its CLI and its GUI. |
| C2 | **Clip packaging** | **Decided 2026-11-04:** one FBX + `.anim.json` per clip, SkyAnim's existing output, under `<out>/anims/` (mirroring dropped folders). |
| C3 | **Rig** | `--rig auto` (default): Humanoid when the skeleton has every required Humanoid bone, else Generic. `humanoid` / `generic` force it (`humanoid` fails, naming the missing bones). The model and all its clips get the same rig, recorded in both sidecars. |
| C4 | **Humanoid aliases** | `HumanoidMap()` entries may name an alternative Skyrim spelling: `LeftFoot` accepts `NPC L Foot [LLft ]` (vanilla draugr). The first spelling the skeleton has wins. NifMerge and SkyAnim share it. |
| C5 | **Root motion** | **Decided 2026-11-04: exported.** When a clip's reference frame moves, every frame's pose is composed under the reference-frame transform (translation + yaw about up) in actor space, as the whole actor moves in game: the FBX's top-level bones carry it. The clip sidecar records `rootMotion: true` and the motion per frame. Unity (§1.4): the top-level bones' curves are rewritten without the motion; a Generic clip gets it as `MotionT` / `MotionQ`, a Humanoid clip leaves root rotation/height/XZ unbaked; such clips import without keyframe reduction (ANIM_DESIGN D14). Verified Generic on the dragon's takeoff and Humanoid on a mod's running attack (ANIM_DESIGN §5). Clips without motion export and import exactly as before. The rule is SkyAnim's too. A reference frame whose up axis is not +Z is refused. |
| C6 | **Loop flag** | Measured on every Havok bone the clip drives, not only Humanoid bones: the clip loops when, on each, the jump from the last frame back to the first is at most 0.5° or at most twice that bone's largest frame-to-frame step (§1.2). The root's motion is separate (C5) and not part of it. Applies to SkyAnim too (ANIM_DESIGN D9); the human sample clips keep their flags. |
| C7 | **Clip rig field** | Clip sidecars gain `rig`: `humanoid`, `generic`, or `either`. SkyCreature writes the creature's rig. SkyAnim writes `either` (the project setting decides, as before) when its skeleton maps onto Humanoid, else `generic`, so a creature exported with SkyAnim alone also imports correctly. |
| C8 | **Model sidecar** | `<name>.materials.json`, formatVersion 3: NifMerge's format plus `rig` (`humanoid` / `generic`) and `generator` (`NifMerge` or `SkyCreature`). NifMerge always writes `humanoid`. The Unity package reads both generators. |
| C9 | **Inputs** | Any mix of files and folders, sorted by content. A `.nif` given as a *file* is the skeleton when it holds no shapes, else a model part. Folders are searched for `.hkx` only (a creature folder holds every variant of the model, which must not all merge into one); NIFs inside dropped folders are skipped with a log line. `.hkx`: skeleton → Havok skeleton, clip → clip list. `--skeleton` / `--hkx` / `--part` override. Skeleton problems stop the run before anything is written. |
| C10 | **Output** | `<out>/<name>.fbx`, `<out>/<name>.materials.json`, `<out>/anims/…`, `<out>/anims/batch_report.json`. `<name>` is `--name`, else the first part's file name. A failing clip is reported and the rest continue (SkyAnim's batch rule); a model that cannot be made stops the run with nothing written. Clips are optional. |
| C11 | **Partial clips** | Exported as they are (bones without a track hold the rest pose and get no curves). The sidecar records `drivenBones` and `skeletonBones`; no heuristic warning. |
| C12 | **GUI** | Same host as the other two tools: Skeleton (`.nif`, `.hkx`), Model parts, Animations (files and folders), Rig (Auto / Humanoid / Generic), Name, Output folder, **Go!**, log. Drops are routed by content as in C9, onto the window or the exe icon. |
| C13 | **A .nif that is not the .hkx's skeleton** | Creature skeletons share only `NPC Root [Root]`. When no bone the clip drives below the root has a node in the `.nif`, the clip fails, naming the mismatch; driven bones without a node are otherwise left out with a warning (ANIM_DESIGN D16). |
| C14 | **Bones in the model** | NifMerge's set (weighted bones and their ancestors) plus every node the Havok skeleton animates (`MergeRequest::keepBones`), so every curve of every clip has a bone on the model (the tongue, effect and attachment nodes). |
| C15 | **DDS files Unity refuses** | The Unity package writes a copy with the mip chain completed (`NifMergeDds`): every level the file has, byte for byte, and each missing level (the smallest, a few blocks) repeating the last block of the smallest present; it goes to `Assets/NifMergeTextureFixes/<path below the textures root>` and is used in the refused file's place. NifMerge's characters get it too. |
| C16 | **Material names** | `:` becomes `_` in output material names (§1.4); the shape name is kept as it is in the sidecar. NifMerge's too. |

---

## 3. Architecture

```
inputs (files/folders) → Classify (C9): skeleton .nif, skeleton .hkx, parts, clips
  → Rig (C3/C4): Humanoid when the skeleton has every required bone, else Generic
  → Model   MergeModel(rig) → WriteFbx → WriteMaterialsJson (rig, generator SkyCreature)
  → Clips   RunBatch(rig) → per clip: Bind → Pose (root motion C5, loop C6) → clip FBX + .anim.json (rig C7)
```

Code:
- **`src/core`**: `HumanoidMap()` aliases, `FindHumanoidBone`, `MissingHumanoidBones`; `MergeRequest::rig`;
  sidecar `rig` / `generator` (`RunOptions::generator`); a skeleton `.nif` is one in which `ReadShapeSlots`
  finds no geometry.
- **`src/anim`**: reference-frame up axis and samples in `HkxClip`; root motion, the loop rule and missing
  NIF nodes in `PoseClip`; `BatchRequest::rig` and the sidecar fields.
- **`src/creature/`** (`skycreature_core`): `ClassifyCreatureInputs`, `RunCreature` (skeleton, rig, model,
  clips).
- **`src/creature_app/`**, exe `SkyCreature`:
  - `SkyCreature export [--skeleton <nif>] [--hkx <hkx>] [--part <nif>]... [--rig auto|humanoid|generic] [--name <n>] <file|folder>... --out <folder>`
  - GUI: no arguments, or files/folders dropped on the exe; `SkyCreature gui …` for tests
- **`unity/com.nifmerge.importer`**: `NifMergeImporter` imports a `rig: generic` model as Generic with an
  avatar from the model; `SkyAnimImporter` follows the clip's `rig` and converts root motion
  (`OnPostprocessAnimation`); `NifMergeDds` completes refused DDS files for `NifMergeMaterials`.

---

## 4. Milestones

| # | Deliverable | Done when |
|---|---|---|
| C0 | Shared changes: aliases, rig in `MergeModel`, loop rule, root motion, missing nodes, sidecar fields | core and anim suites green; human clips keep their loop flags |
| C1 | `skycreature_core` + `SkyCreature export` | §5 C++ checks pass on all four creatures |
| C2 | Unity: Generic model, clip rig, root motion | §5 Unity checks pass on 6000.0 and 2022.3 |
| C3 | GUI with drops | GUI output equals the CLI's, by `--go` and by real drag and drop |
| C4 | Docs | README, ANIM_DESIGN and DEV_PLAN state the new rules |

---

## 5. Verification (real files, real paths)

- **`tests/creature_tests.cpp`**: rig detection (wolf, horse, dragon Generic; draugr Humanoid through the
  foot alias, the human skeleton unchanged); every creature's model merges on its skeleton (materials, no
  Humanoid map when Generic, weights to output bones summing to 1, standing on a foot bone); the horse's
  re-posed vertices equal its file's own (its bones all imply the identity bind); forced Humanoid on the wolf
  fails naming Hips; every sample clip exports with its measured loop flag (§1.2), driven bone count, root
  motion flag and the model's rig; every bone of the takeoff equals the motion composed over the motionless
  pose, its root rises 2048 units and its FBX root node 29.26 m; a wolf clip on the horse's `.nif` is named a
  mismatch; input sorting (shapeless NIF = skeleton, folder NIFs and the behavior file skipped, the draugr
  folder's three Havok skeletons resolved by name, two skeleton `.nif` files stop the run with nothing
  written); a model-only run; forced Generic on the draugr; no `:` in material names; every clip bone is in
  its model, with the same path in both FBX files.
- **`tests/anim_tests.cpp`**: human clips keep their loop flags under the new rule; `rig: either`; no root
  motion on the sample clips; the coverage attack's root motion composed over its pose.
- **`tests/run_creature_gui_test.py`**: `SkyCreature gui … --go --exit-when-done` equals the CLI; real OLE
  drops (skeleton, Havok skeleton and model, then the clip folder) plus a click on Go! equal the CLI; a
  failing clip exits 1 with the model and the other clips written; Humanoid asked of the wolf exits 1 with
  nothing written.
- **`tests/run_unity_harness.py`** (6000.0.68f1, 2022.3.61f1): wolf, horse, dragon and draugr models and
  clips, with their real textures. Generic models import Generic with a valid avatar, the draugr Humanoid
  with a valid human avatar; every slot's diffuse texture is assigned; every clip imports with its model's
  rig, loop flag, length and events, and every transform curve binds on its model; Generic playback on the
  model equals the clip's own skeleton; the draugr's Humanoid clips keep its head at standing height; every
  diffuse is the completed DDS copy with its full mip chain; the dragon's takeoff, played through an
  AnimatorController, carries its motion in `MotionT` / `MotionQ`: with root motion it lifts the creature
  29.261 m with the root bone at rest under it and the body ending at the creature's height, without root
  motion it stays put; a screenshot of each creature.

---

## 6. Known gaps

| Gap | Size | What closing it takes |
|---|---|---|
| Textures not in the samples: Paarthurnax, the skeleton enemy, the Imperial/Stormcloak armour, cubemaps, effects | extras only; every sample body has its textures | the user's texture files |
| `BSEffectShaderProperty` shapes (glows, fire) import with Standard materials, as in NifMerge | fire wolf, magic draugr, `fx*` | an effect shader in the Unity package (out of this design) |
| A one-shot clip that ends in its first pose is flagged as a loop (the dragon's bite) | by pose it cannot be told from a cycle | the behavior graph's own loop setting (behavior graphs are out of this design) |

---

## 7. Explicitly not in this design

- **Outfits on creatures** (equipping horse saddles or draugr armour by body slot): parts are merged as
  given.
- **Behavior graphs** (`hkbCharacterData`, e.g. `draugr.hkx`, `wolf.hkx`): skipped as in SkyAnim.
- **BSA archives**: inputs are extracted files.
