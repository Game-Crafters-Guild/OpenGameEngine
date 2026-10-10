// The sky's Scene View overlay: registered by the sky's own editor code on the component gizmo
// registry, drawn from the sky's fields (never its entity's rotation), and only for a physical sky.

#include <gtest/gtest.h>

#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Mathematics/Vector3.h"
#include "SceneView/ComponentGizmoRegistry.h"
#include "SceneView/SceneViewGizmos.h"
#include "Sky/SkyPathGizmo.h"
#include "Types/Color.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Editor::SceneTools;

namespace
{

constexpr Rendering::ViewId kViewId = 11;
constexpr Rendering::CameraId kCameraId = 11;

// The overlay's strokes are camera-facing quads (a core over a dark halo), so it draws triangles.
std::vector<float> DrawnVertices(const ECS::World& world, ECS::EntityHandle entity)
{
    const Mathematics::Vector3 camera(0.0f, 0.0f, -10.0f);
    GizmoRenderContext context(kViewId, kCameraId, &camera);
    ResetGizmoLineGroups(kViewId);
    ResetGizmoTriangleGroups(kViewId);
    ComponentGizmoRegistry::Get().Draw(context, world, entity);
    std::vector<float> vertices;
    if (const auto* groups = GetGizmoTriangleGroups(kViewId))
    {
        for (const GizmoTriangleGroup& group : *groups)
            vertices.insert(vertices.end(), group.vertices.begin(), group.vertices.end());
    }
    return vertices;
}

// Every overlay stroke has a dark halo drawn beneath it, and the overlay leaves the sticky triangle
// layer at its default for the next gizmo.
TEST(SkyPathGizmo, EveryStrokeHasAHaloBeneathItAndTheLayerIsHandedBack)
{
    Editor::RegisterSkyPathGizmo();
    ECS::World world;
    const ECS::EntityHandle sky = world.CreateEntity();
    world.AddComponentImmediate(sky, Components::SkyEnvironment{});
    world.AddComponentImmediate(sky, Components::WorldTransform{});

    const Mathematics::Vector3 camera(0.0f, 0.0f, -10.0f);
    GizmoRenderContext context(kViewId, kCameraId, &camera);
    ResetGizmoTriangleGroups(kViewId);
    ComponentGizmoRegistry::Get().Draw(context, world, sky);
    const auto* groups = GetGizmoTriangleGroups(kViewId);
    ASSERT_NE(groups, nullptr);
    int lowestLayer = 0;
    int highestLayer = -100;
    for (const GizmoTriangleGroup& group : *groups)
    {
        lowestLayer = std::min(lowestLayer, static_cast<int>(group.layer));
        highestLayer = std::max(highestLayer, static_cast<int>(group.layer));
        EXPECT_EQ(group.depthMode, GizmoDepthMode::AlwaysOnTop);
        EXPECT_EQ(group.color.a, 1.0f) << "strokes are opaque, so the halo stays beneath its core";
    }
    EXPECT_LT(lowestLayer, highestLayer) << "the halo sits on a layer below the core";
    EXPECT_LT(highestLayer, 0) << "every other gizmo (layer 0 and up) draws over the dome";

    const Mathematics::Vector3 probe[3] = {
        Mathematics::Vector3(0.0f, 0.0f, 0.0f), Mathematics::Vector3(1.0f, 0.0f, 0.0f), Mathematics::Vector3(0.0f, 1.0f, 0.0f)};
    const Color probeColor(0.25f, 0.5f, 0.75f, 1.0f);
    context.DrawTriangles(probe, 3, probeColor);
    EXPECT_EQ(GetGizmoTriangleGroups(kViewId)->back().layer, 0);
}

// One marker ring: its segments, each a core quad over a halo quad, as the floats of their vertices.
constexpr std::size_t kMarkerSegments = 24;
constexpr std::size_t kFloatsPerStrokedSegment = 2 * 6 * 3;
constexpr std::size_t kFloatsPerMarker = kMarkerSegments * kFloatsPerStrokedSegment;

std::size_t DrawnFloats(float axisAltitude, float noonHeight)
{
    ECS::World world;
    const ECS::EntityHandle entity = world.CreateEntity();
    Components::SkyEnvironment sky{};
    sky.SunPath = Components::SkySunPathKind::Custom;
    sky.CustomAxisAltitude = axisAltitude;
    sky.CustomNoonHeight = noonHeight;
    world.AddComponentImmediate(entity, sky);
    world.AddComponentImmediate(entity, Components::WorldTransform{});
    return DrawnVertices(world, entity).size();
}

} // namespace

// A stroke keeps its width on screen along its length. The axis runs from the dome's centre away
// from a camera that looks along north, so its far end is drawn as much wider as it is farther: the
// shaft is as wide on screen there as at the centre, and as wide as the rings.
TEST(SkyPathGizmo, TheAxisShaftKeepsItsWidthAlongItsLength)
{
    Editor::RegisterSkyPathGizmo();
    ECS::World world;
    const ECS::EntityHandle entity = world.CreateEntity();
    Components::SkyEnvironment sky{};
    sky.Latitude = 51.5f;
    world.AddComponentImmediate(entity, sky);
    world.AddComponentImmediate(entity, Components::WorldTransform{});

    const Mathematics::Vector3 camera(0.0f, 0.0f, -10.0f);
    GizmoRenderContext context(kViewId, kCameraId, &camera);
    ResetGizmoTriangleGroups(kViewId);
    ComponentGizmoRegistry::Get().Draw(context, world, entity);
    const auto* groups = GetGizmoTriangleGroups(kViewId);
    ASSERT_NE(groups, nullptr);

    // The axis is the overlay's only cyan on the Earth path, and its shaft the first cyan stroke.
    constexpr float kAxisRed = 0.15f;
    const GizmoTriangleGroup* axisCore = nullptr;
    for (const GizmoTriangleGroup& group : *groups)
    {
        if (group.color.r == kAxisRed && group.layer == -1)
            axisCore = &group;
    }
    ASSERT_NE(axisCore, nullptr);
    ASSERT_GE(axisCore->vertices.size(), 18u);
    // A stroke's quad is two triangles over its corners 0 1 2, 0 2 3: 0 and 1 at its start, 2 and
    // 3 at its end.
    const float* v = axisCore->vertices.data();
    const auto distance = [](const float* a, const float* b) {
        return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
    };
    const float* startLeft = v;
    const float* startRight = v + 3;
    const float* endRight = v + 6;
    const float* endLeft = v + 15;
    const float startMiddle[3] = {0.5f * (startLeft[0] + startRight[0]), 0.5f * (startLeft[1] + startRight[1]),
                                  0.5f * (startLeft[2] + startRight[2])};
    const float endMiddle[3] = {0.5f * (endLeft[0] + endRight[0]), 0.5f * (endLeft[1] + endRight[1]),
                                0.5f * (endLeft[2] + endRight[2])};
    const float startDistance = distance(startMiddle, &camera[0]);
    const float endDistance = distance(endMiddle, &camera[0]);
    ASSERT_GT(endDistance, startDistance * 1.1f) << "the axis runs away from this camera";

    const float widthOnScreenAtStart = distance(startLeft, startRight) / startDistance;
    const float widthOnScreenAtEnd = distance(endLeft, endRight) / endDistance;
    EXPECT_NEAR(widthOnScreenAtEnd, widthOnScreenAtStart, widthOnScreenAtStart * 1e-3f);
}

// Sunrise and sunset are marked on a day that has them, by the reading the inspector's card words:
// a sun that circles along the horizon, or stands still on it, crosses nothing, though the sunrise
// equation gives such a day twelve hours.
TEST(SkyPathGizmo, SunriseAndSunsetAreMarkedOnlyOnADayThatHasThem)
{
    Editor::RegisterSkyPathGizmo();
    const std::size_t risesAndSets = DrawnFloats(30.0f, 60.0f);
    const std::size_t neverSets = DrawnFloats(70.0f, 60.0f);
    EXPECT_EQ(risesAndSets, neverSets + 2 * kFloatsPerMarker) << "the two rings a sunrise and a sunset add";

    EXPECT_EQ(DrawnFloats(-90.0f, 180.0f), neverSets) << "circling along the horizon";
    EXPECT_EQ(DrawnFloats(0.0f, 0.0f), neverSets) << "standing still on the horizon";
    EXPECT_EQ(DrawnFloats(75.0f, -8.43f), neverSets) << "never rising";
}

TEST(SkyPathGizmo, APhysicalSkyDrawsItsPathAndAGradientSkyDoesNot)
{
    Editor::RegisterSkyPathGizmo();
    ECS::World world;
    const ECS::EntityHandle sky = world.CreateEntity();
    world.AddComponentImmediate(sky, Components::SkyEnvironment{});
    world.AddComponentImmediate(sky, Components::WorldTransform{});
    EXPECT_FALSE(DrawnVertices(world, sky).empty());

    Components::SkyEnvironment gradient{};
    gradient.Mode = Components::SkyMode::Gradient;
    world.AddComponentImmediate(sky, gradient);
    EXPECT_TRUE(DrawnVertices(world, sky).empty());
}

// The overlay follows the fields: a Custom axis turned to +X draws a different path from the same
// sky under Earth, and turning the entity itself changes nothing.
TEST(SkyPathGizmo, TheOverlayFollowsTheFieldsNotTheEntitysRotation)
{
    Editor::RegisterSkyPathGizmo();
    ECS::World world;
    const ECS::EntityHandle entity = world.CreateEntity();
    Components::SkyEnvironment sky{};
    sky.CustomAxisHeading = 90.0f;
    sky.CustomAxisAltitude = 40.0f;
    world.AddComponentImmediate(entity, sky);
    world.AddComponentImmediate(entity, Components::WorldTransform{});
    const std::vector<float> earth = DrawnVertices(world, entity);

    Components::WorldTransform turned{};
    turned.matrix[0] = 0.0f;
    turned.matrix[2] = -1.0f;
    turned.matrix[8] = 1.0f;
    turned.matrix[10] = 0.0f;
    world.AddComponentImmediate(entity, turned);
    EXPECT_EQ(DrawnVertices(world, entity), earth) << "the entity's rotation does not move the overlay";

    sky.SunPath = Components::SkySunPathKind::Custom;
    world.AddComponentImmediate(entity, sky);
    EXPECT_NE(DrawnVertices(world, entity), earth);
}
