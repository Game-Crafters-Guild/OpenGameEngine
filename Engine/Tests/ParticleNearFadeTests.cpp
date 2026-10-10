// A particle quad's near-camera fade (Includes/particle_billboard.glsl): a camera-facing quad whose
// bounding sphere comes within one radius of the near plane, within a capped distance, fades out and
// is gone by the time the near plane could cut it; what an authored blend's colour takes for it
// (Includes/particle_coverage.glsl). The functions are evaluated on the device by
// particle_near_fade_probe.comp; the CPU only writes the quads and reads back the results.
#include <gtest/gtest.h>

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"

#include "Components/Rendering/ParticleRenderer.h"
#include "Particles/Rendering/ParticleMaterials.h"
#include "Platform/Shell.h"

#include "TestDeviceHelper.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

struct ProbePC
{
    float Params[4]; // quad count, unused, unused, unused
};

// The shader's alignment codes (Components::ParticleBillboard).
constexpr float kFaceCameraPosition = 5.0f;
constexpr float kFaceCameraYAlongVelocity = 3.0f;
constexpr float kHorizontal = 6.0f;

// One quad as the probe saw it: its centre in view space at ViewDepth ahead and ViewSideways to the
// side of the camera.
struct Quad
{
    float ViewDepth = 0.0f;
    float Radius = 0.0f;
    float NearPlane = 0.0f;
    float Alignment = kFaceCameraPosition;
    float VelocityStretch = 0.0f;
    // The near-fade colour exponent the particle material derives from its blend.
    float ColorExponent = 0.0f;
    float ViewSideways = 0.0f;
    float Fade = -1.0f;
    float ColorScale = -1.0f;
};

// A quad `distance` metres from the camera, `degrees` off the view direction.
Quad QuadAtAngle(float distance, float degrees, float radius, float nearPlane)
{
    const float radians = degrees * 3.14159265f / 180.0f;
    Quad quad{distance * std::cos(radians), radius, nearPlane};
    quad.ViewSideways = distance * std::sin(radians);
    return quad;
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

std::filesystem::path FindProbeSpv()
{
    namespace fs = std::filesystem;
    const fs::path exe = GameEngine::Platform::GetExecutablePath();
    fs::path dir = exe.empty() ? fs::current_path() : exe.parent_path();
    for (int up = 0; up < 6; ++up)
    {
        const fs::path candidate = dir / "Shaders" / "particle_near_fade_probe.comp.spv";
        std::error_code ec;
        if (fs::exists(candidate, ec))
            return candidate;
        if (!dir.has_parent_path() || dir.parent_path() == dir)
            break;
        dir = dir.parent_path();
    }
    return {};
}

class ParticleNearFadeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        const std::filesystem::path spvPath = FindProbeSpv();
        ASSERT_FALSE(spvPath.empty()) << "particle_near_fade_probe.comp.spv was not found above the test executable ("
                                      << GameEngine::Platform::GetExecutablePath().string()
                                      << "); the shader target stages it, so its absence is a build defect";
        const std::vector<uint8_t> spv = Utils::ReadFile(spvPath.string());
        ASSERT_FALSE(spv.empty()) << "staged probe SPIR-V is empty: " << spvPath.string();

        DescriptorBinding quads{};
        quads.binding = 0;
        quads.type = DescriptorType::StorageBuffer;
        quads.count = 1;
        quads.shaderStages = kShaderStageCompute;
        m_Layout.debugName = "ParticleNearFadeProbe.Set0";
        m_Layout.bindings = {quads};

        ComputePipelineDesc desc{};
        desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(spv);
        desc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        desc.PushConstants.Size = sizeof(ProbePC);
        desc.PushConstants.StageMask = kShaderStageCompute;
        desc.DebugName = "ParticleNearFadeProbe";
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

    // Runs the probe over the quads and fills each quad's fade and colour scale.
    void Probe(std::vector<Quad>& quads)
    {
        const size_t bytes = quads.size() * 12u * sizeof(float);
        BufferDesc bufferDesc{};
        bufferDesc.size = bytes;
        bufferDesc.usage = static_cast<uint32_t>(BufferUsage::Storage) |
                           static_cast<uint32_t>(BufferUsage::TransferSrc) |
                           static_cast<uint32_t>(BufferUsage::TransferDst);
        bufferDesc.memoryUsage = BufferMemoryUsage::Readback;
        bufferDesc.flags = BufferCreateFlags::PersistentlyMapped;
        bufferDesc.debugName = "ParticleNearFadeProbe.Quads";
        m_Buffer = m_Device->CreateBuffer(bufferDesc);
        ASSERT_TRUE(m_Buffer.IsValid());
        // Poison the output: a dispatch that never lands reads as -1, not as a fade.
        std::vector<float> raw(quads.size() * 12u, -1.0f);
        for (size_t i = 0; i < quads.size(); ++i)
        {
            raw[i * 12u] = quads[i].ViewSideways;
            raw[i * 12u + 1u] = 0.0f;
            raw[i * 12u + 2u] = quads[i].ViewDepth;
            raw[i * 12u + 3u] = quads[i].Radius;
            raw[i * 12u + 4u] = quads[i].NearPlane;
            raw[i * 12u + 5u] = quads[i].Alignment;
            raw[i * 12u + 6u] = quads[i].VelocityStretch;
            raw[i * 12u + 7u] = quads[i].ColorExponent;
        }
        m_Device->UpdateBuffer(m_Buffer, 0, bytes, raw.data());

        DescriptorSetDesc setDesc{};
        setDesc.layout = m_Layout;
        setDesc.transient = true;
        setDesc.debugName = "ParticleNearFadeProbe.DS";
        DescriptorSetHandle set = m_Device->CreateDescriptorSet(setDesc);
        m_Device->UpdateStorageBufferBinding(set, 0, m_Buffer, 0, bytes);

        ProbePC pc{};
        pc.Params[0] = static_cast<float>(quads.size());
        auto commands = m_Device->CreateCommandList(IDevice::QueueType::Compute);
        commands->Begin();
        commands->SetPipeline(m_Pipeline);
        commands->BindDescriptorSet(0, set, m_Pipeline);
        commands->SetPushConstants(pc);
        commands->Dispatch((static_cast<uint32_t>(quads.size()) + 63u) / 64u, 1u, 1u);
        commands->End();
        m_Device->ExecuteCommandLists({commands.get()});
        m_Device->WaitForIdle();

        void* mapped = m_Device->MapBuffer(m_Buffer);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(raw.data(), mapped, bytes);
        m_Device->UnmapBuffer(m_Buffer);
        for (size_t i = 0; i < quads.size(); ++i)
        {
            quads[i].Fade = raw[i * 12u + 8u];
            quads[i].ColorScale = raw[i * 12u + 9u];
        }
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    BufferHandle m_Buffer{};
};

} // namespace

// A camera-facing quad whose centre is between one and two radii past the near plane carries its
// alpha faded in proportion; closer it is gone, farther it is untouched. The same holds for a small
// quad at its own scale and for a camera whose near plane is farther out; a quad larger than the cap
// fades only within it, and a ground-aligned or stretched quad does not fade.
TEST_F(ParticleNearFadeTest, AQuadInsideTheFadeDistanceCarriesTheFadedAlpha)
{
    struct Row
    {
        Quad In;
        float Expected;
        const char* What;
    };
    const float bigPuff = 10.0f * std::sqrt(2.0f); // half the diagonal of a 20 m square
    const std::vector<Row> rows = {
        {{3.0f, 1.0f, 0.1f}, 1.0f, "two radii and more past the near plane: untouched"},
        {{1.6f, 1.0f, 0.1f}, 0.5f, "halfway through the fade distance"},
        {{1.1f, 1.0f, 0.1f}, 0.0f, "the bounding sphere touches the near plane: gone"},
        {{0.475f, 0.25f, 0.1f}, 0.5f, "a small quad halfway through its own fade distance"},
        {{1.75f, 0.5f, 1.0f}, 0.5f, "a far near plane moves the fade out with it"},
        {{15.0f, bigPuff, 0.1f}, 1.0f, "a 20 m puff 15 m away is drawn as without the fade"},
        {{2.35f, bigPuff, 0.1f}, 0.5f, "a 20 m puff halfway through the capped distance"},
        {{1.6f, 1.0f, 0.1f, kHorizontal}, 1.0f, "a ground-aligned quad at the same depth does not fade"},
        {{1.6f, 1.0f, 0.1f, kFaceCameraYAlongVelocity, 2.0f}, 1.0f, "a velocity-stretched spark does not fade"},
        {{1.6f, 1.0f, 0.1f, kFaceCameraPosition, 0.5f}, 1.0f, "a camera-facing quad on a stretching emitter does not fade, at rest or moving"},
    };
    std::vector<Quad> quads;
    for (const Row& row : rows)
        quads.push_back(row.In);
    Probe(quads);
    for (size_t i = 0; i < rows.size(); ++i)
        EXPECT_NEAR(quads[i].Fade, rows[i].Expected, 1e-5f)
            << rows[i].What << " (view depth " << rows[i].In.ViewDepth << ", radius " << rows[i].In.Radius
            << ", near plane " << rows[i].In.NearPlane << ")";
}

// The fade follows the distance from the camera to the quad, not its depth along the view: the same
// quad at the same distance fades alike straight ahead and towards the edge of the view, so a cloud
// does not thin and thicken as the camera turns.
TEST_F(ParticleNearFadeTest, TheSameDistanceFadesAlikeWhereverTheQuadIsInView)
{
    std::vector<Quad> quads = {QuadAtAngle(1.6f, 0.0f, 1.0f, 0.1f), QuadAtAngle(1.6f, 60.0f, 1.0f, 0.1f),
                               QuadAtAngle(1.6f, 85.0f, 1.0f, 0.1f)};
    Probe(quads);
    ASSERT_NEAR(quads[0].Fade, 0.5f, 1e-5f) << "positive control: halfway through the fade straight ahead";
    EXPECT_NEAR(quads[1].Fade, quads[0].Fade, 1e-5f) << "60 degrees off the view direction";
    EXPECT_NEAR(quads[2].Fade, quads[0].Fade, 1e-5f) << "85 degrees off the view direction";
}

// A quad halfway through the ramp drawn with the material the particle renderer builds for `blend`.
Quad QuadDrawnWith(const std::optional<MaterialBlendState>& blend, float viewDepth)
{
    const Components::ParticleRenderer renderer;
    MaterialDocument source;
    source.blend = blend;
    const MaterialDocument material = Particles::BuildParticleRenderMaterialDocument(renderer, blend ? &source : nullptr);
    Quad quad{viewDepth, 1.0f, 0.1f};
    quad.ColorExponent = std::get<float>(material.properties.at("nearFadeColorExponent"));
    return quad;
}

// The colour fades by the blend. Where the destination keeps one minus the source alpha (straight
// alpha, premultiplied), the sprite hides what is behind it by its faded alpha, so its colour fades
// exactly as much. Where the destination is One (additive), the colour fades as the square of the fade.
// Any other blend (multiply) keeps its colour: fading a multiply sprite's colour toward 0 would turn the
// frame behind it black. Each quad is drawn with the material the renderer builds for its blend.
TEST_F(ParticleNearFadeTest, AGlowUnderAnAuthoredBlendFadesWithItsSprite)
{
    using Factor = MaterialBlendFactor;
    std::vector<Quad> quads = {
        QuadDrawnWith(std::nullopt, 1.6f),
        QuadDrawnWith(MaterialBlendState{.SrcColorFactor = Factor::SrcAlpha, .DstColorFactor = Factor::One}, 1.6f),
        QuadDrawnWith(MaterialBlendState{.SrcColorFactor = Factor::One, .DstColorFactor = Factor::One}, 1.6f),
        QuadDrawnWith(MaterialBlendState{.SrcColorFactor = Factor::One, .DstColorFactor = Factor::One}, 3.0f),
        QuadDrawnWith(MaterialBlendState{.SrcColorFactor = Factor::One, .DstColorFactor = Factor::OneMinusSrcAlpha}, 1.6f),
        QuadDrawnWith(MaterialBlendState{.SrcColorFactor = Factor::DstColor, .DstColorFactor = Factor::Zero}, 1.6f),
    };
    Probe(quads);
    ASSERT_NEAR(quads[1].Fade, 0.5f, 1e-5f) << "positive control: halfway through the ramp";
    EXPECT_NEAR(quads[0].ColorScale, 1.0f, 1e-5f) << "straight alpha: the alpha already carries the fade";
    // What reaches the target: the colour scale, times the faded alpha where the blend multiplies by it.
    EXPECT_NEAR(quads[1].ColorScale * quads[1].Fade, 0.25f, 1e-5f) << "SrcAlpha / One at the ramp's midpoint";
    EXPECT_NEAR(quads[2].ColorScale, 0.25f, 1e-5f) << "One / One at the ramp's midpoint";
    EXPECT_NEAR(quads[3].ColorScale, 1.0f, 1e-5f) << "a glow outside the fade keeps its colour";
    EXPECT_NEAR(quads[4].ColorScale, quads[4].Fade, 1e-5f)
        << "premultiplied One / OneMinusSrcAlpha: the colour fades as the alpha that hides the background";
    EXPECT_NEAR(quads[5].ColorScale, 1.0f, 1e-5f) << "multiply DstColor / Zero keeps its colour: it never blackens the frame";
}

// The vertex stage decides by the emitter's authored Velocity Stretch, which the particle carries in
// particleAnimation.z, not by the stretch this frame's speed gives: a resting particle on a stretching
// emitter must not fade and then pop back to full opacity once it moves.
TEST(ParticleNearFadeSourceContract, TheVertexStageGatesTheFadeOnTheAuthoredStretch)
{
#ifndef GE_RENDERER_REPO_ROOT
    GTEST_SKIP() << "GE_RENDERER_REPO_ROOT is not defined for this target";
#else
    const std::string vertex = ReadTextFile(std::filesystem::path(GE_RENDERER_REPO_ROOT) /
                                            "Engine/Modules/Rendering/Shaders/Particles/particle_vertex.glsl");
    ASSERT_FALSE(vertex.empty()) << "particle_vertex.glsl was not readable";
    EXPECT_NE(vertex.find("GE_ParticleFadesNearCamera(alignment, inst.particleAnimation.z)"), std::string::npos)
        << "the fade gate must read the authored Velocity Stretch";
    EXPECT_EQ(vertex.find("stretchLength"), std::string::npos) << "the fade gate must not read this frame's stretch length";
#endif
}
