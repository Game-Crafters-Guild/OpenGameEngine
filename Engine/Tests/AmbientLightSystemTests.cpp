#include <gtest/gtest.h>

#include "Components/Rendering/AmbientLight.h"
#include "Components/Rendering/LightPhotometry.h" // kReferenceWhiteNits
#include "ECS/Components.h" // ECS::Disabled
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECSModules/Rendering/Systems/AmbientLightSystem.h"

#include <cmath>
#include <limits>

using namespace GameEngine;
using GameEngine::Engine::Renderer::AmbientFloorData;
using GameEngine::Engine::Renderer::AmbientLightSystem;

namespace
{
float ExpectedScale(float intensityNits) { return intensityNits / Components::kReferenceWhiteNits; }
} // namespace

// No AmbientLight in the world -> the resolved floor is fully off (mode 0, zero colors), which the
// shader turns into an exact vec3(0) add. This is the byte-identical no-component invariant.
TEST(AmbientLightSystem, NoComponent_ZeroFloor)
{
    ECS::World world;
    AmbientFloorData floor = AmbientLightSystem::ResolveActiveFloor(world);
    EXPECT_EQ(floor.Mode, 0u);
    EXPECT_EQ(floor.AffectSpecular, 0u);
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_FLOAT_EQ(floor.Sky[i], 0.0f);
        EXPECT_FLOAT_EQ(floor.Equator[i], 0.0f);
        EXPECT_FLOAT_EQ(floor.Ground[i], 0.0f);
    }
}

// The default AmbientFloorData (what the IBL feature holds before any system runs) is the off state.
TEST(AmbientLightSystem, DefaultFloorIsOff)
{
    AmbientFloorData floor{};
    EXPECT_EQ(floor.Mode, 0u);
    EXPECT_EQ(floor.AffectSpecular, 0u);
    EXPECT_FLOAT_EQ(floor.Sky[0], 0.0f);
    EXPECT_FLOAT_EQ(floor.Ground[2], 0.0f);
}

// Flat mode packs the single color (x Intensity / 203) into every slot; AffectSpecular passes through.
TEST(AmbientLightSystem, Flat_ResolvesConstantOnAnchor)
{
    ECS::World world;
    ECS::EntityHandle e = world.CreateEntity();
    Components::AmbientLight a{};
    a.Mode = Components::AmbientLightMode::Flat;
    a.Color[0] = 0.5f;
    a.Color[1] = 0.25f;
    a.Color[2] = 1.0f;
    a.Intensity = 60.0f;
    a.AffectSpecular = true;
    world.AddComponentImmediate(e, a);

    AmbientFloorData floor = AmbientLightSystem::ResolveActiveFloor(world);
    const float s = ExpectedScale(60.0f);
    EXPECT_EQ(floor.Mode, 1u);
    EXPECT_EQ(floor.AffectSpecular, 1u);
    EXPECT_FLOAT_EQ(floor.Sky[0], 0.5f * s);
    EXPECT_FLOAT_EQ(floor.Equator[0], 0.5f * s);
    EXPECT_FLOAT_EQ(floor.Ground[2], 1.0f * s);
}

// Gradient mode resolves each slot from its own authored color on the 203 anchor.
TEST(AmbientLightSystem, Gradient_ResolvesGradientOnAnchor)
{
    ECS::World world;
    ECS::EntityHandle e = world.CreateEntity();
    Components::AmbientLight a{};
    a.Mode = Components::AmbientLightMode::Gradient;
    a.SkyColor[0] = 0.2f;
    a.SkyColor[2] = 0.35f;
    a.EquatorColor[0] = 0.3f;
    a.GroundColor[2] = 0.06f;
    a.Intensity = 100.0f;
    world.AddComponentImmediate(e, a);

    AmbientFloorData floor = AmbientLightSystem::ResolveActiveFloor(world);
    const float s = ExpectedScale(100.0f);
    EXPECT_EQ(floor.Mode, 2u);
    EXPECT_EQ(floor.AffectSpecular, 0u);
    EXPECT_FLOAT_EQ(floor.Sky[0], 0.2f * s);
    EXPECT_FLOAT_EQ(floor.Sky[2], 0.35f * s);
    EXPECT_FLOAT_EQ(floor.Equator[0], 0.3f * s);
    EXPECT_FLOAT_EQ(floor.Ground[2], 0.06f * s);
}

// A Disabled AmbientLight is excluded from extraction by the query engine:
// the resolved floor stays fully off, exactly as if the component were absent.
TEST(AmbientLightSystem, DisabledComponent_Excluded)
{
    ECS::World world;
    ECS::EntityHandle e = world.CreateEntity();
    Components::AmbientLight a{};
    a.Mode = Components::AmbientLightMode::Flat;
    a.Color[0] = 1.0f;
    a.Intensity = 60.0f;
    world.AddComponentImmediate(e, a);
    world.AddComponentImmediate(e, ECS::Disabled{});

    AmbientFloorData floor = AmbientLightSystem::ResolveActiveFloor(world);
    EXPECT_EQ(floor.Mode, 0u);
    EXPECT_FLOAT_EQ(floor.Sky[0], 0.0f);
}

// Hostile serialized values never reach the UBO: a negative or non-finite Intensity collapses
// the scale to 0, and a negative color channel clamps at 0 — the resolved floor is always
// finite and non-negative (0 x inf = NaN would otherwise reach every lit pixel).
TEST(AmbientLightSystem, HostileIntensityAndColors_ClampFiniteNonNegative)
{
    {
        ECS::World world;
        ECS::EntityHandle e = world.CreateEntity();
        Components::AmbientLight a{};
        a.Mode = Components::AmbientLightMode::Flat;
        a.Color[0] = 1.0f;
        a.Intensity = -50.0f;
        world.AddComponentImmediate(e, a);

        AmbientFloorData floor = AmbientLightSystem::ResolveActiveFloor(world);
        EXPECT_EQ(floor.Mode, 1u);
        EXPECT_FLOAT_EQ(floor.Sky[0], 0.0f);
    }
    {
        ECS::World world;
        ECS::EntityHandle e = world.CreateEntity();
        Components::AmbientLight a{};
        a.Mode = Components::AmbientLightMode::Gradient;
        a.SkyColor[0] = 0.0f; // 0 x inf scale would resolve to NaN without the finite clamp
        a.SkyColor[1] = 1.0f;
        a.Intensity = std::numeric_limits<float>::infinity();
        world.AddComponentImmediate(e, a);

        AmbientFloorData floor = AmbientLightSystem::ResolveActiveFloor(world);
        for (int i = 0; i < 3; ++i)
        {
            EXPECT_TRUE(std::isfinite(floor.Sky[i]));
            EXPECT_TRUE(std::isfinite(floor.Equator[i]));
            EXPECT_TRUE(std::isfinite(floor.Ground[i]));
            EXPECT_GE(floor.Sky[i], 0.0f);
        }
        EXPECT_FLOAT_EQ(floor.Sky[1], 0.0f); // inf intensity collapses the scale to 0
    }
    {
        ECS::World world;
        ECS::EntityHandle e = world.CreateEntity();
        Components::AmbientLight a{};
        a.Mode = Components::AmbientLightMode::Flat;
        a.Color[0] = -1.0f; // negative channel with a sane intensity
        a.Color[1] = 0.5f;
        a.Intensity = Components::kReferenceWhiteNits; // scale exactly 1
        world.AddComponentImmediate(e, a);

        AmbientFloorData floor = AmbientLightSystem::ResolveActiveFloor(world);
        EXPECT_FLOAT_EQ(floor.Sky[0], 0.0f);
        EXPECT_FLOAT_EQ(floor.Sky[1], 0.5f);
    }
}

// A present-but-zero-intensity floor keeps its mode set yet resolves every channel to 0 (still a
// byte-identical add in the shader).
TEST(AmbientLightSystem, ZeroIntensity_ZeroColors)
{
    ECS::World world;
    ECS::EntityHandle e = world.CreateEntity();
    Components::AmbientLight a{};
    a.Mode = Components::AmbientLightMode::Gradient;
    a.SkyColor[0] = 1.0f;
    a.Intensity = 0.0f;
    world.AddComponentImmediate(e, a);

    AmbientFloorData floor = AmbientLightSystem::ResolveActiveFloor(world);
    EXPECT_EQ(floor.Mode, 2u);
    EXPECT_FLOAT_EQ(floor.Sky[0], 0.0f);
}

// Two AmbientLights: first-wins (a single winner, never a blend/accumulation). Same mode, different
// intensity; the resolved value must equal exactly one of them and never their sum.
TEST(AmbientLightSystem, MultipleComponents_FirstWinsSingleWinner)
{
    ECS::World world;
    ECS::EntityHandle e1 = world.CreateEntity();
    Components::AmbientLight a1{};
    a1.Mode = Components::AmbientLightMode::Flat;
    a1.Color[0] = 1.0f;
    a1.Intensity = 60.0f;
    world.AddComponentImmediate(e1, a1);

    ECS::EntityHandle e2 = world.CreateEntity();
    Components::AmbientLight a2{};
    a2.Mode = Components::AmbientLightMode::Flat;
    a2.Color[0] = 1.0f;
    a2.Intensity = 200.0f;
    world.AddComponentImmediate(e2, a2);

    AmbientFloorData floor = AmbientLightSystem::ResolveActiveFloor(world);
    EXPECT_EQ(floor.Mode, 1u);
    const float s60 = ExpectedScale(60.0f);
    const float s200 = ExpectedScale(200.0f);
    const bool matchesOneWinner =
        std::abs(floor.Sky[0] - s60) < 1e-6f || std::abs(floor.Sky[0] - s200) < 1e-6f;
    EXPECT_TRUE(matchesOneWinner);
    EXPECT_LT(floor.Sky[0], s60 + s200 - 1e-4f); // never accumulated
}
