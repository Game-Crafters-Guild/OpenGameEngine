// GPU-executed parity gate for the Slang lane (spike Gate 2, HZB compute):
// build the SAME pyramid twice on a real device — once with the shipped
// glslang-compiled hzb_build.comp package, once with hzb_build.slang compiled
// through ShaderCompileService's slangc lane — and require BIT-IDENTICAL
// texels on every mip. The chain is pure IEEE min over an identical footprint
// rule, so any codegen divergence that matters (operand order, precision
// decoration, wrong fold) lands here as a bit mismatch instead of a
// hard-to-attribute wrong cull in the editor.
//
// Skips without a Vulkan device or without GE_SLANGC (the external toolchain is
// never vendored); fails when the build did not stage Shaders/Slang. Opt-in:
// built into EngineRenderOptInTests, which no gate runs.

#include <gtest/gtest.h>

#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include "StorageImageLayoutHelper.h"
#include "TestDeviceHelper.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

struct HzbBuildPC
{
    uint32_t dstW;
    uint32_t dstH;
    uint32_t srcW;
    uint32_t srcH;
    uint32_t mode; // 0 = copy, 1 = 2x2 MIN reduce
};

class ScopedEnvVar
{
public:
    ScopedEnvVar(const char* name, const char* value) : m_Name(name)
    {
        if (const char* old = std::getenv(name))
        {
            m_Old = old;
            m_HadOld = true;
        }
        Set(value);
    }
    ~ScopedEnvVar() { Set(m_HadOld ? m_Old.c_str() : ""); }

private:
    void Set(const char* value)
    {
#ifdef _WIN32
        _putenv_s(m_Name, value);
#else
        if (value && value[0])
            setenv(m_Name, value, 1);
        else
            unsetenv(m_Name);
#endif
    }
    const char* m_Name;
    std::string m_Old;
    bool m_HadOld = false;
};

// The Slang source the build stages beside the packaged shaders, wherever the test runs from.
fs::path StagedSlangSource()
{
    return fs::path(ENGINE_TEST_BUILD_DIR) / "Shaders/Slang/hzb_build.slang";
}

class HzbSlangParityTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (!(std::getenv("GE_SLANGC") && std::getenv("GE_SLANGC")[0]))
            GTEST_SKIP() << "GE_SLANGC not set — external slangc unavailable";
        std::error_code ec;
        if (!fs::exists(StagedSlangSource(), ec))
            FAIL() << StagedSlangSource().generic_string() << " is not staged in the build root";

        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        m_Layout.debugName = "HzbSlangParity_SetLayout";
        for (uint32_t i = 0; i < 2u; ++i)
        {
            DescriptorBinding b{};
            b.binding = i;
            b.type = DescriptorType::StorageImage;
            b.count = 1u;
            b.shaderStages = kShaderStageCompute;
            m_Layout.bindings.push_back(b);
        }

        // Arm 1: the shipped glslang-compiled package.
        {
            ShaderPackage pkg{};
            std::string err;
            if (!LoadShaderPkg("Shaders/hzb_build.shaderpkg", ShaderSourceKind::SpirV, pkg, &err))
                FAIL() << "hzb_build.shaderpkg not available: " << err;
            auto itCs = pkg.stageBytes.find("cs");
            ASSERT_NE(itCs, pkg.stageBytes.end());
            m_GlslPipeline = MakePipeline(itCs->second, "HzbParity.Glsl");
            ASSERT_TRUE(m_GlslPipeline.IsValid());
        }

        // Arm 2: hzb_build.slang through the full ShaderCompileService lane
        // (slangc -> reflect -> meta), exactly what HZBBuildNode runs.
        {
            ScopedEnvVar lane("GE_SHADER_SLANG_LANE", "1");
            m_CacheRoot = fs::temp_directory_path() /
                          ("ge_hzb_slang_parity_" +
                           std::to_string(::testing::UnitTest::GetInstance()->random_seed()));

            ShaderProgramCompileRequest req{};
            req.debugName = "hzb_build.slang(parity)";
            const fs::path src = StagedSlangSource();
            req.baseDirectory = src.parent_path();
            req.cacheRoot = m_CacheRoot;
            ShaderStageCompileSpec spec{};
            spec.stage = "cs";
            spec.sourcePath = src;
            req.stages.push_back(std::move(spec));

            ShaderProgramCompileResult res{};
            std::string err;
            ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(req, ShaderSourceKind::SpirV, res, &err)) << err;
            m_SlangPipeline = MakePipeline(res.stageBytes.at("cs"), "HzbParity.Slang");
            ASSERT_TRUE(m_SlangPipeline.IsValid());
        }
    }

    void TearDown() override
    {
        for (TextureViewHandle& v : m_Views)
            if (v.IsValid())
                m_Device->DestroyTextureView(v);
        m_Views.clear();
        for (BufferHandle& b : m_Buffers)
            if (b.IsValid())
                m_Device->DestroyBuffer(b);
        m_Buffers.clear();
        if (m_Device)
            m_Device->Shutdown();
        if (!m_CacheRoot.empty())
        {
            std::error_code ec;
            fs::remove_all(m_CacheRoot, ec);
        }
    }

    PipelineHandle MakePipeline(const std::vector<uint8_t>& spv, const char* name)
    {
        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(spv);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        cd.PushConstants.Size = sizeof(HzbBuildPC);
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = name;
        return m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(cd));
    }

    TextureViewHandle MipView(TextureHandle tex, uint32_t mip)
    {
        TextureViewDesc vd{};
        vd.viewType = TextureViewType::View2D;
        vd.baseMip = mip;
        vd.levelCount = 1u;
        vd.baseLayer = 0u;
        vd.layerCount = 1u;
        vd.debugName = "HzbParity.MipView";
        TextureViewHandle v = m_Device->CreateTextureView(tex, vd);
        m_Views.push_back(v);
        return v;
    }

    // Seed mip 0, reduce the full chain with `pipeline`, read every mip back.
    std::vector<std::vector<float>> BuildAndReadback(PipelineHandle pipeline, uint32_t w,
                                                     uint32_t h, const std::vector<float>& mip0)
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
        td.debugName = "HzbParity.Pyramid";
        TextureHandle tex = m_Device->CreateTexture(td);
        EXPECT_TRUE(tex.IsValid());

        BufferDesc ud{};
        ud.size = mip0.size() * sizeof(float);
        ud.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
        ud.memoryUsage = BufferMemoryUsage::Upload;
        ud.debugName = "HzbParity.Upload";
        BufferHandle upload = m_Device->CreateBuffer(ud);
        m_Buffers.push_back(upload);
        void* mapped = m_Device->MapBuffer(upload);
        EXPECT_NE(mapped, nullptr);
        std::memcpy(mapped, mip0.data(), ud.size);
        m_Device->UnmapBuffer(upload);

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->CopyBufferToTextureSubresource(upload, tex, 0u, 0u, w, h);

        // Every mip is about to be bound as a storage image, so every mip must
        // be in the storage-image layout first — including the ones nothing has
        // touched yet. Without this the readback below resolves oldLayout =
        // UNDEFINED and the driver may discard the mip (see the header).
        TransitionPyramidForStorageAccess(cl.get(), tex, levels);

        for (uint32_t m = 1u; m < levels; ++m)
        {
            const uint32_t srcW = std::max(1u, w >> (m - 1u));
            const uint32_t srcH = std::max(1u, h >> (m - 1u));
            const uint32_t dstW = std::max(1u, w >> m);
            const uint32_t dstH = std::max(1u, h >> m);

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "HzbParity.DS";
            DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
            EXPECT_TRUE(ds.IsValid());
            m_Device->UpdateStorageImageBinding(ds, 0, MipView(tex, m - 1u));
            m_Device->UpdateStorageImageBinding(ds, 1, MipView(tex, m));

            cl->SetPipeline(pipeline);
            cl->BindDescriptorSet(0, ds, pipeline);
            HzbBuildPC pc{dstW, dstH, srcW, srcH, 1u};
            cl->SetPushConstants(pc);
            cl->Dispatch((dstW + 7u) / 8u, (dstH + 7u) / 8u, 1u);

            ResourceBarrier rb = ResourceBarrier::CreateMemoryBarrier(
                static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
                static_cast<uint64_t>(ResourceAccessMask::ShaderRead));
            cl->Barrier(rb);
        }

        std::vector<BufferHandle> readbacks(levels);
        for (uint32_t m = 0u; m < levels; ++m)
        {
            const uint32_t mw = std::max(1u, w >> m);
            const uint32_t mh = std::max(1u, h >> m);
            readbacks[m] =
                m_Device->CreateReadbackBuffer(static_cast<size_t>(mw) * mh * sizeof(float));
            m_Buffers.push_back(readbacks[m]);
            cl->CopyTextureSubresourceToBuffer(tex, m, 0u, readbacks[m], mw, mh, 0, 0, 0, 0);
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
        m_Device->DestroyTexture(tex);
        return out;
    }

    void ExpectBitIdentical(uint32_t w, uint32_t h, const std::vector<float>& mip0)
    {
        const auto glsl = BuildAndReadback(m_GlslPipeline, w, h, mip0);
        const auto slang = BuildAndReadback(m_SlangPipeline, w, h, mip0);
        ASSERT_EQ(glsl.size(), slang.size());
        for (size_t m = 0; m < glsl.size(); ++m)
        {
            ASSERT_EQ(glsl[m].size(), slang[m].size()) << "mip " << m;
            const int cmp = std::memcmp(glsl[m].data(), slang[m].data(),
                                        glsl[m].size() * sizeof(float));
            EXPECT_EQ(cmp, 0) << w << "x" << h << " mip " << m
                              << " diverges between glslang and slangc builds";
        }
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_GlslPipeline{};
    PipelineHandle m_SlangPipeline{};
    fs::path m_CacheRoot;
    std::vector<TextureViewHandle> m_Views;
    std::vector<BufferHandle> m_Buffers;
};

TEST_F(HzbSlangParityTest, ChainPyramidBitIdenticalAcrossCompilers)
{
    // Pow2: every reduce is the plain 2x2 path.
    ExpectBitIdentical(4u, 4u,
                       {
                           0.90f, 0.10f, 0.55f, 0.60f, //
                           0.20f, 0.80f, 0.70f, 0.65f, //
                           0.00f, 0.30f, 0.95f, 0.85f, //
                           0.40f, 0.50f, 0.75f, 0.99f, //
                       });

    // NPOT with both odd extents: exercises the trailing row/column/corner
    // folds down a multi-level chain (67x41 -> 33x20 -> 16x10 -> 8x5 -> ...),
    // deterministic LCG values in (0,1) with exact float variety.
    const uint32_t w = 67u, h = 41u;
    std::vector<float> mip0(static_cast<size_t>(w) * h);
    uint32_t state = 0x12345678u;
    for (float& v : mip0)
    {
        state = state * 1664525u + 1013904223u;
        v = static_cast<float>(state >> 8) / 16777216.0f;
    }
    ExpectBitIdentical(w, h, mip0);
}

} // namespace
