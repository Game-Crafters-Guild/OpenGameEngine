#pragma once

// World + headless RenderServices + the production SkyEnvironmentSystem: what the sky tests run a
// frame through and read back. Shared by the sky's test files in this executable.

#include "Components/Rendering/Light.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/SkyEnvironmentSystem.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"

#include <gtest/gtest.h>

namespace GameEngine
{

// One tick, then read SkySettings.
struct SkySunAnchorHarness
{
    ECS::World World;
    Engine::Renderer::RenderServices Services;
    Engine::Renderer::SkyEnvironmentSystem System{&Services};
    ECS::EntityHandle SkyEntity;

    ECS::EntityHandle AddDirectional(float intensity, Components::LightUnit unit)
    {
        const ECS::EntityHandle e = World.CreateEntity();
        Components::Light light{};
        light.Type = Components::LightType::Directional;
        light.Intensity = intensity;
        light.IntensityUnit = unit;
        World.AddComponentImmediate(e, light);
        World.AddComponentImmediate(e, Components::Transform{});
        return e;
    }

    // A physical sky exactly as an author gets it out of the box: enabled, Physical, no SunLight.
    void AddDefaultSky()
    {
        SkyEntity = World.CreateEntity();
        World.AddComponentImmediate(SkyEntity, Components::SkyEnvironment{});
    }

    void SetSky(const Components::SkyEnvironment& sky)
    {
        SkyEntity = World.IsValid(SkyEntity) ? SkyEntity : World.CreateEntity();
        World.AddComponentImmediate(SkyEntity, sky);
    }

    Components::SkyEnvironment Sky() const
    {
        const auto* sky = World.GetComponent<Components::SkyEnvironment>(SkyEntity);
        return sky ? *sky : Components::SkyEnvironment{};
    }

    float RunAndReadAnchor()
    {
        System.Update(World, 0.0f);
        auto* feature = Services.GetFeature<Engine::Renderer::SkyRenderFeature>();
        EXPECT_NE(feature, nullptr);
        EXPECT_TRUE(feature->HasActiveSettings());
        return feature->GetSettings().primarySunIntensity;
    }
};

} // namespace GameEngine
