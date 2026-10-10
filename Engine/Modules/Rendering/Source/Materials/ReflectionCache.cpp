#include "Rendering/Materials/ReflectionCache.h"
#include <functional>
#include <algorithm>
#include <utility>

namespace GameEngine { namespace Rendering {

size_t ReflectionCacheKeyHash::operator()(const ReflectionCacheKey& k) const noexcept {
    size_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t x){ h ^= x; h *= 1099511628211ull; };
    for (auto& p : k.StagePaths) { for (char c : p) { mix((unsigned char)c); } mix(0xFF); }
    for (auto& ep : k.StageEntryPoints) {
        for (char c : ep.first) { mix((unsigned char)c); }
        mix(0xFE);
        for (char c : ep.second) { mix((unsigned char)c); }
        mix(0xFD);
    }
    for (auto id : k.SpecConstantIds) { mix(id); }
    mix(k.ToolVersion);
    return h;
}

const ShaderMeta* ReflectionCache::Find(const ReflectionCacheKey& key) const {
    auto it = m_Cache.find(key); if (it == m_Cache.end()) return nullptr; return &it->second;
}

void ReflectionCache::Put(const ReflectionCacheKey& key, ShaderMeta meta) {
    m_Cache.emplace(key, std::move(meta));
}

}} // namespace GameEngine::Rendering

