// GPU-executed gate for the at-surface ground-floor composite.
//
// The IBL capture renders from a camera the fill code places EXACTLY on the planet sphere
// (SkyEnvironmentSource FillCaptureUbo writes cameraPositionWS = 0; the shader puts the
// planet centre at (0, -planetRadius, 0)). There the ray-sphere solve is degenerate --
// t0 = tca - thc = R*sin(t) - R*sin(t) = 0 for every below-horizon direction -- so the
// shipped `t0 > 0` guard rejected the entire lower hemisphere and float32 rounding decided
// which directions composited a ground bounce at all.
//
// The fix handles the surface case analytically, and there the floor is DIRECTION-
// INDEPENDENT: the surface normal is the camera's own up and distToFloor is 0, which makes
// the aerial-perspective term exp(0) = 1. So every below-horizon direction must come back
// with the SAME colour, bit for bit.
//
// This test has to run on a real device to mean anything. The hazard it guards is the
// DRIVER's float32: SPIR-V permits 3 ULP in sqrt and one ULP at planetRadius 6.36e6 is
// ~0.5 m, so a conformant driver can land camR outside a too-tight surface band and
// silently re-enter the degenerate path. A source-text contract test cannot see that, and
// a CPU simulation of the arithmetic is not the arithmetic that ships.
//
// Skips ONLY without a Vulkan device. A missing staged probe shader FAILS instead: the
// same target builds it, so its absence is a build defect, and an all-skip suite that
// reads as green is the exact failure mode this area has been burned by.

#include <gtest/gtest.h>

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"

#include "Platform/Shell.h"

#include "TestDeviceHelper.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// Mirrors sky_ground_floor_probe.comp's ProbePC (8 vec4 == 128 bytes, the guaranteed
// minimum push-constant size).
struct ProbePC
{
    float camPosAndRadius[4];
    float centerAndBrightness[4];
    float sunDirAndIntensity[4];
    float albedoAndScaleHeight[4];
    float sunColorAndHaze[4];
    float betaAndPad[4];
    float skyColorAndCount[4];
    float meanRadiance[4];
};
static_assert(sizeof(ProbePC) == 128, "the probe push constant must fit the guaranteed 128 bytes");

// Earth-scale, matching SkyEnvironment's shipped default and the scale the degeneracy needs.
constexpr float kPlanetRadius = 6360000.0f;
constexpr uint32_t kDirections = 64u;

// Resolve the staged probe SPIR-V by walking up from the EXECUTABLE, never from the
// working directory. This target declares no WORKING_DIRECTORY, so a CWD-relative lookup
// silently misses whenever the binary is run from its own directory -- which is the fast
// loop CLAUDE.md documents -- and the suite then skips instead of running. Anchoring to
// the executable is the same rule the runtime itself follows.
std::filesystem::path FindProbeSpv()
{
    namespace fs = std::filesystem;
    const fs::path exe = GameEngine::Platform::GetExecutablePath();
    fs::path dir = exe.empty() ? fs::current_path() : exe.parent_path();
    for (int up = 0; up < 6; ++up)
    {
        const fs::path candidate = dir / "Shaders" / "sky_ground_floor_probe.comp.spv";
        std::error_code ec;
        if (fs::exists(candidate, ec))
            return candidate;
        if (!dir.has_parent_path() || dir.parent_path() == dir)
            break;
        dir = dir.parent_path();
    }
    return {};
}

class SkyGroundFloorParityTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        // A missing staged shader is a BUILD DEFECT, not an environment gap: the same
        // target that builds this test compiles and stages the probe. Failing here keeps a
        // silent all-skip from reading as a pass.
        const std::filesystem::path spvPath = FindProbeSpv();
        ASSERT_FALSE(spvPath.empty())
            << "sky_ground_floor_probe.comp.spv was not found above the test executable ("
            << GameEngine::Platform::GetExecutablePath().string()
            << ") -- the shader target stages it, so its absence is a build defect";
        const std::vector<uint8_t> spv = Utils::ReadFile(spvPath.string());
        ASSERT_FALSE(spv.empty()) << "staged probe SPIR-V is empty: " << spvPath.string();

        DescriptorBinding trans{};
        trans.binding = 0;
        trans.type = DescriptorType::CombinedImageSampler;
        trans.count = 1;
        trans.shaderStages = kShaderStageCompute;
        DescriptorBinding out{};
        out.binding = 1;
        out.type = DescriptorType::StorageBuffer;
        out.count = 1;
        out.shaderStages = kShaderStageCompute;
        m_Layout.debugName = "SkyGroundFloorProbe.Set0";
        m_Layout.bindings = {trans, out};

        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(spv);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        cd.PushConstants.Size = sizeof(ProbePC);
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = "SkyGroundFloorProbe";
        m_Pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(cd));
        ASSERT_TRUE(m_Pipeline.IsValid());

        m_Sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearClamp("SkyGroundFloorProbeSampler"));
        ASSERT_TRUE(m_Sampler.IsValid());
        m_TransLUT = MakeConstantLut(0.75f);
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

    // Stand-in transmittance LUT. Its content does not matter to the property under test --
    // in the analytic branch the lookup coordinate is direction-independent, so a constant
    // texture keeps the direct term constant too, which is what makes "all directions equal"
    // the right assertion rather than an accident of sampling.
    TextureHandle MakeConstantLut(float value)
    {
        constexpr uint32_t kW = 8u, kH = 2u;
        TextureDesc td{};
        td.width = kW;
        td.height = kH;
        td.depth = 1u;
        td.mipLevels = 1u;
        td.arrayLayers = 1u;
        td.sampleCount = 1u;
        td.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
                   static_cast<uint32_t>(TextureUsage::TransferDst);
        td.debugName = "SkyGroundFloorProbe.TransLUT";
        TextureHandle tex = m_Device->CreateTexture(td);
        m_Textures.push_back(tex);

        std::vector<float> texels(static_cast<size_t>(kW) * kH * 4u, value);
        BufferDesc ud{};
        ud.size = texels.size() * sizeof(float);
        ud.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
        ud.memoryUsage = BufferMemoryUsage::Upload;
        ud.debugName = "SkyGroundFloorProbe.LutUpload";
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

    // Run the probe with the camera `altitude` metres above the surface. altitude == 0 is
    // the capture's own geometry.
    std::vector<float> Probe(float altitude)
    {
        ProbePC pc{};
        // Camera on the +Y axis at camR; centre below it, so up == (0,1,0).
        pc.camPosAndRadius[0] = 0.0f;
        pc.camPosAndRadius[1] = altitude;
        pc.camPosAndRadius[2] = 0.0f;
        pc.camPosAndRadius[3] = kPlanetRadius;
        pc.centerAndBrightness[0] = 0.0f;
        pc.centerAndBrightness[1] = -kPlanetRadius;
        pc.centerAndBrightness[2] = 0.0f;
        pc.centerAndBrightness[3] = 0.9f;   // groundBrightness
        pc.sunDirAndIntensity[0] = 0.3f;
        pc.sunDirAndIntensity[1] = 0.8f;
        pc.sunDirAndIntensity[2] = 0.5f;
        pc.sunDirAndIntensity[3] = 4.0f;    // sunIntensity
        pc.albedoAndScaleHeight[0] = 0.30f;
        pc.albedoAndScaleHeight[1] = 0.24f;
        pc.albedoAndScaleHeight[2] = 0.15f; // shipped warm-earth midday albedo
        pc.albedoAndScaleHeight[3] = 8000.0f;
        pc.sunColorAndHaze[0] = 1.0f;
        pc.sunColorAndHaze[1] = 0.97f;
        pc.sunColorAndHaze[2] = 0.93f;
        pc.sunColorAndHaze[3] = 1.0f;       // groundHazeStrength
        pc.betaAndPad[0] = 5.8e-6f;
        pc.betaAndPad[1] = 13.5e-6f;
        pc.betaAndPad[2] = 33.1e-6f;
        pc.betaAndPad[3] = 0.0f; // camR is computed on the GPU, see the probe shader
        // A sky colour nothing else can produce, so "the floor did not composite" is
        // distinguishable from "the floor composited to something similar".
        pc.skyColorAndCount[0] = 11.0f;
        pc.skyColorAndCount[1] = 22.0f;
        pc.skyColorAndCount[2] = 33.0f;
        pc.skyColorAndCount[3] = static_cast<float>(kDirections);
        pc.meanRadiance[0] = 0.20f;
        pc.meanRadiance[1] = 0.35f;
        pc.meanRadiance[2] = 0.80f;

        const size_t bytes = static_cast<size_t>(kDirections) * 4u * sizeof(float);
        BufferDesc od{};
        od.size = bytes;
        od.usage = static_cast<uint32_t>(BufferUsage::Storage) |
                   static_cast<uint32_t>(BufferUsage::TransferSrc) |
                   static_cast<uint32_t>(BufferUsage::TransferDst);
        od.memoryUsage = BufferMemoryUsage::Readback;
        od.flags = BufferCreateFlags::PersistentlyMapped;
        od.debugName = "SkyGroundFloorProbe.Out";
        BufferHandle outBuf = m_Device->CreateBuffer(od);
        m_Buffers.push_back(outBuf);
        const std::vector<float> poison(static_cast<size_t>(kDirections) * 4u, -1.0f);
        m_Device->UpdateBuffer(outBuf, 0, bytes, poison.data());

        DescriptorSetDesc dsDesc{};
        dsDesc.layout = m_Layout;
        dsDesc.transient = true;
        dsDesc.debugName = "SkyGroundFloorProbe.DS";
        DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
        m_Device->UpdateCombinedImageSamplerBinding(ds, 0, m_TransLUT, m_Sampler);
        m_Device->UpdateStorageBufferBinding(ds, 1, outBuf, 0, bytes);

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Compute);
        cl->Begin();
        cl->SetPipeline(m_Pipeline);
        cl->BindDescriptorSet(0, ds, m_Pipeline);
        cl->SetPushConstants(pc);
        cl->Dispatch((kDirections + 63u) / 64u, 1u, 1u);
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();

        std::vector<float> out(static_cast<size_t>(kDirections) * 4u);
        void* mapped = m_Device->MapBuffer(outBuf);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
            std::memcpy(out.data(), mapped, bytes);
        m_Device->UnmapBuffer(outBuf);
        return out;
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    SamplerHandle m_Sampler{};
    TextureHandle m_TransLUT{};
    std::vector<TextureHandle> m_Textures;
    std::vector<BufferHandle> m_Buffers;
};

} // namespace

// THE anchor. On the surface the composited floor cannot depend on the view direction, so
// every below-horizon probe must return bit-identical bytes. Pre-fix this face was a
// patchwork decided by float32 rounding, which is what makes bit-equality (not a tolerance)
// the right bar: any tolerance would have accepted the defect.
TEST_F(SkyGroundFloorParityTest, AtSurfaceFloorIsIdenticalInEveryBelowHorizonDirection)
{
    const std::vector<float> got = Probe(0.0f);
    ASSERT_EQ(got.size(), static_cast<size_t>(kDirections) * 4u);

    const float r0 = got[0], g0 = got[1], b0 = got[2];
    EXPECT_NE(r0, -1.0f) << "the probe never wrote: the dispatch did not reach the buffer";

    // Not the sky: the floor actually composited, rather than every direction falling
    // through the miss path to the same input colour.
    EXPECT_FALSE(r0 == 11.0f && g0 == 22.0f && b0 == 33.0f)
        << "every direction returned the incoming sky colour — the floor never composited, "
           "which is the pre-fix behaviour this test exists to catch";

    size_t divergent = 0;
    for (uint32_t i = 1; i < kDirections; ++i)
    {
        const float r = got[i * 4u + 0], g = got[i * 4u + 1], b = got[i * 4u + 2];
        if (r != r0 || g != g0 || b != b0)
        {
            if (divergent < 4)
            {
                ADD_FAILURE() << "direction " << i << " (viewZenithCos " << got[i * 4u + 3]
                              << ") returned (" << r << ", " << g << ", " << b << ") but "
                              << "direction 0 returned (" << r0 << ", " << g0 << ", " << b0
                              << ") — on the surface the floor is direction-independent, so "
                                 "this is the degenerate intersection deciding output by "
                                 "rounding";
            }
            ++divergent;
        }
    }
    EXPECT_EQ(divergent, 0u) << divergent << " of " << (kDirections - 1)
                             << " directions diverged from the first";
}

// The relative surface tolerance must actually admit a camera that a driver's sqrt could
// place a few metres off the sphere. At Earth scale the band is planetRadius * 8e-7 ~ 5.1 m,
// so 3 m in must still take the analytic branch and match the exactly-on-surface result.
TEST_F(SkyGroundFloorParityTest, CameraWithinTheSurfaceToleranceTakesTheAnalyticBranch)
{
    const std::vector<float> onSurface = Probe(0.0f);
    const std::vector<float> justAbove = Probe(3.0f);
    ASSERT_EQ(onSurface.size(), justAbove.size());

    for (uint32_t i = 0; i < kDirections; ++i)
    {
        for (uint32_t c = 0; c < 3u; ++c)
        {
            ASSERT_EQ(justAbove[i * 4u + c], onSurface[i * 4u + c])
                << "a camera 3 m above the surface fell out of the tolerance band at "
                   "direction " << i << ", channel " << c
                << " — the band must cover the driver's float32 slack at planet scale";
        }
    }
}

// Well above the surface the intersection path is the correct one and the floor SHOULD vary
// with direction (the hit point moves, and the haze column with it). Without this, a
// mutation that took the analytic branch everywhere would pass the anchor above by making
// every direction trivially equal.
TEST_F(SkyGroundFloorParityTest, WellAboveTheSurfaceTheFloorVariesWithDirection)
{
    const std::vector<float> high = Probe(20000.0f);
    ASSERT_EQ(high.size(), static_cast<size_t>(kDirections) * 4u);

    size_t distinct = 0;
    for (uint32_t i = 1; i < kDirections; ++i)
    {
        if (high[i * 4u + 0] != high[0] || high[i * 4u + 1] != high[1] ||
            high[i * 4u + 2] != high[2])
            ++distinct;
    }
    EXPECT_GT(distinct, 0u)
        << "at 20 km every direction returned the same floor — the analytic branch is being "
           "taken above the surface, where the real intersection distance must vary";
}
