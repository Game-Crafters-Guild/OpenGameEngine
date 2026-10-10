#include "Engine/RenderingHotReloadBridge.h"

#include <filesystem>
#include <algorithm>
#include <regex>

#include "AssetCore/AssetEvents.h"
#include "AssetCore/SharedFileRead.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/HotReloadIntegration.h"

namespace GameEngine { namespace EngineIntegration {

namespace {
    // FNV-1a 64-bit
    constexpr uint64_t kFNVOffsetBasis = 1469598103934665603ull;
    constexpr uint64_t kFNVPrime = 1099511628211ull;

    uint64_t HashBytes(const std::vector<uint8_t>& bytes) {
        uint64_t h = kFNVOffsetBasis;
        for (uint8_t b : bytes) { h ^= b; h *= kFNVPrime; }
        return h;
    }

    bool ReadFileAll(const std::filesystem::path& p, std::vector<uint8_t>& out) {
        return ReadFileBytesShared(p, out) && !out.empty();
    }

    static inline std::string ToLowerExt(const std::filesystem::path& p) {
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](char c){ return static_cast<char>(std::tolower(c)); });
        return ext;
    }
}

uint32_t RegisterShaderHotReloadBridge(Rendering::IDevice& device, AssetEventDispatcher& dispatcher) {
    return dispatcher.AddCallback([&device](const AssetEvent& e){
        using GameEngine::AssetEventType; using GameEngine::AssetType;
        if (!(
            (e.EventType == AssetEventType::AssetReloaded || e.EventType == AssetEventType::AssetModified) &&
            (e.Type == AssetType::Shader || e.Type == AssetType::Material))) {
            return;
        }

        std::vector<uint64_t> tags;
        bool forceFullInvalidation = false;
        const std::filesystem::path path = e.AssetPath;
        const std::string ext = ToLowerExt(path);

        if (e.Type == AssetType::Shader) {
            if (ext == ".shader") {
                // Shader program assets can compile to cached SPIR-V/shaderdesc, but the
                // derived output paths are intentionally not stored in the Assets tree.
                // Without dependency metadata here, do a conservative invalidation.
                forceFullInvalidation = true;
            } else if (ext == ".glsl" || ext == ".hlsl" || ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".geom") {
                // Shader source edits require recompilation; invalidate conservatively.
                forceFullInvalidation = true;
            }
        } else if (e.Type == AssetType::Material) {
            // Materials can reference shader *sources* directly (composition) or indirectly via shader programs.
            // Without dependency metadata here, do a conservative invalidation.
            forceFullInvalidation = true;
        }

        if (!tags.empty()) {
            // Selective invalidation via Rendering helper (keeps Engine decoupled from cache impl)
            GameEngine::Rendering::InvalidatePipelineVariantsByTags(device, tags);
        } else if (forceFullInvalidation) {
            GameEngine::Rendering::NotifyShadersHotReloaded(device);
        } else {
            // Conservative fallback
            GameEngine::Rendering::NotifyShadersHotReloaded(device);
        }
    });
}

void UnregisterShaderHotReloadBridge(AssetEventDispatcher& dispatcher, uint32_t handle) {
    dispatcher.RemoveCallback(handle);
}

}} // namespace GameEngine::EngineIntegration

