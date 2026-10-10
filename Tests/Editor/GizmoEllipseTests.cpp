#include "SceneView/SceneViewGizmos.h"
#include <gtest/gtest.h>
#include <cmath>

using namespace GameEngine::Editor::SceneTools;
using GameEngine::Mathematics::Vector3;

namespace
{
const GameEngine::Color kWhite(1.0f, 1.0f, 1.0f, 1.0f);
}

TEST(GizmoEllipse, SkewedNonuniformBasisPreservesItsLocalCircle)
{
    constexpr GameEngine::Rendering::ViewId kView = 9071;
    ResetGizmoLineGroups(kView);
    GizmoRenderContext context(kView, 0);
    context.DrawWireEllipse({4, 5, 7}, {2, 0, 0}, {1, 3, 0}, kWhite);
    const auto* groups = GetGizmoLineGroups(kView);
    ASSERT_NE(groups, nullptr);
    ASSERT_EQ(groups->size(), 1u);
    const auto& vertices = groups->front().vertices;
    ASSERT_EQ(vertices.size(), 64u * 6u);
    for (size_t index = 0; index < vertices.size(); index += 3)
    {
        const float localY = (vertices[index + 1] - 5) / 3;
        const float localX = (vertices[index] - 4 - localY) / 2;
        EXPECT_NEAR(localX * localX + localY * localY, 1, 1e-5f);
        EXPECT_FLOAT_EQ(vertices[index + 2], 7);
    }
    for (size_t axis = 0; axis < 3; ++axis)
        EXPECT_NEAR(vertices[axis], vertices[vertices.size() - 3 + axis], 1e-5f);
    ResetGizmoLineGroups(kView);
}

TEST(GizmoEllipse, WireCircleRemainsInItsAuthoredPlane)
{
    constexpr GameEngine::Rendering::ViewId kView = 9072;
    ResetGizmoLineGroups(kView);
    GizmoRenderContext context(kView, 0);
    const Vector3 center{4, 5, 6}, normal{1, 2, 3};
    context.DrawWireCircle(center, normal, 2, kWhite);
    const auto* groups = GetGizmoLineGroups(kView);
    ASSERT_NE(groups, nullptr);
    ASSERT_EQ(groups->size(), 1u);
    const auto& vertices = groups->front().vertices;
    ASSERT_EQ(vertices.size(), 64u * 6u);
    for (size_t index = 0; index < vertices.size(); index += 3)
    {
        const Vector3 point{vertices[index], vertices[index + 1], vertices[index + 2]};
        const auto offset = point - center;
        EXPECT_NEAR(offset.Length(), 2, 1e-5f);
        EXPECT_NEAR(Vector3::Dot(offset, normal), 0, 1e-5f);
    }
    ResetGizmoLineGroups(kView);
}

TEST(GizmoEllipse, CollapsedAxisProducesFiniteFlatGeometry)
{
    constexpr GameEngine::Rendering::ViewId kView = 9073;
    ResetGizmoLineGroups(kView);
    GizmoRenderContext context(kView, 0);
    context.DrawWireEllipse({1, 2, 3}, {2, 0, 0}, {}, kWhite);
    const auto* groups = GetGizmoLineGroups(kView);
    ASSERT_NE(groups, nullptr);
    ASSERT_EQ(groups->size(), 1u);
    const auto& vertices = groups->front().vertices;
    ASSERT_EQ(vertices.size(), 64u * 6u);
    for (size_t index = 0; index < vertices.size(); index += 3)
    {
        EXPECT_TRUE(std::isfinite(vertices[index]));
        EXPECT_GE(vertices[index], -1.00001f);
        EXPECT_LE(vertices[index], 3.00001f);
        EXPECT_FLOAT_EQ(vertices[index + 1], 2);
        EXPECT_FLOAT_EQ(vertices[index + 2], 3);
    }
    ResetGizmoLineGroups(kView);
}
