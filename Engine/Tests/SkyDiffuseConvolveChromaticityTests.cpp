// GPU-executed contract for the sky's diffuse convolution (sky_diffuse_convolve.comp): the
// irradiance an up-facing surface receives carries the chromaticity of the sky dome above it,
// weighted by the cosine to the zenith, and nothing else.
//
// Such irradiance is bluer than the sky a viewer looks at, because the cosine weights the zenith
// above the bright, whiter horizon; the test states that too, against the unweighted mean.
//
// The synthetic sky depends only on the elevation: above the horizon it runs from a whitish
// horizon H to a blue zenith Z as sqrt(cos(theta)), below it is a flat ground colour. For that
// sky the cosine-weighted mean over the upper hemisphere is exactly 0.2 H + 0.8 Z, and the
// unweighted (solid-angle) mean is H/3 + 2Z/3.
//
// Skips only without a Vulkan device. A missing shader fails: the build compiles it into the
// build directory, so its absence is a build defect.

#include <gtest/gtest.h>

#include "EngineTestShaderSetup.h"
#include "Mathematics/HalfFloat.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"

#include "TestDeviceHelper.h"

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

using Rgb = std::array<float, 3>;

constexpr uint32_t kEnvSize = 32u;
constexpr uint32_t kIrradianceSize = 8u;
constexpr uint32_t kPosYFace = 2u;
constexpr Rgb kHorizon{0.90f, 0.85f, 0.80f};
constexpr Rgb kZenith{0.15f, 0.35f, 1.00f};
constexpr Rgb kGround{0.35f, 0.30f, 0.25f};
// The convolution integrates 128 samples of a pre-blurred mip; 5% on each ratio admits that
// sampling error and still rejects a cosine applied twice.
constexpr float kChromaTolerance = 0.05f;

Rgb Mix(const Rgb& a, const Rgb& b, float t)
{
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

Rgb SkyRadiance(float y)
{
    return y >= 0.0f ? Mix(kHorizon, kZenith, std::sqrt(y)) : kGround;
}

// Elevation (the direction's y) of a cube texel under the Vulkan cube-face convention.
float TexelElevation(uint32_t face, uint32_t x, uint32_t y, uint32_t n)
{
    const float s = (static_cast<float>(x) + 0.5f) / static_cast<float>(n) * 2.0f - 1.0f;
    const float t = (static_cast<float>(y) + 0.5f) / static_cast<float>(n) * 2.0f - 1.0f;
    float dir[3]{};
    switch (face)
    {
    case 0: dir[0] = 1.0f; dir[1] = -t; dir[2] = -s; break;
    case 1: dir[0] = -1.0f; dir[1] = -t; dir[2] = s; break;
    case 2: dir[0] = s; dir[1] = 1.0f; dir[2] = t; break;
    case 3: dir[0] = s; dir[1] = -1.0f; dir[2] = -t; break;
    case 4: dir[0] = s; dir[1] = -t; dir[2] = 1.0f; break;
    default: dir[0] = -s; dir[1] = -t; dir[2] = -1.0f; break;
    }
    return dir[1] / std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
}

float RedOverBlue(const Rgb& c) { return c[0] / c[2]; }
float GreenOverBlue(const Rgb& c) { return c[1] / c[2]; }

class SkyDiffuseConvolveChromaticityTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        const std::filesystem::path spvPath =
            GameEngine::Testing::EngineTestShaderPathResolver("Shaders/sky_diffuse_convolve.comp.spv");
        ASSERT_FALSE(spvPath.empty())
            << "Shaders/sky_diffuse_convolve.comp.spv was not built into the build directory";
        const std::vector<uint8_t> spv = Utils::ReadFile(spvPath.string());
        ASSERT_FALSE(spv.empty());

        DescriptorBinding env{};
        env.binding = 0;
        env.type = DescriptorType::CombinedImageSampler;
        env.count = 1;
        env.shaderStages = kShaderStageCompute;
        DescriptorBinding out{};
        out.binding = 1;
        out.type = DescriptorType::StorageImage;
        out.count = 1;
        out.shaderStages = kShaderStageCompute;
        m_Layout.debugName = "SkyDiffuseConvolveTest.Set0";
        m_Layout.bindings = {env, out};

        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(spv);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        cd.DebugName = "SkyDiffuseConvolveTest";
        m_Pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(cd));
        ASSERT_TRUE(m_Pipeline.IsValid());

        m_Sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearClamp("SkyDiffuseConvolveTestSampler"));
        ASSERT_TRUE(m_Sampler.IsValid());
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (TextureViewHandle v : m_Views)
            m_Device->DestroyTextureView(v);
        for (TextureHandle t : m_Textures)
            m_Device->DestroyTexture(t);
        for (BufferHandle b : m_Buffers)
            m_Device->DestroyBuffer(b);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
        m_Device->Shutdown();
    }

    BufferHandle MakeBuffer(size_t bytes, uint32_t usage, BufferMemoryUsage memory, const char* name)
    {
        BufferDesc bd{};
        bd.size = bytes;
        bd.usage = usage;
        bd.memoryUsage = memory;
        bd.debugName = name;
        const BufferHandle b = m_Device->CreateBuffer(bd);
        m_Buffers.push_back(b);
        return b;
    }

    // Convolves the synthetic sky and returns the +Y irradiance at the face centre.
    Rgb ConvolvePosY()
    {
        TextureDesc ed{};
        ed.width = kEnvSize;
        ed.height = kEnvSize;
        ed.arrayLayers = 6;
        ed.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
        ed.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
                   static_cast<uint32_t>(TextureUsage::TransferDst);
        ed.flags = TextureCreateFlags::CubeCompatible;
        ed.debugName = "SkyDiffuseConvolveTest.Env";
        const TextureHandle envTex = m_Device->CreateTexture(ed);
        m_Textures.push_back(envTex);
        TextureViewDesc cube{};
        cube.viewType = TextureViewType::ViewCube;
        cube.debugName = "SkyDiffuseConvolveTest.EnvCube";
        const TextureViewHandle envView = m_Device->CreateTextureView(envTex, cube);
        m_Views.push_back(envView);

        TextureDesc od{};
        od.width = kIrradianceSize;
        od.height = kIrradianceSize;
        od.arrayLayers = 6;
        od.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
        od.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess) |
                   static_cast<uint32_t>(TextureUsage::TransferSrc);
        od.debugName = "SkyDiffuseConvolveTest.Irradiance";
        const TextureHandle outTex = m_Device->CreateTexture(od);
        m_Textures.push_back(outTex);
        TextureViewDesc array{};
        array.viewType = TextureViewType::View2DArray;
        array.debugName = "SkyDiffuseConvolveTest.IrradianceArray";
        const TextureViewHandle outView = m_Device->CreateTextureView(outTex, array);
        m_Views.push_back(outView);

        const size_t faceBytes = static_cast<size_t>(kEnvSize) * kEnvSize * 4u * sizeof(float);
        const BufferHandle upload = MakeBuffer(faceBytes * 6u, static_cast<uint32_t>(BufferUsage::TransferSrc),
                                               BufferMemoryUsage::Upload, "SkyDiffuseConvolveTest.Upload");
        auto* texels = static_cast<float*>(m_Device->MapBuffer(upload));
        for (uint32_t face = 0; face < 6u; ++face)
            for (uint32_t y = 0; y < kEnvSize; ++y)
                for (uint32_t x = 0; x < kEnvSize; ++x)
                {
                    const Rgb c = SkyRadiance(TexelElevation(face, x, y, kEnvSize));
                    float* t = texels + (static_cast<size_t>(face) * kEnvSize * kEnvSize + y * kEnvSize + x) * 4u;
                    t[0] = c[0];
                    t[1] = c[1];
                    t[2] = c[2];
                    t[3] = 1.0f;
                }
        m_Device->UnmapBuffer(upload);

        const size_t readBytes = static_cast<size_t>(kIrradianceSize) * kIrradianceSize * 4u * sizeof(uint16_t);
        const BufferHandle readback = MakeBuffer(readBytes, static_cast<uint32_t>(BufferUsage::TransferDst),
                                                 BufferMemoryUsage::Readback, "SkyDiffuseConvolveTest.Readback");

        DescriptorSetDesc dsDesc{};
        dsDesc.layout = m_Layout;
        dsDesc.transient = true;
        dsDesc.debugName = "SkyDiffuseConvolveTest.DS";
        const DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
        m_Device->UpdateCombinedImageSamplerBinding(ds, 0, envView, m_Sampler);
        m_Device->UpdateStorageImageBinding(ds, 1, outView);

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(envTex, ResourceState::Undefined, ResourceState::CopyDest,
                                                          0, 1, 0, 6));
        for (uint32_t face = 0; face < 6u; ++face)
            cl->CopyBufferToTextureSubresource(upload, envTex, 0u, face, kEnvSize, kEnvSize, faceBytes * face);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(envTex, ResourceState::CopyDest,
                                                          ResourceState::ShaderResource, 0, 1, 0, 6));
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(outTex, ResourceState::Undefined,
                                                          ResourceState::UnorderedAccess, 0, 1, 0, 6));
        cl->SetPipeline(m_Pipeline);
        cl->BindDescriptorSet(0, ds, m_Pipeline);
        cl->Dispatch((kIrradianceSize + 7u) / 8u, (kIrradianceSize + 7u) / 8u, 6u);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(outTex, ResourceState::UnorderedAccess,
                                                          ResourceState::CopySource, 0, 1, 0, 6));
        cl->CopyTextureSubresourceToBuffer(outTex, 0u, kPosYFace, readback, kIrradianceSize, kIrradianceSize);
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();

        std::vector<uint16_t> halfs(readBytes / sizeof(uint16_t));
        const void* mapped = m_Device->MapBuffer(readback);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
            std::memcpy(halfs.data(), mapped, readBytes);
        m_Device->UnmapBuffer(readback);

        // The four texels around the face centre, whose normals are within a few degrees of +Y.
        Rgb sum{};
        for (uint32_t y = kIrradianceSize / 2u - 1u; y <= kIrradianceSize / 2u; ++y)
            for (uint32_t x = kIrradianceSize / 2u - 1u; x <= kIrradianceSize / 2u; ++x)
                for (uint32_t c = 0; c < 3u; ++c)
                    sum[c] += Mathematics::HalfToFloat(halfs[(y * kIrradianceSize + x) * 4u + c]) / 4.0f;
        return sum;
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    SamplerHandle m_Sampler{};
    std::vector<TextureHandle> m_Textures;
    std::vector<TextureViewHandle> m_Views;
    std::vector<BufferHandle> m_Buffers;
};

} // namespace

TEST_F(SkyDiffuseConvolveChromaticityTest, UpFacingIrradianceCarriesTheCosineWeightedDomeChromaticity)
{
    const Rgb irradiance = ConvolvePosY();
    ASSERT_GT(irradiance[2], 0.0f) << "the convolution wrote nothing";

    const Rgb cosineWeighted = Mix(kHorizon, kZenith, 0.8f);
    const Rgb unweighted = Mix(kHorizon, kZenith, 2.0f / 3.0f);

    EXPECT_NEAR(RedOverBlue(irradiance) / RedOverBlue(cosineWeighted), 1.0f, kChromaTolerance)
        << "r/b of the up-facing irradiance " << RedOverBlue(irradiance) << " against the cosine-weighted dome "
        << RedOverBlue(cosineWeighted);
    EXPECT_NEAR(GreenOverBlue(irradiance) / GreenOverBlue(cosineWeighted), 1.0f, kChromaTolerance)
        << "g/b of the up-facing irradiance " << GreenOverBlue(irradiance) << " against the cosine-weighted dome "
        << GreenOverBlue(cosineWeighted);

    // The ambient is bluer than the sky a viewer sees, and correctly so: the cosine favours the zenith.
    EXPECT_LT(RedOverBlue(irradiance), RedOverBlue(unweighted));
}
