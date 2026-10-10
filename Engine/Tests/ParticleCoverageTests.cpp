// A particle sheet's alpha as coverage (Includes/particle_coverage.glsl): block compression leaves
// up to about 2/255 of alpha where a sheet is transparent, and a particle multiplies whatever alpha
// it samples by unbounded HDR colour, so the residue alone draws the quad's edges. The functions are
// evaluated on the device by particle_coverage_probe.comp; the CPU only builds the sheet and reads
// back what each texel adds and whether its fragment is discarded.
#include <gtest/gtest.h>

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"

#include "Platform/Shell.h"

#include "TestDeviceHelper.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

struct ProbePC
{
    float Params[4]; // texel count, the particle's HDR colour, unused, unused
};

// A hand-tuned ember's colour: bright enough that 1/255 of it reads on screen.
constexpr float kHdrColour = 10000.0f;
constexpr uint32_t kSheetSize = 16u;
constexpr float kResidue = 1.0f / 255.0f;
constexpr float kFaintestRim = 3.0f / 255.0f;

// One sheet texel as the probe saw it.
struct Texel
{
    float Alpha = 0.0f;
    float Coverage = 0.0f;
    float Added = 0.0f; // the colour it adds over black
    bool Discarded = false;
    bool Border = false;
};

// A 16 x 16 sheet the way the BC7 cook leaves a soft sprite: the outer ring transparent but for
// the compression residue, the inside a disc whose alpha ramps from 3/255 at its rim to 1 at its
// centre.
std::vector<Texel> CompressedSoftSprite()
{
    std::vector<Texel> sheet(kSheetSize * kSheetSize);
    const float centre = (static_cast<float>(kSheetSize) - 1.0f) * 0.5f;
    for (uint32_t y = 0; y < kSheetSize; ++y)
        for (uint32_t x = 0; x < kSheetSize; ++x)
        {
            Texel& texel = sheet[y * kSheetSize + x];
            texel.Border = x == 0u || y == 0u || x == kSheetSize - 1u || y == kSheetSize - 1u;
            const float distance =
                std::hypot(static_cast<float>(x) - centre, static_cast<float>(y) - centre) / (centre - 1.0f);
            const float disc = std::clamp(1.0f - distance, 0.0f, 1.0f);
            texel.Alpha = texel.Border ? kResidue : std::max(disc, kFaintestRim);
        }
    sheet[kSheetSize / 2u * kSheetSize + kSheetSize / 2u].Alpha = 1.0f;
    return sheet;
}

bool ByAlpha(const Texel& a, const Texel& b)
{
    return a.Alpha < b.Alpha;
}

std::filesystem::path FindProbeSpv()
{
    namespace fs = std::filesystem;
    const fs::path exe = GameEngine::Platform::GetExecutablePath();
    fs::path dir = exe.empty() ? fs::current_path() : exe.parent_path();
    for (int up = 0; up < 6; ++up)
    {
        const fs::path candidate = dir / "Shaders" / "particle_coverage_probe.comp.spv";
        std::error_code ec;
        if (fs::exists(candidate, ec))
            return candidate;
        if (!dir.has_parent_path() || dir.parent_path() == dir)
            break;
        dir = dir.parent_path();
    }
    return {};
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open())
        return {};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

class ParticleCoverageTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        const std::filesystem::path spvPath = FindProbeSpv();
        ASSERT_FALSE(spvPath.empty()) << "particle_coverage_probe.comp.spv was not found above the test executable ("
                                      << GameEngine::Platform::GetExecutablePath().string()
                                      << "); the shader target stages it, so its absence is a build defect";
        const std::vector<uint8_t> spv = Utils::ReadFile(spvPath.string());
        ASSERT_FALSE(spv.empty()) << "staged probe SPIR-V is empty: " << spvPath.string();

        DescriptorBinding texels{};
        texels.binding = 0;
        texels.type = DescriptorType::StorageBuffer;
        texels.count = 1;
        texels.shaderStages = kShaderStageCompute;
        m_Layout.debugName = "ParticleCoverageProbe.Set0";
        m_Layout.bindings = {texels};

        ComputePipelineDesc desc{};
        desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(spv);
        desc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        desc.PushConstants.Size = sizeof(ProbePC);
        desc.PushConstants.StageMask = kShaderStageCompute;
        desc.DebugName = "ParticleCoverageProbe";
        m_Pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(desc));
        ASSERT_TRUE(m_Pipeline.IsValid());
    }

    void TearDown() override
    {
        if (m_Buffer.IsValid())
            m_Device->DestroyBuffer(m_Buffer);
        if (m_Device)
            m_Device->Shutdown();
    }

    // Runs the probe over the sheet and fills each texel's coverage and added colour.
    void Probe(std::vector<Texel>& sheet)
    {
        const size_t bytes = sheet.size() * 4u * sizeof(float);
        BufferDesc bufferDesc{};
        bufferDesc.size = bytes;
        bufferDesc.usage = static_cast<uint32_t>(BufferUsage::Storage) |
                           static_cast<uint32_t>(BufferUsage::TransferSrc) |
                           static_cast<uint32_t>(BufferUsage::TransferDst);
        bufferDesc.memoryUsage = BufferMemoryUsage::Readback;
        bufferDesc.flags = BufferCreateFlags::PersistentlyMapped;
        bufferDesc.debugName = "ParticleCoverageProbe.Texels";
        m_Buffer = m_Device->CreateBuffer(bufferDesc);
        ASSERT_TRUE(m_Buffer.IsValid());
        // Poison the outputs: a dispatch that never lands reads as -1, not as a transparent texel.
        std::vector<float> raw(sheet.size() * 4u, -1.0f);
        for (size_t i = 0; i < sheet.size(); ++i)
            raw[i * 4u] = sheet[i].Alpha;
        m_Device->UpdateBuffer(m_Buffer, 0, bytes, raw.data());

        DescriptorSetDesc setDesc{};
        setDesc.layout = m_Layout;
        setDesc.transient = true;
        setDesc.debugName = "ParticleCoverageProbe.DS";
        DescriptorSetHandle set = m_Device->CreateDescriptorSet(setDesc);
        m_Device->UpdateStorageBufferBinding(set, 0, m_Buffer, 0, bytes);

        ProbePC pc{};
        pc.Params[0] = static_cast<float>(sheet.size());
        pc.Params[1] = kHdrColour;
        auto commands = m_Device->CreateCommandList(IDevice::QueueType::Compute);
        commands->Begin();
        commands->SetPipeline(m_Pipeline);
        commands->BindDescriptorSet(0, set, m_Pipeline);
        commands->SetPushConstants(pc);
        commands->Dispatch((static_cast<uint32_t>(sheet.size()) + 63u) / 64u, 1u, 1u);
        commands->End();
        m_Device->ExecuteCommandLists({commands.get()});
        m_Device->WaitForIdle();

        void* mapped = m_Device->MapBuffer(m_Buffer);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(raw.data(), mapped, bytes);
        m_Device->UnmapBuffer(m_Buffer);
        for (size_t i = 0; i < sheet.size(); ++i)
        {
            sheet[i].Coverage = raw[i * 4u + 1u];
            sheet[i].Added = raw[i * 4u + 2u];
            sheet[i].Discarded = raw[i * 4u + 3u] == 1.0f;
        }
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    BufferHandle m_Buffer{};
};

} // namespace

// The compression residue at the sheet's transparent border adds nothing, however bright the
// particle; the sprite itself keeps its coverage, opaque at the centre and continuous down to its
// faintest rim.
TEST_F(ParticleCoverageTest, ACompressedTransparentBorderAddsNothing)
{
    std::vector<Texel> sheet = CompressedSoftSprite();
    Probe(sheet);

    uint32_t border = 0;
    for (const Texel& texel : sheet)
    {
        if (texel.Border)
        {
            ++border;
            EXPECT_EQ(texel.Added, 0.0f) << "a 1/255 residue under colour " << kHdrColour << " draws the quad's edge";
        }
        else
        {
            EXPECT_GT(texel.Coverage, 0.0f) << "alpha " << texel.Alpha << " is sprite, not residue";
        }
    }
    EXPECT_EQ(border, 4u * (kSheetSize - 1u)) << "positive control: the sheet has its transparent ring";

    const Texel& centre = sheet[kSheetSize / 2u * kSheetSize + kSheetSize / 2u];
    EXPECT_EQ(centre.Coverage, 1.0f) << "an opaque texel stays opaque";
    EXPECT_EQ(centre.Added, kHdrColour);

    std::vector<Texel> sorted = sheet;
    std::sort(sorted.begin(), sorted.end(), ByAlpha);
    for (size_t i = 1; i < sorted.size(); ++i)
        EXPECT_GE(sorted[i].Coverage, sorted[i - 1u].Coverage) << "coverage follows alpha";
}

// A fragment of coverage 0 draws nothing and is discarded unshaded; any coverage above 0 draws,
// however faint: a plume stacks a hundred layers and more per pixel, so dropping fragments far
// fainter than an 8-bit level still moves some of its pixels.
TEST_F(ParticleCoverageTest, OnlyAFullyTransparentFragmentIsDiscarded)
{
    constexpr float kHalfLevel = 0.5f / 255.0f;
    std::vector<Texel> sheet(3);
    sheet[0].Alpha = kResidue;
    sheet[1].Alpha = 2.0f / 255.0f + 1.0f / 4096.0f;
    sheet[2].Alpha = 1.0f;
    Probe(sheet);

    EXPECT_EQ(sheet[0].Coverage, 0.0f) << "positive control: the residue has no coverage";
    EXPECT_TRUE(sheet[0].Discarded) << "a fragment of coverage 0 draws nothing";
    ASSERT_GT(sheet[1].Coverage, 0.0f);
    ASSERT_LT(sheet[1].Coverage, kHalfLevel) << "positive control: fainter than half an 8-bit level";
    EXPECT_FALSE(sheet[1].Discarded) << "coverage " << sheet[1].Coverage << " still draws";
    EXPECT_FALSE(sheet[2].Discarded) << "an opaque fragment draws";
}

// The particle surface takes its opacity through the coverage, or the probe above measures a
// function nothing draws with.
TEST(ParticleCoverageSourceContract, TheParticleSurfaceTakesItsOpacityThroughTheCoverage)
{
#ifndef GE_RENDERER_REPO_ROOT
    GTEST_SKIP() << "GE_RENDERER_REPO_ROOT is not defined for this target";
#else
    const std::string surface = ReadTextFile(std::filesystem::path(GE_RENDERER_REPO_ROOT) /
                                             "Engine/Modules/Rendering/Shaders/Surfaces/particle_surface.glsl");
    ASSERT_FALSE(surface.empty()) << "particle_surface.glsl was not readable";
    EXPECT_NE(surface.find("#include \"Includes/particle_coverage.glsl\""), std::string::npos);
    const size_t coverage = surface.find("opacity = GE_ParticleCoverage(opacity);");
    const size_t output = surface.find("o.opacity = ");
    ASSERT_NE(coverage, std::string::npos) << "the sampled alpha must pass through GE_ParticleCoverage";
    ASSERT_NE(output, std::string::npos);
    EXPECT_LT(coverage, output) << "the coverage must apply before the surface writes its opacity";
#endif
}

// Unlit particle colour is relative to the view's exposure: the vertex stage resolves the reciprocal
// of the view's exposure for unlit particles, and inside the unlit particle branch the surface
// multiplies its colour and emission by it, after every other write to them.
TEST(ParticleExposureSourceContract, UnlitParticlesScaleTheirColourAndEmissionByTheReciprocalExposure)
{
#ifndef GE_RENDERER_REPO_ROOT
    GTEST_SKIP() << "GE_RENDERER_REPO_ROOT is not defined for this target";
#else
    const std::filesystem::path shaders = std::filesystem::path(GE_RENDERER_REPO_ROOT) / "Engine/Modules/Rendering/Shaders";
    const std::string vertex = ReadTextFile(shaders / "Particles/particle_vertex.glsl");
    ASSERT_FALSE(vertex.empty()) << "particle_vertex.glsl was not readable";
    const size_t resolve = vertex.find("v.particleColorScale = 1.0 / GE_ViewExposureScale();");
    ASSERT_NE(resolve, std::string::npos) << "the vertex stage must resolve the reciprocal of the view's exposure";
    const size_t vertexBranch = vertex.rfind("#if", resolve);
    ASSERT_NE(vertexBranch, std::string::npos);
    constexpr std::string_view kUnlitBranch = "#if !defined(GE_USER_PARTICLE_LIT)";
    EXPECT_EQ(vertex.compare(vertexBranch, kUnlitBranch.size(), kUnlitBranch), 0)
        << "only unlit particles are exposure-relative";

    const std::string surface = ReadTextFile(shaders / "Surfaces/particle_surface.glsl");
    ASSERT_FALSE(surface.empty()) << "particle_surface.glsl was not readable";
    EXPECT_EQ(surface.find("GE_ViewExposureScale"), std::string::npos) << "no fragment reads the view's exposure";
    const size_t colour = surface.rfind("o.baseColor *= sIn.particleColorScale;");
    ASSERT_NE(colour, std::string::npos) << "unlit colour must be scaled by the reciprocal exposure";
    const size_t branch = surface.rfind("#if", colour);
    ASSERT_NE(branch, std::string::npos);
    EXPECT_EQ(surface.compare(branch, 70, "#if defined(GE_USER_PARTICLE_BUFFER) && !defined(GE_USER_PARTICLE_LIT)"), 0)
        << "the scale belongs to unlit particles only";
    const size_t emission = surface.find("o.emissive *= sIn.particleColorScale;", colour);
    ASSERT_NE(emission, std::string::npos) << "unlit emission must be scaled by the reciprocal exposure";
    const size_t end = surface.find("#endif", colour);
    EXPECT_LT(emission, end);
    EXPECT_EQ(surface.find("o.baseColor =", colour), std::string::npos) << "nothing may overwrite the scaled colour";
    EXPECT_EQ(surface.find("o.emissive =", colour), std::string::npos) << "nothing may overwrite the scaled emission";
#endif
}
