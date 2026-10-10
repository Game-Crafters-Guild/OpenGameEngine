// Metal backend bring-up smoke test: clear + one hand-written MSL triangle,
// verified by blit readback of the swapchain drawable.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"

#if defined(HAVE_GLFW)
#include <GLFW/glfw3.h>
#endif

#include <cstring>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{

constexpr uint32_t kWidth = 320;
constexpr uint32_t kHeight = 200;

// Small centered triangle so interior and exterior pixels see different
// colors: interior = green triangle, exterior = red clear.
const char* kTriangleMSL = R"(
#include <metal_stdlib>
using namespace metal;

struct VSOut
{
    float4 position [[position]];
    float4 color;
};

vertex VSOut vertex_main(uint vid [[vertex_id]])
{
    float2 positions[3] = { float2(-0.5, -0.5), float2(0.5, -0.5), float2(0.0, 0.5) };
    VSOut out;
    out.position = float4(positions[vid], 0.0, 1.0);
    out.color = float4(0.0, 1.0, 0.0, 1.0);
    return out;
}

fragment float4 fragment_main(VSOut in [[stage_in]])
{
    return in.color;
}
)";

std::vector<uint8_t> ToBytes(const char* text)
{
    const size_t len = std::strlen(text);
    return std::vector<uint8_t>(text, text + len);
}

} // namespace

TEST(MetalTriangleSmoke, ClearAndTriangle_ReadbackMatches)
{
#if !defined(HAVE_GLFW)
    GTEST_SKIP() << "GLFW not available on this build agent";
#else
    if (!DeviceFactory::IsAPISupported(GraphicsAPI::Metal))
    {
        GTEST_SKIP() << "Metal backend not built";
    }

    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(kWidth, kHeight, "MetalTriangleSmoke", nullptr, nullptr);
    ASSERT_NE(window, nullptr);

    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Metal;
    desc.enableDebugLayer = true;
    auto device = DeviceFactory::CreateDevice(desc);
    ASSERT_TRUE(device);
    ASSERT_TRUE(device->Initialize(desc));
    ASSERT_EQ(device->GetAPI(), GraphicsAPI::Metal);
    ASSERT_TRUE(device->CreateAndActivateWindowTarget(window, kWidth, kHeight));

    // Triangle pipeline from embedded MSL (SPIR-V translation is milestone 3).
    PipelineDesc pipelineDesc{};
    pipelineDesc.type = PipelineType::Graphics;
    pipelineDesc.vertexShader = ToBytes(kTriangleMSL);
    pipelineDesc.pixelShader = ToBytes(kTriangleMSL);
    pipelineDesc.colorAttachmentFormats = {static_cast<uint32_t>(TextureFormat::BGRA8_UNORM)};
    pipelineDesc.rasterizationState.cullMode = CullModeFlagBits::None;
    pipelineDesc.depthStencilState.depthTestEnable = false;
    pipelineDesc.depthStencilState.depthWriteEnable = false;
    pipelineDesc.debugName = "MetalSmokeTriangle";
    PipelineHandle pipeline = device->CreatePipeline(pipelineDesc);
    ASSERT_TRUE(pipeline.IsValid());

    // Readback destination (BGRA8: 4 bytes/pixel).
    BufferDesc readbackDesc{};
    readbackDesc.size = static_cast<size_t>(kWidth) * kHeight * 4;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    readbackDesc.debugName = "MetalSmokeReadback";
    BufferHandle readback = device->CreateBuffer(readbackDesc);
    ASSERT_TRUE(readback.IsValid());

    constexpr uint32_t kFrames = 6;
    for (uint32_t frame = 0; frame < kFrames; ++frame)
    {
        ASSERT_TRUE(device->BeginFrame()) << "frame " << frame;
        TextureHandle backbuffer = device->GetCurrentSwapchainImageHandle();
        ASSERT_TRUE(backbuffer.IsValid()) << "frame " << frame;

        auto commandList = device->CreateCommandList(IDevice::QueueType::Graphics);
        ASSERT_TRUE(commandList);
        commandList->Begin();

        RenderPassDesc pass{};
        pass.colorTargets[0] = backbuffer;
        pass.colorTargetCount = 1;
        pass.clearColor[0] = true;
        pass.clearColorValue[0][0] = 1.0f; // red clear
        pass.clearColorValue[0][1] = 0.0f;
        pass.clearColorValue[0][2] = 0.0f;
        pass.clearColorValue[0][3] = 1.0f;
        pass.depthTarget = INVALID_TEXTURE_HANDLE;
        pass.clearDepth = false;

        commandList->BeginRenderPass(pass);
        commandList->SetPipeline(pipeline);
        commandList->SetViewport(0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight));
        commandList->SetScissor(0, 0, kWidth, kHeight);
        commandList->Draw(3);
        commandList->EndRenderPass();

        const bool isLastFrame = (frame == kFrames - 1);
        if (isLastFrame)
        {
            commandList->CopyTextureToBuffer(backbuffer, readback, kWidth, kHeight);
        }
        commandList->End();

        std::vector<CommandList*> lists{commandList.get()};
        device->ExecuteCommandLists(lists);

        if (isLastFrame)
        {
            device->WaitForIdle();

            const uint8_t* pixels = static_cast<const uint8_t*>(device->MapBuffer(readback));
            ASSERT_NE(pixels, nullptr);

            auto pixelAt = [&](uint32_t x, uint32_t y) {
                return pixels + (static_cast<size_t>(y) * kWidth + x) * 4;
            };

            // Center: inside the triangle -> green. BGRA layout.
            const uint8_t* center = pixelAt(kWidth / 2, kHeight / 2);
            EXPECT_EQ(center[0], 0u) << "center B";
            EXPECT_EQ(center[1], 255u) << "center G";
            EXPECT_EQ(center[2], 0u) << "center R";
            EXPECT_EQ(center[3], 255u) << "center A";

            // Near top-left corner: outside the triangle -> red clear.
            const uint8_t* corner = pixelAt(4, 4);
            EXPECT_EQ(corner[0], 0u) << "corner B";
            EXPECT_EQ(corner[1], 0u) << "corner G";
            EXPECT_EQ(corner[2], 255u) << "corner R";
            EXPECT_EQ(corner[3], 255u) << "corner A";

            device->UnmapBuffer(readback);
        }

        device->Present();
        glfwPollEvents();
    }

    device->WaitForIdle();
    device->DestroyBuffer(readback);
    device->Shutdown();
    device.reset();

    glfwDestroyWindow(window);
    glfwTerminate();
#endif
}

TEST(MetalTriangleSmoke, PresentWithoutDraws_IsClean)
{
#if !defined(HAVE_GLFW)
    GTEST_SKIP() << "GLFW not available on this build agent";
#else
    if (!DeviceFactory::IsAPISupported(GraphicsAPI::Metal))
    {
        GTEST_SKIP() << "Metal backend not built";
    }

    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(kWidth, kHeight, "MetalPresentNoDraw", nullptr, nullptr);
    ASSERT_NE(window, nullptr);

    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Metal;
    auto device = DeviceFactory::CreateDevice(desc);
    ASSERT_TRUE(device);
    ASSERT_TRUE(device->Initialize(desc));
    ASSERT_TRUE(device->CreateAndActivateWindowTarget(window, kWidth, kHeight));

    for (int i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(device->BeginFrame());
        device->Present();
    }

    device->Shutdown();
    device.reset();
    glfwDestroyWindow(window);
    glfwTerminate();
#endif
}
