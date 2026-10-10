// GitHub #757 — `overflow-wrap` and `word-break` are two independent CSS
// properties (css-text-3 5.1 and 5.4), and `word-wrap` is the legacy alias for
// OVERFLOW-WRAP, not for word-break.
//
// The specimen below is the declaration pair the editor actually ships
// (Apps/Editor/Assets/UI/panels/SettingsPanel.css:348-349 and :531-532):
//
//     overflow-wrap: break-word;
//     word-break: normal;
//
// Two properties sharing one storage slot make that pair order-sensitive — the
// second declaration overwrites the first — so the engine renders one
// overflowing line where a browser renders three. Declaration order is
// therefore the instrument here: the same two declarations in either order must
// produce the same layout, and only independent storage can do that.
//
// Both breakers are pinned, because they are separate implementations that must
// agree and either one left unfixed still ships a broken panel:
//
//   the Yoga measure callback   UIManager_Layout.cpp   -> the element's HEIGHT
//   the shaping/emit path       Text/TextLayout.cpp    -> the emitted GLYPH ROWS
//
// Nothing here derives an expectation from the code under test. The grounds are
// the CSS the test authored (an explicit `line-height` makes n lines exactly
// n * line-height tall), and a same-sheet control whose only difference is a
// declaration this property is supposed to be independent of.

#include "IsolatedUIFixture.h"

#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using GameEngine::UI::PrimitiveMode;
using GameEngine::UI::UIPrimitive;
using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;

namespace
{

// One word, no space, no hyphen: it offers the line breaker no opportunity at
// all, so it wraps if and only if a property says an overflowing word may be
// broken. At 16px Roboto it is far wider than the 120px box.
constexpr char kUnbreakableWord[] = "Supercalifragilisticexpialidocious";

constexpr char kXml[] = R"(<uielement id="root">
  <label id="wrapThenBreak" text="Supercalifragilisticexpialidocious"/>
  <label id="breakThenWrap" text="Supercalifragilisticexpialidocious"/>
  <label id="legacyWordWrap" text="Supercalifragilisticexpialidocious"/>
  <label id="breakAllThenWrapNormal" text="Supercalifragilisticexpialidocious"/>
  <label id="neither" text="Supercalifragilisticexpialidocious"/>
</uielement>)";

// `height` is deliberately absent: each label is sized by the measure callback,
// so its border box reports how many lines that callback decided on.
//
// #wrapThenBreak and #breakThenWrap carry the SAME two declarations in opposite
// order. #legacyWordWrap uses the `word-wrap` alias, which css-text-3 5.4
// defines as an alias of overflow-wrap. #neither is the control: no
// overflow-wrap at all, so its initial value (`normal`) must leave the word
// unbroken and overflowing.
constexpr char kCss[] = R"(
#root { display: flex; flex-direction: column; width: 600px; height: 500px; }
#wrapThenBreak, #breakThenWrap, #legacyWordWrap, #breakAllThenWrapNormal, #neither {
  width: 120px;
  font-family: Roboto;
  font-size: 16px;
  line-height: 20px;
  padding: 0px;
  border-width: 0px;
  color: #ffffff;
}
#wrapThenBreak  { overflow-wrap: break-word; word-break: normal; }
#breakThenWrap  { word-break: normal; overflow-wrap: break-word; }
#legacyWordWrap { word-wrap: break-word; word-break: normal; }
#breakAllThenWrapNormal { word-break: break-all; overflow-wrap: normal; }
#neither        { word-break: normal; }
)";

constexpr float kLineHeightPx = 20.0f;

// Ink boxes vary in height within a row (an ascender starts above an x-height
// letter) but consecutive rows are a full 20px line box apart, so any threshold
// comfortably inside that gap separates rows without splitting one.
constexpr float kRowGapThresholdPx = 10.0f;

constexpr float kHeightEpsilonPx = 0.75f;

size_t CountGlyphRows(const std::vector<UIPrimitive>& glyphs)
{
    if (glyphs.empty())
        return 0;
    std::vector<float> tops;
    tops.reserve(glyphs.size());
    for (const UIPrimitive& g : glyphs)
        tops.push_back(g.Y);
    std::sort(tops.begin(), tops.end());

    size_t rows = 1;
    for (size_t i = 1; i < tops.size(); ++i)
    {
        if (tops[i] - tops[i - 1] > kRowGapThresholdPx)
            ++rows;
    }
    return rows;
}

size_t MeasuredLines(const IsolatedUIFixture& fx, const std::string& id)
{
    const float h = fx.BorderBox(id).H;
    return static_cast<size_t>(std::lround(h / kLineHeightPx));
}

// A shared build: every case reads off one cascade over one sheet, so a
// difference between two ids is the declarations and nothing else.
struct Specimen
{
    IsolatedUIFixture Fx;

    bool Build()
    {
        const bool built = Fx.Build(1.0f, kXml, kCss);
        return built && Fx.ResolvedFontFamily("neither") == "Roboto";
    }

    size_t Rows(const std::string& id) const
    {
        return CountGlyphRows(Fx.Primitives(id, PrimitiveMode::Slug));
    }
};

} // namespace

// The shipped SettingsPanel pair. `overflow-wrap: break-word` must survive a
// later `word-break: normal` in the same block, in both breakers.
TEST(OverflowWrap, BreakWordSurvivesALaterWordBreakNormal)
{
    Specimen s;
    const bool built = s.Build();
    if (!s.Fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << s.Fx.Diagnostic()
                       << " (resolved family: " << s.Fx.ResolvedFontFamily("neither") << ")";

    EXPECT_GT(MeasuredLines(s.Fx, "wrapThenBreak"), 1u);
    EXPECT_GT(s.Rows("wrapThenBreak"), 1u);
}

// Order-independence is the property that only separate storage can give. The
// two ids differ by nothing except which of the two declarations is written
// first, so any difference between them is the shared slot.
TEST(OverflowWrap, DeclarationOrderDoesNotChangeTheLayout)
{
    Specimen s;
    const bool built = s.Build();
    if (!s.Fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << s.Fx.Diagnostic();

    EXPECT_EQ(MeasuredLines(s.Fx, "wrapThenBreak"), MeasuredLines(s.Fx, "breakThenWrap"));
    EXPECT_EQ(s.Rows("wrapThenBreak"), s.Rows("breakThenWrap"));
}

// `word-wrap` aliases overflow-wrap, so it must survive the same later
// `word-break: normal` that `overflow-wrap` does.
TEST(OverflowWrap, LegacyWordWrapAliasesOverflowWrapNotWordBreak)
{
    Specimen s;
    const bool built = s.Build();
    if (!s.Fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << s.Fx.Diagnostic();

    // Stated absolutely as well as relatively: an equality against
    // #wrapThenBreak alone would be satisfied by both being broken.
    EXPECT_GT(MeasuredLines(s.Fx, "legacyWordWrap"), 1u);
    EXPECT_GT(s.Rows("legacyWordWrap"), 1u);
    EXPECT_EQ(MeasuredLines(s.Fx, "legacyWordWrap"), MeasuredLines(s.Fx, "wrapThenBreak"));
    EXPECT_EQ(s.Rows("legacyWordWrap"), s.Rows("wrapThenBreak"));
}

// The other half of the shared slot, which the cases above cannot see: they
// only ever check that overflow-wrap survives word-break. A single slot loses
// whichever declaration is written first, so `word-break: break-all` must
// equally survive a later `overflow-wrap: normal`. Chrome 150 lays this pair
// out over three lines at this width; a shared slot leaves one overflowing
// line, because `overflow-wrap: normal` parses to the same Normal that
// `word-break: normal` does and overwrites break-all.
TEST(OverflowWrap, BreakAllSurvivesALaterOverflowWrapNormal)
{
    Specimen s;
    const bool built = s.Build();
    if (!s.Fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << s.Fx.Diagnostic();

    EXPECT_GT(MeasuredLines(s.Fx, "breakAllThenWrapNormal"), 1u);
    EXPECT_GT(s.Rows("breakAllThenWrapNormal"), 1u);
}

// The control. Initial `overflow-wrap` is `normal`, so an unbreakable word
// still overflows on one line — a fix that simply made everything breakable
// would fail here.
TEST(OverflowWrap, InitialValueLeavesAnUnbreakableWordOnOneLine)
{
    Specimen s;
    const bool built = s.Build();
    if (!s.Fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << s.Fx.Diagnostic();

    EXPECT_NEAR(s.Fx.BorderBox("neither").H, kLineHeightPx, kHeightEpsilonPx);
    EXPECT_EQ(s.Rows("neither"), 1u);

    // ...and it does overflow: the specimen is only a valid instrument if the
    // word is genuinely wider than the box it is in.
    const auto glyphs = s.Fx.Primitives("neither", PrimitiveMode::Slug);
    ASSERT_FALSE(glyphs.empty());
    float left = glyphs.front().X;
    float right = glyphs.front().X + glyphs.front().W;
    for (const UIPrimitive& g : glyphs)
    {
        left = std::min(left, g.X);
        right = std::max(right, g.X + g.W);
    }
    EXPECT_GT(right - left, s.Fx.BorderBox("neither").W);
    EXPECT_GT(std::string(kUnbreakableWord).size(), 20u);
}
