#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>
#include <string>

#include "Engine/RenderingHotReloadBridge.h"
#include "Rendering/Core/Device.h"
#include "AssetCore/AssetEvents.h"
#include "AssetCore/GUID.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::EngineIntegration;

static std::unique_ptr<IDevice> MakeDevice()
{
    DeviceDesc d{}; d.preferredAPI = GraphicsAPI::Vulkan; d.enableDebugLayer = false; d.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(d);
    if (!dev || !dev->Initialize(d)) return nullptr;
    return dev;
}

TEST(HotReloadBridgeSmoke, ShaderSpvAndMaterialRefs_NoCrash)
{
    auto dev = MakeDevice();
    if (!dev) GTEST_SKIP() << "No Vulkan device available";

    AssetEventDispatcher dispatcher;
    const uint32_t handle = RegisterShaderHotReloadBridge(*dev, dispatcher);

    // Create a small temp workspace with dummy .spv and a .shaderdesc referencing it
    const auto tmpRoot = std::filesystem::temp_directory_path() / "ge_hotreload_smoke";
    std::filesystem::create_directories(tmpRoot);

    const auto spvPath = tmpRoot / "dummy.comp.spv";
    const auto descPath = tmpRoot / "dummy.shaderdesc";
    const auto matPath  = tmpRoot / "dummy.material";

    // Write a few bytes to spv (not a real shader; tag hashing only)
    {
        std::ofstream out(spvPath, std::ios::binary);
        const unsigned char data[8] = {0x03,0x02,0x23,0x07,0xAA,0xBB,0xCC,0xDD};
        out.write(reinterpret_cast<const char*>(data), sizeof(data));
    }

    // Minimal .shaderdesc JSON with stageSpv
    {
        std::ofstream out(descPath, std::ios::binary);
        out << "{\n"
               "  \"metadataVersion\":1,\n"
               "  \"toolVersion\":1,\n"
               "  \"meta\": { \"version\":1 },\n"
               "  \"stageSpv\": { \"cs\": \"dummy.comp.spv\" }\n"
               "}\n";
    }

    // Material referencing both the .shaderdesc and the .spv to exercise both scanners
    {
        std::ofstream out(matPath, std::ios::binary);
        out << "material_name: Dummy\n"
               "shaderDesc: dummy.shaderdesc\n"
               "stages: { cs: dummy.comp.spv }\n";
    }

    // Dispatch events; the bridge should handle these without throwing/crashing
    GUID guid = GUID::Generate();
    ASSERT_NO_THROW(dispatcher.DispatchEvent(AssetEvent{AssetEventType::AssetReloaded, guid, AssetType::Shader, descPath.string()}));
    ASSERT_NO_THROW(dispatcher.DispatchEvent(AssetEvent{AssetEventType::AssetReloaded, guid, AssetType::Shader, spvPath.string()}));
    ASSERT_NO_THROW(dispatcher.DispatchEvent(AssetEvent{AssetEventType::AssetReloaded, guid, AssetType::Material, matPath.string()}));

    UnregisterShaderHotReloadBridge(dispatcher, handle);

    dev->Shutdown();
}

