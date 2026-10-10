#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace GameEngine::UI
{
// Reference modes preserve proportions; Fit keeps the reference canvas visible,
// while Fill covers the target. Layout remains responsive on the other axis.
enum class UIScaleMode : uint8_t { Platform, Width, Height, Fit, Fill };

struct UIScaleSettings
{
    UIScaleMode mode = UIScaleMode::Platform;
    float referenceWidth = 1920.0f;
    float referenceHeight = 1080.0f;

    float Resolve(uint32_t width, uint32_t height, float platformScale) const
    {
        const float fallback = std::isfinite(platformScale) ? std::max(0.01f, platformScale) : 1.0f;
        const bool validWidth = std::isfinite(referenceWidth) && referenceWidth > 0.0f;
        const bool validHeight = std::isfinite(referenceHeight) && referenceHeight > 0.0f;
        float scale = fallback;
        switch (mode)
        {
        case UIScaleMode::Platform: break;
        case UIScaleMode::Width:
            if (validWidth) scale = static_cast<float>(width) / referenceWidth;
            break;
        case UIScaleMode::Height:
            if (validHeight) scale = static_cast<float>(height) / referenceHeight;
            break;
        case UIScaleMode::Fit:
        case UIScaleMode::Fill:
            if (validWidth && validHeight)
            {
                const float x = static_cast<float>(width) / referenceWidth;
                const float y = static_cast<float>(height) / referenceHeight;
                scale = mode == UIScaleMode::Fit ? std::min(x, y) : std::max(x, y);
            }
            break;
        }
        return std::isfinite(scale) ? std::max(0.01f, scale) : fallback;
    }
};
}
