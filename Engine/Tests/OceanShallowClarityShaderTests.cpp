#include <gtest/gtest.h>

#include "Ocean/OceanTypes.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "StagedTestPaths.h"
#include "TestDeviceHelper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <memory>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Ocean;

namespace
{
// Set 2 binding 1 is the OceanParamsBuffer the ocean draw binds by name.
constexpr uint32_t kOceanParamsSet = 2;
constexpr uint32_t kOceanParamsBinding = 1;
constexpr uint32_t kPathCount = 8;

// View-ray path lengths (m) through the water: the surface, inside the default
// window, its end, and well past it.
constexpr std::array<float, kPathCount> kPaths{0.0f, 0.5f, 1.0f, 2.0f, 3.5f, 6.0f, 8.0f, 20.0f};

// The probe's output block (ocean_shallow_clarity_probe.comp), std430.
struct ProbeResult
{
    float Scale[kPathCount];
    float Legacy[kPathCount];
    float SeenFromBelow;
    float Neighbours[3];
};

struct ProbeInput
{
    float Paths[kPathCount];
    float UnderBelow[2];
};

float Smoothstep(float edge0, float edge1, float x)
{
    const float t = std::fmin(std::fmax((x - edge0) / (edge1 - edge0), 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

class OceanShallowClarityShaderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        // The normal shader target compiles and stages this probe beside the
        // other ocean compute shaders. Missing SPIR-V is a build error.
        m_Probe = Utils::ReadFile((TestPaths::StagedRoot() / "Shaders" /
                                   "ocean_shallow_clarity_probe.comp.spv").string());
        ASSERT_FALSE(m_Probe.empty());
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (const auto buffer : m_Buffers)
        {
            if (buffer.IsValid())
                m_Device->DestroyBuffer(buffer);
        }
        m_Device->Shutdown();
    }

    BufferHandle MakeBuffer(size_t size, BufferUsage usage, const void* data)
    {
        BufferDesc desc{};
        desc.size = size;
        desc.usage = static_cast<uint32_t>(usage);
        desc.memoryUsage = BufferMemoryUsage::Upload;
        auto buffer = m_Device->CreateBuffer(desc);
        m_Buffers.push_back(buffer);
        if (buffer.IsValid())
        {
            void* mapped = m_Device->MapBuffer(buffer);
            EXPECT_NE(mapped, nullptr);
            if (mapped)
                std::memcpy(mapped, data, size);
            m_Device->UnmapBuffer(buffer);
        }
        return buffer;
    }

    // Uploads the real CPU parameter block, dispatches the probe once and reads
    // its result back.
    ProbeResult Run(const OceanParamsGPU& params, float underBelow)
    {
        ProbeResult result{};
        DescriptorSetLayoutDesc probeLayout{}, emptyLayout{}, oceanLayout{};
        auto binding = [](uint32_t slot) {
            DescriptorBinding value{};
            value.binding = slot;
            value.type = DescriptorType::StorageBuffer;
            value.count = 1;
            value.shaderStages = kShaderStageCompute;
            return value;
        };
        probeLayout.bindings = {binding(0), binding(1)};
        oceanLayout.bindings = {binding(kOceanParamsBinding)};
        ComputePipelineDesc pipelineDesc{};
        pipelineDesc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(m_Probe);
        for (const auto& layout : {probeLayout, emptyLayout, oceanLayout})
            pipelineDesc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
        const auto pipeline =
            m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(pipelineDesc));
        EXPECT_TRUE(pipeline.IsValid());
        if (!pipeline.IsValid())
            return result;

        ProbeInput input{};
        std::memcpy(input.Paths, kPaths.data(), sizeof(input.Paths));
        input.UnderBelow[0] = underBelow;
        input.UnderBelow[1] = 1.0f;
        ProbeResult poison{};
        std::fill(std::begin(poison.Scale), std::end(poison.Scale), -17.0f);
        std::fill(std::begin(poison.Legacy), std::end(poison.Legacy), -17.0f);
        const auto outputBuffer = MakeBuffer(sizeof(poison), BufferUsage::Storage, &poison);
        const auto inputBuffer = MakeBuffer(sizeof(input), BufferUsage::Storage, &input);
        const auto paramsBuffer = MakeBuffer(sizeof(params), BufferUsage::Storage, &params);
        EXPECT_TRUE(outputBuffer.IsValid() && inputBuffer.IsValid() && paramsBuffer.IsValid());

        DescriptorSetDesc descriptor{};
        descriptor.layout = probeLayout;
        const auto probeSet = m_Device->CreateDescriptorSet(descriptor);
        descriptor.layout = oceanLayout;
        const auto oceanSet = m_Device->CreateDescriptorSet(descriptor);
        EXPECT_TRUE(probeSet.IsValid() && oceanSet.IsValid());
        m_Device->UpdateStorageBufferBinding(probeSet, 0, outputBuffer, 0, sizeof(poison));
        m_Device->UpdateStorageBufferBinding(probeSet, 1, inputBuffer, 0, sizeof(input));
        m_Device->UpdateStorageBufferBinding(oceanSet, kOceanParamsBinding, paramsBuffer, 0,
                                             sizeof(params));

        auto command = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        command->Begin();
        command->SetPipeline(pipeline);
        command->BindDescriptorSet(0, probeSet, pipeline);
        command->BindDescriptorSet(kOceanParamsSet, oceanSet, pipeline);
        command->Dispatch(1, 1, 1);
        command->End();
        m_Device->ExecuteCommandLists({command.get()});
        m_Device->WaitForIdle();

        void* mapped = m_Device->MapBuffer(outputBuffer);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
            std::memcpy(&result, mapped, sizeof(result));
        m_Device->UnmapBuffer(outputBuffer);
        return result;
    }

    std::unique_ptr<IDevice> m_Device;
    std::vector<uint8_t> m_Probe;
    std::vector<BufferHandle> m_Buffers;
};
} // namespace

// The surface passes underBelow as 0 (above water) or 1 (seen from below).
// Above water the default upload must render exactly as the window's literals
// did: same bits at every path, not merely close. Seen from below there is no
// window, so the scale is exactly 1; it is not compared with the literal form
// there, whose nested mix a driver may contract to one ULP below 1.
TEST_F(OceanShallowClarityShaderTest, DefaultWindowMatchesTheFormerConstantsAboveWaterAndIsOneFromBelow)
{
    OceanParamsGPU params{};
    ASSERT_FLOAT_EQ(params.ShallowClarityDistance, 8.0f);
    ASSERT_FLOAT_EQ(params.ShallowClarityFloor, 0.22f);
    const ProbeResult above = Run(params, 0.0f);
    const ProbeResult below = Run(params, 1.0f);
    for (uint32_t i = 0; i < kPathCount; ++i)
    {
        SCOPED_TRACE(::testing::Message() << "path=" << kPaths[i]);
        uint32_t scaleBits = 0, legacyBits = 0;
        std::memcpy(&scaleBits, &above.Scale[i], sizeof(float));
        std::memcpy(&legacyBits, &above.Legacy[i], sizeof(float));
        EXPECT_EQ(scaleBits, legacyBits) << above.Scale[i] << " vs " << above.Legacy[i];
        EXPECT_EQ(below.Scale[i], 1.0f);
    }
}

// An authored window reaches the shader: the ramp ends at the authored distance
// and starts at the authored floor, and seen from below there is no window.
TEST_F(OceanShallowClarityShaderTest, AuthoredWindowReachesTheShader)
{
    struct Case
    {
        float Distance;
        float Floor;
    };
    for (const Case window : {Case{2.0f, 0.0f}, Case{3.5f, 0.6f}, Case{8.0f, 1.0f}, Case{20.0f, 0.22f}})
    {
        SCOPED_TRACE(::testing::Message() << "distance=" << window.Distance << " floor=" << window.Floor);
        OceanParamsGPU params{};
        params.ShallowClarityDistance = window.Distance;
        params.ShallowClarityFloor = window.Floor;
        // Neighbours on either side of the window's two floats.
        params.RefractionAvailable = 1u;
        params.RefractionDepthMatched = 1u;
        params.RefractionStrength = 0.35f;
        params.DepthFogStartDistance = 1.25f;
        const ProbeResult result = Run(params, 0.0f);
        for (uint32_t i = 0; i < kPathCount; ++i)
        {
            SCOPED_TRACE(::testing::Message() << "path=" << kPaths[i]);
            const float ramp = Smoothstep(0.0f, window.Distance, kPaths[i]);
            const float expected = window.Floor + (1.0f - window.Floor) * ramp;
            EXPECT_NEAR(result.Scale[i], expected, 1.0e-5f);
        }
        EXPECT_FLOAT_EQ(result.Scale[0], window.Floor);
        EXPECT_FLOAT_EQ(result.SeenFromBelow, 1.0f);
        EXPECT_FLOAT_EQ(result.Neighbours[0], 3.0f);
        EXPECT_FLOAT_EQ(result.Neighbours[1], 0.35f);
        EXPECT_FLOAT_EQ(result.Neighbours[2], 1.25f);
    }
}
