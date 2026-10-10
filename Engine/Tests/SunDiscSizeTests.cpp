// SkyEnvironment::SunSize — the stylistic multiplier on the radius the sun disc is DRAWN at.
//
// The value is authored on the component and consumed as SkySettings::sunSize, which
// SkyRenderNode multiplies into the single sunAngularRadius it uploads. Three properties are
// worth pinning, and each fails silently:
//
//   1. FREE BY DEFAULT. 1.0 must mean "the physical sun", or every existing scene moved the
//      day this landed. A default that is not exactly 1 produces a perfectly plausible sun.
//   2. IT REACHES PRODUCTION. The plumbing runs component -> SkyEnvironmentSystem::Update ->
//      SkyRenderFeature settings. A test that copied the struct itself would still pass with
//      the system's assignment deleted, so these drive a real Update and read the feature.
//   3. IT CANNOT PRODUCE A BAD RADIUS. sun_disc.glsl divides the sun's irradiance by the solid
//      angle the drawn radius covers. std::clamp passes a NaN through unchanged, and a NaN or
//      zero radius there is a NaN in SceneColor that the whole frame carries — which is why
//      sun_disc.glsl floors its own divisor. The clamp is the CPU half of that guarantee.
//
// The disc's radiance contract at whatever radius it ends up with is SunDiscRadianceTests;
// this file only pins the size that gets handed to it.

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "Components/Rendering/Light.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h" // World::AddComponentImmediate/GetComponent definitions
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/SkyEnvironmentSystem.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Rendering/Sky/SkySettings.h"

using namespace GameEngine;

namespace
{

// A sky entity plus the linked directional light the system expects, ticked through the real
// SkyEnvironmentSystem so the value read back is the one production consumes.
struct SunSizeHarness
{
    ECS::World World;
    Engine::Renderer::RenderServices Services;
    Engine::Renderer::SkyEnvironmentSystem System{&Services};
    ECS::EntityHandle SkyEntity;
    ECS::EntityHandle LightEntity;

    SunSizeHarness()
    {
        LightEntity = World.CreateEntity();
        Components::Light light{};
        light.Type = Components::LightType::Directional;
        light.Intensity = 100000.0f;
        World.AddComponentImmediate(LightEntity, light);
        World.AddComponentImmediate(LightEntity, Components::Transform{});

        SkyEntity = World.CreateEntity();
        Components::SkyEnvironment sky{};
        sky.TimeOfDayHours = 12.0f; // sun high: the disc is drawn, far from the moon handoff
        sky.SunLight = LightEntity;
        World.AddComponentImmediate(SkyEntity, sky);
    }

    void SetSunSize(float size)
    {
        auto* sky = World.GetComponent<Components::SkyEnvironment>(SkyEntity);
        ASSERT_NE(sky, nullptr);
        Components::SkyEnvironment updated = *sky;
        updated.SunSize = size;
        World.AddComponentImmediate(SkyEntity, updated);
    }

    // One production tick, then the size the renderer will draw with.
    float Run()
    {
        System.Update(World, 0.0f);
        auto* feature = Services.GetFeature<Engine::Renderer::SkyRenderFeature>();
        EXPECT_NE(feature, nullptr);
        EXPECT_TRUE(feature->HasActiveSettings());
        return feature->GetSettings().sunSize;
    }
};

} // namespace

// 1.0 exactly, on the component AND on the settings struct it feeds. If either drifts, every
// scene that never touched the lever is rendering a different sun than it did before.
TEST(SunDiscSize, TheDefaultIsThePhysicalSun)
{
    const Components::SkyEnvironment defaults{};
    EXPECT_FLOAT_EQ(defaults.SunSize, 1.0f);

    const Rendering::SkySettings settingsDefaults{};
    EXPECT_FLOAT_EQ(settingsDefaults.sunSize, 1.0f);

    SunSizeHarness harness;
    EXPECT_FLOAT_EQ(harness.Run(), 1.0f)
        << "an untouched sky no longer draws the sun at its physical radius";
}

// The authored value has to survive the system, not just the struct: this is the assignment in
// SkyEnvironmentSystem that a refactor can drop without any other test noticing.
TEST(SunDiscSize, TheAuthoredSizeReachesTheRendererSettings)
{
    SunSizeHarness harness;
    harness.SetSunSize(3.0f);
    EXPECT_FLOAT_EQ(harness.Run(), 3.0f)
        << "SkyEnvironment::SunSize is not reaching SkySettings::sunSize";
}

// Both ends of the authoring range, from the shared constants rather than repeated literals.
TEST(SunDiscSize, TheSizeIsClampedToTheAuthoringRange)
{
    ASSERT_GT(Components::kSunSizeMin, 0.0f)
        << "a zero or negative floor would let the drawn radius reach sun_disc.glsl's "
           "division-by-zero case";
    ASSERT_LT(Components::kSunSizeMin, Components::kSunSizeMax);

    SunSizeHarness harness;

    harness.SetSunSize(-4.0f);
    EXPECT_FLOAT_EQ(harness.Run(), Components::kSunSizeMin);

    harness.SetSunSize(0.0f);
    EXPECT_FLOAT_EQ(harness.Run(), Components::kSunSizeMin);

    harness.SetSunSize(1000.0f);
    EXPECT_FLOAT_EQ(harness.Run(), Components::kSunSizeMax);
}

// std::clamp(NaN, lo, hi) returns NaN: neither comparison is true, so the value falls through.
// The drawn radius is the one input sun_disc.glsl explicitly guards, so the CPU side refuses a
// non-finite size instead of handing one to it.
TEST(SunDiscSize, ANonFiniteSizeFallsBackToPhysicalRatherThanPropagating)
{
    SunSizeHarness harness;

    harness.SetSunSize(std::numeric_limits<float>::quiet_NaN());
    const float fromNan = harness.Run();
    EXPECT_TRUE(std::isfinite(fromNan)) << "a NaN sun size reached the renderer settings";
    EXPECT_FLOAT_EQ(fromNan, 1.0f);

    harness.SetSunSize(std::numeric_limits<float>::infinity());
    const float fromInf = harness.Run();
    EXPECT_TRUE(std::isfinite(fromInf)) << "an infinite sun size reached the renderer settings";
    EXPECT_FLOAT_EQ(fromInf, 1.0f);
}

// The size is a look dial over a physical constant, so the range has to keep the disc drawable:
// below the floor it is a sub-pixel dot that crawls, and the ceiling is what the inspector, the
// scene parser and the system all bound against.
TEST(SunDiscSize, TheAuthoringRangeStaysAroundThePhysicalSun)
{
    EXPECT_LE(Components::kSunSizeMin, 1.0f)
        << "the physical sun must be authorable, and it is the default";
    EXPECT_GE(Components::kSunSizeMax, 1.0f);
}
