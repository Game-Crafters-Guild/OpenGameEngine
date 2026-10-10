#pragma once

namespace GameEngine
{
namespace ECS
{
class World;
} // namespace ECS
} // namespace GameEngine

namespace GameEngine::Engine::Renderer
{

// Expands `userFarClip` so an enabled Spherical (planet) terrain's bounding sphere fits in
// view (>= ~4 x (PlanetRadius + PlanetReliefAmplitude)), never shrinking below the user's
// setting. Returns `userFarClip` unchanged when the world has no enabled spherical terrain.
//
// This is the ONE derivation shared by the editor Scene View (SceneViewController) and the
// game / Player camera (CameraSystem), so both frame a planet identically — a planet at
// radius R needs the far plane out past ~4R to frame the globe, far beyond the metre-scale
// default. The extension is transient (recomputed each frame) and reverts when the planet
// is disabled or deleted.
float ExpandFarClipForSphericalTerrain(ECS::World& world, float userFarClip);

} // namespace GameEngine::Engine::Renderer
