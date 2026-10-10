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

struct PC32 { unsigned char bytes[32]; };
struct PC96 { unsigned char bytes[96]; };

TEST(TypedPushConstantsDeath, Offset_ExceedsRange) {
#ifndef _DEBUG
    GTEST_SKIP() << "Death test relies on debug assertions; skipping in non-debug builds";
#endif
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    std::vector<uint8_t> vs, fs;
    ASSERT_TRUE(LoadShaderWithFallbacks("triangle.vert.spv", vs));
    ASSERT_TRUE(LoadShaderWithFallbacks("triangle.frag.spv", fs));

    PipelineDesc p{}; p.type = PipelineType::Graphics; p.debugName = "PCOffsetDeath";
    p.vertexShader = vs; p.pixelShader = fs;
    p.colorAttachmentFormats = { (uint32_t)TextureFormat::RGBA8_UNORM };

    PipelineHandle ph = dev->CreatePipeline(p);
    ASSERT_TRUE(ph.IsValid());

    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->SetPipeline(ph);

#if GTEST_HAS_DEATH_TEST
    PC96 big{};
    // Offset + size = 160 > 128 (should assert)
    ASSERT_DEATH({ cl->SetConstants(0, /*offset*/64, sizeof(big), &big); }, "exceed");
#else
    GTEST_SKIP() << "Death tests not supported";
#endif
    cl->End();
}

TEST(TypedPushConstants, Offset_WithinRange) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    std::vector<uint8_t> vs, fs;
    if (!LoadShaderWithFallbacks("triangle.vert.spv", vs) || !LoadShaderWithFallbacks("triangle.frag.spv", fs)) {
        GTEST_SKIP() << "Missing triangle shaders";
    }

    PipelineDesc p{}; p.type = PipelineType::Graphics; p.debugName = "PCOffsetWithin";
    p.vertexShader = vs; p.pixelShader = fs;
    p.colorAttachmentFormats = { (uint32_t)TextureFormat::RGBA8_UNORM };

    PipelineHandle ph = dev->CreatePipeline(p);
    ASSERT_TRUE(ph.IsValid());

    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->SetPipeline(ph);

    PC32 a{}; PC96 b{};
    cl->SetConstants(0, /*offset*/0, sizeof(a), &a);
    cl->SetConstants(0, /*offset*/32, sizeof(b), &b); // 32 + 96 = 128 OK
    cl->End();

    SUCCEED();
}

