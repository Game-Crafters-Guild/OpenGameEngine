#pragma once

// Test helper: create a real headless device the same way the existing render-graph
// tests do (e.g. RenderGraphAutoCompileBehaviorTests). RenderGraph's device-touching tests
// run against the REAL IDevice (not a mock) so there is no production disconnect.
// Returns null if no device can be created (e.g. no GPU) — callers GTEST_SKIP.

#include "Rendering/Core/Device.h"

#include <memory>

namespace GameEngine::Rendering::RenderGraph::Test
{

inline std::unique_ptr<IDevice> MakeHeadlessDevice()
{
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    dd.enableSwapchain = false; // headless: no window/surface in the test harness
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
        return nullptr;
    return dev;
}

} // namespace GameEngine::Rendering::RenderGraph::Test
