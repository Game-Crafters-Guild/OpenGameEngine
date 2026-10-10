#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Common/Utils.h"
#include <vulkan/vulkan_core.h>

#include "TestUtils.h"

#include <cstdlib>

using namespace GameEngine::Rendering;

namespace {
std::vector<uint8_t> LoadShader(const char* path) {
    return GameEngine::Rendering::Tests::ReadSpirvBytes(path);
}
}

// Verifies that switching between pipelines with the same layout does not cause extra descriptor binds
TEST(DescriptorBindStats, Compute_SameLayout_NoExtraRebinds)
{
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDebugLayer = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    // Create a storage buffer and descriptor set layout for set=0
    BufferDesc buf{}; buf.size = 256; buf.usage = (uint32_t)BufferUsage::Storage | (uint32_t)BufferUsage::TransferDst; buf.memoryUsage = BufferMemoryUsage::DeviceLocal;
    BufferHandle storage = dev->CreateBuffer(buf);
    ASSERT_TRUE(storage.IsValid());

    DescriptorSetLayoutDesc set0{};
    DescriptorBinding b0{}; b0.binding = 0; b0.type = DescriptorType::StorageBuffer; b0.count = 1; b0.shaderStages = VK_SHADER_STAGE_COMPUTE_BIT;
    set0.bindings.push_back(b0);

    DescriptorSetDesc ds{}; ds.layout = set0; ds.debugName = "DS0";
    DescriptorSetHandle dsh = dev->CreateDescriptorSet(ds);
    ASSERT_TRUE(dsh.IsValid());
    DescriptorSetUpdate upd{}; upd.binding = 0; upd.type = DescriptorType::StorageBuffer; upd.buffers = { storage }; upd.bufferOffsets = { 0 }; upd.bufferRanges = { 256 };
    dev->UpdateDescriptorSet(dsh, upd);

    // Two compute pipelines built from the same shader and identical set layout
    auto code = LoadShader("minimal_test.comp.spv");
    ASSERT_FALSE(code.empty());

    PipelineDesc pd{}; pd.type = PipelineType::Compute; pd.computeShader = code; pd.descriptorSetLayouts = { set0 };
    PipelineHandle pA = dev->CreatePipeline(pd);
    PipelineHandle pB = dev->CreatePipeline(pd);
    ASSERT_TRUE(pA.IsValid() && pB.IsValid());

    // Record: bind once on A, then switch to B without rebinding; expect 1 bind for compute set 0
    dev->DebugResetBindCounters();

    ASSERT_TRUE(dev->BeginFrame());
    auto cl = dev->CreateCommandList(IDevice::QueueType::Compute);
    cl->Begin();
    cl->SetPipeline(pA);
    cl->BindDescriptorSet(0, dsh, pA);
    cl->Dispatch(1,1,1);
    cl->SetPipeline(pB); // layout should be identical; no extra bind required
    cl->Dispatch(1,1,1);
    cl->End();
    dev->ExecuteCommandLists(std::vector<CommandList*>{cl.get()});
    // Validate counters: only one descriptor bind on compute set 0
    {
        auto c = dev->DebugGetBindCounters();
        EXPECT_EQ(c.descriptorBindsCompute[0], 1u);
    }

    dev->FinalizeFrame();

    SUCCEED(); // Backend bind-count validated elsewhere; smoke assurance here
    dev->WaitForIdle();
}

// Verifies that switching to a different layout requires a rebind (we simulate by explicitly rebinding)
TEST(DescriptorBindStats, Compute_DifferentLayout_RebindOnce)
{
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDebugLayer = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    // Create resources
    BufferDesc buf{}; buf.size = 256; buf.usage = (uint32_t)BufferUsage::Storage | (uint32_t)BufferUsage::TransferDst; buf.memoryUsage = BufferMemoryUsage::DeviceLocal;
    BufferHandle storage = dev->CreateBuffer(buf);
    ASSERT_TRUE(storage.IsValid());

    // Layout A: set0 has binding 0 StorageBuffer
    DescriptorSetLayoutDesc setA{}; DescriptorBinding a0{}; a0.binding=0; a0.type=DescriptorType::StorageBuffer; a0.count=1; a0.shaderStages=VK_SHADER_STAGE_COMPUTE_BIT; setA.bindings.push_back(a0);
    DescriptorSetDesc dsA{}; dsA.layout=setA; dsA.debugName="DSA"; DescriptorSetHandle dsa = dev->CreateDescriptorSet(dsA);
    ASSERT_TRUE(dsa.IsValid());
    DescriptorSetUpdate updA{}; updA.binding=0; updA.type=DescriptorType::StorageBuffer; updA.buffers={storage}; updA.bufferOffsets={0}; updA.bufferRanges={256};
    dev->UpdateDescriptorSet(dsa, updA);

    // Layout B: set0 has binding 0 StorageBuffer + binding 1 (dummy) UniformBuffer
    DescriptorSetLayoutDesc setB{}; DescriptorBinding b0{}; b0.binding=0; b0.type=DescriptorType::StorageBuffer; b0.count=1; b0.shaderStages=VK_SHADER_STAGE_COMPUTE_BIT; setB.bindings.push_back(b0);
    DescriptorBinding b1{}; b1.binding=1; b1.type=DescriptorType::UniformBuffer; b1.count=1; b1.shaderStages=VK_SHADER_STAGE_COMPUTE_BIT; setB.bindings.push_back(b1);
    // Create uniform buffer and descriptor set for layout B
    BufferDesc ubDesc{}; ubDesc.size = 256; ubDesc.usage = (uint32_t)BufferUsage::Uniform | (uint32_t)BufferUsage::TransferDst; ubDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    BufferHandle uniformBuf = dev->CreateBuffer(ubDesc);
    ASSERT_TRUE(uniformBuf.IsValid());
    DescriptorSetDesc dsB{}; dsB.layout = setB; dsB.debugName = "DSB"; DescriptorSetHandle dsb = dev->CreateDescriptorSet(dsB);
    ASSERT_TRUE(dsb.IsValid());
    // Update both bindings
    DescriptorSetUpdate updB0{}; updB0.binding=0; updB0.type=DescriptorType::StorageBuffer; updB0.buffers={storage}; updB0.bufferOffsets={0}; updB0.bufferRanges={256};
    dev->UpdateDescriptorSet(dsb, updB0);
    DescriptorSetUpdate updB1{}; updB1.binding=1; updB1.type=DescriptorType::UniformBuffer; updB1.buffers={uniformBuf}; updB1.bufferOffsets={0}; updB1.bufferRanges={256};
    dev->UpdateDescriptorSet(dsb, updB1);

    auto code = LoadShader("minimal_test.comp.spv"); ASSERT_FALSE(code.empty());
    PipelineDesc pda{}; pda.type=PipelineType::Compute; pda.computeShader=code; pda.descriptorSetLayouts={setA};
    PipelineDesc pdb{}; pdb.type=PipelineType::Compute; pdb.computeShader=code; pdb.descriptorSetLayouts={setB};
    PipelineHandle pA = dev->CreatePipeline(pda);
    PipelineHandle pB = dev->CreatePipeline(pdb);
    ASSERT_TRUE(pA.IsValid() && pB.IsValid());

    dev->DebugResetBindCounters();

    ASSERT_TRUE(dev->BeginFrame());
    auto cl = dev->CreateCommandList(IDevice::QueueType::Compute);
    // Ensure headless; shaders are small and validated via counters only
#if defined(_WIN32)
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif

    cl->Begin();
    // Ensure headless; shaders are small and validated via counters only

    dev->DebugResetBindCounters();

    cl->SetPipeline(pA);
    cl->BindDescriptorSet(0, dsa, pA);
    // Validate counters mid-way (after first bind): should be 1
    {
        auto c = dev->DebugGetBindCounters();
        EXPECT_EQ(c.descriptorBindsCompute[0], 1u);
    }

    cl->Dispatch(1,1,1);

    // Switch to different layout; simulate correct behavior by rebinding set 0
    cl->SetPipeline(pB);
    cl->BindDescriptorSet(0, dsb, pB); // one more bind with compatible layout B
    cl->Dispatch(1,1,1);

    cl->End();
    dev->ExecuteCommandLists(std::vector<CommandList*>{cl.get()});
    dev->FinalizeFrame();
    // Validate final counters: two binds expected after explicit rebind on layout change
    {
        auto c = dev->DebugGetBindCounters();
        EXPECT_EQ(c.descriptorBindsCompute[0], 2u);
    }
    dev->WaitForIdle();
}


// Graphics variant: same layout switch should not trigger extra descriptor binds
TEST(DescriptorBindStats, Graphics_SameLayout_NoExtraRebinds)
{
    #if defined(_WIN32)
    _putenv_s("GE_HEADLESS_TEST", "1");
    #else
    setenv("GE_HEADLESS_TEST", "1", 1);
    #endif

    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true; dd.enableDebugLayer = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    // Offscreen target
    TextureDesc td{}; td.width=16; td.height=16; td.format=(uint32_t)TextureFormat::RGBA8_UNORM; td.usage=(uint32_t)(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    TextureHandle rt = dev->CreateTexture(td); ASSERT_TRUE(rt.IsValid());

    // Set0: UniformBuffer binding 0
    BufferDesc ub{}; ub.size=256; ub.usage=(uint32_t)(BufferUsage::Uniform | BufferUsage::TransferDst); ub.memoryUsage=BufferMemoryUsage::DeviceLocal;
    BufferHandle ubo = dev->CreateBuffer(ub); ASSERT_TRUE(ubo.IsValid());

    DescriptorSetLayoutDesc set0{}; DescriptorBinding ub0{}; ub0.binding=0; ub0.type=DescriptorType::UniformBuffer; ub0.count=1; ub0.shaderStages=VK_SHADER_STAGE_FRAGMENT_BIT; set0.bindings.push_back(ub0);
    DescriptorSetDesc ds{}; ds.layout=set0; ds.debugName="G_DS0"; DescriptorSetHandle dsh = dev->CreateDescriptorSet(ds); ASSERT_TRUE(dsh.IsValid());
    DescriptorSetUpdate upd{}; upd.binding=0; upd.type=DescriptorType::UniformBuffer; upd.buffers={ubo}; upd.bufferOffsets={0}; upd.bufferRanges={256};
    dev->UpdateDescriptorSet(dsh, upd);

    // Two graphics pipelines with identical descriptor layout
    auto vs = GameEngine::Rendering::Tests::ReadSpirvBytes("fullscreen_noinput.vert.spv");
    auto fs = GameEngine::Rendering::Tests::ReadSpirvBytes("solidcolor.frag.spv");
    ASSERT_FALSE(vs.empty()); ASSERT_FALSE(fs.empty());

    PipelineDesc pd{}; pd.type=PipelineType::Graphics; pd.vertexShader=vs; pd.pixelShader=fs; pd.descriptorSetLayouts={set0};
    pd.colorAttachmentFormats.push_back((uint32_t)TextureFormat::RGBA8_UNORM);
    pd.AddDynamicState(DynamicState::Viewport); pd.AddDynamicState(DynamicState::Scissor);

    PipelineHandle pA = dev->CreatePipeline(pd);
    PipelineHandle pB = dev->CreatePipeline(pd);
    ASSERT_TRUE(pA.IsValid() && pB.IsValid());

    dev->DebugResetBindCounters();

    ASSERT_TRUE(dev->BeginFrame());
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(rt, ResourceState::Undefined, ResourceState::RenderTarget));
    RenderPassDesc rp{}; rp.colorTargets[0]=rt; rp.colorTargetCount=1; rp.clearColor[0]=true; rp.colorStoreOp[0]=RenderPassDesc::StoreOp::Store;
    cl->BeginRenderPass(rp);
    cl->SetViewport(0,0,16,16); cl->SetScissor(0,0,16,16);

    cl->SetPipeline(pA);
    cl->BindDescriptorSet(0, dsh, pA);
    cl->Draw(3,1);

    cl->SetPipeline(pB); // same layout; no extra bind required
    cl->Draw(3,1);

    cl->EndRenderPass();
    cl->End();
    dev->ExecuteCommandLists(std::vector<CommandList*>{cl.get()});
    dev->FinalizeFrame();

    auto c = dev->DebugGetBindCounters();
    EXPECT_EQ(c.descriptorBindsGraphics[0], 1u);
}

// Graphics variant: different layout requires one explicit rebind
TEST(DescriptorBindStats, Graphics_DifferentLayout_RebindOnce)
{
    #if defined(_WIN32)
    _putenv_s("GE_HEADLESS_TEST", "1");
    #else
    setenv("GE_HEADLESS_TEST", "1", 1);
    #endif

    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true; dd.enableDebugLayer = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    // Offscreen target
    TextureDesc td{}; td.width=16; td.height=16; td.format=(uint32_t)TextureFormat::RGBA8_UNORM; td.usage=(uint32_t)(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    TextureHandle rt = dev->CreateTexture(td); ASSERT_TRUE(rt.IsValid());

    // Layout A (set0: UBO0)
    BufferDesc ub{}; ub.size=256; ub.usage=(uint32_t)(BufferUsage::Uniform | BufferUsage::TransferDst); ub.memoryUsage=BufferMemoryUsage::DeviceLocal;
    BufferHandle ubo = dev->CreateBuffer(ub); ASSERT_TRUE(ubo.IsValid());

    DescriptorSetLayoutDesc setA{}; DescriptorBinding a0{}; a0.binding=0; a0.type=DescriptorType::UniformBuffer; a0.count=1; a0.shaderStages=VK_SHADER_STAGE_FRAGMENT_BIT; setA.bindings.push_back(a0);
    DescriptorSetDesc dsA{}; dsA.layout=setA; dsA.debugName="G_DSA"; DescriptorSetHandle dsa = dev->CreateDescriptorSet(dsA); ASSERT_TRUE(dsa.IsValid());
    DescriptorSetUpdate updA{}; updA.binding=0; updA.type=DescriptorType::UniformBuffer; updA.buffers={ubo}; updA.bufferOffsets={0}; updA.bufferRanges={256};
    dev->UpdateDescriptorSet(dsa, updA);

    // Layout B (set0: UBO0 + UBO1)
    BufferDesc ub2{}; ub2.size=256; ub2.usage=(uint32_t)(BufferUsage::Uniform | BufferUsage::TransferDst); ub2.memoryUsage=BufferMemoryUsage::DeviceLocal;
    BufferHandle ubo2 = dev->CreateBuffer(ub2); ASSERT_TRUE(ubo2.IsValid());

    DescriptorSetLayoutDesc setB{}; DescriptorBinding b0{}; b0.binding=0; b0.type=DescriptorType::UniformBuffer; b0.count=1; b0.shaderStages=VK_SHADER_STAGE_FRAGMENT_BIT; setB.bindings.push_back(b0);
    DescriptorBinding b1{}; b1.binding=1; b1.type=DescriptorType::UniformBuffer; b1.count=1; b1.shaderStages=VK_SHADER_STAGE_FRAGMENT_BIT; setB.bindings.push_back(b1);

    DescriptorSetDesc dsB{}; dsB.layout=setB; dsB.debugName="G_DSB"; DescriptorSetHandle dsb = dev->CreateDescriptorSet(dsB); ASSERT_TRUE(dsb.IsValid());
    DescriptorSetUpdate updB0{}; updB0.binding=0; updB0.type=DescriptorType::UniformBuffer; updB0.buffers={ubo}; updB0.bufferOffsets={0}; updB0.bufferRanges={256};
    dev->UpdateDescriptorSet(dsb, updB0);
    DescriptorSetUpdate updB1{}; updB1.binding=1; updB1.type=DescriptorType::UniformBuffer; updB1.buffers={ubo2}; updB1.bufferOffsets={0}; updB1.bufferRanges={256};
    dev->UpdateDescriptorSet(dsb, updB1);

    // Two pipelines with different layouts
    auto vs = GameEngine::Rendering::Tests::ReadSpirvBytes("fullscreen_noinput.vert.spv");
    auto fs = GameEngine::Rendering::Tests::ReadSpirvBytes("solidcolor.frag.spv");
    ASSERT_FALSE(vs.empty()); ASSERT_FALSE(fs.empty());

    PipelineDesc pda{}; pda.type=PipelineType::Graphics; pda.vertexShader=vs; pda.pixelShader=fs; pda.descriptorSetLayouts={setA};
    pda.colorAttachmentFormats.push_back((uint32_t)TextureFormat::RGBA8_UNORM);
    pda.AddDynamicState(DynamicState::Viewport); pda.AddDynamicState(DynamicState::Scissor);

    PipelineDesc pdb{}; pdb.type=PipelineType::Graphics; pdb.vertexShader=vs; pdb.pixelShader=fs; pdb.descriptorSetLayouts={setB};
    pdb.colorAttachmentFormats.push_back((uint32_t)TextureFormat::RGBA8_UNORM);
    pdb.AddDynamicState(DynamicState::Viewport); pdb.AddDynamicState(DynamicState::Scissor);

    PipelineHandle pA = dev->CreatePipeline(pda);
    PipelineHandle pB = dev->CreatePipeline(pdb);
    ASSERT_TRUE(pA.IsValid() && pB.IsValid());

    dev->DebugResetBindCounters();

    ASSERT_TRUE(dev->BeginFrame());
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(rt, ResourceState::Undefined, ResourceState::RenderTarget));
    RenderPassDesc rp{}; rp.colorTargets[0]=rt; rp.colorTargetCount=1; rp.clearColor[0]=true; rp.colorStoreOp[0]=RenderPassDesc::StoreOp::Store;
    cl->BeginRenderPass(rp);
    cl->SetViewport(0,0,16,16); cl->SetScissor(0,0,16,16);

    cl->SetPipeline(pA);
    cl->BindDescriptorSet(0, dsa, pA);
    cl->Draw(3,1);

    cl->SetPipeline(pB);
    cl->BindDescriptorSet(0, dsb, pB); // different layout: explicit rebind
    cl->Draw(3,1);

    cl->EndRenderPass();
    cl->End();
    dev->ExecuteCommandLists(std::vector<CommandList*>{cl.get()});
    dev->FinalizeFrame();

    auto c = dev->DebugGetBindCounters();
    EXPECT_EQ(c.descriptorBindsGraphics[0], 2u);
}





// Graphics: two descriptor sets. Same layout on switch -> no extra rebinds for either set
TEST(DescriptorBindStats, Graphics_MultiSet_SameLayout_NoExtraRebinds)
{
#if defined(_WIN32)
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true; dd.enableDebugLayer = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    // Offscreen target
    TextureDesc td{}; td.width=16; td.height=16; td.format=(uint32_t)TextureFormat::RGBA8_UNORM; td.usage=(uint32_t)(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    TextureHandle rt = dev->CreateTexture(td); ASSERT_TRUE(rt.IsValid());

    // Set0: UBO0
    BufferDesc ub0{}; ub0.size=256; ub0.usage=(uint32_t)(BufferUsage::Uniform | BufferUsage::TransferDst); ub0.memoryUsage=BufferMemoryUsage::DeviceLocal;
    BufferHandle ubo0 = dev->CreateBuffer(ub0); ASSERT_TRUE(ubo0.IsValid());
    DescriptorSetLayoutDesc set0{}; DescriptorBinding s0b0{}; s0b0.binding=0; s0b0.type=DescriptorType::UniformBuffer; s0b0.count=1; s0b0.shaderStages=VK_SHADER_STAGE_FRAGMENT_BIT; set0.bindings.push_back(s0b0);
    DescriptorSetDesc ds0{}; ds0.layout=set0; ds0.debugName="G_MS_DS0"; DescriptorSetHandle dset0 = dev->CreateDescriptorSet(ds0); ASSERT_TRUE(dset0.IsValid());
    DescriptorSetUpdate upd0{}; upd0.binding=0; upd0.type=DescriptorType::UniformBuffer; upd0.buffers={ubo0}; upd0.bufferOffsets={0}; upd0.bufferRanges={256};
    dev->UpdateDescriptorSet(dset0, upd0);

    // Set1: UBO0
    BufferDesc ub1{}; ub1.size=256; ub1.usage=(uint32_t)(BufferUsage::Uniform | BufferUsage::TransferDst); ub1.memoryUsage=BufferMemoryUsage::DeviceLocal;
    BufferHandle ubo1 = dev->CreateBuffer(ub1); ASSERT_TRUE(ubo1.IsValid());
    DescriptorSetLayoutDesc set1{}; DescriptorBinding s1b0{}; s1b0.binding=0; s1b0.type=DescriptorType::UniformBuffer; s1b0.count=1; s1b0.shaderStages=VK_SHADER_STAGE_FRAGMENT_BIT; set1.bindings.push_back(s1b0);
    DescriptorSetDesc ds1{}; ds1.layout=set1; ds1.debugName="G_MS_DS1"; DescriptorSetHandle dset1 = dev->CreateDescriptorSet(ds1); ASSERT_TRUE(dset1.IsValid());
    DescriptorSetUpdate upd1{}; upd1.binding=0; upd1.type=DescriptorType::UniformBuffer; upd1.buffers={ubo1}; upd1.bufferOffsets={0}; upd1.bufferRanges={256};
    dev->UpdateDescriptorSet(dset1, upd1);

    // Two pipelines with identical multi-set layout {set0,set1}
    auto vs = GameEngine::Rendering::Tests::ReadSpirvBytes("fullscreen_noinput.vert.spv");
    auto fs = GameEngine::Rendering::Tests::ReadSpirvBytes("solidcolor.frag.spv");
    ASSERT_FALSE(vs.empty()); ASSERT_FALSE(fs.empty());

    PipelineDesc pda{}; pda.type=PipelineType::Graphics; pda.vertexShader=vs; pda.pixelShader=fs; pda.descriptorSetLayouts={set0, set1};
    pda.colorAttachmentFormats.push_back((uint32_t)TextureFormat::RGBA8_UNORM);
    pda.AddDynamicState(DynamicState::Viewport); pda.AddDynamicState(DynamicState::Scissor);

    PipelineHandle pA = dev->CreatePipeline(pda);
    PipelineHandle pB = dev->CreatePipeline(pda);
    ASSERT_TRUE(pA.IsValid() && pB.IsValid());

    dev->DebugResetBindCounters();

    ASSERT_TRUE(dev->BeginFrame());
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(rt, ResourceState::Undefined, ResourceState::RenderTarget));
    RenderPassDesc rp{}; rp.colorTargets[0]=rt; rp.colorTargetCount=1; rp.clearColor[0]=true; rp.colorStoreOp[0]=RenderPassDesc::StoreOp::Store;
    cl->BeginRenderPass(rp);
    cl->SetViewport(0,0,16,16); cl->SetScissor(0,0,16,16);

    cl->SetPipeline(pA);
    cl->BindDescriptorSet(0, dset0, pA);
    cl->BindDescriptorSet(1, dset1, pA);
    cl->Draw(3,1);

    cl->SetPipeline(pB); // identical layout; no extra binds for either set
    cl->Draw(3,1);

    cl->EndRenderPass();
    cl->End();
    dev->ExecuteCommandLists(std::vector<CommandList*>{cl.get()});
    dev->FinalizeFrame();

    auto c = dev->DebugGetBindCounters();
    EXPECT_EQ(c.descriptorBindsGraphics[0], 1u);
    EXPECT_EQ(c.descriptorBindsGraphics[1], 1u);
}

// Graphics: only set0 layout differs across pipelines -> only set0 requires rebind
TEST(DescriptorBindStats, Graphics_MultiSet_OnlySet0Changed_RebindOnlySet0)
{
#if defined(_WIN32)
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true; dd.enableDebugLayer = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    // Offscreen target
    TextureDesc td{}; td.width=16; td.height=16; td.format=(uint32_t)TextureFormat::RGBA8_UNORM; td.usage=(uint32_t)(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    TextureHandle rt = dev->CreateTexture(td); ASSERT_TRUE(rt.IsValid());

    // Set0A: UBO0
    BufferDesc ub0{}; ub0.size=256; ub0.usage=(uint32_t)(BufferUsage::Uniform | BufferUsage::TransferDst); ub0.memoryUsage=BufferMemoryUsage::DeviceLocal;
    BufferHandle ubo0 = dev->CreateBuffer(ub0); ASSERT_TRUE(ubo0.IsValid());
    DescriptorSetLayoutDesc set0A{}; DescriptorBinding a0{}; a0.binding=0; a0.type=DescriptorType::UniformBuffer; a0.count=1; a0.shaderStages=VK_SHADER_STAGE_FRAGMENT_BIT; set0A.bindings.push_back(a0);
    DescriptorSetDesc ds0A{}; ds0A.layout=set0A; ds0A.debugName="G_MS0A"; DescriptorSetHandle d0A = dev->CreateDescriptorSet(ds0A); ASSERT_TRUE(d0A.IsValid());
    DescriptorSetUpdate upd0A{}; upd0A.binding=0; upd0A.type=DescriptorType::UniformBuffer; upd0A.buffers={ubo0}; upd0A.bufferOffsets={0}; upd0A.bufferRanges={256};
    dev->UpdateDescriptorSet(d0A, upd0A);

    // Set0B: UBO0 + UBO1 (layout change)
    BufferDesc ub0b{}; ub0b.size=256; ub0b.usage=(uint32_t)(BufferUsage::Uniform | BufferUsage::TransferDst); ub0b.memoryUsage=BufferMemoryUsage::DeviceLocal;
    BufferHandle ubo0b = dev->CreateBuffer(ub0b); ASSERT_TRUE(ubo0b.IsValid());
    DescriptorSetLayoutDesc set0B{}; DescriptorBinding b0{}; b0.binding=0; b0.type=DescriptorType::UniformBuffer; b0.count=1; b0.shaderStages=VK_SHADER_STAGE_FRAGMENT_BIT; set0B.bindings.push_back(b0);
    DescriptorBinding b1{}; b1.binding=1; b1.type=DescriptorType::UniformBuffer; b1.count=1; b1.shaderStages=VK_SHADER_STAGE_FRAGMENT_BIT; set0B.bindings.push_back(b1);
    DescriptorSetDesc ds0B{}; ds0B.layout=set0B; ds0B.debugName="G_MS0B"; DescriptorSetHandle d0B = dev->CreateDescriptorSet(ds0B); ASSERT_TRUE(d0B.IsValid());
    DescriptorSetUpdate upd0B0{}; upd0B0.binding=0; upd0B0.type=DescriptorType::UniformBuffer; upd0B0.buffers={ubo0}; upd0B0.bufferOffsets={0}; upd0B0.bufferRanges={256};
    dev->UpdateDescriptorSet(d0B, upd0B0);
    DescriptorSetUpdate upd0B1{}; upd0B1.binding=1; upd0B1.type=DescriptorType::UniformBuffer; upd0B1.buffers={ubo0b}; upd0B1.bufferOffsets={0}; upd0B1.bufferRanges={256};
    dev->UpdateDescriptorSet(d0B, upd0B1);

    // Set1: UBO0 (unchanged across pipelines)
    BufferDesc ub1{}; ub1.size=256; ub1.usage=(uint32_t)(BufferUsage::Uniform | BufferUsage::TransferDst); ub1.memoryUsage=BufferMemoryUsage::DeviceLocal;
    BufferHandle ubo1 = dev->CreateBuffer(ub1); ASSERT_TRUE(ubo1.IsValid());
    DescriptorSetLayoutDesc set1{}; DescriptorBinding s1{}; s1.binding=0; s1.type=DescriptorType::UniformBuffer; s1.count=1; s1.shaderStages=VK_SHADER_STAGE_FRAGMENT_BIT; set1.bindings.push_back(s1);
    DescriptorSetDesc ds1{}; ds1.layout=set1; ds1.debugName="G_MS1"; DescriptorSetHandle d1 = dev->CreateDescriptorSet(ds1); ASSERT_TRUE(d1.IsValid());
    DescriptorSetUpdate upd1{}; upd1.binding=0; upd1.type=DescriptorType::UniformBuffer; upd1.buffers={ubo1}; upd1.bufferOffsets={0}; upd1.bufferRanges={256};
    dev->UpdateDescriptorSet(d1, upd1);

    // Pipelines: A uses {set0A,set1}, B uses {set0B,set1}
    auto vs = GameEngine::Rendering::Tests::ReadSpirvBytes("fullscreen_noinput.vert.spv");
    auto fs = GameEngine::Rendering::Tests::ReadSpirvBytes("solidcolor.frag.spv");
    ASSERT_FALSE(vs.empty()); ASSERT_FALSE(fs.empty());

    PipelineDesc pA{}; pA.type=PipelineType::Graphics; pA.vertexShader=vs; pA.pixelShader=fs; pA.descriptorSetLayouts={set0A, set1};
    pA.colorAttachmentFormats.push_back((uint32_t)TextureFormat::RGBA8_UNORM);
    pA.AddDynamicState(DynamicState::Viewport); pA.AddDynamicState(DynamicState::Scissor);

    PipelineDesc pB{}; pB.type=PipelineType::Graphics; pB.vertexShader=vs; pB.pixelShader=fs; pB.descriptorSetLayouts={set0B, set1};
    pB.colorAttachmentFormats.push_back((uint32_t)TextureFormat::RGBA8_UNORM);
    pB.AddDynamicState(DynamicState::Viewport); pB.AddDynamicState(DynamicState::Scissor);

    PipelineHandle pipeA = dev->CreatePipeline(pA);
    PipelineHandle pipeB = dev->CreatePipeline(pB);
    ASSERT_TRUE(pipeA.IsValid() && pipeB.IsValid());

    dev->DebugResetBindCounters();

    ASSERT_TRUE(dev->BeginFrame());
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(rt, ResourceState::Undefined, ResourceState::RenderTarget));
    RenderPassDesc rp{}; rp.colorTargets[0]=rt; rp.colorTargetCount=1; rp.clearColor[0]=true; rp.colorStoreOp[0]=RenderPassDesc::StoreOp::Store;
    cl->BeginRenderPass(rp);
    cl->SetViewport(0,0,16,16); cl->SetScissor(0,0,16,16);

    cl->SetPipeline(pipeA);
    cl->BindDescriptorSet(0, d0A, pipeA);
    cl->BindDescriptorSet(1, d1,  pipeA);
    cl->Draw(3,1);

    cl->SetPipeline(pipeB);
    cl->BindDescriptorSet(0, d0B, pipeB); // only set0 needs rebind
    cl->Draw(3,1);

    cl->EndRenderPass();
    cl->End();
    dev->ExecuteCommandLists(std::vector<CommandList*>{cl.get()});
    dev->FinalizeFrame();

    auto c = dev->DebugGetBindCounters();
    EXPECT_EQ(c.descriptorBindsGraphics[0], 2u);
    EXPECT_EQ(c.descriptorBindsGraphics[1], 1u);
}
