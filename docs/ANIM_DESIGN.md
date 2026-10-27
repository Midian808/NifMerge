# SkyAnim: Skyrim HKX → FBX animation exporter

Goal: take Skyrim animation clips (`.hkx`, LE or SE) plus the character's skeleton (`skeleton.hkx` +
`skeleton.nif`), and write FBX animation that plays correctly in Unity on the characters NifMerge
produces. Each clip can be used as a Humanoid clip on any Humanoid character, and as a Generic clip on a
Generic-rigged character built from the same skeleton.

Status: built (milestones A0–A6, §4); the open items are in §6.

Sources of the findings:
- the user's samples in `samples/animationwork/{LE,SSE}`: 7 clips, 2 Havok skeletons, 2 skeleton NIFs
- a `SkyAnim scan` of the installed mod folders: about 19,600 real clips, every one decoded
- real coverage fixtures copied from those mods under neutral names into `samples/animationwork/coverage`
  (local only, like all of `samples/`)

---

## 1. Findings the design rests on

### 1.1 File format

| | Skyrim LE | Skyrim SE |
|---|---|---|
| Container | Havok binary packfile `hk_2010.2.0-r1`, 32-bit (pointer size 4) | same version, **64-bit** (pointer size 8) |
| Base object / array / pointer size | 8 / 12 / 4 bytes | 16 / 16 / 8 bytes |
| Compressed animation data | identical byte format | identical byte format |

- A packfile has three sections: `__classnames__`, `__types__` (empty) and `__data__`. Pointers inside
  `__data__` are resolved through its *local*, *global* and *virtual* fixup tables. The virtual fixups
  give every object's class name, so no Havok type information is needed.
- Member offsets used (each pointer member is confirmed against the fixup tables; the reader rejects a
  non-zero pointer that has no fixup, so a wrong layout fails loudly):

  | Class | 32-bit | 64-bit |
  |---|---|---|
  | `hkaSkeleton` | name 8, parentIndices 12, bones 24, referencePose 36 | 16, 24, 40, 56 |
  | `hkaAnimation` (base) | type 8, duration 12, tracks 16/20, extractedMotion 24, annotationTracks 28 | 16, 20, 24/28, 32, 40 |
  | `hkaSplineCompressedAnimation` | numFrames 40 … frameDuration 64; blockOffsets 68, floatBlockOffsets 80, data 116 | 56 … 80; 88, 104, 152 |
  | `hkaInterleavedUncompressedAnimation` | transforms 40 | 56 |
  | `hkaAnimationBinding` | originalSkeletonName 8, animation 12, transformTrackToBoneIndices 16, blendHint 40 | 16, 24, 32, 64 |
  | `hkaDefaultAnimatedReferenceFrame` | samples 52 | 56 |
  | annotation track / annotation | 16 bytes (name, array) / 8 bytes (time, text) | 24 / 16 |

- A Skyrim `skeleton.hkx` holds two `hkaSkeleton`s, the body (`NPC Root [Root]`) and the ragdoll
  (`Ragdoll_NPC COM [COM ]`), plus physics objects. The body skeleton is the one whose name equals the
  binding's `originalSkeletonName`.
- 38 of the scanned mod files are Havok **XML tagfiles** (`<?xml … <hktagfile>`), not binary packfiles
  (§6).

### 1.2 Spline compression

- A clip is split into blocks of `maxFramesPerBlock` (256) frames. Neighbouring blocks share one frame,
  so block *b* starts at frame *b*·255.
- Each block starts with one 4-byte mask per transform track: a quantization byte (position bits 0–1,
  rotation bits 2–5, scale bits 6–7), then position, rotation and scale type flags (static X/Y/Z/W in
  the low nibble, spline X/Y/Z/W in the high nibble). Track data starts `maskAndQuantizationSize` bytes
  into the block (aligned to 4).
- Each track then stores position, rotation and scale in turn:
  - **Spline component:** u16 control-point count *n*, u8 degree, *n*+degree+2 u8 knots, then (after
    aligning to 4) a float min/max per spline axis or one float per static axis, then (*n*+1)
    quantized control points.
  - **Static component:** one float per static axis, or one quantized quaternion for a rotation.
  - **Neither:** identity (position 0, rotation identity, scale 1).
  - Every track component ends 4-byte aligned. Rotations align to their format's own alignment first.
- Values are evaluated by de Boor's algorithm at the frame's local position in its block. Rotations are
  renormalised after interpolation.
- Each block's transform data must end exactly at the block's `floatBlockOffsets` entry; the decoder
  checks this on every block of every clip. It held on all 7 samples and on all ~19,600 scanned mod clips.
- **Rotation formats in real data:**
  - **THREECOMP40**: 3 × 12 bits, then 2 bits naming the missing component and 1 sign bit (all LE clips,
    most SE clips).
  - **THREECOMP48**: 3 × u16; the missing-component index is spread over the top bits of x and y, the
    sign is in the top bit of z (about 880 SE clips).

  For both, every decoded component is less than or equal to the reconstructed one, and all four
  missing-component slots are confirmed by static tracks that match the reference pose.
- **Positions and scales:** 16-bit quantization everywhere.
- **Never seen** in the samples or the ~19,600 scanned clips: the rotation formats POLAR32, THREECOMP24,
  STRAIGHT16 and UNCOMPRESSED, and 8-bit positions and scales (§6).

### 1.3 Which track drives which bone

| Seen in | Track names | Binding indices | Track count vs skeleton | Correct binding |
|---|---|---|---|---|
| LE `mt_idle` | present | none | 242 tracks vs 115 bones (authored for a bigger physics skeleton) | by name |
| LE walk / run / bow | present | none | 107 tracks (8 breast/butt bones inserted after index 41) | by name |
| All 3 SE samples | **null** | none | 99 tracks = vanilla bone order = first 99 bones | by index |
| 29 scanned SE mod clips | present | **explicit** | e.g. 74 tracks | by name |

- Names must match **case-insensitively**: tracks are `SHIELD`/`WEAPON`/`QUIVER`, bones are
  `Shield`/`Weapon`/`Quiver`.
- **An explicit index map only holds on the skeleton it was authored for.** In 15 of the 27 explicit-map
  clips of one mod, every index names the track's own bone on the user's skeleton. In the other 12, the
  indices of the breast tracks (`NPC L PreBreast` …) land on `NPC L/R Item01–03`: they were written for a
  skeleton variant with breast bones at those slots. A name holds on every variant.

### 1.4 Clip content

- **Events** are annotations, all on track 0:
  - `bow_release`: `arrowRelease` at 0.033 s, `SoundPlay.WPNBowFire` at 0.1 s
  - walk/run: `FootLeft`, `FootRight`
  - LE walk: one event whose text is **empty**
- **Root motion:** no clip has any forward motion. The LE clips contain an all-zero
  `hkaDefaultAnimatedReferenceFrame`; the SE clips contain no root-motion object; Root and COM never move
  forward. Vanilla keeps locomotion speed in the behavior project's animation data. **Decided
  2026-10-27: root motion is out; walk/run export in place.**
- **Scale:** LE `mt_idle` stores junk animated scale (X only, drifting 4.5 → 11 → −1.0; its header
  `type` is also 3 instead of 5). The other samples store exactly 1.0.
- **Additive clips** (6 scanned, all on the Schlongs of Skyrim aux skeleton `NPC GenitalsBase`):
  `blendHint` 1; frame 0 is identity and later frames are offsets, with genuine non-uniform scale on the
  bulge bones (1.2–1.4).
- **Float tracks** (331 scanned clips, e.g. 4 per clip): they drive the skeleton's float slots, not bones.
  The transform data around them decodes exactly.
- **Uncompressed clips** (`hkaInterleavedUncompressedAnimation`, 16 scanned): transforms stored
  frame-major; cyclic clips step at most 4.5° per frame, and 14–20 still bones per clip sit at the
  reference pose.

### 1.5 skeleton.hkx vs skeleton.nif

- The XPMSE `skeleton.nif` inserts helper nodes: `CME …` (identity) between the real bones, and `MOV …`
  (non-identity weapon placement) above the weapon slots. As a result, NIF parents differ from Havok
  parents for 78 bones.
- At rest, the Havok reference pose and the NIF bind pose agree (body chain within 0.08°) except:
  - the NIF COM is 0.35 units off in Y
  - the hands differ by about 4.7°
  - the fingers and the weapon/magic nodes differ by up to 2.4 units
- **SE vs LE:** the SE skeleton is the LE skeleton plus 11 appended bones (`Belly`, 10 lower-body physics
  bones). The SE NIF is the LE NIF plus 28 nodes, and every shared node is identical. The NifMerge sample
  character's `samples/skeleton_female.nif` is node-for-node identical to the SE NIF.
- `characters/defaultfemale.hkx` is `hkbCharacterData`, not a skeleton. It holds the rig path, about
  1552 animation paths and a 99-bone mirror map.

### 1.6 Unity

- An Animator with a **Humanoid avatar** drives the human bones through muscles only: a Generic clip's
  transform curves on those bones are ignored and the body falls to the zero-muscle pose. Generic clips
  therefore play on Generic rigs (an Animator without a Humanoid avatar).
- A Humanoid clip keeps only muscle curves unless its clip mask names other transforms.
- Unity resets a model's clip settings when its animation type changes in the same reimport.
- Unity gives an animated FBX's model the pose of the animation's **first frame** as its default pose
  (exactly: 0.000 m from frame 0), not the FBX rest pose; for the bow release that is up to 1.2 m from
  rest. An avatar built from that pose disagrees with the NifMerge character's, which is built from the rest
  pose: retargeted onto the character, the walk was off by up to 9 cm and the bow release by up to 36 cm.
  Muscle limits play no part (widened ×2.5, no muscle near its limit, the deviation unchanged). With the
  avatar built from the rest pose (D8), Humanoid playback on the character equals the clip on its own model:
  within 1.6 cm (walk) and 2.6 cm (bow release) of exact Generic playback, which is Unity's own keyframe
  reduction on import.

---

## 2. Decisions

| # | Fork | Choice and reason |
|---|---|---|
| D1 | **Where it lives** | **Decided 2026-10-27:** a separate exe `SkyAnim` in this repo, sharing `nifmerge_core`. It reuses `LoadSkeleton` (NifIO), `Xform`/`Mat3`/`Quat` (Math.hpp), the Skyrim → FBX conversion (FbxCommon.hpp), `HumanoidMap()`, the JSON helpers (TextUtil), the Win32/D3D11/ImGui window (`src/app/GuiHost`), the Unity package, and the test harnesses. |
| D2 | **Track binding** | **Named tracks bind by name** (case-insensitive). **Nameless tracks** use the clip's explicit index map when it has one, otherwise track *i* → bone *i*. A named track with no bone in the Havok skeleton is dropped and reported (the engine can only drive skeleton bones). Named tracks whose explicit index names a different bone are counted and reported. Nameless tracks beyond the bone count are a hard error. Reason: §1.3 — each other rule mis-binds a real clip. |
| D3 | **How the pose reaches the NIF hierarchy** | Copy Havok-local transforms onto the NIF nodes of the same name and compose through the NIF hierarchy, as the game does (XPMSE's identity `CME` nodes exist so that copied locals still compose correctly). Havok bones without a track use the Havok reference pose; NIF-only nodes keep their NIF rest transform. |
| D4 | **Scale** | Ignore animated scale; each NIF node keeps its own scale (e.g. the XPMSE hand's 0.852), then the skeleton is written scale-free with `Xform::rigid()`, NifMerge's convention. The mesh is built from the NIF rest pose including that scale, so the animation must use the same scale or hands and fingers detach. It also neutralises LE `mt_idle`'s junk scale. |
| D5 | **Output form** | One FBX per clip, skeleton only (no mesh), named after the clip, plus a sidecar `<clip>.anim.json`. |
| D6 | **Bone set in the FBX** | NIF nodes driven by a Havok bone, plus every Humanoid bone, plus all their ancestors below the NIF file root — `Merge`'s `keepChain` rule — so every bone path matches a NifMerge character built from the same `skeleton.nif`. Bones get curves when driven, or when their local transform changes anyway. |
| D7 | **Events** | Sidecar → Unity package, which writes `ModelImporterClipAnimation.events` (`functionName = "OnSkyrimEvent"`, `stringParameter` = the event text). Empty-text events are dropped and logged. |
| D8 | **Unity rig type** | The FBX is the same either way; the package sets it from a project setting. **Default Humanoid**: the NifMerge mapping, the avatar built from the rest pose (the sidecar's rest transforms, since Unity gives the model the clip's first frame; §1.6) with a T-pose reference, root rotation/height/XZ baked into the pose (in place), and a clip mask with every transform so twist, physics and weapon bones animate. The rig is imported first and the clip configured on top (§1.6). **Generic**: exact playback on a Generic-rigged character built from the same skeleton. |
| D9 | **Loop flag** | Measured: loop when the first and last frames agree within 0.5° on every Humanoid bone the clip drives. |
| D10 | **Rotation curves** | Per-frame Euler keys from the rigid local matrix, linear, then `FbxAnimCurveFilterUnroll` removes ±180° flips. |
| D11 | **Additive clips** | **Refused** with a clear message. They hold offsets the game layers onto another animation; which side of the base pose Havok composes them on is not recorded in the file, and a guess would export a wrong-but-plausible pose (§1.4, §6). |
| D12 | **Float tracks** | Decoded around, not exported: they drive skeleton float slots, which have no FBX/Unity bone. The clip exports with a note. |
| D13 | **Rotation formats never seen** | POLAR32 and UNCOMPRESSED are decoded and reported as unverified; THREECOMP24 and STRAIGHT16 are refused, since neither has a published bit layout nor a real sample to derive one from. 8-bit vectors are decoded and reported as unverified. |

---

## 3. Architecture

```
inputs: files and folders (skeleton.nif, skeleton .hkx and clips recognised by content)
  → Packfile     header → pointer size 4/8 → sections → local/global/virtual fixups; refuse anything that
                 isn't a hk_2010.2.0-r1 binary packfile (XML files named as such)
  → Objects      layout tables per pointer size: hkaSkeleton, hkaAnimationBinding, annotation tracks,
                 hkaSplineCompressedAnimation, hkaInterleavedUncompressedAnimation, reference frame
  → Decode       spline blocks, each checked against its recorded end offset → per frame, per track:
                 translation, quaternion, scale
  → Bind         D2: track → Havok bone; dropped tracks and index/name conflicts listed
  → Pose         per frame: Havok locals copied onto NIF nodes (D3), NIF node scale kept (D4), world
                 composed through the NIF hierarchy, then rigid(); additive clips refused (D11)
  → Write FBX    shared Skyrim→FBX conversion (Y-up metres, (x,y,z)→(−x,z,y)×0.0142875); bone set D6;
                 one take named after the clip at the file's frame rate; Euler unroll (D10)
  → Sidecar      <clip>.anim.json: source, platform, skeleton files, frames, fps, duration, loop (D9),
                 events (D7), binding mode, dropped tracks, encodings, Humanoid map, each bone's rest
                 transform (FBX space) for the Humanoid avatar (D8)
```

Code:
- **`src/anim/`** (`skyanim_core`, linked with `nifmerge_core`): `HkxPackfile` (sections, fixups),
  `HkxAnimation` (skeletons, clips, spline and interleaved decoding, classification), `ClipPose`
  (binding and pose), `AnimExport` (FBX and sidecar), `Batch` (batch mode, §3.1).
- **Shared with NifMerge:** `src/core/FbxCommon.hpp` (Skyrim→FBX conversion, FBX session, bone nodes),
  `Quat` in `Math.hpp`, `JsonQuote`/`JsonNum` in `TextUtil`, and `src/app/GuiHost` (the window, render
  loop, drops and file dialogs both GUIs run in).
- **`src/anim_app/`**, exe `SkyAnim`:
  - `SkyAnim export [--skeleton <nif>] [--hkx <skeleton.hkx>] <file|folder>... --out <folder>`
  - `SkyAnim inspect <file.hkx>`: skeletons, or a clip's tracks, binding, encodings, events
  - `SkyAnim scan <folder> [--list <encoding>]`: encodings and classes across a folder; `--list` prints
    every file using one (how the coverage fixtures were found)
  - `SkyAnim dump <clip.hkx> --out <file.tsv>`: decoded transforms (the decoder oracle's input)
  - GUI (no arguments, or files/folders dropped on the exe; `SkyAnim gui …` for tests): skeleton fields,
    clip list, output folder, **Go!**, log
- **`unity/com.nifmerge.importer`**: `SkyAnimImporter` (an `AssetPostprocessor` for an FBX with a SkyAnim
  `.anim.json` beside it), `AnimSidecar`, and the *SkyAnim animations import as* project setting
  (Humanoid / Generic).

### 3.1 Batch mode

A run takes any mix of files and folders. The behavior character file is not needed.

- **Inputs:** files and folders dropped onto the GUI window or the exe icon, or listed on the CLI.
  Folders are searched recursively for `.hkx` and `.nif` files (extension case-insensitive).
- **Classification by content:**

  | Content | Goes to |
  |---|---|
  | a body `hkaSkeleton` | the Havok-skeleton slot |
  | `hkaSplineCompressedAnimation` / `hkaInterleavedUncompressedAnimation` | the clip list |
  | `hkbCharacterData`, behavior graphs, anything else | skipped, with a log line naming its root class |
  | an unreadable `.hkx` | the clip list, where it fails with its reason |

  A `.nif` goes to the skeleton-NIF slot. More than one skeleton `.hkx` or `.nif` stops the run, naming
  the candidates; in the GUI the user picks one with Browse, on the CLI `--skeleton` / `--hkx` settle it.
  With only a `.nif`, a same-named `.hkx` (or `skeleton.hkx`) beside it is used. In the GUI, skeleton files
  dropped as files fill the skeleton fields; dropped folders and clips join the inputs.
- **LE and SE mix freely**; the platform is detected per file.
- **Output:** one `<clip>.fbx` and `<clip>.anim.json` per clip. Clips found under an input folder keep
  their path relative to it (`Animations\female\MT_Idle.hkx` → `<out>\female\MT_Idle.fbx`); loose files
  go to the output root. Clips that would land on the same output path both fail; nothing is overwritten.
- **Failure isolation:** a failing clip is logged with its reason and the batch continues. Skeleton
  problems stop the run before any clip is written.
- **Report:** log pane / console and `<out>\batch_report.json` (exported, failed with reasons, warnings
  per clip — dropped tracks, binding conflicts, float tracks, unverified encodings, root motion — and
  skipped files). The CLI and `--exit-when-done` exit non-zero if any clip failed.

---

## 4. Milestones

| # | Deliverable | Done when |
|---|---|---|
| A0 | Shared helpers: `FbxCommon.hpp`, `Quat`, JSON helpers in `TextUtil`, `GuiHost` | NifMerge's suites stay green; its FBX summary and sidecar are byte-identical to before |
| A1 | `HkxPackfile` + objects for both layouts; `SkyAnim inspect` | samples and coverage fixtures read; every pointer member has a fixup |
| A2 | Spline and interleaved decoding; `SkyAnim scan` / `dump` | §5 decoder checks pass; the C++ decode equals the Python oracle |
| A3 | Binding + pose (D2–D4, D11) | §5 binding and pose checks pass |
| A4 | FBX writer + sidecar; `SkyAnim export` with batch mode | FBX read-back equals the computed poses; §5 batch checks pass |
| A5 | Unity `SkyAnimImporter`; harness extension | clips import and play on a NifMerge character (Humanoid and Generic) with correct bones, events, loop flags, in place |
| A6 | GUI with drops on the window and the exe | GUI output identical to the CLI's, by `--go` and by real drag and drop |

---

## 5. Verification (real files, real paths)

- **`tests/anim_tests.cpp`** (`build\tests\Release\anim_tests.exe`):
  - packfile layouts (LE 4, SE 8), the character file classified as `hkbCharacterData`, a `.nif` refused
  - skeletons: LE 115 + ragdoll 18; SE 126, whose first 115 bones equal LE's
  - every sample clip: frames, tracks, duration, encodings, no unverified encoding, events (times and
    text), unit quaternions
  - still tracks of rest bones equal the reference pose (pins each rotation encoding's bit layout);
    block seams continuous
  - binding: SE by index, LE by name with the dropped counts (127 for LE `mt_idle`, 8 for the others),
    case-only matches, an explicit map validated and range-checked
  - coverage fixtures: an explicit map whose every index names the track's own bone; one made for another
    skeleton variant (breast tracks kept off the Item bones); an uncompressed clip (smooth, still bones at
    rest, a mounted turn that does not loop); float tracks exported with a note; the additive clip refused;
    the XML tagfile named as such
  - pose: Humanoid bones present, rest pose = the NIF's rigid bind pose, unreachable nodes unmoved, toes on
    the ground, head at standing height, loop flags; a reference-pose frame reproduces the Havok reference
    pose through the NIF hierarchy
  - FBX read-back (LE and SE walk): every bone at every frame within 1e-4 m and 0.01°; sidecar contents
  - batch: a mixed folder (SE skeleton, SE and LE clips, a nested second `mt_idle`, the character file, a
    truncated clip): classification, mirrored output, the truncated clip failing alone, the report; two
    skeleton `.nif` files stop the run before writing; colliding loose files both fail
- **`tests/run_anim_oracle.py`**: an independent Python decoder (`tests/hkx_oracle`) decodes every sample
  clip, asserting block end offsets, the smallest-three property and block seams; `SkyAnim dump` must
  equal it per frame, track and component.
- **`tests/run_unity_harness.py`** (6000.0.68f1 and 2022.3.61f1): SkyAnim exports the SE walk, run, idle
  and the LE bow for the NifMerge sample character's skeleton. Checks, through the automatic import:
  Humanoid import, valid human avatar, clip name, length, loop flag, events (function, text, time), twist
  bone curves kept; Generic copies imported as Generic. Playback through an Animator `PlayableGraph` on the
  NifMerge character: Generic (avatar-less) equals the clip's own skeleton exactly, every curve binds, toes
  on the ground, head at standing height. The sidecar's rest pose, converted to Unity space, equals the
  character's rest pose bone for bone. Humanoid plays in place and, retargeted onto the character, stays
  within 1 cm of the same clip's error on its own model (§1.6); screenshots of both.
- **`tests/run_anim_gui_test.py`**: `SkyAnim gui <folder> --go --exit-when-done` equals the CLI; real OLE
  drops (skeleton files, then the folder) plus a click on Go! equal the CLI with the skeleton named; a
  failing clip gives exit code 1 with the other clips exported. It moves the mouse.

---

## 6. Known gaps

| Gap | Size | What closing it takes |
|---|---|---|
| **FBX Euler rest on bones at ±90° pitch**: `FbxAMatrix::GetR` loses about 0.13° on the skirt roots (`SkirtRBone01`, `SkirtLBone01`), in SkyAnim's FBX and NifMerge's character FBX alike (the shared `AddBoneNode`); the sidecar's rest pose is exact. | 2 non-Humanoid bones | Write those nodes' rest rotation without going through Euler decomposition (e.g. a pre-rotation), in `AddBoneNode`. |
| **XML tagfiles** (`<hktagfile>`) are not read. | 38 of ~19,600 scanned mod files | A second reader for Havok's XML tagfile format. Not in this design's scope (binary packfiles, which is what Skyrim loads); refused with a message naming the format. |
| **Additive clips** are refused (D11). | 6 scanned clips, all on an aux skeleton | Ground truth for Havok's additive composition order (e.g. an in-game capture of a known base + additive pose). |
| **POLAR32 and UNCOMPRESSED rotations and 8-bit vectors** are decoded but unverified; **THREECOMP24 and STRAIGHT16** are refused (D13). | never seen in ~19,600 clips | A real clip using each; `SkyAnim scan --list` finds one when it exists. |
| **Float tracks** are not exported (D12). | 331 scanned clips | A consumer for skeleton float slots on the Unity side (none exists). |

---

## 7. Explicitly not in this design

- **Root motion:** decided out (§1.4).
- **Behavior graphs** and **batch export driven by `hkbCharacterData`**: not asked for; inputs are given
  directly.
- **Writing `.hkx`** (FBX → Skyrim): not asked for.
