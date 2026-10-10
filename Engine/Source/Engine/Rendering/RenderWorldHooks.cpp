#include "Engine/Rendering/RenderWorldHooks.h"

#include "Animation/AnimationEventCollectorStore.h"
#include "Animation/AnimationGraphStore.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Core/Engine.h"
#include "ECS/Entity.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/GPUScene.h"
#include "Logger/Logger.h"

#include <cmath>

namespace GameEngine::Engine::Renderer
{
namespace
{
// GPUScene's "unassigned" sentinel, mirrored from MeshGPUData's field defaults.
constexpr uint32 kUnassignedInstanceIndex = 0xFFFFFFFFu;

// Typed sets and the scripting ABI's immediate/deferred component byte writes
// reach this hook before their playback values can be consumed by a system.
void ClampAnimatorPlaybackInputs(ECS::EntityHandle entity, Components::Animator& animator)
{
    if (std::isfinite(animator.speedScale) && std::isfinite(animator.seekTimeSeconds)
        && std::isfinite(animator.sectionStartSeconds) && std::isfinite(animator.sectionEndSeconds))
        return;

    Logger::Log::Error("Animator playback values must be finite for entity={}: speedScale={}, seekTimeSeconds={}, "
                       "sectionStartSeconds={}, sectionEndSeconds={}. Non-finite speed is reset to 1 "
                       "and non-finite times to 0.", entity.id,
                       animator.speedScale, animator.seekTimeSeconds,
                       animator.sectionStartSeconds, animator.sectionEndSeconds);
    if (!std::isfinite(animator.speedScale))
        animator.speedScale = 1.0f;
    if (!std::isfinite(animator.seekTimeSeconds))
        animator.seekTimeSeconds = 0.0f;
    if (!std::isfinite(animator.sectionStartSeconds))
        animator.sectionStartSeconds = 0.0f;
    if (!std::isfinite(animator.sectionEndSeconds))
        animator.sectionEndSeconds = 0.0f;
}
} // namespace


void ReleaseMeshGpuInstance(Components::MeshGPUData& meshGpu)
{
    if (meshGpu.instanceIndex == kUnassignedInstanceIndex)
        return;

    auto* renderServices = EngineCore::GetInstance().GetRenderServices();
    auto* gpuScene = renderServices ? renderServices->GetGPUScene() : nullptr;
    if (gpuScene)
        gpuScene->RemoveInstance(meshGpu.instanceIndex);

    meshGpu.instanceIndex = kUnassignedInstanceIndex;
}

void ReleaseSkeletonRuntime(Components::SkeletonRef& skeleton)
{
    if (skeleton.runtimeId == 0)
        return;

    auto& store = SkeletonStore::Instance();
    // A model-backed cache copied by a raw world snapshot may outlive its
    // allocation. Never release the new occupant of that numeric slot.
    // Generation zero remains the explicit, in-memory procedural contract.
    if ((skeleton.runtimeGeneration != 0
         && skeleton.runtimeGeneration == store.GetRuntimeGeneration(skeleton.runtimeId))
        || (skeleton.runtimeGeneration == 0 && skeleton.sourceModelGuid.IsNull()))
        store.ReleaseRuntime(skeleton.runtimeId);
    skeleton.runtimeId = 0;
    skeleton.runtimeGeneration = 0;
}

void ReleaseAnimatorRuntime(Components::Animator& animator)
{
    if (animator.eventCollectorId != 0)
    {
        Animation::AnimationEventCollectorStore::Instance().Destroy(animator.eventCollectorId);
        animator.eventCollectorId = 0;
    }
    if (animator.graphRuntimeId == 0)
        return;
    Animation::AnimationGraphStore::Instance().Destroy(animator.graphRuntimeId);
    animator.graphRuntimeId = 0;
    animator.graphInstanceGuid = GUID{};
    animator.pendingGraphParamCount = 0;
}

void RegisterRenderWorldHooks(ECS::World& world)
{
    world.RegisterOnRemove<Components::MeshGPUData>(&ReleaseMeshGpuInstance);
    world.RegisterOnRemove<Components::SkeletonRef>(&ReleaseSkeletonRuntime);
    world.RegisterOnRemove<Components::Animator>(&ReleaseAnimatorRuntime);
    world.RegisterOnSet<Components::Animator>(&ClampAnimatorPlaybackInputs);
}

} // namespace GameEngine::Engine::Renderer
