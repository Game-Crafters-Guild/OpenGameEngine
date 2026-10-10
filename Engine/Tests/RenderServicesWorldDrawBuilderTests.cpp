#include <gtest/gtest.h>

#include <algorithm>
#include <span>
#include <vector>

#include "Engine/Rendering/CpuDrawStreamBuilder.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/WorldDrawTypes.h"
#include "Assets/ModelAsset.h"
#include "AssetCore/GUID.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

#include "TestDeviceHelper.h"

namespace
{

// Build a minimal mesh and register it in the RenderServices' MeshGPURegistry
// so the WorldDrawBuilder's resident-mesh guard accepts submissions for it.
Rendering::MeshGPUHandle RegisterTestMesh(RenderServices& rs)
{
    Mesh m{};
    m.Name = "TestTriangle";
    Vertex v0{}, v1{}, v2{};
    v0.Position[0] = 0.0f; v0.Position[1] = 1.0f; v0.Position[2] = 0.0f;
    v1.Position[0] = -1.0f; v1.Position[1] = -1.0f; v1.Position[2] = 0.0f;
    v2.Position[0] = 1.0f; v2.Position[1] = -1.0f; v2.Position[2] = 0.0f;
    v0.Normal[2] = 1.0f; v1.Normal[2] = 1.0f; v2.Normal[2] = 1.0f;
    m.Vertices = {v0, v1, v2};
    m.Indices = {0, 1, 2};
    return rs.GetMeshGPURegistry().RegisterSubmesh({GUID::Generate(), 0}, m);
}

} // namespace

// Basic harness: a single WorldSubmissionRecord should produce a single
// batch key carrying the same Material* and mesh handle.
TEST(RenderServicesWorldDrawBuilderTests, SingleSubmission_ProducesExpectedBatchKey)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
    {
        GTEST_SKIP() << "No Vulkan device available";
    }

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    Material testMaterial = Material::TestFactory::Create(GUID::Generate(), "TestMaterial", 32u);
    Material::TestFactory::SetGraphicsPipelineId(testMaterial, Rendering::GraphicsPipelineId{1u});
    Material::TestFactory::SetGpuSceneMaterialIndex(testMaterial, 1u);
    Rendering::MeshGPUHandle testMeshHandle = RegisterTestMesh(rs);

    rs.BeginWorldDrawFrame();

    WorldSubmissionRecord rec{};
    rec.viewId         = 42u;
    rec.meshHandle     = testMeshHandle;
    rec.material       = &testMaterial;
    rec.instanceIndex  = 7u;
    rec.renderLayerMask = 0u;
    rec.flags           = 0u;

    std::span<const WorldSubmissionRecord> batch(&rec, 1u);
    rs.SubmitWorldSubmissions(batch);
    rs.BuildWorldBatchKeys();

    auto keys = rs.GetEntityBatchKeys(rec.viewId);
    ASSERT_EQ(keys.size(), 1u);
    EXPECT_EQ(keys[0].material, &testMaterial);
    EXPECT_EQ(keys[0].mesh, testMeshHandle);
    EXPECT_EQ(keys[0].materialIndex, testMaterial.GetGpuSceneMaterialIndex());
    const auto* entry = rs.GetMeshGPURegistry().Find(testMeshHandle);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(keys[0].meshIndex, entry->gpuMeshIndex);

    rs.Shutdown();
    device->Shutdown();
}

// Per-view isolation: submissions targeting different view IDs should end up
// in disjoint batch-key sets, and BeginWorldDrawFrame should clear previous
// state.
TEST(RenderServicesWorldDrawBuilderTests, MultipleViews_ProduceIsolatedBatchKeys)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
    {
        GTEST_SKIP() << "No Vulkan device available";
    }

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    Material testMaterial = Material::TestFactory::Create(GUID::Generate(), "TestMaterial", 32u);
    Material::TestFactory::SetGraphicsPipelineId(testMaterial, Rendering::GraphicsPipelineId{1u});
    Material::TestFactory::SetGpuSceneMaterialIndex(testMaterial, 1u);
    Rendering::MeshGPUHandle testMeshHandle = RegisterTestMesh(rs);

    rs.BeginWorldDrawFrame();

    WorldSubmissionRecord recA{};
    recA.viewId        = 11u;
    recA.meshHandle    = testMeshHandle;
    recA.material      = &testMaterial;
    recA.instanceIndex = 1u;

    WorldSubmissionRecord recB{};
    recB.viewId        = 22u;
    recB.meshHandle    = testMeshHandle;
    recB.material      = &testMaterial;
    recB.instanceIndex = 2u;

    WorldSubmissionRecord records[2] = { recA, recB };
    rs.SubmitWorldSubmissions(std::span<const WorldSubmissionRecord>(records, 2u));
    rs.BuildWorldBatchKeys();

    auto keysA = rs.GetEntityBatchKeys(recA.viewId);
    auto keysB = rs.GetEntityBatchKeys(recB.viewId);
    ASSERT_EQ(keysA.size(), 1u);
    ASSERT_EQ(keysB.size(), 1u);

    EXPECT_EQ(keysA[0].material, &testMaterial);
    EXPECT_EQ(keysB[0].material, &testMaterial);

    // Unused view should yield an empty span.
    auto keysNone = rs.GetEntityBatchKeys(9999u);
    EXPECT_TRUE(keysNone.empty());

    // A new frame should clear previous state.
    rs.BeginWorldDrawFrame();
    rs.BuildWorldBatchKeys();
    EXPECT_TRUE(rs.GetEntityBatchKeys(recA.viewId).empty());
    EXPECT_TRUE(rs.GetEntityBatchKeys(recB.viewId).empty());

    rs.Shutdown();
    device->Shutdown();
}

// Dedup behaviour: multiple submissions with the same (Material*, mesh) pair
// for a single view collapse into one BatchKey, while distinct materials each
// produce their own entry. An empty color-class map is the merge-OFF signal:
// driven through WorldDrawBuilder with one, every key keeps
// colorClassId == materialIndex.
TEST(RenderServicesWorldDrawBuilderTests, DedupAndDistinctMaterials_ProduceUniqueBatchKeys)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
    {
        GTEST_SKIP() << "No Vulkan device available";
    }

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    Material matA = Material::TestFactory::Create(GUID::Generate(), "MaterialA", 32u);
    Material::TestFactory::SetGraphicsPipelineId(matA, Rendering::GraphicsPipelineId{1u});
    Material::TestFactory::SetGpuSceneMaterialIndex(matA, 1u);
    Material matB = Material::TestFactory::Create(GUID::Generate(), "MaterialB", 32u);
    Material::TestFactory::SetGraphicsPipelineId(matB, Rendering::GraphicsPipelineId{1u});
    Material::TestFactory::SetGpuSceneMaterialIndex(matB, 2u);

    const uint32 viewId = 77u;
    Rendering::MeshGPUHandle testMeshHandle = RegisterTestMesh(rs);

    // Three submissions: two using matA (should dedup), one using matB.
    WorldSubmissionRecord records[3]{};
    records[0].viewId        = viewId;
    records[0].meshHandle    = testMeshHandle;
    records[0].material      = &matA;
    records[0].instanceIndex = 0u;

    records[1].viewId        = viewId;
    records[1].meshHandle    = testMeshHandle;
    records[1].material      = &matA;
    records[1].instanceIndex = 1u;

    records[2].viewId        = viewId;
    records[2].meshHandle    = testMeshHandle;
    records[2].material      = &matB;
    records[2].instanceIndex = 2u;

    rs.BeginWorldDrawFrame();
    rs.SubmitWorldSubmissions(std::span<const WorldSubmissionRecord>(records, 3u));
    rs.BuildWorldBatchKeys();

    auto keys = rs.GetEntityBatchKeys(viewId);
    ASSERT_EQ(keys.size(), 2u);
    // The set must contain one entry per distinct material.
    bool sawA = false;
    bool sawB = false;
    for (const auto& key : keys)
    {
        if (key.material == &matA) sawA = true;
        if (key.material == &matB) sawB = true;
    }
    EXPECT_TRUE(sawA);
    EXPECT_TRUE(sawB);

    WorldDrawBuilder wdb;
    wdb.BeginFrame();
    wdb.Submit(std::span<const WorldSubmissionRecord>(records, 3u));
    wdb.BuildBatchKeys(rs.GetMeshGPURegistry(), /*materialColorClass=*/{});
    const auto identityKeys = wdb.GetBatchKeys(viewId);
    ASSERT_EQ(identityKeys.size(), 2u);
    for (const auto& k : identityKeys)
        EXPECT_EQ(k.colorClassId, k.materialIndex) << "empty map = identity classId";

    rs.Shutdown();
    device->Shutdown();
}

// P2 color-class merge (driven directly through WorldDrawBuilder with an
// explicit map, independent of RenderServices' own map build): same-class
// materials on a mesh collapse to ONE key whose representative is
// the lowest materialIndex of the class, deterministically regardless of
// submission order; a different-class material keeps its own key.
TEST(RenderServicesWorldDrawBuilderTests, ColorClassMerge_DedupsByClassLowestMaterialRepresentative)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    Rendering::MeshGPUHandle mesh = RegisterTestMesh(rs);

    Material matA = Material::TestFactory::Create(GUID::Generate(), "MatA", 32u);
    Material::TestFactory::SetGraphicsPipelineId(matA, Rendering::GraphicsPipelineId{1u});
    Material::TestFactory::SetGpuSceneMaterialIndex(matA, 5u);
    Material matB = Material::TestFactory::Create(GUID::Generate(), "MatB", 32u);
    Material::TestFactory::SetGraphicsPipelineId(matB, Rendering::GraphicsPipelineId{1u});
    Material::TestFactory::SetGpuSceneMaterialIndex(matB, 2u);
    Material matC = Material::TestFactory::Create(GUID::Generate(), "MatC", 32u);
    Material::TestFactory::SetGraphicsPipelineId(matC, Rendering::GraphicsPipelineId{1u});
    Material::TestFactory::SetGpuSceneMaterialIndex(matC, 9u);

    // Map: matA(5) + matB(2) share color class C0; matC(9) keeps identity.
    const uint32_t C0 = Rendering::GPUDrawStreamBuilder::kColorClassBase;
    std::vector<uint32_t> map(10u);
    for (uint32_t i = 0; i < map.size(); ++i)
        map[i] = i;
    map[5] = C0;
    map[2] = C0;

    const uint32 viewId = 3u;
    // Submit out of materialIndex order to prove the representative is chosen by
    // the sort, not by submission order.
    WorldSubmissionRecord recs[3]{};
    recs[0].viewId = viewId; recs[0].meshHandle = mesh; recs[0].material = &matA;
    recs[1].viewId = viewId; recs[1].meshHandle = mesh; recs[1].material = &matC;
    recs[2].viewId = viewId; recs[2].meshHandle = mesh; recs[2].material = &matB;

    WorldDrawBuilder wdb;
    wdb.BeginFrame();
    wdb.Submit(std::span<const WorldSubmissionRecord>(recs, 3u));
    wdb.BuildBatchKeys(rs.GetMeshGPURegistry(), map);

    auto keys = wdb.GetBatchKeys(viewId);
    ASSERT_EQ(keys.size(), 2u);

    const WorldDrawBuilder::BatchKey* mergedKey = nullptr;
    const WorldDrawBuilder::BatchKey* identityKey = nullptr;
    for (const auto& k : keys)
        (k.colorClassId == C0 ? mergedKey : identityKey) = &k;
    ASSERT_NE(mergedKey, nullptr);
    ASSERT_NE(identityKey, nullptr);

    // Merged class C0: representative is the LOWEST materialIndex (matB = 2).
    EXPECT_EQ(mergedKey->colorClassId, C0);
    EXPECT_EQ(mergedKey->materialIndex, 2u);
    EXPECT_EQ(mergedKey->material, &matB);
    // matC keeps identity (colorClassId == materialIndex).
    EXPECT_EQ(identityKey->colorClassId, 9u);
    EXPECT_EQ(identityKey->materialIndex, 9u);
    EXPECT_EQ(identityKey->material, &matC);

    rs.Shutdown();
    device->Shutdown();
}


// --- Compatibility profile: CpuDrawStreamBuilder ---
//
// A device without buffer_device_address cannot reach the GPU scatter's
// indirection buffer, so the visible set, the batch keying and the run splits
// are all resolved on the CPU. These pin the three things the recorders depend
// on: run coalescing (consecutive GPUScene indices become ONE instanced draw),
// the frustum cull, and the caster/camera set split.
namespace
{

// Identity view-proj: the frustum is the box x,y in [-1,1], z in [0,1] (the
// engine's reverse-Z clip volume), so a test instance's visibility is decided
// by its world-space bounding centre alone.
Rendering::CameraData MakeIdentityFrustumCamera()
{
    Rendering::CameraData cam{};
    for (int i = 0; i < 16; ++i)
    {
        const float identity = (i % 5 == 0) ? 1.0f : 0.0f;
        cam.view[i] = identity;
        cam.proj[i] = identity;
        cam.viewProj[i] = identity;
        cam.viewRel[i] = identity;
        cam.viewProjRel[i] = identity;
    }
    cam.cameraPos[3] = 0.0f;
    return cam;
}

// A zero-radius instance at `center`. Zero radius keeps TestSphereFrustum's
// 1.5x edge inflation out of the expectations.
uint32_t AddPointInstance(Rendering::GPUScene& scene, float x, float y, float z)
{
    Rendering::GPUInstance inst{};
    inst.boundingCenter = Rendering::Vector3(x, y, z);
    inst.boundingRadius = 0.0f;
    return scene.AddInstance(inst);
}

WorldSubmissionRecord MakeRecord(uint32 viewId, Rendering::MeshGPUHandle mesh,
                                 const Material& material, uint32_t instanceIndex, uint32 flags)
{
    WorldSubmissionRecord rec{};
    rec.viewId = viewId;
    rec.meshHandle = mesh;
    rec.material = &material;
    rec.instanceIndex = instanceIndex;
    rec.flags = flags;
    return rec;
}

} // namespace

// One batch is one instanced draw however its GPUScene indices are spread: a
// gap in the indices, or an out-of-order submission, must not split it. The
// visible instances land contiguously in the view's index list, in GPUScene
// order, each once — a duplicate submission would otherwise draw twice.
TEST(RenderServicesWorldDrawBuilderTests, CpuDrawStream_ListsOneBatchAsOneDrawWhateverTheGaps)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    Rendering::GPUScene* scene = rs.GetGPUScene();
    ASSERT_NE(scene, nullptr);

    Rendering::MeshGPUHandle mesh = RegisterTestMesh(rs);
    const auto* entry = rs.GetMeshGPURegistry().Find(mesh);
    ASSERT_NE(entry, nullptr);

    Material mat = Material::TestFactory::Create(GUID::Generate(), "Mat", 32u);
    Material::TestFactory::SetGraphicsPipelineId(mat, Rendering::GraphicsPipelineId{1u});
    Material::TestFactory::SetGpuSceneMaterialIndex(mat, 5u);

    // Five in-frustum instances. The scene may already hold instances from
    // RenderServices' own initialization, so capture the indices it hands back
    // rather than assuming they start at zero.
    std::vector<uint32_t> idx;
    for (int i = 0; i < 5; ++i)
        idx.push_back(AddPointInstance(*scene, 0.0f, 0.0f, 0.5f));
    ASSERT_EQ(idx[4], idx[0] + 4u) << "GPUScene handed out non-consecutive indices";

    const CameraId camId = rs.Views().AllocateCamera("cam");
    rs.Views().SetCameraData(camId, MakeIdentityFrustumCamera());
    const ViewId viewId = rs.Views().AllocateView("view", camId);

    // Submitted deliberately out of order, with a hole at idx[2] and idx[0]
    // submitted twice.
    const WorldSubmissionRecord recs[] = {
        MakeRecord(viewId, mesh, mat, idx[3], 0u),
        MakeRecord(viewId, mesh, mat, idx[0], 0u),
        MakeRecord(viewId, mesh, mat, idx[4], 0u),
        MakeRecord(viewId, mesh, mat, idx[1], 0u),
        MakeRecord(viewId, mesh, mat, idx[0], 0u),
    };

    WorldDrawBuilder wdb;
    wdb.BeginFrame();
    wdb.Submit(std::span<const WorldSubmissionRecord>(recs, 5u));
    wdb.BuildBatchKeys(rs.GetMeshGPURegistry());

    CpuDrawStreamBuilder cds;
    cds.Build(wdb, rs.GetMeshGPURegistry(), *scene, rs.Views(), nullptr);

    const CpuDrawStreamBuilder::InstanceList batch = cds.GetInstances(
        viewId, 5u, entry->gpuMeshIndex, CpuDrawStreamBuilder::InstanceSet::Camera);
    ASSERT_EQ(batch.Count, 4u) << "one draw over idx0, idx1, idx3, idx4";
    EXPECT_EQ(cds.GetStats().CameraBatches, 1u);
    const std::span<const uint32_t> list = cds.GetIndexList(viewId);
    ASSERT_GE(list.size(), batch.First + batch.Count);
    const std::vector<uint32_t> drawn(list.begin() + batch.First,
                                      list.begin() + batch.First + batch.Count);
    EXPECT_EQ(drawn, (std::vector<uint32_t>{idx[0], idx[1], idx[3], idx[4]}));

    // A batch key that was never submitted draws nothing.
    EXPECT_EQ(cds.GetInstances(viewId, 9u, entry->gpuMeshIndex,
                               CpuDrawStreamBuilder::InstanceSet::Camera).Count, 0u);

    rs.Shutdown();
    device->Shutdown();
}

// The frustum cull removes instances from the CAMERA set only. A caster
// outside the camera frustum still casts into it, so the shadow set keeps it —
// and a non-caster never reaches the shadow set at all.
TEST(RenderServicesWorldDrawBuilderTests, CpuDrawStream_CullsCameraSetButKeepsOffscreenCasters)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    Rendering::GPUScene* scene = rs.GetGPUScene();
    ASSERT_NE(scene, nullptr);

    Rendering::MeshGPUHandle mesh = RegisterTestMesh(rs);
    const auto* entry = rs.GetMeshGPURegistry().Find(mesh);
    ASSERT_NE(entry, nullptr);

    Material mat = Material::TestFactory::Create(GUID::Generate(), "Mat", 32u);
    Material::TestFactory::SetGraphicsPipelineId(mat, Rendering::GraphicsPipelineId{1u});
    Material::TestFactory::SetGpuSceneMaterialIndex(mat, 7u);

    const uint32_t inFrustumCaster = AddPointInstance(*scene, 0.0f, 0.0f, 0.5f);
    const uint32_t offscreenCaster = AddPointInstance(*scene, 100.0f, 0.0f, 0.5f);
    const uint32_t inFrustumNonCaster = AddPointInstance(*scene, 0.5f, 0.5f, 0.5f);

    const CameraId camId = rs.Views().AllocateCamera("cam");
    rs.Views().SetCameraData(camId, MakeIdentityFrustumCamera());
    const ViewId viewId = rs.Views().AllocateView("view", camId);

    const WorldSubmissionRecord recs[] = {
        MakeRecord(viewId, mesh, mat, inFrustumCaster, kSubmissionFlagCastShadows),
        MakeRecord(viewId, mesh, mat, offscreenCaster, kSubmissionFlagCastShadows),
        MakeRecord(viewId, mesh, mat, inFrustumNonCaster, 0u),
    };

    WorldDrawBuilder wdb;
    wdb.BeginFrame();
    wdb.Submit(std::span<const WorldSubmissionRecord>(recs, 3u));
    wdb.BuildBatchKeys(rs.GetMeshGPURegistry());

    CpuDrawStreamBuilder cds;
    cds.Build(wdb, rs.GetMeshGPURegistry(), *scene, rs.Views(), nullptr);

    // Camera set: the two in-frustum instances.
    const CpuDrawStreamBuilder::InstanceList camera = cds.GetInstances(
        viewId, 7u, entry->gpuMeshIndex, CpuDrawStreamBuilder::InstanceSet::Camera);
    EXPECT_EQ(camera.Count, 2u) << "the offscreen instance must be culled";
    EXPECT_EQ(cds.GetStats().CameraVisible, 2u);

    // Shadow set: both casters, including the offscreen one; the non-caster is
    // absent.
    const CpuDrawStreamBuilder::InstanceList shadow = cds.GetInstances(
        viewId, 7u, entry->gpuMeshIndex, CpuDrawStreamBuilder::InstanceSet::ShadowCasters);
    ASSERT_EQ(shadow.Count, 2u) << "both casters draw, camera-culled or not";
    const std::span<const uint32_t> list = cds.GetIndexList(viewId);
    ASSERT_GE(list.size(), shadow.First + shadow.Count);
    EXPECT_EQ(list[shadow.First], inFrustumCaster);
    EXPECT_EQ(list[shadow.First + 1u], offscreenCaster);
    EXPECT_EQ(cds.GetStats().ShadowCasters, 2u);

    rs.Shutdown();
    device->Shutdown();
}

// Different materials over one mesh stay separate batches (the compat profile
// turns the colour-class merge off, so colorClassId == materialIndex), and a
// frame with no submissions clears the lists rather than replaying the last.
TEST(RenderServicesWorldDrawBuilderTests, CpuDrawStream_KeysPerMaterialAndClearsBetweenFrames)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    Rendering::GPUScene* scene = rs.GetGPUScene();
    ASSERT_NE(scene, nullptr);

    Rendering::MeshGPUHandle mesh = RegisterTestMesh(rs);
    const auto* entry = rs.GetMeshGPURegistry().Find(mesh);
    ASSERT_NE(entry, nullptr);

    Material matA = Material::TestFactory::Create(GUID::Generate(), "MatA", 32u);
    Material::TestFactory::SetGraphicsPipelineId(matA, Rendering::GraphicsPipelineId{1u});
    Material::TestFactory::SetGpuSceneMaterialIndex(matA, 5u);
    Material matB = Material::TestFactory::Create(GUID::Generate(), "MatB", 32u);
    Material::TestFactory::SetGraphicsPipelineId(matB, Rendering::GraphicsPipelineId{1u});
    Material::TestFactory::SetGpuSceneMaterialIndex(matB, 2u);

    // Adjacent indices under DIFFERENT materials must not merge into one draw.
    const uint32_t instA = AddPointInstance(*scene, 0.0f, 0.0f, 0.5f);
    const uint32_t instB = AddPointInstance(*scene, 0.0f, 0.0f, 0.5f);
    ASSERT_EQ(instB, instA + 1u);

    const CameraId camId = rs.Views().AllocateCamera("cam");
    rs.Views().SetCameraData(camId, MakeIdentityFrustumCamera());
    const ViewId viewId = rs.Views().AllocateView("view", camId);

    const WorldSubmissionRecord recs[] = {
        MakeRecord(viewId, mesh, matA, instA, 0u),
        MakeRecord(viewId, mesh, matB, instB, 0u),
    };

    WorldDrawBuilder wdb;
    wdb.BeginFrame();
    wdb.Submit(std::span<const WorldSubmissionRecord>(recs, 2u));
    wdb.BuildBatchKeys(rs.GetMeshGPURegistry());

    CpuDrawStreamBuilder cds;
    cds.Build(wdb, rs.GetMeshGPURegistry(), *scene, rs.Views(), nullptr);

    const std::span<const uint32_t> list = cds.GetIndexList(viewId);
    const CpuDrawStreamBuilder::InstanceList a = cds.GetInstances(
        viewId, 5u, entry->gpuMeshIndex, CpuDrawStreamBuilder::InstanceSet::Camera);
    ASSERT_EQ(a.Count, 1u);
    ASSERT_LT(a.First, list.size());
    EXPECT_EQ(list[a.First], instA);

    const CpuDrawStreamBuilder::InstanceList b = cds.GetInstances(
        viewId, 2u, entry->gpuMeshIndex, CpuDrawStreamBuilder::InstanceSet::Camera);
    ASSERT_EQ(b.Count, 1u);
    ASSERT_LT(b.First, list.size());
    EXPECT_EQ(list[b.First], instB);
    EXPECT_EQ(cds.GetStats().CameraBatches, 2u);

    wdb.BeginFrame();
    wdb.BuildBatchKeys(rs.GetMeshGPURegistry());
    cds.Build(wdb, rs.GetMeshGPURegistry(), *scene, rs.Views(), nullptr);
    EXPECT_EQ(cds.GetInstances(viewId, 5u, entry->gpuMeshIndex,
                               CpuDrawStreamBuilder::InstanceSet::Camera).Count, 0u);
    EXPECT_TRUE(cds.GetIndexList(viewId).empty());
    EXPECT_EQ(cds.GetStats().CameraBatches, 0u);
    rs.Shutdown();
    device->Shutdown();
}

// #1014 — the glass-tint shadow cascade must key on transmissive CASTER presence.
// The builder derives that flag in the same pass as the batch keys, so it carries
// identical staleness and costs one branch in a loop the frame already walks.
// Every case below submits the SAME transmissive material and mesh; only the
// instance's castsShadows bit (WorldSubmissionRecord::flags bit 0) moves.
TEST(RenderServicesWorldDrawBuilderTests, TransmissiveCasterPredicateFollowsTheCastShadowsBit)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    Rendering::MeshGPUHandle mesh = RegisterTestMesh(rs);

    Material glass = Material::TestFactory::Create(GUID::Generate(), "Glass", 32u);
    Material::TestFactory::SetGraphicsPipelineId(glass, Rendering::GraphicsPipelineId{1u});
    Material::TestFactory::SetGpuSceneMaterialIndex(glass, 1u);
    Rendering::ShaderVariantKey glassKey{};
    glassKey.materialKeywords = Rendering::MaterialKeyword::Transmission;
    Material::TestFactory::SetVariantKey(glass, glassKey);

    Material opaque = Material::TestFactory::Create(GUID::Generate(), "Opaque", 32u);
    Material::TestFactory::SetGraphicsPipelineId(opaque, Rendering::GraphicsPipelineId{1u});
    Material::TestFactory::SetGpuSceneMaterialIndex(opaque, 2u);

    const uint32 viewId = 11u;

    auto submitOne = [&](WorldDrawBuilder& wdb, const Material& mat, uint32 flags)
    {
        WorldSubmissionRecord rec{};
        rec.viewId = viewId;
        rec.meshHandle = mesh;
        rec.material = &mat;
        rec.flags = flags;
        wdb.BeginFrame();
        wdb.Submit(std::span<const WorldSubmissionRecord>(&rec, 1u));
        wdb.BuildBatchKeys(rs.GetMeshGPURegistry(), /*materialColorClass=*/{});
    };

    // Transmissive but NOT a caster: the batch key still exists (the colour pass
    // needs it), the tint predicate is false.
    {
        WorldDrawBuilder wdb;
        submitOne(wdb, glass, 0u);
        EXPECT_EQ(wdb.GetBatchKeys(viewId).size(), 1u);
        EXPECT_FALSE(wdb.HasTransmissiveCasterSubmissions(viewId));
        EXPECT_FALSE(wdb.HasShadowCastingSubmissions(viewId));
    }

    // Transmissive AND a caster: the only combination the tint cascade can draw.
    {
        WorldDrawBuilder wdb;
        submitOne(wdb, glass, 1u);
        EXPECT_EQ(wdb.GetBatchKeys(viewId).size(), 1u);
        EXPECT_TRUE(wdb.HasTransmissiveCasterSubmissions(viewId));
    }

    // Opaque caster: a shadow caster, but nothing the tint pass would ever draw.
    {
        WorldDrawBuilder wdb;
        submitOne(wdb, opaque, 1u);
        EXPECT_TRUE(wdb.HasShadowCastingSubmissions(viewId));
        EXPECT_FALSE(wdb.HasTransmissiveCasterSubmissions(viewId));
    }

    // A view nobody submitted to answers false rather than inheriting another's.
    {
        WorldDrawBuilder wdb;
        submitOne(wdb, glass, 1u);
        EXPECT_TRUE(wdb.HasTransmissiveCasterSubmissions(viewId));
        EXPECT_FALSE(wdb.HasTransmissiveCasterSubmissions(viewId + 1u));
    }

    // BeginFrame drops last frame's answer: a view that stops submitting must not
    // keep a stale true and keep a 64 MiB tint array alive forever.
    {
        WorldDrawBuilder wdb;
        submitOne(wdb, glass, 1u);
        ASSERT_TRUE(wdb.HasTransmissiveCasterSubmissions(viewId));
        wdb.BeginFrame();
        EXPECT_FALSE(wdb.HasTransmissiveCasterSubmissions(viewId))
            << "the flag must reset with the batch keys it was derived alongside";
    }

    // ClearView drops it too (the editor one-shot/thumbnail path).
    {
        WorldDrawBuilder wdb;
        submitOne(wdb, glass, 1u);
        ASSERT_TRUE(wdb.HasTransmissiveCasterSubmissions(viewId));
        wdb.ClearView(viewId);
        EXPECT_FALSE(wdb.HasTransmissiveCasterSubmissions(viewId));
    }

    // The shadow-caster flag is per view and resets with the keys it was derived
    // alongside, like the transmissive one.
    {
        WorldDrawBuilder wdb;
        submitOne(wdb, opaque, 1u);
        ASSERT_TRUE(wdb.HasShadowCastingSubmissions(viewId));
        EXPECT_FALSE(wdb.HasShadowCastingSubmissions(viewId + 1u));
        wdb.BeginFrame();
        EXPECT_FALSE(wdb.HasShadowCastingSubmissions(viewId));
        submitOne(wdb, opaque, 1u);
        ASSERT_TRUE(wdb.HasShadowCastingSubmissions(viewId));
        wdb.ClearView(viewId);
        EXPECT_FALSE(wdb.HasShadowCastingSubmissions(viewId));
    }
    rs.Shutdown();
    device->Shutdown();
}

// The deforming subset is derived from the same submissions, in the same pass,
// as the batch keys — so the producer records exactly the keys the view has and
// a view with no deforming material costs one predicate per key and no work.
TEST(RenderServicesWorldDrawBuilderTests, DeformingSubsetNamesOnlyTheLaneMembers)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    Rendering::MeshGPUHandle mesh = RegisterTestMesh(rs);

    auto makeMaterial = [](uint32_t index, Rendering::MaterialKeyword keywords)
    {
        Material mat = Material::TestFactory::Create(GUID::Generate(), "Mat", 32u);
        Material::TestFactory::SetGraphicsPipelineId(mat, Rendering::GraphicsPipelineId{1u});
        Material::TestFactory::SetGpuSceneMaterialIndex(mat, index);
        Rendering::ShaderVariantKey key{};
        key.materialKeywords = keywords;
        Material::TestFactory::SetVariantKey(mat, key);
        return mat;
    };

    Material wind = makeMaterial(4u, Rendering::MaterialKeyword::HasVertexMod);
    Material rigid = makeMaterial(6u, Rendering::MaterialKeyword::None);
    Material terrain = makeMaterial(8u, Rendering::MaterialKeyword::HasVertexOutputMod |
                                            Rendering::MaterialKeyword::CustomVertexShader);

    const uint32 viewId = 11u;
    WorldSubmissionRecord recs[3]{};
    recs[0].viewId = viewId; recs[0].meshHandle = mesh; recs[0].material = &wind;
    recs[1].viewId = viewId; recs[1].meshHandle = mesh; recs[1].material = &rigid;
    recs[2].viewId = viewId; recs[2].meshHandle = mesh; recs[2].material = &terrain;

    WorldDrawBuilder wdb;
    wdb.BeginFrame();
    wdb.Submit(std::span<const WorldSubmissionRecord>(recs, 3u));
    wdb.BuildBatchKeys(rs.GetMeshGPURegistry(), /*materialColorClass=*/{});

    EXPECT_EQ(wdb.GetBatchKeys(viewId).size(), 3u);
    const auto deforming = wdb.GetDeformingBatchKeys(viewId);
    ASSERT_EQ(deforming.size(), 1u) << "only the simple-modifier material is in the lane";
    EXPECT_EQ(deforming[0].material, &wind);
    EXPECT_EQ(deforming[0].materialIndex, 4u);

    // It resets with the keys it was derived alongside, and with the view.
    wdb.BeginFrame();
    EXPECT_TRUE(wdb.GetDeformingBatchKeys(viewId).empty());
    wdb.Submit(std::span<const WorldSubmissionRecord>(recs, 3u));
    wdb.BuildBatchKeys(rs.GetMeshGPURegistry(), /*materialColorClass=*/{});
    ASSERT_EQ(wdb.GetDeformingBatchKeys(viewId).size(), 1u);
    wdb.ClearView(viewId);
    EXPECT_TRUE(wdb.GetDeformingBatchKeys(viewId).empty());

    rs.Shutdown();
    device->Shutdown();
}
