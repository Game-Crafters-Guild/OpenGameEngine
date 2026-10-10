#include <gtest/gtest.h>

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "StagedTestPaths.h"
#include "TestDeviceHelper.h"

#include <array>
#include <cstring>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
// Mirrors PcssProbe in shadow_pcss_probe.comp (std430).
struct PcssProbeCase
{
    std::array<float, 4> A{};
    std::array<float, 4> B{};
};
struct PcssProbeData
{
    std::array<uint32_t, 4> Meta{};
    std::array<PcssProbeCase, 16> Cases{};
    std::array<std::array<float, 4>, 16> Results{};
};

enum class PcssProbeOp : uint32_t
{
    IsBlocker = 0,
    KernelProvesLit = 1,
    BoundedSearchTexels = 2,
    PenumbraTexels = 3,
};

PcssProbeCase OpCase(PcssProbeOp op, std::array<float, 4> a, float b0 = 0.0f, float b1 = 0.0f)
{
    return {a, {b0, b1, 0.0f, static_cast<float>(op)}};
}

PcssProbeCase IsBlockerCase(float tapDepth, float refDepth, float offsetU, float offsetV, float gradientU,
                            float gradientV)
{
    return {{tapDepth, refDepth, offsetU, offsetV},
            {gradientU, gradientV, 0.0f, static_cast<float>(PcssProbeOp::IsBlocker)}};
}

class ShadowPcssDecisionTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        if (m_Buffer.IsValid())
            m_Device->DestroyBuffer(m_Buffer);
        m_Device->Shutdown();
    }

    // Runs the probe over `cases` and returns one result per case.
    std::vector<float> Evaluate(const std::vector<PcssProbeCase>& cases)
    {
        std::vector<float> out;
        const auto bytes = Utils::ReadFile(
            (TestPaths::StagedRoot() / "Shaders" / "shadow_pcss_probe.comp.spv").string());
        EXPECT_FALSE(bytes.empty());
        EXPECT_LE(cases.size(), PcssProbeData{}.Cases.size());
        if (bytes.empty() || cases.size() > PcssProbeData{}.Cases.size())
            return out;
        DescriptorSetLayoutDesc layout{};
        DescriptorBinding binding{};
        binding.binding = 0;
        binding.type = DescriptorType::StorageBuffer;
        binding.count = 1;
        binding.shaderStages = kShaderStageCompute;
        layout.bindings = {binding};
        ComputePipelineDesc pipelineDesc{};
        pipelineDesc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(bytes);
        pipelineDesc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
        const auto pipeline =
            m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(pipelineDesc));
        EXPECT_TRUE(pipeline.IsValid());

        PcssProbeData data{};
        data.Meta[0] = static_cast<uint32_t>(cases.size());
        std::copy(cases.begin(), cases.end(), data.Cases.begin());
        for (auto& result : data.Results)
            result.fill(-17.0f);
        BufferDesc desc{};
        desc.size = sizeof(data);
        desc.usage = static_cast<uint32_t>(BufferUsage::Storage);
        desc.memoryUsage = BufferMemoryUsage::Upload;
        m_Buffer = m_Device->CreateBuffer(desc);
        EXPECT_TRUE(m_Buffer.IsValid());
        m_Device->UpdateBuffer(m_Buffer, 0, sizeof(data), &data);
        DescriptorSetDesc setDesc{};
        setDesc.layout = layout;
        const auto set = m_Device->CreateDescriptorSet(setDesc);
        m_Device->UpdateStorageBufferBinding(set, 0, m_Buffer, 0, sizeof(data));
        auto command = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        command->Begin();
        command->SetPipeline(pipeline);
        command->BindDescriptorSet(0, set, pipeline);
        command->Dispatch(1, 1, 1);
        command->End();
        m_Device->ExecuteCommandLists({command.get()});
        m_Device->WaitForIdle();
        PcssProbeData read{};
        if (void* mapped = m_Device->MapBuffer(m_Buffer))
        {
            std::memcpy(&read, mapped, sizeof(read));
            m_Device->UnmapBuffer(m_Buffer);
        }
        for (size_t i = 0; i < cases.size(); ++i)
            out.push_back(read.Results[i][0]);
        return out;
    }

    std::unique_ptr<IDevice> m_Device;
    BufferHandle m_Buffer{};
};
} // namespace

// A receiver tilted toward the light: its plane rises 0.2 in raw depth per UV
// along +U. The blocker search's taps compare against that plane, as the PCF
// taps do, so the receiver's own surface up-slope is not its own blocker.
TEST_F(ShadowPcssDecisionTest, SlopedReceiverIsNotItsOwnBlocker)
{
    const auto results = Evaluate({
        // The receiver surface itself, 0.01 UV up-slope (plane depth 0.502).
        IsBlockerCase(0.502f, 0.5f, 0.01f, 0.0f, 0.2f, 0.0f),
        // A caster 0.008 nearer the light than the plane there.
        IsBlockerCase(0.51f, 0.5f, 0.01f, 0.0f, 0.2f, 0.0f),
        // The same surface depth with the receiver-plane bias off: a flat reference.
        IsBlockerCase(0.502f, 0.5f, 0.01f, 0.0f, 0.0f, 0.0f),
    });
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0], 0.0f) << "receiver surface up-slope";
    EXPECT_EQ(results[1], 1.0f) << "caster above the receiver plane";
    EXPECT_EQ(results[2], 1.0f) << "flat reference when the bias is off";
}

// The pyramid's lit exit stands for "every PCF tap of every kernel up to the cap
// is lit". Taps compare against the receiver plane, so on a slope the lowest
// reference in the kernel sits below the centre's: the receiver's own depth at
// the centre is not proof, a depth below the plane's lowest point is.
TEST_F(ShadowPcssDecisionTest, KernelLitProofHoldsForEveryTapOnTheReceiverPlane)
{
    const auto results = Evaluate({
        OpCase(PcssProbeOp::KernelProvesLit, {0.49f, 0.5f, 0.01f, 0.0f}),
        OpCase(PcssProbeOp::KernelProvesLit, {0.5f, 0.5f, 0.01f, 0.0f}, 0.2f, 0.0f),
        OpCase(PcssProbeOp::KernelProvesLit, {0.497f, 0.5f, 0.01f, 0.0f}, 0.2f, 0.0f),
        OpCase(PcssProbeOp::KernelProvesLit, {0.51f, 0.5f, 0.01f, 0.0f}),
        // A diagonal slope (0.1 per UV on both axes): the tap on the kernel's
        // diagonal sits 0.01 / sqrt(2) UV out on both axes, so its reference is
        // 0.0014 below the centre's. A per-axis maximum (0.001) misses it.
        OpCase(PcssProbeOp::KernelProvesLit, {0.4988f, 0.5f, 0.01f, 0.0f}, 0.1f, 0.1f),
    });
    ASSERT_EQ(results.size(), 5u);
    EXPECT_EQ(results[0], 1.0f) << "flat receiver, nothing nearer the light";
    EXPECT_EQ(results[1], 0.0f) << "slope: the centre depth is above the kernel's lowest tap reference";
    EXPECT_EQ(results[2], 1.0f) << "slope: below the kernel's lowest tap reference";
    EXPECT_EQ(results[3], 0.0f) << "an occluder in the kernel";
    EXPECT_EQ(results[4], 0.0f) << "diagonal slope: above the kernel's lowest tap reference";
}

// The search only sizes the kernel, and the kernel is clamped to the cap, so the
// search radius never exceeds the cap (nor drops below the minimum disk).
TEST_F(ShadowPcssDecisionTest, BlockerSearchIsBoundedByTheKernelCap)
{
    const auto results = Evaluate({
        OpCase(PcssProbeOp::BoundedSearchTexels, {20.0f, 3.0f, 2.0f, 0.0f}),
        OpCase(PcssProbeOp::BoundedSearchTexels, {20.0f, 1.0f, 2.0f, 0.0f}),
        OpCase(PcssProbeOp::BoundedSearchTexels, {2.5f, 3.0f, 2.0f, 0.0f}),
    });
    ASSERT_EQ(results.size(), 3u);
    EXPECT_FLOAT_EQ(results[0], 3.0f) << "wide search bounded by the cap";
    EXPECT_FLOAT_EQ(results[1], 2.0f) << "never below the minimum disk";
    EXPECT_FLOAT_EQ(results[2], 2.5f) << "a search inside the cap is unchanged";
}

// A search that finds no blocker is not proof the receiver is lit: the kernel is
// the minimum one (one texel) and the filter decides. A found blocker gives the
// physical penumbra, clamped to the cap.
TEST_F(ShadowPcssDecisionTest, NoBlockerFoundGivesTheMinimumKernel)
{
    // 0.1 m texels, a 100 m depth span, tan(half angle) 0.01, a 0.5 m cap.
    const auto results = Evaluate({
        OpCase(PcssProbeOp::PenumbraTexels, {-1.0f, 0.5f, 100.0f, 0.01f}, 0.1f, 0.5f),
        OpCase(PcssProbeOp::PenumbraTexels, {0.7f, 0.5f, 100.0f, 0.01f}, 0.1f, 0.5f),
        OpCase(PcssProbeOp::PenumbraTexels, {0.9f, 0.5f, 100.0f, 0.1f}, 0.1f, 0.5f),
    });
    ASSERT_EQ(results.size(), 3u);
    EXPECT_FLOAT_EQ(results[0], 1.0f) << "no blocker: the minimum kernel";
    EXPECT_NEAR(results[1], 2.0f, 1e-4f) << "20 m behind at tan 0.01: 0.2 m, two texels";
    EXPECT_NEAR(results[2], 5.0f, 1e-4f) << "clamped to the 0.5 m cap";
}
