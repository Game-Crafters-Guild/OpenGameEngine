// GPU-executed parity tests for the FUSED two-dispatch SPD build
// (hzb_spd_minmax.comp + hzb_spd_minmax_finalize.comp) vs the hzb_build.comp
// chain, run once per reduce op.
//
// The fused build's content contract is BIT-IDENTICAL pyramids on BOTH of its
// outputs: one seeded depth image feeds the two fused dispatches and two
// independent chain runs (mode 1 = MIN, mode 2 = MAX), and every texel of every
// mip of both pyramids is compared as raw float bits. "Close" is a failure —
// the occlusion consumer and the SSR traversal consumer each depend on an exact
// conservative bound, and a fused build that reuses LDS across the two phases
// can corrupt one pyramid while leaving the other perfect.
//
// Sizes deliberately cover the adversarial NPOT shapes, matching
// HzbSpdParityTests' set:
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
    uint32_t mode; // 0 = copy depth into mip 0, 1 = 2x2 MIN reduce, 2 = 2x2 MAX reduce
};

// Mirrors HZBBuildNode.cpp's HzbSpdPC (shared by hzb_spd_minmax.comp and
// hzb_spd_minmax_finalize.comp — the fused pair takes the same three words as
// the single-pyramid pair, since the second pyramid needs no extra state).
struct HzbSpdPC
{
    uint32_t width;
    uint32_t height;
    uint32_t mipCount;
};

constexpr uint32_t kSpdTileSize = 64u;
constexpr uint32_t kSpdTileMipArrayCount = 6u;
constexpr uint32_t kSpdFinalizeMipArrayCount = 12u;

constexpr uint32_t kChainModeCopy = 0u;
constexpr uint32_t kChainModeMin = 1u;
constexpr uint32_t kChainModeMax = 2u;

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

class HzbSpdMinMaxParityTest : public ::testing::Test
{
  protected:
    // The four pyramids a parity case reads back: the two chain references and
    // the two the fused pair produced from a single depth read.
    struct ParityResult
    {
        std::vector<std::vector<float>> ChainMin;
        std::vector<std::vector<float>> ChainMax;
        std::vector<std::vector<float>> FusedMin;
        std::vector<std::vector<float>> FusedMax;
    };

    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        // Per-mip chain pipeline (the parity reference for both reduce ops —
        // only the push-constant mode differs between the two runs).
        {
            ShaderPackage pkg{};
            std::string err;
            if (!LoadShaderPkg("Shaders/hzb_build.shaderpkg", ShaderSourceKind::SpirV, pkg, &err))
                FAIL() << "hzb_build.shaderpkg not available: " << err;
            auto itCs = pkg.stageBytes.find("cs");
            ASSERT_NE(itCs, pkg.stageBytes.end());

            m_ChainLayout.debugName = "HzbSpdMinMaxParity_ChainLayout";
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
            cd.DebugName = "HzbSpdMinMaxParity.Chain";
            m_ChainPipeline =
                m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(cd));
            ASSERT_TRUE(m_ChainPipeline.IsValid());
        }

        // Fused tile pipeline: one depth read, both pyramids' mips 0..6.
        {
            ShaderPackage pkg{};
            std::string err;
            if (!LoadShaderPkg("Shaders/hzb_spd_minmax.shaderpkg", ShaderSourceKind::SpirV, pkg, &err))
                FAIL() << "hzb_spd_minmax.shaderpkg not available: " << err;
            auto itCs = pkg.stageBytes.find("cs");
            ASSERT_NE(itCs, pkg.stageBytes.end());

            m_TileLayout.debugName = "HzbSpdMinMaxParity_TileLayout";
            auto add = [&](uint32_t binding, uint32_t count) {
                DescriptorBinding b{};
                b.binding = binding;
                b.type = DescriptorType::StorageImage;
                b.count = count;
                b.shaderStages = kShaderStageCompute;
                m_TileLayout.bindings.push_back(b);
            };
            add(0u, 1u);                    // uDepth
            add(1u, 1u);                    // uMip0Min
            add(2u, kSpdTileMipArrayCount); // uMipsMin[6]
            add(3u, 1u);                    // uMip0Max
            add(4u, kSpdTileMipArrayCount); // uMipsMax[6]
            ComputePipelineDesc cd{};
            cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(itCs->second);
            cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_TileLayout));
            cd.PushConstants.Size = sizeof(HzbSpdPC);
            cd.PushConstants.StageMask = kShaderStageCompute;
            cd.DebugName = "HzbSpdMinMaxParity.Tile";
            m_TilePipeline =
                m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(cd));
            ASSERT_TRUE(m_TilePipeline.IsValid());
        }

        // Fused finalize pipeline: edge fixups plus mips 7.. for both pyramids.
        {
            ShaderPackage pkg{};
            std::string err;
            if (!LoadShaderPkg("Shaders/hzb_spd_minmax_finalize.shaderpkg", ShaderSourceKind::SpirV, pkg, &err))
                FAIL() << "hzb_spd_minmax_finalize.shaderpkg not available: " << err;
            auto itCs = pkg.stageBytes.find("cs");
            ASSERT_NE(itCs, pkg.stageBytes.end());

            m_FinalizeLayout.debugName = "HzbSpdMinMaxParity_FinalizeLayout";
            for (uint32_t i = 0; i < 2u; ++i)
            {
                DescriptorBinding b{};
                b.binding = i; // 0 = uMipsMin[12], 1 = uMipsMax[12]
                b.type = DescriptorType::StorageImage;
                b.count = kSpdFinalizeMipArrayCount;
                b.shaderStages = kShaderStageCompute;
                m_FinalizeLayout.bindings.push_back(b);
            }
            ComputePipelineDesc cd{};
            cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(itCs->second);
            cd.DescriptorSetLayouts.push_back(
                m_Device->InternDescriptorSetLayout(m_FinalizeLayout));
            cd.PushConstants.Size = sizeof(HzbSpdPC);
            cd.PushConstants.StageMask = kShaderStageCompute;
            cd.DebugName = "HzbSpdMinMaxParity.Finalize";
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
        vd.debugName = "HzbSpdMinMaxParity.MipView";
        TextureViewHandle v = m_Device->CreateTextureView(tex, vd);
        m_Views.push_back(v);
        return v;
    }

    // One reference pyramid: a mode-0 copy into mip 0, then one 2x2 reduce
    // dispatch per further mip under `reduceMode`. Runs twice per case (MIN and
    // MAX), which is the only thing that differs between the two references.
    void RecordChain(CommandList& cl, TextureHandle depthTex, TextureHandle dstTex, uint32_t w,
                     uint32_t h, uint32_t levels, uint32_t reduceMode,
                     const ResourceBarrier& computeToCompute)
    {
        {
            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_ChainLayout;
            dsDesc.transient = true;
            dsDesc.debugName = "HzbSpdMinMaxParity.Chain.DS";
            DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
            m_Device->UpdateStorageImageBinding(ds, 0, depthTex);
            m_Device->UpdateStorageImageBinding(ds, 1, MipView(dstTex, 0u));
            cl.SetPipeline(m_ChainPipeline);
            cl.BindDescriptorSet(0, ds, m_ChainPipeline);
            HzbBuildPC pc{w, h, w, h, kChainModeCopy};
            cl.SetPushConstants(pc);
            cl.Dispatch((w + 7u) / 8u, (h + 7u) / 8u, 1u);
            cl.Barrier(computeToCompute);
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
            dsDesc.debugName = "HzbSpdMinMaxParity.Chain.DS";
            DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
            m_Device->UpdateStorageImageBinding(ds, 0, MipView(dstTex, m - 1u));
            m_Device->UpdateStorageImageBinding(ds, 1, MipView(dstTex, m));
            cl.SetPipeline(m_ChainPipeline);
            cl.BindDescriptorSet(0, ds, m_ChainPipeline);
            HzbBuildPC pc{dstW, dstH, srcW, srcH, reduceMode};
            cl.SetPushConstants(pc);
            cl.Dispatch((dstW + 7u) / 8u, (dstH + 7u) / 8u, 1u);
            cl.Barrier(computeToCompute);
        }
    }

    // Runs both reference chains and the fused pair over the same depth
    // pattern, then reads back every mip of all four pyramids.
    void BuildAll(uint32_t w, uint32_t h, const std::vector<float>& depthTexels, ParityResult& out)
    {
        const uint32_t levels = MipChainLength(w, h);
        ASSERT_LE(w >> 7u, 32u) << "size exceeds the fused path's mip-7 tile cap";
        ASSERT_LE(h >> 7u, 32u) << "size exceeds the fused path's mip-7 tile cap";

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
        dd.debugName = "HzbSpdMinMaxParity.Depth";
        TextureHandle depthTex = m_Device->CreateTexture(dd);
        m_Textures.push_back(depthTex);

        BufferDesc ud{};
        ud.size = depthTexels.size() * sizeof(float);
        ud.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
        ud.memoryUsage = BufferMemoryUsage::Upload;
        ud.debugName = "HzbSpdMinMaxParity.DepthUpload";
        BufferHandle upload = m_Device->CreateBuffer(ud);
        m_Buffers.push_back(upload);
        void* mapped = m_Device->MapBuffer(upload);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(mapped, depthTexels.data(), ud.size);
        m_Device->UnmapBuffer(upload);

        TextureHandle chainMinTex = MakePyramid(w, h, levels, "HzbSpdMinMaxParity.ChainMin");
        TextureHandle chainMaxTex = MakePyramid(w, h, levels, "HzbSpdMinMaxParity.ChainMax");
        TextureHandle fusedMinTex = MakePyramid(w, h, levels, "HzbSpdMinMaxParity.FusedMin");
        TextureHandle fusedMaxTex = MakePyramid(w, h, levels, "HzbSpdMinMaxParity.FusedMax");

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->CopyBufferToTextureSubresource(upload, depthTex, 0u, 0u, w, h);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(depthTex, ResourceState::CopyDest,
                                                          ResourceState::UnorderedAccess));
        // The pyramids are write-first, but "nothing wrote it yet" is not a
        // layout: leaving their mips untransitioned makes the readback below
        // resolve oldLayout = UNDEFINED, which may discard them (see the header).
        for (TextureHandle pyramid : {chainMinTex, chainMaxTex, fusedMinTex, fusedMaxTex})
            TransitionForStorageAccess(cl.get(), pyramid, ResourceState::Undefined, 0u, levels);

        const ResourceBarrier computeToCompute = ResourceBarrier::CreateMemoryBarrier(
            static_cast<uint64_t>(PipelineStageMask::ComputeShader),
            static_cast<uint64_t>(PipelineStageMask::ComputeShader),
            static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
            static_cast<uint64_t>(ResourceAccessMask::ShaderRead));

        // ── Path A: the per-mip chain, once per reduce op ───────────────────
        RecordChain(*cl, depthTex, chainMinTex, w, h, levels, kChainModeMin, computeToCompute);
        RecordChain(*cl, depthTex, chainMaxTex, w, h, levels, kChainModeMax, computeToCompute);

        // ── Path B: the two fused dispatches (tile pass, barrier, finalize),
        // which produce BOTH pyramids from one depth read ───────────────────
        {
            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_TileLayout;
            dsDesc.transient = true;
            dsDesc.debugName = "HzbSpdMinMaxParity.Tile.DS";
            DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
            m_Device->UpdateStorageImageBinding(ds, 0, depthTex);
            m_Device->UpdateStorageImageBinding(ds, 1, MipView(fusedMinTex, 0u));
            m_Device->UpdateStorageImageBinding(ds, 3, MipView(fusedMaxTex, 0u));
            for (uint32_t i = 0; i < kSpdTileMipArrayCount; ++i)
            {
                const uint32_t mip = std::min(i + 1u, levels - 1u);
                m_Device->UpdateStorageImageBinding(ds, 2, MipView(fusedMinTex, mip), i);
                m_Device->UpdateStorageImageBinding(ds, 4, MipView(fusedMaxTex, mip), i);
            }

            const uint32_t wgX = (w + kSpdTileSize - 1u) / kSpdTileSize;
            const uint32_t wgY = (h + kSpdTileSize - 1u) / kSpdTileSize;
            cl->SetPipeline(m_TilePipeline);
            cl->BindDescriptorSet(0, ds, m_TilePipeline);
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
            dsDesc.debugName = "HzbSpdMinMaxParity.Finalize.DS";
            DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
            for (uint32_t i = 0; i < kSpdFinalizeMipArrayCount; ++i)
            {
                const uint32_t mip = std::min(i + 1u, levels - 1u);
                m_Device->UpdateStorageImageBinding(ds, 0, MipView(fusedMinTex, mip), i);
                m_Device->UpdateStorageImageBinding(ds, 1, MipView(fusedMaxTex, mip), i);
            }
            cl->SetPipeline(m_FinalizePipeline);
            cl->BindDescriptorSet(0, ds, m_FinalizePipeline);
            HzbSpdPC pc{w, h, levels};
            cl->SetPushConstants(pc);
            cl->Dispatch(1u, 1u, 1u);
            cl->Barrier(computeToCompute);
        }

        // ── Readback: every mip of all four pyramids ────────────────────────
        const TextureHandle sources[kReadbackPyramidCount] = {chainMinTex, chainMaxTex, fusedMinTex,
                                                              fusedMaxTex};
        std::vector<std::vector<float>>* dests[kReadbackPyramidCount] = {
            &out.ChainMin, &out.ChainMax, &out.FusedMin, &out.FusedMax};
        std::vector<BufferHandle> readback[kReadbackPyramidCount];

        for (uint32_t p = 0u; p < kReadbackPyramidCount; ++p)
            readback[p].resize(levels);
        for (uint32_t m = 0u; m < levels; ++m)
        {
            const uint32_t mw = std::max(1u, w >> m);
            const uint32_t mh = std::max(1u, h >> m);
            const size_t bytes = static_cast<size_t>(mw) * mh * sizeof(float);
            for (uint32_t p = 0u; p < kReadbackPyramidCount; ++p)
            {
                readback[p][m] = m_Device->CreateReadbackBuffer(bytes);
                m_Buffers.push_back(readback[p][m]);
                cl->CopyTextureSubresourceToBuffer(sources[p], m, 0u, readback[p][m], mw, mh, 0, 0,
                                                   0, 0);
            }
        }
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->FinalizeFrame();
        m_Device->WaitForIdle();

        for (uint32_t p = 0u; p < kReadbackPyramidCount; ++p)
            dests[p]->resize(levels);
        for (uint32_t m = 0u; m < levels; ++m)
        {
            const uint32_t mw = std::max(1u, w >> m);
            const uint32_t mh = std::max(1u, h >> m);
            const size_t count = static_cast<size_t>(mw) * mh;
            for (uint32_t p = 0u; p < kReadbackPyramidCount; ++p)
            {
                (*dests[p])[m].resize(count);
                const void* src = m_Device->MapBuffer(readback[p][m]);
                ASSERT_NE(src, nullptr);
                std::memcpy((*dests[p])[m].data(), src, count * sizeof(float));
                m_Device->UnmapBuffer(readback[p][m]);
            }
        }
    }

    // Bit-exact comparison over the full pyramid; failure names the pyramid and
    // the first mismatching (mip, x, y) with both bit patterns.
    void ExpectBitExact(const char* pyramid, uint32_t w,
                        const std::vector<std::vector<float>>& chain,
                        const std::vector<std::vector<float>>& fused)
    {
        ASSERT_EQ(chain.size(), fused.size()) << pyramid;
        // A == B is vacuous if both are blank: two pyramids the driver discarded
        // compare bit-identical. Pin the comparison to real data first.
        ASSERT_FALSE(IsAllZero(chain)) << pyramid << " chain pyramid is entirely zero — "
                                                     "nothing was compared";
        ASSERT_FALSE(IsAllZero(fused)) << pyramid << " fused pyramid is entirely zero — "
                                                     "nothing was compared";
        for (uint32_t m = 0u; m < chain.size(); ++m)
        {
            const uint32_t mw = std::max(1u, w >> m);
            ASSERT_EQ(chain[m].size(), fused[m].size()) << pyramid << " mip " << m;
            for (size_t i = 0; i < chain[m].size(); ++i)
            {
                const uint32_t a = std::bit_cast<uint32_t>(chain[m][i]);
                const uint32_t b = std::bit_cast<uint32_t>(fused[m][i]);
                if (a != b)
                {
                    ADD_FAILURE() << pyramid << " pyramid: mip " << m << " texel (" << (i % mw)
                                  << ", " << (i / mw) << "): chain bits 0x" << std::hex << a
                                  << " vs fused bits 0x" << b;
                    return; // first divergence is the diagnostic; avoid failure spam
                }
            }
        }
    }

    void RunParityCase(uint32_t w, uint32_t h)
    {
        SCOPED_TRACE(::testing::Message() << "extent " << w << "x" << h);
        ParityResult result;
        BuildAll(w, h, MakeDepthPattern(w, h), result);
        if (::testing::Test::HasFatalFailure())
            return;
        // Both pyramids are checked even if the first diverges: which of the two
        // is wrong localizes the bug to the reduce op vs the shared LDS reuse.
        ExpectBitExact("MIN", w, result.ChainMin, result.FusedMin);
        ExpectBitExact("MAX", w, result.ChainMax, result.FusedMax);
    }

    static constexpr uint32_t kReadbackPyramidCount = 4u;

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_ChainLayout{};
    DescriptorSetLayoutDesc m_TileLayout{};
    DescriptorSetLayoutDesc m_FinalizeLayout{};
    PipelineHandle m_ChainPipeline{};
    PipelineHandle m_TilePipeline{};
    PipelineHandle m_FinalizePipeline{};
    std::vector<TextureHandle> m_Textures;
    std::vector<TextureViewHandle> m_Views;
    std::vector<BufferHandle> m_Buffers;
};

TEST_F(HzbSpdMinMaxParityTest, Pow2SingleTile) { RunParityCase(4u, 4u); }

TEST_F(HzbSpdMinMaxParityTest, NpotTiny) { RunParityCase(5u, 3u); }

// levels == 2: the finalize pass is skipped entirely (mip 1 is already exact
// from the tile pass — nothing to repair, no mips 7+), so both pyramids must be
// complete out of the tile dispatch alone.
TEST_F(HzbSpdMinMaxParityTest, TwoLevelChainSkipsFinalize) { RunParityCase(3u, 2u); }

TEST_F(HzbSpdMinMaxParityTest, ExactTile) { RunParityCase(64u, 64u); }

TEST_F(HzbSpdMinMaxParityTest, NpotOddBothAxes) { RunParityCase(67u, 35u); }

// mip1 = 65x33: mip2's last column must fold mip1 col 64, which belongs to the
// NEXT 64x64 tile — the exact cross-workgroup case the finalize pass's edge
// fixup exists for, now on two pyramids sharing one eCol/eRow overlay.
TEST_F(HzbSpdMinMaxParityTest, CrossTileFoldFixup) { RunParityCase(130u, 66u); }

TEST_F(HzbSpdMinMaxParityTest, NpotDeepOddChain) { RunParityCase(259u, 131u); }

// Odd at every level: 1023 -> 511 -> 255 -> ... folds on both axes throughout.
TEST_F(HzbSpdMinMaxParityTest, AllOddChain) { RunParityCase(1023u, 1023u); }

TEST_F(HzbSpdMinMaxParityTest, ProductionShapeSmall) { RunParityCase(640u, 360u); }

TEST_F(HzbSpdMinMaxParityTest, ProductionShape1080p) { RunParityCase(1920u, 1080u); }

// 12-level chain (4K): exercises the finalize tail through GE_SPD_TAIL_LEVEL
// 11 and the deeper StoreMip cases.
TEST_F(HzbSpdMinMaxParityTest, ProductionShape4K) { RunParityCase(3840u, 2160u); }

// The supported-extent boundary: 13 levels, both axes exactly at the mip-7
// 32x32 tile cap (4223 >> 7 == 32), tail level 12, and the edge-overlay
// arrays at peak occupancy (mip2 = 1055 wide -> eCol/eRow index 1054, one
// below the 1056 capacity).
TEST_F(HzbSpdMinMaxParityTest, MaxSupportedExtentBoundary) { RunParityCase(4223u, 4223u); }

} // namespace
