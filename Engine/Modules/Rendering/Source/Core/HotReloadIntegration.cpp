#include "Rendering/Core/HotReloadIntegration.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineCache.h"

namespace GameEngine { namespace Rendering {

void NotifyShadersHotReloaded(IDevice& device) {
    // Simple policy: drop every concrete pipeline. Material recompiles will
    // re-intern fresh ids and the (id, formatKey) cache refills on first draw.
    device.GetMutablePipelineCache().Clear();
}

void InvalidatePipelineVariantsByTags(IDevice& device, const std::vector<uint64_t>& tags) {
    auto& cache = device.GetMutablePipelineCache();
    if (tags.empty()) { cache.Clear(); return; }
    cache.InvalidateByTags(tags);
}

// Note: Asset-event registration is implemented in higher-level modules that include AssetCore.
// These stubs are provided so lower layers can link without pulling AssetCore.
uint32_t RegisterShaderHotReload(IDevice& /*device*/, class AssetEventDispatcher& /*dispatcher*/) {
    return 0; // no-op in Rendering-only build
}

void UnregisterShaderHotReload(class AssetEventDispatcher& /*dispatcher*/, uint32_t /*handle*/) {
    // no-op in Rendering-only build
}

}} // namespace GameEngine::Rendering

