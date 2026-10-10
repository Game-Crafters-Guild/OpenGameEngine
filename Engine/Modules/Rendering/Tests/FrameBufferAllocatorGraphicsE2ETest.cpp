#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/FrameBufferAllocator.h"
#include "Rendering/Common/Utils.h"
#include "TestUtils.h"

using namespace GameEngine::Rendering;

// End-to-end graphics draw that binds a FrameBufferAllocator suballocation as a
// UBO and shades based on its content. Uses fullscreen VS and a fragment shader
// that reads CameraUBO (binding=5, set=0) and outputs green for identity view
// matrix. The bound suballocation sits at a NON-ZERO offset (a leading dummy
// allocation pushes it to the 256-byte alignment boundary) so the offset/size
// arguments of UpdateBufferBinding are load-bearing — an offset-alignment bug
// corrupts the readback instead of passing silently.
TEST(FrameBufferAllocator, GraphicsE2E_UBOReadNonZeroOffset_GreenCenter)
{
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif

    // Device
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true; dd.enableDebugLayer = true;
    auto dev = DeviceFactory::CreateDevice(dd); ASSERT_TRUE(dev && dev->Initialize(dd));

    // Offscreen RT
    const uint32_t W = 64, H = 64;
    TextureDesc td{}; td.width=W; td.height=H; td.format=(uint32_t)TextureFormat::RGBA8_UNORM;
    td.usage=(uint32_t)(TextureUsage::RenderTarget | TextureUsage::ShaderResource | TextureUsage::TransferSrc);
    TextureHandle rt = dev->CreateTexture(td); ASSERT_TRUE(rt.IsValid());

    // Build pipeline (no metadata): fullscreen VS + FS that samples UBO at set0,binding5
    PipelineDesc p{}; p.type = PipelineType::Graphics; p.debugName = "FrameBufferAllocator_UBO_GraphicsE2E";
    p.vertexShader = Utils::LoadShaderFile("fullscreen_noinput.vert.spv");
    p.pixelShader  = Utils::LoadShaderFile("ubo_sample.frag.spv");

    // Descriptor set layout 0: binding 5 = UniformBuffer (VS|FS)
    DescriptorBinding ub{}; ub.binding=5; ub.type=DescriptorType::UniformBuffer; ub.count=1; ub.shaderStages=(0x1|0x10); // VS|FS
    DescriptorSetLayoutDesc set0{}; set0.debugName="Set0_Frame"; set0.bindings.push_back(ub);
    p.descriptorSetLayouts.push_back(set0);

    // Dynamic rendering formats + common post states
    p.colorAttachmentFormats = { (uint32_t)TextureFormat::RGBA8_UNORM };
    p.depthAttachmentFormat = 0;
    p.dynamicState.states.push_back(DynamicState::Viewport);
    p.dynamicState.states.push_back(DynamicState::Scissor);
    p.rasterizationState.cullMode = (uint32_t)CullModeFlagBits::None;
    p.depthStencilState.depthTestEnable = false;

    PipelineHandle pipe = dev->CreatePipeline(p);
    ASSERT_TRUE(pipe.IsValid());

    // Per-frame UBO allocator (uniform usage, 256-byte UBO offset alignment)
    // and the camera UBO (identity V so cam.uV[0][0] == 1).
    FrameBufferAllocator alloc;
    ASSERT_TRUE(alloc.Initialize(dev.get(), 16 * 1024, BufferUsage::Uniform,
                                 dev->GetFramesInFlight(), 256,
                                 "FrameBufferAllocator_E2E"));
    alloc.BeginFrame(0);
    struct CameraUBO { float V[16]; float P[16]; float VP[16]; } cam{};
    auto MakeIdentity = [](float m[16]){ for(int i=0;i<16;++i) m[i] = (i%5==0)?1.0f:0.0f; };
    MakeIdentity(cam.V); MakeIdentity(cam.P); MakeIdentity(cam.VP);
    // Leading dummy allocation pushes the real UBO off offset 0.
    auto dummy = alloc.Allocate(sizeof(CameraUBO));
    ASSERT_TRUE(dummy.IsValid());
    auto a = alloc.Allocate(sizeof(CameraUBO));
    ASSERT_TRUE(a.IsValid());
    ASSERT_TRUE(a.buffer.IsValid());
    ASSERT_NE(a.offset, 0u);
    ASSERT_EQ(a.offset % 256u, 0u);
    std::memcpy(a.ptr, &cam, sizeof(cam));

    // Record draw
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    RenderPassDesc rp{}; rp.colorTargets[0]=rt; rp.colorTargetCount=1;
    // Transition to RenderTarget before starting the pass (validation-clean)
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(rt, ResourceState::Undefined, ResourceState::RenderTarget));
    cl->BeginRenderPass(rp);

    cl->SetPipeline(pipe);
    // transient set that matches the pipeline's set0 layout
    DescriptorSetDesc d{}; d.layout=set0; d.transient=true; d.debugName="Set0_Frame";
    auto ds = dev->CreateDescriptorSet(d);
    dev->UpdateBufferBinding(ds, 5, a.buffer, (uint32_t)a.offset, (uint32_t)a.size);
    cl->BindDescriptorSet(0, ds, pipe);

    cl->Draw(3,1);
    cl->EndRenderPass();
    cl->End();

    std::vector<CommandList*> lists{ cl.get() };
    dev->ExecuteCommandLists(lists);
    dev->WaitForIdle();

    // Readback center texel
    BufferDesc rb{}; rb.size = W*H*4ull; rb.usage=(uint32_t)BufferUsage::TransferDst; rb.memoryUsage=BufferMemoryUsage::Readback;
    BufferHandle readback = dev->CreateBuffer(rb); ASSERT_TRUE(readback.IsValid());
    auto cl2 = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl2->Begin();
    cl2->Barrier(ResourceBarrier::CreateTextureBarrier(rt, ResourceState::RenderTarget, ResourceState::CopySource));
    cl2->CopyTextureToBuffer(rt, readback, W, H);
    cl2->End();
    std::vector<CommandList*> lists2{ cl2.get() };
    dev->ExecuteCommandLists(lists2);
    dev->WaitForIdle();

    const uint8_t* data = static_cast<const uint8_t*>(dev->MapBuffer(readback));
    ASSERT_NE(data, nullptr);
    auto idx = [&](uint32_t x, uint32_t y){ return (y*W + x) * 4u; };
    uint32_t i = idx(W/2, H/2);
    // Expect green (G high, R/B low, A high)
    EXPECT_GE(data[i+1], 200);
    EXPECT_LE(data[i+0], 50);
    EXPECT_LE(data[i+2], 50);
    EXPECT_GE(data[i+3], 200);
    dev->UnmapBuffer(readback);

    dev->DestroyBuffer(readback);
    dev->DestroyTexture(rt);
}
