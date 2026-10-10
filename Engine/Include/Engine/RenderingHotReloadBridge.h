#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace GameEngine {
class AssetEventDispatcher;
struct AssetEvent;

namespace Rendering { class IDevice; }

namespace EngineIntegration {

// Registers a Shader/Material hot-reload listener that computes shader tags
// and requests selective invalidation from the Rendering layer.
// Returns a callback handle to be passed to UnregisterShaderHotReloadBridge on shutdown.
uint32_t RegisterShaderHotReloadBridge(Rendering::IDevice& device, AssetEventDispatcher& dispatcher);

void UnregisterShaderHotReloadBridge(AssetEventDispatcher& dispatcher, uint32_t handle);

} // namespace EngineIntegration

} // namespace GameEngine

