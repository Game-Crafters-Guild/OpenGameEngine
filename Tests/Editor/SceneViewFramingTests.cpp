#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

#include "SceneView/SceneViewFraming.h"

#include "Components/Hierarchy.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Vector3.h"

using GameEngine::ECS::Entity;
using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::World;

using GameEngine::Components::LocalBounds;
using GameEngine::Components::Parent;
using GameEngine::Components::Transform;
using GameEngine::Components::WorldTransform;

using GameEngine::Editor::ComputeSubtreeWorldBounds;
using GameEngine::Editor::SubtreeWorldBounds;

using GameEngine::Mathematics::BoundingBox;
using GameEngine::Mathematics::Vector3;

namespace
{

constexpr float kEpsilon = 1.0e-4f;

WorldTransform MakeTranslation(float x, float y, float z)
{
    WorldTransform xf{};
    xf.matrix[12] = x;
    xf.matrix[13] = y;
    xf.matrix[14] = z;
    return xf;
}

LocalBounds MakeBounds(const Vector3& center, const Vector3& halfExtents)
{
    LocalBounds bounds{};
    bounds.Box = BoundingBox{center, halfExtents};
    return bounds;
}

class SceneViewFramingTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        World* world = GetWorld();
        ASSERT_NE(world, nullptr);
        world->Clear();
    }

    World* GetWorld() const
    {
        static World world;
        return &world;
    }
};

TEST_F(SceneViewFramingTests, SingleEntityWithBoundsUsesWorldSpaceBox)
{
    World* world = GetWorld();

    Entity entity = world->Create();
    entity.Set(MakeTranslation(10.0f, 0.0f, 0.0f));
    entity.Set(MakeBounds(Vector3(1.0f, 2.0f, 3.0f), Vector3(2.0f, 3.0f, 4.0f)));
    world->ProcessCommands();

    SubtreeWorldBounds result{};
    ASSERT_TRUE(ComputeSubtreeWorldBounds(*world, entity.GetHandle(), result));
    ASSERT_TRUE(result.HasBounds);

    EXPECT_NEAR(result.Box.min.x, 9.0f, kEpsilon);
    EXPECT_NEAR(result.Box.min.y, -1.0f, kEpsilon);
    EXPECT_NEAR(result.Box.min.z, -1.0f, kEpsilon);
    EXPECT_NEAR(result.Box.max.x, 13.0f, kEpsilon);
    EXPECT_NEAR(result.Box.max.y, 5.0f, kEpsilon);
    EXPECT_NEAR(result.Box.max.z, 7.0f, kEpsilon);

    const Vector3 center = result.Center();
    EXPECT_NEAR(center.x, 11.0f, kEpsilon);
    EXPECT_NEAR(center.y, 2.0f, kEpsilon);
    EXPECT_NEAR(center.z, 3.0f, kEpsilon);
    EXPECT_NEAR(result.Radius(), std::sqrt(4.0f + 9.0f + 16.0f), kEpsilon);
}

TEST_F(SceneViewFramingTests, SubtreeUnionCoversChildrenWhenRootHasNoBounds)
{
    World* world = GetWorld();

    // Model-root shape: the root carries only a transform, geometry (and
    // therefore LocalBounds) lives on submesh children.
    Entity root = world->Create();
    root.Set(MakeTranslation(5.0f, 0.0f, 0.0f));

    Entity childA = world->Create();
    childA.Set(Parent{root.GetHandle()});
    childA.Set(MakeTranslation(0.0f, 0.0f, 0.0f));
    childA.Set(MakeBounds(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f)));

    Entity childB = world->Create();
    childB.Set(Parent{root.GetHandle()});
    childB.Set(MakeTranslation(10.0f, 0.0f, 0.0f));
    childB.Set(MakeBounds(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f)));

    world->ProcessCommands();

    SubtreeWorldBounds result{};
    ASSERT_TRUE(ComputeSubtreeWorldBounds(*world, root.GetHandle(), result));
    ASSERT_TRUE(result.HasBounds);

    EXPECT_NEAR(result.Box.min.x, -1.0f, kEpsilon);
    EXPECT_NEAR(result.Box.max.x, 11.0f, kEpsilon);
    EXPECT_NEAR(result.Box.min.y, -1.0f, kEpsilon);
    EXPECT_NEAR(result.Box.max.y, 1.0f, kEpsilon);
    EXPECT_NEAR(result.Box.min.z, -1.0f, kEpsilon);
    EXPECT_NEAR(result.Box.max.z, 1.0f, kEpsilon);

    const Vector3 center = result.Center();
    EXPECT_NEAR(center.x, 5.0f, kEpsilon);
    EXPECT_NEAR(center.y, 0.0f, kEpsilon);
    EXPECT_NEAR(center.z, 0.0f, kEpsilon);
    EXPECT_NEAR(result.Radius(), std::sqrt(36.0f + 1.0f + 1.0f), kEpsilon);
}

TEST_F(SceneViewFramingTests, NoBoundsAnywhereFallsBackToRootPosition)
{
    World* world = GetWorld();

    Entity root = world->Create();
    root.Set(MakeTranslation(7.0f, 8.0f, 9.0f));

    Entity child = world->Create();
    child.Set(Parent{root.GetHandle()});
    child.Set(MakeTranslation(20.0f, 0.0f, 0.0f));

    world->ProcessCommands();

    SubtreeWorldBounds result{};
    ASSERT_TRUE(ComputeSubtreeWorldBounds(*world, root.GetHandle(), result));
    EXPECT_FALSE(result.HasBounds);
    EXPECT_NEAR(result.FallbackPosition.x, 7.0f, kEpsilon);
    EXPECT_NEAR(result.FallbackPosition.y, 8.0f, kEpsilon);
    EXPECT_NEAR(result.FallbackPosition.z, 9.0f, kEpsilon);

    // An invalid root is the only hard failure.
    SubtreeWorldBounds invalidResult{};
    EXPECT_FALSE(ComputeSubtreeWorldBounds(*world, EntityHandle{}, invalidResult));
}

TEST_F(SceneViewFramingTests, RotatedScaledChildSwapsExtents)
{
    World* world = GetWorld();

    Entity root = world->Create();
    root.Set(MakeTranslation(0.0f, 0.0f, 0.0f));

    // Child world matrix: rotate 90 degrees about Y, scale local Y by 2,
    // translate to (3, 0, 0). Column-major: column0 = image of local X,
    // column1 = image of local Y, column2 = image of local Z.
    WorldTransform childXf{};
    float m[16] = {
        0.0f, 0.0f, -1.0f, 0.0f, // local X -> world -Z
        0.0f, 2.0f, 0.0f, 0.0f,  // local Y -> world Y, scaled by 2
        1.0f, 0.0f, 0.0f, 0.0f,  // local Z -> world X
        3.0f, 0.0f, 0.0f, 1.0f};
    std::memcpy(childXf.matrix, m, sizeof(m));

    Entity child = world->Create();
    child.Set(Parent{root.GetHandle()});
    child.Set(childXf);
    // Long axis is local X; after the 90-degree yaw it must map to world Z.
    // A scale-only transform of the extents would leave it on world X.
    child.Set(MakeBounds(Vector3(0.0f, 0.0f, 0.0f), Vector3(4.0f, 1.0f, 1.0f)));

    world->ProcessCommands();

    SubtreeWorldBounds result{};
    ASSERT_TRUE(ComputeSubtreeWorldBounds(*world, root.GetHandle(), result));
    ASSERT_TRUE(result.HasBounds);

    EXPECT_NEAR(result.Box.min.x, 2.0f, kEpsilon);
    EXPECT_NEAR(result.Box.max.x, 4.0f, kEpsilon);
    EXPECT_NEAR(result.Box.min.y, -2.0f, kEpsilon);
    EXPECT_NEAR(result.Box.max.y, 2.0f, kEpsilon);
    EXPECT_NEAR(result.Box.min.z, -4.0f, kEpsilon);
    EXPECT_NEAR(result.Box.max.z, 4.0f, kEpsilon);
}

} // namespace
