// The shared curve editor's display options: a logarithmic value axis that places a key where the
// log-domain evaluator reads it, a reference line that is only drawn (never grabbed), and a
// playback indicator whose left-edge value marker a read-only graph can leave out.

#include <gtest/gtest.h>

#include "Mathematics/Curve.h"
#include "UI/Controls/CurveField.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIPrimitive.h"
#include "UI/UITextureRegistry.h"
#include "RobotoTestFont.h"

#include <cmath>
#include <optional>
#include <vector>

using namespace GameEngine;

namespace
{

constexpr float kFieldWidthPx = 400.0f;
constexpr float kFieldHeightPx = 200.0f;
// Sweeps start above and end below the field so a key handle drawn on an edge is swept whole.
constexpr float kSweepMarginPx = 20.0f;
constexpr float kSweepStepPx = 0.5f;

struct Point
{
    float X = 0.0f;
    float Y = 0.0f;
};

Math::CurveKey Key(float time, float value)
{
    Math::CurveKey key;
    key.Time = time;
    key.Value = value;
    return key;
}

// The field reads its own layout rect when it hit-tests, so a test that never runs a layout pass
// writes the rect the solver would have committed.
void PlaceField(CurveField& field, const CurveField::Config& config, const std::vector<Math::CurveKey>& keys)
{
    UILayoutAccess::SetLastLayoutRect(field, 0.0f, 0.0f, kFieldWidthPx, kFieldHeightPx);
    field.SetConfig(config);
    field.SetKeys(keys);
}

void SendMouse(CurveField& field, EventId id, Point at, int button = 0)
{
    UIEvent e{};
    e.Id = id;
    e.X = at.X;
    e.Y = at.Y;
    e.Button = button;
    e.Target = &field;
    e.CurrentTarget = &field;
    field.DispatchEvent(e);
}

// A key's handle is layout-derived, so find it the way a pointer does: press every point of the
// field and keep those that select the key. The pick area is a disc around the handle, so the
// centroid of the points that pick it is the handle's centre.
std::optional<Point> HandleCentre(CurveField& field, int keyIndex)
{
    double sumX = 0.0;
    double sumY = 0.0;
    int hits = 0;
    for (float y = -kSweepMarginPx; y < kFieldHeightPx + kSweepMarginPx; y += kSweepStepPx)
    {
        for (float x = -kSweepMarginPx; x < kFieldWidthPx + kSweepMarginPx; x += kSweepStepPx)
        {
            SendMouse(field, kEventMouseDown, {x, y});
            SendMouse(field, kEventMouseUp, {x, y});
            if (field.GetSelectedKey() == keyIndex)
            {
                sumX += x;
                sumY += y;
                ++hits;
            }
        }
    }
    if (hits == 0)
        return std::nullopt;
    return Point{static_cast<float>(sumX / hits), static_cast<float>(sumY / hits)};
}

size_t PrimitiveCount(CurveField& field)
{
    std::vector<UI::UIPrimitive> primitives;
    UI::PrimitiveEmitContext ctx{primitives, /*ClipIndex=*/0, /*Opacity=*/1.0f};
    const ResolvedStyle style{};
    field.OnGeneratePrimitives(ctx, style, 0.0f, 0.0f, kFieldWidthPx, kFieldHeightPx);
    return primitives.size();
}

} // namespace

// On a logarithmic axis from 1 to 10 000, the key at 100 (the geometric mean) sits halfway up
// between the keys at 1 and 10 000; a linear axis would put it one percent of the way up.
TEST(CurveField, LogarithmicAxisPlacesTheGeometricMeanHalfwayUp)
{
    CurveField field;
    CurveField::Config config;
    config.ValueMin = 1.0f;
    config.ValueMax = 10000.0f;
    config.LogarithmicValues = true;
    config.LogarithmicFloor = 0.001f;
    config.AllowTimeDrag = false;
    config.AllowAddRemove = false;
    PlaceField(field, config, {Key(0.0f, 1.0f), Key(0.5f, 100.0f), Key(1.0f, 10000.0f)});

    const std::optional<Point> bottom = HandleCentre(field, 0);
    const std::optional<Point> middle = HandleCentre(field, 1);
    const std::optional<Point> top = HandleCentre(field, 2);
    ASSERT_TRUE(bottom && middle && top);
    EXPECT_NEAR(middle->Y, 0.5f * (bottom->Y + top->Y), 1.0f);
}

// A logarithmic axis keeps its bottom: a key below it sits on the bottom edge, with the key at the
// bottom value, and the keys above keep their places.
TEST(CurveField, AKeyBelowTheLogarithmicAxisSitsOnItsBottomEdge)
{
    CurveField field;
    CurveField::Config config;
    config.ValueMin = 1.0f;
    config.ValueMax = 10000.0f;
    config.LogarithmicValues = true;
    config.LogarithmicFloor = 0.001f;
    config.AllowTimeDrag = false;
    config.AllowAddRemove = false;
    PlaceField(field, config, {Key(0.0f, 0.01f), Key(0.5f, 1.0f), Key(1.0f, 10000.0f)});

    const std::optional<Point> below = HandleCentre(field, 0);
    const std::optional<Point> bottom = HandleCentre(field, 1);
    const std::optional<Point> top = HandleCentre(field, 2);
    ASSERT_TRUE(below && bottom && top);
    EXPECT_NEAR(below->Y, bottom->Y, 1.0f) << "the key under the axis stretched it down";
    EXPECT_GT(bottom->Y - top->Y, 0.8f * kFieldHeightPx) << "1 lx is the axis's bottom";
}

// A reference line drawn through the graph is never picked: pressing and dragging all along it
// selects no key, moves no key and commits nothing.
TEST(CurveField, ReferenceLineIsNotEditable)
{
    CurveField field;
    CurveField::Config config;
    config.AllowAddRemove = false;
    PlaceField(field, config, {Key(0.0f, 0.0f), Key(1.0f, 0.0f)});
    std::vector<Math::CurveKey> reference;
    for (int i = 0; i <= 8; ++i)
        reference.push_back(Key(static_cast<float>(i) / 8.0f, 1.0f));
    field.SetReferenceLine(reference);
    int commits = 0;
    field.SetOnChanged([&commits](const std::vector<Math::CurveKey>&) { ++commits; });

    const std::vector<Math::CurveKey> before = field.GetKeys();
    for (float y = 0.0f; y < 0.5f * kFieldHeightPx; y += 1.0f)
    {
        for (float x = 0.0f; x < kFieldWidthPx; x += 4.0f)
        {
            SendMouse(field, kEventMouseDown, {x, y});
            SendMouse(field, kEventMouseMove, {x, y + 30.0f});
            SendMouse(field, kEventMouseUp, {x, y + 30.0f});
            ASSERT_EQ(field.GetSelectedKey(), -1) << "at " << x << ", " << y;
        }
    }
    ASSERT_EQ(field.GetKeys().size(), before.size());
    for (size_t i = 0; i < before.size(); ++i)
    {
        EXPECT_EQ(field.GetKeys()[i].Time, before[i].Time);
        EXPECT_EQ(field.GetKeys()[i].Value, before[i].Value);
    }
    EXPECT_EQ(commits, 0);
}

// A read-only graph whose value is read out elsewhere drops the playback indicator's left-edge
// marker and nothing else.
TEST(CurveField, PlaybackValueMarkerCanBeLeftOut)
{
    CurveField field;
    CurveField::Config config;
    config.ReadOnly = true;
    config.ShowPlaybackIndicator = true;
    PlaceField(field, config, {Key(0.0f, 0.0f), Key(1.0f, 1.0f)});
    field.SetPlaybackIndicator(0.5f, 0.5f);
    const size_t withMarker = PrimitiveCount(field);

    config.ShowPlaybackValueMarker = false;
    field.SetConfig(config);
    EXPECT_EQ(PrimitiveCount(field), withMarker - 1);
}

// The hour-label strip is sized from the resolved font size, so a larger label font grows the
// control by exactly the larger strip; below 12 px the strip keeps its 12 px size.
TEST(CurveField, TheLabelStripFollowsTheResolvedFontSize)
{
    CurveField field;
    CurveField::Config config;
    config.TimeAxisLabels = CurveField::TimeLabels::HoursOfDay;
    field.SetConfig(config);
    const auto heightAt = [&field](float fontSize) {
        field.GetMutableResolvedStyle().Visual.FontSize = fontSize;
        field.OnPostLayout();
        return field.Overrides().Get(Style::Height).value_or(StyleLength::Px(0.0f)).Value;
    };
    const float at12 = heightAt(12.0f);
    const float at20 = heightAt(20.0f);
    EXPECT_GT(at12, 110.0f);
    EXPECT_NEAR(at20 - at12, (20.0f - 12.0f) * 1.3f, 1e-3f);
    EXPECT_FLOAT_EQ(heightAt(9.0f), at12);
}

// A host sheet sizes a labelled graph's plot with --curve-field-plot-height; the label strip is added
// under it, so the plot keeps the height the host asked for.
TEST(CurveField, AHostSetsTheLabelledPlotsHeight)
{
    CurveField field;
    CurveField::Config config;
    config.TimeAxisLabels = CurveField::TimeLabels::HoursOfDay;
    field.SetConfig(config);
    field.GetMutableResolvedStyle().Visual.FontSize = 12.0f;
    field.OnPostLayout();
    const float defaultHeight = field.Overrides().Get(Style::Height).value_or(StyleLength::Px(0.0f)).Value;

    auto scope = std::make_shared<CustomPropertyScope>();
    scope->UniqueId = 1;
    scope->Local.InsertOrAssign(HashStringId("--curve-field-plot-height"), CustomPropertyScope::Entry{false, "200px"});
    field.GetMutableResolvedStyle().CustomScope = scope;
    field.OnPostLayout();
    EXPECT_FLOAT_EQ(field.Overrides().Get(Style::Height).value_or(StyleLength::Px(0.0f)).Value,
                    defaultHeight + (200.0f - 110.0f));
}

TEST(CurveField, NarrowHourLabelsUseTwelveHourSteps)
{
    auto font = UITesting::LoadRobotoAtlas();
    ASSERT_NE(font, nullptr) << "Roboto-Regular.ttf must be staged beside the test executable";
    UI::UITextureRegistry textures(nullptr);
    CurveField field;
    CurveField::Config config;
    config.TimeMin = 0.0f;
    config.TimeMax = 24.0f;
    config.TimeAxisLabels = CurveField::TimeLabels::HoursOfDay;
    config.ReadOnly = true;
    config.ShowPlaybackIndicator = false;
    field.SetConfig(config);
    for (const float width : {180.0f, 360.0f})
    {
        SCOPED_TRACE(width);
        std::vector<UI::UIPrimitive> primitives;
        UI::PrimitiveEmitContext context{primitives, 0, 1.0f};
        context.FontAtlas = font.get();
        context.Textures = &textures;
        const ResolvedStyle style{};
        field.OnGeneratePrimitives(context, style, 0.0f, 0.0f, width, kFieldHeightPx);
        std::vector<UI::UIPrimitive> glyphs;
        for (const auto& primitive : primitives)
            if (UI::GetMode(primitive.ModeAndFlags) == UI::PrimitiveMode::Slug)
                glyphs.push_back(primitive);
        const std::string_view expectedText = width < 200.0f ? "00:0012:0024:00" : "00:0006:0012:0018:0024:00";
        std::vector<UI::UIPrimitive> expected;
        UI::PrimitiveEmitContext expectedContext{expected, 0, 1.0f};
        expectedContext.Textures = &textures;
        expectedContext.EmitText(expectedText, 0.0f, 0.0f, 12.0f, style.Visual.Color, font.get());
        ASSERT_EQ(glyphs.size(), expectedText.size());
        ASSERT_EQ(glyphs.size(), expected.size());
        for (size_t index = 0; index < glyphs.size(); ++index)
        {
            EXPECT_EQ(std::bit_cast<uint32_t>(glyphs[index].BorderWidths[0]),
                      std::bit_cast<uint32_t>(expected[index].BorderWidths[0]));
            EXPECT_EQ(std::bit_cast<uint32_t>(glyphs[index].BorderWidths[1]),
                      std::bit_cast<uint32_t>(expected[index].BorderWidths[1]));
        }
        const float minimumGap = font->MeasureUtf8("000", 12.0f).width;
        for (size_t index = 5; index < glyphs.size(); index += 5)
            EXPECT_GE(glyphs[index].X - (glyphs[index - 1].X + glyphs[index - 1].W), minimumGap);
    }
}

TEST(CurveField, DragReadoutUsesTheResolvedTextColor)
{
    auto font = UITesting::LoadRobotoAtlas();
    ASSERT_NE(font, nullptr) << "Roboto-Regular.ttf must be staged beside the test executable";
    UI::UITextureRegistry textures(nullptr);
    CurveField field;
    CurveField::Config config;
    config.AllowAddRemove = false;
    PlaceField(field, config, {Key(0.0f, 0.0f), Key(0.5f, 0.5f), Key(1.0f, 1.0f)});
    const auto centre = HandleCentre(field, 1);
    ASSERT_TRUE(centre);
    SendMouse(field, kEventMouseDown, *centre);
    SendMouse(field, kEventMouseMove, {centre->X + 20.0f, centre->Y - 20.0f});

    std::vector<UI::UIPrimitive> primitives;
    UI::PrimitiveEmitContext context{primitives, 0, 1.0f};
    context.FontAtlas = font.get();
    context.Textures = &textures;
    ResolvedStyle style;
    style.Visual.Color = 0xFF1245C7u;
    field.OnGeneratePrimitives(context, style, 0.0f, 0.0f, kFieldWidthPx, kFieldHeightPx);
    size_t glyphCount = 0;
    for (const auto& primitive : primitives)
    {
        if (UI::GetMode(primitive.ModeAndFlags) != UI::PrimitiveMode::Slug)
            continue;
        ++glyphCount;
        EXPECT_EQ(primitive.FillColor, UI::PackFromARGB(style.Visual.Color));
    }
    EXPECT_GT(glyphCount, 0u) << "drag readout emitted no glyphs";
    SendMouse(field, kEventMouseUp, {centre->X + 20.0f, centre->Y - 20.0f});
}
namespace
{
// The glyphs the field paints, as primitives, with a staged font.
size_t GlyphCount(CurveField& field, Rendering::Text::FontAtlas* font, UI::UITextureRegistry& textures)
{
    std::vector<UI::UIPrimitive> primitives;
    UI::PrimitiveEmitContext context{primitives, 0, 1.0f};
    context.FontAtlas = font;
    context.Textures = &textures;
    const ResolvedStyle style{};
    field.OnGeneratePrimitives(context, style, 0.0f, 0.0f, kFieldWidthPx, kFieldHeightPx);
    size_t glyphs = 0;
    for (const auto& primitive : primitives)
        if (UI::GetMode(primitive.ModeAndFlags) == UI::PrimitiveMode::Slug)
            ++glyphs;
    return glyphs;
}
} // namespace

// The value axis says what it measures: each mark inside the axis is labelled with ValueLabel, a mark
// outside it is not drawn, and the playback value marker carries the value it marks.
TEST(CurveField, ValueAxisMarksAndThePlaybackValueAreLabelled)
{
    auto font = UITesting::LoadRobotoAtlas();
    ASSERT_NE(font, nullptr) << "Roboto-Regular.ttf must be staged beside the test executable";
    UI::UITextureRegistry textures(nullptr);
    CurveField field;
    CurveField::Config config;
    config.ReadOnly = true;
    config.LogarithmicValues = true;
    config.LogarithmicFloor = 1.0f;
    config.ValueMin = 1.0f;
    config.ValueMax = 10000.0f;
    PlaceField(field, config, {Key(0.0f, 1.0f), Key(1.0f, 10000.0f)});
    const size_t unlabelled = GlyphCount(field, font.get(), textures);

    config.ValueAxisMarks = {1.0f, 100.0f, 1e9f};
    config.ValueLabel = [](float value) { return value >= 100.0f ? std::string("100") : std::string("1"); };
    field.SetConfig(config);
    EXPECT_EQ(GlyphCount(field, font.get(), textures), unlabelled + 4) << "the marks at 1 and 100 are labelled";

    config.ShowPlaybackIndicator = true;
    field.SetConfig(config);
    field.SetPlaybackIndicator(0.5f, 100.0f);
    EXPECT_EQ(GlyphCount(field, font.get(), textures), unlabelled + 7) << "the playback value is labelled";
}

// A shaded span is drawn behind the curve with its label, and only inside the graph.
TEST(CurveField, AShadedSpanIsDrawnWithItsLabel)
{
    auto font = UITesting::LoadRobotoAtlas();
    ASSERT_NE(font, nullptr) << "Roboto-Regular.ttf must be staged beside the test executable";
    UI::UITextureRegistry textures(nullptr);
    CurveField field;
    CurveField::Config config;
    config.ReadOnly = true;
    PlaceField(field, config, {Key(0.0f, 0.0f), Key(1.0f, 1.0f)});
    const size_t before = PrimitiveCount(field);
    const size_t glyphsBefore = GlyphCount(field, font.get(), textures);
    field.SetShadedSpans({{0.75f, 1.0f, "moon"}});
    EXPECT_EQ(PrimitiveCount(field), before + 1) << "one shaded rectangle";
    EXPECT_EQ(GlyphCount(field, font.get(), textures), glyphsBefore + 4) << "labelled \"moon\"";
    field.SetShadedSpans({});
    EXPECT_EQ(PrimitiveCount(field), before);
}

// A logarithmic graph without a floor above 0 is a misuse (log2 of a dark key would be minus
// infinity), and SetConfig asserts rather than drawing an axis that reaches down to 1e-38.
#if !defined(NDEBUG)
TEST(CurveFieldDeathTest, ALogarithmicGraphWithoutAFloorAsserts)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    CurveField::Config config;
    config.LogarithmicValues = true;
    config.LogarithmicFloor = 0.0f;
    ASSERT_DEATH(
        {
            CurveField field;
            field.SetConfig(config);
        },
        "LogarithmicFloor");
}
#endif
