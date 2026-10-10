#include <gtest/gtest.h>
#include <Rendering/Core/Device.h>
#include <Rendering/Core/CommandList.h>
#include "Rendering/Common/Utils.h"
#include "TestUtils.h"


using namespace GameEngine::Rendering;

TEST(DynamicRenderingPipelineFormats, OffscreenRGBA8WritesRed)
{
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif

    // Create device with dynamic rendering enabled (no validation layers required)
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    dd.enableDebugLayer = false;
    dd.enableDescriptorValidation = false;

    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    if (!dev->Initialize(dd)) {
        GTEST_SKIP() << "Device init failed";
    }

    // Create a 1x1 source texture filled solid red
    TextureDesc src{}; src.width = 1; src.height = 1; src.mipLevels = 1; src.arrayLayers = 1;
    src.format = (uint32_t)TextureFormat::RGBA8_UNORM;
    src.usage = (uint32_t)(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    src.debugName = "DynDR_Src1x1";
    TextureHandle th = dev->CreateTexture(src);
    ASSERT_TRUE(th.IsValid());

    {
        auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        const float red[4] = {1.f, 0.f, 0.f, 1.f};
        cl->ClearColorImageSubresource(th, /*mip*/0, /*layer*/0, red);
        // Transition to sample from
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(
            th, ResourceState::CopyDest, ResourceState::ShaderResource, 0, 1, 0, 1));
        cl->End();
        dev->ExecuteCommandLists({ cl.get() });
        dev->WaitForIdle();
    }

    // Create 16x16 destination render target
    TextureDesc dst{}; dst.width = 16; dst.height = 16; dst.mipLevels = 1; dst.arrayLayers = 1;
    dst.format = (uint32_t)TextureFormat::RGBA8_UNORM;
    dst.usage = (uint32_t)(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    dst.debugName = "DynDR_Dst16x16";
    TextureHandle dth = dev->CreateTexture(dst);
    ASSERT_TRUE(dth.IsValid());

    // Build pipeline (dynamic rendering, explicit color attachment format RGBA8)
    DescriptorSetLayoutDesc setLayout{};
    setLayout.bindings.push_back({0, DescriptorType::CombinedImageSampler, 1, /*frag*/0x10});

    PipelineDesc pd{}; pd.type = PipelineType::Graphics; pd.debugName = "DynDR_Copy";
    pd.vertexShader = GameEngine::Rendering::Tests::ReadSpirvBytes("fullscreen_noinput.vert.spv");
    pd.pixelShader  = GameEngine::Rendering::Tests::ReadSpirvBytes("copy.frag.spv");
    ASSERT_FALSE(pd.vertexShader.empty());
    ASSERT_FALSE(pd.pixelShader.empty());
    pd.EnableDepthTest(false);
    pd.AddDynamicState(DynamicState::Viewport);
    pd.AddDynamicState(DynamicState::Scissor);
    pd.colorAttachmentFormats.push_back((uint32_t)TextureFormat::RGBA8_UNORM);
    pd.descriptorSetLayouts.push_back(setLayout);

    PipelineHandle pipe = dev->CreatePipeline(pd);
    ASSERT_TRUE(pipe.IsValid());

    // Sampler + descriptor set
    SamplerDesc sdef = SamplerDesc::MaterialLinearRepeat("DynDR_Samp");
    SamplerHandle samp = dev->CreateSampler(sdef);
    ASSERT_TRUE(samp.IsValid());

    DescriptorSetDesc dsd{}; dsd.layout = setLayout; dsd.transient = false; dsd.debugName = "DynDR_Set";
    DescriptorSetHandle ds = dev->CreateDescriptorSet(dsd);
    ASSERT_TRUE(ds.IsValid());

    dev->UpdateCombinedImageSamplerBinding(ds, 0, th, samp);

    // Render: sample src into dst
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    RenderPassDesc rp{}; rp.colorTargets[0] = dth; rp.colorTargetCount = 1; rp.clearColor[0] = true; // clear to black first
    cl->BeginRenderPass(rp);
    cl->SetPipeline(pipe);
    cl->SetViewport(0, 0, (float)dst.width, (float)dst.height);
    cl->SetScissor(0, 0, dst.width, dst.height);
    cl->BindDescriptorSet(0, ds, pipe);
    cl->Draw(3, 1);
    cl->EndRenderPass();

    // Prepare readback
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(dth, ResourceState::RenderTarget, ResourceState::CopySource));
    const uint32_t rbSize = dst.width * dst.height * 4u;
    BufferHandle rb = dev->CreateReadbackBuffer(rbSize, "DynDR_RB");
    ASSERT_TRUE(rb.IsValid());
    cl->CopyTextureToBuffer(dth, rb, dst.width, dst.height);
    cl->End();

    dev->ExecuteCommandLists({ cl.get() });
    dev->WaitForIdle();

    // Validate pixels are red (proves pipeline wrote to color 0 under dynamic rendering)
    const uint8_t* data = static_cast<const uint8_t*>(dev->MapBuffer(rb));
    ASSERT_NE(data, nullptr);
    auto idx = [&](uint32_t x, uint32_t y){ return (y*dst.width + x)*4u; };
    for (auto xy : { std::pair<uint32_t,uint32_t>{0,0}, {8,8}, {15,15} }) {
        uint32_t i = idx(xy.first, xy.second);
        EXPECT_GE(data[i+0], 200) << "R at ("<<xy.first<<","<<xy.second<<")";
        EXPECT_LE(data[i+1], 20)  << "G at ("<<xy.first<<","<<xy.second<<")";
        EXPECT_LE(data[i+2], 20)  << "B at ("<<xy.first<<","<<xy.second<<")";
        EXPECT_GE(data[i+3], 200) << "A at ("<<xy.first<<","<<xy.second<<")";
    }

    // Cleanup to keep validation clean (even if disabled here)
    dev->UnmapBuffer(rb);
    dev->DestroyDescriptorSet(ds);
    dev->DestroySampler(samp);
    dev->DestroyPipeline(pipe);
    dev->DestroyBuffer(rb);
    dev->DestroyTexture(dth);
    dev->DestroyTexture(th);
}



#if GTEST_HAS_DEATH_TEST
TEST(DynamicRenderingPipelineFormats, MismatchedFormatsDeath)
{
#ifndef NDEBUG
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif

    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true; dd.enableDebugLayer = false; dd.enableDescriptorValidation = false;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    if (!dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    // Build pipeline expecting RGBA8
    DescriptorSetLayoutDesc setLayout{};
    PipelineDesc pd{}; pd.type = PipelineType::Graphics; pd.debugName = "DynDR_Mismatch";
    pd.vertexShader = GameEngine::Rendering::Tests::ReadSpirvBytes("fullscreen_noinput.vert.spv");
    pd.pixelShader  = GameEngine::Rendering::Tests::ReadSpirvBytes("copy.frag.spv");
    ASSERT_FALSE(pd.vertexShader.empty()); ASSERT_FALSE(pd.pixelShader.empty());
    pd.EnableDepthTest(false);
    pd.AddDynamicState(DynamicState::Viewport);
    pd.AddDynamicState(DynamicState::Scissor);
    pd.colorAttachmentFormats.push_back((uint32_t)TextureFormat::RGBA8_UNORM);
    pd.descriptorSetLayouts.push_back(setLayout);

    PipelineHandle pipe = dev->CreatePipeline(pd);
    ASSERT_TRUE(pipe.IsValid());

    ASSERT_DEATH({
        auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        TextureDesc dst{}; dst.width = 8; dst.height = 8; dst.mipLevels = 1; dst.arrayLayers = 1;
        dst.format = (uint32_t)TextureFormat::RGBA8_SRGB; // Mismatch with pipeline below
        dst.usage = (uint32_t)(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
        TextureHandle dth_local = dev->CreateTexture(dst);
        RenderPassDesc rp{}; rp.colorTargets[0] = dth_local; rp.colorTargetCount = 1; rp.clearColor[0] = true;
        cl->BeginRenderPass(rp);
        cl->SetPipeline(pipe); // Should trigger assert in debug due to format mismatch
    }, ".*");

    // Cleanup
    dev->DestroyPipeline(pipe);
#else
    GTEST_SKIP() << "Death test only runs in Debug builds";
#endif
}
#endif // GTEST_HAS_DEATH_TEST

#if GTEST_HAS_DEATH_TEST
TEST(DynamicRenderingPipelineFormats, MismatchedAttachmentCountDeath)
{
#ifndef NDEBUG
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif

    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true; dd.enableDebugLayer = false; dd.enableDescriptorValidation = false;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    if (!dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    // Build pipeline expecting TWO color attachments
    PipelineDesc pd{}; pd.type = PipelineType::Graphics; pd.debugName = "DynDR_CountMismatch";
    pd.vertexShader = GameEngine::Rendering::Tests::ReadSpirvBytes("fullscreen_noinput.vert.spv");
    pd.pixelShader  = GameEngine::Rendering::Tests::ReadSpirvBytes("copy.frag.spv");
    ASSERT_FALSE(pd.vertexShader.empty()); ASSERT_FALSE(pd.pixelShader.empty());
    pd.EnableDepthTest(false);
    pd.AddDynamicState(DynamicState::Viewport);
    pd.AddDynamicState(DynamicState::Scissor);
    pd.colorAttachmentFormats.push_back((uint32_t)TextureFormat::RGBA8_UNORM);
    pd.colorAttachmentFormats.push_back((uint32_t)TextureFormat::RGBA8_UNORM);

    PipelineHandle pipe = dev->CreatePipeline(pd);
    ASSERT_TRUE(pipe.IsValid());

    ASSERT_DEATH({
        auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        TextureDesc dst{}; dst.width = 8; dst.height = 8; dst.mipLevels = 1; dst.arrayLayers = 1;
        dst.format = (uint32_t)TextureFormat::RGBA8_UNORM;
        dst.usage = (uint32_t)(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
        TextureHandle dth_local = dev->CreateTexture(dst);
        RenderPassDesc rp{}; rp.colorTargets[0] = dth_local; rp.colorTargetCount = 1; rp.clearColor[0] = true;
        cl->BeginRenderPass(rp);
        cl->SetPipeline(pipe); // Should trigger assert in debug due to COUNT mismatch
    }, ".*");

    // Cleanup
    dev->DestroyPipeline(pipe);
#else
    GTEST_SKIP() << "Death test only runs in Debug builds";
#endif
}
#endif // GTEST_HAS_DEATH_TEST



TEST(DynamicRenderingPipelineFormats, MissingFormats_FailFast)
{
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif

    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true; dd.enableDebugLayer = false; dd.enableDescriptorValidation = false;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    if (!dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    // Build a graphics pipeline with dynamic rendering but without explicit formats
    PipelineDesc pd{}; pd.type = PipelineType::Graphics; pd.debugName = "DynDR_MissingFormats";
    pd.vertexShader = GameEngine::Rendering::Tests::ReadSpirvBytes("fullscreen_noinput.vert.spv");
    pd.pixelShader  = GameEngine::Rendering::Tests::ReadSpirvBytes("copy.frag.spv");
    ASSERT_FALSE(pd.vertexShader.empty()); ASSERT_FALSE(pd.pixelShader.empty());
    pd.EnableDepthTest(false);
    pd.AddDynamicState(DynamicState::Viewport);
    pd.AddDynamicState(DynamicState::Scissor);
    // Intentionally leave pd.colorAttachmentFormats empty and depthAttachmentFormat = 0

    PipelineHandle pipe = dev->CreatePipeline(pd);
    EXPECT_FALSE(pipe.IsValid()) << "Pipeline should fail to create when dynamic rendering is enabled and no formats are provided";
}
