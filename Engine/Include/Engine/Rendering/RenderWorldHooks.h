#pragma once

namespace GameEngine::Components
{
struct Animator;
struct MeshGPUData;
struct SkeletonRef;
} // namespace GameEngine::Components

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

namespace GameEngine::Engine::Renderer
{

/// Releases the GPUScene instance slot a MeshGPUData holds and invalidates the
/// component's index. With no GPUScene reachable (process shutdown, deviceless
/// hosting) the index is invalidated and nothing is dereferenced.
void ReleaseMeshGpuInstance(Components::MeshGPUData& meshGpu);

/// Releases the SkeletonStore runtime a SkeletonRef holds.
void ReleaseSkeletonRuntime(Components::SkeletonRef& skeleton);

/// Releases the AnimationGraphStore player and the AnimationEventCollectorStore
/// collector an Animator holds. A world holds one OnRemove hook per component
/// type, so both go through this one.
void ReleaseAnimatorRuntime(Components::Animator& animator);

/// Registers the above as World OnRemove hooks. Every world that can
/// provision these resources needs this — the primary world and the thumbnail
/// and preview worlds alike, which draw their instances from the same shared
/// GPUScene, SkeletonStore, AnimationGraphStore and AnimationEventCollectorStore.
void RegisterRenderWorldHooks(ECS::World& world);

} // namespace GameEngine::Engine::Renderer
