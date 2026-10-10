# Humanoid Retargeting Examples

An example scene demonstrating cross-rig retargeting. It ships without a
character model or clip: to run it, import your own character and clip,
then assign them to the character's `MeshRenderer` and `Animator` in the
Inspector.

| Scene | What it demonstrates | Readme |
|---|---|---|
| `CrossRigWalk.scene` | Basic T-pose to T-pose retargeting; the silent-success case | [Readme](CrossRigWalk.Readme.md) |

## Synthetic fallback

The repo doesn't ship characters / clips (license, size). The scene's
Readme describes a synthetic equivalent that runs headlessly as a unit
test. The scene file documents the editor-side wiring; the tests prove
the math.

For headless / CI verification of the full end-to-end stack, see
`Engine/Tests/HumanoidRetargetEndToEndSmokeTest.cpp` — runs 30 frames
through `HumanoidRetargetSystem` on a synthetic rig and asserts no
exceptions, non-identity skin matrices, no NaNs.

## Related docs

- [`docs/Animation/Quickstart.md`](../../../../../../docs/Animation/Quickstart.md)
- [`docs/Animation/HumanoidRetargeting.html`](../../../../../../docs/Animation/HumanoidRetargeting.html)
- [`docs/Animation/Architecture.md`](../../../../../../docs/Animation/Architecture.md)
