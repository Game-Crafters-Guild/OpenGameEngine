#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Common/Utils.h"
#include "TestUtils.h"

using namespace GameEngine::Rendering;

namespace {
struct PC64 { unsigned char bytes[64]; };
struct PC96 { unsigned char bytes[96]; };
}

TEST(TypedPushConstantsUX, ReflectedLimitNarrowingDeath) {
#ifndef _DEBUG
    GTEST_SKIP() << "Death test relies on debug assertions";
#else
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    std::vector<uint8_t> vs = Utils::LoadShaderFile("triangle.vert.spv");
    std::vector<uint8_t> fs = Utils::LoadShaderFile("triangle.frag.spv");
    ASSERT_FALSE(vs.empty());
    ASSERT_FALSE(fs.empty());

    // Explicitly narrow push constants to 64B
    PipelineDesc pd{}; pd.type = PipelineType::Graphics; pd.debugName = "TypedPCUX";
    pd.vertexShader = vs; pd.pixelShader = fs;
    pd.colorAttachmentFormats = { (uint32_t)TextureFormat::RGBA8_UNORM };
    pd.pushConstantSize = 64; pd.pushConstantStagesMask = 0xFFFFFFFFu;

    PipelineHandle ph = dev->CreatePipeline(pd);
    ASSERT_TRUE(ph.IsValid());

    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl != nullptr);
    cl->Begin();
    cl->SetPipeline(ph);

#if GTEST_HAS_DEATH_TEST
    PC96 big{};
    ASSERT_DEATH({ cl->SetPushConstants(big); }, "exceed.*declared");
#else
    GTEST_SKIP() << "No death-test support";
#endif

    cl->End();
#endif
}

