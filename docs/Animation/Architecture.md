# Humanoid Retargeting — Architecture

How the retargeter runs inside the engine: the per-frame pipeline, the GPU
and CPU paths, hot reload, auto-bootstrap, LOD levels and rig import. For
the feature overview, see
[`HumanoidRetargeting.html`](HumanoidRetargeting.html).

---

## Per-frame pipeline

The retargeter runs once per character per frame:

```
0. Sample          source clip -> source skeleton local pose; during an
                   AnimatorRef crossfade the clip it blends from is sampled
                   too and the two poses are blended by the crossfade weight
                   (rotations slerped, hip translation interpolated)
1. FK transport    target bones mapped to a profile bone that the source
                   also maps: tgtWorld = srcWorld * BindCorrection, where
                   BindCorrection = inverse(srcBindWorld) * tgtBindWorld is
                   precomputed when the character is built; every other
                   target bone keeps its rest rotation
2. Translation     hips: source hip translation * tgtHipHeight / srcHipHeight;
                   every other bone keeps its rest position
3. Palette emit    skin = meshRootInverse * world * inverseBind, written
                   into SkinPaletteAtlas
```

Stage 1 carries the source's motion, including any difference between the
source's bind pose and the target's, onto the target. Stage 2 scales root
motion to the target's size. Stage 3 lets the instanced skinned draw path
read retargeted palettes exactly like non-retargeted ones.

A `RetargetMap` also stores per-chain FK modes, IK settings and an op
stack; no stage reads them. See
[`HumanoidRetargeting.html`](HumanoidRetargeting.html#authored-but-not-evaluated).

---

## GPU residency

Retargeting runs on the GPU by default in one compute kernel,
`retarget_full.comp`, which `RetargetFullPass` dispatches once per frame
for every retargeted character (one workgroup per character). The kernel
runs all four stages: it samples the source clip, walks the source and
target FK chains, applies the hip translation and writes each character's
skin palette into `SkinPaletteAtlas`. `RetargetGPUDataStore` holds the
uploaded rig pairs, clips and per-character parameters, and releases a rig
pair or clip when its asset unloads.

Skinned playback without retargeting samples on the GPU in
`animation_skinning.comp`, which holds a skeleton of up to 256 bones,
counting the skin's joints and their ancestors, and 64 hierarchy levels.
A rig over either animates on the CPU instead and logs one warning naming
its model.

A character whose rig pair or clip cannot be uploaded, or which gets no
atlas slot, is evaluated on the CPU by `RetargetNode` with the same math.
Setting `GE_RETARGET_CPU=1` (or `r.RetargetCPU=1`; both are read as
environment variables) before the process starts turns the GPU dispatch
off. `RetargetGPUParityTests` compares the kernel's palettes against the
CPU `RetargetNode` within 1e-4 per matrix element, and skips when no
Vulkan device is available.

---

## Hot-reload chain

Three asset types feed the retargeter. `RetargetAssetWatcher` subscribes to
the asset manager's reload events on the first update that has an asset
manager, and queues each reload. `HumanoidRetargetSystem` drains the queue
at the top of `Update()`, before it iterates characters, so no character
sees a half-applied reload:

```
SkeletonProfile reload  -> drop every cached rig; mark every character dirty
HumanoidRig reload      -> drop that rig; mark every character dirty
RetargetMap reload      -> drop that map; mark the characters using it dirty

dirty character         -> RetargetNode rebuilt on its next update
```

Editing a `*.profile.json`, `*.humanoidrig.json` or `*.retargetmap.json`
by hand or in an inspector therefore takes effect on the next frame.

---

## Auto-bootstrap data flow

`AnimationSystem` sets up retargeting when an `AnimatorRef` plays a clip
authored on another skeleton:

```
AnimatorRef plays a clip on an entity
   |
   |  clip's source model != entity's model, and the clip's channels
   |  do not match the entity's skeleton bone indices
   v
target HumanoidRig: load <target>.humanoidrig.json, or import and write it
source HumanoidRig: load <source>.humanoidrig.json (must already exist)
   |
   v
RetargetMap: load <source>_to_<target>.retargetmap.json next to the
             target model, or create it with AutoCreateRetargetMap and save
   |
   v
AddComponent(HumanoidRetargeterComponent)   -- no dialog
   |
   v
HumanoidRetargetSystem picks up the entity on its next Update
```

Each skeleton-and-clip pair is attempted once until its assets or the
world change. Same-skeleton clips add no component and play through
`AnimationSystem`.

---

## ECS placement

- `AnimationSystem` skips every entity that carries
  `HumanoidRetargeterComponent`; `HumanoidRetargetSystem` animates it
  instead.
- When the entity also has an `AnimatorRef`, `HumanoidRetargetSystem`
  copies its `Speed`, `Loop` and `Paused` flags, advances its time, and
  writes the time back, so the Animator inspector drives retarget playback.
  During its crossfade, both clips are sampled and blended before
  retargeting, on the GPU and the CPU path alike.
- `SkinningUploadSystem` uploads the CPU palettes
  (`SkeletonRuntimeState::CompactSkinMatrices`) of characters the GPU
  kernel does not handle.

---

## LOD levels

`HumanoidRetargeterComponent` carries a `HumanoidRetargetLOD` level:

| Level | Chosen when | Effect |
|---|---|---|
| `Full` | default | retargets the character |
| `FKOnly` | `IKEnabled = false` | same as `Full`: no IK runs on any level |
| `NoOps` | only through `LODOverride` | same as `Full`: no op stack runs on any level |
| `PoseHold` | only through `LODOverride` | skips retargeting for the character this frame |

`LODOverrideEnabled = true` forces `LODOverride`. There is no automatic
distance or visibility selection.

---

## Rig import

Importing a skinned model maps its bones to the `SkeletonProfile` by name
(`AutoImportHumanoidRig`, using `HumanoidNameMatcher`), bakes the retarget
pose from the bind pose, and writes `<model>.humanoidrig.json`.
Auto-bootstrap uses the same function when a target rig is missing.

The Animation module also has bone-map readers for VRM 1.0 and 0.x
(`ImportFromVRM1`) and for `KHR_humanoid`, and a dispatcher
(`AutoImportHumanoidRigDispatch`) that tries them before the name
heuristic. The import pipeline does not call the dispatcher, so a VRM or
`KHR_humanoid` file is mapped by bone name like any other model.

When called, `ImportFromVRM1` reads the first-person mesh annotations
(`VRMC_vrm.firstPerson`), the look-at settings (`VRMC_vrm.lookAt`) and the
expression names (`VRMC_vrm.expressions`) into `VRM1Metadata`. Spring bones
(`VRMC_springBone`) are not read. Nothing in the engine consumes
`VRM1Metadata`: there is no spring-bone, expression or look-at runtime for
VRM avatars.
