#include <gtest/gtest.h>
#include <vector>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "TestUtils.h"

using namespace GameEngine::Rendering;

// A tiny compute push constant payload
struct PCCompute { int a; float b; };

TEST(TypedPushConstants, ComputeStageMaskRespected) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    // The blob is produced by the CompileShaders dependency into the build output
    // shader dir; a miss here is a build regression, not an environment condition.
    std::vector<uint8_t> cs;
    ASSERT_TRUE(Tests::ReadSpirvBytes("pc_compute.comp.spv", cs))
        << "pc_compute.comp.spv not found in CompileShaders output";

    PipelineDesc p{}; p.type = PipelineType::Compute; p.debugName = "PCComputeMask";
    p.computeShader = cs;

    PipelineHandle ph = dev->CreatePipeline(p);
    ASSERT_TRUE(ph.IsValid());

    auto cl = dev->CreateCommandList(IDevice::QueueType::Compute);
    ASSERT_TRUE(cl != nullptr);
    cl->Begin();
    cl->SetPipeline(ph);

    // Should use compute-only stage mask based on reflection
    PCCompute pc{ 7, 3.14f };
    cl->SetPushConstants(pc);

    cl->Dispatch(1,1,1);
    cl->End();

    std::vector<CommandList*> lists{ cl.get() };
    dev->ExecuteCommandLists(lists);
    dev->WaitForIdle();

    SUCCEED();
}

