#include <gtest/gtest.h>
#include <vector>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "TestUtils.h"

using namespace GameEngine::Rendering;

// Larger than 128 bytes to trigger enforcement (e.g., 144 bytes)
struct LargePC { uint8_t bytes[144]; };

TEST(TypedPushConstantsDeath, ExceedsEffectiveLimit) {
#ifndef _DEBUG
    GTEST_SKIP() << "Death test relies on debug assertions; skipping in non-debug builds";
#endif
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    // Minimal triangle pipeline, defaults to 128B effective limit. The blobs are
    // produced by the CompileShaders dependency into the build output shader dir;
    // a miss here is a build regression, not an environment condition.
    std::vector<uint8_t> vs, fs;
    ASSERT_TRUE(Tests::ReadSpirvBytes("triangle.vert.spv", vs)) << "triangle.vert.spv not found in CompileShaders output";
    ASSERT_TRUE(Tests::ReadSpirvBytes("triangle.frag.spv", fs)) << "triangle.frag.spv not found in CompileShaders output";

    PipelineDesc p{}; p.type = PipelineType::Graphics; p.debugName = "TypedPCDeath";
    p.vertexShader = vs; p.pixelShader = fs;
    p.colorAttachmentFormats = { (uint32_t)TextureFormat::RGBA8_UNORM };

    PipelineHandle ph = dev->CreatePipeline(p);
    ASSERT_TRUE(ph.IsValid());

    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl != nullptr);
    cl->Begin();
    cl->SetPipeline(ph);

    LargePC big{};
#if GTEST_HAS_DEATH_TEST
    ASSERT_DEATH({ cl->SetPushConstants(big); }, "Push constants");
#else
    GTEST_SKIP() << "Death tests not supported";
#endif
    cl->End();
}

