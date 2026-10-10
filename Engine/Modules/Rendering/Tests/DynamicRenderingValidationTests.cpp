#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Common/Utils.h"
#include "TestUtils.h"


using namespace GameEngine::Rendering;

// Ensure dynamic rendering runtime validation does not crash and surfaces mismatches gracefully
TEST(DynamicRenderingValidation, Graphics_PipelineFormatMismatch_DoesNotCrash)
{
#if defined(_WIN32)
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true; dd.enableDebugLayer = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    // Offscreen color target: RGBA8
    TextureDesc td{}; td.width=32; td.height=32; td.format=(uint32_t)TextureFormat::RGBA8_UNORM; td.usage=(uint32_t)(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    TextureHandle rt = dev->CreateTexture(td);
    ASSERT_TRUE(rt.IsValid());

    // Pipeline intentionally declares a different color attachment format (RGBA16F)
    auto vs = GameEngine::Rendering::Tests::ReadSpirvBytes("fullscreen_noinput.vert.spv");
    auto fs = GameEngine::Rendering::Tests::ReadSpirvBytes("solidcolor.frag.spv");
    ASSERT_FALSE(vs.empty()); ASSERT_FALSE(fs.empty());

    PipelineDesc pso{}; pso.type = PipelineType::Graphics; pso.vertexShader = vs; pso.pixelShader = fs;
    pso.colorAttachmentFormats.push_back((uint32_t)TextureFormat::R16G16B16A16_FLOAT);
    pso.AddDynamicState(DynamicState::Viewport); pso.AddDynamicState(DynamicState::Scissor);

    PipelineHandle pipe = dev->CreatePipeline(pso);
    ASSERT_TRUE(pipe.IsValid());

    ASSERT_TRUE(dev->BeginFrame());
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();

    // Begin dynamic rendering with RGBA8 target
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(rt, ResourceState::Undefined, ResourceState::RenderTarget));
    RenderPassDesc rp{}; rp.colorTargets[0]=rt; rp.colorTargetCount=1; rp.clearColor[0]=true; rp.colorStoreOp[0]=RenderPassDesc::StoreOp::Store;
    cl->BeginRenderPass(rp);
    cl->SetViewport(0,0,32,32); cl->SetScissor(0,0,32,32);

    // Bind mismatched pipeline and draw.
    // Debug builds assert; release builds log an error and continue.
#ifndef NDEBUG
    ASSERT_DEATH({
        cl->SetPipeline(pipe);
        cl->Draw(3,1);
    }, "DynamicRendering format mismatch");
#else
    cl->SetPipeline(pipe);
    cl->Draw(3,1);
#endif

    cl->EndRenderPass();
    cl->End();

    dev->ExecuteCommandLists(std::vector<CommandList*>{ cl.get() });
    dev->FinalizeFrame();

    // If we reached here without device loss or crash, validation path is robust
    dev->DestroyTexture(rt);
}

