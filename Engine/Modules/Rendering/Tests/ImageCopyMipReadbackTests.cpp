#include <gtest/gtest.h>
#include <cstdlib>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"

using namespace GameEngine::Rendering;

// Minimal device-level test that bypasses RenderGraph: clear mip1, copy mip1 -> buffer, validate red
TEST(ImageCopyMipReadback, CopyNonZeroMipToBufferIsRed) {
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif

    DeviceDesc desc{}; desc.applicationName = "ImageCopyMipReadback"; desc.preferredAPI = GraphicsAPI::Vulkan; desc.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(desc);
    if (!dev || !dev->Initialize(desc)) { GTEST_SKIP() << "Device init failed"; }

    const uint32_t baseW = 64, baseH = 64;
    const uint32_t mip = 1; // 32x32
    const uint32_t w = baseW >> mip; const uint32_t h = baseH >> mip;

    TextureDesc td{};
    td.width = baseW; td.height = baseH; td.mipLevels = 2; td.arrayLayers = 1;
    td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) | static_cast<uint32_t>(TextureUsage::TransferSrc) | static_cast<uint32_t>(TextureUsage::TransferDst);
    td.persistent = false; td.debugName = "ImageCopyMipReadbackTex";
    TextureHandle tex = dev->CreateTexture(td);
    ASSERT_NE(tex, INVALID_HANDLE);

    BufferHandle rb = dev->CreateReadbackBuffer(static_cast<size_t>(w)*h*4);
    ASSERT_NE(rb, INVALID_HANDLE);

    // Record commands
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl);
    cl->Begin();
    const float red[4] = {1.f,0.f,0.f,1.f};
    cl->ClearColorImageSubresource(tex, mip, 0, red);
    // Diagnostic toggle available: set GE_DEBUG_BLIT_READBACK=1 to use blit+copy path inside the implementation
    cl->CopyTextureSubresourceToBuffer(tex, mip, 0, rb, w, h, 0, 0, 0, 0);
    cl->End();

    dev->ExecuteCommandLists({ cl.get() });
    dev->FinalizeFrame();
    dev->WaitForIdle();

    const uint8_t* data = static_cast<const uint8_t*>(dev->MapBuffer(rb));
    ASSERT_NE(data, nullptr);

    auto idx = [&](uint32_t x, uint32_t y){ return (y*w + x) * 4u; };
    auto expectRed = [&](uint32_t x, uint32_t y){
        auto i = idx(x,y);
        EXPECT_GE(data[i+0], 200) << "R at (" << x << "," << y << ")";
        EXPECT_LE(data[i+1], 80)  << "G at (" << x << "," << y << ")";
        EXPECT_LE(data[i+2], 80)  << "B at (" << x << "," << y << ")";
        EXPECT_GE(data[i+3], 200) << "A at (" << x << "," << y << ")";
    };

    expectRed(0,0);
    expectRed(w/2, h/2);
    expectRed(w-1, h-1);

    dev->UnmapBuffer(rb);

    dev->DestroyBuffer(rb);
    dev->DestroyTexture(tex);
}

