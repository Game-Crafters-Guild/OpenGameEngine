#pragma once

#include <algorithm>
#include <cstdint>

namespace GameEngine::Math
{

constexpr float Lerp(float a, float b, float t) { return a + (b - a) * t; }
constexpr float InverseLerp(float a, float b, float v) { return (b != a) ? (v - a) / (b - a) : 0.0f; }
constexpr float Clamp01(float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }

constexpr float SmoothStep(float edge0, float edge1, float x)
{
    float t = Clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

constexpr float Remap(float inMin, float inMax, float outMin, float outMax, float v)
{
    float t = InverseLerp(inMin, inMax, v);
    return Lerp(outMin, outMax, t);
}

// Interpolates two 0xAARRGGBB colours in PREMULTIPLIED alpha, as css-color-4
// 13.3 requires: each colour's channels are scaled by its own alpha, the
// results are interpolated, and the interpolated channels are divided back out
// by the interpolated alpha.
//
// The premultiply step is what keeps a colour's hue constant while it fades.
// CSS `transparent` is rgba(0, 0, 0, 0), so interpolating channels directly
// would pull them toward black on the way out and the element would darken
// rather than simply disappear.
inline uint32_t LerpColorARGB(uint32_t a, uint32_t b, float t)
{
    auto channel = [](uint32_t c, int shift) -> float {
        return static_cast<float>((c >> shift) & 0xFFu);
    };
    auto pack = [](float v) -> uint32_t {
        return static_cast<uint32_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
    };

    const float aA = channel(a, 24);
    const float bA = channel(b, 24);
    const float outA = Lerp(aA, bA, t);
    // Fully transparent result: premultiplied space holds no colour to recover,
    // and un-premultiplying would divide by zero.
    if (outA <= 0.0f)
        return 0u;

    const float aWeight = aA * (1.0f / 255.0f);
    const float bWeight = bA * (1.0f / 255.0f);
    const float unPremultiply = 255.0f / outA;

    auto lerpChannel = [&](int shift) -> uint32_t {
        const float premultiplied =
            Lerp(channel(a, shift) * aWeight, channel(b, shift) * bWeight, t);
        return pack(premultiplied * unPremultiply);
    };

    return (pack(outA) << 24) |
           (lerpChannel(16) << 16) |
           (lerpChannel(8) << 8) |
            lerpChannel(0);
}

} // namespace GameEngine::Math
