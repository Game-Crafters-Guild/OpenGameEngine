#include <gtest/gtest.h>

#include "UI/ResolvedStyle.h"
#include "UI/UIStyle.h"

#include "../Source/StyleInterpolation.h"

using namespace GameEngine;

// ---------------------------------------------------------------------------
// IsInterpolable
// ---------------------------------------------------------------------------

TEST(StyleInterpolation, InterpolableFloatProperties)
{
    EXPECT_TRUE(IsInterpolable(StylePropertyId::Opacity));
    EXPECT_TRUE(IsInterpolable(StylePropertyId::FontSize));
    EXPECT_TRUE(IsInterpolable(StylePropertyId::FlexGrow));
}

TEST(StyleInterpolation, InterpolableColorProperties)
{
    EXPECT_TRUE(IsInterpolable(StylePropertyId::BackgroundColor));
    EXPECT_TRUE(IsInterpolable(StylePropertyId::Color));
    EXPECT_TRUE(IsInterpolable(StylePropertyId::BackgroundTint));
}

TEST(StyleInterpolation, InterpolableLengthProperties)
{
    EXPECT_TRUE(IsInterpolable(StylePropertyId::Width));
    EXPECT_TRUE(IsInterpolable(StylePropertyId::Height));
    EXPECT_TRUE(IsInterpolable(StylePropertyId::MarginTop));
    EXPECT_TRUE(IsInterpolable(StylePropertyId::PaddingLeft));
}

TEST(StyleInterpolation, InterpolableCompoundProperties)
{
    EXPECT_TRUE(IsInterpolable(StylePropertyId::BorderRadius));
    EXPECT_TRUE(IsInterpolable(StylePropertyId::BorderWidth));
    EXPECT_TRUE(IsInterpolable(StylePropertyId::BoxShadow));
    EXPECT_TRUE(IsInterpolable(StylePropertyId::Glow));
}

TEST(StyleInterpolation, NonInterpolableProperties)
{
    EXPECT_FALSE(IsInterpolable(StylePropertyId::Display));
    EXPECT_FALSE(IsInterpolable(StylePropertyId::Position));
    EXPECT_FALSE(IsInterpolable(StylePropertyId::FlexDir));
    EXPECT_FALSE(IsInterpolable(StylePropertyId::FontFamily));
    EXPECT_FALSE(IsInterpolable(StylePropertyId::Overflow));
    EXPECT_FALSE(IsInterpolable(StylePropertyId::Cursor));
}

// ---------------------------------------------------------------------------
// InterpolateProperty — float
// ---------------------------------------------------------------------------

TEST(StyleInterpolation, InterpolateFloatAtHalf)
{
    StyleValue a = 0.0f;
    StyleValue b = 1.0f;
    auto result = InterpolateProperty(StylePropertyId::Opacity, a, b, 0.5f);
    EXPECT_NEAR(std::get<float>(result), 0.5f, 0.001f);
}

TEST(StyleInterpolation, InterpolateFloatAtBoundaries)
{
    StyleValue a = 2.0f;
    StyleValue b = 8.0f;
    EXPECT_NEAR(std::get<float>(InterpolateProperty(StylePropertyId::Opacity, a, b, 0.0f)), 2.0f, 0.001f);
    EXPECT_NEAR(std::get<float>(InterpolateProperty(StylePropertyId::Opacity, a, b, 1.0f)), 8.0f, 0.001f);
}

// ---------------------------------------------------------------------------
// InterpolateProperty — color
// ---------------------------------------------------------------------------

TEST(StyleInterpolation, InterpolateColorBlackToWhite)
{
    StyleValue a = uint32_t(0xFF000000u);
    StyleValue b = uint32_t(0xFFFFFFFFu);
    auto result = InterpolateProperty(StylePropertyId::BackgroundColor, a, b, 0.5f);
    EXPECT_EQ(std::get<uint32_t>(result), 0xFF808080u);
}

TEST(StyleInterpolation, InterpolateColorIdentity)
{
    StyleValue c = uint32_t(0xAABBCCDDu);
    auto result = InterpolateProperty(StylePropertyId::Color, c, c, 0.5f);
    EXPECT_EQ(std::get<uint32_t>(result), 0xAABBCCDDu);
}

// ---------------------------------------------------------------------------
// InterpolateProperty — StyleLength
// ---------------------------------------------------------------------------

TEST(StyleInterpolation, InterpolateStyleLengthSameUnit)
{
    StyleValue a = StyleLength::Px(10.0f);
    StyleValue b = StyleLength::Px(30.0f);
    auto result = InterpolateProperty(StylePropertyId::Width, a, b, 0.5f);
    auto len = std::get<StyleLength>(result);
    EXPECT_EQ(len.Unit, StyleLength::UnitType::Px);
    EXPECT_NEAR(len.Value, 20.0f, 0.001f);
}

TEST(StyleInterpolation, InterpolateStyleLengthDifferentUnitSnapsBeforeHalf)
{
    StyleValue a = StyleLength::Px(10.0f);
    StyleValue b = StyleLength::Percent(50.0f);
    auto result = InterpolateProperty(StylePropertyId::Width, a, b, 0.3f);
    auto len = std::get<StyleLength>(result);
    EXPECT_EQ(len.Unit, StyleLength::UnitType::Px);
    EXPECT_NEAR(len.Value, 10.0f, 0.001f);
}

TEST(StyleInterpolation, InterpolateStyleLengthDifferentUnitSnapsAfterHalf)
{
    StyleValue a = StyleLength::Px(10.0f);
    StyleValue b = StyleLength::Percent(50.0f);
    auto result = InterpolateProperty(StylePropertyId::Width, a, b, 0.7f);
    auto len = std::get<StyleLength>(result);
    EXPECT_EQ(len.Unit, StyleLength::UnitType::Percent);
    EXPECT_NEAR(len.Value, 50.0f, 0.001f);
}

// ---------------------------------------------------------------------------
// InterpolateProperty — Box4
// ---------------------------------------------------------------------------

TEST(StyleInterpolation, InterpolateBox4)
{
    StyleValue a = Box4{1.0f, 2.0f, 3.0f, 4.0f};
    StyleValue b = Box4{5.0f, 6.0f, 7.0f, 8.0f};
    auto result = InterpolateProperty(StylePropertyId::BorderWidth, a, b, 0.5f);
    auto box = std::get<Box4>(result);
    EXPECT_NEAR(box.Top, 3.0f, 0.001f);
    EXPECT_NEAR(box.Right, 4.0f, 0.001f);
    EXPECT_NEAR(box.Bottom, 5.0f, 0.001f);
    EXPECT_NEAR(box.Left, 6.0f, 0.001f);
}

// ---------------------------------------------------------------------------
// InterpolateProperty — CornerRadii
// ---------------------------------------------------------------------------

TEST(StyleInterpolation, InterpolateCornerRadii)
{
    StyleValue a = CornerRadiiTLTRBRBL{0.0f, 0.0f, 0.0f, 0.0f};
    StyleValue b = CornerRadiiTLTRBRBL{10.0f, 20.0f, 30.0f, 40.0f};
    auto result = InterpolateProperty(StylePropertyId::BorderRadius, a, b, 0.5f);
    auto radii = std::get<CornerRadiiTLTRBRBL>(result);
    // Both semi-axes interpolate; the endpoints here are circular, so the
    // midpoint is too.
    EXPECT_NEAR(radii.TopLeft.X, 5.0f, 0.001f);
    EXPECT_NEAR(radii.TopLeft.Y, 5.0f, 0.001f);
    EXPECT_NEAR(radii.TopRight.X, 10.0f, 0.001f);
    EXPECT_NEAR(radii.BottomRight.X, 15.0f, 0.001f);
    EXPECT_NEAR(radii.BottomLeft.X, 20.0f, 0.001f);
}

// ---------------------------------------------------------------------------
// InterpolateProperty — BoxShadow
// ---------------------------------------------------------------------------

TEST(StyleInterpolation, InterpolateBoxShadow)
{
    StyleValue a = BoxShadowValue{0.0f, 0.0f, 0.0f, 0xFF000000u};
    StyleValue b = BoxShadowValue{10.0f, 20.0f, 16.0f, 0xFFFFFFFFu};
    auto result = InterpolateProperty(StylePropertyId::BoxShadow, a, b, 0.5f);
    auto shadow = std::get<BoxShadowValue>(result);
    EXPECT_NEAR(shadow.OffsetX, 5.0f, 0.001f);
    EXPECT_NEAR(shadow.OffsetY, 10.0f, 0.001f);
    EXPECT_NEAR(shadow.Blur, 8.0f, 0.001f);
    EXPECT_EQ(shadow.Color, 0xFF808080u);
}

// ---------------------------------------------------------------------------
// InterpolateProperty — Glow
// ---------------------------------------------------------------------------

TEST(StyleInterpolation, InterpolateGlow)
{
    StyleValue a = GlowValue{0.0f, 0xFF000000u};
    StyleValue b = GlowValue{10.0f, 0xFFFFFFFFu};
    auto result = InterpolateProperty(StylePropertyId::Glow, a, b, 0.5f);
    auto glow = std::get<GlowValue>(result);
    EXPECT_NEAR(glow.Radius, 5.0f, 0.001f);
    EXPECT_EQ(glow.Color, 0xFF808080u);
}

// ---------------------------------------------------------------------------
// ReadProperty / WriteProperty round-trip
// ---------------------------------------------------------------------------

TEST(StyleInterpolation, RoundTripOpacity)
{
    ResolvedStyle rs{};
    WriteProperty(rs, StylePropertyId::Opacity, StyleValue{0.75f});
    auto val = ReadProperty(rs, StylePropertyId::Opacity);
    EXPECT_NEAR(std::get<float>(val), 0.75f, 0.001f);
}

TEST(StyleInterpolation, RoundTripBackgroundColor)
{
    ResolvedStyle rs{};
    WriteProperty(rs, StylePropertyId::BackgroundColor, StyleValue{uint32_t(0xFF112233u)});
    auto val = ReadProperty(rs, StylePropertyId::BackgroundColor);
    EXPECT_EQ(std::get<uint32_t>(val), 0xFF112233u);
}

TEST(StyleInterpolation, RoundTripWidth)
{
    ResolvedStyle rs{};
    WriteProperty(rs, StylePropertyId::Width, StyleValue{StyleLength::Px(100.0f)});
    auto val = ReadProperty(rs, StylePropertyId::Width);
    auto len = std::get<StyleLength>(val);
    EXPECT_EQ(len.Unit, StyleLength::UnitType::Px);
    EXPECT_NEAR(len.Value, 100.0f, 0.001f);
}

TEST(StyleInterpolation, RoundTripBorderWidth)
{
    ResolvedStyle rs{};
    WriteProperty(rs, StylePropertyId::BorderWidth, StyleValue{Box4{1.0f, 2.0f, 3.0f, 4.0f}});
    auto val = ReadProperty(rs, StylePropertyId::BorderWidth);
    auto box = std::get<Box4>(val);
    EXPECT_NEAR(box.Top, 1.0f, 0.001f);
    EXPECT_NEAR(box.Right, 2.0f, 0.001f);
    EXPECT_NEAR(box.Bottom, 3.0f, 0.001f);
    EXPECT_NEAR(box.Left, 4.0f, 0.001f);
}

TEST(StyleInterpolation, RoundTripBorderRadius)
{
    ResolvedStyle rs{};
    WriteProperty(rs, StylePropertyId::BorderRadius, StyleValue{CornerRadiiTLTRBRBL{1.0f, 2.0f, 3.0f, 4.0f}});
    auto val = ReadProperty(rs, StylePropertyId::BorderRadius);
    auto radii = std::get<CornerRadiiTLTRBRBL>(val);
    // The aggregate round-trips through both semi-axes of each corner.
    EXPECT_NEAR(radii.TopLeft.X, 1.0f, 0.001f);
    EXPECT_NEAR(radii.TopLeft.Y, 1.0f, 0.001f);
    EXPECT_NEAR(radii.TopRight.X, 2.0f, 0.001f);
    EXPECT_NEAR(radii.TopRight.Y, 2.0f, 0.001f);
    EXPECT_NEAR(radii.BottomRight.X, 3.0f, 0.001f);
    EXPECT_NEAR(radii.BottomLeft.X, 4.0f, 0.001f);
}

TEST(StyleInterpolation, RoundTripBoxShadow)
{
    ResolvedStyle rs{};
    WriteProperty(rs, StylePropertyId::BoxShadow, StyleValue{BoxShadowValue{2.0f, 4.0f, 8.0f, 0xFF000000u}});
    auto val = ReadProperty(rs, StylePropertyId::BoxShadow);
    auto shadow = std::get<BoxShadowValue>(val);
    EXPECT_NEAR(shadow.OffsetX, 2.0f, 0.001f);
    EXPECT_NEAR(shadow.OffsetY, 4.0f, 0.001f);
    EXPECT_NEAR(shadow.Blur, 8.0f, 0.001f);
    EXPECT_EQ(shadow.Color, 0xFF000000u);
}

TEST(StyleInterpolation, RoundTripGlow)
{
    ResolvedStyle rs{};
    WriteProperty(rs, StylePropertyId::Glow, StyleValue{GlowValue{6.0f, 0xFF4488FFu}});
    auto val = ReadProperty(rs, StylePropertyId::Glow);
    auto glow = std::get<GlowValue>(val);
    EXPECT_NEAR(glow.Radius, 6.0f, 0.001f);
    EXPECT_EQ(glow.Color, 0xFF4488FFu);
}
