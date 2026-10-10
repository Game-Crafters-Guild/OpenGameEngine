#pragma once

namespace GameEngine
{
namespace ECS { class World; }
namespace Engine::Renderer { class RenderServices; }

namespace Editor
{

// Seeds a freshly-cleared world with a basic set of entities so the scene
// view is immediately usable: directional light, sky environment, main camera,
// global post-process volume, ground plane, and two spheres (one with the
// mirror material that shows the environment it reflects). It seeds no
// reflection probe: a new scene's ambient and reflections come from the sky.
void SeedDefaultSceneEntities(ECS::World& world, Engine::Renderer::RenderServices* renderServices);

} // namespace Editor
} // namespace GameEngine
