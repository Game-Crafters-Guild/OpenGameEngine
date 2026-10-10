#pragma once

// Phase 4a Stage submission helpers + r.RetargetCPU=1 toggle. Glue between
// the ECS-side HumanoidRetargetSystem (Phase 6) and the GPU compute path.
//
// The CPU path (RetargetNode) is the ground truth; the GPU path runs the
// same math against the same uploaded RetargetMap. r.RetargetCPU=1 (env var)
// forces the CPU path even when GPU is available — required for offline
// tools, automated regression diffs, and editor preview without a graphics
// device.
//
// Phase 8 adds the LOD policy + per-frame stats accumulators. The stats
// helpers are deliberately process-global so the crowd benchmark + perf
// gate test can read counters without a back-channel into the system. The
// HumanoidRetargetSystem clears them at the top of every Update.

#include "Components/Animation/HumanoidRetargeterComponent.h"

#include <cstdint>

namespace GameEngine { namespace Engine { namespace Renderer {

// Returns true when the user has opted into the CPU fallback via
// r.RetargetCPU=1 (or the equivalent GE_RETARGET_CPU=1 env var). Cached
// after first read so the lookup costs nothing on hot paths.
//
// The lookup is intentionally process-scoped: changing the env var
// mid-frame requires a process restart. This matches the existing
// r.* env-var pattern used in the renderer (e.g., GE_RG_PHASE_TIMING).
bool IsRetargetCPUOnly();

// Reset the cached toggle. Used by tests to flip the env var across calls.
void ResetRetargetCPUToggle();

// Phase 8 LOD inputs. The system fills this from the entity's transform +
// camera + visibility flags before calling SelectRetargetLOD. Distances are
// world-space meters; OnScreen reflects the existing visibility system's
// classification. When the visibility system isn't available, callers can
// pass OnScreen=true and lean on DistanceMeters alone.
struct LODInputs
{
    float    DistanceMeters = 0.0f;
    bool     OnScreen = true;
    // Forces the system to skip Stage 5 IK regardless of distance. Mirrors
    // HumanoidRetargeterComponent::IKEnabled. When false, the result clamps
    // to FKOnly or above so IK never runs.
    bool     IKEnabledByComponent = true;
};

// Returns the LOD level that matches the inputs. Decision tree:
//   distance < kLODNoOpsDistance  AND on-screen  -> Full
//   distance < kLODFKOnlyDistance AND on-screen  -> NoOps
//   distance < kLODPoseHoldDistance              -> FKOnly
//   otherwise                                    -> PoseHold
// IKEnabledByComponent=false promotes NoOps -> FKOnly.
::GameEngine::Components::HumanoidRetargetLOD SelectRetargetLOD(const LODInputs& inputs);

// Per-frame accumulators. Cleared at the top of each Update by the system.
struct RetargetFrameStats
{
    uint32_t TotalCharacters = 0;
    uint32_t LODFullCount = 0;
    uint32_t LODNoOpsCount = 0;
    uint32_t LODFKOnlyCount = 0;
    uint32_t LODPoseHoldCount = 0;
    // Sum of nanoseconds spent in the per-character CPU evaluation across
    // the frame. Helps the perf gate scale tolerance with character count
    // when the workload changes.
    uint64_t CpuEvaluateNanos = 0;
    // CPU sample step (clip evaluation onto source skeleton).
    uint64_t CpuSampleNanos = 0;
    // Pose-to-skin-matrices step.
    uint64_t CpuPaletteEmitNanos = 0;
};

// Snapshot the most recent completed-frame stats. Single-threaded reader/
// writer in v1: the system writes from the render thread, tests read
// between Update calls.
RetargetFrameStats GetLastFrameStats();
void               ResetFrameStats();
void               WriteFrameStats(const RetargetFrameStats& stats);

}}} // namespace GameEngine::Engine::Renderer
