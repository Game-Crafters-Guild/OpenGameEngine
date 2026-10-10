// Oracle for the planet far-plane auto-scale derivation shared by the editor Scene View
// (SceneViewController) and the game / Player camera (CameraSystem): an enabled spherical
// terrain extends the far plane to fit its bounding sphere, never shrinking below the
// user's setting; absent (or planar/disabled) terrain leaves it untouched. Both camera
// paths call this one helper, so this suite covers the game-camera far derivation too.

#include <gtest/gtest.h>

#include "Engine/Rendering/PlanetCameraFraming.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::World;
using GameEngine::Components::Terrain;
using GameEngine::Components::TerrainDomain;
using GameEngine::Components::TerrainPlanetRelief;
using GameEngine::Engine::Renderer::ExpandFarClipForSphericalTerrain;

namespace
{
// Mirror of the helper's constant: far must reach >= 4*(radius + relief).
constexpr float kFarMultiple = 4.0f;

EntityHandle AddTerrain(World& world, TerrainDomain domain, float radius, float relief,
                        bool enabled)
{
    const EntityHandle e = world.CreateEntity();
    Terrain t{};
    t.Domain = domain;
    t.PlanetRadius = radius;
    world.AddComponentImmediate(e, t);
    GameEngine::ECS::Entity(&world, e).SetEnabled<Terrain>(enabled);
    // Base relief lives in the companion component the far-clip framing reads.
    TerrainPlanetRelief r{};
    r.Amplitude = relief;
    world.AddComponentImmediate(e, r);
    return e;
}
} // namespace

TEST(SceneViewFarPlane, NoTerrainLeavesFarUntouched)
{
    World world;
    EXPECT_FLOAT_EQ(ExpandFarClipForSphericalTerrain(world, 3000.0f), 3000.0f);
}

TEST(SceneViewFarPlane, PlanarTerrainLeavesFarUntouched)
{
    World world;
    AddTerrain(world, TerrainDomain::Planar, 5000.0f, 125.0f, /*enabled*/ true);
    EXPECT_FLOAT_EQ(ExpandFarClipForSphericalTerrain(world, 3000.0f), 3000.0f);
}

TEST(SceneViewFarPlane, DisabledSphericalTerrainLeavesFarUntouched)
{
    World world;
    AddTerrain(world, TerrainDomain::Spherical, 5000.0f, 125.0f, /*enabled*/ false);
    EXPECT_FLOAT_EQ(ExpandFarClipForSphericalTerrain(world, 3000.0f), 3000.0f);
}

TEST(SceneViewFarPlane, SphericalTerrainExtendsFarToFitPlanet)
{
    World world;
    AddTerrain(world, TerrainDomain::Spherical, 5000.0f, 125.0f, /*enabled*/ true);
    const float expected = (5000.0f + 125.0f) * kFarMultiple; // 20500
    EXPECT_FLOAT_EQ(ExpandFarClipForSphericalTerrain(world, 3000.0f), expected);
}

TEST(SceneViewFarPlane, UsersLargerFarWins)
{
    World world;
    AddTerrain(world, TerrainDomain::Spherical, 5000.0f, 125.0f, /*enabled*/ true);
    // User far (50000) already exceeds the planet bound (20500) — never shrink it.
    EXPECT_FLOAT_EQ(ExpandFarClipForSphericalTerrain(world, 50000.0f), 50000.0f);
}

TEST(SceneViewFarPlane, LargestEnabledPlanetWins)
{
    World world;
    AddTerrain(world, TerrainDomain::Spherical, 5000.0f, 0.0f, /*enabled*/ true);
    AddTerrain(world, TerrainDomain::Spherical, 50000.0f, 1250.0f, /*enabled*/ true);
    const float expected = (50000.0f + 1250.0f) * kFarMultiple; // 205000
    EXPECT_FLOAT_EQ(ExpandFarClipForSphericalTerrain(world, 3000.0f), expected);
}
