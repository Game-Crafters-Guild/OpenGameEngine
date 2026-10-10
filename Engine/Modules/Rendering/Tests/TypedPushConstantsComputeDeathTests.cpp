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

struct LargePC { unsigned char bytes[144]; };

TEST(TypedPushConstantsDeath, Compute_ExceedsEffectiveLimit) {
#ifndef _DEBUG
    GTEST_SKIP() << "Death test relies on debug assertions; skipping in non-debug builds";
#endif
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    std::vector<uint8_t> cs;
    ASSERT_TRUE(LoadShaderWithFallbacks("pc_compute.comp.spv", cs));

    PipelineDesc p{}; p.type = PipelineType::Compute; p.debugName = "PCDeathCompute";
    p.computeShader = cs;

    PipelineHandle ph = dev->CreatePipeline(p);
    ASSERT_TRUE(ph.IsValid());

    auto cl = dev->CreateCommandList(IDevice::QueueType::Compute);
    ASSERT_TRUE(cl != nullptr);
    cl->Begin();
    cl->SetPipeline(ph);

#if GTEST_HAS_DEATH_TEST
    LargePC big{};
    ASSERT_DEATH({ cl->SetPushConstants(big); }, "Push constants");
#else
    GTEST_SKIP() << "Death tests not supported";
#endif
    cl->End();
}

