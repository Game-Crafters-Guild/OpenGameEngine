// The chunk commit: how a spline recipe's generated meshes are registered under
// stable keys, drawn, and released (Placement/SplineChunkCommit.h). Driven on a
// registry with no device — registration then keeps its keys and handles
// without uploading, which is all the lifecycle under test reads — and a bare
// world.

#include "Placement/SplineChunkCommit.h"

#include "Assets/ModelAsset.h"
#include "Components/Hierarchy.h"
#include "Components/Rendering/MeshRenderer.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Engine/Rendering/MeshGPURegistry.h"

#include <gtest/gtest.h>

#include <vector>

using namespace GameEngine;
using Editor::SplineChunkCommit;
using Rendering::MeshGPUHandle;
using Rendering::MeshGPUKey;
using Rendering::MeshGPURegistry;

namespace
{

Mesh Triangle(float lift)
{
    Mesh mesh;
    Vertex a{};
    Vertex b{};
    Vertex c{};
    a.Position[1] = lift;
    b.Position[0] = 1.0f;
    b.Position[1] = lift;
    c.Position[2] = 1.0f;
    c.Position[1] = lift;
    mesh.Vertices = {a, b, c};
    mesh.Indices = {0u, 1u, 2u};
    mesh.MaxBounds[0] = 1.0f;
    mesh.MaxBounds[2] = 1.0f;
    mesh.MinBounds[1] = mesh.MaxBounds[1] = lift;
    return mesh;
}

MeshGPUKey KeyOf(const char* name, uint32 index = 0u)
{
    return MeshGPUKey{GUID::Derive(GUID::Null(), name), index};
}

size_t ChildCount(ECS::World& world, ECS::EntityHandle owner)
{
    size_t count = 0;
    world.Query<ECS::Read<Components::Parent>>().Each(
        [&](ECS::EntityHandle, const Components::Parent& parent)
        {
            if (parent.parent == owner)
                ++count;
        });
    return count;
}

} // namespace

// A key registered twice is updated in place under the same handle, a key a
// build keeps survives it with that handle, and a key a build reaches no more
// is released.
TEST(SplineChunkCommit, KeysSurviveTheBuildsThatReachThemAndNoOther)
{
    MeshGPURegistry registry;
    SplineChunkCommit commit;
    const MeshGPUHandle first = commit.Register(registry, KeyOf("a"), Triangle(0.0f));
    commit.Register(registry, KeyOf("b"), Triangle(1.0f));
    commit.RetireUnreached(registry);
    ASSERT_NE(registry.FindByKey(KeyOf("a")), nullptr);
    ASSERT_NE(registry.FindByKey(KeyOf("b")), nullptr);

    EXPECT_EQ(commit.Register(registry, KeyOf("a"), Triangle(2.0f)), first)
        << "a re-registered key must keep its handle";
    commit.RetireUnreached(registry);
    EXPECT_NE(registry.FindByKey(KeyOf("a")), nullptr);
    EXPECT_EQ(registry.FindByKey(KeyOf("b")), nullptr) << "an unreached key must be released";

    MeshGPUHandle kept;
    EXPECT_TRUE(commit.Keep(KeyOf("a"), kept));
    EXPECT_EQ(kept, first);
    EXPECT_FALSE(commit.Keep(KeyOf("b"), kept)) << "a released key is not kept";
    commit.RetireUnreached(registry);
    EXPECT_NE(registry.FindByKey(KeyOf("a")), nullptr) << "a kept key must survive";
}

// Slots are addressed by index: an empty mesh leaves a hole that is not a dead
// chunk, a shorter build retires the tail and releases its keys, and a
// steady-state rebuild spawns nothing.
TEST(SplineChunkCommit, SlotsKeepTheirKeysLeaveHolesAndRetireTheTail)
{
    ECS::World world;
    MeshGPURegistry registry;
    SplineChunkCommit commit;
    const ECS::EntityHandle owner = world.Create().GetHandle();
    const GUID space = GUID::Derive(GUID::Null(), "space");
    const Components::MeshRenderer renderer{};

    const std::vector<Mesh> three = {Triangle(0.0f), Mesh{}, Triangle(2.0f)};
    EXPECT_TRUE(commit.CommitSlots(world, registry, owner, space, three, renderer, true));
    world.ProcessCommands();
    EXPECT_EQ(ChildCount(world, owner), 2u);
    EXPECT_NE(registry.FindByKey({space, 0u}), nullptr);
    EXPECT_EQ(registry.FindByKey({space, 1u}), nullptr) << "a hole registers nothing";
    EXPECT_NE(registry.FindByKey({space, 2u}), nullptr);
    EXPECT_TRUE(commit.ChunksAlive(world)) << "a hole is not a dead chunk";

    EXPECT_FALSE(commit.CommitSlots(world, registry, owner, space, three, renderer, true))
        << "a steady-state rebuild spawns nothing";

    const std::vector<Mesh> one = {Triangle(0.0f)};
    EXPECT_TRUE(commit.CommitSlots(world, registry, owner, space, one, renderer, true));
    world.ProcessCommands();
    EXPECT_EQ(ChildCount(world, owner), 1u);
    EXPECT_EQ(registry.FindByKey({space, 2u}), nullptr) << "the retired tail keeps its key";

    EXPECT_TRUE(commit.Retire(world, &registry));
    world.ProcessCommands();
    EXPECT_EQ(ChildCount(world, owner), 0u);
    EXPECT_EQ(registry.FindByKey({space, 0u}), nullptr);
}

// After a world reset the chunk handles alias whatever recycled their
// indices: they are forgotten, never destroyed, and the keys still go.
TEST(SplineChunkCommit, AWorldResetForgetsTheChunksAndReleasesTheKeys)
{
    ECS::World world;
    MeshGPURegistry registry;
    SplineChunkCommit commit;
    const ECS::EntityHandle owner = world.Create().GetHandle();
    const GUID space = GUID::Derive(GUID::Null(), "space");
    const std::vector<Mesh> meshes = {Triangle(0.0f)};
    commit.CommitSlots(world, registry, owner, space, meshes, Components::MeshRenderer{}, true);
    world.ProcessCommands();
    ASSERT_EQ(ChildCount(world, owner), 1u);

    commit.DropAfterWorldReset(&registry);
    world.ProcessCommands();
    EXPECT_EQ(ChildCount(world, owner), 1u) << "nothing may be destroyed after a reset";
    EXPECT_EQ(registry.FindByKey({space, 0u}), nullptr);
}
