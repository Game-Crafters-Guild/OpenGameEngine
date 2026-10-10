#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace GameEngine { namespace Rendering {

// Simple hash/dedup cache API independent of device impl details
struct DescriptorBindingKey {
    uint32_t Binding;
    uint32_t Type;
    uint32_t Count;
    uint32_t Stages;
};

struct DescriptorSetLayoutKey {
    std::vector<DescriptorBindingKey> Bindings;
};

inline uint64_t Hash(const DescriptorSetLayoutKey& k) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t x){ h ^= x; h *= 1099511628211ull; };
    mix(k.Bindings.size());
    for (auto& b : k.Bindings) { mix(b.Binding); mix(b.Type); mix(b.Count); mix(b.Stages); }
    return h;
}

}} // namespace

