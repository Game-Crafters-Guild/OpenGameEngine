#pragma once

#include "Rendering/Core/Device.h"

#include <memory>

inline std::unique_ptr<GameEngine::Rendering::IDevice> CreateVulkanDeviceFast()
{
    GameEngine::Rendering::DeviceDesc desc{};
    desc.preferredAPI = GameEngine::Rendering::GraphicsAPI::Vulkan;
    desc.enableDebugLayer = false;
    desc.enableDynamicRendering = true;

    auto device = GameEngine::Rendering::DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        return nullptr;
    return device;
}
