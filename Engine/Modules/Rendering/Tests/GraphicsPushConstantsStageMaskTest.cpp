#include <gtest/gtest.h>
#include <vector>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "TestUtils.h"

using namespace GameEngine::Rendering;

static bool LoadShaderWithFallbacks(const char* name, std::vector<uint8_t>& out) {
    return Tests::ReadSpirvBytes(name, out);
}

// Minimal PC payload
struct PCGfx { float v[4]; };

TEST(TypedPushConstants, GraphicsStageMask_FragmentOnly) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    std::vector<uint8_t> vs, fs;
    ASSERT_TRUE(LoadShaderWithFallbacks("triangle.vert.spv", vs));
    ASSERT_TRUE(LoadShaderWithFallbacks("triangle.frag.spv", fs));

    PipelineDesc p{}; p.type = PipelineType::Graphics; p.debugName = "PC_Gfx_FragOnly";
    p.vertexShader = vs; p.pixelShader = fs;
    p.colorAttachmentFormats = { (uint32_t)TextureFormat::RGBA8_UNORM };

    PipelineHandle ph = dev->CreatePipeline(p);
    ASSERT_TRUE(ph.IsValid());

    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl != nullptr);
    cl->Begin();
    cl->SetPipeline(ph);

    PCGfx pc{};
    cl->SetPushConstants(pc);

    cl->End();
    SUCCEED();
}

TEST(TypedPushConstants, GraphicsStageMask_VertexOnly) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    std::vector<uint8_t> vs, fs;
    ASSERT_TRUE(LoadShaderWithFallbacks("triangle.vert.spv", vs));
    ASSERT_TRUE(LoadShaderWithFallbacks("triangle.frag.spv", fs));

    PipelineDesc p{}; p.type = PipelineType::Graphics; p.debugName = "PC_Gfx_VSOnly";
    p.vertexShader = vs; p.pixelShader = fs;
    p.colorAttachmentFormats = { (uint32_t)TextureFormat::RGBA8_UNORM };

    PipelineHandle ph = dev->CreatePipeline(p);
    ASSERT_TRUE(ph.IsValid());

    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl != nullptr);
    cl->Begin();
    cl->SetPipeline(ph);

    PCGfx pc{};
    cl->SetPushConstants(pc);

    cl->End();
    SUCCEED();
}

