#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"

using namespace GameEngine::Rendering;

TEST(DynamicRendering, OffscreenSmoke_ClearOnly)
{
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd); ASSERT_TRUE(dev && dev->Initialize(dd));

    // Create offscreen texture (RGBA8)
    TextureDesc td{}; td.width = 64; td.height = 64; td.format = (uint32_t)TextureFormat::RGBA8_UNORM; td.usage = (uint32_t)(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    TextureHandle offscreen = dev->CreateTexture(td);
    ASSERT_NE(offscreen, INVALID_HANDLE);

    // Record commands - clear-only offscreen dynamic rendering
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();

    RenderPassDesc rp{}; rp.colorTargets[0] = offscreen; rp.colorTargetCount = 1; rp.clearColor[0] = true;
    rp.clearColorValue[0][0] = 0.2f; rp.clearColorValue[0][1] = 0.4f; rp.clearColorValue[0][2] = 0.6f; rp.clearColorValue[0][3] = 1.0f;
    cl->BeginRenderPass(rp);
    cl->EndRenderPass();

    cl->End();

    std::vector<CommandList*> lists{ cl.get() };
    dev->ExecuteCommandLists(lists);
    dev->WaitForIdle();

    dev->DestroyTexture(offscreen);
}

