#pragma once

#include <bit>
#include <cstdint>

namespace GameEngine::Mathematics
{

inline constexpr uint16_t kHalfSignMask = 0x8000u;
inline constexpr uint16_t kHalfExponentMask = 0x7C00u;
inline constexpr uint16_t kHalfMantissaMask = 0x03FFu;

/// Exact IEEE-754 binary16 -> binary32. Subnormals stay subnormal-exact,
/// infinities stay infinite and NaN payloads are kept. A caller that wants a
/// different policy for those classes tests the bits against the masks above
/// before calling.
inline float HalfToFloat(uint16_t bits)
{
    constexpr uint32_t kHalfMantissaBits = 10u;
    constexpr uint32_t kFloatMantissaShift = 23u - kHalfMantissaBits;
    constexpr uint32_t kExponentRebias = 127u - 15u;
    constexpr uint32_t kFloatExponentMask = 0x7F800000u;
    constexpr uint32_t kHalfImplicitBit = 1u << kHalfMantissaBits;

    const uint32_t sign = static_cast<uint32_t>(bits & kHalfSignMask) << 16u;
    uint32_t exponent = static_cast<uint32_t>(bits & kHalfExponentMask) >> kHalfMantissaBits;
    uint32_t mantissa = static_cast<uint32_t>(bits & kHalfMantissaMask);

    uint32_t out = sign;
    if (exponent == 0u)
    {
        if (mantissa != 0u)
        {
            // Subnormal: normalize the mantissa into float's wider exponent range.
            exponent = 1u;
            while ((mantissa & kHalfImplicitBit) == 0u)
            {
                mantissa <<= 1u;
                --exponent;
            }
            mantissa &= kHalfMantissaMask;
            out |= ((exponent + kExponentRebias) << 23u) | (mantissa << kFloatMantissaShift);
        }
    }
    else if (exponent == (kHalfExponentMask >> kHalfMantissaBits))
    {
        out |= kFloatExponentMask | (mantissa << kFloatMantissaShift);
    }
    else
    {
        out |= ((exponent + kExponentRebias) << 23u) | (mantissa << kFloatMantissaShift);
    }
    return std::bit_cast<float>(out);
}

} // namespace GameEngine::Mathematics
