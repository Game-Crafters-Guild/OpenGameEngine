#pragma once

// Shared helpers for the CBTTerrain real-device tests. Mirrors the headless
// Vulkan device bring-up used by the Rendering module's DeviceDiagnosticsTests.

#include <cstdlib>
#include <filesystem>
#include <memory>

#include "Rendering/Core/Device.h"

namespace GameEngine::CBTTerrain::Test
{

inline void SetEnvVar(const char* key, const char* value)
{
#if defined(_WIN32)
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

inline void UnsetEnvVar(const char* key)
{
#if defined(_WIN32)
    _putenv_s(key, ""); // an empty value removes the variable
#else
    unsetenv(key);
#endif
}

inline void SetHeadlessEnv()
{
    SetEnvVar("GE_HEADLESS_TEST", "1");
}

inline std::unique_ptr<Rendering::IDevice> MakeHeadlessDevice()
{
    using namespace Rendering;
    SetHeadlessEnv();
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    auto device = DeviceFactory::CreateDevice(dd);
    if (!device || !device->Initialize(dd))
        return nullptr;
    return device;
}

inline std::filesystem::path ShaderOutputDir()
{
    return std::filesystem::path(CBT_SHADER_OUTPUT_DIR);
}

} // namespace GameEngine::CBTTerrain::Test
