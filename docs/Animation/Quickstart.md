# Humanoid Retargeting — Quickstart

Play a clip authored on another humanoid skeleton on your own character
in five steps.

This walks the auto-bootstrap path: the editor detects a clip-skeleton
mismatch, auto-imports `HumanoidRig`s for both rigs, auto-creates a
`RetargetMap` and attaches `HumanoidRetargeterComponent`, with no dialog.
Hit Play; the character walks.

---

## Step 1 — Drop the character into the project

Copy your character FBX / glTF into `Assets/Models/` (project mount).
Example: `Assets/Models/MyCharacter.glb`.

The asset pipeline imports the mesh, skeleton, and bind pose. The first
time the editor sees the file, `Animation::AutoImportHumanoidRig`
runs:

1. The bone-name regex matcher (`Animation::HumanoidNameMatcher`) maps
   each source-rig bone name onto a `HumanBone` canonical slot. Mixamo
   (`mixamorig:` prefix), Synty, MetaHuman, Maya, VRM, glTF — all covered.
2. Body proportions (hip height, shoulder width, leg / arm length) are
   computed from world-rest positions.
3. Canonical chains (LeftArm = Shoulder -> Hand, etc.) are derived.
4. Per-bone retarget-pose rotations are baked from rest-pose deltas. Synty
   A-pose binds get non-identity rotations on shoulders / upper arms;
   Mixamo / MetaHuman T-pose binds get identity (no fix-up needed).

The result lands at `Assets/Models/MyCharacter.humanoidrig.json`.

If required-bone coverage drops below `kAutoImportCoverageThreshold`
(0.8), the import fails and logs a warning with the coverage; fix the
mapping in `HumanoidRigInspector`. Otherwise: silent success.

---

## Step 2 — Drop the animation clip

Copy a clip authored on another humanoid skeleton into
`Assets/Animations/`. Example:
`Assets/Animations/Walk.glb`.

The clip's source skeleton triggers a second `AutoImportHumanoidRig`
pass — one rig per skeleton, even if many clips share a skeleton. The
result lands at `Assets/Animations/Walk.humanoidrig.json`.

---

## Step 3 — Place the character in a scene

Open or create a scene. Drop your character onto a transform entity. The
entity now carries:

- `Transform`
- `MeshRenderer` (with the character's skin mesh)
- `SkeletonRef` (target skeleton runtime)

No `Animator` yet; the character holds rest pose.

---

## Step 4 — Assign the clip

Add an `Animator` component to the entity and pick `Walk.glb` in its
**Clip** field (`Animator.clipGuid`), leaving **Auto-play in play mode**
on. When the clip's source model differs
from the entity's model and the clip's bone indices do not match the
entity's skeleton, `AnimationSystem` runs the auto-bootstrap:

1. Loads the target `HumanoidRig` (next to your character mesh),
   importing and writing it if needed.
2. Loads the source `HumanoidRig` (next to `Walk.glb`), which
   must already exist.
3. Looks for `Walk_to_MyCharacter.retargetmap.json` next to the
   target model.
4. If absent, calls `Animation::AutoCreateRetargetMap` to synthesize one
   with default chain pairings and saves it there.
5. Adds `Components::HumanoidRetargeterComponent` to the entity with
   `Map = <map GUID>`, `SourceClipIndex = <clip store index>`, defaults
   for the rest.

No dialog is shown; the log records the setup:

```
[AnimationSystem] auto-bootstrap: cross-rig clip routed via HumanoidRetargetSystem (src='...', tgt='...', map='...')
```

To change the pairing, open the map in `RetargetMapInspector` or edit the
JSON; hot reload applies the change on the next frame.

---

## Step 5 — Hit Play

The character walks. Per-frame, `HumanoidRetargetSystem`:

1. Drains hot-reload events at the top of `Update()` (so no character
   reads a torn pointer mid-frame).
2. Iterates entities with `HumanoidRetargeterComponent`.
3. Resolves the map and rigs (cached after first lookup).
4. Samples the source clip at `ClipTimeSeconds` onto the source
   skeleton.
5. By default, retargets on the GPU: one compute kernel,
   `retarget_full.comp`, dispatched by `RetargetFullPass`, writes skin
   matrices straight into `SkinPaletteAtlas`. The existing instanced
   rasterization path consumes the palette unchanged.
6. For a character the GPU path cannot take, or with
   `GE_RETARGET_CPU=1` set, evaluates `RetargetNode` on the CPU and writes
   into `SkeletonRuntimeState::CompactSkinMatrices`;
   `SkinningUploadSystem` ships them to the atlas.

`ClipTimeSeconds` advances by `deltaTime * Speed`. If `Loop = true`,
wraps at clip duration. If `Paused = true`, holds.

---

## Verifying it worked

If the character is in T-pose with arms splayed: the source rig's
retarget pose didn't bake correctly. Check `HumanoidRigInspector` —
shoulder / upper-arm rotations should be ~15-30 deg for Synty A-pose
binds, identity for Mixamo / MetaHuman T-pose binds. Use the "Force bind
= retarget pose" button if the source rig was authored canonically.

If the feet slide: the retargeter carries rotations and scales the hip
translation, and runs no foot locking or IK; a source and target with
different leg proportions slide. FootLock settings in a `RetargetMap` are
stored but not evaluated (see
[`HumanoidRetargeting.html`](HumanoidRetargeting.html#authored-but-not-evaluated)).

If the character drifts off-screen: the source clip has hip translation;
this is correct retargeting behavior. Use `Components::HumanoidRetargeter
Component::Loop` + a Position component to handle world placement.

If hot-reload regresses retargeting: edit the rig / map JSON and save.
The watcher fires `RetargetAssetWatcher`'s reload callback;
`HumanoidRetargetSystem::DrainHotReloadEvents` clears caches; next frame
re-resolves. No editor restart needed.

---

## Power-user path

Skip auto-bootstrap and add `HumanoidRetargeterComponent` manually:

```cpp
auto& retargeter = entity.AddComponent<Components::HumanoidRetargeterComponent>();
retargeter.Map = mapGuid;
retargeter.SourceClipIndex = clipStoreIndex;
```

Use this for:
- A hand-authored or hand-edited `RetargetMap`.
- Programmatic spawning (crowd scenes, replays).

`RetargetNode` runs only inside `HumanoidRetargetSystem`; animation graph
assets cannot contain it.

---

## Drop in a VRM avatar

A VRM or `KHR_humanoid` avatar imports like any other model: its bones are
mapped to the profile by name, as in Step 1. The humanoid bone map stored
in the file's glTF extension is not read by the import pipeline (see
[`Architecture.md`](Architecture.md#rig-import)).

Real VRM 1.0 sample assets live at
[vrm-c/vrm-specification samples](https://github.com/vrm-c/vrm-specification/tree/master/samples).
Check each model's license and usage conditions before downloading or redistributing it.

---

## Same-skeleton case

If you drop a clip whose skeleton matches the entity's target skeleton,
no `HumanoidRetargeterComponent` is added. The existing `AnimationSystem`
same-rig sample path runs zero-config. The component's overhead is paid
only when actual retargeting is needed.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Auto-import didn't run | Skeleton coverage < 80% required bones | Open `HumanoidRigInspector`, fix red bone slots manually |
| Character T-poses with splayed arms | Synty A-pose source, retarget pose not baked | Verify shoulder / upper-arm `RetargetPoseRotation` are non-identity in `HumanoidRigInspector` |
| Foot sliding | Source and target leg proportions differ; no foot locking runs | Use a source clip authored on similar proportions |
| Cloak / sword / hair doesn't follow | No `AttachmentBone` declared, or wrong parent | Add via `HumanoidRigInspector` with parent canonical bone |
| Clip plays once and stops | `Loop = false` | Toggle `Loop` on the component |
| Character locks at first frame | `Paused = true` | Toggle on the component |

---

## Next steps

- [`Architecture.md`](Architecture.md) — per-frame pipeline, GPU and CPU
  paths, hot-reload chain.
- [`HumanoidRetargeting.html`](HumanoidRetargeting.html) — overview of the
  assets, components, inspectors and what runs each frame.
