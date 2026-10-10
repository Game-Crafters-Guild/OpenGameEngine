// ReadbackSourceSpaceTests.cpp - ReadbackToRgba8Srgb takes its transfer-curve
// decision from the STATED source space, never from the readback format (#767 P5).
//
// The discriminating case is R16G16B16A16_FLOAT: the pre-P5 converter read "F16"
// and encoded unconditionally, so an F16 target holding already-encoded bytes came
// out double-encoded. Here the same format under SrgbAuthored must quantize without
// the curve, and under a linear space must apply it — one format, two outputs.
//
// Expectations are computed from the sRGB piecewise definition in this file, and the
// load-bearing ones are also pinned to hand-computed byte anchors so the test cannot
// pass by agreeing with a wrong implementation of the same formula.

#include "Engine/Rendering/ViewReadbackUtils.h"
#include "UI/UITextureSpace.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

using GameEngine::Rendering::ReadbackToRgba8Srgb;
using GameEngine::Rendering::TextureFormat;
using GameEngine::Rendering::ViewReadbackResult;
using GameEngine::UI::UITextureSpace;

namespace
{
// IEEE-754 binary16 bit patterns for exactly representable test values.
constexpr uint16_t kHalfZero    = 0x0000; // 0.0
constexpr uint16_t kHalfQuarter = 0x3400; // 0.25
constexpr uint16_t kHalfHalf    = 0x3800; // 0.5
constexpr uint16_t kHalfOne     = 0x3C00; // 1.0
constexpr uint16_t kHalfTwo     = 0x4000; // 2.0 — above SDR paper white

uint8_t RefEncode(double linear)
{
    const double c = linear < 0.0 ? 0.0 : (linear > 1.0 ? 1.0 : linear);
    const double s = c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    return static_cast<uint8_t>(s * 255.0 + 0.5);
}

uint8_t RefQuantize(double v)
{
    const double c = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    return static_cast<uint8_t>(c * 255.0 + 0.5);
}

ViewReadbackResult MakeHalfRgba(const std::vector<uint16_t>& halves, uint32_t width, uint32_t height)
{
    ViewReadbackResult r;
    r.width = width;
    r.height = height;
    r.format = TextureFormat::R16G16B16A16_FLOAT;
    r.pixels.resize(halves.size() * 2);
    std::memcpy(r.pixels.data(), halves.data(), r.pixels.size());
    return r;
}

ViewReadbackResult MakeBytes(TextureFormat format, std::vector<uint8_t> bytes, uint32_t width,
                             uint32_t height)
{
    ViewReadbackResult r;
    r.width = width;
    r.height = height;
    r.format = format;
    r.pixels = std::move(bytes);
    return r;
}
} // namespace

// The single format that used to imply "linear" now yields two different results,
// selected only by the caller's stated space.
TEST(ReadbackSourceSpace, FloatSourceEncodesUnderLinearAndPassesThroughUnderEncoded)
{
    // One pixel per channel value so a single run covers the curve's whole range.
    const std::vector<uint16_t> px = {kHalfQuarter, kHalfHalf, kHalfOne, kHalfOne};
    const ViewReadbackResult rb = MakeHalfRgba(px, 1, 1);

    const std::vector<uint8_t> linear = ReadbackToRgba8Srgb(rb, UITextureSpace::DisplayLinearSdr());
    ASSERT_EQ(linear.size(), 4u);
    EXPECT_EQ(linear[0], RefEncode(0.25));
    EXPECT_EQ(linear[1], RefEncode(0.5));
    EXPECT_EQ(linear[2], RefEncode(1.0));
    // Hand-computed anchors: 1.055*0.25^(1/2.4)-0.055 = 0.53715 -> 137;
    //                        1.055*0.5^(1/2.4)-0.055  = 0.73536 -> 188.
    EXPECT_EQ(linear[0], 137u);
    EXPECT_EQ(linear[1], 188u);
    EXPECT_EQ(linear[2], 255u);

    const std::vector<uint8_t> encoded = ReadbackToRgba8Srgb(rb, UITextureSpace::SrgbAuthored());
    ASSERT_EQ(encoded.size(), 4u);
    EXPECT_EQ(encoded[0], RefQuantize(0.25));
    EXPECT_EQ(encoded[1], RefQuantize(0.5));
    EXPECT_EQ(encoded[2], RefQuantize(1.0));
    EXPECT_EQ(encoded[0], 64u);
    EXPECT_EQ(encoded[1], 128u);
    EXPECT_EQ(encoded[2], 255u);

    // The whole point: same bytes, same format, different answer.
    EXPECT_NE(linear[1], encoded[1]);
}

// Float sources emit opaque alpha whatever the space says; alpha is coverage and
// never takes the transfer curve.
TEST(ReadbackSourceSpace, FloatSourceAlphaIsOpaqueAndUncurved)
{
    const std::vector<uint16_t> px = {kHalfZero, kHalfZero, kHalfZero, kHalfHalf};
    const ViewReadbackResult rb = MakeHalfRgba(px, 1, 1);

    EXPECT_EQ(ReadbackToRgba8Srgb(rb, UITextureSpace::DisplayLinearSdr())[3], 255u);
    EXPECT_EQ(ReadbackToRgba8Srgb(rb, UITextureSpace::SrgbAuthored())[3], 255u);
}

// HdrLinear is a linear space for this converter and clips at paper white: the
// output is an 8-bit SDR artifact either way. It must agree with DisplayLinearSdr
// in [0,1] rather than silently taking the encoded arm.
TEST(ReadbackSourceSpace, HdrLinearTakesTheSdrCurveAndClipsAbovePaperWhite)
{
    const std::vector<uint16_t> px = {kHalfHalf, kHalfOne, kHalfTwo, kHalfOne};
    const ViewReadbackResult rb = MakeHalfRgba(px, 1, 1);

    const std::vector<uint8_t> hdr = ReadbackToRgba8Srgb(rb, UITextureSpace::HdrLinear());
    const std::vector<uint8_t> sdr = ReadbackToRgba8Srgb(rb, UITextureSpace::DisplayLinearSdr());
    ASSERT_EQ(hdr.size(), 4u);
    EXPECT_EQ(hdr, sdr);
    EXPECT_EQ(hdr[0], RefEncode(0.5));
    EXPECT_EQ(hdr[2], 255u) << "2.0 must clip, not wrap";
}

// 8-bit arms: encoded-at-rest is a copy (BGRA also swizzles), which is what every
// shipped caller of those formats declares.
TEST(ReadbackSourceSpace, EightBitEncodedSourcesCopyAndSwizzleOnly)
{
    const std::vector<uint8_t> rgba = {10u, 20u, 30u, 40u};
    const std::vector<uint8_t> outRgba =
        ReadbackToRgba8Srgb(MakeBytes(TextureFormat::RGBA8_UNORM, rgba, 1, 1),
                            UITextureSpace::SrgbAuthored());
    EXPECT_EQ(outRgba, rgba);

    // Source order is B,G,R,A.
    const std::vector<uint8_t> bgra = {30u, 20u, 10u, 40u};
    const std::vector<uint8_t> outBgra =
        ReadbackToRgba8Srgb(MakeBytes(TextureFormat::BGRA8_UNORM, bgra, 1, 1),
                            UITextureSpace::SrgbAuthored());
    EXPECT_EQ(outBgra, rgba);
}

// An 8-bit source stated linear takes the curve in 8 bits and keeps its alpha —
// the mirror of the F16 case, and the reason the space cannot be read off "8-bit".
TEST(ReadbackSourceSpace, EightBitLinearSourceTakesTheCurveAndKeepsAlpha)
{
    const std::vector<uint8_t> rgba = {64u, 128u, 255u, 40u};
    const std::vector<uint8_t> out =
        ReadbackToRgba8Srgb(MakeBytes(TextureFormat::RGBA8_UNORM, rgba, 1, 1),
                            UITextureSpace::DisplayLinearSdr());
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0], RefEncode(64.0 / 255.0));
    EXPECT_EQ(out[1], RefEncode(128.0 / 255.0));
    EXPECT_EQ(out[2], 255u);
    EXPECT_EQ(out[0], 137u);
    EXPECT_EQ(out[1], 188u);
    EXPECT_EQ(out[3], 40u) << "alpha is never curve-mapped";
}

// Truncated or empty readbacks stay empty rather than reading past the buffer.
TEST(ReadbackSourceSpace, ShortAndEmptyReadbacksYieldEmpty)
{
    EXPECT_TRUE(ReadbackToRgba8Srgb(MakeBytes(TextureFormat::RGBA8_UNORM, {}, 4, 4),
                                    UITextureSpace::SrgbAuthored())
                    .empty());
    // 4x4 needs 64 bytes; hand it 8.
    EXPECT_TRUE(ReadbackToRgba8Srgb(
                    MakeBytes(TextureFormat::RGBA8_UNORM, std::vector<uint8_t>(8u, 0u), 4, 4),
                    UITextureSpace::SrgbAuthored())
                    .empty());
    EXPECT_TRUE(ReadbackToRgba8Srgb(MakeBytes(TextureFormat::D32_FLOAT,
                                              std::vector<uint8_t>(16u, 0u), 2, 2),
                                    UITextureSpace::DisplayLinearSdr())
                    .empty())
        << "depth carries no colour space; the converter refuses it";
}

// The predicate the readback consumers share: exactly one space is encoded at rest.
TEST(ReadbackSourceSpace, OnlySrgbAuthoredIsEncodedAtRest)
{
    EXPECT_TRUE(GameEngine::UI::IsEncodedAtRest(UITextureSpace::SrgbAuthored()));
    EXPECT_FALSE(GameEngine::UI::IsEncodedAtRest(UITextureSpace::DisplayLinearSdr()));
    EXPECT_FALSE(GameEngine::UI::IsEncodedAtRest(UITextureSpace::HdrLinear()));
}
