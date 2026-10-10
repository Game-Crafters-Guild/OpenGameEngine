// Run the production DDGI glossy-resolve blur on a flat scene with non-finite
// texels planted in the history, the current resolve, or both. The blur
// accumulates into a temporal history, and mix() keeps a NaN even at weight 0,
// so an accepted non-finite sample would stay in the history for good (#2838).
#include <gtest/gtest.h>

#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "TestDeviceHelper.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <glm/gtc/packing.hpp>

using namespace GameEngine::Rendering;

namespace
{
constexpr uint32_t kSize = 8;
constexpr uint32_t kTexels = kSize * kSize;
constexpr uint32_t kPoisoned = 2 * kSize + 2;  // texel (2, 2)
constexpr uint32_t kNeighbour = 3 * kSize + 2;  // texel (2, 3), inside (2, 2)'s blur footprint
constexpr float kCurrent = 1.0f;
constexpr float kHistory = 2.0f;
constexpr float kDepth = 0.5f;  // reverse-Z; every texel is surface

class DDGIResolveBlurComputeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        ShaderPackage pkg{};
        std::string err;
        ASSERT_TRUE(LoadShaderPkg("Shaders/ddgi_glossy_resolve_blur.shaderpkg", m_Device->PreferredShaderSource(),
                                  pkg, &err))
            << err;
        m_Meta = std::move(pkg.meta);
        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(pkg.stageBytes.at("cs"));
        auto keepSet0 = [this](uint32_t set, DescriptorSetLayoutDesc& layout)
        {
            if (set == 0)
                m_Layout = layout;
        };
        MaterialHelper::ApplyShaderMetaToComputeDesc(*m_Device, m_Meta, cd, MaterialBuilder::MergeMode::Auto,
                                                     {true, 128}, keepSet0, &err);
        m_Pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(std::move(cd)));
        ASSERT_TRUE(m_Pipeline.IsValid()) << err;
        m_Sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearClamp("DDGIResolveBlurTest"));
        ASSERT_TRUE(m_Sampler.IsValid());
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (TextureHandle texture : m_Textures)
            m_Device->DestroyTexture(texture);
        for (BufferHandle buffer : m_Buffers)
            m_Device->DestroyBuffer(buffer);
        m_Device->DestroySampler(m_Sampler);
        m_Device->Shutdown();
    }

    BufferHandle MakeBuffer(size_t bytes, BufferUsage usage, BufferMemoryUsage memory)
    {
        BufferDesc desc{};
        desc.size = bytes;
        desc.usage = static_cast<uint32_t>(usage);
        desc.memoryUsage = memory;
        m_Buffers.push_back(m_Device->CreateBuffer(desc));
        return m_Buffers.back();
    }

    // An 8x8 texture uploaded from `texels` (RGBA16F when four channels, else R32F),
    // left in the shader-read state.
    TextureHandle MakeTexture(CommandList& cl, const std::vector<float>& texels, bool rgba)
    {
        TextureDesc desc{};
        desc.width = desc.height = kSize;
        desc.depth = desc.mipLevels = desc.arrayLayers = desc.sampleCount = 1;
        desc.format = static_cast<uint32_t>(rgba ? TextureFormat::R16G16B16A16_FLOAT : TextureFormat::R32_FLOAT);
        desc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::UnorderedAccess |
                                           TextureUsage::TransferSrc | TextureUsage::TransferDst);
        const TextureHandle texture = m_Device->CreateTexture(desc);
        m_Textures.push_back(texture);
        const size_t bytes = texels.size() * (rgba ? sizeof(uint16_t) : sizeof(float));
        const BufferHandle upload = MakeBuffer(bytes, BufferUsage::TransferSrc, BufferMemoryUsage::Upload);
        void* mapped = m_Device->MapBuffer(upload);
        if (rgba)
            for (size_t i = 0; i < texels.size(); ++i)
                static_cast<uint16_t*>(mapped)[i] = glm::packHalf1x16(texels[i]);
        else
            std::memcpy(mapped, texels.data(), bytes);
        m_Device->UnmapBuffer(upload);
        cl.Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::Undefined, ResourceState::CopyDest));
        cl.CopyBufferToTextureSubresource(upload, texture, 0, 0, kSize, kSize);
        cl.Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest, ResourceState::ShaderResource));
        return texture;
    }

    static std::vector<float> Rgba(float rgb, float alpha)
    {
        std::vector<float> texels(kTexels * 4, rgb);
        for (uint32_t i = 0; i < kTexels; ++i)
            texels[i * 4 + 3] = alpha;
        return texels;
    }

    static void SetRgb(std::vector<float>& texels, uint32_t texel, float rgb)
    {
        for (uint32_t c = 0; c < 3; ++c)
            texels[texel * 4 + c] = rgb;
    }

    static void ExpectAllFinite(const std::vector<float>& rgb)
    {
        for (uint32_t i = 0; i < kTexels; ++i)
            for (uint32_t c = 0; c < 3; ++c)
                ASSERT_TRUE(std::isfinite(rgb[i * 3 + c])) << "texel " << i << " is not finite";
    }

    // Blurs `current` against `history` with `temporalAlpha` of the weight on the
    // history (0 never reads it) and returns the accumulated irradiance's rgb,
    // one float per channel.
    std::vector<float> RunBlur(const std::vector<float>& current, const std::vector<float>& history,
                               float temporalAlpha = 0.5f)
    {
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        const TextureHandle irradianceIn = MakeTexture(*cl, current, true);
        const TextureHandle historyIn = MakeTexture(*cl, history, true);
        const TextureHandle depth = MakeTexture(*cl, std::vector<float>(kTexels, kDepth), false);
        const TextureHandle lobeIn = MakeTexture(*cl, Rgba(0.0f, 0.0f), true);
        const TextureHandle irradianceOut = MakeTexture(*cl, Rgba(0.0f, 0.0f), true);
        const TextureHandle lobeOut = MakeTexture(*cl, Rgba(0.0f, 0.0f), true);
        for (TextureHandle storage : {irradianceOut, lobeOut})
            cl->Barrier(ResourceBarrier::CreateTextureBarrier(storage, ResourceState::ShaderResource,
                                                              ResourceState::UnorderedAccess));

        // Identity matrices: every texel reprojects onto itself at its own depth,
        // so the history always matches.
        ViewParamsUBO params{};
        for (float* m : {params.ge_invProj, params.ge_invView, params.ge_prevViewProj})
            for (int i = 0; i < 4; ++i)
                m[i * 5] = 1.0f;
        const BufferHandle viewParams = MakeBuffer(sizeof(params), BufferUsage::Uniform, BufferMemoryUsage::Upload);
        std::memcpy(m_Device->MapBuffer(viewParams), &params, sizeof(params));
        m_Device->UnmapBuffer(viewParams);

        DescriptorSetDesc dsDesc{};
        dsDesc.layout = m_Layout;
        dsDesc.transient = true;
        const DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
        NamedDescriptorWriter writer(m_Device.get(), ds, m_Meta, 0);
        writer.AddCombinedImageSampler("uRoughIn", lobeIn, m_Sampler);
        writer.AddCombinedImageSampler("uGlossyIn", lobeIn, m_Sampler);
        writer.AddCombinedImageSampler("uViewDepth", depth, m_Sampler);
        writer.AddCombinedImageSampler("uIrradianceIn", irradianceIn, m_Sampler);
        writer.AddCombinedImageSampler("uHistoryIrr", historyIn, m_Sampler);
        writer.AddUniformBuffer("ViewParams", viewParams, 0, sizeof(params));
        writer.Flush();
        using GameEngine::Engine::Renderer::Pipeline::Nodes::Detail::BindStorageImageByName;
        BindStorageImageByName(m_Device.get(), ds, m_Meta, "uRoughOut", lobeOut);
        BindStorageImageByName(m_Device.get(), ds, m_Meta, "uGlossyOut", lobeOut);
        BindStorageImageByName(m_Device.get(), ds, m_Meta, "uIrradianceOut", irradianceOut);

        cl->SetPipeline(m_Pipeline);
        cl->BindDescriptorSet(0, ds, m_Pipeline);
        NamedPushConstantWriter pc(m_Meta, m_Meta.PushConstants[0].Name);
        pc.Add("lobesActive", 0u);
        pc.Add("temporalAlpha", temporalAlpha);
        pc.Flush(cl.get());
        cl->Dispatch(1, 1, 1);

        const BufferHandle readback = m_Device->CreateReadbackBuffer(kTexels * 4 * sizeof(uint16_t));
        m_Buffers.push_back(readback);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(irradianceOut, ResourceState::UnorderedAccess,
                                                          ResourceState::CopySource));
        cl->CopyTextureSubresourceToBuffer(irradianceOut, 0, 0, readback, kSize, kSize);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();

        std::vector<float> rgb;
        const auto* halves = static_cast<const uint16_t*>(m_Device->MapBuffer(readback));
        for (uint32_t i = 0; i < kTexels; ++i)
            for (uint32_t c = 0; c < 3; ++c)
                rgb.push_back(glm::unpackHalf1x16(halves[i * 4 + c]));
        m_Device->UnmapBuffer(readback);
        return rgb;
    }

    std::unique_ptr<IDevice> m_Device;
    ShaderMeta m_Meta;
    DescriptorSetLayoutDesc m_Layout;
    PipelineHandle m_Pipeline;
    SamplerHandle m_Sampler;
    std::vector<TextureHandle> m_Textures;
    std::vector<BufferHandle> m_Buffers;
};

} // namespace

// A neighbour brighter than the rest makes the poisoned texel's current blur
// differ from its unblurred resolve, so keeping the blur is observable apart
// from falling back to the resolve.
TEST_F(DDGIResolveBlurComputeTest, NonFiniteHistoryTexelLeavesTheCurrentBlur)
{
    std::vector<float> current = Rgba(kCurrent, kDepth);
    SetRgb(current, kNeighbour, 3.0f * kCurrent);
    std::vector<float> history = Rgba(kHistory, kDepth);
    SetRgb(history, kPoisoned, std::numeric_limits<float>::quiet_NaN());
    const std::vector<float> spatialOnly = RunBlur(current, history, 0.0f);
    ASSERT_NE(spatialOnly[kPoisoned * 3], kCurrent) << "the neighbour must reach the poisoned texel's blur";
    const std::vector<float> rgb = RunBlur(current, history);
    ExpectAllFinite(rgb);
    EXPECT_FLOAT_EQ(rgb[kPoisoned * 3], spatialOnly[kPoisoned * 3])
        << "the poisoned history texel must keep the current blur";
    EXPECT_FLOAT_EQ(rgb[0], 0.5f * (kCurrent + kHistory)) << "a finite history still blends";
}

TEST_F(DDGIResolveBlurComputeTest, NonFiniteCurrentTexelTakesTheHistory)
{
    std::vector<float> current = Rgba(kCurrent, kDepth);
    SetRgb(current, kPoisoned, std::numeric_limits<float>::infinity());
    const std::vector<float> rgb = RunBlur(current, Rgba(kHistory, kDepth));
    ExpectAllFinite(rgb);
    EXPECT_FLOAT_EQ(rgb[kPoisoned * 3], kHistory) << "the poisoned current texel must take the history";
}

// The neighbour's infinity makes the texel's current blur non-finite while its
// own resolve stays finite; its history was written at another depth.
TEST_F(DDGIResolveBlurComputeTest, NonFiniteCurrentNeverTakesAMismatchedHistory)
{
    std::vector<float> current = Rgba(kCurrent, kDepth);
    SetRgb(current, kNeighbour, std::numeric_limits<float>::infinity());
    std::vector<float> history = Rgba(kHistory, kDepth);
    history[kPoisoned * 4 + 3] = 0.2f * kDepth;
    const std::vector<float> rgb = RunBlur(current, history);
    ExpectAllFinite(rgb);
    EXPECT_FLOAT_EQ(rgb[kPoisoned * 3], kCurrent) << "a history at another depth must fall to the unblurred resolve";
}

// Current, history and the unblurred resolve all NaN at one texel: it stores 0,
// and the next frame, with that output as its history, blends from 0 again.
TEST_F(DDGIResolveBlurComputeTest, AllNonFiniteTexelStoresZeroAndRecovers)
{
    std::vector<float> current = Rgba(kCurrent, kDepth);
    SetRgb(current, kPoisoned, std::numeric_limits<float>::quiet_NaN());
    std::vector<float> history = Rgba(kHistory, kDepth);
    SetRgb(history, kPoisoned, std::numeric_limits<float>::quiet_NaN());
    const std::vector<float> rgb = RunBlur(current, history);
    ExpectAllFinite(rgb);
    EXPECT_FLOAT_EQ(rgb[kPoisoned * 3], 0.0f) << "a texel with nothing finite must store 0";

    std::vector<float> nextHistory = Rgba(0.0f, kDepth);
    for (uint32_t i = 0; i < kTexels; ++i)
        for (uint32_t c = 0; c < 3; ++c)
            nextHistory[i * 4 + c] = rgb[i * 3 + c];
    const std::vector<float> next = RunBlur(Rgba(kCurrent, kDepth), nextHistory);
    ExpectAllFinite(next);
    EXPECT_FLOAT_EQ(next[kPoisoned * 3], 0.5f * kCurrent) << "the next finite frame must blend from the stored 0";
}
