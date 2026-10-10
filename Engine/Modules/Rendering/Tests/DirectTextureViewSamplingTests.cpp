#include <gtest/gtest.h>
#include <Rendering/Core/Device.h>
#include <Rendering/Core/CommandList.h>
#include <Rendering/Common/Utils.h>
#include "TestUtils.h"

#include "RenderDocShim.h"
#include "../Source/Vulkan/VulkanDevice.h"
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace GameEngine::Rendering;
using namespace TestUtils;

static uint32_t GetPauseMsFromEnv(const char* name) {
    const char* v = std::getenv(name);
    if (!v) return 0u;
    int ms = std::atoi(v);
    return ms > 0 ? static_cast<uint32_t>(ms) : 0u;
}

static void PauseIfRequested(const char* name) {
    uint32_t ms = GetPauseMsFromEnv(name);
    if (ms == 0u) return;
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

TEST(DirectTextureViewSampling, Mip1ViewSamplesRed)
{
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif

    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true; dd.enableDebugLayer = false; dd.enableDescriptorValidation = false;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    // Create source texture (64x64, 2 mips) and destination RT (32x32)
    TextureDesc src{}; src.width=64; src.height=64; src.mipLevels=2; src.arrayLayers=1;
    src.format = (uint32_t)TextureFormat::RGBA8_UNORM;
    src.usage = (uint32_t)(TextureUsage::ShaderResource | TextureUsage::RenderTarget | TextureUsage::TransferDst | TextureUsage::TransferSrc);
    src.debugName = "DirectSrc";
    TextureHandle th = dev->CreateTexture(src);
    ASSERT_TRUE(th.IsValid());

    TextureDesc dst{}; dst.width=32; dst.height=32; dst.mipLevels=1; dst.arrayLayers=1;
    dst.format = (uint32_t)TextureFormat::RGBA8_UNORM;
    dst.usage = (uint32_t)(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    dst.debugName = "DirectDst";
    TextureHandle dth = dev->CreateTexture(dst);
    ASSERT_TRUE(dth.IsValid());

    // Fill mip1 of src with solid red via clear
    auto clFill = dev->CreateCommandList(IDevice::QueueType::Graphics);
    clFill->Begin();
    const float red[4] = {1.f,0.f,0.f,1.f};
    clFill->ClearColorImageSubresource(th, 1, 0, red);
    // Transition mip1 to ShaderResource for sampling
    {
        ResourceBarrier b = ResourceBarrier::CreateTextureBarrier(th, ResourceState::CopyDest, ResourceState::ShaderResource, 1, 1, 0, 1);
        clFill->Barrier(b);
    }
    clFill->End();
    dev->ExecuteCommandLists({ clFill.get() });
    dev->WaitForIdle();

    // Build pipeline: fullscreen + copy.frag
    DescriptorSetLayoutDesc setLayout{};
    setLayout.bindings.push_back({0, DescriptorType::CombinedImageSampler, 1, /*frag*/0x10});

    PipelineDesc pd{}; pd.type=PipelineType::Graphics; pd.debugName="DirectCopy";
    pd.vertexShader = GameEngine::Rendering::Tests::ReadSpirvBytes("fullscreen_noinput.vert.spv");
    pd.pixelShader  = GameEngine::Rendering::Tests::ReadSpirvBytes("copy.frag.spv");
    ASSERT_FALSE(pd.vertexShader.empty()); ASSERT_FALSE(pd.pixelShader.empty());
    pd.EnableDepthTest(false);
    pd.SetCullingMode(CullModeFlagBits::None, FrontFace::CounterClockwise);
    pd.AddDynamicState(DynamicState::Viewport);
    pd.AddDynamicState(DynamicState::Scissor);
    pd.colorAttachmentFormats.push_back((uint32_t)TextureFormat::RGBA8_UNORM);
    pd.descriptorSetLayouts.push_back(setLayout);
    PipelineHandle pipe = dev->CreatePipeline(pd);
    ASSERT_TRUE(pipe.IsValid());

    // Sampler and descriptor set
    SamplerDesc sdef = SamplerDesc::MaterialLinearRepeat("DirectCopySamp");
    SamplerHandle samp = dev->CreateSampler(sdef); ASSERT_TRUE(samp.IsValid());
    DescriptorSetDesc dsd{}; dsd.layout=setLayout; dsd.transient=false; dsd.debugName="DirectCopySet";
    DescriptorSetHandle ds = dev->CreateDescriptorSet(dsd); ASSERT_TRUE(ds.IsValid());

    TextureViewDesc vd{}; vd.aspect=TextureAspect::Color; vd.baseMip=1; vd.levelCount=1;
    TextureViewHandle view = dev->CreateTextureView(th, vd);
    ASSERT_TRUE(view.IsValid());
    dev->UpdateCombinedImageSamplerBinding(ds, 0, view, samp);

    // Optional: pause to allow RenderDoc to attach before rendering
    PauseIfRequested("GE_RENDERDOC_PAUSE_MS");

    // Auto-capture with RenderDoc if requested and available
    RenderDocShim rdoc;
    bool injectedOnly = IsEnvEnabled("GE_RENDERDOC_INJECTED_ONLY");
    bool doCap = IsEnvEnabled("GE_RENDERDOC_AUTOCAPTURE") && rdoc.Init(injectedOnly);
    VulkanDevice* vkd = dynamic_cast<VulkanDevice*>(dev.get());
    void* capDev = vkd ? (void*)vkd->GetVkDevice() : nullptr;

    // If injection arrives late, retry a couple times before giving up
    if (!doCap && injectedOnly) {
        for (int i=0;i<10 && !doCap;i++) {
#ifdef _WIN32
            Sleep(50);
#else
            usleep(50*1000);
#endif
            doCap = rdoc.Init(true);
        }
    }

    if (doCap) rdoc.StartCapture(capDev);

    // Render: sample view into dst
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    RenderPassDesc rp{}; rp.colorTargets[0]=dth; rp.colorTargetCount=1; rp.clearColor[0]=true; // clear to black first
    cl->BeginRenderPass(rp);
    cl->SetPipeline(pipe);
    cl->SetViewport(0,0,(float)dst.width,(float)dst.height);
    cl->SetScissor(0,0,dst.width,dst.height);
    cl->BindDescriptorSet(0, ds, pipe);

    // Optional: pause right before the draw for precise capture timing
    PauseIfRequested("GE_RENDERDOC_PAUSE_BEFORE_DRAW_MS");

    cl->Draw(3,1);
    cl->EndRenderPass();

    // Transition dst to CopySource and copy to readback
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(dth, ResourceState::RenderTarget, ResourceState::CopySource));
    const uint32_t rbSize = 32u*32u*4u;
    BufferHandle rb = dev->CreateReadbackBuffer(rbSize, "RB_DirectDst");
    ASSERT_TRUE(rb.IsValid());
    cl->CopyTextureToBuffer(dth, rb, 32, 32);

    cl->End();
    dev->ExecuteCommandLists({ cl.get() });

    if (doCap) rdoc.EndCapture(capDev);

    // Optional: pause after submit to keep the process alive for capture/inspection
    PauseIfRequested("GE_RENDERDOC_PAUSE_AFTER_SUBMIT_MS");

    dev->WaitForIdle();

    // Readback and validate a few pixels
    const uint8_t* data = static_cast<const uint8_t*>(dev->MapBuffer(rb));
    ASSERT_NE(data, nullptr);
    auto idx = [&](uint32_t x, uint32_t y){ return (y*32u + x)*4u; };
    // Corners and center should be red
    for (auto xy : { std::pair<uint32_t,uint32_t>{0,0}, {16,16}, {31,31} }) {
        uint32_t i = idx(xy.first, xy.second);
        EXPECT_GE(data[i+0], 200) << "R at ("<<xy.first<<","<<xy.second<<")";
        EXPECT_LE(data[i+1], 20)  << "G at ("<<xy.first<<","<<xy.second<<")";
        EXPECT_LE(data[i+2], 20)  << "B at ("<<xy.first<<","<<xy.second<<")";
        EXPECT_GE(data[i+3], 200) << "A at ("<<xy.first<<","<<xy.second<<")";
    }

    // Minimal cleanup to keep validation clean
    dev->UnmapBuffer(rb);
    dev->DestroyTextureView(view);
}

