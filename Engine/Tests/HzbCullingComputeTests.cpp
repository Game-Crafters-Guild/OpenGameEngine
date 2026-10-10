// GPU-executed tests for hzb_culling.comp (R2.1 P2): dispatch the two-phase
// occlusion cull through the real pipeline over a synthetic scene + HZB and
// verify the visibility output, the prevVisible history rewrite (design §5-A2),
// and the per-slice stats against CPU expectations.
//
//   Mode 1 (phase A): visibility = frustum ∧ prevVisible (prevVisible read-only).
//   Mode 2 (phase B): draws only what A didn't (h ∧ ¬(f ∧ prevVisibleOld)) and
//                     rewrites prevVisible = h (occluded persistents DROP,
//                     revealed instances RECOVER, near-plane crossings force
//                     visible).
//
// The synthetic HZB is a uniform reverse-Z depth so the discriminator is each
// instance's distance (its nearest NDC depth vs the occluder depth) — a clean,
// deterministic exercise of the occlusion comparison; the pyramid MIN reduction
// itself is covered by HzbBuildComputeTests.

#include <gtest/gtest.h>

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include "Mathematics/MatrixOps.h"
#include "Assets/MeshLODGeometry.h"
#include "Assets/ModelAsset.h"

#include "TestDeviceHelper.h"

#include <cstring>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// Mirrors GPUCulling.cpp's HzbCullPC.
struct HzbCullPC
{
    uint32_t instanceCount;
    uint32_t sliceOffset;
};

// Mirrors hzb_culling.comp's OcclusionStatsBuffer / GPUCullingPipeline::OcclusionStats.
struct OcclusionStats
{
    uint32_t frustumPassed;
    uint32_t hzbCulled;
    uint32_t recovered;
    uint32_t prevVisibleNew;
};

constexpr uint32_t kHzbModePhaseA = 1u;
constexpr uint32_t kHzbModePhaseB = 2u;

// Reverse-Z occluder depth seeded uniformly into the synthetic HZB. Instances
// whose nearest NDC depth is >= this are "in front" (visible); < this are
// occluded. Chosen to cleanly separate the near (viewZ≈3) and far (viewZ≈50)
// instances below.
constexpr float kOccluderDepth = 0.02f;

struct CullResult
{
    std::vector<uint32_t> visibility;
    std::vector<uint32_t> prevVisible;
    OcclusionStats stats{};
};

class HzbCullingComputeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        ShaderPackage pkg{};
        std::string err;
        if (!LoadShaderPkg("Shaders/hzb_culling.shaderpkg", ShaderSourceKind::SpirV, pkg, &err))
            FAIL() << "hzb_culling.shaderpkg not available: " << err;
        auto itCs = pkg.stageBytes.find("cs");
        ASSERT_NE(itCs, pkg.stageBytes.end());

        m_Layout = MakeLayout();
        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(itCs->second);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        cd.PushConstants.Size = sizeof(HzbCullPC);
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = "HzbCullTest";
        const auto id = m_Device->InternComputePipeline(cd);
        m_Pipeline = m_Device->GetOrCreateComputePipeline(id);
        ASSERT_TRUE(m_Pipeline.IsValid());

        m_Sampler = m_Device->CreateSampler(SamplerDesc::PointClamp("HzbCullTestSampler"));
        ASSERT_TRUE(m_Sampler.IsValid());
    }

    void TearDown() override
    {
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
        for (TextureHandle& t : m_Textures)
            if (t.IsValid())
                m_Device->DestroyTexture(t);
        for (BufferHandle& b : m_Buffers)
            if (b.IsValid())
                m_Device->DestroyBuffer(b);
        if (m_Device)
            m_Device->Shutdown();
    }

    static DescriptorSetLayoutDesc MakeLayout()
    {
        DescriptorSetLayoutDesc layout{};
        layout.debugName = "HzbCullTest_SetLayout";
        auto addStorage = [&](uint32_t binding)
        {
            DescriptorBinding b{};
            b.binding = binding;
            b.type = DescriptorType::StorageBuffer;
            b.count = 1;
            b.shaderStages = kShaderStageCompute;
            layout.bindings.push_back(b);
        };
        addStorage(0);
        addStorage(1);
        addStorage(2);
        addStorage(3);
        DescriptorBinding hzb{};
        hzb.binding = 4;
        hzb.type = DescriptorType::CombinedImageSampler;
        hzb.count = 1;
        hzb.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(hzb);
        addStorage(5);
        return layout;
    }

    BufferHandle MakeHostBuffer(size_t bytes, const char* name)
    {
        BufferDesc d{};
        d.size = std::max<size_t>(bytes, 16);
        d.usage = static_cast<uint32_t>(BufferUsage::Storage) |
                  static_cast<uint32_t>(BufferUsage::TransferDst);
        // Readback, not Upload: the test seeds these once and then reads the
        // dispatch results back, and Readback is the class that asks for a
        // host-cached type rather than a write-combined one.
        d.memoryUsage = BufferMemoryUsage::Readback;
        d.debugName = name;
        BufferHandle h = m_Device->CreateBuffer(d);
        m_Buffers.push_back(h);
        return h;
    }

    template <typename T>
    void Write(BufferHandle h, const std::vector<T>& data)
    {
        void* mapped = m_Device->MapBuffer(h);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(mapped, data.data(), data.size() * sizeof(T));
        m_Device->UnmapBuffer(h);
    }

    void WriteOne(BufferHandle h, const GPUCullingData& cd)
    {
        void* mapped = m_Device->MapBuffer(h);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(mapped, &cd, sizeof(cd));
        m_Device->UnmapBuffer(h);
    }

    template <typename T>
    std::vector<T> Read(BufferHandle h, size_t count)
    {
        std::vector<T> out(count);
        void* mapped = m_Device->MapBuffer(h);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
            std::memcpy(out.data(), mapped, count * sizeof(T));
        m_Device->UnmapBuffer(h);
        return out;
    }

    // 8x8 R32F, single mip, uniform depth. Left in ShaderResource layout.
    TextureHandle MakeUniformHzb(float depth)
    {
        constexpr uint32_t kW = 8u, kH = 8u;
        TextureDesc td{};
        td.width = kW;
        td.height = kH;
        td.depth = 1u;
        td.mipLevels = 1u;
        td.arrayLayers = 1u;
        td.sampleCount = 1u;
        td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
                   static_cast<uint32_t>(TextureUsage::TransferDst);
        td.debugName = "HzbCullTest.HZB";
        TextureHandle tex = m_Device->CreateTexture(td);
        m_Textures.push_back(tex);

        const std::vector<float> texels(kW * kH, depth);
        BufferDesc ud{};
        ud.size = texels.size() * sizeof(float);
        ud.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
        ud.memoryUsage = BufferMemoryUsage::Upload;
        ud.debugName = "HzbCullTest.HzbUpload";
        BufferHandle upload = m_Device->CreateBuffer(ud);
        m_Buffers.push_back(upload);
        void* mapped = m_Device->MapBuffer(upload);
        std::memcpy(mapped, texels.data(), ud.size);
        m_Device->UnmapBuffer(upload);

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->CopyBufferToTextureSubresource(upload, tex, 0u, 0u, kW, kH);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::CopyDest,
                                                          ResourceState::ShaderResource));
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();
        return tex;
    }

    // A centered instance at (0,0,viewZ) with the given world radius.
    static GPUInstance MakeInstance(float viewZ, float radius)
    {
        GPUInstance inst{};
        inst.boundingCenter = Vector3(0.0f, 0.0f, viewZ);
        inst.boundingRadius = radius;
        return inst;
    }

    // Reverse-Z perspective culling data; identity view so world == view.
    static GPUCullingData MakeCullingData(uint32_t instanceCount, uint32_t mode,
                                          float margin0 = 1.0f)
    {
        constexpr float kNear = 0.1f, kFar = 100.0f;
        GPUCullingData cd{};
        cd.viewMatrix = Mathematics::Matrix4x4::Identity();
        cd.projMatrix = Mathematics::MakePerspectiveLH_ZO_ReverseZ(1.0472f /*60°*/, 1.0f, kNear, kFar);
        cd.viewProjMatrix = cd.projMatrix; // view is identity
        GPUCullingUtils::ExtractFrustumPlanes(cd.viewProjMatrix, &cd.frustumPlanes[0][0]);
        cd.cameraPosition = Vector3(0.0f, 0.0f, 0.0f);
        cd.cameraForward = Vector3(0.0f, 0.0f, 1.0f);
        cd.nearPlane = kNear;
        cd.farPlane = kFar;
        cd.firstInstance = 0u;
        cd.instanceCount = instanceCount;
        cd.cullShadowCasters = 0u;
        cd.cullMargins[0] = margin0;
        for (uint32_t m = 1; m < 4u; ++m)
            cd.cullMargins[m] = 1.0f;
        cd.hzbMode = mode;
        cd.hzbMipCount = 1u;
        return cd;
    }

    CullResult RunCull(uint32_t mode, const std::vector<GPUInstance>& instances,
                       const std::vector<uint32_t>& prevVisibleSeed, TextureHandle hzb,
                       float margin0 = 1.0f, uint32_t layerMask = 0xFFFFFFFFu)
    {
        const uint32_t n = static_cast<uint32_t>(instances.size());

        BufferHandle instanceBuf = MakeHostBuffer(n * sizeof(GPUInstance), "HzbCull.Instances");
        Write(instanceBuf, instances);

        GPUCullingData cd = MakeCullingData(n, mode, margin0);
        cd.renderLayerMask = layerMask;
        BufferHandle cullingBuf = MakeHostBuffer(sizeof(GPUCullingData), "HzbCull.CullingData");
        WriteOne(cullingBuf, cd);

        BufferHandle visBuf = MakeHostBuffer(n * sizeof(uint32_t), "HzbCull.Visibility");
        Write(visBuf, std::vector<uint32_t>(n, 0xDEADBEEFu));

        BufferHandle prevBuf = MakeHostBuffer(n * sizeof(uint32_t), "HzbCull.PrevVisible");
        Write(prevBuf, prevVisibleSeed);

        BufferHandle statsBuf = MakeHostBuffer(sizeof(OcclusionStats), "HzbCull.Stats");
        Write(statsBuf, std::vector<uint32_t>{0u, 0u, 0u, 0u});

        DescriptorSetDesc setDesc{};
        setDesc.layout = m_Layout;
        setDesc.transient = true;
        setDesc.debugName = "HzbCullTest.DS";
        DescriptorSetHandle ds = m_Device->CreateDescriptorSet(setDesc);
        m_Device->UpdateStorageBufferBinding(ds, 0, instanceBuf, 0, n * sizeof(GPUInstance));
        m_Device->UpdateStorageBufferBinding(ds, 1, cullingBuf, 0, sizeof(GPUCullingData));
        m_Device->UpdateStorageBufferBinding(ds, 2, visBuf, 0, n * sizeof(uint32_t));
        m_Device->UpdateStorageBufferBinding(ds, 3, prevBuf, 0, n * sizeof(uint32_t));
        m_Device->UpdateCombinedImageSamplerBinding(ds, 4, hzb, m_Sampler);
        m_Device->UpdateStorageBufferBinding(ds, 5, statsBuf, 0, sizeof(OcclusionStats));

        HzbCullPC pc{};
        pc.instanceCount = n;
        pc.sliceOffset = 0u;

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->SetPipeline(m_Pipeline);
        cl->BindDescriptorSet(0, ds, m_Pipeline);
        cl->SetPushConstants(pc);
        cl->Dispatch((n + 63u) / 64u, 1, 1);
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();

        CullResult r;
        r.visibility = Read<uint32_t>(visBuf, n);
        r.prevVisible = Read<uint32_t>(prevBuf, n);
        const auto s = Read<uint32_t>(statsBuf, 4);
        r.stats = {s[0], s[1], s[2], s[3]};
        return r;
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    SamplerHandle m_Sampler{};
    std::vector<TextureHandle> m_Textures;
    std::vector<BufferHandle> m_Buffers;
};

// Phase B (mode 2): the full §5-A2 rule end to end.
//   inst0 far  + prevVisible=1  -> occluded persistent DROPS       (vis 0, prev 0)
//   inst1 near + prevVisible=0  -> revealed RECOVERS this frame     (vis 1, prev 1)
//   inst2 near + prevVisible=1  -> already drawn in A, no re-draw   (vis 0, prev 1)
//   inst3 near-plane crossing   -> forced visible, recovers         (vis 1, prev 1)
TEST_F(HzbCullingComputeTest, PhaseBAppliesCorrectedPrevVisibleRule)
{
    TextureHandle hzb = MakeUniformHzb(kOccluderDepth);

    const std::vector<GPUInstance> instances = {
        MakeInstance(/*viewZ=*/50.0f, /*radius=*/1.0f), // occluded (far behind occluder)
        MakeInstance(/*viewZ=*/3.0f, /*radius=*/1.0f),  // in front of occluder
        MakeInstance(/*viewZ=*/3.0f, /*radius=*/1.0f),  // in front of occluder
        MakeInstance(/*viewZ=*/0.5f, /*radius=*/1.0f),  // crosses the near plane
    };
    const std::vector<uint32_t> prevSeed = {1u, 0u, 1u, 0u};

    const CullResult r = RunCull(kHzbModePhaseB, instances, prevSeed, hzb);

    EXPECT_EQ(r.visibility[0], 0u) << "occluded persistent must not draw in phase B";
    EXPECT_EQ(r.visibility[1], 1u) << "revealed instance must recover this frame";
    EXPECT_EQ(r.visibility[2], 0u) << "instance already drawn in phase A must not re-draw";
    EXPECT_EQ(r.visibility[3], 1u) << "near-plane crossing must be forced visible + recovered";

    EXPECT_EQ(r.prevVisible[0], 0u) << "§5-A2: occluded persistent DROPS out of prevVisible";
    EXPECT_EQ(r.prevVisible[1], 1u);
    EXPECT_EQ(r.prevVisible[2], 1u);
    EXPECT_EQ(r.prevVisible[3], 1u);

    EXPECT_EQ(r.stats.frustumPassed, 4u);
    EXPECT_EQ(r.stats.hzbCulled, 1u);      // inst0
    EXPECT_EQ(r.stats.recovered, 2u);      // inst1, inst3
    EXPECT_EQ(r.stats.prevVisibleNew, 3u); // inst1, inst2, inst3 (h == true)
}

// Phase B: the occlusion test must consume the MARGINED radius, not the raw
// bounding radius. cullMargins[0] carries the skinned-pose slack for bind-pose
// bounds (GPUScene.h kCullMarginConservative); if HzbTest ignores it, a pose
// that exceeds the bind-pose sphere can be occlusion-culled while visibly
// protruding past an occluder — and the §5-A2 prevVisible drop makes that a
// PERSISTENT hole, not a one-frame pop.
//
// Geometry (near=0.1, far=100, occluder depth 0.02 ⇔ viewZ≈4.77):
//   inst0 viewZ=6, r=1: raw nearest z=5 → ndc≈0.0190 < 0.02 (occluded raw);
//                       margin 2 nearest z=4 → ndc≈0.0240 ≥ 0.02 (visible).
//   inst1 viewZ=50, r=1: occluded even with margin (nearest z=48 → ndc≈0.0011).
TEST_F(HzbCullingComputeTest, PhaseBOcclusionTestUsesMarginedRadius)
{
    TextureHandle hzb = MakeUniformHzb(kOccluderDepth);

    const std::vector<GPUInstance> instances = {
        MakeInstance(/*viewZ=*/6.0f, /*radius=*/1.0f),
        MakeInstance(/*viewZ=*/50.0f, /*radius=*/1.0f),
    };
    const std::vector<uint32_t> prevSeed = {0u, 0u};

    const CullResult r = RunCull(kHzbModePhaseB, instances, prevSeed, hzb, /*margin0=*/2.0f);

    EXPECT_EQ(r.visibility[0], 1u) << "margined sphere clears the occluder — must recover";
    EXPECT_EQ(r.visibility[1], 0u) << "margin must not blanket-pass a deeply occluded instance";
    EXPECT_EQ(r.prevVisible[0], 1u);
    EXPECT_EQ(r.prevVisible[1], 0u);

    EXPECT_EQ(r.stats.frustumPassed, 2u);
    EXPECT_EQ(r.stats.hzbCulled, 1u);
    EXPECT_EQ(r.stats.recovered, 1u);
    EXPECT_EQ(r.stats.prevVisibleNew, 1u);
}

// Phase A (mode 1): visibility = frustum ∧ prevVisible, prevVisible untouched.
TEST_F(HzbCullingComputeTest, LowerGeometryRemainsVisibleAcrossFormerFrustumBoundary)
{
    Mesh mesh;
    mesh.Vertices.resize(3);
    mesh.Vertices[0].Position[0] = -0.1f;
    mesh.Vertices[0].Position[1] = -0.1f;
    mesh.Vertices[1].Position[0] = 0.1f;
    mesh.Vertices[1].Position[1] = -0.1f;
    mesh.Vertices[2].Position[1] = 0.1f;
    mesh.Indices = {0, 1, 2};
    for (int axis = 0; axis < 3; ++axis) {
        mesh.MinBounds[axis] = -0.1f;
        mesh.MaxBounds[axis] = 0.1f;
    }
    mesh.ExtraLODs = {{0, 1, 2}};
    mesh.ExtraLODVertices = {mesh.Vertices};
    mesh.ExtraLODVertices[0][0].Position[0] = -3.0f;
    const auto geometry = ResolveMeshLODGeometry(mesh);
    ASSERT_EQ(geometry.LevelCount, 2u);
    GPUInstance old = MakeInstance(5.0f, geometry.ReferenceBounds.Radius());
    old.boundingCenter.x = 5.0f;
    GPUInstance corrected = old;
    corrected.boundingRadius = geometry.Bounds.Radius();
    // At z=5 the right frustum edge is x≈2.89. LOD0's small box is
    // outside, but the lower level reaches x=2 and must remain a candidate.
    const auto result = RunCull(kHzbModePhaseA, {old, corrected}, {1u, 1u}, MakeUniformHzb(0.0f));
    ASSERT_EQ(result.visibility.size(), 2u);
    EXPECT_EQ(result.visibility[0], 0u);
    EXPECT_EQ(result.visibility[1], 1u);
}

TEST_F(HzbCullingComputeTest, PhaseAGatesOnPrevVisible)
{
    TextureHandle hzb = MakeUniformHzb(kOccluderDepth);

    const std::vector<GPUInstance> instances = {
        MakeInstance(/*viewZ=*/3.0f, /*radius=*/1.0f), // in frustum, prevVisible=0 -> not drawn
        MakeInstance(/*viewZ=*/3.0f, /*radius=*/1.0f), // in frustum, prevVisible=1 -> drawn
    };
    const std::vector<uint32_t> prevSeed = {0u, 1u};

    const CullResult r = RunCull(kHzbModePhaseA, instances, prevSeed, hzb);

    EXPECT_EQ(r.visibility[0], 0u) << "prevVisible=0 inside frustum must not draw in phase A";
    EXPECT_EQ(r.visibility[1], 1u) << "prevVisible=1 inside frustum must draw in phase A";

    // Mode 1 is read-only over prevVisible.
    EXPECT_EQ(r.prevVisible[0], 0u);
    EXPECT_EQ(r.prevVisible[1], 1u);

    // Mode 1 writes no stats.
    EXPECT_EQ(r.stats.frustumPassed, 0u);
    EXPECT_EQ(r.stats.hzbCulled, 0u);
    EXPECT_EQ(r.stats.recovered, 0u);
    EXPECT_EQ(r.stats.prevVisibleNew, 0u);
}

} // namespace

TEST_F(HzbCullingComputeTest, RenderLayersGateBothPhasesAndDropHiddenHistory)
{
    const auto hzb = MakeUniformHzb(0.0f);
    std::vector<GPUInstance> instances(4, MakeInstance(3.0f, .1f));
    instances[0].renderLayerMask = 0;
    instances[1].renderLayerMask = 1;
    instances[2].renderLayerMask = 0x80000000u;
    instances[3].renderLayerMask = 0xFFFFFFFFu;
    for (uint32_t mask : {0u, 1u, 0x80000000u, 0xFFFFFFFFu})
    {
        std::vector<uint32_t> expected;
        for (const auto& i : instances)
            expected.push_back((i.renderLayerMask & mask) != 0 ? 1u : 0u);
        auto a = RunCull(kHzbModePhaseA, instances, {1, 1, 1, 1}, hzb, 1.0f, mask);
        EXPECT_EQ(a.visibility, expected);
        auto b = RunCull(kHzbModePhaseB, instances, {1, 1, 1, 1}, hzb, 1.0f, mask);
        EXPECT_EQ(b.visibility, (std::vector<uint32_t>{0, 0, 0, 0}));
        EXPECT_EQ(b.prevVisible, expected);
        auto reveal = RunCull(kHzbModePhaseB, instances, {0, 0, 0, 0}, hzb, 1.0f, mask);
        EXPECT_EQ(reveal.visibility, expected);
        EXPECT_EQ(reveal.prevVisible, expected);
    }
}
