#include "Engine/Rendering/ScreenSpaceShadows/ScreenSpaceShadowDispatch.h"

#include <algorithm>
#include <cmath>
#include "BendScreenSpaceShadows/bend_sss_cpu.h"

namespace GameEngine::Engine::Renderer
{
ScreenSpaceShadowDispatchList BuildScreenSpaceShadowDispatches(
    const float lightClip[4], uint32_t width, uint32_t height)
{
    ScreenSpaceShadowDispatchList result{};
    if (!lightClip || width == 0 || height == 0 || width > 16384 || height > 16384)
        return result;
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(lightClip[i]))
            return result;
    // Parallel rays project at infinity. Keep cascades through this tiny angular
    // interval instead of feeding the radial planner imprecise pixel coordinates.
    if (std::abs(lightClip[3]) < 0.001f)
        return result;
    const float x = (lightClip[0] / lightClip[3] * 0.5f + 0.5f) * width;
    const float y = (lightClip[1] / lightClip[3] * -0.5f + 0.5f) * height;
    if (std::abs(x) > 1.0e6f || std::abs(y) > 1.0e6f ||
        !std::isfinite(lightClip[2] / lightClip[3]))
        return result;
    float clip[4];
    std::copy_n(lightClip, 4, clip);
    int viewport[2] = {static_cast<int>(width), static_cast<int>(height)};
    int lower[2] = {0, 0};
    auto plan = Bend::BuildDispatchList(clip, viewport, lower, viewport);
    std::copy_n(plan.LightCoordinate_Shader, 4, result.Light);
    result.Count = static_cast<uint32_t>(plan.DispatchCount);
    for (uint32_t i = 0; i < result.Count; ++i)
    {
        std::copy_n(plan.Dispatch[i].WaveCount, 3, result.Dispatches[i].Groups);
        std::copy_n(plan.Dispatch[i].WaveOffset_Shader, 2, result.Dispatches[i].Offset);
    }
    return result;
}
} // namespace GameEngine::Engine::Renderer
