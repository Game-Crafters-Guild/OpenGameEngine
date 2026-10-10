# Animation — Humanoid Retargeting

This directory holds the documentation for the engine's humanoid
retargeting system. The system handles cross-rig animation playback —
"play this Mixamo walk on my Synty character without manual rework" —
across every common rig combination (Mixamo, Synty, MetaHuman, glTF,
VRM), including an A-pose source on a T-pose target.

Retargeting runs on the GPU by default: one compute kernel,
`retarget_full.comp`, writes every retargeted character's skin palette
straight into `SkinPaletteAtlas`. Setting the environment variable
`GE_RETARGET_CPU=1` before start turns the GPU dispatch off.

## Reading order

1. [`HumanoidRetargeting.html`](HumanoidRetargeting.html) — What the system
   does: assets, import, auto-bootstrap, what runs each frame, what is
   authored but not evaluated, and what it does not do.
2. [`Quickstart.md`](Quickstart.md) — Play a clip authored on another
   humanoid skeleton on your own character in five steps.
3. [`Architecture.md`](Architecture.md) — Per-frame pipeline, GPU and CPU
   paths, hot-reload chain, auto-bootstrap, LOD levels, rig import.

For example scenes:

- [`Apps/Editor/Assets/Examples/Animation/Retargeting/`](../../Apps/Editor/Assets/Examples/Animation/Retargeting/Readme.md)
  — An example scene, `CrossRigWalk` (a clip authored for one humanoid
  skeleton played on a model with another).
