// CSS letter-spacing (issue #812), pinned from the outside in.
//
// Every reference number below was measured in Chrome (`--headless=new`,
// `--force-device-scale-factor=1`) rendering the same Roboto-Regular.ttf this
// test loads, embedded as a data: URI so no system face can substitute for it.
// Advances were read four ways that agreed: inline-block getBoundingClientRect,
// Range.getBoundingClientRect, CanvasRenderingContext2D.measureText, and a
// canvas pixel scan for ink edges. The fixture reproduces the eleven
// measureText pins already in TextGeometryChromeParityTests.cpp to 0.0000000px,
// which is what says it is measuring the same Chrome/Roboto pair those came
// from.
//
// The rule those numbers describe: letter-spacing is added once per typographic
// cluster, the run's LAST cluster included. N clusters = N x spacing, and the
// trailing gap is part of the advance, not trimmed off it:
//
//   letter-spacing:5px on "A"  -> +5.00px (a run of one cluster still tracks)
//   "HEALTH" 14px, 2px         -> +12.00px (six clusters, not five gaps)
//   per-caret deltas           -> 0,2,4,6,8,10,12 (the end caret carries a gap)
//   right-aligned "SCORE" 2px  -> ink left -10, ink right -2, box flush at the
//                                 container edge in both arms
//
// Two consequences worth stating because they are what make this rule the right
// one rather than merely the measured one: sub-runs compose (a prefix measured
// alone plus the rest equals the whole, so a highlight built from a prefix lands
// on its match), and the caret at end-of-text equals the measured width.
//
// Non-zero spacing also suppresses optional ligatures (CSS Text 3 8.2). Chrome's
// tracked "fi" is exactly the DE-LIGATED width plus 2 x spacing (9.4375 + 4 =
// 13.4375), never the ligated 8.859375 + spacing — otherwise a word's tracking
// would depend on which letter pairs happen to ligate.

#include "IsolatedUIFixture.h"
#include "RobotoTestFont.h"

#include "Rendering/Text/TextLayout.h"
#include "UI/Controls/TextField.h"
#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace GameEngine;
using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::LoadRobotoAtlas;

namespace
{

// Spacing is added in render px, so a delta between two arms of the same text is
// an exact float addition. 0.01px keeps it falsifiable against per-glyph
// application and against a dropped trailing gap, while tolerating the
// atlas-space round-trip's ulp noise.
constexpr float kExactPx = 0.01f;

// Absolute agreement with Chrome is looser than that: Chrome quantises advances
// to 1/64px and the engine shapes at the 20px atlas ppem before scaling to the
// render size. Same constant as TextGeometryChromeParityTests.
constexpr float kChromePx = 0.1f;

// Min/max glyph ink edges over every Slug primitive an element emitted, in
// physical px. Labels here use ASCII-only text, so every glyph is a Slug quad.
struct InkSpan
{
    float MinX = 1e30f;
    float MaxX = -1e30f;
};

InkSpan GlyphInkSpan(const IsolatedUIFixture& fx, const std::string& id)
{
    InkSpan span{};
    for (const UI::UIPrimitive& p : fx.Primitives(id, UI::PrimitiveMode::Slug))
    {
        span.MinX = std::min(span.MinX, p.X);
        span.MaxX = std::max(span.MaxX, p.X + p.W);
    }
    return span;
}

} // namespace

// --- The kill-shot, end to end: CSS in, layout width out ---------------------
//
// Two labels, identical but for the letter-spacing declaration. The tracked
// one's border box must be wider by clusters x spacing; at base (letter-spacing
// parsed and dropped) the two boxes are identical, which is the bug.

TEST(LetterSpacing, HealthAt14pxWith2pxTrackingIsTwelveWiderThanUntracked)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <label id="tracked">HEALTH</label>
  <label id="untracked">HEALTH</label>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 400px; height: 300px; }
label { font-family: Roboto; font-size: 14px; color: #ffffff; }
#tracked { letter-spacing: 2px; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_EQ(fx.ResolvedFontFamily("tracked"), "Roboto")
        << "staged Roboto missing; widths below would pin a system face";

    const float tracked = fx.BorderBox("tracked").W;
    const float untracked = fx.BorderBox("untracked").W;
    // HEALTH: six clusters, six gaps, 2px each (Chrome: 51.0625 -> 63.0625).
    EXPECT_NEAR(tracked, untracked + 12.0f, kExactPx);
}

TEST(LetterSpacing, ScoreDigitsAt30pxWith1pxTrackingAreSixWiderThanUntracked)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <label id="tracked">12,450</label>
  <label id="untracked">12,450</label>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 400px; height: 300px; }
label { font-family: Roboto; font-size: 30px; color: #ffffff; }
#tracked { letter-spacing: 1px; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_EQ(fx.ResolvedFontFamily("tracked"), "Roboto");

    const float tracked = fx.BorderBox("tracked").W;
    const float untracked = fx.BorderBox("untracked").W;
    // "12,450": six clusters, 1px each (Chrome: 90.125 -> 96.125).
    EXPECT_NEAR(tracked, untracked + 6.0f, kExactPx);
}

// --- The untracked control ---------------------------------------------------
//
// Absent, `0`, and `normal` are the same computed value and none of them may
// move a run: each equals the direct FontAtlas measurement of the same text at
// the same device size. This is the byte-stability control for the whole
// existing suite — it must pass BEFORE the fix lands and stay green after.

TEST(LetterSpacing, ZeroAndNormalAndAbsentAgreeWithTheDirectMeasurement)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <label id="absent">78 / 100</label>
  <label id="zero">78 / 100</label>
  <label id="normal">78 / 100</label>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 400px; height: 300px; }
label { font-family: Roboto; font-size: 16px; color: #ffffff; }
#zero { letter-spacing: 0px; }
#normal { letter-spacing: normal; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_EQ(fx.ResolvedFontFamily("absent"), "Roboto");

    const float absent = fx.BorderBox("absent").W;
    EXPECT_NEAR(fx.BorderBox("zero").W, absent, kExactPx);
    EXPECT_NEAR(fx.BorderBox("normal").W, absent, kExactPx);

    // The engine-vs-engine identity: the label's box is exactly MeasureUtf8 for
    // the same text and device size — untracked, because letter-spacing is
    // absent/zero/normal here — with no allowance for the SDF quad dilation,
    // which is paint bleed and not part of the box.
    auto atlas = LoadRobotoAtlas();
    ASSERT_TRUE(atlas);
    const float expected = atlas->MeasureUtf8("78 / 100", 16.0f).width;
    EXPECT_NEAR(absent, expected, kExactPx);
}

// --- The most visible symptom: right-aligned runs shift ----------------------
//
// The HUD score panel is right-aligned. The trailing gap is inside the advance,
// so the alignment offset (box - lineWidth) parks the whole run one gap further
// left than the untracked one: the LAST glyph's ink lands exactly `spacing` left
// of where it was, and the first glyph's ink `clusters x spacing` left. Chrome
// pixel-scan on right-aligned "SCORE" at 2px: ink left -10, ink right -2.

TEST(LetterSpacing, RightAlignedTrackedRunPullsBothInkEdgesLeft)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <label id="tracked">HEALTH</label>
  <label id="untracked">HEALTH</label>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: column; width: 300px; height: 300px; }
label { font-family: Roboto; font-size: 14px; color: #ffffff; text-align: right; }
#tracked { letter-spacing: 2px; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_EQ(fx.ResolvedFontFamily("tracked"), "Roboto");

    const InkSpan tracked = GlyphInkSpan(fx, "tracked");
    const InkSpan untracked = GlyphInkSpan(fx, "untracked");
    ASSERT_LT(tracked.MinX, tracked.MaxX);
    ASSERT_LT(untracked.MinX, untracked.MaxX);

    // Chrome, same arms: ink left -12 (six clusters x 2), ink right -2.
    EXPECT_NEAR(tracked.MaxX, untracked.MaxX - 2.0f, kExactPx);
    EXPECT_NEAR(tracked.MinX, untracked.MinX - 12.0f, kExactPx);
}

// --- Inheritance -------------------------------------------------------------
//
// letter-spacing inherits (CSS Text 4). A label with no declaration of its own
// under a container that declares 2px must track exactly as if it declared it.

TEST(LetterSpacing, InheritsFromAnAncestorContainer)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <uielement id="panel">
    <label id="inherited">HEALTH</label>
  </uielement>
  <label id="untracked">HEALTH</label>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 400px; height: 300px; }
#panel { display: flex; flex-direction: column; align-items: flex-start; letter-spacing: 2px; }
label { font-family: Roboto; font-size: 14px; color: #ffffff; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_EQ(fx.ResolvedFontFamily("inherited"), "Roboto");

    EXPECT_NEAR(fx.BorderBox("inherited").W, fx.BorderBox("untracked").W + 12.0f, kExactPx);
}

// A pseudo-state on a CONTAINER changes an inherited value, so the descendants
// that inherit it must be re-emitted even though nothing about them changed.
// The label below is fixed-size, so its box never moves and no layout path can
// queue it: if the inherited-property invalidation list in
// UIManager_Layout.cpp does not carry LetterSpacing, its glyphs keep the
// positions they were emitted with and the hover visibly does nothing.
TEST(LetterSpacing, HoverOnAContainerRepaintsDescendantsThatInheritTheSpacing)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <uielement id="panel">
    <label id="child">HEALTH</label>
  </uielement>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 400px; height: 300px; }
#panel { display: flex; flex-direction: column; width: 300px; height: 100px; }
#panel:hover { letter-spacing: 4px; }
label { font-family: Roboto; font-size: 14px; color: #ffffff; width: 250px; height: 20px; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_EQ(fx.ResolvedFontFamily("child"), "Roboto");

    const InkSpan before = GlyphInkSpan(fx, "child");
    ASSERT_LT(before.MinX, before.MaxX);
    ASSERT_EQ(fx.Style("child")->Visual.LetterSpacing, 0.0f);

    // Pointer into the panel: hover is a paint-only change here (the label's box
    // is fixed), which is exactly the frame shape the invalidation list exists
    // for.
    fx.Manager().OnMouseMove(20.0f, 20.0f);
    fx.Settle();

    ASSERT_EQ(fx.Style("child")->Visual.LetterSpacing, 4.0f)
        << "hover did not reach the child's resolved style; the rest of this test is vacuous";
    const InkSpan after = GlyphInkSpan(fx, "child");
    // Five gaps between the six glyphs' ink, the trailing one sitting past the
    // last glyph: the ink extent grows by 5 x 4.
    EXPECT_NEAR(after.MaxX - after.MinX, (before.MaxX - before.MinX) + 20.0f, kExactPx)
        << "the label's glyphs were never re-emitted with the inherited spacing";
}

// --- FontAtlas API: the single definition every consumer shares ---------------

// Absolute agreement with Chrome, not just the right delta: a rule that added
// the correct number of gaps to a wrong base width would pass a delta-only pin.
TEST(LetterSpacingAtlas, TrackedWidthsMatchChrome)
{
    auto atlas = LoadRobotoAtlas();
    if (!atlas)
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    struct Case
    {
        const char* Text;
        float Px;
        float Spacing;
        float ChromeUntracked;
        float ChromeTracked;
    };
    const Case cases[] = {
        {"HEALTH", 14.0f, 2.0f, 51.0625f, 63.0625f},
        {"HEALTH", 14.0f, -1.0f, 51.0625f, 45.0625f},
        {"HEALTH", 14.0f, 0.5f, 51.0625f, 54.0625f},
        {"12,450", 30.0f, 1.0f, 90.125f, 96.125f},
        {"SCORE", 14.0f, 2.0f, 43.625f, 53.625f},
        {"A", 14.0f, 5.0f, 9.140625f, 14.140625f},
        {"78 / 100", 16.0f, 2.0f, 59.4375f, 75.4375f},
        {"cache probe", 16.0f, 3.0f, 87.515625f, 120.515625f},
    };
    for (const auto& c : cases)
    {
        EXPECT_NEAR(atlas->MeasureUtf8(c.Text, c.Px).width, c.ChromeUntracked, kChromePx)
            << "\"" << c.Text << "\" px=" << c.Px << " untracked";
        EXPECT_NEAR(atlas->MeasureUtf8(c.Text, c.Px, c.Spacing).width, c.ChromeTracked, kChromePx)
            << "\"" << c.Text << "\" px=" << c.Px << " ls=" << c.Spacing;
    }
}

// The same rule stated as an exact arithmetic identity: one gap per cluster,
// including the last one. "A" is the discriminator — a run of one cluster has
// no INTERNAL gap, so an implementation that spaces between clusters leaves it
// unchanged where Chrome makes it 5px wider.
TEST(LetterSpacingAtlas, MeasuredWidthGrowsByExactlyClustersTimesSpacing)
{
    auto atlas = LoadRobotoAtlas();
    if (!atlas)
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    struct Case
    {
        const char* Text;
        float Px;
        float Spacing;
        float Clusters;
    };
    const Case cases[] = {
        {"HEALTH", 14.0f, 2.0f, 6.0f},   // the issue kill-shot row
        {"12,450", 30.0f, 1.0f, 6.0f},   // the 30px score row
        {"HEALTH", 14.0f, -1.0f, 6.0f},  // negative spacing tightens
        {"A", 14.0f, 5.0f, 1.0f},        // one cluster still takes its gap
        {"78 / 100", 16.0f, 2.0f, 8.0f}, // spaces are clusters too
    };
    for (const auto& c : cases)
    {
        const float untracked = atlas->MeasureUtf8(c.Text, c.Px).width;
        const float tracked = atlas->MeasureUtf8(c.Text, c.Px, c.Spacing).width;
        EXPECT_NEAR(tracked, untracked + c.Clusters * c.Spacing, kExactPx)
            << "\"" << c.Text << "\" px=" << c.Px << " ls=" << c.Spacing;
    }
}

// A combining mark shares its base's cluster. "x" + U+0301 has no precomposed
// glyph in Roboto, so it stays TWO glyphs inside ONE cluster: per-glyph spacing
// would add two gaps and fail, per-cluster adds one. Chrome: 7.9375 -> 9.9375
// at 16px with 2px.
TEST(LetterSpacingAtlas, CombiningMarksStayInsideTheirCluster)
{
    auto atlas = LoadRobotoAtlas();
    if (!atlas)
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    const std::string xAcute = "x\xCC\x81";      // x, U+0301 COMBINING ACUTE ACCENT
    const std::string xAcuteY = "x\xCC\x81y";    // ... plus a second cluster
    const std::string eAcuteX = "e\xCC\x81x";    // e + U+0301 composes via ccmp, then x

    // The discriminator is only a discriminator if the run really is multi-glyph
    // inside one cluster.
    Rendering::Text::FontAtlas::ShapeResult shaped;
    atlas->ShapeText(xAcute, 16.0f, shaped);
    ASSERT_EQ(shaped.glyphs.size(), 2u)
        << "x + U+0301 shaped to one glyph; this fixture no longer separates "
           "per-glyph from per-cluster spacing";

    EXPECT_NEAR(atlas->MeasureUtf8(xAcute, 16.0f, 2.0f).width,
                atlas->MeasureUtf8(xAcute, 16.0f).width + 2.0f, kExactPx);
    EXPECT_NEAR(atlas->MeasureUtf8(xAcute, 16.0f, 2.0f).width, 9.9375f, kChromePx);

    EXPECT_NEAR(atlas->MeasureUtf8(xAcuteY, 16.0f, 2.0f).width,
                atlas->MeasureUtf8(xAcuteY, 16.0f).width + 4.0f, kExactPx);
    EXPECT_NEAR(atlas->MeasureUtf8(xAcuteY, 16.0f, 2.0f).width, 19.5f, kChromePx);

    EXPECT_NEAR(atlas->MeasureUtf8(eAcuteX, 16.0f, 2.0f).width,
                atlas->MeasureUtf8(eAcuteX, 16.0f).width + 4.0f, kExactPx);
    EXPECT_NEAR(atlas->MeasureUtf8(eAcuteX, 16.0f, 2.0f).width, 20.40625f, kChromePx);
}

// CSS Text 3 8.2: optional ligatures must not survive non-zero spacing. The
// tracked width is the DE-LIGATED run plus one gap per cluster, so "fi" tracks
// as two clusters and is 13.4375 wide at 16px/2px — the ligated 8.859375 plus
// spacing would be narrower whatever gap rule was used, and would make tracking
// depend on which pairs ligate.
TEST(LetterSpacingAtlas, OptionalLigaturesAreSuppressedUnderTracking)
{
    auto atlas = LoadRobotoAtlas();
    if (!atlas)
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    struct Case
    {
        const char* Text;
        float ChromeLigatedUntracked;
        float ChromeTracked2px; // de-ligated + clusters x 2
        size_t Clusters;
    };
    const Case cases[] = {
        {"fi", 8.859375f, 13.4375f, 2},
        {"fl", 9.09375f, 13.4375f, 2},
        {"ffi", 14.421875f, 21.0f, 3},
        {"ffl", 14.640625f, 21.0f, 3},
    };
    for (const auto& c : cases)
    {
        EXPECT_NEAR(atlas->MeasureUtf8(c.Text, 16.0f).width, c.ChromeLigatedUntracked, kChromePx)
            << c.Text << " untracked must still ligate";
        EXPECT_NEAR(atlas->MeasureUtf8(c.Text, 16.0f, 2.0f).width, c.ChromeTracked2px, kChromePx)
            << c.Text << " tracked";

        // Same claim from the glyph side: the ligature is one glyph untracked
        // and the run de-ligates to one glyph per character when tracked.
        Rendering::Text::FontAtlas::ShapeResult ligated, deligated;
        atlas->ShapeText(c.Text, 16.0f, ligated);
        atlas->ShapeText(c.Text, 16.0f, deligated, 0xFF000000u, 2.0f);
        EXPECT_LT(ligated.glyphs.size(), c.Clusters) << c.Text << " did not ligate untracked";
        EXPECT_EQ(deligated.glyphs.size(), c.Clusters) << c.Text << " did not de-ligate tracked";
    }

    // "ff" ligates to a glyph whose advance equals f+f, so it is the control:
    // suppression must not move a width that does not depend on it.
    EXPECT_NEAR(atlas->MeasureUtf8("ff", 16.0f).width, 11.109375f, kChromePx);
    EXPECT_NEAR(atlas->MeasureUtf8("ff", 16.0f, 2.0f).width, 15.109375f, kChromePx);
}

// Caret positions shift by the spacing accumulated before them, and the final
// insertion point equals the measured width — carets, hit testing and
// measurement stay one geometry.
TEST(LetterSpacingAtlas, CaretMapShiftsPerClusterAndEndsAtTheMeasuredWidth)
{
    auto atlas = LoadRobotoAtlas();
    if (!atlas)
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    const std::string text = "HEALTH";
    const float px = 14.0f;
    const float ls = 2.0f;

    std::vector<float> untracked, tracked;
    ASSERT_TRUE(atlas->BuildCaretMapUtf8(text, px, untracked));
    ASSERT_TRUE(atlas->BuildCaretMapUtf8(text, px, tracked, ls));
    ASSERT_EQ(untracked.size(), text.size() + 1);
    ASSERT_EQ(tracked.size(), text.size() + 1);

    // Caret i sits after i gaps — the end-of-text caret included, which is what
    // keeps it equal to the measured width.
    for (size_t i = 0; i <= text.size(); ++i)
        EXPECT_NEAR(tracked[i], untracked[i] + static_cast<float>(i) * ls, kExactPx) << "byte " << i;

    // Chrome's own Range widths for the same tracked string.
    const float kChromeCarets[] = {0.0f,      11.984375f, 21.9375f,  33.078125f,
                                   40.734375f, 51.078125f, 63.0625f};
    ASSERT_EQ(tracked.size(), std::size(kChromeCarets));
    for (size_t i = 0; i < tracked.size(); ++i)
        EXPECT_NEAR(tracked[i], kChromeCarets[i], kChromePx) << "byte " << i;

    EXPECT_NEAR(tracked.back(), atlas->MeasureUtf8(text, px, ls).width, kExactPx);
}

// Sub-runs compose. Measuring a prefix on its own and adding the rest must equal
// measuring the whole run, and a caret map built from a prefix must end where
// the full run's caret for that byte is. This is the property the ScriptTextArea
// search highlight is built on: it locates a match by measuring the line prefix
// and sizes the box by measuring the match text. Isolating the SPACING
// contribution (tracked minus untracked, per piece) keeps the pin exact —
// shaping a prefix separately can lose a kerning pair, which is not what this
// test is about.
TEST(LetterSpacingAtlas, SubRunsComposeIntoTheWholeRun)
{
    auto atlas = LoadRobotoAtlas();
    if (!atlas)
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    const float px = 14.0f;
    const float ls = 2.0f;
    for (const std::string& text : {std::string("HEALTH"), std::string("12,450")})
    {
        std::vector<float> fullTracked, fullUntracked;
        ASSERT_TRUE(atlas->BuildCaretMapUtf8(text, px, fullUntracked));
        ASSERT_TRUE(atlas->BuildCaretMapUtf8(text, px, fullTracked, ls));

        const float whole = atlas->MeasureUtf8(text, px, ls).width -
                            atlas->MeasureUtf8(text, px).width;

        for (size_t split = 0; split <= text.size(); ++split)
        {
            SCOPED_TRACE(::testing::Message() << text << " split at " << split);
            const std::string head = text.substr(0, split);
            const std::string tail = text.substr(split);

            const float headSpacing = atlas->MeasureUtf8(head, px, ls).width -
                                      atlas->MeasureUtf8(head, px).width;
            const float tailSpacing = atlas->MeasureUtf8(tail, px, ls).width -
                                      atlas->MeasureUtf8(tail, px).width;
            EXPECT_NEAR(headSpacing + tailSpacing, whole, kExactPx);

            // The same identity through the caret map the highlight actually
            // uses: the prefix's end caret carries exactly the spacing the full
            // run has accumulated at that byte.
            std::vector<float> headTracked, headUntracked;
            ASSERT_TRUE(atlas->BuildCaretMapUtf8(head, px, headUntracked));
            ASSERT_TRUE(atlas->BuildCaretMapUtf8(head, px, headTracked, ls));
            EXPECT_NEAR(headTracked.back() - headUntracked.back(),
                        fullTracked[split] - fullUntracked[split], kExactPx);
        }
    }
}

// Emitted glyph quads move with the tracked pen: glyph i sits i gaps right of
// its untracked position, and the reported advance width matches the measure
// path. The ink extent grows by five gaps, not six — the sixth sits past the
// last glyph, inside the advance.
TEST(LetterSpacingAtlas, ShapedGlyphQuadsAndMetricsCarryTheTracking)
{
    auto atlas = LoadRobotoAtlas();
    if (!atlas)
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    const std::string text = "HEALTH";
    const float px = 14.0f;
    const float ls = 2.0f;

    Rendering::Text::FontAtlas::ShapeResult untracked, tracked;
    atlas->ShapeText(text, px, untracked);
    atlas->ShapeText(text, px, tracked, 0xFF000000u, ls);
    ASSERT_EQ(untracked.glyphs.size(), text.size());
    ASSERT_EQ(tracked.glyphs.size(), text.size());

    for (size_t i = 0; i < text.size(); ++i)
    {
        EXPECT_NEAR(tracked.glyphs[i].x, untracked.glyphs[i].x + static_cast<float>(i) * ls,
                    kExactPx)
            << "glyph " << i;
        EXPECT_NEAR(tracked.glyphs[i].width, untracked.glyphs[i].width, kExactPx)
            << "glyph " << i << " (tracking must not scale the quad)";
    }

    const auto inkRight = [](const Rendering::Text::FontAtlas::ShapeResult& r)
    { return r.glyphs.back().x + r.glyphs.back().width; };
    EXPECT_NEAR(inkRight(tracked), inkRight(untracked) + 5.0f * ls, kExactPx);
    EXPECT_NEAR(tracked.metrics.width, untracked.metrics.width + 6.0f * ls, kExactPx);
}

// The shaped-width LRU keys on the ligature arm as well as (text, atlas px), so
// a tracked measure and an untracked one of the same string are separate
// entries: neither can be served the other's glyph run. Within an arm one entry
// still serves every spacing — the width is stored without it.
TEST(LetterSpacingAtlas, WidthCacheKeepsTheTrackedAndUntrackedArmsApart)
{
    auto atlas = LoadRobotoAtlas();
    if (!atlas)
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";
    auto reference = LoadRobotoAtlas(); // independent atlas: cold, untracked truth
    ASSERT_TRUE(reference);

    const std::string text = "cache probe";
    const float px = 16.0f;
    const float truth = reference->MeasureUtf8(text, px).width;

    // Tracked first (populates the cache), untracked second (must not inherit
    // the tracking), tracked again (served from cache, must still track).
    const float tracked1 = atlas->MeasureUtf8(text, px, 3.0f).width;
    const float untracked = atlas->MeasureUtf8(text, px).width;
    const float tracked2 = atlas->MeasureUtf8(text, px, 3.0f).width;

    EXPECT_NEAR(untracked, truth, kExactPx);
    EXPECT_NEAR(tracked1, truth + 11.0f * 3.0f, kExactPx); // 11 clusters
    EXPECT_NEAR(tracked2, tracked1, kExactPx);

    // One entry per arm, every spacing: a second spacing on the same text is
    // served from the tracked entry and must still be exact.
    EXPECT_NEAR(atlas->MeasureUtf8(text, px, 1.0f).width, truth + 11.0f, kExactPx);

    // A ligating string is where a shared entry would be caught: the two arms
    // have different glyph runs, so serving one from the other's cached width
    // is off by the de-ligation delta even before spacing.
    for (int pass = 0; pass < 2; ++pass)
    {
        EXPECT_NEAR(atlas->MeasureUtf8("fi", 16.0f, 2.0f).width, 13.4375f, kChromePx) << "pass " << pass;
        EXPECT_NEAR(atlas->MeasureUtf8("fi", 16.0f).width, 8.859375f, kChromePx) << "pass " << pass;
    }
}

// Line breaking sees tracked advances. At the 150px fixture width Chrome moves
// the first break from after "fox" to before it, and the whole paragraph
// repacks; the pins below are Chrome's line texts in both arms. Each reported
// line width must be the tracked width of its own text.
TEST(LetterSpacingAtlas, WrappingBreaksEarlierUnderTracking)
{
    auto atlas = LoadRobotoAtlas();
    if (!atlas)
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    using Rendering::Text::StyledRun;
    using Rendering::Text::TextLayout;

    const std::string text = "The quick brown fox jumps over the lazy dog";
    StyledRun run{};
    run.Font = atlas.get();
    run.PixelSize = 16.0f;
    run.Text = text;

    const auto untracked = TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1),
                                                      150.0f, 0.0f, TextLayout::WordBreak::Normal);
    run.LetterSpacingPx = 2.0f;
    const auto tracked = TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1),
                                                    150.0f, 0.0f, TextLayout::WordBreak::Normal);

    const auto lineTexts = [&text](const auto& result)
    {
        std::vector<std::string> out;
        for (const auto& line : result.LineBreaks)
            out.push_back(text.substr(line.ByteStart, line.ByteEnd - line.ByteStart));
        return out;
    };

    // Chrome at width 150px, 16px Roboto.
    EXPECT_EQ(lineTexts(untracked),
              (std::vector<std::string>{"The quick brown fox ", "jumps over the lazy ", "dog"}));
    EXPECT_EQ(lineTexts(tracked),
              (std::vector<std::string>{"The quick brown ", "fox jumps over ", "the lazy dog"}));

    // Every tracked line's reported width is the tracked measure of its bytes
    // minus any hanging trailing whitespace — the same rule as untracked.
    for (const auto& line : tracked.LineBreaks)
    {
        std::string lineText = text.substr(line.ByteStart, line.ByteEnd - line.ByteStart);
        while (!lineText.empty() && lineText.back() == ' ')
            lineText.pop_back();
        const float expected = atlas->MeasureUtf8(lineText, 16.0f, 2.0f).width;
        EXPECT_NEAR(line.Width, expected, 0.05f) << "line \"" << lineText << "\"";
    }
}

// --- Caret and hit-testing through a tracked TextInput ------------------------
//
// The TextFieldPointerTests pattern, with tracking: clicking each tracked
// caret position must land the caret on that byte. The pointer path builds its
// geometry from style.Visual.LetterSpacing, so this exercises the control's own
// plumbing, not just the atlas.

TEST(LetterSpacingPointer, ClickAtTrackedCaretBoundariesPlacesCaretCorrectly)
{
    auto atlas = LoadRobotoAtlas();
    if (!atlas)
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    const std::string text = "HEALTH";
    const float px = 14.0f;
    const float ls = 2.0f;
    const float padL = 4.0f;
    const float fieldX = 50.0f, fieldY = 40.0f, fieldW = 200.0f, fieldH = 24.0f;

    const auto measure = atlas->MeasureText(text, px, ls);
    ASSERT_EQ(measure.caretXByByte.size(), text.size() + 1);

    ResolvedStyle style{};
    style.Layout.Padding.Left = padL;
    style.Layout.Padding.Right = 4.0f;
    style.Layout.Padding.Top = style.Layout.Padding.Bottom = 2.0f;
    style.Visual.FontSize = px;
    style.Visual.LetterSpacing = ls;
    style.Visual.TextAlign = TextAlign::Left;
    style.Visual.Color = 0xFF000000u;

    for (size_t expectedCaret = 0; expectedCaret <= text.size(); ++expectedCaret)
    {
        SCOPED_TRACE(::testing::Message() << "expectedCaret=" << expectedCaret);

        TextInput tf;
        tf.SetId("tracked-pointer-test");
        tf.SetValue(text);
        tf.OnFocusChanged(true);

        const float mouseX = fieldX + padL + measure.caretXByByte[expectedCaret];
        tf.OnPointerDown(mouseX, fieldY + fieldH / 2.0f, fieldX, fieldY, fieldW, fieldH,
                         style, atlas.get());

        EXPECT_EQ(tf.GetCaretIndex(), static_cast<int>(expectedCaret));
    }

    // The discriminating pin: at byte 3 the tracked caret sits three gaps right
    // of the untracked one, so the pointer path demonstrably used the tracked
    // geometry rather than agreeing by accident.
    const auto untrackedMeasure = atlas->MeasureText(text, px);
    EXPECT_NEAR(measure.caretXByByte[3], untrackedMeasure.caretXByByte[3] + 3.0f * ls, kExactPx);
}
