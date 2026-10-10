// GPU-executed tests for hzb_build.comp (R2.1 P1): seed a known depth image,
// run the mip-0 copy + per-mip reductions through the real pipeline, and
// verify every pyramid texel equals the CPU-computed conservative MIN of its
// source footprint. Reverse-Z: MIN = farthest — a texel may only ever move
// FARTHER than the surfaces it covers, never nearer (design §5-A5). The NPOT
// case pins the trailing-texel fold (floor-sized mips must still cover the
// whole parent).

#include <gtest/gtest.h>

#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include "StorageImageLayoutHelper.h"
#include "TestDeviceHelper.h"

#include <algorithm>
#include <cstring>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

struct HzbBuildPC
{
    uint32_t dstW;
    uint32_t dstH;
    uint32_t srcW;
    uint32_t srcH;
    uint32_t mode; // 0 = copy, 1 = 2x2 MIN reduce, 2 = 2x2 MAX reduce
};

// hzb_build.comp push-constant `mode` values for mips >= 1.
constexpr uint32_t kReduceMin = 1u;
constexpr uint32_t kReduceMax = 2u;

class HzbBuildComputeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        ShaderPackage pkg{};
        std::string err;
        if (!LoadShaderPkg("Shaders/hzb_build.shaderpkg", ShaderSourceKind::SpirV, pkg, &err))
            FAIL() << "hzb_build.shaderpkg not available: " << err;
        auto itCs = pkg.stageBytes.find("cs");
        ASSERT_NE(itCs, pkg.stageBytes.end());

        m_Layout.debugName = "HzbTest_SetLayout";
        for (uint32_t i = 0; i < 2u; ++i)
        {
            DescriptorBinding b{};
            b.binding = i;
            b.type = DescriptorType::StorageImage;
            b.count = 1u;
            b.shaderStages = kShaderStageCompute;
            m_Layout.bindings.push_back(b);
        }
        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(itCs->second);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        cd.PushConstants.Size = sizeof(HzbBuildPC);
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = "HzbBuildTest";
        const auto id = m_Device->InternComputePipeline(cd);
        m_Pipeline = m_Device->GetOrCreateComputePipeline(id);
        ASSERT_TRUE(m_Pipeline.IsValid());
    }

    void TearDown() override
    {
        for (TextureViewHandle& v : m_Views)
            if (v.IsValid())
                m_Device->DestroyTextureView(v);
        m_Views.clear();
        if (m_Tex.IsValid())
            m_Device->DestroyTexture(m_Tex);
        for (BufferHandle& b : m_Buffers)
            if (b.IsValid())
                m_Device->DestroyBuffer(b);
        m_Buffers.clear();
        if (m_Device)
            m_Device->Shutdown();
    }

    TextureViewHandle MipView(uint32_t mip)
    {
        TextureViewDesc vd{};
        vd.viewType = TextureViewType::View2D;
        vd.baseMip = mip;
        vd.levelCount = 1u;
        vd.baseLayer = 0u;
        vd.layerCount = 1u;
        vd.debugName = "HzbTest.MipView";
        TextureViewHandle v = m_Device->CreateTextureView(m_Tex, vd);
        m_Views.push_back(v);
        return v;
    }

    // Build the pyramid over a seeded mip 0 and read every mip back.
    // Returns one vector<float> per mip level.
    std::vector<std::vector<float>> BuildAndReadback(uint32_t w, uint32_t h,
                                                     const std::vector<float>& mip0,
                                                     uint32_t reduceMode = kReduceMin)
    {
        uint32_t levels = 1u;
        for (uint32_t s = std::max(w, h); s > 1u; s >>= 1u)
            ++levels;

        TextureDesc td{};
        td.width = w;
        td.height = h;
        td.depth = 1u;
        td.mipLevels = levels;
        td.arrayLayers = 1u;
        td.sampleCount = 1u;
        td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess |
                                         TextureUsage::ShaderResource |
                                         TextureUsage::TransferSrc | TextureUsage::TransferDst);
        td.debugName = "HzbTest.Pyramid";
        m_Tex = m_Device->CreateTexture(td);
        EXPECT_TRUE(m_Tex.IsValid());

        // Seed mip 0 with the known pattern.
        BufferDesc ud{};
        ud.size = mip0.size() * sizeof(float);
        ud.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
        ud.memoryUsage = BufferMemoryUsage::Upload;
        ud.debugName = "HzbTest.Upload";
        BufferHandle upload = m_Device->CreateBuffer(ud);
        m_Buffers.push_back(upload);
        void* mapped = m_Device->MapBuffer(upload);
        EXPECT_NE(mapped, nullptr);
        std::memcpy(mapped, mip0.data(), ud.size);
        m_Device->UnmapBuffer(upload);

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->CopyBufferToTextureSubresource(upload, m_Tex, /*mip=*/0u, /*layer=*/0u, w, h);

        // Every mip is about to be bound as a storage image, so every mip must
        // be in the storage-image layout first — including the ones nothing has
        // touched yet. Without this the readback below resolves oldLayout =
        // UNDEFINED and the driver may discard the mip (see the header).
        TransitionPyramidForStorageAccess(cl.get(), m_Tex, levels);

        // Reduce mip by mip; compute->compute barrier between levels so each
        // read sees the previous write (device-level CL — no RG edges here).
        for (uint32_t m = 1u; m < levels; ++m)
        {
            const uint32_t srcW = std::max(1u, w >> (m - 1u));
            const uint32_t srcH = std::max(1u, h >> (m - 1u));
            const uint32_t dstW = std::max(1u, w >> m);
            const uint32_t dstH = std::max(1u, h >> m);

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "HzbTest.DS";
            DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
            EXPECT_TRUE(ds.IsValid());
            m_Device->UpdateStorageImageBinding(ds, 0, MipView(m - 1u));
            m_Device->UpdateStorageImageBinding(ds, 1, MipView(m));

            cl->SetPipeline(m_Pipeline);
            cl->BindDescriptorSet(0, ds, m_Pipeline);
            HzbBuildPC pc{dstW, dstH, srcW, srcH, reduceMode};
            cl->SetPushConstants(pc);
            cl->Dispatch((dstW + 7u) / 8u, (dstH + 7u) / 8u, 1u);

            ResourceBarrier rb = ResourceBarrier::CreateMemoryBarrier(
                static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
                static_cast<uint64_t>(ResourceAccessMask::ShaderRead));
            cl->Barrier(rb);
        }

        // Read every mip back into its own readback buffer.
        std::vector<BufferHandle> readbacks(levels);
        for (uint32_t m = 0u; m < levels; ++m)
        {
            const uint32_t mw = std::max(1u, w >> m);
            const uint32_t mh = std::max(1u, h >> m);
            readbacks[m] =
                m_Device->CreateReadbackBuffer(static_cast<size_t>(mw) * mh * sizeof(float));
            m_Buffers.push_back(readbacks[m]);
            cl->CopyTextureSubresourceToBuffer(m_Tex, m, 0u, readbacks[m], mw, mh, 0, 0, 0, 0);
        }
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->FinalizeFrame();
        m_Device->WaitForIdle();

        std::vector<std::vector<float>> out(levels);
        for (uint32_t m = 0u; m < levels; ++m)
        {
            const uint32_t mw = std::max(1u, w >> m);
            const uint32_t mh = std::max(1u, h >> m);
            out[m].resize(static_cast<size_t>(mw) * mh);
            const void* p = m_Device->MapBuffer(readbacks[m]);
            EXPECT_NE(p, nullptr);
            if (p)
                std::memcpy(out[m].data(), p, out[m].size() * sizeof(float));
            m_Device->UnmapBuffer(readbacks[m]);
        }

        // Mip 0 is the seed round-tripped through the GPU. Checking it here
        // keeps the harness honest: a mip-1 mismatch then means the reduction
        // is wrong, not that the upload or the readback lost the image.
        EXPECT_EQ(out[0], mip0) << "mip 0 did not survive upload -> readback";
        return out;
    }

    // CPU reference: the shader's exact footprint rule (2x2 + trailing fold on
    // odd source extents, clamped) — dst(x,y) = MIN (or MAX) over the footprint.
    static std::vector<float> ReferenceReduce(const std::vector<float>& src, uint32_t srcW,
                                              uint32_t srcH, uint32_t dstW, uint32_t dstH,
                                              uint32_t reduceMode = kReduceMin)
    {
        std::vector<float> dst(static_cast<size_t>(dstW) * dstH);
        for (uint32_t y = 0; y < dstH; ++y)
            for (uint32_t x = 0; x < dstW; ++x)
            {
                const uint32_t x1 = (srcW % 2u != 0u && x == dstW - 1u) ? srcW - 1u
                                                                        : std::min(2u * x + 1u, srcW - 1u);
                const uint32_t y1 = (srcH % 2u != 0u && y == dstH - 1u) ? srcH - 1u
                                                                        : std::min(2u * y + 1u, srcH - 1u);
                float v = src[static_cast<size_t>(2u * y) * srcW + 2u * x];
                for (uint32_t sy = 2u * y; sy <= y1; ++sy)
                    for (uint32_t sx = 2u * x; sx <= x1; ++sx)
                    {
                        const float t = src[static_cast<size_t>(sy) * srcW + sx];
                        v = (reduceMode == kReduceMax) ? std::max(v, t) : std::min(v, t);
                    }
                dst[static_cast<size_t>(y) * dstW + x] = v;
            }
        return dst;
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    TextureHandle m_Tex{};
    std::vector<TextureViewHandle> m_Views;
    std::vector<BufferHandle> m_Buffers;
};

TEST_F(HzbBuildComputeTest, Pow2PyramidIsConservativeMin)
{
    const uint32_t w = 4u, h = 4u;
    // Distinct values; reverse-Z: smaller = farther. Include 0.0 (far plane
    // clear) and near-1.0 values.
    const std::vector<float> mip0 = {
        0.90f, 0.10f, 0.55f, 0.60f, //
        0.20f, 0.80f, 0.70f, 0.65f, //
        0.00f, 0.30f, 0.95f, 0.85f, //
        0.40f, 0.50f, 0.75f, 0.99f, //
    };
    const auto mips = BuildAndReadback(w, h, mip0);
    ASSERT_EQ(mips.size(), 3u);

    const auto ref1 = ReferenceReduce(mip0, 4u, 4u, 2u, 2u);
    for (size_t i = 0; i < ref1.size(); ++i)
        EXPECT_FLOAT_EQ(mips[1][i], ref1[i]) << "mip1 texel " << i;
    // {min(0.9,0.1,0.2,0.8)=0.1, min(0.55,0.6,0.7,0.65)=0.55,
    //  min(0,0.3,0.4,0.5)=0, min(0.95,0.85,0.75,0.99)=0.75}
    EXPECT_FLOAT_EQ(mips[1][0], 0.10f);
    EXPECT_FLOAT_EQ(mips[1][3], 0.75f);

    const auto ref2 = ReferenceReduce(ref1, 2u, 2u, 1u, 1u);
    EXPECT_FLOAT_EQ(mips[2][0], ref2[0]);
    EXPECT_FLOAT_EQ(mips[2][0], 0.0f) << "the apex is the FARTHEST depth in the image";
}

TEST_F(HzbBuildComputeTest, NpotTrailingTexelsFoldIntoLastRowColumn)
{
    // 5x3: mip1 is 2x1 (floor) — dst(0,0) covers cols 0-1 x rows 0-2 (odd-H
    // fold), dst(1,0) covers cols 2-4 x rows 0-2 (odd-W + odd-H + corner
    // folds). Every source texel must be covered by exactly the shader's rule.
    const uint32_t w = 5u, h = 3u;
    const std::vector<float> mip0 = {
        0.90f, 0.85f, 0.70f, 0.75f, 0.60f, //
        0.80f, 0.95f, 0.65f, 0.72f, 0.55f, //
        0.15f, 0.88f, 0.92f, 0.68f, 0.05f, //
    };
    const auto mips = BuildAndReadback(w, h, mip0);
    ASSERT_EQ(mips.size(), 3u); // 5x3 -> 2x1 -> 1x1

    const auto ref1 = ReferenceReduce(mip0, 5u, 3u, 2u, 1u);
    ASSERT_EQ(mips[1].size(), 2u);
    EXPECT_FLOAT_EQ(mips[1][0], ref1[0]);
    EXPECT_FLOAT_EQ(mips[1][1], ref1[1]);
    // The trailing texels MUST be folded: 0.15 lives in row 2 (odd-H fold of
    // dst 0); 0.05 lives at (4,2) (the corner fold of dst 1).
    EXPECT_FLOAT_EQ(mips[1][0], 0.15f);
    EXPECT_FLOAT_EQ(mips[1][1], 0.05f);

    ASSERT_EQ(mips[2].size(), 1u);
    EXPECT_FLOAT_EQ(mips[2][0], 0.05f) << "apex = farthest of the whole image";
}

// Mode 2 (MAX) is the SSR Hi-Z bound: reverse-Z ⇒ MAX = NEAREST surface, the
// opposite of the occlusion pyramid. The ray march needs a texel to be no
// FARTHER than the nearest surface it covers, so a ray registers a crossing as
// soon as it could hit anything the tile covers.
TEST_F(HzbBuildComputeTest, Pow2MaxPyramidIsNearestSurface)
{
    const uint32_t w = 4u, h = 4u;
    const std::vector<float> mip0 = {
        0.90f, 0.10f, 0.55f, 0.60f, //
        0.20f, 0.80f, 0.70f, 0.65f, //
        0.00f, 0.30f, 0.95f, 0.85f, //
        0.40f, 0.50f, 0.75f, 0.99f, //
    };
    const auto mips = BuildAndReadback(w, h, mip0, kReduceMax);
    ASSERT_EQ(mips.size(), 3u);

    const auto ref1 = ReferenceReduce(mip0, 4u, 4u, 2u, 2u, kReduceMax);
    for (size_t i = 0; i < ref1.size(); ++i)
        EXPECT_FLOAT_EQ(mips[1][i], ref1[i]) << "mip1 texel " << i;
    // max(0.9,0.1,0.2,0.8)=0.9 and max(0.95,0.85,0.75,0.99)=0.99 — the exact
    // texels the MIN pyramid reduces to 0.10 and 0.75, so these rows cannot
    // pass against a MIN build.
    EXPECT_FLOAT_EQ(mips[1][0], 0.90f);
    EXPECT_FLOAT_EQ(mips[1][3], 0.99f);

    const auto ref2 = ReferenceReduce(ref1, 2u, 2u, 1u, 1u, kReduceMax);
    EXPECT_FLOAT_EQ(mips[2][0], ref2[0]);
    EXPECT_FLOAT_EQ(mips[2][0], 0.99f) << "the apex is the NEAREST depth in the image";
}

TEST_F(HzbBuildComputeTest, NpotMaxFoldsTrailingTexelsToo)
{
    // Same 5x3 shape as the MIN NPOT case: the trailing fold must cover the
    // whole parent under MAX as well, or a near surface in the odd row/column
    // would be invisible to the ray march.
    const uint32_t w = 5u, h = 3u;
    const std::vector<float> mip0 = {
        0.90f, 0.85f, 0.70f, 0.75f, 0.60f, //
        0.80f, 0.95f, 0.65f, 0.72f, 0.55f, //
        0.15f, 0.88f, 0.92f, 0.68f, 0.98f, //
    };
    const auto mips = BuildAndReadback(w, h, mip0, kReduceMax);
    ASSERT_EQ(mips.size(), 3u); // 5x3 -> 2x1 -> 1x1

    const auto ref1 = ReferenceReduce(mip0, 5u, 3u, 2u, 1u, kReduceMax);
    ASSERT_EQ(mips[1].size(), 2u);
    EXPECT_FLOAT_EQ(mips[1][0], ref1[0]);
    EXPECT_FLOAT_EQ(mips[1][1], ref1[1]);
    // 0.95 is the max of dst 0's 2x3 footprint; 0.98 sits at (4,2), reachable
    // only through the odd-W + odd-H corner fold.
    EXPECT_FLOAT_EQ(mips[1][0], 0.95f);
    EXPECT_FLOAT_EQ(mips[1][1], 0.98f) << "corner fold must reach the trailing texel";

    ASSERT_EQ(mips[2].size(), 1u);
    EXPECT_FLOAT_EQ(mips[2][0], 0.98f) << "apex = nearest of the whole image";
}

} // namespace
