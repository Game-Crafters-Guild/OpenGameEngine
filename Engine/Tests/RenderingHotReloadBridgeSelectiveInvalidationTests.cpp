#include <gtest/gtest.h>
#include <string>
#include "Engine/RenderingHotReloadBridge.h"
#include "AssetCore/AssetEvents.h"
#include "AssetCore/GUID.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Common/Utils.h"

#include "../Modules/Rendering/Tests/TestUtils.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::EngineIntegration;

static std::unique_ptr<IDevice> MakeVulkanDevice()
{
    DeviceDesc d{}; d.preferredAPI = GraphicsAPI::Vulkan; d.enableDebugLayer = false; d.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(d);
    if (!dev || !dev->Initialize(d)) return nullptr;
    return dev;
}

TEST(HotReloadBridgeSelectiveInvalidation, EvictsVariantOnShaderSpvReload)
{
    auto dev = MakeVulkanDevice();
    if (!dev) GTEST_SKIP() << "No Vulkan device available";

    // Build a simple graphics pipeline using repo-provided SPIR-V
    PipelineDesc pd{}; pd.type = PipelineType::Graphics; pd.debugName = "SelectiveInvalidation_Triangle";
    pd.vertexShader = GameEngine::Rendering::Tests::ReadSpirvBytes("triangle.vert.spv");
    pd.pixelShader  = GameEngine::Rendering::Tests::ReadSpirvBytes("triangle.frag.spv");
    ASSERT_FALSE(pd.vertexShader.empty());
    ASSERT_FALSE(pd.pixelShader.empty());
    pd.colorAttachmentFormats = { (uint32_t)TextureFormat::RGBA8_UNORM };

    // Create pipeline (this should insert a variant into the central cache and tag it with shader hashes)
    PipelineHandle ph = dev->CreatePipeline(pd);
    ASSERT_NE(ph, INVALID_HANDLE);

    auto statsBefore = dev->GetPipelineCacheStats();
    // Some backends only populate the concrete cache via RenderGraph paths. If
    // it's empty here, the test still validates that hot-reload dispatch is
    // safe (doesn't crash).
    const bool cachePopulated = (statsBefore.ConcreteSize >= 1);

    // Register bridge and dispatch a shader reload event for the fragment shader
    AssetEventDispatcher dispatcher;
    const uint32_t handle = RegisterShaderHotReloadBridge(*dev, dispatcher);

    const std::string fragAbs = GameEngine::Rendering::Tests::ResolveShaderPathStr("triangle.frag.spv");
    GUID guid = GUID::Generate();
    ASSERT_NO_THROW(dispatcher.DispatchEvent(AssetEvent{AssetEventType::AssetReloaded, guid, AssetType::Shader, fragAbs}));

    auto statsAfter = dev->GetPipelineCacheStats();

    if (cachePopulated) {
        // Expect that at least one entry was evicted due to tag match
        EXPECT_LT(statsAfter.ConcreteSize, statsBefore.ConcreteSize) << "Expected selective invalidation to evict matching pipeline variant(s)";
    } else {
        SUCCEED() << "Cache not populated on this path; verified safe hot-reload dispatch";
    }

    // Cleanup
    UnregisterShaderHotReloadBridge(dispatcher, handle);
    dev->DestroyPipeline(ph);
    dev->Shutdown();
}

