#pragma once

// One texel of a clear color, encoded in a format's memory layout. The Metal
// backend clears a texture that has no render-target usage by copying these
// bytes in from a buffer (see MetalCommandList::ClearColorImageSubresource),
// so the bytes must be exactly what a render-pass clear to the same color
// stores:
//   - UNORM channels clamp to [0, 1] and round to nearest;
//   - sRGB formats take a linear color and store it encoded, as a render-pass
//     clear and vkCmdClearColorImage both do;
//   - integer channels take the color's numeric value, truncated toward zero
//     and saturated to the channel's range, which is what a Metal render-pass
//     clear of an integer target stores;
//   - float channels round to nearest; the unsigned 11/10-bit floats round
//     through binary16 and clamp negatives to zero.
// Channels a format does not store are ignored.

#include "Rendering/Core/Device.h"
#include "Types/Color.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace GameEngine::Rendering
{

inline constexpr size_t kMaxClearTexelBytes = 16;
using ClearTexel = std::array<uint8_t, kMaxClearTexelBytes>;

namespace ClearTexelDetail
{
inline uint32_t Unorm(float value, uint32_t bits)
{
    const float maxValue = static_cast<float>((1u << bits) - 1u);
    return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * maxValue));
}

template <typename T>
T SaturatingInt(float value)
{
    if (std::isnan(value))
        return T{0};
    const double v = std::trunc(static_cast<double>(value));
    const double lo = static_cast<double>(std::numeric_limits<T>::min());
    const double hi = static_cast<double>(std::numeric_limits<T>::max());
    return static_cast<T>(std::clamp(v, lo, hi));
}

inline uint16_t Half(float value)
{
    return std::bit_cast<uint16_t>(static_cast<_Float16>(value));
}

// binary16 -> an unsigned small float with the same 5-bit exponent and
// `mantissaBits` of mantissa, rounding to nearest even on the dropped bits.
inline uint32_t UnsignedSmallFloat(float value, uint32_t mantissaBits)
{
    if (std::isnan(value))
        return (0x1Fu << mantissaBits) | 1u;
    if (!(value > 0.0f))
        return 0u; // negatives and -0 clamp to zero; the format has no sign
    const uint32_t half = Half(value) & 0x7FFFu;
    const uint32_t drop = 10u - mantissaBits;
    if ((half & 0x7C00u) == 0x7C00u)
        return half >> drop; // infinity
    uint32_t out = half >> drop;
    const uint32_t rest = half & ((1u << drop) - 1u);
    const uint32_t halfway = 1u << (drop - 1u);
    if (rest > halfway || (rest == halfway && (out & 1u) != 0u))
        ++out; // a carry into the exponent is the correct rounding, up to infinity
    return out;
}

template <typename T>
void Store(ClearTexel& out, uint32_t index, T value)
{
    std::memcpy(out.data() + index * sizeof(T), &value, sizeof(T));
}

// 8-bit UNORM channels; `bgra` stores red and blue swapped, `srgb` encodes
// the color channels (alpha stays linear).
inline uint32_t EncodeUnorm8(const float rgba[4], uint32_t channels, bool srgb, bool bgra, ClearTexel& out)
{
    const ColorSRGB encoded = ColorLinear(rgba[0], rgba[1], rgba[2], rgba[3]).ToSRGB();
    const float srgbChannels[3] = {encoded.r, encoded.g, encoded.b};
    for (uint32_t i = 0; i < channels; ++i)
    {
        const uint32_t src = bgra && i < 3 ? 2u - i : i;
        // The encoder clamps NaN to 1; a NaN channel stores what it stores unencoded.
        const float v = srgb && src < 3 && !std::isnan(rgba[src]) ? srgbChannels[src] : rgba[src];
        out[i] = static_cast<uint8_t>(Unorm(v, 8));
    }
    return channels;
}

template <typename T>
uint32_t EncodeInts(const float rgba[4], uint32_t channels, ClearTexel& out)
{
    for (uint32_t i = 0; i < channels; ++i)
        Store(out, i, SaturatingInt<T>(rgba[i]));
    return channels * static_cast<uint32_t>(sizeof(T));
}

template <typename T>
uint32_t EncodeFloats(const float rgba[4], uint32_t channels, ClearTexel& out)
{
    for (uint32_t i = 0; i < channels; ++i)
    {
        if constexpr (std::is_same_v<T, uint16_t>)
            Store(out, i, Half(rgba[i]));
        else
            Store(out, i, rgba[i]);
    }
    return channels * static_cast<uint32_t>(sizeof(T));
}
} // namespace ClearTexelDetail

// Writes one texel of `rgba` in `format` to `out` and returns its size in
// bytes, or 0 for a format this cannot encode (depth/stencil, block
// compressed, formats without a Metal equivalent).
inline uint32_t EncodeClearTexel(TextureFormat format, const float rgba[4], ClearTexel& out)
{
    using namespace ClearTexelDetail;
    out.fill(0);

    switch (format)
    {
    case TextureFormat::R8_UNORM: return EncodeUnorm8(rgba, 1, false, false, out);
    case TextureFormat::R8G8_UNORM: return EncodeUnorm8(rgba, 2, false, false, out);
    case TextureFormat::RGBA8_UNORM: return EncodeUnorm8(rgba, 4, false, false, out);
    case TextureFormat::RGBA8_SRGB: return EncodeUnorm8(rgba, 4, true, false, out);
    case TextureFormat::BGRA8_UNORM: return EncodeUnorm8(rgba, 4, false, true, out);
    case TextureFormat::BGRA8_SRGB: return EncodeUnorm8(rgba, 4, true, true, out);

    case TextureFormat::R8_UINT: return EncodeInts<uint8_t>(rgba, 1, out);
    case TextureFormat::R8G8_UINT: return EncodeInts<uint8_t>(rgba, 2, out);
    case TextureFormat::RGBA8_UINT: return EncodeInts<uint8_t>(rgba, 4, out);
    case TextureFormat::R8_SINT: return EncodeInts<int8_t>(rgba, 1, out);
    case TextureFormat::R8G8_SINT: return EncodeInts<int8_t>(rgba, 2, out);
    case TextureFormat::RGBA8_SINT: return EncodeInts<int8_t>(rgba, 4, out);
    case TextureFormat::R16_UINT: return EncodeInts<uint16_t>(rgba, 1, out);
    case TextureFormat::R16G16_UINT: return EncodeInts<uint16_t>(rgba, 2, out);
    case TextureFormat::RGBA16_UINT: return EncodeInts<uint16_t>(rgba, 4, out);
    case TextureFormat::R16_SINT: return EncodeInts<int16_t>(rgba, 1, out);
    case TextureFormat::R16G16_SINT: return EncodeInts<int16_t>(rgba, 2, out);
    case TextureFormat::RGBA16_SINT: return EncodeInts<int16_t>(rgba, 4, out);
    case TextureFormat::R32_UINT: return EncodeInts<uint32_t>(rgba, 1, out);
    case TextureFormat::R32G32_UINT: return EncodeInts<uint32_t>(rgba, 2, out);
    case TextureFormat::RGBA32_UINT: return EncodeInts<uint32_t>(rgba, 4, out);
    case TextureFormat::R32_SINT: return EncodeInts<int32_t>(rgba, 1, out);
    case TextureFormat::R32G32_SINT: return EncodeInts<int32_t>(rgba, 2, out);
    case TextureFormat::RGBA32_SINT: return EncodeInts<int32_t>(rgba, 4, out);

    case TextureFormat::R16G16B16A16_UNORM:
        for (uint32_t i = 0; i < 4; ++i)
            Store(out, i, static_cast<uint16_t>(Unorm(rgba[i], 16)));
        return 8;
    case TextureFormat::R16_FLOAT: return EncodeFloats<uint16_t>(rgba, 1, out);
    case TextureFormat::R16G16_FLOAT: return EncodeFloats<uint16_t>(rgba, 2, out);
    case TextureFormat::R16G16B16A16_FLOAT: return EncodeFloats<uint16_t>(rgba, 4, out);
    case TextureFormat::R32_FLOAT: return EncodeFloats<float>(rgba, 1, out);
    case TextureFormat::R32G32_FLOAT: return EncodeFloats<float>(rgba, 2, out);
    case TextureFormat::R32G32B32A32_FLOAT: return EncodeFloats<float>(rgba, 4, out);
    case TextureFormat::RGB10A2_UNORM:
        Store(out, 0, Unorm(rgba[0], 10) | (Unorm(rgba[1], 10) << 10) | (Unorm(rgba[2], 10) << 20) |
                          (Unorm(rgba[3], 2) << 30));
        return 4;
    case TextureFormat::R11G11B10_FLOAT:
        Store(out, 0, UnsignedSmallFloat(rgba[0], 6) | (UnsignedSmallFloat(rgba[1], 6) << 11) |
                          (UnsignedSmallFloat(rgba[2], 5) << 22));
        return 4;
    default:
        return 0;
    }
}

} // namespace GameEngine::Rendering
