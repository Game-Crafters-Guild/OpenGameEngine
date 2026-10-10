// GPU-executed gate for the transmittance LUT's ray-sphere path lengths.
//
// The LUT bakes exp(-tau) along two path lengths: out to the top of the atmosphere, and -- for
// downward rays -- down to the ground. Both solves are exact-cancellation-prone: at planet scale
// r*r and sphereRadius*sphereRadius are ~4e13, so the difference of the two squares keeps no
// float32 precision, and a compiler free to contract one product into an FMA leaves a residual of
// ~1e6 m^2 where the exact answer is 0. That is not a hypothetical: it is what made the CPU sun
// colour resolve a 222-2165 km chord as a 0.25-2.97 m near root on arm64, so a set sun was never
// extinguished.
//
// The shader now factors both terms as (a - b) * (a + b) and takes each root in its non-cancelling
// direction. This test checks that on a REAL device, because the compiler that decides whether to
// contract is the driver's -- MoltenVK hands SPIR-V to Metal, which recompiles it. A CPU
// simulation of the arithmetic is not the arithmetic that ships.
//
// The LUT's own texels cannot serve as the instrument: it stores rgba16f, and near the surface a
// 5% path-length error moves a transmittance of ~1 by less than one fp16 ULP. So the probe reports
// the path lengths and this compares them against the REPLACED quadratic evaluated in double,
// which is the contract the change claims: identical wherever that form was not asked to cancel.
//
// Skips ONLY without a Vulkan device. A missing staged probe shader FAILS instead: the same target
// builds it, so its absence is a build defect, and an all-skip suite that reads as green is the
// exact failure mode this area has been burned by.

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
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// Mirrors sky_transmittance_path_probe.comp's ProbePC.
struct ProbePC
{
    float radiiAndCount[4];
};

// SkyRenderer::FillDefaultAtmosphere's Earth-scale shell, which is the scale the cancellation
// needs: at R = 6.36e6 the exact difference of the squares is what a contracted FMA destroys.
constexpr double kPlanetRadius = 6360000.0;
constexpr double kAtmosphereRadius = 6460000.0;

// One invocation per LUT column, so the sweep is the shader's own pixel-centre mu sequence.
constexpr uint32_t kSamples = 256u;

// The LUT's row radii, from DecodeUvToMuR: rNorm = t*t with t = (row + 0.5) / 64. Row 0 is the
// one the defect lives closest to -- 6 m of shell above the surface, where the ground root is
// metres long and a cancelling solve rounds it into nonsense.
//
// The float round-trip only picks a radius near the row's; the value the shader solves at is the
// one the probe is HANDED, and every reference below is evaluated at the radius the probe reports
// back (result.w), so this quantisation cannot reach the comparison. The shader's own fp32 mix()
// would land elsewhere again -- 6.5 m for row 0 without contraction, against 6.0 m here -- which is
// exactly why the reference reads the radius back instead of recomputing it.
double RowRadius(uint32_t row, uint32_t rows = 64u)
{
    const double t = (static_cast<double>(row) + 0.5) / static_cast<double>(rows);
    return static_cast<double>(
        static_cast<float>(kPlanetRadius + t * t * (kAtmosphereRadius - kPlanetRadius)));
}

// The reference is the REPLACED general quadratic, evaluated in double: nearest positive root of
// t^2 + 2*r*mu*t + (r^2 - sphereRadius^2), zero when there is none.
//
// Two things follow from that choice, and both are the point. It is independent of the shader's new
// expression -- a reference written in the new form would replicate an algebraic branch error along
// with it -- and it is literally the contract this change claims: the closed forms must agree with
// the quadratic everywhere the quadratic was correct, and the quadratic is correct exactly when it
// is not asked to cancel. In double it never is: r^2 is ~4e13 with an ULP of ~0.008, so the
// difference of the squares keeps every metre the fp32 version loses.
double ReferenceNearestPositiveRoot(double r, double mu, double sphereRadius)
{
    const double b = 2.0 * r * mu;
    const double c = (r - sphereRadius) * (r + sphereRadius);
    const double disc = b * b - 4.0 * c;
    if (disc <= 0.0)
        return 0.0;
    const double sq = std::sqrt(disc);
    const double t0 = 0.5 * (-b - sq);
    const double t1 = 0.5 * (-b + sq);
    if (t0 > 0.0)
        return t0;
    return (t1 > 0.0) ? t1 : 0.0;
}

std::filesystem::path FindProbeSpv()
{
    namespace fs = std::filesystem;
    const fs::path exe = GameEngine::Platform::GetExecutablePath();
    fs::path dir = exe.empty() ? fs::current_path() : exe.parent_path();
    for (int up = 0; up < 6; ++up)
    {
        const fs::path candidate = dir / "Shaders" / "sky_transmittance_path_probe.comp.spv";
        std::error_code ec;
        if (fs::exists(candidate, ec))
            return candidate;
        if (!dir.has_parent_path() || dir.parent_path() == dir)
            break;
        dir = dir.parent_path();
    }
    return {};
}

class SkyTransmittanceLutParityTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        const std::filesystem::path spvPath = FindProbeSpv();
        ASSERT_FALSE(spvPath.empty())
            << "sky_transmittance_path_probe.comp.spv was not found above the test executable ("
            << GameEngine::Platform::GetExecutablePath().string()
            << ") -- the shader target stages it, so its absence is a build defect";
        const std::vector<uint8_t> spv = Utils::ReadFile(spvPath.string());
        ASSERT_FALSE(spv.empty()) << "staged probe SPIR-V is empty: " << spvPath.string();

        DescriptorBinding out{};
        out.binding = 0;
        out.type = DescriptorType::StorageBuffer;
        out.count = 1;
        out.shaderStages = kShaderStageCompute;
        m_Layout.debugName = "SkyTransmittancePathProbe.Set0";
        m_Layout.bindings = {out};

        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(spv);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        cd.PushConstants.Size = sizeof(ProbePC);
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = "SkyTransmittancePathProbe";
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

    // Returns kSamples x (sAtmosphere, sGround, mu, r) at the given LUT row radius.
    std::vector<float> Probe(double rowRadius)
    {
        ProbePC pc{};
        pc.radiiAndCount[0] = static_cast<float>(kPlanetRadius);
        pc.radiiAndCount[1] = static_cast<float>(kAtmosphereRadius);
        pc.radiiAndCount[2] = static_cast<float>(rowRadius);
        pc.radiiAndCount[3] = static_cast<float>(kSamples);

        const size_t bytes = static_cast<size_t>(kSamples) * 4u * sizeof(float);
        BufferDesc od{};
        od.size = bytes;
        od.usage = static_cast<uint32_t>(BufferUsage::Storage) |
                   static_cast<uint32_t>(BufferUsage::TransferSrc) |
                   static_cast<uint32_t>(BufferUsage::TransferDst);
        od.memoryUsage = BufferMemoryUsage::Readback;
        od.flags = BufferCreateFlags::PersistentlyMapped;
        od.debugName = "SkyTransmittancePathProbe.Out";
        BufferHandle outBuf = m_Device->CreateBuffer(od);
        m_Buffers.push_back(outBuf);
        const std::vector<float> poison(static_cast<size_t>(kSamples) * 4u, -1.0f);
        m_Device->UpdateBuffer(outBuf, 0, bytes, poison.data());

        DescriptorSetDesc dsDesc{};
        dsDesc.layout = m_Layout;
        dsDesc.transient = true;
        dsDesc.debugName = "SkyTransmittancePathProbe.DS";
        DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
        m_Device->UpdateStorageBufferBinding(ds, 0, outBuf, 0, bytes);

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Compute);
        cl->Begin();
        cl->SetPipeline(m_Pipeline);
        cl->BindDescriptorSet(0, ds, m_Pipeline);
        cl->SetPushConstants(pc);
        cl->Dispatch((kSamples + 63u) / 64u, 1u, 1u);
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();

        std::vector<float> out(static_cast<size_t>(kSamples) * 4u);
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
    std::vector<BufferHandle> m_Buffers;
};

// float32 relative bound, one for both roots. The bound has to be tight enough that the SHELL exit
// separates too: the replaced quadratic misses the ground root by 5e-2 but the shell exit by only
// 1.2e-4, so a looser bound would leave the shell-exit case with no red arm at all. At 1e-4 the
// replaced form is red on all three cases and the closed forms clear it by 60x on the ground root
// and 400x on the shell exit -- decided by that measured gap, not tuned.
constexpr double kRelativeTolerance = 1e-4;

} // namespace

// THE anchor: the ground root near the surface. This is the exact geometry the CPU evaluator's
// contraction bug destroyed, and the rows the sky actually samples from a camera standing on the
// planet. A metres-long root is the defect's signature, so every row is checked against the
// double-precision answer rather than against a mirror of the shader's own new expression.
TEST_F(SkyTransmittanceLutParityTest, GroundRootIsExactOnEveryLutRow)
{
    size_t checked = 0;
    double worstRel = 0.0;
    for (uint32_t row = 0; row < 64u; ++row)
    {
        const double r = RowRadius(row);
        const std::vector<float> got = Probe(r);
        ASSERT_EQ(got.size(), static_cast<size_t>(kSamples) * 4u);
        ASSERT_NE(got[0], -1.0f) << "the probe never wrote: the dispatch did not reach the buffer";

        for (uint32_t i = 0; i < kSamples; ++i)
        {
            const double mu = static_cast<double>(got[i * 4u + 2]);
            const double expected =
                ReferenceNearestPositiveRoot(static_cast<double>(got[i * 4u + 3]), mu, kPlanetRadius);
            const double actual = static_cast<double>(got[i * 4u + 1]);
            if (expected <= 0.0)
            {
                EXPECT_EQ(actual, 0.0) << "row " << row << " mu " << mu
                                       << ": the ray misses the planet, so there is no root";
                continue;
            }
            ++checked;
            const double rel = std::fabs(actual - expected) / expected;
            worstRel = std::max(worstRel, rel);
            if (rel > kRelativeTolerance)
            {
                ADD_FAILURE() << "row " << row << " (r - R = " << (r - kPlanetRadius) << " m) mu "
                              << mu << ": ground root " << actual << " m, exact " << expected
                              << " m, relative error " << rel
                              << " -- a cancelling ray-sphere solve is back in the LUT";
                row = 64u; // one row's worth of detail is enough to diagnose
                break;
            }
        }
    }
    EXPECT_GT(checked, 0u) << "no downward ray hit the planet: the sweep never exercised the root";
    RecordProperty("worstGroundRootRelativeError", std::to_string(worstRel));
}

// The other half of the same solve: the distance out through the shell, which every row uses and
// which is the LUT's whole upward path. Its cancelling direction is the OPPOSITE one -- an outward
// ray subtracts near-equal terms -- so a form that fixed only the ground root fails here, but only
// against a bound tight enough to see it: the replaced quadratic's worst shell error is 1.2e-4,
// three orders of magnitude smaller than the 5e-2 it misses the ground root by. That gap is what
// kRelativeTolerance is set from.
TEST_F(SkyTransmittanceLutParityTest, AtmosphereExitIsExactOnEveryLutRow)
{
    double worstRel = 0.0;
    for (uint32_t row = 0; row < 64u; ++row)
    {
        const double r = RowRadius(row);
        const std::vector<float> got = Probe(r);
        ASSERT_EQ(got.size(), static_cast<size_t>(kSamples) * 4u);

        for (uint32_t i = 0; i < kSamples; ++i)
        {
            const double mu = static_cast<double>(got[i * 4u + 2]);
            const double expected =
                ReferenceNearestPositiveRoot(static_cast<double>(got[i * 4u + 3]), mu, kAtmosphereRadius);
            const double actual = static_cast<double>(got[i * 4u + 0]);
            ASSERT_GT(expected, 0.0) << "row " << row << " mu " << mu
                                     << ": every LUT row is inside the shell, so the ray must exit";
            const double rel = std::fabs(actual - expected) / expected;
            worstRel = std::max(worstRel, rel);
            if (rel > kRelativeTolerance)
            {
                ADD_FAILURE() << "row " << row << " (r - R = " << (r - kPlanetRadius) << " m) mu "
                              << mu << ": shell exit " << actual << " m, exact " << expected
                              << " m, relative error " << rel;
                row = 64u;
                break;
            }
        }
    }
    RecordProperty("worstShellExitRelativeError", std::to_string(worstRel));
}

// On the surface exactly, both roots collapse and the forward one is the chord -2*R*mu. That is
// the case the CPU evaluator resolves in closed form, and the shape a contracted solve turns into
// a near root of a few metres -- the failure that left a set sun lit.
TEST_F(SkyTransmittanceLutParityTest, OnTheSurfaceTheGroundRootIsTheChord)
{
    const std::vector<float> got = Probe(kPlanetRadius);
    ASSERT_EQ(got.size(), static_cast<size_t>(kSamples) * 4u);

    for (uint32_t i = 0; i < kSamples; ++i)
    {
        const double mu = static_cast<double>(got[i * 4u + 2]);
        if (mu >= 0.0)
            continue;
        const double expected = -2.0 * kPlanetRadius * mu;
        const double actual = static_cast<double>(got[i * 4u + 1]);
        const double rel = std::fabs(actual - expected) / expected;
        ASSERT_LE(rel, kRelativeTolerance)
            << "mu " << mu << ": on-surface ground root " << actual << " m, chord " << expected
            << " m -- the metres-long near root is exactly the contraction failure";
    }
}
