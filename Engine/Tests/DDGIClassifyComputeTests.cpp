// Dispatch the shipped software classify kernel. Classification owns the
// probe-cell history reset: a probe whose activity changes enters a different
// lighting domain, so the kernel must unstamp its cell record and every later
// blend pass then discards the probe's accumulated history.
#include <gtest/gtest.h>

#include "DDGIShippedKernel.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "TestDeviceHelper.h"

#include <array>
#include <cstring>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{
using Vec4 = std::array<float, 4>;

// Mirrors GE_DDGI_PROBE_CELL_UNSTAMPED in Includes/ddgi_common.glsl.
constexpr float kUnstampedCell = -1.0e30f;
constexpr uint32_t kProbeCount = 8;  // a 2x2x2 grid

class DDGIClassifyComputeTest : public ::testing::Test
{
  protected:
    // std140 layout of ddgi_classify_sw.comp's DDGIClassifyParams.
    struct Params
    {
        Vec4 GridMinWS{0, 0, 0, 0};
        Vec4 GridSizeWS{2, 2, 2, 0};
        std::array<int32_t, 4> ProbeCount{2, 2, 2, static_cast<int32_t>(kProbeCount)};
        Vec4 Params0{2, 0, 0, 0};  // x = minimum cell size
        std::array<uint32_t, 4> Dispatch0{0, kProbeCount, 0, 0};
        std::array<uint32_t, 4> SwScene{0, 0, 0, 0};  // x = 0: no TLAS nodes, every ray misses
    };

    void SetUp() override
    {
        device = CreateVulkanDeviceFast();
        if (!device)
            GTEST_SKIP() << "No Vulkan device available";
        std::string error;
        ASSERT_TRUE(LoadDDGIShippedKernel(*device, "ddgi_classify_sw", kernel, error)) << error;
    }

    void TearDown() override
    {
        if (!device)
            return;
        device->WaitForIdle();
        for (auto buffer : buffers)
            device->DestroyBuffer(buffer);
    }

    BufferHandle MakeBuffer(const void* data, size_t bytes, bool uniform)
    {
        BufferDesc desc{};
        desc.size = bytes;
        desc.usage = static_cast<uint32_t>(uniform ? BufferUsage::Uniform : BufferUsage::Storage);
        desc.memoryUsage = uniform ? BufferMemoryUsage::Upload : BufferMemoryUsage::Readback;
        desc.debugName = "DDGI.ClassifyRegression.Buffer";
        const auto buffer = device->CreateBuffer(desc);
        buffers.push_back(buffer);
        if (auto* mapped = buffer.IsValid() ? device->MapBuffer(buffer) : nullptr)
        {
            std::memcpy(mapped, data, bytes);
            device->UnmapBuffer(buffer);
        }
        else
            ADD_FAILURE() << "Could not create or map a test buffer";
        return buffer;
    }

    void Bind(DescriptorSetHandle set, const char* name, BufferHandle buffer, size_t bytes, bool uniform = false)
    {
        const uint32_t binding = kernel.Binding(name);
        ASSERT_NE(binding, DDGIShippedKernel::kMissingBinding) << "ddgi_classify_sw declares no " << name;
        if (uniform)
            device->UpdateBufferBinding(set, binding, buffer, 0, bytes);
        else
            device->UpdateStorageBufferBinding(set, binding, buffer, 0, bytes);
    }

    std::vector<Vec4> Readback(BufferHandle buffer)
    {
        std::vector<Vec4> values(kProbeCount);
        const auto* mapped = device->MapBuffer(buffer);
        if (!mapped)
        {
            ADD_FAILURE() << "Could not map a readback buffer";
            return values;
        }
        std::memcpy(values.data(), mapped, values.size() * sizeof(Vec4));
        device->UnmapBuffer(buffer);
        return values;
    }

    // Classifies every probe once in an empty scene. Returns the probe state
    // and cell records the kernel leaves behind.
    void Classify(const std::vector<Vec4>& state, const std::vector<Vec4>& cells)
    {
        const Params params{};
        const std::array<float, 4> unusedScene{};
        const auto uniform = MakeBuffer(&params, sizeof(params), true);
        const auto stateBuffer = MakeBuffer(state.data(), state.size() * sizeof(Vec4), false);
        const auto cellBuffer = MakeBuffer(cells.data(), cells.size() * sizeof(Vec4), false);
        const auto sceneBuffer = MakeBuffer(unusedScene.data(), sizeof(unusedScene), false);
        DescriptorSetDesc setDesc{};
        setDesc.layout = kernel.Layout;
        const auto set = device->CreateDescriptorSet(setDesc);
        Bind(set, "DDGIParams", uniform, sizeof(params), true);
        Bind(set, "DDGIProbeState", stateBuffer, state.size() * sizeof(Vec4));
        Bind(set, "DDGIProbeCellRW", cellBuffer, cells.size() * sizeof(Vec4));
        for (const char* scene : {"DDGISwPackedScene", "DDGISwNodes", "DDGISwTriangleIndices", "DDGISwVertexData"})
            Bind(set, scene, sceneBuffer, sizeof(unusedScene));
        auto commands = device->CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        commands->SetPipeline(kernel.Pipeline);
        commands->BindDescriptorSet(0, set, kernel.Pipeline);
        commands->Dispatch(kProbeCount, 1, 1);
        commands->End();
        device->ExecuteCommandLists({commands.get()});
        device->WaitForIdle();
        stateResult = Readback(stateBuffer);
        cellResult = Readback(cellBuffer);
    }

    std::unique_ptr<IDevice> device;
    DDGIShippedKernel kernel;
    std::vector<BufferHandle> buffers;
    std::vector<Vec4> stateResult;
    std::vector<Vec4> cellResult;
};

TEST_F(DDGIClassifyComputeTest, ActivationUnstampsTheCellWhileAStableProbeKeepsIt)
{
    // Probes 0-3 were inactive and probes 4-7 active. With nothing to hit,
    // every probe classifies as free space, so only the first four change
    // activity. All cells carry their own stamp, as the blend pass leaves them.
    std::vector<Vec4> state(kProbeCount);
    std::vector<Vec4> cells(kProbeCount);
    for (uint32_t probe = 0; probe < kProbeCount; ++probe)
    {
        state[probe] = {0, 0, 0, probe < 4 ? 0.0f : 1.0f};
        cells[probe] = {float(probe % 2), float((probe / 2) % 2), float(probe / 4), 0};
    }
    Classify(state, cells);
    ASSERT_EQ(stateResult.size(), kProbeCount);
    ASSERT_EQ(cellResult.size(), kProbeCount);
    for (uint32_t probe = 0; probe < kProbeCount; ++probe)
    {
        SCOPED_TRACE(::testing::Message() << "probe " << probe);
        EXPECT_FLOAT_EQ(stateResult[probe][3], 1.0f);
        if (probe < 4)
        {
            EXPECT_EQ(cellResult[probe], (Vec4{kUnstampedCell, kUnstampedCell, kUnstampedCell, 1.0f}));
        }
        else
        {
            EXPECT_EQ(cellResult[probe], cells[probe]);
        }
    }
}
}  // namespace
