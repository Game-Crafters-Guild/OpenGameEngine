#pragma once

#include "Types/Types.h"

#include <cstdint>

namespace GameEngine
{
namespace Rendering { enum class DeviceHealth : std::uint8_t; }
namespace ECS { class World; }
namespace Engine::Renderer
{
class RenderServices;

// Counts published by RecoverEcsComponentHandlesAfterDeviceRebuild so the caller
// can log a single observable line proving the pass ran and how much it touched.
struct DeviceLostEcsRecoveryReport
{
    uint32 MorphRuntimeHandlesCleared = 0; // MorphTargetWeights runtime meshes zeroed
    uint32 MeshRenderersDirtied = 0;       // entities whose MeshRenderer column was stamped
    uint32 LocalBoundsDirtied = 0;         // entities whose LocalBounds column was stamped
    uint64 ColumnStampsTaken = 0;          // world-wide write-grant stamps this pass took
    bool FullExtractionRequested = false;  // RenderServices full-lane hook flagged
};

// Q6 slice 3b — the ECS-wide component-handle recovery pass (design §8b, F2).
//
// After an in-place device rebuild, RenderServices::OnDeviceRebuilt (slice 3a)
// re-provisions the RenderServices-owned GPU layer, but two classes of stale
// state remain unreachable by that fan-out:
//   1. GPU handles cached ON ECS components. This pass owns the procedurally
//      generated ones the core engine can reach: MorphTargetWeights runtime
//      meshes (zeroed here so MorphTargetSystem re-creates them from CPU asset
//      data). Asset-backed MeshRenderer.meshGpuHandleId is deliberately LEFT
//      ALONE — slice 4 restores the registry entries in place, keeping those
//      generational handles valid. EZTree (a separate module) and HLOD proxies
//      (a scene-layer concern) run their own generation-polled recovery.
//   2. Change-gated extraction won't re-fire for static entities: a rebuild
//      dirties no ECS column, so RenderExtractionSystem's Changed<> probes skip
//      every unchanged chunk and keep replaying stale submissions. This pass
//      force-dirties the render columns (a visiting Write<> query stamps each
//      chunk with a fresh, strictly-greater global version), so the first
//      extraction after rendering resumes takes the full lane for EVERY entity.
//
// Runs on the thread that owns the World (the main/update thread) while the
// device is AwaitingReprovision — after slice 3a re-provision, before extraction
// resumes. Idempotent; cheap (one-time per rebuild). Returns what it touched.
DeviceLostEcsRecoveryReport RecoverEcsComponentHandlesAfterDeviceRebuild(
    ECS::World& world, RenderServices& renderServices);

// Q6 slice 3b — health-gated generation poll for recovery steps whose GPU work must
// NOT run while rendering is suppressed. Returns true (and advances lastSeenGeneration)
// only when the device is Healthy AND its rebuild generation moved since the last
// accepted run. While the device is not Healthy this DEFERS: it returns false WITHOUT
// consuming the generation, so the step fires on the first Healthy tick after
// re-provision completes.
//
// This gate is load-bearing for HLOD proxy recovery: HlodRuntime::ReconcileScene ->
// RegisterSubmesh -> UploadMesh writes through MeshGPURegistry's persistently-mapped
// VMA pools, which an in-place device rebuild frees and only slice 4 re-provisions
// (RenderServices::OnDeviceRebuilt does not touch the registry). Running it during
// AwaitingReprovision is an upload-time use-after-free. EZTree's generation poll needs
// no explicit gate — it runs inside the render extraction phase, which RenderingLoop
// already suppresses until the device is Healthy.
bool ShouldRunHealthyGatedDeviceRecovery(Rendering::DeviceHealth health,
                                         uint64_t rebuildGeneration,
                                         uint64_t& lastSeenGeneration);

} // namespace Engine::Renderer
} // namespace GameEngine
