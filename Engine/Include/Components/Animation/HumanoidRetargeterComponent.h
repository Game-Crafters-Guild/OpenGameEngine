#pragma once

#include "AssetCore/GUID.h"
#include "Components/AssetRef.h"
#include "Types/Types.h"

#include <cstdint>

namespace GameEngine { namespace Components {

// Phase 8 LOD policy. The system picks a level per character per frame from
// distance / on-screen heuristics; an explicit per-component override forces
// the level (used by tests + crowd benchmarks). Higher levels shed more work:
//
//   Full      = Stages 1-7 + op stack (FootLock + Attachment + LookAt). Default
//               for close, on-screen characters.
//   NoOps     = skip op stack only. Saves ~50 us / character (plan §6).
//   FKOnly    = skip Stage 5 IK + op stack. Saves ~200 us / character.
//   PoseHold  = skip retarget entirely; reuse last frame's pose. Saves the
//               whole pipeline cost minus the 1 atlas write the rasterizer
//               still needs (and we leave the previous-frame palette resident,
//               so even that cost is amortized across frames).
//
// Stable values: serialized into save files, used in baseline JSON.
enum class HumanoidRetargetLOD : uint8_t
{
    Full     = 0,
    NoOps    = 1,
    FKOnly   = 2,
    PoseHold = 3,
};

// Phase 6: declarative ECS component for entities driven by humanoid
// retargeting. Per plan-v4 §2.2 the component is intentionally a flat,
// state-free record. All runtime tables (RetargetNode, GPU character slot,
// asset pointers, persistent IK scratch) live in HumanoidRetargetSystem
// keyed off entity / SkeletonRef.runtimeId — adding the component declares
// "this entity should be retargeted using this map onto this skeleton";
// the system owns everything else.
//
// Coexists with AnimatorRef + SkeletonRef. AnimatorRef supplies the
// playback time / loop / pause flags; SkeletonRef supplies the *target*
// skeleton (the one the mesh skins to). HumanoidRetargetSystem reads the
// source clip directly from ClipStore and samples it onto the source rig's
// skeleton (resolved via the RetargetMap's SourceRigRef -> HumanoidRig).
struct HumanoidRetargeterComponent
{
    // RetargetMap GUID. Authoritative reference — null GUID means the
    // entity has been bootstrapped but the asset hasn't loaded yet, in
    // which case HumanoidRetargetSystem skips it for the frame.
    Components::AssetRef<AssetType::RetargetMap> Map;

    // Source clip ID (ClipStore index, mirroring AnimatorRef::ClipIndex).
    // 0 = no clip.
    uint32 SourceClipIndex = 0; // [DoNotSerialize] ClipStore handle rebuilt from the source clip GUID at load

    // SkeletonStore runtime identity of the rig the clip's channel bone
    // indices are authored against. In a cross-rig pair this differs from the
    // target skin's skeleton, and sampling onto the target instead collapses
    // the retarget to same-rig and distorts the pose.
    //
    // For a clip that names a source model, HumanoidRetargetSystem re-resolves
    // this from that model every update and writes the result back here; 0
    // means the model has not resolved yet and the character is skipped for
    // the update. For a source-less standalone or procedural clip the value is
    // the authored source that auto-bootstrap wrote at component creation, and
    // 0 selects the target skeleton (the intentional same-rig path).
    uint32 SourceSkeletonId = 0;

    // Per-frame clock. Identical semantics to AnimatorRef::Time. The system
    // advances this when the entity is enabled and not paused.
    float32 ClipTimeSeconds = 0.0f;

    // Playback speed multiplier. Mirrors AnimatorRef::Speed.
    float32 Speed = 1.0f;

    // Phase 4b IK toggle. When false the system uploads ikSolverKind=None
    // for every chain regardless of the RetargetMap's per-chain settings.
    bool IKEnabled = true;

    // 0..1 IK blend factor. Multiplies ChainPairing.IK.BlendToSource at
    // upload time. 0 = post-FK pose; 1 = full IK.
    float32 IKBlendAlpha = 1.0f;

    // Loop flag mirror of AnimatorRef::kFlag_Loop. Stored explicitly here
    // so a HumanoidRetargeterComponent can drive an entity that doesn't
    // also carry an AnimatorRef.
    bool Loop = true;

    // Pause flag. Time advancement halts when true.
    bool Paused = false;

    // Phase 8 LOD override. When LODOverrideEnabled is true the system uses
    // LODOverride directly instead of running the heuristic. Used by the
    // crowd benchmark to force a level + by tests to assert the gating logic.
    HumanoidRetargetLOD LODOverride = HumanoidRetargetLOD::Full;
    bool LODOverrideEnabled = false;
};

} } // namespace GameEngine::Components
