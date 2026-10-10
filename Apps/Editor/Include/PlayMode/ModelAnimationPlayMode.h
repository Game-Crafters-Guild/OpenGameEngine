#pragma once

namespace GameEngine::ECS
{
class World;
}
namespace GameEngine
{
class AssetManager;
}

namespace GameEngine::Editor
{

// For entities with Animator component: on enter play mode, either assigns the
// first embedded clip to AnimatorRef components under the root (auto-play on) or
// forces rest state (clip index 0, paused). An explicit clip source still
// loading leaves a deferred initial command; permanent failures remain at rest.
void ApplyAnimatorOnEnterPlayMode(ECS::World& world);
void ApplyAnimatorOnEnterPlayMode(ECS::World& world, AssetManager& assetManager);

} // namespace GameEngine::Editor
