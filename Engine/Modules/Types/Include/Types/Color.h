#pragma once

#include <cstdint>
#include <algorithm>
#include <cmath>

namespace GameEngine {

// Forward declarations so the structs can reference each other
struct ColorSRGB;
struct ColorHSV;

// Canonical linear RGBA color used by engine/rendering code
struct ColorLinear
{
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
	float a = 1.0f;

	ColorLinear() = default;
	ColorLinear(float rValue, float gValue, float bValue, float aValue = 1.0f)
		: r(rValue), g(gValue), b(bValue), a(aValue) {}

	// Exact per-channel comparison; != is the rewritten negation.
	bool operator==(const ColorLinear&) const = default;

	// Create directly from linear [0,1] components
	static ColorLinear FromLinear01(float rValue, float gValue, float bValue, float aValue = 1.0f)
	{
		return ColorLinear(rValue, gValue, bValue, aValue);
	}

	// Convert this linear color to sRGB-space representation in [0,1]
	ColorSRGB ToSRGB() const;
};

// sRGB-encoded RGBA color used for UI / serialization / user-facing values
struct ColorSRGB
{
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
	float a = 1.0f;

	ColorSRGB() = default;
	ColorSRGB(float rValue, float gValue, float bValue, float aValue = 1.0f)
		: r(rValue), g(gValue), b(bValue), a(aValue) {}

	// Exact per-channel comparison; != is the rewritten negation.
	bool operator==(const ColorSRGB&) const = default;

	// Create from 0-1 sRGB components (e.g., UI sliders)
	static ColorSRGB FromSRGB01(float rValue, float gValue, float bValue, float aValue = 1.0f)
	{
		return ColorSRGB(rValue, gValue, bValue, aValue);
	}

	// Create from 0-255 sRGB components (e.g., CSS bytes)
	static ColorSRGB FromSRGB255(std::uint8_t rValue, std::uint8_t gValue, std::uint8_t bValue, std::uint8_t aValue = 255)
	{
		constexpr float kInv255 = 1.0f / 255.0f;
		return ColorSRGB(
			static_cast<float>(rValue) * kInv255,
			static_cast<float>(gValue) * kInv255,
			static_cast<float>(bValue) * kInv255,
			static_cast<float>(aValue) * kInv255);
	}

	// Convert this sRGB color to linear-space representation
	ColorLinear ToLinear() const;

	// Convert this sRGB color to HSV representation
	ColorHSV ToHSV() const;
};

// HSV (Hue-Saturation-Value) color representation for UI color pickers
struct ColorHSV
{
	float h = 0.0f;   // hue 0..360
	float s = 0.0f;   // saturation 0..1
	float v = 0.0f;   // value 0..1
	float a = 1.0f;   // alpha 0..1

	ColorHSV() = default;
	ColorHSV(float hValue, float sValue, float vValue, float aValue = 1.0f)
		: h(hValue), s(sValue), v(vValue), a(aValue) {}

	// Exact per-channel comparison; != is the rewritten negation. Hue is not
	// wrapped, so 0 and 360 compare unequal.
	bool operator==(const ColorHSV&) const = default;

	// Create from HSV components (h: 0-360, s: 0-1, v: 0-1)
	static ColorHSV FromHSV(float hValue, float sValue, float vValue, float aValue = 1.0f)
	{
		return ColorHSV(hValue, sValue, vValue, aValue);
	}

	// Create from ARGB packed integer (0xAARRGGBB)
	static ColorHSV FromARGB(std::uint32_t argb);

	// Convert this HSV color to sRGB representation
	ColorSRGB ToSRGB() const;

	// Convert to ARGB packed integer (0xAARRGGBB)
	std::uint32_t ToARGB() const;
};

// Inline implementation helpers (file-local)
namespace Detail
{
	inline float Clamp01(float value)
	{
		return std::max(0.0f, std::min(1.0f, value));
	}

	inline float SRGBChannelToLinear(float c)
	{
		float clamped = Clamp01(c);
		if (clamped <= 0.04045f)
		{
			return clamped / 12.92f;
		}
		return std::pow((clamped + 0.055f) / 1.055f, 2.4f);
	}

	inline float LinearChannelToSRGB(float c)
	{
		float clamped = Clamp01(c);
		if (clamped <= 0.0031308f)
		{
			return 12.92f * clamped;
		}
		return 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
	}
} // namespace Detail

// Implementations that depend on the helpers above
inline ColorLinear ColorSRGB::ToLinear() const
{
	using namespace Detail;
	return ColorLinear(
		SRGBChannelToLinear(r),
		SRGBChannelToLinear(g),
		SRGBChannelToLinear(b),
		Clamp01(a));
}

inline ColorSRGB ColorLinear::ToSRGB() const
{
	using namespace Detail;
	return ColorSRGB(
		LinearChannelToSRGB(r),
		LinearChannelToSRGB(g),
		LinearChannelToSRGB(b),
		Clamp01(a));
}

// HSV to sRGB conversion
inline ColorSRGB ColorHSV::ToSRGB() const
{
	using namespace Detail;
	float r, g, b;
	if (s <= 0.0f)
	{
		r = g = b = v;
	}
	else
	{
		float hNorm = std::fmod(h, 360.0f);
		if (hNorm < 0.0f) hNorm += 360.0f;
		float c = v * s;
		float x = c * (1.0f - std::abs(std::fmod(hNorm / 60.0f, 2.0f) - 1.0f));
		float m = v - c;
		if (hNorm < 60.0f)
			r = c, g = x, b = 0.0f;
		else if (hNorm < 120.0f)
			r = x, g = c, b = 0.0f;
		else if (hNorm < 180.0f)
			r = 0.0f, g = c, b = x;
		else if (hNorm < 240.0f)
			r = 0.0f, g = x, b = c;
		else if (hNorm < 300.0f)
			r = x, g = 0.0f, b = c;
		else
			r = c, g = 0.0f, b = x;
		r += m;
		g += m;
		b += m;
	}
	return ColorSRGB(Clamp01(r), Clamp01(g), Clamp01(b), Clamp01(a));
}

// sRGB to HSV conversion
inline ColorHSV ColorSRGB::ToHSV() const
{
	using namespace Detail;
	float mx = std::max({r, g, b});
	float mn = std::min({r, g, b});
	float v = mx;
	float d = mx - mn;
	float h = 0.0f, s = 0.0f;
	if (mx > 0.0f && d > 0.0f)
	{
		s = d / mx;
		if (r >= mx)
			h = 60.0f * (0.0f + (g - b) / d);
		else if (g >= mx)
			h = 60.0f * (2.0f + (b - r) / d);
		else
			h = 60.0f * (4.0f + (r - g) / d);
		if (h < 0.0f)
			h += 360.0f;
	}
	return ColorHSV(h, s, v, Clamp01(a));
}

// HSV from ARGB packed integer
inline ColorHSV ColorHSV::FromARGB(std::uint32_t argb)
{
	std::uint8_t a8 = static_cast<std::uint8_t>((argb >> 24) & 0xFF);
	std::uint8_t r8 = static_cast<std::uint8_t>((argb >> 16) & 0xFF);
	std::uint8_t g8 = static_cast<std::uint8_t>((argb >> 8) & 0xFF);
	std::uint8_t b8 = static_cast<std::uint8_t>(argb & 0xFF);
	ColorSRGB srgb = ColorSRGB::FromSRGB255(r8, g8, b8, a8);
	return srgb.ToHSV();
}

// HSV to ARGB packed integer
inline std::uint32_t ColorHSV::ToARGB() const
{
	using namespace Detail;
	ColorSRGB srgb = ToSRGB();
	return (static_cast<std::uint32_t>(Clamp01(srgb.a) * 255.0f) << 24) |
	       (static_cast<std::uint32_t>(Clamp01(srgb.r) * 255.0f) << 16) |
	       (static_cast<std::uint32_t>(Clamp01(srgb.g) * 255.0f) << 8) |
	       static_cast<std::uint32_t>(Clamp01(srgb.b) * 255.0f);
}

// Convenience alias: Color in engine code means linear color
using Color = ColorLinear;

} // namespace GameEngine
