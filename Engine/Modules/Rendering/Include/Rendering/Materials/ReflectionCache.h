#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "Rendering/Materials/ShaderMeta.h"

namespace GameEngine { namespace Rendering {

struct ReflectionCacheKey {
    // Key includes stage file paths, stage entry points, and specialization constants IDs
    std::vector<std::string> StagePaths; // e.g., {vs.spv, fs.spv}
    std::vector<std::pair<std::string,std::string>> StageEntryPoints; // sorted {stage, entry}
    std::vector<uint32_t> SpecConstantIds; // sorted IDs
    uint32_t ToolVersion = 1;
    bool operator==(const ReflectionCacheKey& o) const {
        return StagePaths == o.StagePaths && StageEntryPoints == o.StageEntryPoints && SpecConstantIds == o.SpecConstantIds && ToolVersion == o.ToolVersion;
    }
};

struct ReflectionCacheKeyHash { size_t operator()(const ReflectionCacheKey& k) const noexcept; };

class ReflectionCache {
public:
    const ShaderMeta* Find(const ReflectionCacheKey& key) const;
    void Put(const ReflectionCacheKey& key, ShaderMeta meta);

private:
    std::unordered_map<ReflectionCacheKey, ShaderMeta, ReflectionCacheKeyHash> m_Cache;
};

}} // namespace GameEngine::Rendering

