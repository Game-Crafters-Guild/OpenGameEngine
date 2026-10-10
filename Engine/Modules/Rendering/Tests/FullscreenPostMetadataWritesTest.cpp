#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/PipelineBuilder.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include "ScopedEnvVar.h"
#include "TestUtils.h"

using namespace GameEngine::Rendering;

TEST(FullscreenPost, MetadataBuiltPipelineWritesRed)
{
    // Deliberately unscoped: every device in this process wants headless mode.
    GameEngine::Rendering::Tests::SetEnvVar("GE_HEADLESS_TEST", "1");

    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd); ASSERT_TRUE(dev && dev->Initialize(dd));

    // Offscreen RT (RGBA8)
    TextureDesc td{}; td.width = 64; td.height = 64; td.format = (uint32_t)TextureFormat::RGBA8_UNORM;
    td.usage = (uint32_t)(TextureUsage::RenderTarget | TextureUsage::ShaderResource | TextureUsage::TransferSrc);
    TextureHandle rt = dev->CreateTexture(td);
    ASSERT_NE(rt, INVALID_HANDLE);

    // Build pipeline via metadata with fullscreen post profile
    MaterialBuilder::FormatsHint fmt{}; fmt.ColorFormats = { (uint32_t)TextureFormat::RGBA8_UNORM };
    PipelineBuilder pb{}; std::string err;
    {
        ShaderPackage pkg;
        // Bare name: TestUtils.h installs the resolver that finds it under the
        // build's shader output dir. An absolute path built from the working
        // directory would tie the result to whatever cwd the runner chose.
        ASSERT_TRUE(LoadShaderPkg("solidcolor.shaderpkg", ShaderSourceKind::SpirV, pkg, &err)) << err;
        ASSERT_TRUE(pb.ApplyMeta(pkg.meta, fmt, MaterialBuilder::MergeMode::Auto, {}, &err)) << err;
    }
    pb.ProfileFullscreenPost({ (uint32_t)TextureFormat::RGBA8_UNORM })
      .SetType(PipelineType::Graphics)
      .SetDebugName("TestSolidColor_FullscreenPost");
    // Ensure VS/FS are present (desc provides both, but be explicit)
    pb.SetVertexShader(GameEngine::Rendering::Tests::ReadSpirvBytes("fullscreen_noinput.vert.spv"));
    pb.SetPixelShader(GameEngine::Rendering::Tests::ReadSpirvBytes("solidcolor.frag.spv"));

    PipelineHandle pipe = pb.Build(dev.get());
    ASSERT_TRUE(pipe.IsValid());

    // Record draw
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();

    RenderPassDesc rp{}; rp.colorTargets[0] = rt; rp.colorTargetCount = 1;
    cl->BeginRenderPass(rp);

    cl->SetPipeline(pipe);
    // Viewport/scissor are dynamic and auto-set by backend on begin; ok to rely on defaults
    cl->Draw(3, 1);

    cl->EndRenderPass();
    cl->End();

    std::vector<CommandList*> lists{ cl.get() };
    dev->ExecuteCommandLists(lists);
    dev->WaitForIdle();

    // Readback
    BufferDesc rb{}; rb.size = 64ull * 64ull * 4ull; rb.usage = (uint32_t)BufferUsage::TransferDst; rb.memoryUsage = BufferMemoryUsage::Readback;
    BufferHandle readback = dev->CreateBuffer(rb); ASSERT_NE(readback, INVALID_HANDLE);

    auto cl2 = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl2->Begin();
    cl2->CopyTextureToBuffer(rt, readback, 64, 64);
    cl2->End();
    std::vector<CommandList*> copyLists{ cl2.get() };
    dev->ExecuteCommandLists(copyLists);
    dev->WaitForIdle();

    const uint8_t* data = static_cast<const uint8_t*>(dev->MapBuffer(readback));
    ASSERT_NE(data, nullptr);

    auto idx = [&](uint32_t x, uint32_t y){ return (y*64u + x) * 4u; };
    // Expect solid red (from shader) at center
    uint32_t i = idx(32, 32);
    EXPECT_GE(data[i+0], 200); // R
    EXPECT_LE(data[i+1], 50);  // G
    EXPECT_LE(data[i+2], 50);  // B
    EXPECT_GE(data[i+3], 200); // A

    dev->UnmapBuffer(readback);

    dev->DestroyBuffer(readback);
    dev->DestroyTexture(rt);
}

