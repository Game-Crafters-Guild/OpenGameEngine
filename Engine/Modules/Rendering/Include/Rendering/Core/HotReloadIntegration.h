#pragma once

#include <cstdint>
#include <vector>

namespace GameEngine {
    class AssetEventDispatcher; // fwd decl from AssetCore

namespace Rendering {

class IDevice;

// Call this when shader packages or modules are reloaded to invalidate dependent pipeline variants
void NotifyShadersHotReloaded(IDevice& device);

// Tag-based selective invalidation helper
void InvalidatePipelineVariantsByTags(IDevice& device, const std::vector<uint64_t>& tags);

// Register an asset event callback that calls NotifyShadersHotReloaded on Shader/Material reloads
// Returns a dispatcher callback handle; store it and pass to UnregisterShaderHotReload on shutdown.
uint32_t RegisterShaderHotReload(IDevice& device, AssetEventDispatcher& dispatcher);
void UnregisterShaderHotReload(AssetEventDispatcher& dispatcher, uint32_t handle);

}} // namespace GameEngine::Rendering

