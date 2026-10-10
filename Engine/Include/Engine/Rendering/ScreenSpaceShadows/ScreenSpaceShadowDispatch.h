#pragma once

#include <array>
#include <cstdint>

namespace GameEngine::Engine::Renderer
{
inline constexpr uint32_t kScreenSpaceShadowGroupSize = 64;
inline constexpr uint32_t kScreenSpaceShadowMaxDispatches = 8;
struct ScreenSpaceShadowDispatch
{
    uint32_t Groups[3]{};
    int32_t Offset[2]{};
};
struct ScreenSpaceShadowDispatchList
{
    float Light[4]{};
    std::array<ScreenSpaceShadowDispatch, kScreenSpaceShadowMaxDispatches> Dispatches{};
    uint32_t Count = 0;
};
// Clip-space surface-to-light direction (w=0 before view-projection).
// Invalid, degenerate or numerically extreme projections produce an empty list.
ScreenSpaceShadowDispatchList BuildScreenSpaceShadowDispatches(
    const float lightClip[4], uint32_t width, uint32_t height);
} // namespace GameEngine::Engine::Renderer
