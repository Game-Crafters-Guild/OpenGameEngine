#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include "Rendering/Common/Utils.h"
#include "TestUtils.h"
#include "Rendering/Core/CommandList.h"


using namespace GameEngine::Rendering;

TEST(UniformBuffer, UBOWriteVisibleToComputeShader) {
    // Headless device for CI-friendly run
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif

    DeviceDesc desc{}; desc.applicationName = "UBO_Compute_Readback"; desc.preferredAPI = GraphicsAPI::Vulkan; desc.enableDynamicRendering = true; desc.enableDebugLayer = true;
    auto dev = DeviceFactory::CreateDevice(desc);
    ASSERT_TRUE(dev);
    if (!dev->Initialize(desc)) GTEST_SKIP() << "Device init failed";
    dev->BeginFrame();

    // Create UBO (Upload, persistently mapped) and SSBO (DeviceLocal) + readback buffer
    struct U { float v; } init{ 3.14159f };

    BufferDesc uboD{}; uboD.size = sizeof(U); uboD.usage = (uint32_t)BufferUsage::Uniform; uboD.memoryUsage = BufferMemoryUsage::Upload; uboD.persistent = true; uboD.debugName = "TestUBO";
    BufferHandle ubo = dev->CreateBuffer(uboD);
    ASSERT_TRUE(ubo.IsValid());
    dev->UpdateBuffer(ubo, 0, sizeof(U), &init);

    BufferDesc ssboD{}; ssboD.size = sizeof(float); ssboD.usage = (uint32_t)(BufferUsage::Storage | BufferUsage::TransferSrc); ssboD.memoryUsage = BufferMemoryUsage::DeviceLocal; ssboD.debugName = "SSBO_Out";
    BufferHandle ssbo = dev->CreateBuffer(ssboD);
    ASSERT_TRUE(ssbo.IsValid());

    BufferDesc rbD{}; rbD.size = sizeof(float); rbD.usage = (uint32_t)BufferUsage::TransferDst; rbD.memoryUsage = BufferMemoryUsage::Readback; rbD.debugName = "RB";
    BufferHandle rb = dev->CreateBuffer(rbD);
    ASSERT_TRUE(rb.IsValid());

    // Create compute pipeline: set0 binding0 = UBO, binding1 = SSBO
    std::vector<uint8_t> cs = Utils::LoadShaderFile("ubo_copy.comp.spv");
    ASSERT_FALSE(cs.empty());

    PipelineDesc pd{}; pd.type = PipelineType::Compute; pd.computeShader = cs; pd.debugName = "UBO_Copy_CS";
    DescriptorSetLayoutDesc set{};
    { DescriptorBinding b{}; b.binding=0; b.type=DescriptorType::UniformBuffer; b.count=1; b.shaderStages=0x20; set.bindings.push_back(b); }
    { DescriptorBinding b{}; b.binding=1; b.type=DescriptorType::StorageBuffer; b.count=1; b.shaderStages=0x20; set.bindings.push_back(b); }
    pd.descriptorSetLayouts.push_back(set);

    PipelineHandle pipe = dev->CreatePipeline(pd);
    ASSERT_NE(pipe, INVALID_PIPELINE_HANDLE);

    DescriptorSetDesc dsd{}; dsd.layout=set; dsd.transient=false; dsd.debugName = "UBO_Copy_DS";
    DescriptorSetHandle ds = dev->CreateDescriptorSet(dsd);
    ASSERT_TRUE(ds.IsValid());

    // Bind UBO and SSBO
    dev->UpdateBufferBinding(ds, 0, ubo, 0, sizeof(U));
    dev->UpdateStorageBufferBinding(ds, 1, ssbo, 0, sizeof(float));

    // Dispatch
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->SetPipeline(pipe);
    cl->BindDescriptorSet(0, ds, pipe);
    cl->Dispatch(1,1,1);
    // Ensure compute writes to SSBO are visible before transfer reads
    cl->Barrier(ResourceBarrier::CreateBufferBarrier(ssbo, ResourceState::UnorderedAccess, ResourceState::CopySource));
    cl->CopyBuffer(ssbo, rb, sizeof(float), 0, 0);

    cl->End();
    std::vector<CommandList*> lists{ cl.get() }; dev->ExecuteCommandLists(lists);
    dev->FinalizeFrame();

    dev->WaitForIdle();

    // Read back
    const float* p = reinterpret_cast<const float*>(dev->MapBuffer(rb));
    ASSERT_NE(p, nullptr);
    float got = *p; dev->UnmapBuffer(rb);
    std::cout << "Readback: " << got << " expected: " << init.v << std::endl;

    EXPECT_NEAR(got, init.v, 1e-6f);

    // Cleanup
    dev->DestroyDescriptorSet(ds);
    dev->DestroyPipeline(pipe);
    dev->DestroyBuffer(rb);
    dev->DestroyBuffer(ssbo);
    dev->DestroyBuffer(ubo);
    // Rely on device destructor to shutdown cleanly to avoid double-shutdown quirks in some drivers
}

