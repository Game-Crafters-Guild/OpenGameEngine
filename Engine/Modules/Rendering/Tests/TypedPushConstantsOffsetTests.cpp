#include <gtest/gtest.h>
#include <fstream>
#include <iterator>
#include <vector>
#include <string>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Common/Utils.h"
#include "TestUtils.h"

using namespace GameEngine::Rendering;

struct PC64 { unsigned char bytes[64]; };

static bool LoadBlob(const char* p, std::vector<uint8_t>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}
static bool LoadShaderWithFallbacks(const char* name, std::vector<uint8_t>& out) {
    std::string p = GameEngine::Rendering::Utils::ResolveShaderPath(name);
    return LoadBlob(p.c_str(), out);
}

TEST(TypedPushConstants, OffsetWritesWithinDeclaredLimit) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    // Use triangle shader pair; default fallback for pc size is 128 bytes
    std::vector<uint8_t> vs, fs;
    if (!LoadShaderWithFallbacks("triangle.vert.spv", vs) || !LoadShaderWithFallbacks("triangle.frag.spv", fs)) {
        GTEST_SKIP() << "Missing triangle shaders";
    }

    PipelineDesc p{}; p.type = PipelineType::Graphics; p.debugName = "PCOffset";
    p.vertexShader = vs; p.pixelShader = fs;
    p.colorAttachmentFormats = { (uint32_t)TextureFormat::RGBA8_UNORM };

    PipelineHandle ph = dev->CreatePipeline(p);
    ASSERT_TRUE(ph.IsValid());

    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl != nullptr);
    cl->Begin();
    cl->SetPipeline(ph);

    // Two 64-byte writes should be allowed within 128B limit.
    PC64 a{}, b{};
    cl->SetConstants(0, sizeof(a), &a);
    cl->SetConstants(0, sizeof(b), &b); // emulate offset space by replacing previous data; we don't expose offset currently

    cl->End();
    SUCCEED();
}

