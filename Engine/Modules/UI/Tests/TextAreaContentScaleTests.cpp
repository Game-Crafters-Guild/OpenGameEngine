// GitHub #733 — TextArea derives its text metrics in LOGICAL px while
// everything around them is in PHYSICAL px.
//
// The defect is TWO independent sites, and either one left unfixed still ships
// a broken control:
//
//   TextArea.cpp:339   out.Px = fsize;                         <- glyph raster
//   TextArea.cpp:344   ResolveLineBoxPx(..., /*scale=*/1.0f);  <- line box
//   TextArea.cpp:1202  "(x, y) arrive in physical px"          <- physical
//   TextArea.cpp:1205  padding/border * ctx.ContentScale       <- physical
//
// The built-in text path, which every other control emits through, scales both:
//
//   UIManager_PrimitiveGen.cpp:2191  vs.FontSize * m_ContentScale
//   UIManager_PrimitiveGen.cpp:2224  ResolveLineBoxPx(..., m_ContentScale)
//
// At contentScale 1.0 the two spaces coincide and nothing is wrong. Above it, a
// TextArea draws glyphs 1/scale too small AND stacks its lines 1/scale too
// close, inside a box, a padding origin and a background rect that all grew —
// 0.667x on both axes at the 150% Windows step.
//
// Both axes are asserted separately and on purpose: a suite that only measured
// glyph quads would report #733 closed on a half-fix.
// `LineAdvanceIsUndersizedByTheContentScale` is the site-:344 half, and it is
// independent of :339 by construction — its `line-height` is an explicit
// LENGTH, so it routes through the cssLineHeight > 0 branch of
// ResolveLineBoxPx, which multiplies the authored length by the scale argument
// and never reads out.Px at all.
//
// Measured, by patching each site in turn and running this file (Debug,
// vs2026-x64-local). Each engine state gives a distinct signature, so no
// partial fix reports all-green:
//
//   engine state    AtScaleOne x2   GlyphRun   LineAdvance   BoxShare
//   both broken     pass            pass       pass          pass
//   :339 only       pass            FAIL       pass          FAIL
//   :344 only       pass            pass       FAIL          pass
//   both fixed      pass            FAIL       FAIL          FAIL
//
// A `pass` in the two middle columns means "that half of #733 is still live",
// which is why the half-fix rows are not all-green: the axis that moved fails
// and says so.
//
// The existing content-scale suites do run at 1.25/1.5/2.0 and still miss this,
// because they compute `fontSize * contentScale` themselves and hand the result
// to the atlas (MultilineTextLayoutTests.cpp PhysicalPixelSize, and the caret
// round-trip which feeds ComputeMetrics' own Px back into a hit-test that uses
// ComputeMetrics): they exercise the shaper, and they agree with themselves at
// every scale no matter what the caller does. Nothing below derives an
// expectation from the code under test. The three grounds used instead are:
//
//   1. a second consumer of the same shaper — a Label with byte-identical CSS,
//      whose glyph quads and line boxes must therefore be the same size;
//   2. the control's own chrome — its background rect, emitted by the generic
//      physical-space path — against which the glyph run must keep a constant
//      share of the box when only DPI changes; and
//   3. the CSS the test authored: `line-height: 24px` is 24 LOGICAL px, so at
//      contentScale S its line box is 24*S physical px, by the same
//      logical->physical mapping IsolatedUIFixtureTests pins on the border box.
//
// SPECIMEN NOTE — the multiline case uses explicit newlines under
// `white-space: nowrap`, not wrapping. Wrapping cannot be the instrument here:
// OnGeneratePrimitives receives a PHYSICAL `w` (UIManager_PrimitiveGen.cpp:1755)
// and ComputeMetrics subtracts LOGICAL padding from it, so the emit path's
// WrapWidth is physical while its shaping is logical — a third facet of the same
// mixed-space bug, which makes the wrapped line COUNT change with the scale.
// Explicit newlines give both paths the same three lines at every scale, so the
// line-advance comparison measures the line box and nothing else.
//
// ScriptTextArea inherits the defect through the same ComputeMetrics (it
// overrides OnGeneratePrimitives and EmitTextGlyphs, not the metrics), but it
// lives in Apps/Editor and there is no Editor test target, so it is out of
// reach from here.
//
// WHY THESE TESTS ARE ACTIVE AND ASSERT THE WRONG NUMBERS. This repo has no CI,
// so a DISABLED_ test is a test that never runs again: nothing would report
// that #733 was fixed, half-fixed or re-broken. Every DISABLED_ test in the
// tree is opt-in for COST (benchmarks) or blocked on a missing environment
// (PipelineCacheTest DISABLED_RealVulkanIntegration, RenderPipelineDeclareTests
// on EngineCore teardown) — none of them parks a plain correctness defect. The
// convention for that is the opposite one, and it is next door:
// TextGeometryChromeParityTests.cpp:118-141 keeps an ACTIVE test asserting the
// current, imperfect ppem truncation and names issue #729 as the tracker. These
// follow it. Each assertion below states the value #733 must change it to;
// fixing #733 MUST flip these expectations, and the failure is the report.

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

// 1.0, the two fractional Windows steps, and Retina.
constexpr float kContentScales[] = {1.0f, 1.25f, 1.5f, 2.0f};

// A Label and a TextArea, siblings, same text, same declarations. The only
// difference between them is which emit path draws the glyphs. The specimen is
// Latin only, takes no wrap opportunity at 300px, and is long enough that a
// per-glyph size error accumulates into an unmistakable run-extent difference.
constexpr char kXml[] = R"(<uielement id="root">
  <label id="lbl" text="Hamburgefonstiv"/>
  <textarea id="area" value="Hamburgefonstiv"/>
</uielement>)";

constexpr char kCss[] = R"(
#root { display: flex; flex-direction: column; width: 600px; height: 400px; }
#lbl, #area {
  width: 300px;
  height: 40px;
  font-family: Roboto;
  font-size: 16px;
  white-space: nowrap;
  padding: 0px;
  border-width: 0px;
  background-color: #272727;
  color: #ffffff;
}
)";

// Three lines of one repeated letter. Identical outlines give identical quad
// tops within a line, so the distinct quad tops ARE the lines and their spacing
// is the line advance — no clustering heuristic, no baseline reconstruction.
// `&#10;` is the newline: it survives XML attribute parsing where a literal one
// would be normalised to a space.
constexpr char kMultilineXml[] = R"(<uielement id="root">
  <label id="lbl" text="HH&#10;HH&#10;HH"/>
  <textarea id="area" value="HH&#10;HH&#10;HH"/>
</uielement>)";

// `line-height` is a LENGTH, not `normal` and not a multiplier, and that choice
// is what makes this specimen pin site :344 on its own. ResolveLineBoxPx takes
// the cssLineHeight > 0 branch, which is `cssLineHeight * contentScale` — it
// never reads the font size, so the line box here is wrong if and only if the
// scale argument is wrong. With `normal` the advance would fall back to the
// font's metric height at out.Px and a fix to :339 alone would move it.
constexpr float kLineHeightPx = 24.0f;
constexpr int kSpecimenLines = 3;

constexpr char kMultilineCss[] = R"(
#root { display: flex; flex-direction: column; width: 600px; height: 400px; }
#lbl, #area {
  width: 300px;
  height: 120px;
  font-family: Roboto;
  font-size: 16px;
  line-height: 24px;
  white-space: nowrap;
  padding: 0px;
  border-width: 0px;
  background-color: #272727;
  color: #ffffff;
}
)";

// 16px at each scale lands on 16/20/24/32 — whole ppem everywhere, so nothing
// below can be blamed on fractional-size rounding in the atlas.
constexpr float kFontSizePx = 16.0f;

// Glyph quads are analytic Slug bounds, not integers; a tenth of a physical
// pixel is far below any real difference here and far above float noise.
constexpr float kQuadEpsilon = 0.1f;

// Two quad tops belong to the same row when they are closer than this. The
// smallest line advance in play is 24 physical px and same-letter quads in one
// row are bit-identical, so any value well inside that gap separates rows.
constexpr float kRowEpsilon = 0.5f;

struct GlyphRun
{
    float Left = 0.0f;
    float Right = 0.0f;
    float MaxHeight = 0.0f;
    float Extent() const { return Right - Left; }
};

GlyphRun MeasureRun(const std::vector<UIPrimitive>& glyphs)
{
    GlyphRun run;
    if (glyphs.empty())
        return run;
    run.Left = glyphs.front().X;
    run.Right = glyphs.front().X + glyphs.front().W;
    for (const UIPrimitive& g : glyphs)
    {
        run.Left = std::min(run.Left, g.X);
        run.Right = std::max(run.Right, g.X + g.W);
        run.MaxHeight = std::max(run.MaxHeight, g.H);
    }
    return run;
}

// Distinct quad tops in physical px, ascending — one per rendered line. Quads
// with no area are not rendered rows and must not open one.
std::vector<float> GlyphRowTops(const std::vector<UIPrimitive>& glyphs)
{
    std::vector<float> tops;
    for (const UIPrimitive& g : glyphs)
    {
        if (g.W <= 0.0f || g.H <= 0.0f)
            continue;
        const bool known = std::any_of(tops.begin(), tops.end(), [&](float t)
                                       { return std::fabs(t - g.Y) < kRowEpsilon; });
        if (!known)
            tops.push_back(g.Y);
    }
    std::sort(tops.begin(), tops.end());
    return tops;
}

// The numbers compared below are glyph outlines, not layout boxes, so which
// face resolved decides what they mean: without the staged Roboto the resolver
// falls through to a system face for both elements.
bool RobotoResolved(const IsolatedUIFixture& fx)
{
    return fx.ResolvedFontFamily("lbl").find("Roboto") != std::string::npos &&
           fx.ResolvedFontFamily("area").find("Roboto") != std::string::npos;
}

} // namespace

// The control. At contentScale 1.0 the logical and physical spaces coincide, so
// the two emit paths must already agree — and if they do not, every comparison
// in the tests below is measuring something other than the scale defect.
TEST(TextAreaContentScale, GlyphsMatchTheBuiltInTextPathAtScaleOne)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();
    if (!RobotoResolved(fx))
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    const auto labelGlyphs = fx.Primitives("lbl", PrimitiveMode::Slug);
    const auto areaGlyphs = fx.Primitives("area", PrimitiveMode::Slug);

    ASSERT_FALSE(labelGlyphs.empty()) << "Label emitted no glyphs";
    ASSERT_FALSE(areaGlyphs.empty()) << "TextArea emitted no glyphs";
    ASSERT_EQ(labelGlyphs.size(), areaGlyphs.size())
        << "the two paths shaped the same specimen into different glyph counts";

    for (size_t i = 0; i < labelGlyphs.size(); ++i)
    {
        EXPECT_NEAR(areaGlyphs[i].W, labelGlyphs[i].W, kQuadEpsilon) << "glyph " << i;
        EXPECT_NEAR(areaGlyphs[i].H, labelGlyphs[i].H, kQuadEpsilon) << "glyph " << i;
    }

    const GlyphRun label = MeasureRun(labelGlyphs);
    const GlyphRun area = MeasureRun(areaGlyphs);
    EXPECT_NEAR(area.Extent(), label.Extent(), kQuadEpsilon);
}

// The vertical control, and the check that the multiline instrument works at
// all: three lines out of both paths, evenly spaced, at the authored 24px.
// Every reading in LineAdvanceIsUndersizedByTheContentScale depends on this.
TEST(TextAreaContentScale, LineAdvanceMatchesTheBuiltInTextPathAtScaleOne)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kMultilineXml, kMultilineCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();
    if (!RobotoResolved(fx))
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    const auto labelRows = GlyphRowTops(fx.Primitives("lbl", PrimitiveMode::Slug));
    const auto areaRows = GlyphRowTops(fx.Primitives("area", PrimitiveMode::Slug));

    ASSERT_EQ(labelRows.size(), static_cast<size_t>(kSpecimenLines))
        << "the built-in path did not draw three lines — the &#10; newlines did not survive "
           "XML parsing, so this specimen measures nothing";
    ASSERT_EQ(areaRows.size(), static_cast<size_t>(kSpecimenLines))
        << "the TextArea did not draw three lines";

    for (int i = 1; i < kSpecimenLines; ++i)
    {
        EXPECT_NEAR(labelRows[i] - labelRows[i - 1], kLineHeightPx, kQuadEpsilon)
            << "built-in path, gap " << i;
        EXPECT_NEAR(areaRows[i] - areaRows[i - 1], kLineHeightPx, kQuadEpsilon)
            << "TextArea, gap " << i;
    }
}

// #733, horizontal half — site :339. ACTIVE, and the expectations below are the
// CURRENT, WRONG behaviour: the TextArea's glyph run is 1/scale of the built-in
// path's at every scale above 1.0.
//
// Fixing #733 must flip this test: `kExpectedRatio` becomes 1.0 at every scale,
// and the TextArea run extent must then track the Label's rather than staying
// at its scale-1.0 value. Both assertions are written so the fixed engine fails
// them loudly instead of passing quietly.
TEST(TextAreaContentScale, GlyphRunIsUndersizedByTheContentScale)
{
    float areaExtentAtScaleOne = 0.0f;
    float labelExtentAtScaleOne = 0.0f;

    for (float scale : kContentScales)
    {
        IsolatedUIFixture fx;
        const bool built = fx.Build(scale, kXml, kCss);
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(built) << fx.Diagnostic() << " scale=" << scale;
        if (!RobotoResolved(fx))
            GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

        const auto labelGlyphs = fx.Primitives("lbl", PrimitiveMode::Slug);
        const auto areaGlyphs = fx.Primitives("area", PrimitiveMode::Slug);
        ASSERT_FALSE(labelGlyphs.empty()) << "scale=" << scale;
        ASSERT_FALSE(areaGlyphs.empty()) << "scale=" << scale;
        ASSERT_EQ(labelGlyphs.size(), areaGlyphs.size()) << "scale=" << scale;

        const GlyphRun label = MeasureRun(labelGlyphs);
        const GlyphRun area = MeasureRun(areaGlyphs);
        ASSERT_GT(label.Extent(), 0.0f) << "scale=" << scale;

        GTEST_LOG_(INFO) << "scale=" << scale << "  logical font-size=" << kFontSizePx
                         << "px  expected physical ppem=" << (kFontSizePx * scale)
                         << "  label run extent=" << label.Extent()
                         << "  textarea run extent=" << area.Extent()
                         << "  ratio=" << (area.Extent() / label.Extent());

        if (scale == 1.0f)
        {
            areaExtentAtScaleOne = area.Extent();
            labelExtentAtScaleOne = label.Extent();
        }

        // The instrument: the built-in path does scale, and Slug's analytic
        // outlines are linear in ppem (pinned against the face's design units
        // in TextGeometryChromeParityTests).
        EXPECT_NEAR(label.Extent(), labelExtentAtScaleOne * scale, kQuadEpsilon)
            << "scale=" << scale << ": the built-in path is not the reference this test assumes";

        // #733: the TextArea's glyphs are frozen at the scale-1.0 size. When
        // #733 is fixed this becomes `label.Extent()`.
        EXPECT_NEAR(area.Extent(), areaExtentAtScaleOne, kQuadEpsilon)
            << "scale=" << scale << ": #733 may be fixed — the TextArea run moved off its "
            << "scale-1.0 extent of " << areaExtentAtScaleOne << ". Flip this test.";

        const float kExpectedRatio = 1.0f / scale;
        EXPECT_NEAR(area.Extent() / label.Extent(), kExpectedRatio, 0.001f)
            << "scale=" << scale << ": TextArea glyph run is " << area.Extent()
            << " physical px where the built-in path draws " << label.Extent()
            << ". #733 fixed means this ratio is 1.0.";
        EXPECT_NEAR(area.MaxHeight / label.MaxHeight, kExpectedRatio, 0.001f)
            << "scale=" << scale << ": TextArea glyph height is " << area.MaxHeight
            << " physical px where the built-in path draws " << label.MaxHeight;
    }
}

// #733, vertical half — site :344. ACTIVE, and the expectations below are the
// CURRENT, WRONG behaviour: the TextArea stacks its lines at the LOGICAL 24px
// at every scale while the built-in path stacks them at 24 * scale.
//
// This is the half a glyph-size-only fix leaves behind. `line-height` is a
// length, so the line box does not depend on out.Px at all: repair :339 and
// leave :344 at scale 1.0 and this test still fails with the same numbers.
//
// Fixing #733 must flip this test: the TextArea's advance becomes
// kLineHeightPx * scale, matching the Label's, and the ratio becomes 1.0.
TEST(TextAreaContentScale, LineAdvanceIsUndersizedByTheContentScale)
{
    for (float scale : kContentScales)
    {
        IsolatedUIFixture fx;
        const bool built = fx.Build(scale, kMultilineXml, kMultilineCss);
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(built) << fx.Diagnostic() << " scale=" << scale;
        if (!RobotoResolved(fx))
            GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

        const auto labelRows = GlyphRowTops(fx.Primitives("lbl", PrimitiveMode::Slug));
        const auto areaRows = GlyphRowTops(fx.Primitives("area", PrimitiveMode::Slug));
        ASSERT_EQ(labelRows.size(), static_cast<size_t>(kSpecimenLines)) << "scale=" << scale;
        ASSERT_EQ(areaRows.size(), static_cast<size_t>(kSpecimenLines)) << "scale=" << scale;

        const float labelAdvance = labelRows[1] - labelRows[0];
        const float areaAdvance = areaRows[1] - areaRows[0];

        GTEST_LOG_(INFO) << "scale=" << scale << "  authored line-height=" << kLineHeightPx
                         << "px  label advance=" << labelAdvance
                         << "  textarea advance=" << areaAdvance
                         << "  ratio=" << (areaAdvance / labelAdvance);

        // The instrument, from the CSS the test authored: 24 LOGICAL px is
        // 24 * scale physical px, the same mapping the fixture's border-box
        // test pins.
        EXPECT_NEAR(labelAdvance, kLineHeightPx * scale, kQuadEpsilon)
            << "scale=" << scale << ": the built-in path is not the reference this test assumes";
        EXPECT_NEAR(labelRows[2] - labelRows[1], kLineHeightPx * scale, kQuadEpsilon)
            << "scale=" << scale << ": the built-in path's line spacing is not uniform";

        // #733: the line box never left logical space. When #733 is fixed this
        // becomes `kLineHeightPx * scale`.
        EXPECT_NEAR(areaAdvance, kLineHeightPx, kQuadEpsilon)
            << "scale=" << scale << ": #733 may be fixed — the TextArea line advance moved off "
            << "the logical " << kLineHeightPx << "px. Flip this test.";
        EXPECT_NEAR(areaRows[2] - areaRows[1], kLineHeightPx, kQuadEpsilon)
            << "scale=" << scale << ": TextArea line spacing is not uniform";

        EXPECT_NEAR(areaAdvance / labelAdvance, 1.0f / scale, 0.001f)
            << "scale=" << scale << ": TextArea stacks lines " << areaAdvance
            << " physical px apart where the built-in path uses " << labelAdvance
            << ". #733 fixed means this ratio is 1.0.";
    }
}

// #733 without the Label, against the control's own chrome. ACTIVE, and the
// expectation is again the CURRENT, WRONG behaviour: raising the display's DPI
// must not change how much of its own box a control's text covers, and here it
// does. The background rect comes from the generic physical-space emit and
// grows with the scale; the glyph run does not, so the share falls as 1/scale.
//
// Fixing #733 must flip this test: `share * scale` stops equalling the
// scale-1.0 share and `share` itself becomes the invariant.
TEST(TextAreaContentScale, GlyphRunLosesShareOfTheBoxAsTheScaleRises)
{
    float shareAtScaleOne = 0.0f;

    for (float scale : kContentScales)
    {
        IsolatedUIFixture fx;
        const bool built = fx.Build(scale, kXml, kCss);
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(built) << fx.Diagnostic() << " scale=" << scale;
        if (!RobotoResolved(fx))
            GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

        const auto areaGlyphs = fx.Primitives("area", PrimitiveMode::Slug);
        ASSERT_FALSE(areaGlyphs.empty()) << "scale=" << scale;

        const auto areaRects = fx.Primitives("area", PrimitiveMode::Rect);
        ASSERT_FALSE(areaRects.empty()) << "scale=" << scale << ": no background rect to measure";
        const float boxWidth = areaRects[0].W;
        ASSERT_GT(boxWidth, 0.0f) << "scale=" << scale;

        const PhysicalRect box = fx.BorderBox("area");
        ASSERT_NEAR(boxWidth, box.W, kQuadEpsilon)
            << "scale=" << scale << ": the background rect is not the element's border box";

        const GlyphRun area = MeasureRun(areaGlyphs);
        const float share = area.Extent() / boxWidth;

        GTEST_LOG_(INFO) << "scale=" << scale << "  box width=" << boxWidth
                         << "  glyph run extent=" << area.Extent() << "  share=" << share;

        if (scale == 1.0f)
        {
            shareAtScaleOne = share;
            ASSERT_GT(shareAtScaleOne, 0.0f);
            continue;
        }

        // When #733 is fixed this becomes EXPECT_NEAR(share, shareAtScaleOne).
        EXPECT_NEAR(share * scale, shareAtScaleOne, 0.001f)
            << "scale=" << scale << ": text covers " << share << " of the box where at 1.0 it "
            << "covered " << shareAtScaleOne << ". #733 fixed means the share is scale-invariant, "
            << "not this 1/scale decay — flip this test.";
    }
}
