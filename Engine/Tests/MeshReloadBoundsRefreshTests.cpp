// LocalBounds refresh after an in-place model hot-reload.
//
// ReloadModelMeshes swaps geometry under live MeshGPUHandles, which is what
// keeps entities rendering without a scene reload — and is exactly why nothing
// re-resolves them afterwards. ResolveOneModelEntity seeds LocalBounds only
// when the component is ABSENT, so without the refresh pass every entity keeps
// describing its pre-reload geometry, and everything derived from that column
// goes stale: the per-instance frustum cull radius extraction publishes, and
// the Scene TLAS leaf AABB behind picking / marquee / framing.
//
// These pin the pass itself. The wiring that drives it in a real frame
// (RenderServices publishing the drained GUIDs, RenderingLoop consuming them
// between BeginWorldDrawFrame and the schedule) is covered by the editor
// runtime check, not here.

#include <gtest/gtest.h>

#include "Engine/Rendering/MeshReloadBoundsRefresh.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/RenderServices.h"

#include "ECS/ChangeFilter.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/World.h"

#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"

#include "Assets/ModelAsset.h"
#include "AssetCore/GUID.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

using namespace GameEngine;
using GameEngine::Engine::Renderer::MeshReloadBoundsRefreshReport;
using GameEngine::Engine::Renderer::RefreshLocalBoundsAfterMeshReload;
using GameEngine::Engine::Renderer::RenderServices;

#include "TestDeviceHelper.h"

namespace
{

// A triangle spanning y in [-halfHeight, +halfHeight]. Vertex data moves with
// the extent so a regrown mesh hashes differently — RegisterSubmesh dedups on
// the content hash, and a bounds-only edit would take the skip-upload path and
// never reach the entry.
Mesh MakeTriangle(float halfHeight)
{
    Mesh m{};
    m.Name = "MeshReloadBoundsTriangle";
    Vertex v0{}, v1{}, v2{};
    v0.Position[1] = halfHeight;
    v1.Position[0] = -1.0f;
    v1.Position[1] = -halfHeight;
    v2.Position[0] = 1.0f;
    v2.Position[1] = -halfHeight;
    v0.Normal[2] = v1.Normal[2] = v2.Normal[2] = 1.0f;
    m.Vertices = {v0, v1, v2};
    m.Indices = {0, 1, 2};
    m.MinBounds[0] = -1.0f;
    m.MinBounds[1] = -halfHeight;
    m.MinBounds[2] = 0.0f;
    m.MaxBounds[0] = 1.0f;
    m.MaxBounds[1] = halfHeight;
    m.MaxBounds[2] = 0.0f;
    return m;
}

std::unique_ptr<ModelAsset> MakeModel(const GUID& guid, float halfHeight)
{
    auto model = std::make_unique<ModelAsset>(guid,
                                              std::filesystem::path("Synthetic://MeshReloadBounds"));
    Vector<Mesh> meshes;
    meshes.push_back(MakeTriangle(halfHeight));
    model->SetMeshesForTest(std::move(meshes));
    return model;
}

// Entities visited by a Changed<C>-gated scan since `gate` — the same probe
// shape RenderExtractionSystem escalates on.
template <typename C>
size_t VisitedSince(ECS::World& world, uint64 gate)
{
    ECS::ChangeGate g;
    g.LastRunVersion = gate;
    size_t visited = 0;
    auto q = world.Query<ECS::Read<C>>();
    q.template Changed<C>(g);
    q.BatchEach([&](const C*, std::size_t count) { visited += count; });
    return visited;
}

// Seed an entity the way ResolveOneModelEntity does: bounds taken from the
// registry entry the handle addresses.
ECS::Entity MakeResolvedEntity(ECS::World& world, RenderServices& rs,
                               const GUID& modelGuid, uint64 handleId)
{
    ECS::Entity e = world.Create();
    Components::MeshRenderer mr{};
    mr.modelAssetGuid.Set(modelGuid);
    mr.meshId = 0;
    mr.meshGpuHandleId = handleId;
    e.Set(mr);

    Components::LocalBounds lb{};
    if (const auto* entry = rs.GetMeshGPURegistry().Find(Rendering::MeshGPUHandle(handleId)))
        lb.Box = entry->bounds;
    e.Set(lb);

    world.ProcessCommands();
    return e;
}

constexpr float kStartHalfHeight = 1.0f;
constexpr float kGrownHalfHeight = 2.5f;

} // namespace

// The defect and its repair in one arc: reload grows the geometry, the entry
// follows, the ECS column does not, and the pass reconciles it. Shrinking back is
// the mirror case: the entity would otherwise keep an oversized radius and cull late.
TEST(MeshReloadBoundsRefreshTests, GrownThenShrunkReloadRefreshesLocalBounds)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    const GUID modelGuid = GUID::Generate();
    auto model = MakeModel(modelGuid, kStartHalfHeight);
    const auto handles = rs.GetMeshGPURegistry().RegisterModelMeshes(modelGuid, *model);
    ASSERT_EQ(handles.size(), 1u);

    ECS::World world(nullptr);
    ECS::Entity e = MakeResolvedEntity(world, rs, modelGuid, static_cast<uint64>(handles[0]));

    const auto* lb = world.GetComponent<Components::LocalBounds>(e.GetHandle());
    ASSERT_NE(lb, nullptr);
    ASSERT_FLOAT_EQ(lb->Box.halfExtents.y, kStartHalfHeight);

    // Re-import: the same GUID, taller geometry, refreshed in place.
    auto regrown = MakeModel(modelGuid, kGrownHalfHeight);
    const auto report = rs.GetMeshGPURegistry().ReloadModelMeshes(modelGuid, *regrown);
    ASSERT_EQ(report.SubmeshesReuploaded, 1u) << "reload did not re-upload; the rest proves nothing";

    // Instrument check: the registry entry really did grow, and the handle the
    // component holds still resolves to it.
    const auto* entry = rs.GetMeshGPURegistry().Find(Rendering::MeshGPUHandle(static_cast<uint64>(handles[0])));
    ASSERT_NE(entry, nullptr) << "in-place reload must keep the handle valid";
    ASSERT_FLOAT_EQ(entry->bounds.halfExtents.y, kGrownHalfHeight);

    // The defect: the reload alone leaves the ECS column describing old geometry.
    lb = world.GetComponent<Components::LocalBounds>(e.GetHandle());
    ASSERT_NE(lb, nullptr);
    EXPECT_FLOAT_EQ(lb->Box.halfExtents.y, kStartHalfHeight)
        << "precondition: nothing but the refresh pass updates LocalBounds";

    const MeshReloadBoundsRefreshReport refreshed =
        RefreshLocalBoundsAfterMeshReload(world, rs, {modelGuid});

    EXPECT_EQ(refreshed.EntitiesVisited, 1u);
    EXPECT_EQ(refreshed.BoundsRefreshed, 1u);
    EXPECT_EQ(refreshed.HandlesUnresolved, 0u);
    EXPECT_TRUE(refreshed.ChangedAnything());

    lb = world.GetComponent<Components::LocalBounds>(e.GetHandle());
    ASSERT_NE(lb, nullptr);
    EXPECT_FLOAT_EQ(lb->Box.halfExtents.y, kGrownHalfHeight)
        << "LocalBounds must describe the reloaded geometry";
    EXPECT_GT(lb->Box.Radius(), kStartHalfHeight)
        << "the cull radius extraction derives from this must have grown";

    auto shrunk = MakeModel(modelGuid, kStartHalfHeight);
    ASSERT_EQ(rs.GetMeshGPURegistry().ReloadModelMeshes(modelGuid, *shrunk).SubmeshesReuploaded, 1u);
    EXPECT_EQ(RefreshLocalBoundsAfterMeshReload(world, rs, {modelGuid}).BoundsRefreshed, 1u);
    lb = world.GetComponent<Components::LocalBounds>(e.GetHandle());
    ASSERT_NE(lb, nullptr);
    EXPECT_FLOAT_EQ(lb->Box.halfExtents.y, kStartHalfHeight) << "LocalBounds must follow the shrink too";
}

// LOD0 (including its reference bounds) is byte-identical throughout this arc.
// Only one lower level grows on +X, shrinks, and is removed. This distinguishes
// derived-envelope invalidation from the older whole-base reimport tests and
// detects cumulative growth or accidentally replacing the reference center.
TEST(MeshReloadBoundsRefreshTests, LowerOnlyAsymmetricReloadGrowsShrinksAndRemovesBounds)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    ASSERT_NE(rs.GetGPUScene(), nullptr);
    const GUID guid = GUID::Generate();

    Mesh base = MakeTriangle(1.0f);
    const float center[3] = {4.0f, 6.0f, -2.0f};
    for (Vertex& vertex : base.Vertices)
        for (int axis = 0; axis < 3; ++axis)
            vertex.Position[axis] += center[axis];
    for (int axis = 0; axis < 3; ++axis)
    {
        base.MinBounds[axis] += center[axis];
        base.MaxBounds[axis] += center[axis];
    }
    const auto makeLowerModel = [&](float positiveExtent)
    {
        auto model = std::make_unique<ModelAsset>(guid,
            std::filesystem::path("Synthetic://LowerOnlyBoundsReload"));
        Mesh mesh = base;
        if (positiveExtent > 0)
        {
            mesh.ExtraLODs = {{0u, 1u, 2u}};
            mesh.ExtraLODVertices = {mesh.Vertices};
            mesh.ExtraLODVertices[0][2].Position[0] = center[0] + positiveExtent;
            mesh.ExtraLODCoverage = {.25f};
            mesh.AuthoredLODs = true;
        }
        Vector<Mesh> meshes;
        meshes.push_back(std::move(mesh));
        model->SetMeshesForTest(std::move(meshes));
        return model;
    };

    auto initial = makeLowerModel(2.0f);
    const auto handles = rs.GetMeshGPURegistry().RegisterModelMeshes(guid, *initial);
    ASSERT_EQ(handles.size(), 1u);
    const auto handle = handles[0];
    ECS::World world(nullptr);
    const ECS::Entity entity = MakeResolvedEntity(world, rs, guid, static_cast<uint64>(handle));
    auto* flags = world.GetComponentForWrite<Components::LocalBounds>(entity.GetHandle());
    ASSERT_NE(flags, nullptr);
    flags->DynamicObject = false;
    flags->CastShadows = false;

    const auto* initialEntry = rs.GetMeshGPURegistry().Find(handle);
    ASSERT_NE(initialEntry, nullptr);
    ASSERT_FLOAT_EQ(initialEntry->bounds.halfExtents.x, 2.0f);
    const uint32_t rowIndex = initialEntry->gpuMeshIndex;
    float previousExtent = 2.0f;
    for (float lowerExtent : {7.0f, 1.5f, 0.0f})
    {
        SCOPED_TRACE(lowerExtent);
        const float expectedExtent = lowerExtent > 0 ? lowerExtent : 1.0f;
        auto replacement = makeLowerModel(lowerExtent);
        const Mesh& source = replacement->GetMesh(0);
        ASSERT_EQ(source.Vertices.size(), base.Vertices.size());
        EXPECT_EQ(std::memcmp(source.Vertices.data(), base.Vertices.data(),
                              base.Vertices.size() * sizeof(Vertex)), 0);
        EXPECT_EQ(std::memcmp(source.MinBounds, base.MinBounds, sizeof(base.MinBounds)), 0);
        EXPECT_EQ(std::memcmp(source.MaxBounds, base.MaxBounds, sizeof(base.MaxBounds)), 0);
        ASSERT_EQ(rs.GetMeshGPURegistry().ReloadModelMeshes(guid, *replacement).SubmeshesReuploaded, 1u);

        const auto* entry = rs.GetMeshGPURegistry().Find(handle);
        ASSERT_NE(entry, nullptr) << "same-GUID reload must preserve the live handle";
        EXPECT_EQ(entry->gpuMeshIndex, rowIndex);
        EXPECT_EQ(entry->lodCount, lowerExtent > 0 ? 2u : 1u);
        EXPECT_FLOAT_EQ(entry->bounds.center.x, center[0]);
        EXPECT_FLOAT_EQ(entry->bounds.center.y, center[1]);
        EXPECT_FLOAT_EQ(entry->bounds.center.z, center[2]);
        EXPECT_FLOAT_EQ(entry->bounds.halfExtents.x, expectedExtent);
        EXPECT_FLOAT_EQ(entry->bounds.halfExtents.y, 1.0f);
        EXPECT_FLOAT_EQ(entry->bounds.halfExtents.z, 0.0f);
        EXPECT_FLOAT_EQ(entry->lodReferenceRadius, std::sqrt(2.0f));
        EXPECT_FLOAT_EQ(entry->lodReferenceMaxExtent, 2.0f);
        const auto& rows = rs.GetGPUScene()->GetMeshes();
        ASSERT_LT(rowIndex, rows.size());
        EXPECT_FLOAT_EQ(rows[rowIndex].boundingCenter.x, center[0]);
        EXPECT_FLOAT_EQ(rows[rowIndex].boundingRadius, std::sqrt(expectedExtent * expectedExtent + 1.0f));
        EXPECT_FLOAT_EQ(rows[rowIndex].lodCoverageScale,
                        std::sqrt(2.0f) / std::sqrt(expectedExtent * expectedExtent + 1.0f));

        // The registry change alone must not masquerade as an ECS refresh.
        const auto* before = world.GetComponent<Components::LocalBounds>(entity.GetHandle());
        ASSERT_NE(before, nullptr);
        EXPECT_FLOAT_EQ(before->Box.halfExtents.x, previousExtent);
        const auto refreshed = RefreshLocalBoundsAfterMeshReload(world, rs, {guid});
        EXPECT_EQ(refreshed.EntitiesVisited, 1u);
        EXPECT_EQ(refreshed.BoundsRefreshed, 1u);
        EXPECT_EQ(refreshed.HandlesUnresolved, 0u);
        const auto* after = world.GetComponent<Components::LocalBounds>(entity.GetHandle());
        ASSERT_NE(after, nullptr);
        EXPECT_FLOAT_EQ(after->Box.center.x, center[0]);
        EXPECT_FLOAT_EQ(after->Box.center.y, center[1]);
        EXPECT_FLOAT_EQ(after->Box.center.z, center[2]);
        EXPECT_FLOAT_EQ(after->Box.halfExtents.x, expectedExtent);
        EXPECT_FLOAT_EQ(after->Box.Radius(), rows[rowIndex].boundingRadius);
        EXPECT_FALSE(after->DynamicObject);
        EXPECT_FALSE(after->CastShadows);
        const uint64 gate = world.GetGlobalSystemVersion();
        EXPECT_EQ(RefreshLocalBoundsAfterMeshReload(world, rs, {guid}).BoundsRefreshed, 0u);
        EXPECT_EQ(VisitedSince<Components::LocalBounds>(world, gate), 0u);
        previousExtent = expectedExtent;
    }
}

// Scoping is load-bearing, not an optimisation. LocalBounds has writers whose
// value deliberately differs from the entity's own submesh entry — a
// default-parameter primitive box, a package extraction system publishing its own
// generated bounds — so a world-wide refresh would silently rewrite those
// entities whenever some unrelated model reloaded.
TEST(MeshReloadBoundsRefreshTests, RefreshIsScopedToTheReloadedModel)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    const GUID reloadedGuid = GUID::Generate();
    const GUID bystanderGuid = GUID::Generate();
    auto reloadedModel = MakeModel(reloadedGuid, kStartHalfHeight);
    auto bystanderModel = MakeModel(bystanderGuid, kStartHalfHeight);

    const auto reloadedHandles = rs.GetMeshGPURegistry().RegisterModelMeshes(reloadedGuid, *reloadedModel);
    const auto bystanderHandles = rs.GetMeshGPURegistry().RegisterModelMeshes(bystanderGuid, *bystanderModel);
    ASSERT_EQ(reloadedHandles.size(), 1u);
    ASSERT_EQ(bystanderHandles.size(), 1u);

    ECS::World world(nullptr);
    ECS::Entity reloadedEntity = MakeResolvedEntity(world, rs, reloadedGuid, static_cast<uint64>(reloadedHandles[0]));

    // The bystander carries bounds that deliberately do NOT match its entry,
    // standing in for the inspector's whole-model union.
    ECS::Entity bystander = world.Create();
    Components::MeshRenderer bystanderMr{};
    bystanderMr.modelAssetGuid.Set(bystanderGuid);
    bystanderMr.meshGpuHandleId = static_cast<uint64>(bystanderHandles[0]);
    bystander.Set(bystanderMr);
    Components::LocalBounds unionBounds{};
    unionBounds.Box.center = {0.0f, 0.0f, 0.0f};
    unionBounds.Box.halfExtents = {9.0f, 9.0f, 9.0f};
    bystander.Set(unionBounds);
    world.ProcessCommands();

    auto regrown = MakeModel(reloadedGuid, kGrownHalfHeight);
    ASSERT_EQ(rs.GetMeshGPURegistry().ReloadModelMeshes(reloadedGuid, *regrown).SubmeshesReuploaded, 1u);

    const auto refreshed = RefreshLocalBoundsAfterMeshReload(world, rs, {reloadedGuid});
    EXPECT_EQ(refreshed.EntitiesVisited, 1u) << "only the reloaded model's entities are in scope";
    EXPECT_EQ(refreshed.BoundsRefreshed, 1u);

    const auto* reloadedLb = world.GetComponent<Components::LocalBounds>(reloadedEntity.GetHandle());
    ASSERT_NE(reloadedLb, nullptr);
    EXPECT_FLOAT_EQ(reloadedLb->Box.halfExtents.y, kGrownHalfHeight);

    const auto* bystanderLb = world.GetComponent<Components::LocalBounds>(bystander.GetHandle());
    ASSERT_NE(bystanderLb, nullptr);
    EXPECT_FLOAT_EQ(bystanderLb->Box.halfExtents.y, 9.0f)
        << "an unrelated model's reload must not rewrite this entity's bounds";
}

// An entity whose model reference was cleared, and one whose handle no longer
// resolves (the UnregisterModel shape, which announces through the same event)
// must be counted and skipped rather than crash or write garbage.
TEST(MeshReloadBoundsRefreshTests, UnresolvableHandlesAreSkipped)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    const GUID modelGuid = GUID::Generate();
    auto model = MakeModel(modelGuid, kStartHalfHeight);
    const auto handles = rs.GetMeshGPURegistry().RegisterModelMeshes(modelGuid, *model);
    ASSERT_EQ(handles.size(), 1u);

    ECS::World world(nullptr);

    // Cleared model reference: no geometry to derive from.
    ECS::Entity cleared = world.Create();
    Components::MeshRenderer clearedMr{};
    clearedMr.modelAssetGuid.Set(modelGuid);
    clearedMr.meshGpuHandleId = 0u;
    cleared.Set(clearedMr);
    cleared.Set(Components::LocalBounds{});

    // Live entity, then the model leaves memory entirely.
    ECS::Entity stranded = MakeResolvedEntity(world, rs, modelGuid, static_cast<uint64>(handles[0]));
    world.ProcessCommands();

    rs.GetMeshGPURegistry().UnregisterModel(modelGuid);
    ASSERT_EQ(rs.GetMeshGPURegistry().Find(Rendering::MeshGPUHandle(static_cast<uint64>(handles[0]))), nullptr);

    const auto refreshed = RefreshLocalBoundsAfterMeshReload(world, rs, {modelGuid});
    EXPECT_EQ(refreshed.EntitiesVisited, 2u);
    EXPECT_EQ(refreshed.BoundsRefreshed, 0u);
    EXPECT_EQ(refreshed.HandlesUnresolved, 1u) << "the destroyed handle is the only unresolved one";

    const auto* lb = world.GetComponent<Components::LocalBounds>(stranded.GetHandle());
    ASSERT_NE(lb, nullptr);
    EXPECT_FLOAT_EQ(lb->Box.halfExtents.y, kStartHalfHeight)
        << "an unresolvable handle must leave the last known bounds alone";
}

// An empty GUID list is the every-frame case: it must cost nothing and touch
// nothing.
TEST(MeshReloadBoundsRefreshTests, EmptyGuidListIsANoOp)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    const GUID modelGuid = GUID::Generate();
    auto model = MakeModel(modelGuid, kStartHalfHeight);
    const auto handles = rs.GetMeshGPURegistry().RegisterModelMeshes(modelGuid, *model);
    ASSERT_EQ(handles.size(), 1u);

    ECS::World world(nullptr);
    ECS::Entity e = MakeResolvedEntity(world, rs, modelGuid, static_cast<uint64>(handles[0]));

    auto regrown = MakeModel(modelGuid, kGrownHalfHeight);
    ASSERT_EQ(rs.GetMeshGPURegistry().ReloadModelMeshes(modelGuid, *regrown).SubmeshesReuploaded, 1u);

    const uint64 gate = world.GetGlobalSystemVersion();
    const auto refreshed = RefreshLocalBoundsAfterMeshReload(world, rs, {});
    EXPECT_EQ(refreshed.EntitiesVisited, 0u);
    EXPECT_EQ(refreshed.BoundsRefreshed, 0u);
    EXPECT_EQ(VisitedSince<Components::LocalBounds>(world, gate), 0u);

    const auto* lb = world.GetComponent<Components::LocalBounds>(e.GetHandle());
    ASSERT_NE(lb, nullptr);
    EXPECT_FLOAT_EQ(lb->Box.halfExtents.y, kStartHalfHeight);
}
