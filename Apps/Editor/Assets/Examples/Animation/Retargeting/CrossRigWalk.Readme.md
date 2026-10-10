# Example: a walk clip played on a character with another skeleton

**Scene:** [`CrossRigWalk.scene`](CrossRigWalk.scene)

A clip authored for one humanoid skeleton plays on a model with another.

**Demonstrates:** Basic T-pose to T-pose retargeting. The auto-bootstrap
path handles this case silently; no manual setup required.

## What's in the scene

- Directional sun light (100,000 lux) driven by the sky's time of day
  (14:00 at latitude -45, behind the camera); the camera uses auto
  exposure with +1 stop of compensation; a post-process volume tonemaps.
- 500x500 ground plane, and a thin `Stand` slab at the origin marking
  where the character stands.
- One target character entity (`TargetCharacter`) with:
  - `MeshRenderer` with no model assigned.
  - `Animator` set to play a clip, with no clip assigned.

  It has no `HumanoidRetargeterComponent`: the auto-bootstrap adds one
  the first time a clip authored on another skeleton plays.
- A camera at (0, 1.6, 3) looking at the character.

## Running it live

The scene ships without a character model or clip. Assign your own:

1. Drop a humanoid character mesh into your project's
   `Assets/Models/`. When the mesh first loads (at the latest when the
   scene that uses it opens), the importer writes its rig next to it as
   `<meshname>.humanoidrig.json`, in lower case.
2. Drop a `.glb` walk clip authored on a different humanoid skeleton
   into `Assets/Animations/`. Its rig,
   `<clipname>.humanoidrig.json`, is written next to it when the clip
   first loads, at the latest on the first Play.
3. Open `CrossRigWalk.scene` in the editor.
4. Select the `TargetCharacter` entity. In the Inspector:
   - Set the `MeshRenderer` **Model** field to your character mesh.
   - Pick your clip in the `Animator` **Clip** field
     (`Animator.clipGuid`), leaving **Auto-play in play mode** on.
     The Animator resolves the clip into the runtime `AnimatorRef` that
     plays it.
5. Hit Play. The first time the clip plays, the animation system sees
   that it was authored on another skeleton and sets up retargeting with
   no dialog: it loads `<Source>_to_<Target>.retargetmap.json` next to the
   character mesh, or creates and saves it, and adds a
   `HumanoidRetargeterComponent` that uses it. The log records each step:

   ```
   [AnimationSystem] auto-bootstrap: cross-rig detected (...)
   [AnimationSystem] auto-bootstrap: created RetargetMap '<path>'
   [AnimationSystem] auto-bootstrap: cross-rig clip routed via HumanoidRetargetSystem (...)
   ```

   The `created RetargetMap` line appears only when the map file did not
   exist. When it already exists it is loaded, and the log shows
   `rewriting stale retargetmap` only if the map names rigs other than the
   two in use.

6. The character walks in place.

## What this scene proves

The headline case the auto-bootstrap was designed to make zero-config:
- T-pose source bind, T-pose target bind -> identity retarget poses on
  both `HumanoidRig`s.
- Required-bone coverage clears 80% on both rigs (the regex matcher
  covers the common humanoid bone naming schemes).
- Default `RetargetMap` (FK rotation OneToOne, translation None, IK off,
  empty op-stack) produces correct visual output.

If the character doesn't move correctly out of the box on this case, the
matcher or auto-import is broken. Use this scene as the canary.

## Synthetic fallback

The repo doesn't ship a character mesh or clip (license, size).
If you don't have your own:

- For a synthetic equivalent, see
  `Engine/Tests/HumanoidRetargetSystemTests.cpp` —
  `EndToEnd_CPUEvaluatePopulatesSkinMatrices` builds two synthetic
  `HumanoidRig`s in code and runs the same pipeline headlessly. The
  scene is the editor-side equivalent.

## Related docs

- [`Quickstart.md`](../../../../../../docs/Animation/Quickstart.md)
- [`HumanoidRetargeting.html`](../../../../../../docs/Animation/HumanoidRetargeting.html)
