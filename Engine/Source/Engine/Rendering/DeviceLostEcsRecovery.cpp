#include "Engine/Rendering/DeviceLostEcsRecovery.h"

#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/MorphTargetWeights.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"

#include "ECS/Query.h"
#include "ECS/World.h"

#include "Logger/Logger.h"

namespace GameEngine
{
namespace Engine::Renderer
{

DeviceLostEcsRecoveryReport RecoverEcsComponentHandlesAfterDeviceRebuild(
    ECS::World& world, RenderServices& renderServices)
{
    DeviceLostEcsRecoveryReport report{};
    const uint64 stampsBefore = world.GetColumnStampCount();

    // (1) Zero the procedurally-generated runtime mesh handle so MorphTargetSystem
    //     treats it as missing and re-registers a fresh morphed mesh from CPU asset
    //     data on its next tick. The registry entry still exists post-rebuild (the
    //     registry survives; only its GPU buffers died), so Find() would report the
    //     dead handle as valid and the system would skip the rebuild — zeroing the
    //     handle is what forces runtimeMissing. runtimeModelGuid is LEFT SET so the
    //     system unregisters the stale entry before re-registering. sourceMeshGpuHandleId
    //     is asset-backed (slice 4 restores it in place) and is left alone.
    // A device loss invalidates every entity's GPU state, disabled ones
    // included: their handles are just as dead, and they are re-enabled later.
    world.Query<ECS::Write<Components::MorphTargetWeights>>()
        .IncludeDisabled()
        .Each([&](ECS::EntityHandle, Components::MorphTargetWeights& morph)
        {
            if (morph.runtimeMeshGpuHandleId != 0u)
            {
                morph.runtimeMeshGpuHandleId = 0u;
                ++report.MorphRuntimeHandlesCleared;
            }
        });

    // (2) Force-dirty the render columns extraction's Changed<> probes read. A
    //     visiting Write<> query takes one NextGlobalSystemVersion() stamp per
    //     visited chunk (strictly greater than any pre-loss gate), so E5 fires and
    //     the first extraction after rendering resumes rebuilds every entity's
    //     records — including static ones a rebuild would otherwise never re-touch.
    //     MeshRenderer is the universal renderable column; LocalBounds is the other
    //     probe column an asset mesh always carries. The lambdas intentionally do
    //     not write — the Write grant stamps the chunk regardless (stamp-at-visit).
    world.Query<ECS::Write<Components::MeshRenderer>>()
        .IncludeDisabled()
        .Each([&](ECS::EntityHandle, Components::MeshRenderer&) { ++report.MeshRenderersDirtied; });
    world.Query<ECS::Write<Components::LocalBounds>>()
        .IncludeDisabled()
        .Each([&](ECS::EntityHandle, Components::LocalBounds&) { ++report.LocalBoundsDirtied; });

    report.ColumnStampsTaken = world.GetColumnStampCount() - stampsBefore;

    // (3) Belt-and-suspenders full-lane escalation. The column stamp above already
    //     trips E5 for this world; this also flags the RenderServices full-extraction
    //     hook so the first extraction after resume takes the full lane even if the
    //     probe path is bypassed (extraction feed disabled, caches unprimed). One
    //     boolean, consumed once — free at steady state.
    renderServices.RequestHlodResidencyExtraction();
    report.FullExtractionRequested = true;

    Logger::Log::Warning(
        "Q6 slice 3b: ECS component-handle recovery pass ran (world={}) — morph runtime "
        "handles cleared={}, MeshRenderer dirtied={}, LocalBounds dirtied={}, column stamps={}",
        world.GetWorldId(), report.MorphRuntimeHandlesCleared, report.MeshRenderersDirtied,
        report.LocalBoundsDirtied, report.ColumnStampsTaken);

    return report;
}

bool ShouldRunHealthyGatedDeviceRecovery(Rendering::DeviceHealth health,
                                         uint64_t rebuildGeneration,
                                         uint64_t& lastSeenGeneration)
{
    // Defer while suppressed — do NOT consume the generation, so the caller re-checks
    // and fires on the first Healthy tick after re-provision.
    if (health != Rendering::DeviceHealth::Healthy)
        return false;
    if (rebuildGeneration == lastSeenGeneration)
        return false;
    lastSeenGeneration = rebuildGeneration;
    return rebuildGeneration != 0u; // generation 0 == never rebuilt
}

} // namespace Engine::Renderer
} // namespace GameEngine
