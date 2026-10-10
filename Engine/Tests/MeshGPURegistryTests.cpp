// Tests for MeshGPURegistry: GUID-keyed deduplication, shared GPU resources,
// multi-entity same model, unregistration, handle stability, and the mutual
// exclusion that lets several ECS-wave systems register at once.

#include <gtest/gtest.h>

#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/RenderServices.h"
#include "Assets/ModelAsset.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

#include "TestDeviceHelper.h"

// --- Unit tests using RegisterSubmesh directly (no file I/O needed) ---

class MeshGPURegistryTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_device = CreateVulkanDeviceFast();
        if (!m_device)
        {
            GTEST_SKIP() << "No Vulkan device available";
        }
        m_registry.Initialize(m_device.get());
    }

    void TearDown() override
    {
        m_registry.Shutdown();
        if (m_device)
            m_device->Shutdown();
    }

    // Build a minimal test mesh with a single triangle.
    Mesh MakeTriangleMesh(bool withNormals = true, bool withUV = true, bool skinned = false)
    {
        Mesh m{};
        m.Name = "TestTriangle";

        Vertex v0{}, v1{}, v2{};
        v0.Position[0] = 0.0f; v0.Position[1] = 1.0f; v0.Position[2] = 0.0f;
        v1.Position[0] = -1.0f; v1.Position[1] = -1.0f; v1.Position[2] = 0.0f;
        v2.Position[0] = 1.0f; v2.Position[1] = -1.0f; v2.Position[2] = 0.0f;

        if (withNormals)
        {
            v0.Normal[2] = 1.0f;
            v1.Normal[2] = 1.0f;
            v2.Normal[2] = 1.0f;
        }

        if (withUV)
        {
            v0.TexCoords[0] = 0.5f; v0.TexCoords[1] = 0.0f;
            v1.TexCoords[0] = 0.0f; v1.TexCoords[1] = 1.0f;
            v2.TexCoords[0] = 1.0f; v2.TexCoords[1] = 1.0f;
        }

        m.Vertices = {v0, v1, v2};
        m.Indices = {0, 1, 2};
        m.MaterialIndex = 0;

        m.MinBounds[0] = -1.0f; m.MinBounds[1] = -1.0f; m.MinBounds[2] = 0.0f;
        m.MaxBounds[0] = 1.0f;  m.MaxBounds[1] = 1.0f;  m.MaxBounds[2] = 0.0f;

        if (skinned)
        {
            m.Skinned = true;
            m.Joints0 = {0, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0};
            m.Weights0 = {1.0f, 0.0f, 0.0f, 0.0f,
                          0.5f, 0.5f, 0.0f, 0.0f,
                          0.5f, 0.5f, 0.0f, 0.0f};
        }

        return m;
    }

    Mesh MakeUint32IndexMesh(uint32_t vertexCount, uint32_t indexCount)
    {
        Mesh m{};
        m.Name = "TestUint32Mesh";
        m.MaterialIndex = 0;

        m.Vertices.resize(vertexCount);
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            Vertex& v = m.Vertices[i];
            v.Position[0] = static_cast<float>(i % 256u) * 0.01f;
            v.Position[1] = static_cast<float>((i / 256u) % 256u) * 0.01f;
            v.Position[2] = 0.0f;
            v.Normal[2] = 1.0f;
            v.TexCoords[0] = 0.0f;
            v.TexCoords[1] = 0.0f;
        }

        m.Indices.resize(indexCount);
        for (uint32_t i = 0; i < indexCount; ++i)
            m.Indices[i] = i % 3u;

        m.MinBounds[0] = 0.0f; m.MinBounds[1] = 0.0f; m.MinBounds[2] = 0.0f;
        m.MaxBounds[0] = 2.55f; m.MaxBounds[1] = 2.55f; m.MaxBounds[2] = 0.0f;
        return m;
    }

    std::unique_ptr<IDevice> m_device;
    MeshGPURegistry m_registry;
};

// Basic registration produces a valid handle with correct vertex flags.
TEST_F(MeshGPURegistryTest, RegisterSubmesh_ProducesValidHandle)
{
    GUID guid = GUID::Generate();
    Mesh mesh = MakeTriangleMesh();
    MeshGPUKey key{guid, 0};

    MeshGPUHandle handle = m_registry.RegisterSubmesh(key, mesh);
    ASSERT_TRUE(handle.IsValid());

    const MeshGPUEntry* entry = m_registry.Find(handle);
    ASSERT_NE(entry, nullptr);
    MeshGPUEntryBindings bindings{};
    ASSERT_TRUE(m_registry.TryGetDrawableBindings(*entry, bindings));
    EXPECT_TRUE(bindings.coreVB.IsValid());
    EXPECT_TRUE(bindings.indexBuffer.IsValid());
    EXPECT_EQ(entry->indexCount, 3u);
    EXPECT_TRUE(HasFlag(entry->vertexFlags, VertexAttributeFlags::HasPosition));
    EXPECT_TRUE(HasFlag(entry->vertexFlags, VertexAttributeFlags::HasNormal));
    EXPECT_TRUE(HasFlag(entry->vertexFlags, VertexAttributeFlags::HasUV0));
}

// Registering the same key twice returns the same handle (deduplication).
TEST_F(MeshGPURegistryTest, RegisterSubmesh_Deduplicates)
{
    GUID guid = GUID::Generate();
    Mesh mesh = MakeTriangleMesh();
    MeshGPUKey key{guid, 0};

    MeshGPUHandle h1 = m_registry.RegisterSubmesh(key, mesh);
    MeshGPUHandle h2 = m_registry.RegisterSubmesh(key, mesh);

    EXPECT_EQ(h1, h2) << "Same key must return the same handle";
    EXPECT_EQ(m_registry.GetEntryCount(), 1u) << "Should not create a second entry";
}

// Different GUIDs produce different handles (no false sharing).
TEST_F(MeshGPURegistryTest, DifferentGUIDs_ProduceDifferentHandles)
{
    GUID guid1 = GUID::Generate();
    GUID guid2 = GUID::Generate();
    Mesh mesh = MakeTriangleMesh();

    MeshGPUHandle h1 = m_registry.RegisterSubmesh({guid1, 0}, mesh);
    MeshGPUHandle h2 = m_registry.RegisterSubmesh({guid2, 0}, mesh);

    EXPECT_NE(h1, h2) << "Different GUIDs must produce different handles";
    EXPECT_EQ(m_registry.GetEntryCount(), 2u);
}

// Same GUID, different submesh indices produce different handles.
TEST_F(MeshGPURegistryTest, DifferentSubmeshIndices_ProduceDifferentHandles)
{
    GUID guid = GUID::Generate();
    Mesh mesh = MakeTriangleMesh();

    MeshGPUHandle h0 = m_registry.RegisterSubmesh({guid, 0}, mesh);
    MeshGPUHandle h1 = m_registry.RegisterSubmesh({guid, 1}, mesh);

    EXPECT_NE(h0, h1);
    EXPECT_EQ(m_registry.GetEntryCount(), 2u);
}

// FindByKey returns the correct entry.
TEST_F(MeshGPURegistryTest, FindByKey_ReturnsCorrectEntry)
{
    GUID guid = GUID::Generate();
    Mesh mesh = MakeTriangleMesh();
    MeshGPUKey key{guid, 0};

    MeshGPUHandle handle = m_registry.RegisterSubmesh(key, mesh);
    const MeshGPUEntry* entry = m_registry.FindByKey(key);

    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry, m_registry.Find(handle)) << "FindByKey and Find(handle) must return same pointer";
}

// FindByKey for unregistered key returns nullptr.
TEST_F(MeshGPURegistryTest, FindByKey_UnregisteredReturnsNull)
{
    GUID guid = GUID::Generate();
    MeshGPUKey key{guid, 0};

    EXPECT_EQ(m_registry.FindByKey(key), nullptr);
    EXPECT_FALSE(m_registry.FindHandle(key).IsValid());
}

// Multiple submeshes registered under the same GUID are all individually findable.
TEST_F(MeshGPURegistryTest, RegisterMultipleSubmeshes_AllFindable)
{
    GUID guid = GUID::Generate();
    Mesh mesh = MakeTriangleMesh();

    std::vector<MeshGPUHandle> handles;
    for (uint32_t i = 0; i < 3; ++i)
    {
        handles.push_back(m_registry.RegisterSubmesh({guid, i}, mesh));
    }
    EXPECT_EQ(m_registry.GetEntryCount(), 3u);

    for (uint32_t i = 0; i < 3; ++i)
    {
        EXPECT_NE(m_registry.FindByKey({guid, i}), nullptr);
    }
}

TEST_F(MeshGPURegistryTest, Uint32IndexOverflowCreatesSiblingPool)
{
    constexpr uint32_t kVertexCount = 70000u; // Forces uint32 index buffers.
    constexpr uint32_t kLargeIndexCount = 1050000u; // Just over the initial 4 MiB uint32 pool.

    GUID guid = GUID::Generate();

    Mesh smallUint32Mesh = MakeUint32IndexMesh(kVertexCount, 3u);
    MeshGPUHandle h0 = m_registry.RegisterSubmesh({guid, 0}, smallUint32Mesh);
    ASSERT_TRUE(h0.IsValid());
    const MeshGPUEntry* entry0 = m_registry.Find(h0);
    ASSERT_NE(entry0, nullptr);
    ASSERT_EQ(entry0->indexCount, 3u);

    ASSERT_EQ(m_registry.GetBucketIndexPoolCount(entry0->bucketKey, entry0->indexType), 1u);
    MeshGPUEntryBindings firstBindings{};
    ASSERT_TRUE(m_registry.TryGetDrawableBindings(*entry0, firstBindings));
    ASSERT_TRUE(firstBindings.indexBuffer.IsValid());

    Mesh largeUint32Mesh = MakeUint32IndexMesh(kVertexCount, kLargeIndexCount);
    MeshGPUHandle h1 = m_registry.RegisterSubmesh({guid, 1}, largeUint32Mesh);
    ASSERT_TRUE(h1.IsValid());
    const MeshGPUEntry* entry1 = m_registry.Find(h1);
    ASSERT_NE(entry1, nullptr);
    EXPECT_EQ(entry1->indexCount, kLargeIndexCount);

    EXPECT_GE(m_registry.GetBucketIndexPoolCount(entry1->bucketKey, entry1->indexType), 2u);
    EXPECT_EQ(entry1->indexPoolIndex, 1u);

    MeshGPUEntryBindings largeBindings{};
    ASSERT_TRUE(m_registry.TryGetDrawableBindings(*entry1, largeBindings));
    EXPECT_TRUE(largeBindings.indexBuffer.IsValid());
    EXPECT_NE(firstBindings.indexBuffer, largeBindings.indexBuffer);
}

// Mesh with only position (no normals, no UV) sets correct flags.
// The Vertex struct always includes position, normal, and UV0 fields, so the
// registry unconditionally sets those flags regardless of whether the data is
// meaningful. Verify that a mesh with zeroed normals/UVs still gets all core flags.
TEST_F(MeshGPURegistryTest, ZeroedNormalsUV_StillHasCoreFlags)
{
    GUID guid = GUID::Generate();
    Mesh mesh = MakeTriangleMesh(/*withNormals=*/false, /*withUV=*/false);
    MeshGPUKey key{guid, 0};

    MeshGPUHandle handle = m_registry.RegisterSubmesh(key, mesh);
    const MeshGPUEntry* entry = m_registry.Find(handle);

    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(HasFlag(entry->vertexFlags, VertexAttributeFlags::HasPosition));
    EXPECT_TRUE(HasFlag(entry->vertexFlags, VertexAttributeFlags::HasNormal));
    EXPECT_TRUE(HasFlag(entry->vertexFlags, VertexAttributeFlags::HasUV0));
}

// Skinned mesh sets the Skinned flags and creates joints/weights VBs.
TEST_F(MeshGPURegistryTest, SkinnedMesh_HasSkinnedFlags)
{
    GUID guid = GUID::Generate();
    Mesh mesh = MakeTriangleMesh(/*withNormals=*/true, /*withUV=*/true, /*skinned=*/true);
    MeshGPUKey key{guid, 0};

    MeshGPUHandle handle = m_registry.RegisterSubmesh(key, mesh);
    const MeshGPUEntry* entry = m_registry.Find(handle);

    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(IsSkinned(entry->vertexFlags));
    MeshGPUEntryBindings bindings{};
    ASSERT_TRUE(m_registry.TryGetDrawableBindings(*entry, bindings));
    EXPECT_TRUE(bindings.jointsVB.IsValid());
    EXPECT_TRUE(bindings.weightsVB.IsValid());
}

// Optional color/UV1 streams set their flags and create matching VBs.
TEST_F(MeshGPURegistryTest, ColorAndUV1Streams_HaveFlagsAndBindings)
{
    GUID guid = GUID::Generate();
    Mesh mesh = MakeTriangleMesh();
    mesh.Color0 = {
        1.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 1.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f, 1.0f,
    };
    mesh.TexCoords1 = {
        0.5f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f,
    };
    MeshGPUKey key{guid, 0};

    MeshGPUHandle handle = m_registry.RegisterSubmesh(key, mesh);
    const MeshGPUEntry* entry = m_registry.Find(handle);

    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(HasFlag(entry->vertexFlags, VertexAttributeFlags::HasColor));
    EXPECT_TRUE(HasFlag(entry->vertexFlags, VertexAttributeFlags::HasUV1));
    MeshGPUEntryBindings bindings{};
    ASSERT_TRUE(m_registry.TryGetDrawableBindings(*entry, bindings));
    EXPECT_TRUE(bindings.colorVB.IsValid());
    EXPECT_TRUE(bindings.uv1VB.IsValid());
}

// Meshes with more than four skin influences upload JOINTS_1/WEIGHTS_1 too.
TEST_F(MeshGPURegistryTest, EightWeightSkinning_HasSecondSkinningBindings)
{
    GUID guid = GUID::Generate();
    Mesh mesh = MakeTriangleMesh(/*withNormals=*/true, /*withUV=*/true, /*skinned=*/true);
    mesh.Joints1 = {2, 3, 0, 0,  2, 3, 0, 0,  2, 3, 0, 0};
    mesh.Weights1 = {0.0f, 0.0f, 0.0f, 0.0f,
                     0.25f, 0.25f, 0.0f, 0.0f,
                     0.25f, 0.25f, 0.0f, 0.0f};
    MeshGPUKey key{guid, 0};

    MeshGPUHandle handle = m_registry.RegisterSubmesh(key, mesh);
    const MeshGPUEntry* entry = m_registry.Find(handle);

    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(HasFlag(entry->vertexFlags, VertexAttributeFlags::HasJoints1));
    EXPECT_TRUE(HasFlag(entry->vertexFlags, VertexAttributeFlags::HasWeights1));
    MeshGPUEntryBindings bindings{};
    ASSERT_TRUE(m_registry.TryGetDrawableBindings(*entry, bindings));
    EXPECT_TRUE(bindings.joints1VB.IsValid());
    EXPECT_TRUE(bindings.weights1VB.IsValid());
}

TEST_F(MeshGPURegistryTest, UnregisterSubmesh_RemovesRuntimeKey)
{
    GUID guid = GUID::Generate();
    Mesh mesh = MakeTriangleMesh();
    MeshGPUKey key{guid, 0};

    MeshGPUHandle handle = m_registry.RegisterSubmesh(key, mesh);
    ASSERT_TRUE(handle.IsValid());
    EXPECT_EQ(m_registry.GetEntryCount(), 1u);

    EXPECT_TRUE(m_registry.UnregisterSubmesh(key));
    EXPECT_FALSE(m_registry.FindHandle(key).IsValid());
    EXPECT_EQ(m_registry.Find(handle), nullptr);
}

TEST_F(MeshGPURegistryTest, CreateMorphedMesh_AppliesDeltasAndBounds)
{
    Mesh mesh = MakeTriangleMesh();
    MorphTarget target{};
    target.Name = "raise_tip";
    target.VertexIndices = {0u, 2u};
    target.PositionDeltas = {
        0.0f, 2.0f, 0.0f,
        0.0f, 0.0f, 1.0f,
    };
    target.NormalDeltas = {
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f,
    };
    mesh.MorphTargets.push_back(std::move(target));

    const float weights[] = {0.5f};
    Mesh morphed = CreateMorphedMesh(mesh, weights, 1);

    EXPECT_TRUE(morphed.MorphTargets.empty());
    EXPECT_NEAR(morphed.Vertices[0].Position[1], 2.0f, 1e-5f);
    EXPECT_NEAR(morphed.Vertices[0].Normal[1], 0.4472136f, 1e-5f);
    EXPECT_NEAR(morphed.Vertices[0].Normal[2], 0.8944272f, 1e-5f);
    EXPECT_NEAR(morphed.Vertices[2].Position[2], mesh.Vertices[2].Position[2] + 0.5f, 1e-5f);
    for (int axis = 0; axis < 3; ++axis)
        EXPECT_EQ(morphed.Vertices[1].Position[axis], mesh.Vertices[1].Position[axis]);
    EXPECT_NEAR(morphed.MaxBounds[1], 2.0f, 1e-5f);
}

// Multiple entities referencing the same model share the same GPU entry.
// This simulates the ECS scenario where many entities have the same ModelAsset GUID.
TEST_F(MeshGPURegistryTest, MultipleEntitiesSameModel_ShareGPUResources)
{
    GUID modelGuid = GUID::Generate();
    Mesh mesh = MakeTriangleMesh();
    MeshGPUKey key{modelGuid, 0};

    // First entity registers.
    MeshGPUHandle h1 = m_registry.RegisterSubmesh(key, mesh);
    const MeshGPUEntry* entry1 = m_registry.Find(h1);

    // Second entity registers the same model (different entity, same asset GUID).
    MeshGPUHandle h2 = m_registry.RegisterSubmesh(key, mesh);
    const MeshGPUEntry* entry2 = m_registry.Find(h2);

    // They must be the exact same handle and entry.
    EXPECT_EQ(h1, h2);
    EXPECT_EQ(entry1, entry2);
    MeshGPUEntryBindings b1{};
    MeshGPUEntryBindings b2{};
    ASSERT_TRUE(m_registry.TryGetDrawableBindings(*entry1, b1));
    ASSERT_TRUE(m_registry.TryGetDrawableBindings(*entry2, b2));
    EXPECT_EQ(b1.coreVB, b2.coreVB) << "GPU buffers must be shared";
    EXPECT_EQ(b1.indexBuffer, b2.indexBuffer);
}

// Bounds are computed correctly from mesh min/max.
TEST_F(MeshGPURegistryTest, BoundsComputedCorrectly)
{
    GUID guid = GUID::Generate();
    Mesh mesh = MakeTriangleMesh();
    MeshGPUKey key{guid, 0};

    MeshGPUHandle handle = m_registry.RegisterSubmesh(key, mesh);
    const MeshGPUEntry* entry = m_registry.Find(handle);

    ASSERT_NE(entry, nullptr);
    // Center should be (0, 0, 0) for our test mesh bounds [-1,-1,0] to [1,1,0].
    EXPECT_NEAR(entry->bounds.center.x, 0.0f, 1e-5f);
    EXPECT_NEAR(entry->bounds.center.y, 0.0f, 1e-5f);
    EXPECT_NEAR(entry->bounds.center.z, 0.0f, 1e-5f);
    // Half-extents should be (1, 1, 0) for bounds [-1,-1,0] to [1,1,0].
    EXPECT_NEAR(entry->bounds.halfExtents.x, 1.0f, 1e-5f);
    EXPECT_NEAR(entry->bounds.halfExtents.y, 1.0f, 1e-5f);
    EXPECT_NEAR(entry->bounds.halfExtents.z, 0.0f, 1e-5f);
    // Radius should be sqrt(1+1+0) = sqrt(2) ~= 1.414
    EXPECT_NEAR(entry->bounds.Radius(), std::sqrt(2.0f), 1e-4f);
}

// RenderServices exposes MeshGPURegistry and it initializes correctly.
TEST_F(MeshGPURegistryTest, RenderServicesOwnsRegistry)
{
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(m_device.get()));

    auto& reg = rs.GetMeshGPURegistry();
    EXPECT_EQ(reg.GetEntryCount(), 0u);

    // Register something through the RenderServices-owned registry.
    GUID guid = GUID::Generate();
    Mesh mesh = MakeTriangleMesh();
    MeshGPUHandle h = reg.RegisterSubmesh({guid, 0}, mesh);
    EXPECT_TRUE(h.IsValid());
    EXPECT_EQ(reg.GetEntryCount(), 1u);

    rs.Shutdown();
}

// The content hash (drives the same-key re-upload path) must react to an
// authored-vertex-block edit, or an authored geometry change would re-upload
// stale vertices under the same handle. Verified via the entry's contentHash,
// which RegisterSubmesh recomputes and stores.
TEST_F(MeshGPURegistryTest, ContentHash_ChangesWhenAuthoredVerticesChange)
{
    GUID guid = GUID::Generate();
    MeshGPUKey key{guid, 0};

    Mesh mesh = MakeTriangleMesh();
    const uint64_t baseHash = m_registry.Find(m_registry.RegisterSubmesh(key, mesh))->contentHash;

    // Attach an authored LOD1 block (own vertices + LOD-local indices).
    Mesh authored = mesh;
    authored.ExtraLODVertices.push_back(mesh.Vertices);
    authored.ExtraLODs.push_back({0u, 1u, 2u});
    auto handle = m_registry.RegisterSubmesh(key, authored);
    const uint64_t authoredHash = m_registry.Find(handle)->contentHash;
    EXPECT_NE(authoredHash, baseHash)
        << "attaching an authored vertex block must change the content hash";

    // Editing a vertex inside the authored block must change it again.
    Mesh edited = authored;
    edited.ExtraLODVertices[0][1].Position[0] += 5.0f;
    m_registry.RegisterSubmesh(key, edited);
    EXPECT_NE(m_registry.Find(handle)->contentHash, authoredHash)
        << "editing an authored vertex must change the content hash";
}

// ExtraLODCoverage feeds lodThreshold from C2; a coverage-only edit must move
// the content hash or thresholds would go stale under the same handle.
TEST_F(MeshGPURegistryTest, ContentHash_ChangesWhenAuthoredCoverageChanges)
{
    GUID guid = GUID::Generate();
    MeshGPUKey key{guid, 0};

    Mesh mesh = MakeTriangleMesh();
    mesh.ExtraLODVertices.push_back(mesh.Vertices);
    mesh.ExtraLODs.push_back({0u, 1u, 2u});
    mesh.ExtraLODCoverage = {0.5f};
    auto handle = m_registry.RegisterSubmesh(key, mesh);
    const uint64_t withCoverage = m_registry.Find(handle)->contentHash;

    Mesh changed = mesh;
    changed.ExtraLODCoverage = {0.25f};
    m_registry.RegisterSubmesh(key, changed);
    EXPECT_NE(m_registry.Find(handle)->contentHash, withCoverage)
        << "an authored-coverage-only edit must change the content hash";
}

// Two identical non-authored meshes hash identically (empty authored fields are
// a constant term — the non-authored dedup path is unperturbed by Phase C1).
TEST_F(MeshGPURegistryTest, ContentHash_NonAuthoredMeshesHashIdentically)
{
    GUID guid = GUID::Generate();
    Mesh mesh = MakeTriangleMesh();
    auto h0 = m_registry.RegisterSubmesh({guid, 0}, mesh);
    auto h1 = m_registry.RegisterSubmesh({guid, 1}, mesh);
    EXPECT_EQ(m_registry.Find(h0)->contentHash, m_registry.Find(h1)->contentHash);
}

// Index width keys on the MAX PER-LEVEL vertex count, not the concatenated
// total: a 40k base + 30k authored LOD (70k concatenated, each level < 65535)
// stays u16, while a single 70k block is u32.
TEST_F(MeshGPURegistryTest, AuthoredIndexWidthKeysOnMaxPerLevelVertexCount)
{
    Mesh authored = MakeTriangleMesh();
    authored.Vertices.resize(40000);          // base level: 40k verts (u16-eligible)
    authored.Indices = {0u, 1u, 2u};          // tiny; width keys on vertex count
    authored.ExtraLODVertices.push_back(Vector<Vertex>(30000)); // own 30k verts
    authored.ExtraLODs.push_back({0u, 1u, 2u});                 // LOD-local

    auto ha = m_registry.RegisterSubmesh({GUID::Generate(), 0}, authored);
    const auto* ea = m_registry.Find(ha);
    ASSERT_NE(ea, nullptr);
    EXPECT_EQ(ea->indexType, static_cast<uint32_t>(IndexType::Uint16))
        << "each level's local indices are < 65535, so u16 despite 70k total";
    EXPECT_EQ(ea->lodVertexOffset[0], 0u);
    EXPECT_EQ(ea->lodVertexOffset[1], 40000u)
        << "authored LOD1 sits right after LOD0's 40k-vertex block";

    Mesh control = MakeTriangleMesh();
    control.Vertices.resize(70000);           // single 70k block => u32
    control.Indices = {0u, 1u, 2u};
    auto hc = m_registry.RegisterSubmesh({GUID::Generate(), 0}, control);
    EXPECT_EQ(m_registry.Find(hc)->indexType, static_cast<uint32_t>(IndexType::Uint32));
}

// --- Concurrent registration ---
//
// An ECS wave dispatches its systems onto job workers and runs them at once;
// more than one of them registers meshes (MorphTarget, EZTreeExtraction). These
// drive that shape directly: disjoint keys, no shared entry, nothing but the
// registry's own tables in common.

namespace
{
constexpr uint32_t kConcurrentThreads    = 8;
constexpr uint32_t kRegistrationsPerThread = 96;
constexpr uint32_t kUnregisterEvery      = 4; // every 4th key is unregistered again
} // namespace

// N threads x M registrations on disjoint keys. Without mutual exclusion the
// concurrent GenerationalVector::Create resizes, unordered_map rehashes and
// bucket-allocator mutations corrupt the tables (or crash outright); with it,
// every key lands exactly once and every surviving handle stays resolvable.
TEST_F(MeshGPURegistryTest, ConcurrentRegistration_DisjointKeys_KeepsTablesConsistent)
{
    const Mesh mesh = MakeTriangleMesh();

    // GUIDs are minted up front on this thread: the subject under test is the
    // registry's tables, not GUID::Generate's own thread safety.
    std::vector<std::vector<MeshGPUKey>> keys(kConcurrentThreads);
    for (uint32_t t = 0; t < kConcurrentThreads; ++t)
    {
        keys[t].reserve(kRegistrationsPerThread);
        for (uint32_t i = 0; i < kRegistrationsPerThread; ++i)
            keys[t].push_back(MeshGPUKey{GUID::Generate(), 0});
    }

    std::vector<std::vector<MeshGPUHandle>> handles(kConcurrentThreads);
    std::atomic<uint32_t> readyCount{0};
    std::atomic<bool>     start{false};

    std::vector<std::thread> workers;
    workers.reserve(kConcurrentThreads);
    for (uint32_t t = 0; t < kConcurrentThreads; ++t)
    {
        workers.emplace_back([&, t]() {
            handles[t].resize(kRegistrationsPerThread);
            readyCount.fetch_add(1u, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();

            for (uint32_t i = 0; i < kRegistrationsPerThread; ++i)
            {
                handles[t][i] = m_registry.RegisterSubmesh(keys[t][i], mesh);
                if ((i % kUnregisterEvery) == (kUnregisterEvery - 1u))
                    m_registry.UnregisterSubmesh(keys[t][i]);
            }
        });
    }

    while (readyCount.load(std::memory_order_acquire) < kConcurrentThreads)
        std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (std::thread& worker : workers)
        worker.join();

    // Post-conditions, evaluated on this thread with the workers joined.
    std::unordered_set<uint64_t> distinctHandles;
    uint32_t survivors = 0;
    for (uint32_t t = 0; t < kConcurrentThreads; ++t)
    {
        for (uint32_t i = 0; i < kRegistrationsPerThread; ++i)
        {
            const bool unregistered = (i % kUnregisterEvery) == (kUnregisterEvery - 1u);
            const MeshGPUHandle handle = handles[t][i];
            ASSERT_TRUE(handle.IsValid()) << "thread " << t << " registration " << i;

            if (unregistered)
            {
                EXPECT_FALSE(m_registry.FindHandle(keys[t][i]).IsValid())
                    << "unregistered key still resolves (thread " << t << ", " << i << ")";
                continue;
            }

            ++survivors;
            EXPECT_TRUE(distinctHandles.insert(static_cast<uint64_t>(handle)).second)
                << "two live registrations share one handle (thread " << t << ", " << i << ")";

            const MeshGPUEntry* entry = m_registry.Find(handle);
            ASSERT_NE(entry, nullptr) << "live handle stopped resolving (thread " << t << ", " << i << ")";
            EXPECT_EQ(entry->indexCount, 3u) << "entry content torn (thread " << t << ", " << i << ")";
            EXPECT_EQ(m_registry.FindHandle(keys[t][i]), handle)
                << "dedup map lost a key (thread " << t << ", " << i << ")";
        }
    }

    EXPECT_EQ(m_registry.GetEntryCount(), survivors)
        << "live entry count disagrees with the registrations that were not unregistered";
    EXPECT_EQ(m_registry.GetRegisteredModelCount(), survivors)
        << "per-asset index disagrees with the surviving registrations";
}

// The wave-side shape: one system registering while another reads. The reader
// holds a TableScope, which is what makes its lookups and the entries they hand
// back safe against the registrar's table resizes.
TEST_F(MeshGPURegistryTest, ScopedLookupsAreConsistentBesideConcurrentRegistration)
{
    const Mesh mesh = MakeTriangleMesh();

    // Pre-registered keys the reader walks; the registrar only ever touches its
    // own disjoint keys, so any inconsistency the reader sees is a table race.
    std::vector<MeshGPUKey>    readerKeys;
    std::vector<MeshGPUHandle> readerHandles;
    for (uint32_t i = 0; i < 32u; ++i)
    {
        const MeshGPUKey key{GUID::Generate(), 0};
        readerKeys.push_back(key);
        readerHandles.push_back(m_registry.RegisterSubmesh(key, mesh));
    }

    std::vector<MeshGPUKey> registrarKeys;
    for (uint32_t i = 0; i < kRegistrationsPerThread; ++i)
        registrarKeys.push_back(MeshGPUKey{GUID::Generate(), 0});

    std::atomic<bool>     start{false};
    std::atomic<uint32_t> mismatches{0};
    std::atomic<uint32_t> lookups{0};

    std::thread registrar([&]() {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (const MeshGPUKey& key : registrarKeys)
            m_registry.RegisterSubmesh(key, mesh);
    });

    std::thread reader([&]() {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (uint32_t pass = 0; pass < 64u; ++pass)
        {
            MeshGPURegistry::TableScope scope(m_registry);
            for (size_t i = 0; i < readerKeys.size(); ++i)
            {
                const MeshGPUHandle handle = m_registry.FindHandle(readerKeys[i]);
                const MeshGPUEntry* entry  = m_registry.Find(handle);
                lookups.fetch_add(1u, std::memory_order_relaxed);
                if (handle != readerHandles[i] || !entry || entry->indexCount != 3u)
                    mismatches.fetch_add(1u, std::memory_order_relaxed);
            }
        }
    });

    start.store(true, std::memory_order_release);
    registrar.join();
    reader.join();

    EXPECT_EQ(mismatches.load(), 0u) << "a scoped reader observed a table mid-mutation";
    EXPECT_EQ(lookups.load(), 64u * readerKeys.size()) << "the reader did not run its passes";
    EXPECT_EQ(m_registry.GetEntryCount(), readerKeys.size() + registrarKeys.size());
}

// GetPoolStats is the only witness to pool fragmentation, which converts into
// GPU memory growth and extra pool groups: a failed best-fit does not fail the
// upload, it appends a sibling pool at double the capacity. The editor reads it
// over IPC (renderServices.meshPools), so the shape it reports is a contract.
TEST_F(MeshGPURegistryTest, PoolStatsReportOneEntryPerLivePoolWithItsOccupancy)
{
    EXPECT_TRUE(m_registry.GetPoolStats().empty()) << "no pools exist before the first upload";

    GUID guid = GUID::Generate();
    Mesh mesh = MakeTriangleMesh();
    MeshGPUHandle handle = m_registry.RegisterSubmesh({guid, 0}, mesh);
    ASSERT_TRUE(handle.IsValid());
    const MeshGPUEntry* entry = m_registry.Find(handle);
    ASSERT_NE(entry, nullptr);

    const std::vector<MeshGPUPoolStats> stats = m_registry.GetPoolStats();
    ASSERT_FALSE(stats.empty());

    // A core stream and an index stream at minimum, both in this mesh's bucket.
    const auto findStream = [&stats](const char* stream) -> const MeshGPUPoolStats*
    {
        for (const MeshGPUPoolStats& s : stats)
        {
            if (std::strcmp(s.Stream, stream) == 0)
                return &s;
        }
        return nullptr;
    };

    const MeshGPUPoolStats* core = findStream("Core");
    ASSERT_NE(core, nullptr) << "every registered mesh occupies a core pool";
    EXPECT_EQ(core->BucketKey, entry->bucketKey);
    EXPECT_EQ(core->PoolIndex, 0u);
    EXPECT_GT(core->Allocator.capacity, 0u);
    EXPECT_GT(core->Allocator.used, 0u);
    EXPECT_LE(core->Allocator.used, core->Allocator.capacity);
    EXPECT_GE(core->Allocator.peak, core->Allocator.used);
    EXPECT_LE(core->Allocator.largestFreeRange, core->Allocator.capacity);

    ASSERT_NE(findStream("IB16"), nullptr) << "a 3-index triangle uses the 16-bit index pool";

    // One entry per live pool buffer, so every reported stream is one this
    // bucket actually created — never a placeholder row for an empty stream.
    for (const MeshGPUPoolStats& s : stats)
    {
        EXPECT_EQ(s.BucketKey, entry->bucketKey);
        EXPECT_GT(s.Allocator.capacity, 0u) << "stream " << s.Stream;
    }
}

// The residency log latches on the first pool of a session and stays quiet
// while later pools match it — so a device rebuild, which frees every pool that
// set the latch, must clear it. Otherwise the rebuilt device's pools land
// silently and the header's claim that any later statement about mesh-pool
// residency rests on this observation is false on exactly the path that
// re-decides residency.
TEST_F(MeshGPURegistryTest, PoolResidencyIsLoggedAgainAfterADeviceRebuild)
{
    Logger::Log::Initialize({});
    auto sink = Logger::MakeUnique<Logger::CallbackSink>();
    auto* sinkPtr = sink.get();
    auto residencyLines = std::make_shared<std::atomic<int>>(0);
    const Logger::uint64 callbackId = sinkPtr->RegisterCallback(
        [residencyLines](const Logger::LogMessage& msg)
        {
            if (msg.Message.find("MeshGPURegistry pool") != Logger::String::npos)
                residencyLines->fetch_add(1);
        });
    Logger::Log::AddSink(std::move(sink));

    ASSERT_TRUE(m_registry.RegisterSubmesh({GUID::Generate(), 0}, MakeTriangleMesh()).IsValid());
    Logger::Log::Flush();
    const int afterFirstUpload = residencyLines->load();
    EXPECT_GT(afterFirstUpload, 0) << "the first pool of the session must report where it landed";

    // No source lookup: every entry tombstones, which is fine — what matters is
    // that the pools (and the latch they set) are gone.
    m_registry.ReprovisionAfterDeviceRebuild([](const MeshGPUKey&) -> MeshGPUCpuSource { return {}; });

    ASSERT_TRUE(m_registry.RegisterSubmesh({GUID::Generate(), 0}, MakeTriangleMesh()).IsValid());
    Logger::Log::Flush();
    EXPECT_GT(residencyLines->load(), afterFirstUpload)
        << "post-rebuild pools were created but logged nothing: the first-pool latch survived the "
           "rebuild, so where the rebuilt device's mesh pools landed is unobservable";

    sinkPtr->UnregisterCallback(callbackId);
}
