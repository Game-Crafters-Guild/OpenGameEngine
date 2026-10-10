#pragma once

#include <cstdint>
#include <span>

namespace GameEngine::Engine::Renderer
{

// World-space bounds enclosing BOTH the previous and current caster bounds.
// A departing caster must invalidate the map that still contains its old shadow.
struct ShadowCasterChangeSphere
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float Radius = 0.0f;
};

// Explains only the latest version advance. Consumers that missed a version
// must invalidate conservatively. The span is borrowed until the next notify.
struct ShadowCasterChangeSet
{
    uint64_t Version = 0;
    std::span<const ShadowCasterChangeSphere> Spheres{};
    bool Unattributed = true;
};

} // namespace GameEngine::Engine::Renderer
