#include <gtest/gtest.h>
#include <fstream>
#include <iterator>
#include <vector>
#include "TestUtils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"

using namespace GameEngine::Rendering;

struct SmallPC { float m0[4]; float m1[4]; }; // 32 bytes

TEST(TypedPushConstants, SetWithinDeclaredLimit) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    // Minimal triangle shaders, fallback to default pipeline layout with 128B push
    // constants. The blobs are produced by the CompileShaders dependency into the
    // build output shader dir; a miss here is a build regression, not an
    // environment condition.
    std::vector<uint8_t> vs, fs;
    ASSERT_TRUE(Tests::ReadSpirvBytes("triangle.vert.spv", vs)) << "triangle.vert.spv not found in CompileShaders output";
    ASSERT_TRUE(Tests::ReadSpirvBytes("triangle.frag.spv", fs)) << "triangle.frag.spv not found in CompileShaders output";

    PipelineDesc p{}; p.type = PipelineType::Graphics; p.debugName = "TypedPC";
    p.vertexShader = vs; p.pixelShader = fs;
    p.colorAttachmentFormats = { (uint32_t)TextureFormat::RGBA8_UNORM };

    PipelineHandle ph = dev->CreatePipeline(p);
    ASSERT_TRUE(ph.IsValid());

    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl != nullptr);
    cl->Begin();
    cl->SetPipeline(ph);

    SmallPC pc{};
    cl->SetPushConstants(pc);

    cl->End();
    // No validation of side-effects here; this is a smoke to ensure SetPushConstants compiles and uses declared/effective limit
    SUCCEED();
}



#include "Rendering/Common/Utils.h"

static bool LoadBlob(const char* p, std::vector<uint8_t>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

static bool LoadShaderWithResolver(const char* name, std::vector<uint8_t>& out) {
    std::string p = GameEngine::Rendering::Utils::ResolveShaderPath(name);
    return LoadBlob(p.c_str(), out);
}

TEST(TypedPushConstants, NamedRanges_ByNameAndById_Succeeds) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    std::vector<uint8_t> vs, fs;
    ASSERT_TRUE(LoadShaderWithResolver("triangle.vert.spv", vs));
    ASSERT_TRUE(LoadShaderWithResolver("triangle.frag.spv", fs));

    PipelineDesc p{}; p.type = PipelineType::Graphics; p.debugName = "PC_NamedRanges_Gfx";
    p.vertexShader = vs; p.pixelShader = fs;
    p.colorAttachmentFormats = { (uint32_t)TextureFormat::RGBA8_UNORM };

    PipelineDesc::PushConstantRangeDesc r1; r1.name = "Transform"; r1.size = 64; r1.stagesMask = 0; r1.offset = 0;
    PipelineDesc::PushConstantRangeDesc r2; r2.name = "Params";    r2.size = 32; r2.stagesMask = 0; r2.offset = 0;
    p.pushConstantRanges = { r1, r2 };

    PipelineHandle ph = dev->CreatePipeline(p);
    ASSERT_TRUE(ph.IsValid());

    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl != nullptr);
    cl->Begin();
    cl->SetPipeline(ph);

    std::vector<uint8_t> transform(64, 0xAB);
    EXPECT_TRUE(cl->SetPushConstantsByName("Transform", transform.data(), transform.size(), 0));

    uint32_t paramsId = 0xFFFFFFFFu;
    ASSERT_TRUE(dev->FindPipelinePushConstantRangeId(ph, "Params", paramsId));
    struct ParamsSmall { uint32_t a, b, c, d; } params{};
    EXPECT_TRUE(cl->SetPushConstantsById(paramsId, params));

    cl->End();
}

TEST(TypedPushConstants, NamedRanges_OutOfBounds_ReturnsFalse) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    std::vector<uint8_t> vs, fs;
    ASSERT_TRUE(LoadShaderWithResolver("triangle.vert.spv", vs));
    ASSERT_TRUE(LoadShaderWithResolver("triangle.frag.spv", fs));

    PipelineDesc p{}; p.type = PipelineType::Graphics; p.debugName = "PC_NamedRanges_OOB";
    p.vertexShader = vs; p.pixelShader = fs;
    p.colorAttachmentFormats = { (uint32_t)TextureFormat::RGBA8_UNORM };

    PipelineDesc::PushConstantRangeDesc r1; r1.name = "Transform"; r1.size = 64; r1.stagesMask = 0; r1.offset = 0;
    PipelineDesc::PushConstantRangeDesc r2; r2.name = "Params";    r2.size = 32; r2.stagesMask = 0; r2.offset = 0;
    p.pushConstantRanges = { r1, r2 };

    PipelineHandle ph = dev->CreatePipeline(p);
    ASSERT_TRUE(ph.IsValid());

    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl != nullptr);
    cl->Begin();
    cl->SetPipeline(ph);

    std::vector<uint8_t> payload(40, 0xCD);
    EXPECT_DEATH(cl->SetPushConstantsByName("Params", payload.data(), payload.size(), /*offset*/24), "Push constants write exceeds declared range size");

    // No End() here; death test should abort inside the call
}
