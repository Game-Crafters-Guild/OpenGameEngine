// Contract for the sun's veiling glare.
//
// The glare is a FRACTION of the sun's irradiance redistributed over the image by a kernel of
// unit integral. Two properties keep that honest, and each fails silently:
//
//   1. ENERGY. The kernel must integrate to exactly 1 over the sphere, so the composited term
//      delivers exactly `fraction * E * T` and no more. A kernel whose normalisation is off by
//      pi, by 2, or by the wrong measure still produces a perfectly plausible halo — nothing
//      about the picture says the sun is scattering ten times the light it has. The integral
//      is the only statement that catches it.
//   2. DISJOINTNESS FROM THE DISC. The halo must not re-draw the sun. The kernel's core is
//      regularised at the source's own angular radius, which bounds how peaked it can be:
//      scattered light carries no structure finer than the thing that scattered it.
//
// Both are properties of the shipped GLSL, so the kernel is evaluated on the device
// (sun_glare_probe.comp) and this file only sums what comes back. A CPU re-implementation of
// GE_SunGlarePsf and GE_SunGlareNormalisation would prove that a third copy agrees with
// itself, which is not the property.
//
// The occlusion cases run the shipped GE_SunGlareVisibility against a depth texture this file
// builds, because "the sun behind a ridge produces no glare" is the one behaviour a still
// frame of a clear sky can never show.
//
// The source-contract tests below need no device: they pin that the glare pass reaches the
// kernel through the shared include (so the probe measures what ships), that there is exactly
// one glare-fraction constant, and that the blueprint composites the pass before the tonemap.
//
// Skips ONLY without a Vulkan device. A missing staged probe FAILS: the same target builds
// it, so its absence is a build defect, and an all-skip suite that reads green is exactly the
// failure this area has been burned by.

#include <gtest/gtest.h>

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"

#include "Platform/Shell.h"

#include "TestDeviceHelper.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// Mirrors sun_glare_probe.comp's ProbePC.
struct GlareProbePC
{
    float kernelParams[4]; // angular radius, irradiance, sample count, mode
    float sweepParams[4];  // smallest polar angle swept, largest
    float visParams[4];    // probe uv, probe radius uv
    float cascadeVP[16];   // mode 3: the cascade's light view-projection, column-major
    float cascadeProbe[4]; // mode 3: xyz world position under test, w depth bias
};

// The sun's apparent angular radius, radians — the width SunGlareRenderNode gives the kernel's
// core. Half of the ~0.533 degree apparent diameter.
constexpr float kSunAngularRadius = 0.004651f;
// Irradiance of a 100 klx sun on the engine's unitless scale: 100000 / 203
// (LightPhotometry.h), times the sky's ground-to-TOA anchor of 1.33.
constexpr float kShippedSunIrradiance = 655.2f;
// The fraction sun_glare.glsl documents. Read back from the device rather than trusted, but
// the tests need a value to compare the readback against.
constexpr double kDocumentedGlareFraction = 0.01;
// Log-spaced sweep from a thousandth of the core width out to the antipode. The kernel spans
// six decades of angle; a uniform sweep would spend every sample in the tail.
constexpr uint32_t kSamples = 65536u;
constexpr float kSweepInnerFraction = 0.001f;

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open())
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Strip //-comments so prose naming a symbol never counts as a use of it.
std::string StripLineComments(const std::string& source)
{
    std::string out;
    out.reserve(source.size());
    std::size_t pos = 0;
    while (pos < source.size())
    {
        const std::size_t comment = source.find("//", pos);
        const std::size_t lineEnd = source.find('\n', pos);
        if (comment == std::string::npos || (lineEnd != std::string::npos && comment > lineEnd))
        {
            if (lineEnd == std::string::npos)
            {
                out.append(source, pos, std::string::npos);
                break;
            }
            out.append(source, pos, lineEnd + 1 - pos);
            pos = lineEnd + 1;
            continue;
        }
        out.append(source, pos, comment - pos);
        if (lineEnd == std::string::npos)
            break;
        out.push_back('\n');
        pos = lineEnd + 1;
    }
    return out;
}

// Reads a repo file via GE_RENDERER_REPO_ROOT, the same dev-only anchor
// SkyGroundBounceContractTests uses: the source is the artifact under test, and a staged copy
// only refreshes when its staging target rebuilds, which a shader-only edit does not.
std::string LoadRepoFileWithoutComments(const char* relativePath)
{
#ifndef GE_RENDERER_REPO_ROOT
    (void)relativePath;
    return {};
#else
    const std::filesystem::path path = std::filesystem::path(GE_RENDERER_REPO_ROOT) / relativePath;
    return StripLineComments(ReadTextFile(path));
#endif
}

std::size_t CountOccurrences(const std::string& haystack, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size()))
        ++count;
    return count;
}

// Resolve the staged probe SPIR-V by walking up from the EXECUTABLE, never from the working
// directory: this target declares no WORKING_DIRECTORY, so a CWD-relative lookup misses
// whenever the binary is run from its own directory and the suite skips instead of running.
std::filesystem::path FindProbeSpv()
{
    namespace fs = std::filesystem;
    const fs::path exe = GameEngine::Platform::GetExecutablePath();
    fs::path dir = exe.empty() ? fs::current_path() : exe.parent_path();
    for (int up = 0; up < 6; ++up)
    {
        const fs::path candidate = dir / "Shaders" / "sun_glare_probe.comp.spv";
        std::error_code ec;
        if (fs::exists(candidate, ec))
            return candidate;
        if (!dir.has_parent_path() || dir.parent_path() == dir)
            break;
        dir = dir.parent_path();
    }
    return {};
}

// What one kernel-sweep run returns.
struct GlareProbeResult
{
    // Closed-form normalisation the shader divides by.
    double Normalisation = 0.0;
    // Kernel at the sun's own centre, sr^-1.
    double PsfAtCentre = 0.0;
    // Composited scalar radiance at the centre, for irradiance E.
    double RadianceAtCentre = 0.0;
    // The glare fraction the include declares.
    double Fraction = 0.0;
    // The kernel integrated over the sphere's own measure, 2 pi sin(theta) d(theta), plus the
    // inner cap the log sweep cannot reach. Must be 1.
    double KernelIntegral = 0.0;
    // The composited radiance integrated the same way. Must be fraction * E.
    double RadianceIntegral = 0.0;
    // Kernel exactly one source-radius off centre, for the core-width contract.
    double PsfAtOneRadius = 0.0;
};

class SunGlareRadianceTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        const std::filesystem::path spvPath = FindProbeSpv();
        ASSERT_FALSE(spvPath.empty())
            << "sun_glare_probe.comp.spv was not found above the test executable ("
            << GameEngine::Platform::GetExecutablePath().string()
            << ") -- the shader target stages it, so its absence is a build defect";
        const std::vector<uint8_t> spv = Utils::ReadFile(spvPath.string());
        ASSERT_FALSE(spv.empty()) << "staged probe SPIR-V is empty: " << spvPath.string();

        DescriptorBinding out{};
        out.binding = 0;
        out.type = DescriptorType::StorageBuffer;
        out.count = 1;
        out.shaderStages = kShaderStageCompute;
        DescriptorBinding depth{};
        depth.binding = 1;
        depth.type = DescriptorType::CombinedImageSampler;
        depth.count = 1;
        depth.shaderStages = kShaderStageCompute;
        DescriptorBinding shadow{};
        shadow.binding = 2;
        shadow.type = DescriptorType::CombinedImageSampler;
        shadow.count = 1;
        shadow.shaderStages = kShaderStageCompute;
        m_Layout.debugName = "SunGlareProbe.Set0";
        m_Layout.bindings = {out, depth, shadow};

        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(spv);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        cd.PushConstants.Size = sizeof(GlareProbePC);
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = "SunGlareProbe";
        m_Pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(cd));
        ASSERT_TRUE(m_Pipeline.IsValid());

        m_Sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearClamp("SunGlareProbe.Smp"));
        ASSERT_TRUE(m_Sampler.IsValid());
        // The cascades are sampled through a COMPARISON sampler; sampling the same array with a
        // plain sampler would test a different function from the one that ships.
        m_ShadowSampler =
            m_Device->CreateSampler(SamplerDesc::ShadowComparePCF("SunGlareProbe.ShadowSmp"));
        ASSERT_TRUE(m_ShadowSampler.IsValid());
    }

    void TearDown() override
    {
        for (TextureHandle& t : m_Textures)
            if (t.IsValid())
                m_Device->DestroyTexture(t);
        for (BufferHandle& b : m_Buffers)
            if (b.IsValid())
                m_Device->DestroyBuffer(b);
        if (m_Device)
            m_Device->Shutdown();
    }

    // A stand-in scene-depth texture. Reverse-Z: 0 is the far plane (sky), anything greater is
    // drawn geometry. `occludedColumns` texels on the LEFT carry geometry.
    TextureHandle MakeDepth(uint32_t occludedColumns, bool occludeCentreTexelOnly = false)
    {
        constexpr uint32_t kW = 16u, kH = 16u;
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
        td.debugName = "SunGlareProbe.Depth";
        TextureHandle tex = m_Device->CreateTexture(td);
        m_Textures.push_back(tex);

        std::vector<float> texels(static_cast<size_t>(kW) * kH, 0.0f);
        if (occludeCentreTexelOnly)
        {
            texels[static_cast<size_t>(kH / 2u) * kW + (kW / 2u)] = 0.5f;
        }
        else
        {
            for (uint32_t y = 0; y < kH; ++y)
                for (uint32_t x = 0; x < occludedColumns && x < kW; ++x)
                    texels[static_cast<size_t>(y) * kW + x] = 0.5f;
        }

        BufferDesc ud{};
        ud.size = texels.size() * sizeof(float);
        ud.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
        ud.memoryUsage = BufferMemoryUsage::Upload;
        ud.debugName = "SunGlareProbe.DepthUpload";
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

    // A one-layer depth-comparison array carrying a single stored depth, so the cascade lookup
    // has something real to compare against. `storedDepth` is reverse-Z NDC: a LARGER value is a
    // nearer occluder, so 0.9 stands for something close in front of the eye and 0.0 for an
    // empty cascade cleared to the far plane -- which is what the island scene turned out to
    // have, and the reason this term needed a test of its own.
    // A one-layer depth-comparison array carrying a single stored depth, so the cascade lookup
    // has something real to compare against. `storedDepth` is reverse-Z NDC: a LARGER value is
    // a nearer occluder, so 0.9 stands for something close in front of the receiver and 0.0 for
    // an empty cascade cleared to the far plane -- which is what the island scene turned out to
    // have, and the reason this term needed a behavioural test at all.
    //
    // Written by CLEARING a depth render pass rather than by uploading a buffer:
    // CopyBufferToTextureSubresource hardcodes VK_IMAGE_ASPECT_COLOR_BIT
    // (VulkanCommandList.cpp), so a buffer copy into a depth image silently writes nothing --
    // which is exactly what the first version of this test did, and what its own
    // two-different-maps-same-answer check caught.
    TextureHandle MakeShadowArray(float storedDepth)
    {
        constexpr uint32_t kW = 4u, kH = 4u;
        TextureDesc td{};
        td.width = kW;
        td.height = kH;
        td.depth = 1u;
        td.mipLevels = 1u;
        td.arrayLayers = 1u;
        td.sampleCount = 1u;
        td.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
                   static_cast<uint32_t>(TextureUsage::DepthStencil);
        // A single layer declared ARRAY-shaped, so the shader can bind it as
        // sampler2DArrayShadow exactly as the cascades are bound.
        td.flags = TextureCreateFlags::ForceArrayView;
        td.debugName = "SunGlareProbe.ShadowArray";
        TextureHandle tex = m_Device->CreateTexture(td);
        m_Textures.push_back(tex);

        RenderPassDesc rp{};
        rp.depthTarget = tex;
        rp.colorTargetCount = 0u;
        rp.clearDepth = true;
        rp.clearDepthValue = storedDepth;

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->BeginRenderPass(rp);
        cl->EndRenderPass();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::DepthWrite,
                                                          ResourceState::ShaderResource));
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();
        return tex;
    }

    TextureHandle EnsureDummyShadowArray()
    {
        if (!m_DummyShadow.IsValid())
            m_DummyShadow = MakeShadowArray(0.0f);
        return m_DummyShadow;
    }

    // GE_SunGlareCascadeSunlit at `posWS`, through a cascade whose stored depth is
    // `storedDepth`. The light view-projection is the IDENTITY, so posWS.z IS the receiver's
    // NDC depth and posWS.xy lands directly in the cascade's [-1, 1] box.
    float ProbeCascade(float storedDepth, float posX, float posY, float posZ, float bias = 0.0f)
    {
        TextureHandle shadow = MakeShadowArray(storedDepth);
        TextureHandle dummyDepth = MakeDepth(0u);
        GlareProbePC pc{};
        pc.kernelParams[0] = kSunAngularRadius;
        pc.kernelParams[3] = 3.0f; // mode 3
        for (int i = 0; i < 16; ++i)
            pc.cascadeVP[i] = (i % 5 == 0) ? 1.0f : 0.0f; // identity, column-major
        pc.cascadeProbe[0] = posX;
        pc.cascadeProbe[1] = posY;
        pc.cascadeProbe[2] = posZ;
        pc.cascadeProbe[3] = bias;
        const std::vector<float> raw = Dispatch(pc, 1u, dummyDepth, shadow);
        return raw[0];
    }

    std::vector<float> Dispatch(const GlareProbePC& pc, uint32_t slots, TextureHandle depth,
                                TextureHandle shadowArray = TextureHandle{})
    {
        const size_t bytes = static_cast<size_t>(slots) * 4u * sizeof(float);
        BufferDesc od{};
        od.size = bytes;
        od.usage = static_cast<uint32_t>(BufferUsage::Storage) |
                   static_cast<uint32_t>(BufferUsage::TransferSrc) |
                   static_cast<uint32_t>(BufferUsage::TransferDst);
        od.memoryUsage = BufferMemoryUsage::Readback;
        od.flags = BufferCreateFlags::PersistentlyMapped;
        od.debugName = "SunGlareProbe.Out";
        BufferHandle outBuf = m_Device->CreateBuffer(od);
        m_Buffers.push_back(outBuf);
        // Poison: a dispatch that never lands is distinguishable from one that wrote zeros.
        const std::vector<float> poison(static_cast<size_t>(slots) * 4u, -1.0f);
        m_Device->UpdateBuffer(outBuf, 0, bytes, poison.data());

        DescriptorSetDesc dsDesc{};
        dsDesc.layout = m_Layout;
        dsDesc.transient = true;
        dsDesc.debugName = "SunGlareProbe.DS";
        DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
        m_Device->UpdateStorageBufferBinding(ds, 0, outBuf, 0, bytes);
        m_Device->UpdateCombinedImageSamplerBinding(ds, 1, depth, m_Sampler);
        // The set must be complete whether or not the mode under test uses the cascade array,
        // so an unused slot takes a real 1x1x1 array rather than a null handle.
        m_Device->UpdateCombinedImageSamplerBinding(
            ds, 2, shadowArray.IsValid() ? shadowArray : EnsureDummyShadowArray(),
            m_ShadowSampler);

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Compute);
        cl->Begin();
        cl->SetPipeline(m_Pipeline);
        cl->BindDescriptorSet(0, ds, m_Pipeline);
        cl->SetPushConstants(pc);
        cl->Dispatch((slots + 63u) / 64u, 1u, 1u);
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();

        std::vector<float> raw(static_cast<size_t>(slots) * 4u);
        void* mapped = m_Device->MapBuffer(outBuf);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
            std::memcpy(raw.data(), mapped, bytes);
        m_Device->UnmapBuffer(outBuf);
        return raw;
    }

    GlareProbeResult ProbeKernel(float irradiance, float angularRadius)
    {
        TextureHandle dummy = MakeDepth(0u); // unused in mode 0, but the layout wants a bind

        const float thetaMin = angularRadius * kSweepInnerFraction;
        GlareProbePC pc{};
        pc.kernelParams[0] = angularRadius;
        pc.kernelParams[1] = irradiance;
        pc.kernelParams[2] = static_cast<float>(kSamples);
        pc.kernelParams[3] = 0.0f; // kernel sweep
        pc.sweepParams[0] = thetaMin;
        pc.sweepParams[1] = static_cast<float>(std::numbers::pi);

        const uint32_t slots = kSamples + 1u; // the tail slot carries the scalars
        const std::vector<float> raw = Dispatch(pc, slots, dummy);

        GlareProbeResult r{};
        r.Normalisation = raw[kSamples * 4u + 0];
        r.PsfAtCentre = raw[kSamples * 4u + 1];
        r.RadianceAtCentre = raw[kSamples * 4u + 2];
        r.Fraction = raw[kSamples * 4u + 3];

        // The sphere's own measure, accumulated in double so the sum's rounding is nowhere
        // near the tolerance the assertions use.
        double kernel = 0.0;
        double radiance = 0.0;
        double nearestDelta = 1e30;
        for (uint32_t i = 0; i < kSamples; ++i)
        {
            const double theta = raw[i * 4u + 0];
            const double psf = raw[i * 4u + 1];
            const double dTheta = raw[i * 4u + 2];
            const double weight = 2.0 * std::numbers::pi * std::sin(theta) * dTheta;
            kernel += psf * weight;
            radiance += static_cast<double>(raw[i * 4u + 3]) * weight;
            const double delta = std::fabs(theta - angularRadius);
            if (delta < nearestDelta)
            {
                nearestDelta = delta;
                r.PsfAtOneRadius = psf;
            }
        }
        // The cap the log sweep starts above. The kernel is flat there (it is inside the
        // source), so its value at the centre times the cap's solid angle is exact to the
        // sweep's own precision. The cap's geometry is CPU arithmetic that knows nothing
        // about the kernel.
        const double capSolidAngle = 2.0 * std::numbers::pi * (1.0 - std::cos(thetaMin));
        r.KernelIntegral = kernel + r.PsfAtCentre * capSolidAngle;
        r.RadianceIntegral = radiance + r.RadianceAtCentre * capSolidAngle;
        return r;
    }

    float ProbeVisibility(TextureHandle depth, float u, float v, float radiusU, float radiusV)
    {
        GlareProbePC pc{};
        pc.kernelParams[0] = kSunAngularRadius;
        pc.kernelParams[1] = kShippedSunIrradiance;
        pc.kernelParams[2] = 1.0f;
        pc.kernelParams[3] = 1.0f; // screen visibility
        pc.visParams[0] = u;
        pc.visParams[1] = v;
        pc.visParams[2] = radiusU;
        pc.visParams[3] = radiusV;
        return Dispatch(pc, 1u, depth)[0];
    }

    // The UNNORMALISED kernel at two angles (degrees), evaluated on the device.
    std::pair<double, double> ProbeKernelAtTwoAngles(float degA, float degB)
    {
        TextureHandle dummy = MakeDepth(0u);
        GlareProbePC pc{};
        // A radius small enough that its floor is 1e-6 of theta^2 at the smaller angle, so
        // the samples are the bare power law the coefficients belong to.
        pc.kernelParams[0] = 1e-5f;
        pc.kernelParams[1] = kShippedSunIrradiance;
        pc.kernelParams[2] = 2.0f;
        pc.kernelParams[3] = 2.0f; // coefficients
        pc.sweepParams[0] = degA;
        pc.sweepParams[1] = degB;
        const std::vector<float> raw = Dispatch(pc, 2u, dummy);
        return {raw[1], raw[5]};
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    SamplerHandle m_Sampler{};
    SamplerHandle m_ShadowSampler{};
    TextureHandle m_DummyShadow{};
    std::vector<BufferHandle> m_Buffers;
    std::vector<TextureHandle> m_Textures;
};

} // namespace

// THE anchor. The kernel integrates to exactly one over the sphere, so the glare it spreads is
// exactly the fraction of the sun's irradiance the constant names — no more, no less.
TEST_F(SunGlareRadianceTest, GlareKernelIntegratesToUnityOverTheSphere)
{
    const GlareProbeResult r = ProbeKernel(kShippedSunIrradiance, kSunAngularRadius);

    ASSERT_GT(r.Normalisation, 0.0)
        << "the probe never wrote its scalar slot: the dispatch did not reach the buffer";
    EXPECT_NEAR(r.KernelIntegral, 1.0, 1e-3)
        << "the kernel integrates to " << r.KernelIntegral
        << " over the sphere — a normalisation that is not the integral of the kernel means the "
           "glare delivers that multiple of the energy it claims to";
}

// The composited radiance the fragment stage writes, integrated over the sphere, is exactly
// `fraction * E`. This is the statement the whole term exists to make.
TEST_F(SunGlareRadianceTest, GlareCarriesTheDocumentedFractionOfTheSunsIrradiance)
{
    const GlareProbeResult r = ProbeKernel(kShippedSunIrradiance, kSunAngularRadius);

    EXPECT_NEAR(r.Fraction, kDocumentedGlareFraction, 1e-9)
        << "the include's glare fraction is " << r.Fraction << ", not the documented "
        << kDocumentedGlareFraction;

    const double expected = kDocumentedGlareFraction * kShippedSunIrradiance;
    EXPECT_NEAR(r.RadianceIntegral, expected, expected * 1e-3)
        << "the glare delivers " << r.RadianceIntegral << " where " << expected
        << " was intended (ratio " << (r.RadianceIntegral / expected) << ")";
}

// The disc is the DIRECT term and the halo the SCATTERED one, and the halo must not sneak a
// second sun into the middle of the first. The kernel's core is regularised at the source's
// own angular radius, which bounds how peaked it can be: for a single 1/theta^n term the
// centre is exactly 2^(n/2) times the value one radius out, so a mixture of the 1/theta^3 and
// 1/theta^2 terms cannot exceed 2^1.5. A core that got narrower than the source would show
// here as a ratio above that bound and would put a spike on top of the drawn disc.
TEST_F(SunGlareRadianceTest, KernelCoreIsNoNarrowerThanTheSunItself)
{
    const GlareProbeResult r = ProbeKernel(kShippedSunIrradiance, kSunAngularRadius);

    ASSERT_GT(r.PsfAtOneRadius, 0.0);
    const double ratio = r.PsfAtCentre / r.PsfAtOneRadius;
    EXPECT_GT(ratio, 1.0);
    EXPECT_LE(ratio, std::pow(2.0, 1.5) + 1e-3)
        << "kernel centre / kernel at one sun-radius is " << ratio
        << ", above the 2^1.5 a 1/theta^3 term regularised at the source width can reach; the "
           "core is sharper than the source that made it";
}

// The coefficients are the published ones, extracted from the DEVICE rather than compared
// against a second copy of themselves. Two samples of the unnormalised kernel determine both
// terms of a / theta^3 + b / theta^2 exactly, and the pair must be the CIE general
// disability-glare equation's 10 and 5 (1 + (25/62.5)^4) = 5.128.
TEST_F(SunGlareRadianceTest, KernelCoefficientsAreThePublishedCieValues)
{
    // Sample angles chosen small: the kernel's argument is the squared CHORD, which is
    // theta^2 (1 - theta^2/12), so an extraction from widely separated samples also picks up
    // that compression. At 0.2 and 1.0 degrees it is under 0.02%, which the tolerances below
    // bound — so this test pins the coefficients AND the size of the chord substitution.
    // (At 0.5 and 5.0 degrees the same extraction returns 9.9972 and 5.1337, and a CPU model
    // of the chord form reproduces those to five digits: the residual is the substitution,
    // not the shipped coefficients.)
    constexpr double kA = 0.2;   // degrees
    constexpr double kB = 1.0;
    const auto [gA, gB] = ProbeKernelAtTwoAngles(static_cast<float>(kA), static_cast<float>(kB));
    ASSERT_GT(gA, 0.0);
    ASSERT_GT(gB, 0.0);

    const double a3 = gA * kA * kA * kA;
    const double b3 = gB * kB * kB * kB;
    const double wing = (a3 - b3) / (kA - kB);
    const double core = a3 - wing * kA;

    EXPECT_NEAR(core, 10.0, 5e-3)
        << "the 1/theta^3 coefficient the shipped kernel actually uses is " << core
        << ", not the CIE general equation's 10";
    EXPECT_NEAR(wing, 5.0 * (1.0 + std::pow(25.0 / 62.5, 4.0)), 5e-3)
        << "the 1/theta^2 coefficient is " << wing << ", not 5 (1 + (25/62.5)^4)";
}

// Draw the source wider and the same energy spreads further: the integral is invariant while
// the peak falls. The property that makes the term size-independent, the same way the disc's
// radiance is independent of its drawn radius.
TEST_F(SunGlareRadianceTest, DeliveredGlareIsIndependentOfTheSourceWidth)
{
    const GlareProbeResult narrow = ProbeKernel(kShippedSunIrradiance, kSunAngularRadius);
    const GlareProbeResult wide = ProbeKernel(kShippedSunIrradiance, kSunAngularRadius * 4.0f);

    EXPECT_NEAR(wide.KernelIntegral, 1.0, 1e-3);
    EXPECT_LT(wide.PsfAtCentre, narrow.PsfAtCentre)
        << "a wider source produced a peakier kernel";
    const double expected = kDocumentedGlareFraction * kShippedSunIrradiance;
    EXPECT_NEAR(wide.RadianceIntegral, expected, expected * 1e-3);
}

// The sun behind geometry produces no glare at all: an occluder blocks the beam before it
// enters the optics, so it takes the whole halo with it, not just the pixels behind itself.
TEST_F(SunGlareRadianceTest, SunBehindGeometryProducesNoGlare)
{
    TextureHandle fullyOccluded = MakeDepth(16u);
    EXPECT_FLOAT_EQ(ProbeVisibility(fullyOccluded, 0.5f, 0.5f, 0.25f, 0.25f), 0.0f)
        << "every probe tap is behind drawn geometry and the sun still reads visible";
}

TEST_F(SunGlareRadianceTest, SunAgainstClearSkyIsFullyVisible)
{
    TextureHandle clear = MakeDepth(0u);
    EXPECT_FLOAT_EQ(ProbeVisibility(clear, 0.5f, 0.5f, 0.25f, 0.25f), 1.0f)
        << "reverse-Z sky depth is 0.0; an all-sky probe must read fully visible";
}

// A silhouette edge crossing the sun fades the halo instead of switching it off between two
// frames. Asserted as a strict interval rather than an exact fraction so the tap layout stays
// free to change.
TEST_F(SunGlareRadianceTest, PartialOcclusionFadesTheGlare)
{
    TextureHandle halfCovered = MakeDepth(8u);
    const float half = ProbeVisibility(halfCovered, 0.5f, 0.5f, 0.25f, 0.25f);
    EXPECT_GT(half, 0.0f);
    EXPECT_LT(half, 1.0f) << "a probe straddling a silhouette edge read a hard 0 or 1";

    TextureHandle oneTexel = MakeDepth(0u, /*occludeCentreTexelOnly*/ true);
    const float centre = ProbeVisibility(oneTexel, 0.5f, 0.5f, 0.25f, 0.25f);
    EXPECT_LT(centre, 1.0f)
        << "a single occluding texel at the sun's own position did not dim the halo — the "
           "centre tap is not being read";
    EXPECT_GT(centre, 0.5f) << "one occluded texel took out more than half the probe";
}

// The cascade term, on the device. Until now it had NO behavioural test -- only shader text --
// and the scene it was supposed to work in turned out to have an empty shadow map, so a lookup
// that always returned "lit" would have looked identical to a correct one.
//
// One pose, two stored depths. The light view-projection is the identity, so the eye at the
// origin lands at the map's centre with NDC depth 0.5; the comparison is GreaterOrEqual on
// reverse-Z, i.e. "lit if the receiver is at least as near as what was stored".
TEST_F(SunGlareRadianceTest, CascadeLookupReportsShadowedWhenSomethingIsStoredInFront)
{
    // Receiver at NDC depth 0.5, between the two stored values, so ONE pose reaches both
    // verdicts by changing only what is in the map.
    const float occluded = ProbeCascade(0.9f, 0.0f, 0.0f, 0.5f);
    const float clear = ProbeCascade(0.1f, 0.0f, 0.0f, 0.5f);

    // INSTRUMENT FIRST. If the depth upload does not land, every stored value reads 0.0 and the
    // comparison returns lit for any in-range receiver — which is indistinguishable from a
    // lookup that never gates. Two different maps giving the same answer means the fixture is
    // broken, not that the shader is right.
    ASSERT_NE(occluded, clear)
        << "the synthetic cascade reads the same through two different stored depths — the "
           "depth upload is not landing, so this test would pass a lookup that never gates";

    EXPECT_FLOAT_EQ(occluded, 0.0f)
        << "a cascade carrying an occluder in front of the receiver reported sunlit — the glare "
           "would keep its halo behind a shadow-casting wall";
    EXPECT_FLOAT_EQ(clear, 1.0f)
        << "a cascade whose stored depth is behind the receiver reported shadowed — the glare "
           "would be gated by empty space";

    // An EMPTY cascade, cleared to the reverse-Z far plane. This is the state the island scene
    // is actually in, and it must read LIT: a term that gated on an empty map would take the
    // halo out everywhere.
    EXPECT_FLOAT_EQ(ProbeCascade(0.0f, 0.0f, 0.0f, 0.5f), 1.0f)
        << "an empty (far-cleared) cascade reported shadowed";
}

// Outside its own map a cascade knows nothing, and must say LIT rather than 0 — otherwise the
// minimum over cascades would be zero wherever the innermost cascade does not reach.
TEST_F(SunGlareRadianceTest, CascadeLookupOutsideItsMapReportsLit)
{
    // With the identity VP, |x| > 1 is outside the cascade's NDC box.
    EXPECT_FLOAT_EQ(ProbeCascade(0.9f, 5.0f, 0.0f, 0.5f), 1.0f)
        << "a position outside the cascade's footprint was treated as shadowed";
}

// The bias is ADDED to the receiver's depth, which under reverse-Z pushes it TOWARD the light.
// A bias large enough to clear the stored occluder must turn a shadowed sample lit; the sign
// being wrong here is the classic reverse-Z mistake and would show up as a halo that never
// gates.
TEST_F(SunGlareRadianceTest, CascadeBiasPushesTheReceiverTowardTheLight)
{
    ASSERT_FLOAT_EQ(ProbeCascade(0.9f, 0.0f, 0.0f, 0.5f), 0.0f);
    EXPECT_FLOAT_EQ(ProbeCascade(0.9f, 0.0f, 0.0f, 0.5f, /*bias*/ 0.45f), 1.0f)
        << "adding the bias did not move the receiver toward the light — the sign is inverted "
           "for reverse-Z";
}

// ---------------------------------------------------------------------------
// Source contracts. No device needed; these pin what the probe above measures.
// ---------------------------------------------------------------------------

TEST(SunGlareSourceContract, GlarePassReachesTheKernelThroughTheSharedInclude)
{
    const std::string frag =
        LoadRepoFileWithoutComments("Engine/Modules/Rendering/Shaders/sun_glare.frag");
    const std::string vert =
        LoadRepoFileWithoutComments("Engine/Modules/Rendering/Shaders/sun_glare.vert");
    if (frag.empty() || vert.empty())
        GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined; shader source unavailable";

    EXPECT_NE(frag.find("#include \"Includes/sun_glare.glsl\""), std::string::npos)
        << "sun_glare.frag no longer includes the shared kernel; the device probe would be "
           "measuring a different function from the one that ships";

    // The vertex stage reaches the kernel through the shared vertex body, which is what both
    // depth-source variants include. Two hops, still one owner.
    EXPECT_NE(vert.find("#include \"Includes/sun_glare_vs.glsl\""), std::string::npos);
    const std::string vsBody =
        LoadRepoFileWithoutComments("Engine/Modules/Rendering/Shaders/Includes/sun_glare_vs.glsl");
    ASSERT_FALSE(vsBody.empty());
    EXPECT_NE(vsBody.find("#include \"Includes/sun_glare.glsl\""), std::string::npos);

    // The kernel must not be re-declared locally: one definition, one owner.
    EXPECT_EQ(CountOccurrences(frag, "float GE_SunGlarePsf"), 0u);
    EXPECT_EQ(CountOccurrences(vert, "float GE_SunGlarePsf"), 0u);
    EXPECT_EQ(CountOccurrences(vsBody, "float GE_SunGlarePsf"), 0u);
}

// The two depth-source variants must differ ONLY in the depth uniform's type and the probe
// they call. Anything else drifting into one and not the other is a second implementation of
// the vertex stage, which is what the shared body exists to prevent.
TEST(SunGlareSourceContract, TheMultisampledVariantIsAThinWrapperOverTheSharedVertexBody)
{
    const std::string single =
        LoadRepoFileWithoutComments("Engine/Modules/Rendering/Shaders/sun_glare.vert");
    const std::string multi =
        LoadRepoFileWithoutComments("Engine/Modules/Rendering/Shaders/sun_glare_ms.vert");
    if (single.empty() || multi.empty())
        GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined; shader source unavailable";

    EXPECT_NE(single.find("uniform sampler2D uSceneDepth"), std::string::npos);
    EXPECT_NE(multi.find("uniform sampler2DMS uSceneDepth"), std::string::npos)
        << "the multisampled variant no longer declares a multisampled depth uniform; binding "
           "a multisampled image to a sampler2D is undefined";
    for (const std::string* v : {&single, &multi})
    {
        EXPECT_NE(v->find("#include \"Includes/sun_glare_vs.glsl\""), std::string::npos);
        EXPECT_NE(v->find("#define GE_SUN_GLARE_PROBE("), std::string::npos);
        // No second copy of the stage: no main, no push-constant block, no varyings.
        EXPECT_EQ(CountOccurrences(*v, "void main"), 0u);
        EXPECT_EQ(CountOccurrences(*v, "push_constant"), 0u);
    }
}

TEST(SunGlareSourceContract, ThereIsExactlyOneGlareFractionConstant)
{
    const std::string include =
        LoadRepoFileWithoutComments("Engine/Modules/Rendering/Shaders/Includes/sun_glare.glsl");
    const std::string frag =
        LoadRepoFileWithoutComments("Engine/Modules/Rendering/Shaders/sun_glare.frag");
    if (include.empty() || frag.empty())
        GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined; shader source unavailable";

    EXPECT_EQ(CountOccurrences(include, "#define GE_SUN_GLARE_FRACTION"), 1u)
        << "the glare fraction is the term's ONE dial; a second definition is a second dial";
    EXPECT_EQ(CountOccurrences(frag, "#define GE_SUN_GLARE_FRACTION"), 0u);

    // The storage clamp is what keeps an Inf out of the fp16 target and a NaN out of bloom.
    EXPECT_NE(frag.find("GE_SUN_GLARE_MAX_STORED_RADIANCE"), std::string::npos)
        << "the glare pass no longer bounds what it writes to the fp16 target";
}

TEST(SunGlareSourceContract, VisibilityIsTheProductOfBothIndependentTerms)
{
    const std::string vsBody =
        LoadRepoFileWithoutComments("Engine/Modules/Rendering/Shaders/Includes/sun_glare_vs.glsl");
    const std::string single =
        LoadRepoFileWithoutComments("Engine/Modules/Rendering/Shaders/sun_glare.vert");
    const std::string multi =
        LoadRepoFileWithoutComments("Engine/Modules/Rendering/Shaders/sun_glare_ms.vert");
    if (vsBody.empty() || single.empty() || multi.empty())
        GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined; shader source unavailable";

    // No term alone can see every occluder in this engine: the cascades cannot see CBT terrain
    // (it casts no shadows), the screen probe cannot see anything while the sun is off screen,
    // and the CPU skyline march sees only terrain. The product is the fix; dropping any one of
    // them silently reopens a hole, and the off-frame one was a visible one-frame POP.
    EXPECT_NE(vsBody.find("GE_SunGlareCascadeSunlit"), std::string::npos)
        << "the cascade term is gone: an off-frame or behind-camera sun would glare through "
           "any occluder";
    EXPECT_NE(vsBody.find("GE_SUN_GLARE_PROBE("), std::string::npos)
        << "the screen probe is gone: CBT terrain casts no shadows, so the sun would glare "
           "through every hill";
    EXPECT_NE(vsBody.find("sunlit * onScreen * pc.horizon.x"), std::string::npos)
        << "visibility is no longer the product of all three terms";
    EXPECT_NE(vsBody.find("pc.horizon.x"), std::string::npos)
        << "the terrain-skyline term is gone: a sun just outside the frame edge would be "
           "unoccludable, and the halo would step the frame its centre crosses in";

    // BOTH variants must reach a probe; a variant that quietly returns 1.0 would leave the
    // terrain hole open on exactly the MSAA settings nobody tests by hand.
    EXPECT_NE(single.find("GE_SunGlareScreenVisibility(uSceneDepth"), std::string::npos);
    EXPECT_NE(multi.find("GE_SunGlareScreenVisibilityMS(uSceneDepth"), std::string::npos);
}

// The multisampled probe AVERAGES sky-ness over a texel's samples. That choice is load-bearing
// rather than stylistic: the probe estimates a coverage fraction, so a min over samples ("is
// any sample sky") would count a texel with one sky sample as fully visible and bias the halo
// bright at every silhouette edge, making MSAA-on leakier than MSAA-off.
TEST(SunGlareSourceContract, TheMultisampledProbeAveragesOverSamplesRatherThanPickingOne)
{
    const std::string include =
        LoadRepoFileWithoutComments("Engine/Modules/Rendering/Shaders/Includes/sun_glare.glsl");
    if (include.empty())
        GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined; shader source unavailable";

    const std::size_t ms = include.find("GE_SunGlareScreenVisibilityMS");
    ASSERT_NE(ms, std::string::npos);
    const std::string body = include.substr(ms);
    EXPECT_NE(body.find("for (int s = 0; s < samples; ++s)"), std::string::npos)
        << "the multisampled probe no longer walks the samples";
    EXPECT_NE(body.find("sky / float(samples)"), std::string::npos)
        << "the multisampled probe no longer averages over samples; a min or a single sample "
           "biases the halo at silhouette edges";
    // Both variants must derive their taps from the SAME geometry helper.
    EXPECT_EQ(CountOccurrences(include, "GE_SunGlareProbeTap(i, size"), 2u)
        << "the two probe variants no longer share their tap geometry";
}

TEST(SunGlareSourceContract, ShadowBlockPrefixMatchesTheShadowSamplingHeader)
{
    const std::string vert =
        LoadRepoFileWithoutComments("Engine/Modules/Rendering/Shaders/Includes/sun_glare_vs.glsl");
    const std::string header =
        LoadRepoFileWithoutComments("Engine/Modules/Rendering/Shaders/Includes/shadow_sampling.glsl");
    if (vert.empty() || header.empty())
        GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined; shader source unavailable";

    // The glare declares a std140 PREFIX of the cascades' ShadowData block rather than
    // including the whole shadow header, which would drag in the PCSS, MSM and bindless
    // surface it has no use for. std140 makes a prefix binding-compatible, but only while the
    // fields it declares are the same fields in the same order — so pin exactly that.
    for (const char* field : {"mat4 ge_shadowVP[4];", "vec4 ge_shadowSplits;",
                              "vec4 ge_shadowParams;"})
    {
        EXPECT_NE(vert.find(field), std::string::npos)
            << "the glare's shadow block no longer declares " << field;
        EXPECT_NE(header.find(field), std::string::npos)
            << "shadow_sampling.glsl no longer declares " << field
            << " — the glare's prefix has drifted from the block it mirrors";
    }
    // Order matters as much as membership.
    EXPECT_LT(vert.find("ge_shadowVP"), vert.find("ge_shadowSplits"));
    EXPECT_LT(vert.find("ge_shadowSplits"), vert.find("ge_shadowParams"));
    EXPECT_LT(header.find("ge_shadowVP"), header.find("ge_shadowSplits"));
    EXPECT_LT(header.find("ge_shadowSplits"), header.find("ge_shadowParams"));
}

// The visibility source: the scene depth ATTACHMENT, read where it already contains the
// terrain, and no resolve of its own.
//
// CBT is excluded from the depth prepass and writes the shared scene depth in the World pass,
// so a probe reading the PREPASS resolve (View.DepthResolved) sees no terrain at all — that
// was this PR's first revision, and on the island the sun glared straight through every hill.
// What makes the attachment hold the terrain by the time the glare reads it is the PHASE: the
// pass declares at kPostProcess, after every depth writer, and the graph derives the ordering
// from the read-after-write edge. So the contract is on the phase and the binding, not on any
// blueprint entry.
TEST(SunGlareSourceContract, TheProbeReadsThePostWorldDepthAttachmentWithNoResolveOfItsOwn)
{
    const std::string node = LoadRepoFileWithoutComments(
        "Engine/Source/Engine/Rendering/Pipeline/Nodes/SunGlareRenderNode.cpp");
    const std::string graph =
        LoadRepoFileWithoutComments("Assets/RenderPipelines/ForwardPlus.rendergraph");
    if (node.empty() || graph.empty())
        GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined; source unavailable";

    EXPECT_NE(node.find("depth = d.ViewDepth"), std::string::npos)
        << "the glare no longer binds the scene depth attachment";
    EXPECT_EQ(CountOccurrences(node, "DepthResolvedWorld"), 0u)
        << "the glare is back on a depth resolve of its own; the whole point of reading the "
           "attachment is that sixteen texels do not justify a pass";
    EXPECT_EQ(CountOccurrences(node, "ViewDepthResolved"), 0u)
        << "the glare reads the PREPASS resolve, which contains no CBT terrain";
    EXPECT_NE(node.find("PassPhase::kPostProcess"), std::string::npos)
        << "the glare no longer declares in the post-process phase, so nothing guarantees the "
           "depth it samples has been through the world pass";

    // The deleted resolve stays deleted: a blueprint carrying it again would be a pass with
    // no reader.
    EXPECT_EQ(CountOccurrences(graph, "DepthResolvedWorld"), 0u)
        << "the blueprint declares a post-world depth resolve again, but nothing reads it";

    // Two pipelines, because the attachment is multisampled whenever MSAA is on.
    const std::string feature =
        LoadRepoFileWithoutComments("Engine/Source/Engine/Rendering/SunGlareRenderFeature.cpp");
    ASSERT_FALSE(feature.empty());
    EXPECT_NE(feature.find("Shaders/sun_glare_ms.shaderpkg"), std::string::npos)
        << "the multisampled variant is not loaded, so an MSAA view would bind a multisampled "
           "image to a sampler2D";
    EXPECT_NE(node.find("GetPipelineId(depthSamples)"), std::string::npos)
        << "the variant is no longer selected from the attachment's own sample count";
}

// The skyline term is a CPU march against the terrain heightfield, and the two properties that
// make it the right fix are both in the node: it does not consult the sun's screen position, and
// it is faded over the SUN'S OWN angular radius rather than a chosen width.
TEST(SunGlareSourceContract, TheSkylineTermIsIndependentOfWhereTheCameraPoints)
{
    const std::string node = LoadRepoFileWithoutComments(
        "Engine/Source/Engine/Rendering/Pipeline/Nodes/SunGlareRenderNode.cpp");
    if (node.empty())
        GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined; source unavailable";

    EXPECT_NE(node.find("GetSkylineTangent(d.View.id)"), std::string::npos)
        << "the node no longer reads the terrain skyline, so a sun behind a hill and off frame "
           "is unoccludable";
    // The march itself is on the terrain side, because the renderer cannot depend on TerrainECS
    // (TerrainECS owns a render feature, so the dependency runs the other way). Pin that the
    // publisher exists and marches along the SUN's azimuth rather than the camera's.
    const std::string publisher = LoadRepoFileWithoutComments(
        "Engine/Modules/TerrainECS/Source/Systems/TerrainExtractionSystem.cpp");
    ASSERT_FALSE(publisher.empty());
    EXPECT_NE(publisher.find("TerrainHorizonTangent("), std::string::npos)
        << "nothing marches the terrain skyline any more";
    EXPECT_NE(publisher.find("settings.scatteringSunDir[0], settings.scatteringSunDir[2]"),
              std::string::npos)
        << "the skyline is no longer marched along the SUN's azimuth";
    // Gated on the terrain being resolvable, NOT on the sun being on screen: the whole point is
    // that this term survives the sun leaving the frame.
    EXPECT_EQ(CountOccurrences(node, "onScreen ? horizonVisibility"), 0u)
        << "the skyline term is gated on the sun being on screen, which is the hole it exists "
           "to close";
    EXPECT_EQ(CountOccurrences(node, "onScreen && horizonVisibility"), 0u);
    // The fade width is the sun's own angular radius, so it is geometry rather than a dial.
    EXPECT_NE(node.find("2.0f * radiusTangent"), std::string::npos)
        << "the skyline fade is no longer spanned by the sun's angular diameter";
    // Four uses: the kernel core width, the probe ring scale on each axis, and the skyline fade
    // width. A fifth occurrence is probably a new constant in disguise.
    EXPECT_EQ(CountOccurrences(node, "kSunAngularRadiusRad"), 4u);
    // The sun's angular radius is named once, in SkySettings.h, and the glare and the sky read
    // the same name. A local definition here is the duplication coming back.
    EXPECT_EQ(CountOccurrences(node, "kSunAngularRadiusRad ="), 0u)
        << "the glare node redefines the sun's angular radius instead of using the shared one";
}

// The probe ring must not reach far past the disc. At three sun radii the taps on the occluded
// side left the sum while the disc was still fully hidden, which put a 0.24-luma blob on a
// hillside with no sun in it.
TEST(SunGlareSourceContract, TheProbeRingStaysCloseToTheDisc)
{
    const std::string node = LoadRepoFileWithoutComments(
        "Engine/Source/Engine/Rendering/Pipeline/Nodes/SunGlareRenderNode.cpp");
    if (node.empty())
        GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined; source unavailable";

    const std::size_t at = node.find("kProbeRadiusInSunRadii = ");
    ASSERT_NE(at, std::string::npos);
    const double radii = std::atof(node.c_str() + at + std::strlen("kProbeRadiusInSunRadii = "));
    EXPECT_GE(radii, 1.0) << "a ring inside the disc cannot see an edge crossing it";
    EXPECT_LE(radii, 1.6)
        << "the ring reaches too far past the disc: its taps clear before the disc does, which "
           "reads as a bright blob sitting on an occluder";
}

TEST(SunGlareSourceContract, BlueprintCompositesTheGlareBeforeTonemap)
{
    const std::string graph =
        LoadRepoFileWithoutComments("Assets/RenderPipelines/ForwardPlus.rendergraph");
    if (graph.empty())
        GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined; blueprint unavailable";

    const std::size_t glare = graph.find("\"type\": \"SunGlare\"");
    ASSERT_NE(glare, std::string::npos)
        << "the shipped ForwardPlus blueprint declares no SunGlare pass, so the term never runs";

    const std::size_t tonemap = graph.find("Shaders/tonemap.shaderpkg");
    ASSERT_NE(tonemap, std::string::npos);
    EXPECT_LT(glare, tonemap)
        << "the glare composites after the tonemap — it is scene-linear radiance and must land "
           "before the display transform";

    // Exactly one instance: two would double the sun's scattered energy.
    EXPECT_EQ(CountOccurrences(graph, "\"type\": \"SunGlare\""), 1u);
}
