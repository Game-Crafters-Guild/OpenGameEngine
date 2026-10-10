// Contract for the visible sun disc's radiance.
//
// The disc is drawn at the sun's apparent angular radius and given the radiance that makes
// the drawn profile carry the sun's irradiance: L = E / omega_drawn. Three properties keep
// that honest, and each has a way of failing silently:
//
//   1. ENERGY. The profile scaled by that peak must integrate over the sphere back to E.
//      A constant in front of the peak (the form this replaced used 0.2, times a 1.0-1.5
//      elevation "contrast boost") still produces a plausible sun, so nothing about the
//      picture says the disc is delivering 5e4 times too little energy for its size. The
//      integral is the only statement that catches it.
//   2. STORAGE. E / omega is ~9.5e6 on the engine's unitless scale, and SceneColor is RGBA16F,
//      which tops out at 65504. Storing the physical value writes +Inf, and Inf * 0 in
//      bloom's Karis weight is NaN. The clamp is a storage bound, so it must bound the value
//      that is actually STORED — after the sky's exposure multiply, not before it.
//   3. SHAPE. Energy says nothing about where the profile puts its brightness. Any profile
//      normalised by its own solid angle satisfies 1, including one that puts half its peak
//      inside 40% of the sun's radius. The limb is pinned on its own: flat face, half
//      brightness at exactly the angle the sun subtends, dark past a narrow symmetric limb.
//      This is a statement about the PROFILE, not about pixels. The drawn disc is one to two
//      orders over the tonemapper's white point, so a frame shows this shape thresholded by
//      the exposure: the visible edge sits out on the limb's OUTSIDE, not at the half-max,
//      and how big the sun looks is that edge and the glare, not this number.
//
// All three are properties of the shipped GLSL, so the profile is evaluated on the device
// (sun_disc_probe.comp) and this file only sums what comes back. A CPU re-implementation of
// GE_SunDiscProfile and GE_SunDiscDrawnSolidAngle would prove that the pair agrees with a
// third copy, which is not the property.
//
// The source-contract tests below need no device: they pin that the sky pass reaches the disc
// through the shared include (so the probe measures what ships) and that the visible dome
// applies no near-sun radiance multiplier the IBL capture lacks.
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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// Mirrors sun_disc_probe.comp's ProbePC.
struct ProbePC
{
    float discParams[4];  // irradiance, drawn angular radius, exposure scale, sample count
    float sweepParams[4]; // max theta swept, unused
};

// The sun's apparent angular radius from Earth, radians — the value SkyRenderNode uploads.
constexpr float kSunAngularRadius = 0.004651f;
// Above-atmosphere irradiance of a 100 klx sun on the engine's unitless scale: 100000 / 203
// (LightPhotometry.h), times the sky's ground-to-TOA anchor of 1.33.
constexpr float kShippedSunIrradiance = 655.2f;
// The profile is exactly zero past the outside of the limb, at (1 + GE_SUN_DISC_LIMB_FRACTION)
// R = 1.25 R, so a 2 R sweep captures all of it and leaves margin to see that it stays zero.
// Sweeping further buys nothing and costs limb resolution: at 2 R the samples below resolve
// the limb to one two-thousandth of the radius.
constexpr float kSweepRadii = 2.0f;
constexpr uint32_t kSamples = 4096u;
// RGBA16F's largest finite value. The stored disc must stay under it with room to spare.
constexpr float kFp16Max = 65504.0f;

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

// Reads the repo shader source via GE_RENDERER_REPO_ROOT, the same dev-only anchor
// SkyGroundBounceContractTests uses: the source file is the artifact under test, and a staged
// copy only refreshes when its staging target rebuilds, which a shader-only edit does not.
std::string LoadShaderWithoutComments(const char* relativePath)
{
#ifndef GE_RENDERER_REPO_ROOT
    (void)relativePath;
    return {};
#else
    const std::filesystem::path path =
        std::filesystem::path(GE_RENDERER_REPO_ROOT) / "Engine/Modules/Rendering/Shaders" / relativePath;
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
        const fs::path candidate = dir / "Shaders" / "sun_disc_probe.comp.spv";
        std::error_code ec;
        if (fs::exists(candidate, ec))
            return candidate;
        if (!dir.has_parent_path() || dir.parent_path() == dir)
            break;
        dir = dir.parent_path();
    }
    return {};
}

// One point of the swept profile, as the device evaluated it.
struct ProfileSample
{
    double Theta = 0.0;
    double Value = 0.0;
};

// What one probe run returns.
struct DiscProbeResult
{
    // The swept profile itself, in increasing theta.
    std::vector<ProfileSample> Samples;
    // Closed-form solid angle the shader reports for the drawn profile, sr.
    double SolidAngleClosedForm = 0.0;
    // Numerically integrated solid angle of the SAME profile, over the sphere's own measure
    // (2 pi sin(theta) d(theta)) rather than the small-angle form the closed form assumes.
    double SolidAngleIntegrated = 0.0;
    // Peak radiance the sky pass would use, after the storage clamp.
    double PeakClamped = 0.0;
    // Peak radiance before the clamp: the physical E / omega.
    double PeakPhysical = 0.0;
    // The storage bound the include declares.
    double MaxStoredRadiance = 0.0;
};

class SunDiscRadianceTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        const std::filesystem::path spvPath = FindProbeSpv();
        ASSERT_FALSE(spvPath.empty())
            << "sun_disc_probe.comp.spv was not found above the test executable ("
            << GameEngine::Platform::GetExecutablePath().string()
            << ") -- the shader target stages it, so its absence is a build defect";
        const std::vector<uint8_t> spv = Utils::ReadFile(spvPath.string());
        ASSERT_FALSE(spv.empty()) << "staged probe SPIR-V is empty: " << spvPath.string();

        DescriptorBinding out{};
        out.binding = 0;
        out.type = DescriptorType::StorageBuffer;
        out.count = 1;
        out.shaderStages = kShaderStageCompute;
        m_Layout.debugName = "SunDiscProbe.Set0";
        m_Layout.bindings = {out};

        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(spv);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        cd.PushConstants.Size = sizeof(ProbePC);
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = "SunDiscProbe";
        m_Pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(cd));
        ASSERT_TRUE(m_Pipeline.IsValid());
    }

    void TearDown() override
    {
        for (BufferHandle& b : m_Buffers)
            if (b.IsValid())
                m_Device->DestroyBuffer(b);
        if (m_Device)
            m_Device->Shutdown();
    }

    DiscProbeResult Probe(float irradiance, float angularRadius, float exposureScale)
    {
        ProbePC pc{};
        pc.discParams[0] = irradiance;
        pc.discParams[1] = angularRadius;
        pc.discParams[2] = exposureScale;
        pc.discParams[3] = static_cast<float>(kSamples);
        pc.sweepParams[0] = angularRadius * kSweepRadii;

        const uint32_t slots = kSamples + 1u; // the tail slot carries the scalars
        const size_t bytes = static_cast<size_t>(slots) * 4u * sizeof(float);
        BufferDesc od{};
        od.size = bytes;
        od.usage = static_cast<uint32_t>(BufferUsage::Storage) |
                   static_cast<uint32_t>(BufferUsage::TransferSrc) |
                   static_cast<uint32_t>(BufferUsage::TransferDst);
        od.memoryUsage = BufferMemoryUsage::Readback;
        od.flags = BufferCreateFlags::PersistentlyMapped;
        od.debugName = "SunDiscProbe.Out";
        BufferHandle outBuf = m_Device->CreateBuffer(od);
        m_Buffers.push_back(outBuf);
        // Poison: a dispatch that never lands is distinguishable from one that wrote zeros.
        const std::vector<float> poison(static_cast<size_t>(slots) * 4u, -1.0f);
        m_Device->UpdateBuffer(outBuf, 0, bytes, poison.data());

        DescriptorSetDesc dsDesc{};
        dsDesc.layout = m_Layout;
        dsDesc.transient = true;
        dsDesc.debugName = "SunDiscProbe.DS";
        DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
        m_Device->UpdateStorageBufferBinding(ds, 0, outBuf, 0, bytes);

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

        DiscProbeResult r{};
        r.SolidAngleClosedForm = raw[kSamples * 4u + 0];
        r.PeakClamped = raw[kSamples * 4u + 1];
        r.PeakPhysical = raw[kSamples * 4u + 2];
        r.MaxStoredRadiance = raw[kSamples * 4u + 3];

        // Solid angle of the drawn profile on the sphere's own measure. Accumulated in double
        // so the sum's rounding is nowhere near the tolerance the assertions use.
        double integral = 0.0;
        r.Samples.reserve(kSamples);
        for (uint32_t i = 0; i < kSamples; ++i)
        {
            const double theta = raw[i * 4u + 0];
            const double profile = raw[i * 4u + 1];
            const double dTheta = raw[i * 4u + 2];
            integral += profile * 2.0 * std::numbers::pi * std::sin(theta) * dTheta;
            r.Samples.push_back({theta, profile});
        }
        r.SolidAngleIntegrated = integral;
        return r;
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    std::vector<BufferHandle> m_Buffers;
};

} // namespace

// THE anchor. The drawn profile, scaled by the peak radiance the sky pass gives it, integrates
// over the sphere to the sun's irradiance. Run with an irradiance low enough that the storage
// clamp does not bind, so this measures the derivation and nothing else.
TEST_F(SunDiscRadianceTest, DrawnDiscCarriesTheSunsIrradiance)
{
    // E / omega at this radius is ~3.6e3, comfortably inside the storage bound.
    constexpr float kIrradiance = 0.25f;
    const DiscProbeResult r = Probe(kIrradiance, kSunAngularRadius, 1.0f);

    ASSERT_GT(r.SolidAngleClosedForm, 0.0)
        << "the probe never wrote its scalar slot: the dispatch did not reach the buffer";
    ASSERT_LT(r.PeakClamped, r.MaxStoredRadiance)
        << "the storage clamp bound at an irradiance chosen to stay under it — this test would "
           "be measuring the clamp instead of the derivation";

    const double delivered = r.PeakClamped * r.SolidAngleIntegrated;
    EXPECT_NEAR(delivered, kIrradiance, kIrradiance * 1e-3)
        << "the disc delivers " << delivered << " where the sun's irradiance is " << kIrradiance
        << " (ratio " << (delivered / kIrradiance)
        << ") — the drawn profile is not normalised by the solid angle it covers";
}

// SHAPE. The disc is limb-sharp: flat across its face, through half brightness at exactly the
// angle the sun subtends, and dark past a narrow symmetric limb.
//
// Nothing else in this file catches a wrong shape. Energy conservation holds for ANY profile
// normalised by its own solid angle, so one whose half-max sits well inside the sun's radius
// passes every other test here and still draws a sun visibly smaller than the one the
// atmosphere model is built around.
//
// The limb is read out of the swept profile rather than mirrored from the shader: the constant
// belongs to GE_SUN_DISC_LIMB_FRACTION, and a second copy of it here would only agree with
// itself. The bounds below are what "limb-sharp" means, wide enough that retuning that
// constant within reason does not trip them.
TEST_F(SunDiscRadianceTest, ProfileIsFlatAcrossTheDiscAndHalfBrightAtTheSunsRadius)
{
    const DiscProbeResult r = Probe(0.25f, kSunAngularRadius, 1.0f);
    ASSERT_GT(r.Samples.size(), 1u) << "the probe returned no swept profile";

    constexpr double kRadius = kSunAngularRadius;
    // The sweep's step: the resolution every angle read out below is measured to.
    constexpr double kStep = kRadius * kSweepRadii / kSamples;
    // Symmetric on both ends, so the two limb halves are measured the same way and their
    // widths can be compared. 1e-5 of full brightness is ~1.8e-3 of the limb's half-width in.
    constexpr double kProfileEps = 1e-5;

    double flatFaceEnd = 0.0; // last theta still at full brightness
    double darkStart = -1.0;  // first theta at (effectively) zero
    double halfMax = -1.0;    // theta where the profile crosses half brightness
    double previous = 1.0;
    for (std::size_t i = 0; i < r.Samples.size(); ++i)
    {
        const double theta = r.Samples[i].Theta;
        const double value = r.Samples[i].Value;

        ASSERT_LE(value, previous + kProfileEps)
            << "the profile brightens again at theta = " << (theta / kRadius)
            << " R — the disc is not monotone outward from its centre";

        if (value >= 1.0 - kProfileEps)
            flatFaceEnd = theta;
        if (darkStart < 0.0 && value <= kProfileEps)
            darkStart = theta;
        if (halfMax < 0.0 && i > 0 && value < 0.5)
        {
            // The profile's inflection is at half brightness, so interpolating linearly across
            // the bracketing pair is accurate well past the step this is asserted to.
            const double t0 = r.Samples[i - 1].Theta;
            halfMax = t0 + (previous - 0.5) * (theta - t0) / (previous - value);
        }
        previous = value;
    }

    ASSERT_GE(darkStart, 0.0)
        << "the profile never reaches zero inside " << kSweepRadii
        << " R — the disc has a tail rather than a limb, and the drawn size is whatever the "
           "exposure happens to make visible";
    ASSERT_GE(halfMax, 0.0) << "the profile never crosses half brightness";

    // The half-max centres the profile on the angle the sun actually subtends, which is what
    // separates a disc drawn AT that angle from one that merely keeps its energy inside it. It
    // is NOT the disc's apparent size on screen: the peak is far over the tonemapper's white
    // point, so white runs out to wherever the profile falls under it, near the limb's outside
    // at R (1 + GE_SUN_DISC_LIMB_FRACTION). Apparent size follows from this profile AND the
    // exposure; only the profile belongs to the shader, so only the profile is asserted here.
    EXPECT_NEAR(halfMax, kRadius, kRadius * 1e-3)
        << "the profile's half-max is at " << (halfMax / kRadius)
        << " R instead of on the drawn radius";

    // Flat face: no falloff at all until the limb.
    EXPECT_GT(flatFaceEnd, kRadius * 0.5)
        << "the profile is already dimming at " << (flatFaceEnd / kRadius)
        << " R — it has no flat face, only a peak and a fade";

    const double inner = kRadius - flatFaceEnd;
    const double outer = darkStart - kRadius;
    EXPECT_NEAR(outer, inner, 2.0 * kStep)
        << "the limb is lopsided: " << (inner / kRadius) << " R inside the sun's radius against "
        << (outer / kRadius)
        << " R outside — a symmetric limb is what puts the half-max on the radius";
    EXPECT_LT(inner + outer, kRadius)
        << "the limb spans " << ((inner + outer) / kRadius)
        << " R, wider than the disc it edges — that is a soft ball, not a limb";
}

// The closed form the shader divides by must BE the profile's solid angle. It uses the
// small-angle measure (2 pi theta d(theta)); the integral above uses the sphere's
// (2 pi sin(theta) d(theta)). At the sun's radius those agree to 1.9e-6 relative — R^2/12, the
// leading term of the difference — and this pins that they do: a closed form off by pi, by 2,
// or by the limb's contribution to the integral shows up here as a clean ratio rather than as
// a slightly-wrong picture.
TEST_F(SunDiscRadianceTest, ClosedFormSolidAngleMatchesTheIntegratedProfile)
{
    const DiscProbeResult r = Probe(0.25f, kSunAngularRadius, 1.0f);
    ASSERT_GT(r.SolidAngleIntegrated, 0.0);
    EXPECT_NEAR(r.SolidAngleClosedForm, r.SolidAngleIntegrated, r.SolidAngleIntegrated * 1e-3)
        << "closed form " << r.SolidAngleClosedForm << " sr vs integrated "
        << r.SolidAngleIntegrated << " sr (ratio "
        << (r.SolidAngleClosedForm / r.SolidAngleIntegrated) << ")";
}

// Energy is conserved across the drawn radius: doubling the radius quarters the peak, so the
// disc delivers the same irradiance whatever size it is drawn. This is what makes the drawn
// radius a size decision and not a brightness one.
TEST_F(SunDiscRadianceTest, DeliveredIrradianceIsIndependentOfTheDrawnRadius)
{
    constexpr float kIrradiance = 0.25f;
    const DiscProbeResult nominal = Probe(kIrradiance, kSunAngularRadius, 1.0f);
    const DiscProbeResult wide = Probe(kIrradiance, kSunAngularRadius * 4.0f, 1.0f);

    ASSERT_LT(wide.PeakClamped, wide.MaxStoredRadiance);
    const double deliveredNominal = nominal.PeakClamped * nominal.SolidAngleIntegrated;
    const double deliveredWide = wide.PeakClamped * wide.SolidAngleIntegrated;

    EXPECT_NEAR(deliveredWide, deliveredNominal, deliveredNominal * 1e-3)
        << "a 4x wider disc delivered " << deliveredWide << " against " << deliveredNominal
        << " — drawn radius is leaking into the energy the disc carries";
    // And the peak really did fall: without this, a fixed peak would satisfy the line above
    // only by the integral compensating, which it cannot, but the direction is worth pinning.
    EXPECT_LT(wide.PeakClamped, nominal.PeakClamped * 0.1)
        << "16x the solid angle must cost ~16x the peak radiance";
}

// The shipped sun overflows the target by over two orders of magnitude, so the clamp is load
// bearing rather than defensive: without it SceneColor stores +Inf and bloom's Karis weight
// turns it into NaN.
TEST_F(SunDiscRadianceTest, ShippedSunIsClampedToTheStorageBound)
{
    const DiscProbeResult r = Probe(kShippedSunIrradiance, kSunAngularRadius, 1.0f);

    EXPECT_GT(r.PeakPhysical, kFp16Max)
        << "the shipped sun's physical disc radiance (" << r.PeakPhysical
        << ") no longer exceeds the RGBA16F maximum — if that is intended the clamp is dead "
           "code, and if it is not, the sun's irradiance has changed";
    EXPECT_FLOAT_EQ(static_cast<float>(r.PeakClamped), static_cast<float>(r.MaxStoredRadiance))
        << "the clamp did not bind on the shipped sun";
    EXPECT_LT(r.PeakClamped, kFp16Max)
        << "the clamped peak still does not fit RGBA16F";
}

// The clamp bounds what is STORED, so it has to move with the sky's exposure multiply. A
// clamp applied before that multiply lets a +2 EV sky trim put the disc back over the format.
TEST_F(SunDiscRadianceTest, StorageClampFollowsTheSkyExposureMultiply)
{
    // The authoring limit on SkyEnvironment::SkyExposureTrim is +/-2 EV, so 4x is the largest
    // multiply the sky pass can apply to the disc on its way into SceneColor.
    constexpr float kMaxTrimScale = 4.0f;
    const DiscProbeResult r = Probe(kShippedSunIrradiance, kSunAngularRadius, kMaxTrimScale);

    const double stored = r.PeakClamped * kMaxTrimScale;
    EXPECT_LT(stored, kFp16Max)
        << "at the maximum sky exposure trim the disc stores " << stored
        << ", over the RGBA16F maximum — the clamp is being applied before the exposure "
           "multiply instead of after it";
    EXPECT_NEAR(stored, r.MaxStoredRadiance, r.MaxStoredRadiance * 1e-4)
        << "the clamp should bind exactly at the storage bound once exposure is folded in";
}

// A hidden sun uploads sunAngularRadius = 0 (SkyRenderNode: showSunDisk ? radius : 0), and the
// sky pass evaluates the peak radiance unconditionally -- outside the `sunRadius > 0` block
// that gates the profile. An unfloored solid angle is exactly zero there, so the peak is a
// division by zero: +Inf, and NaN when the irradiance is zero too. Today only min() rescues
// the first case and nothing rescues the second, and either one reaching SceneColor is a NaN
// the whole frame carries.
//
// This pins the arithmetic, which is what makes the dome claim a proof rather than a
// measurement: the shader multiplies the peak by a sunDisk that is initialised to 0.0 and only
// written inside the radius guard, so a FINITE peak times that zero is exactly zero and the
// dome is byte-identical with the sun hidden. An Inf or NaN peak is the only way that fails.
TEST_F(SunDiscRadianceTest, HiddenSunRadiusStaysFinite)
{
    const DiscProbeResult r = Probe(kShippedSunIrradiance, 0.0f, 1.0f);

    EXPECT_GT(r.SolidAngleClosedForm, 0.0)
        << "a zero drawn radius produced a zero solid angle — the sun's irradiance is about to "
           "be divided by it";
    EXPECT_TRUE(std::isfinite(r.PeakClamped))
        << "hiding the sun produced a non-finite peak radiance (" << r.PeakClamped
        << "); multiplied by the profile's zero that is a NaN in SceneColor";
    EXPECT_FLOAT_EQ(static_cast<float>(r.PeakClamped), static_cast<float>(r.MaxStoredRadiance))
        << "the storage clamp should still bind at a zero radius";
    EXPECT_TRUE(std::isfinite(r.SolidAngleIntegrated))
        << "the profile itself went non-finite at a zero radius";
}

// The other half of the same hazard: with the sun hidden AND no irradiance the unfloored form
// is 0/0, which min() does not rescue -- GLSL leaves min(NaN, x) undefined.
TEST_F(SunDiscRadianceTest, HiddenSunWithNoIrradianceStaysFinite)
{
    const DiscProbeResult r = Probe(0.0f, 0.0f, 1.0f);

    EXPECT_TRUE(std::isfinite(r.PeakPhysical))
        << "zero irradiance over a zero radius produced " << r.PeakPhysical << " — 0/0";
    EXPECT_TRUE(std::isfinite(r.PeakClamped))
        << "hidden sun with no irradiance produced a non-finite clamped peak";
    EXPECT_DOUBLE_EQ(r.PeakClamped, 0.0)
        << "no irradiance must draw nothing, not the storage bound";
}

// The sky pass must reach the disc through the shared include, or the probe above measures a
// function nothing draws with.
TEST(SunDiscSourceContract, SkyPassDerivesTheDiscRadianceFromTheSharedInclude)
{
    const std::string sky = LoadShaderWithoutComments("sky_render.frag");
    ASSERT_FALSE(sky.empty()) << "sky_render.frag source was not readable (GE_RENDERER_REPO_ROOT)";

    EXPECT_NE(sky.find("#include \"Includes/sun_disc.glsl\""), std::string::npos)
        << "sky_render.frag no longer includes the shared sun-disc derivation";
    EXPECT_EQ(CountOccurrences(sky, "GE_SunDiscPeakRadiance("), 1u)
        << "the disc's radiance must come from GE_SunDiscPeakRadiance exactly once";
    EXPECT_EQ(CountOccurrences(sky, "GE_SunDiscProfile("), 1u)
        << "the drawn profile must come from GE_SunDiscProfile exactly once";
}

// The disc's brightness follows from its solid angle and nothing else. The form this replaced
// multiplied the sun's intensity by 0.2 and by a 1.0-1.5 elevation ramp, and grew the drawn
// radius 2x-20x with elevation on top; each of those is a scale factor with no physical
// referent, and each looked fine.
TEST(SunDiscSourceContract, NoElevationRampOrConstantScaleSurvivesOnTheDisc)
{
    const std::string sky = LoadShaderWithoutComments("sky_render.frag");
    ASSERT_FALSE(sky.empty());

    EXPECT_EQ(sky.find("kSunDiskIntensityScale"), std::string::npos)
        << "a constant scale is back in front of the sun disc's radiance";
    EXPECT_EQ(sky.find("diskContrastBoost"), std::string::npos)
        << "an elevation contrast boost is back on the sun disc";
    EXPECT_EQ(sky.find("lowSun"), std::string::npos)
        << "the low-sun ramp is back; the atmosphere's optical depth is what dims and reddens "
           "the disc at sunset";
}

// S5: the visible dome and the dome the IBL bakes must be the same radiance field. The form
// this replaced multiplied the visible sky by mix(1, 0.35, muSun^3) — up to a 65% cut within
// ~30 degrees of the sun — which the capture did not apply, so surfaces were lit by an
// aureole the camera was not allowed to see.
TEST(SunDiscSourceContract, VisibleDomeAppliesNoNearSunMultiplierTheCaptureLacks)
{
    const std::string sky = LoadShaderWithoutComments("sky_render.frag");
    const std::string capture = LoadShaderWithoutComments("sky_capture_cube.frag");
    ASSERT_FALSE(sky.empty());
    ASSERT_FALSE(capture.empty());

    EXPECT_EQ(CountOccurrences(sky, "skyColor *="), 0u)
        << "the visible sky scales the composited dome by something the IBL capture does not — "
           "the camera and the lighting then disagree about how bright the sky near the sun is";
    EXPECT_EQ(CountOccurrences(capture, "skyColor *="), 0u)
        << "the IBL capture scales the composited dome, which the visible sky does not";
}
