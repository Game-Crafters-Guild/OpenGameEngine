#include "Scene/DefaultSceneEntities.h"

#include "Components/Name.h"
#include "Components/Rendering/Camera.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Transform.h"

#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/PlaneColliderShape.h"
#include "PhysicsECS/Components/SphereColliderShape.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"

#include <cstring>

namespace GameEngine::Editor
{

using namespace Components;

namespace
{

void SetName(ECS::Entity& entity, const char* text)
{
    Name name{};
    std::memset(name.value, 0, sizeof(name.value));
    const size_t maxCopy = sizeof(name.value) - 1;
    for (size_t i = 0; i < maxCopy && text[i] != '\0'; ++i)
        name.value[i] = text[i];
    entity.Set(name);
}

MeshRenderer MakeMeshRenderer(Engine::Renderer::RenderServices* rs, const GUID& meshGuid, const GUID& matGuid)
{
    return Engine::Renderer::PrimitiveGenerator::MakePrimitiveMeshRenderer(rs, meshGuid, matGuid);
}

void SpawnDynamicSphere(ECS::World& world,
                        Engine::Renderer::RenderServices* rs,
                        const GUID& sphereGuid,
                        const GUID& materialGuid,
                        const Mathematics::Vector3& position,
                        const char* name)
{
    ECS::Entity e = world.Create();
    e.Set(Transform::FromTRS(position, Mathematics::Quaternion{}, Mathematics::Vector3{1.0f, 1.0f, 1.0f}));
    e.Set(MakeMeshRenderer(rs, sphereGuid, materialGuid));
    e.Set(Engine::Renderer::PrimitiveGenerator::MakePrimitiveLocalBounds(rs, sphereGuid));

    PhysicsBody sphereBody{};
    sphereBody.motionType = Physics::MotionType::Dynamic;
    world.AddComponentImmediate(e.GetHandle(), sphereBody);

    PhysicsCollider sphereCollider{};
    sphereCollider.layer = Physics::Layers::Dynamic;
    world.AddComponentImmediate(e.GetHandle(), sphereCollider);
    world.AddComponentImmediate(e.GetHandle(), SphereColliderShape{});

    SetName(e, name);
}

} // namespace

void SeedDefaultSceneEntities(ECS::World& world, Engine::Renderer::RenderServices* rs)
{
    const GUID matGuid = Engine::Renderer::PrimitiveGenerator::DefaultMaterialGuid();

    // --- Directional Light (inspector Euler 50, -30, 0; shines along +Z) ---
    ECS::EntityHandle directionalLightHandle{};
    {
        ECS::Entity e = world.Create();
        auto rot = QuaternionFromEulerXYZDegrees(kDefaultDirectionalLightEulerXDeg,
                                                kDefaultDirectionalLightEulerYDeg,
                                                kDefaultDirectionalLightEulerZDeg);
        e.Set(Transform::FromTRS(Mathematics::Vector3{0.0f, 3.0f, 0.0f}, rot, Mathematics::Vector3{1.0f, 1.0f, 1.0f}));

        Light light{};
        light.Type = LightType::Directional;
        light.Intensity = kClearNoonSunIlluminanceLux;
        light.IntensityUnit = LightUnit::Lux; // physical units by default
        light.CastsShadows = true;
        e.Set(light);

        SetName(e, "Directional Light");
        directionalLightHandle = e.GetHandle();
    }

    // No flat Ambient Light: ambient comes from the sky-derived IBL irradiance,
    // which tracks the sun physically. A fixed-lux ambient double-counts it and
    // fights the exposure model when the sun dims (ambient-lit ground vs a
    // physically dark sky skews the auto-exposure metering).

    // --- Sky Environment ---
    {
        ECS::Entity e = world.Create();
        e.Set(Transform::FromTRS(Mathematics::Vector3{}, Mathematics::Quaternion{}, Mathematics::Vector3{1.0f, 1.0f, 1.0f}));
        SkyEnvironment sky{}; // defaults: noon, auto sun/moon
        sky.SunLight = directionalLightHandle;
        e.Set(sky);
        SetName(e, "Sky Environment");
    }

    // --- Main Camera (Game View default; matches editor inspector defaults: pos 0,3,-10, identity rot, unit scale) ---
    {
        ECS::Entity e = world.Create();
        e.Set(Transform::FromTRS(
            Mathematics::Vector3{0.0f, 3.0f, -10.0f},
            Mathematics::Quaternion{},
            Mathematics::Vector3{1.0f, 1.0f, 1.0f}));
        e.Set(Camera{});
        SetName(e, "Main Camera");
    }

    // --- Post Process Volume ---
    {
        ECS::Entity e = world.Create();
        e.Set(Transform::FromTRS(Mathematics::Vector3{}, Mathematics::Quaternion{}, Mathematics::Vector3{1.0f, 1.0f, 1.0f}));

        // Tonemap is left at the component default so the default scene and a
        // volume-less world always show the same operator.
        PostProcessVolume volume{};
        volume.IsGlobal = true;
        e.Set(volume);
        // Exposure is on the Main Camera (default Auto); camera-less views (the editor Scene View)
        // auto-meter from the world default.

        SetName(e, "Post Process Volume");
    }

    // --- Ground Plane ---
    if (rs)
    {
        const GUID planeGuid = Engine::Renderer::PrimitiveGenerator::PlaneGuid();
        ECS::Entity e = world.Create();
        e.Set(Transform::FromTRS(Mathematics::Vector3{0.0f, 0.0f, 0.0f}, Mathematics::Quaternion{}, Mathematics::Vector3{100.0f, 1.0f, 100.0f}));
        e.Set(MakeMeshRenderer(rs, planeGuid, matGuid));
        e.Set(Engine::Renderer::PrimitiveGenerator::MakePrimitiveLocalBounds(rs, planeGuid));

        PhysicsBody planeBody{};
        planeBody.motionType = Physics::MotionType::Static;
        planeBody.gravityScale = 0.0f;
        world.AddComponentImmediate(e.GetHandle(), planeBody);

        PhysicsCollider planeCollider{};
        planeCollider.layer = Physics::Layers::Static;
        world.AddComponentImmediate(e.GetHandle(), planeCollider);
        world.AddComponentImmediate(e.GetHandle(), PlaneColliderShape{});

        SetName(e, "Plane");
    }

    // --- Sphere ---
    if (rs)
    {
        const GUID sphereGuid = Engine::Renderer::PrimitiveGenerator::SphereGuid();
        const GUID reflectionMatGuid = Engine::Renderer::PrimitiveGenerator::ReflectionProbeTestMaterialGuid();
        SpawnDynamicSphere(world, rs, sphereGuid, matGuid, Mathematics::Vector3{-1.25f, 1.0f, 0.0f}, "Sphere");
        SpawnDynamicSphere(world, rs, sphereGuid, reflectionMatGuid, Mathematics::Vector3{1.25f, 1.0f, 0.0f}, "Mirror Sphere");
    }

    world.ProcessCommands();
}

} // namespace GameEngine::Editor
