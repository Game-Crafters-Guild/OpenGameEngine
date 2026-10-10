// The Scene View's component-keyed gizmo registry: a component's editor code registers its drawing,
// and the Scene View draws it for an entity that carries the component without naming it. The local
// volumes' gizmos are its first registrations.

#include <gtest/gtest.h>

#include "Components/Rendering/Particles.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/WindVolume.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Particles/ParticleStackAuthoring.h"
#include "Particles/ParticleStackDocument.h"
#include "SceneView/ComponentGizmoRegistry.h"
#include "SceneView/Gizmos/ParticleEmitterGizmo.h"
#include "SceneView/PostProcessVolumeGizmo.h"
#include "SceneView/WindVolumeGizmo.h"
#include "SceneView/SceneViewGizmos.h"

#include <cstddef>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Editor::SceneTools;

namespace
{

constexpr Rendering::ViewId kViewId = 7;
constexpr Rendering::CameraId kCameraId = 7;

// A component no production code registers a gizmo for, so these tests own its entry.
struct RegistryProbe
{
    int Value = 0;
};

int g_ProbeDraws = 0;
int g_ReplacementDraws = 0;

void DrawProbe(GizmoRenderContext&, const ECS::World&, ECS::EntityHandle)
{
    ++g_ProbeDraws;
}

void DrawReplacement(GizmoRenderContext&, const ECS::World&, ECS::EntityHandle)
{
    ++g_ReplacementDraws;
}

std::size_t LineVertexCount()
{
    const auto* groups = GetGizmoLineGroups(kViewId);
    std::size_t count = 0;
    if (groups)
    {
        for (const GizmoLineGroup& group : *groups)
            count += group.vertices.size();
    }
    return count;
}

std::vector<float> DrawnLines()
{
    std::vector<float> vertices;
    if (const auto* groups = GetGizmoLineGroups(kViewId))
    {
        for (const GizmoLineGroup& group : *groups)
            vertices.insert(vertices.end(), group.vertices.begin(), group.vertices.end());
    }
    return vertices;
}

// Every line vertex the particle gizmo draws for `entity`, in drawing order.
std::vector<float> ParticleEmitterGizmoLines(const ECS::World& world, ECS::EntityHandle entity)
{
    GizmoRenderContext context(kViewId, kCameraId);
    ResetGizmoLineGroups(kViewId);
    DrawParticleEmitterGizmo(context, world, entity);
    return DrawnLines();
}

// Every line vertex the particle gizmo draws for an emitter running `document`.
std::vector<float> ParticleStackGizmoLines(const Particles::StackDocument& document)
{
    GizmoRenderContext context(kViewId, kCameraId);
    ResetGizmoLineGroups(kViewId);
    DrawParticleStackGizmo(context, Components::ParticleEmitter3D{}, Components::WorldTransform{}, document);
    return DrawnLines();
}

} // namespace

TEST(ComponentGizmoRegistry, DrawsOnlyForEntitiesThatCarryTheComponent)
{
    ECS::World world;
    const ECS::EntityHandle with = world.CreateEntity();
    world.AddComponentImmediate(with, RegistryProbe{});
    const ECS::EntityHandle without = world.CreateEntity();

    ComponentGizmoRegistry::Get().Register(ECS::GetComponentTypeId<RegistryProbe>(), DrawProbe);
    GizmoRenderContext context(kViewId, kCameraId);

    g_ProbeDraws = 0;
    ComponentGizmoRegistry::Get().Draw(context, world, with);
    ComponentGizmoRegistry::Get().Draw(context, world, without);
    EXPECT_EQ(g_ProbeDraws, 1);
}

// A component switched off through its ECS::ComponentDisabled tag draws no gizmo, and draws again
// once it is switched back on: the registry owns the switch, so no registered drawing checks it.
TEST(ComponentGizmoRegistry, ASwitchedOffComponentDrawsNothing)
{
    ECS::World world;
    const ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, RegistryProbe{});
    const ECS::ComponentTypeId probeId = ECS::GetComponentTypeId<RegistryProbe>();

    ComponentGizmoRegistry::Get().Register(probeId, DrawProbe);
    GizmoRenderContext context(kViewId, kCameraId);

    g_ProbeDraws = 0;
    ASSERT_TRUE(world.SetComponentEnabledImmediate(entity, probeId, false));
    ComponentGizmoRegistry::Get().Draw(context, world, entity);
    EXPECT_EQ(g_ProbeDraws, 0) << "a switched-off component must draw no gizmo";

    ASSERT_TRUE(world.SetComponentEnabledImmediate(entity, probeId, true));
    ComponentGizmoRegistry::Get().Draw(context, world, entity);
    EXPECT_EQ(g_ProbeDraws, 1);
}

TEST(ComponentGizmoRegistry, ARegistrationReplacesTheOneBefore)
{
    ECS::World world;
    const ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, RegistryProbe{});

    ComponentGizmoRegistry::Get().Register(ECS::GetComponentTypeId<RegistryProbe>(), DrawProbe);
    ComponentGizmoRegistry::Get().Register(ECS::GetComponentTypeId<RegistryProbe>(), DrawReplacement);
    GizmoRenderContext context(kViewId, kCameraId);

    g_ProbeDraws = 0;
    g_ReplacementDraws = 0;
    ComponentGizmoRegistry::Get().Draw(context, world, entity);
    EXPECT_EQ(g_ProbeDraws, 0);
    EXPECT_EQ(g_ReplacementDraws, 1);
}

// The volumes' gizmos, moved onto the registry: a local volume draws its wireframe, a global one,
// which has no shape, draws nothing.
TEST(ComponentGizmoRegistry, LocalVolumesDrawAndGlobalOnesDoNot)
{
    RegisterPostProcessVolumeGizmo();
    RegisterWindVolumeGizmo();
    GizmoRenderContext context(kViewId, kCameraId);

    ECS::World world;
    const ECS::EntityHandle local = world.CreateEntity();
    Components::PostProcessVolume localVolume{};
    localVolume.IsGlobal = false;
    world.AddComponentImmediate(local, localVolume);
    world.AddComponentImmediate(local, Components::WorldTransform{});

    const ECS::EntityHandle global = world.CreateEntity();
    Components::PostProcessVolume globalVolume{};
    globalVolume.IsGlobal = true;
    world.AddComponentImmediate(global, globalVolume);
    world.AddComponentImmediate(global, Components::WorldTransform{});

    const ECS::EntityHandle wind = world.CreateEntity();
    Components::WindVolume windVolume{};
    windVolume.IsGlobal = false;
    world.AddComponentImmediate(wind, windVolume);
    world.AddComponentImmediate(wind, Components::WorldTransform{});

    ResetGizmoLineGroups(kViewId);
    ComponentGizmoRegistry::Get().Draw(context, world, global);
    EXPECT_EQ(LineVertexCount(), 0u);

    ComponentGizmoRegistry::Get().Draw(context, world, local);
    const std::size_t afterLocal = LineVertexCount();
    EXPECT_GT(afterLocal, 0u);

    ComponentGizmoRegistry::Get().Draw(context, world, wind);
    EXPECT_GT(LineVertexCount(), afterLocal);
}

// An emitter switched off, itself or through a switched-off ancestor (the tag the hierarchy pass
// writes), runs no simulation, so its gizmo draws nothing.
TEST(ComponentGizmoRegistry, ParticleEmitterGizmoDrawsNothingForAnInactiveEmitter)
{
    ECS::World world;
    const ECS::EntityHandle emitter = world.CreateEntity();
    world.AddComponentImmediate(emitter, Components::ParticleEmitter3D{});
    world.AddComponentImmediate(emitter, Components::WorldTransform{});
    ASSERT_FALSE(ParticleEmitterGizmoLines(world, emitter).empty()) << "an active emitter draws; this test is vacuous";

    world.AddComponentImmediate(emitter, ECS::DisabledInHierarchy{});
    EXPECT_TRUE(ParticleEmitterGizmoLines(world, emitter).empty()) << "an emitter under a switched-off parent draws";
    world.RemoveComponentImmediate<ECS::DisabledInHierarchy>(emitter);
    world.AddComponentImmediate(emitter, ECS::Disabled{});
    EXPECT_TRUE(ParticleEmitterGizmoLines(world, emitter).empty()) << "a switched-off emitter draws";
}

// The gizmo draws the stack the simulation runs: the default stack for an emitter that references
// none, the shapes of the processors a stack switches on, and nothing for a disabled one.
TEST(ComponentGizmoRegistry, ParticleEmitterGizmoDrawsTheShapesOfTheStackTheSimulationRuns)
{
    ECS::World world;
    const ECS::EntityHandle plain = world.CreateEntity();
    world.AddComponentImmediate(plain, Components::ParticleEmitter3D{});
    world.AddComponentImmediate(plain, Components::WorldTransform{});
    const std::vector<float> defaults = ParticleEmitterGizmoLines(world, plain);
    ASSERT_FALSE(defaults.empty());
    EXPECT_EQ(ParticleStackGizmoLines(Particles::MakeDefaultStack()), defaults);

    Particles::StackDocument document;
    std::vector<Particles::StackDiagnostic> diagnostics;
    ASSERT_TRUE(Particles::ParseParticleStack(
        R"({"version":1,"entryPhase":1,"phases":[{"id":1,"processors":[)"
        R"({"type":"shape","id":2,"stage":"birth","parameters":{"shape":"box","extents":[2,1,3]}}]}]})",
        document, diagnostics));
    const std::vector<float> box = ParticleStackGizmoLines(document);
    EXPECT_NE(box, defaults) << "the authored box draws";
    document.Phases.front().Processors.front().Enabled = false;
    const std::vector<float> switchedOff = ParticleStackGizmoLines(document);
    EXPECT_NE(switchedOff, box) << "a switched-off shape draws nothing";
    EXPECT_FALSE(switchedOff.empty()) << "without a shape the origin marker draws";
}
