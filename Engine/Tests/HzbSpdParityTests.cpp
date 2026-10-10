// GPU-executed parity tests for the two-dispatch SPD build (hzb_spd.comp +
// hzb_spd_finalize.comp) vs the hzb_build.comp chain.
//
// The SPD build's content contract is BIT-IDENTICAL pyramids: run both paths
// over the same seeded depth image and compare every texel of every mip as
// raw float bits. Sizes deliberately cover the adversarial NPOT shapes:
//   * odd extents at mip 0 (trailing fold at mip 1, exact from the depth image)
//   * odd extents at LDS-tier levels whose fold source crosses a 64x64 tile
//     boundary (130x66: mip1 = 65x33, mip2's last column folds mip1 col 64,
//     owned by the next workgroup — the finalize pass's edge-fixup path)
//   * all-odd chains (1023x1023: a fold at every level)
//   * production-shaped extents (640x360, 1920x1080) spanning many tiles and
//     exercising the finalize pass's mip-7.. LDS tail.

#include <gtest/gtest.h>

#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include "StorageImageLayoutHelper.h"
#include "TestDeviceHelper.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// Guards the parity comparison against comparing two blank pyramids.
bool IsAllZero(const std::vector<std::vector<float>>& pyramid)
{
    for (const std::vector<float>& mip : pyramid)
        for (float v : mip)
            if (v != 0.0f)
                return false;
    return true;
}

// Mirrors HZBBuildNode.cpp's HzbBuildPC / hzb_build.comp.
struct HzbBuildPC
{
    uint32_t dstW;
    uint32_t dstH;
    uint32_t srcW;
    uint32_t srcH;
    uint32_t mode; // 0 = copy depth into mip 0, 1 = 2x2 MIN reduce
};

// Mirrors HZBBuildNode.cpp's HzbSpdPC (shared by hzb_spd.comp and
// hzb_spd_finalize.comp).
struct HzbSpdPC
{
    uint32_t width;
    uint32_t height;
    uint32_t mipCount;
};

constexpr uint32_t kSpdTileSize = 64u;
constexpr uint32_t kSpdTileMipArrayCount = 6u;
constexpr uint32_t kSpdFinalizeMipArrayCount = 12u;

uint32_t MipChainLength(uint32_t w, uint32_t h)
{
    uint32_t levels = 1u;
    for (uint32_t s = std::max(w, h); s > 1u; s >>= 1u)
        ++levels;
    return levels;
}

// Deterministic pseudo-random depth in [0, 1) — distinct values so any wrong
// footprint shows as a bit mismatch.
std::vector<float> MakeDepthPattern(uint32_t w, uint32_t h)
{
    std::vector<float> depth(static_cast<size_t>(w) * h);
    uint32_t state = 0x9E3779B9u;
    for (float& v : depth)
    {
        state = state * 1664525u + 1013904223u; // LCG
        v = static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
    }
    return depth;
}

class HzbSpdParityTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        // Per-mip chain pipeline (the parity reference).
        {
            ShaderPackage pkg{};
            std::string err;
            if (!LoadShaderPkg("Shaders/hzb_build.shaderpkg", ShaderSourceKind::SpirV, pkg, &err))
                FAIL() << "hzb_build.shaderpkg not available: " << err;
            auto itCs = pkg.stageBytes.find("cs");
            ASSERT_NE(itCs, pkg.stageBytes.end());

            m_ChainLayout.debugName = "HzbSpdParity_ChainLayout";
            for (uint32_t i = 0; i < 2u; ++i)
            {
                DescriptorBinding b{};
                b.binding = i;
                b.type = DescriptorType::StorageImage;
                b.count = 1u;
                b.shaderStages = kShaderStageCompute;
                m_ChainLayout.bindings.push_back(b);
            }
            ComputePipelineDesc cd{};
            cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(itCs->second);
            cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_ChainLayout));
            cd.PushConstants.Size = sizeof(HzbBuildPC);
            cd.PushConstants.StageMask = kShaderStageCompute;
            cd.DebugName = "HzbSpdParity.Chain";
            m_ChainPipeline =
                m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(cd));
            ASSERT_TRUE(m_ChainPipeline.IsValid());
        }

        // SPD tile pipeline.
        {
            ShaderPackage pkg{};
            std::string err;
            if (!LoadShaderPkg("Shaders/hzb_spd.shaderpkg", ShaderSourceKind::SpirV, pkg, &err))
                FAIL() << "hzb_spd.shaderpkg not available: " << err;
            auto itCs = pkg.stageBytes.find("cs");
            ASSERT_NE(itCs, pkg.stageBytes.end());

            m_SpdLayout.debugName = "HzbSpdParity_SpdLayout";
            auto add = [&](uint32_t binding, uint32_t count) {
                DescriptorBinding b{};
                b.binding = binding;
                b.type = DescriptorType::StorageImage;
                b.count = count;
                b.shaderStages = kShaderStageCompute;
                m_SpdLayout.bindings.push_back(b);
            };
            add(0u, 1u);                      // uDepth
            add(1u, 1u);                      // uMip0
            add(2u, kSpdTileMipArrayCount);   // uMips[6]
            ComputePipelineDesc cd{};
            cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(itCs->second);
            cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_SpdLayout));
            cd.PushConstants.Size = sizeof(HzbSpdPC);
            cd.PushConstants.StageMask = kShaderStageCompute;
            cd.DebugName = "HzbSpdParity.Spd";
            m_SpdPipeline =
                m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(cd));
            ASSERT_TRUE(m_SpdPipeline.IsValid());
        }

        // SPD finalize pipeline.
        {
            ShaderPackage pkg{};
            std::string err;
            if (!LoadShaderPkg("Shaders/hzb_spd_finalize.shaderpkg", ShaderSourceKind::SpirV, pkg, &err))
                FAIL() << "hzb_spd_finalize.shaderpkg not available: " << err;
            auto itCs = pkg.stageBytes.find("cs");
            ASSERT_NE(itCs, pkg.stageBytes.end());

            m_FinalizeLayout.debugName = "HzbSpdParity_FinalizeLayout";
            DescriptorBinding b{};
            b.binding = 0u; // uMips[12]
            b.type = DescriptorType::StorageImage;
            b.count = kSpdFinalizeMipArrayCount;
            b.shaderStages = kShaderStageCompute;
            m_FinalizeLayout.bindings.push_back(b);
            ComputePipelineDesc cd{};
            cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(itCs->second);
            cd.DescriptorSetLayouts.push_back(
                m_Device->InternDescriptorSetLayout(m_FinalizeLayout));
            cd.PushConstants.Size = sizeof(HzbSpdPC);
            cd.PushConstants.StageMask = kShaderStageCompute;
            cd.DebugName = "HzbSpdParity.SpdFinalize";
            m_FinalizePipeline =
                m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(cd));
            ASSERT_TRUE(m_FinalizePipeline.IsValid());
        }
    }

    void TearDown() override
    {
        for (TextureViewHandle& v : m_Views)
            if (v.IsValid())
                m_Device->DestroyTextureView(v);
        m_Views.clear();
        for (TextureHandle& t : m_Textures)
            if (t.IsValid())
                m_Device->DestroyTexture(t);
        m_Textures.clear();
        for (BufferHandle& b : m_Buffers)
            if (b.IsValid())
                m_Device->DestroyBuffer(b);
        m_Buffers.clear();
        if (m_Device)
            m_Device->Shutdown();
    }

    TextureHandle MakePyramid(uint32_t w, uint32_t h, uint32_t levels, const char* name)
    {
        TextureDesc td{};
        td.width = w;
        td.height = h;
        td.depth = 1u;
        td.mipLevels = levels;
        td.arrayLayers = 1u;
        td.sampleCount = 1u;
        td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess |
                                         TextureUsage::ShaderResource | TextureUsage::TransferSrc);
        td.debugName = name;
        TextureHandle tex = m_Device->CreateTexture(td);
        m_Textures.push_back(tex);
        return tex;
    }

    TextureViewHandle MipView(TextureHandle tex, uint32_t mip)
    {
        TextureViewDesc vd{};
        vd.viewType = TextureViewType::View2D;
        vd.baseMip = mip;
        vd.levelCount = 1u;
        vd.baseLayer = 0u;
        vd.layerCount = 1u;
        vd.debugName = "HzbSpdParity.MipView";
        TextureViewHandle v = m_Device->CreateTextureView(tex, vd);
        m_Views.push_back(v);
        return v;
    }

    // Runs both build paths over the same depth pattern and reads back every
    // mip of both pyramids (chain first, then the two SPD dispatches).
    void BuildBoth(uint32_t w, uint32_t h, const std::vector<float>& depthTexels,
                   std::vector<std::vector<float>>& outChain,
                   std::vector<std::vector<float>>& outSpd)
    {
        const uint32_t levels = MipChainLength(w, h);
        ASSERT_LE(w >> 7u, 32u) << "size exceeds the SPD path's mip-7 tile cap";
        ASSERT_LE(h >> 7u, 32u) << "size exceeds the SPD path's mip-7 tile cap";

        // Depth source (storage-read in both paths, like View.DepthResolved).
        TextureDesc dd{};
        dd.width = w;
        dd.height = h;
        dd.depth = 1u;
        dd.mipLevels = 1u;
        dd.arrayLayers = 1u;
        dd.sampleCount = 1u;
        dd.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        dd.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess |
                                         TextureUsage::ShaderResource | TextureUsage::TransferDst);
        dd.debugName = "HzbSpdParity.Depth";
        TextureHandle depthTex = m_Device->CreateTexture(dd);
        m_Textures.push_back(depthTex);

        BufferDesc ud{};
        ud.size = depthTexels.size() * sizeof(float);
        ud.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
        ud.memoryUsage = BufferMemoryUsage::Upload;
        ud.debugName = "HzbSpdParity.DepthUpload";
        BufferHandle upload = m_Device->CreateBuffer(ud);
        m_Buffers.push_back(upload);
        void* mapped = m_Device->MapBuffer(upload);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(mapped, depthTexels.data(), ud.size);
        m_Device->UnmapBuffer(upload);

        TextureHandle chainTex = MakePyramid(w, h, levels, "HzbSpdParity.Chain");
        TextureHandle spdTex = MakePyramid(w, h, levels, "HzbSpdParity.Spd");

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->CopyBufferToTextureSubresource(upload, depthTex, 0u, 0u, w, h);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(depthTex, ResourceState::CopyDest,
                                                          ResourceState::UnorderedAccess));
        // The pyramids are write-first, but "nothing wrote it yet" is not a
        // layout: leaving their mips untransitioned makes the readback below
        // resolve oldLayout = UNDEFINED, which may discard them (see the header).
        TransitionForStorageAccess(cl.get(), chainTex, ResourceState::Undefined, 0u, levels);
        TransitionForStorageAccess(cl.get(), spdTex, ResourceState::Undefined, 0u, levels);

        const ResourceBarrier computeToCompute = ResourceBarrier::CreateMemoryBarrier(
            static_cast<uint64_t>(PipelineStageMask::ComputeShader),
            static_cast<uint64_t>(PipelineStageMask::ComputeShader),
            static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
            static_cast<uint64_t>(ResourceAccessMask::ShaderRead));

        // ── Path A: the per-mip chain (mode 0 copy + mode 1 reduces) ────────
        {
            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_ChainLayout;
            dsDesc.transient = true;
            dsDesc.debugName = "HzbSpdParity.Chain.DS";
            DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
            m_Device->UpdateStorageImageBinding(ds, 0, depthTex);
            m_Device->UpdateStorageImageBinding(ds, 1, MipView(chainTex, 0u));
            cl->SetPipeline(m_ChainPipeline);
            cl->BindDescriptorSet(0, ds, m_ChainPipeline);
            HzbBuildPC pc{w, h, w, h, 0u};
            cl->SetPushConstants(pc);
            cl->Dispatch((w + 7u) / 8u, (h + 7u) / 8u, 1u);
            cl->Barrier(computeToCompute);
        }
        for (uint32_t m = 1u; m < levels; ++m)
        {
            const uint32_t srcW = std::max(1u, w >> (m - 1u));
            const uint32_t srcH = std::max(1u, h >> (m - 1u));
            const uint32_t dstW = std::max(1u, w >> m);
            const uint32_t dstH = std::max(1u, h >> m);

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_ChainLayout;
            dsDesc.transient = true;
            dsDesc.debugName = "HzbSpdParity.Chain.DS";
            DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
            m_Device->UpdateStorageImageBinding(ds, 0, MipView(chainTex, m - 1u));
            m_Device->UpdateStorageImageBinding(ds, 1, MipView(chainTex, m));
            cl->SetPipeline(m_ChainPipeline);
            cl->BindDescriptorSet(0, ds, m_ChainPipeline);
            HzbBuildPC pc{dstW, dstH, srcW, srcH, 1u};
            cl->SetPushConstants(pc);
            cl->Dispatch((dstW + 7u) / 8u, (dstH + 7u) / 8u, 1u);
            cl->Barrier(computeToCompute);
        }

        // ── Path B: the two SPD dispatches (tile pass, barrier, finalize) ───
        {
            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_SpdLayout;
            dsDesc.transient = true;
            dsDesc.debugName = "HzbSpdParity.Spd.DS";
            DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
            m_Device->UpdateStorageImageBinding(ds, 0, depthTex);
            m_Device->UpdateStorageImageBinding(ds, 1, MipView(spdTex, 0u));
            for (uint32_t i = 0; i < kSpdTileMipArrayCount; ++i)
            {
                const uint32_t mip = std::min(i + 1u, levels - 1u);
                m_Device->UpdateStorageImageBinding(ds, 2, MipView(spdTex, mip), i);
            }

            const uint32_t wgX = (w + kSpdTileSize - 1u) / kSpdTileSize;
            const uint32_t wgY = (h + kSpdTileSize - 1u) / kSpdTileSize;
            cl->SetPipeline(m_SpdPipeline);
            cl->BindDescriptorSet(0, ds, m_SpdPipeline);
            HzbSpdPC pc{w, h, levels};
            cl->SetPushConstants(pc);
            cl->Dispatch(wgX, wgY, 1u);
            cl->Barrier(computeToCompute);
        }
        if (levels >= 3u)
        {
            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_FinalizeLayout;
            dsDesc.transient = true;
            dsDesc.debugName = "HzbSpdParity.Finalize.DS";
            DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
            for (uint32_t i = 0; i < kSpdFinalizeMipArrayCount; ++i)
            {
                const uint32_t mip = std::min(i + 1u, levels - 1u);
                m_Device->UpdateStorageImageBinding(ds, 0, MipView(spdTex, mip), i);
            }
            cl->SetPipeline(m_FinalizePipeline);
            cl->BindDescriptorSet(0, ds, m_FinalizePipeline);
            HzbSpdPC pc{w, h, levels};
            cl->SetPushConstants(pc);
            cl->Dispatch(1u, 1u, 1u);
            cl->Barrier(computeToCompute);
        }

        // ── Readback: every mip of both pyramids ────────────────────────────
        std::vector<BufferHandle> chainRb(levels);
        std::vector<BufferHandle> spdRb(levels);
        for (uint32_t m = 0u; m < levels; ++m)
        {
            const uint32_t mw = std::max(1u, w >> m);
            const uint32_t mh = std::max(1u, h >> m);
            const size_t bytes = static_cast<size_t>(mw) * mh * sizeof(float);
            chainRb[m] = m_Device->CreateReadbackBuffer(bytes);
            spdRb[m] = m_Device->CreateReadbackBuffer(bytes);
            m_Buffers.push_back(chainRb[m]);
            m_Buffers.push_back(spdRb[m]);
            cl->CopyTextureSubresourceToBuffer(chainTex, m, 0u, chainRb[m], mw, mh, 0, 0, 0, 0);
            cl->CopyTextureSubresourceToBuffer(spdTex, m, 0u, spdRb[m], mw, mh, 0, 0, 0, 0);
        }
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->FinalizeFrame();
        m_Device->WaitForIdle();

        outChain.resize(levels);
        outSpd.resize(levels);
        for (uint32_t m = 0u; m < levels; ++m)
        {
            const uint32_t mw = std::max(1u, w >> m);
            const uint32_t mh = std::max(1u, h >> m);
            const size_t count = static_cast<size_t>(mw) * mh;
            outChain[m].resize(count);
            outSpd[m].resize(count);
            const void* p = m_Device->MapBuffer(chainRb[m]);
            ASSERT_NE(p, nullptr);
            std::memcpy(outChain[m].data(), p, count * sizeof(float));
            m_Device->UnmapBuffer(chainRb[m]);
            p = m_Device->MapBuffer(spdRb[m]);
            ASSERT_NE(p, nullptr);
            std::memcpy(outSpd[m].data(), p, count * sizeof(float));
            m_Device->UnmapBuffer(spdRb[m]);
        }
    }

    // Bit-exact comparison over the full pyramid; failure names the first
    // mismatching (mip, x, y) with both bit patterns.
    void ExpectBitExact(uint32_t w, const std::vector<std::vector<float>>& chain,
                        const std::vector<std::vector<float>>& spd)
    {
        ASSERT_EQ(chain.size(), spd.size());
        // A == B is vacuous if both are blank: two pyramids the driver discarded
        // compare bit-identical. Pin the comparison to real data first.
        ASSERT_FALSE(IsAllZero(chain)) << "chain pyramid is entirely zero — nothing was compared";
        ASSERT_FALSE(IsAllZero(spd)) << "SPD pyramid is entirely zero — nothing was compared";
        for (uint32_t m = 0u; m < chain.size(); ++m)
        {
            const uint32_t mw = std::max(1u, w >> m);
            ASSERT_EQ(chain[m].size(), spd[m].size()) << "mip " << m;
            for (size_t i = 0; i < chain[m].size(); ++i)
            {
                const uint32_t a = std::bit_cast<uint32_t>(chain[m][i]);
                const uint32_t b = std::bit_cast<uint32_t>(spd[m][i]);
                if (a != b)
                {
                    ADD_FAILURE() << "mip " << m << " texel (" << (i % mw) << ", " << (i / mw)
                                  << "): chain bits 0x" << std::hex << a << " vs spd bits 0x" << b;
                    return; // first divergence is the diagnostic; avoid failure spam
                }
            }
        }
    }

    void RunParityCase(uint32_t w, uint32_t h)
    {
        SCOPED_TRACE(::testing::Message() << "extent " << w << "x" << h);
        std::vector<std::vector<float>> chain;
        std::vector<std::vector<float>> spd;
        BuildBoth(w, h, MakeDepthPattern(w, h), chain, spd);
        if (::testing::Test::HasFatalFailure())
            return;
        ExpectBitExact(w, chain, spd);
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_ChainLayout{};
    DescriptorSetLayoutDesc m_SpdLayout{};
    DescriptorSetLayoutDesc m_FinalizeLayout{};
    PipelineHandle m_ChainPipeline{};
    PipelineHandle m_SpdPipeline{};
    PipelineHandle m_FinalizePipeline{};
    std::vector<TextureHandle> m_Textures;
    std::vector<TextureViewHandle> m_Views;
    std::vector<BufferHandle> m_Buffers;
};

TEST_F(HzbSpdParityTest, Pow2SingleTile) { RunParityCase(4u, 4u); }

TEST_F(HzbSpdParityTest, NpotTiny) { RunParityCase(5u, 3u); }

// levels == 2: the finalize pass is skipped entirely (mip 1 is already exact
// from the tile pass — nothing to repair, no mips 7+).
TEST_F(HzbSpdParityTest, TwoLevelChainSkipsFinalize) { RunParityCase(3u, 2u); }

TEST_F(HzbSpdParityTest, ExactTile) { RunParityCase(64u, 64u); }

TEST_F(HzbSpdParityTest, NpotOddBothAxes) { RunParityCase(67u, 35u); }

// mip1 = 65x33: mip2's last column must fold mip1 col 64, which belongs to the
// NEXT 64x64 tile — the exact cross-workgroup case the finalize pass's edge
// fixup exists for.
TEST_F(HzbSpdParityTest, CrossTileFoldFixup) { RunParityCase(130u, 66u); }

TEST_F(HzbSpdParityTest, NpotDeepOddChain) { RunParityCase(259u, 131u); }

// Odd at every level: 1023 -> 511 -> 255 -> ... folds on both axes throughout.
TEST_F(HzbSpdParityTest, AllOddChain) { RunParityCase(1023u, 1023u); }

TEST_F(HzbSpdParityTest, ProductionShapeSmall) { RunParityCase(640u, 360u); }

TEST_F(HzbSpdParityTest, ProductionShape1080p) { RunParityCase(1920u, 1080u); }

// 12-level chain (4K): exercises the finalize tail through GE_SPD_TAIL_LEVEL
// 11 and the deeper StoreMip cases.
TEST_F(HzbSpdParityTest, ProductionShape4K) { RunParityCase(3840u, 2160u); }

// The supported-extent boundary: 13 levels, both axes exactly at the mip-7
// 32x32 tile cap (4223 >> 7 == 32), tail level 12, and the edge-overlay
// arrays at peak occupancy (mip2 = 1055 wide -> eCol/eRow index 1054, one
// below the 1056 capacity).
TEST_F(HzbSpdParityTest, MaxSupportedExtentBoundary) { RunParityCase(4223u, 4223u); }

} // namespace
