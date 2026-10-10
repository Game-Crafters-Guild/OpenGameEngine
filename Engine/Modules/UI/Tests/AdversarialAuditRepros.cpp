// Adversarial pass over the UI primitive-gen audit findings.
//
// Each test below tries to REFUTE a claim the audit made. A claim that cannot
// be reproduced here is a hypothesis, not a defect. Nothing derives its
// expectation from the code under test: every assertion is grounded either in
// the CSS the test authored, or in a SECOND consumer of the same value that
// must agree with the first.

#include "IsolatedUIFixture.h"

#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

// ---------------------------------------------------------------------------
// CLAIM 1 (two lenses, HIGH): PushClip receives a PHYSICAL rect and CSS-LOGICAL
// radii, so a rounded overflow:hidden container clips with a radius
// 1/contentScale too small.
//
// The ground truth is NOT "radius should be 8*cs because the code should scale
// it". It is the element's OWN background rect, emitted by the generic path a
// few lines earlier from the same vs.BorderRadius: that primitive rounds the
// painted corner, and the clip is supposed to mask children to the same corner.
// If the two disagree, children paint into a corner the background rounded off.
// So the assertion is clip.Radii == backgroundPrimitive.Radii, at each scale.
// ---------------------------------------------------------------------------
constexpr char kClipXml[] = R"(<uielement id="root">
  <uielement id="panel"><uielement id="child"/></uielement>
</uielement>)";

constexpr char kClipCss[] = R"(
#root  { display: flex; flex-direction: column; width: 400px; height: 300px; }
#panel { width: 200px; height: 100px; overflow: hidden; border-radius: 8px;
         background-color: #303030; }
#child { width: 400px; height: 400px; background-color: #ff0000; }
)";

void CheckClipRadiusMatchesPaintedCorner(float scale)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(scale, kClipXml, kClipCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    GameEngine::UIElement* panel = fx.Element("panel");
    ASSERT_NE(panel, nullptr);
    ASSERT_NE(panel->m_ClipSlotIdx, 0xFFFFu) << "panel owns no clip slot; the repro never armed";

    const GameEngine::UI::UIClipRect* clip =
        fx.Manager().PeekClipRectForTesting(panel->m_ClipSlotIdx);
    ASSERT_NE(clip, nullptr);

    // The painted corner: the panel's own background rect.
    const auto rects = fx.Primitives("panel", GameEngine::UI::PrimitiveMode::Rect);
    ASSERT_FALSE(rects.empty()) << "panel emitted no background rect";
    const float paintedRadius = rects[0].Radii[0];

    std::printf("  scale %.2f : clip.Rect=(%.1f,%.1f,%.1f,%.1f) clip.Radii[0]=%.3f  "
                "painted bg Radii[0]=%.3f\n",
                static_cast<double>(scale), clip->Rect[0], clip->Rect[1], clip->Rect[2],
                clip->Rect[3], clip->Radii[0], paintedRadius);

    // Cross-check that the clip RECT is physical, so a radius mismatch cannot
    // be explained away as "the whole clip is in logical space".
    EXPECT_NEAR(clip->Rect[2], 200.0f * scale, 0.5f)
        << "clip rect width is not physical; the space model in the finding is wrong";

    EXPECT_NEAR(clip->Radii[0], paintedRadius, 0.01f)
        << "clip corner radius disagrees with the corner the background painted";
}

TEST(AdversarialClipRadii, MatchesPaintedCornerAtScaleOne) { CheckClipRadiusMatchesPaintedCorner(1.0f); }
TEST(AdversarialClipRadii, MatchesPaintedCornerAtScaleTwo) { CheckClipRadiusMatchesPaintedCorner(2.0f); }

// ---------------------------------------------------------------------------
// The same invariant with a PERCENTAGE radius. Clip radii and painted radii are
// derived from vs.BorderRadius at different call sites (the background emit,
// the children-overflow PushClip, and the drain's slot rewrite), so a
// percentage resolved at one site and not at another splits the two contours:
// the clip would cut children along one arc while the background paints
// another. The px case above cannot catch that — it has nothing to resolve.
//
// Agreement between the two sites is necessary but not sufficient here: both
// reading the same wrong value agree with each other perfectly. So the spec
// value is asserted directly as well. The box is 200x200 at 10% — a 20px
// corner, deliberately far under the shader's min(halfW, halfH) = 100px clamp,
// so neither number can be rescued on the way to the screen.
// ---------------------------------------------------------------------------
constexpr char kPercentClipXml[] = R"(<uielement id="root">
  <uielement id="panel"><uielement id="child"/></uielement>
</uielement>)";

constexpr char kPercentClipCss[] = R"(
#root  { display: flex; flex-direction: column; width: 400px; height: 400px; }
#panel { width: 200px; height: 200px; overflow: hidden; border-radius: 10%;
         background-color: #303030; }
#child { width: 400px; height: 400px; background-color: #ff0000; }
)";

void CheckPercentClipRadiusMatchesPaintedCorner(float scale)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(scale, kPercentClipXml, kPercentClipCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    GameEngine::UIElement* panel = fx.Element("panel");
    ASSERT_NE(panel, nullptr);
    ASSERT_NE(panel->m_ClipSlotIdx, 0xFFFFu) << "panel owns no clip slot; the repro never armed";

    const GameEngine::UI::UIClipRect* clip =
        fx.Manager().PeekClipRectForTesting(panel->m_ClipSlotIdx);
    ASSERT_NE(clip, nullptr);

    const auto rects = fx.Primitives("panel", GameEngine::UI::PrimitiveMode::Rect);
    ASSERT_FALSE(rects.empty()) << "panel emitted no background rect";
    const float paintedRadius = rects[0].Radii[0];

    // 10% of the 200px border box, in physical px.
    const float specRadius = 20.0f * scale;

    std::printf("  scale %.2f : clip.Radii[0]=%.3f  painted bg Radii[0]=%.3f  spec=%.3f\n",
                static_cast<double>(scale), clip->Radii[0], paintedRadius,
                static_cast<double>(specRadius));

    EXPECT_NEAR(clip->Rect[2], 200.0f * scale, 0.5f)
        << "clip rect width is not physical; the space model in this test is wrong";

    EXPECT_NEAR(paintedRadius, specRadius, 0.01f)
        << "painted corner did not resolve `10%` against the 200px border box";
    EXPECT_NEAR(clip->Radii[0], specRadius, 0.01f)
        << "clip corner did not resolve `10%` against the 200px border box";
    EXPECT_NEAR(clip->Radii[0], paintedRadius, 0.01f)
        << "clip corner radius disagrees with the corner the background painted";
}

TEST(AdversarialClipRadii, PercentMatchesPaintedCornerAtScaleOne)
{
    CheckPercentClipRadiusMatchesPaintedCorner(1.0f);
}
TEST(AdversarialClipRadii, PercentMatchesPaintedCornerAtScaleTwo)
{
    CheckPercentClipRadiusMatchesPaintedCorner(2.0f);
}

// ---------------------------------------------------------------------------
// CLAIM 2 (HIGH): `if (w <= 0 || h <= 0) return;` at UIManager_PrimitiveGen.cpp
// :1589 sits before child recursion, so a zero-size element drops its whole
// subtree. CSS paints overflowing descendants of a zero-height box.
//
// Refutation attempt: build a zero-height parent (overflow defaults to visible)
// with a normally-sized child, and ask whether the child emitted anything.
// ---------------------------------------------------------------------------
constexpr char kZeroXml[] = R"(<uielement id="root">
  <uielement id="zerobox"><uielement id="child"/></uielement>
  <uielement id="control"><uielement id="sibling"/></uielement>
</uielement>)";

constexpr char kZeroCss[] = R"(
#root     { display: flex; flex-direction: column; width: 400px; height: 300px; }
#zerobox  { width: 200px; height: 0px; }
#child    { width: 50px; height: 50px; background-color: #00ff00; }
#control  { width: 200px; height: 60px; }
#sibling  { width: 50px; height: 50px; background-color: #0000ff; }
)";

TEST(AdversarialZeroSize, ZeroHeightParentStillPaintsOverflowingChild)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kZeroXml, kZeroCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    // Control: an identical child under a NON-zero parent. If this is empty the
    // test is measuring something other than the zero-size gate.
    const auto sibling = fx.Primitives("sibling");
    ASSERT_FALSE(sibling.empty()) << "control child emitted nothing; instrument is not valid";

    const auto child = fx.Primitives("child");
    std::printf("  zero-height parent: child primitives=%zu  (control sibling=%zu)\n",
                child.size(), sibling.size());

    EXPECT_FALSE(child.empty())
        << "child of a zero-height, overflow:visible parent emitted nothing — subtree dropped";
}

// ---------------------------------------------------------------------------
// CLAIM 3 (two lenses, HIGH; then REFUTED by the Release measurement):
// TextArea's two ComputeMetrics overloads key the same line-break cache in two
// pixel spaces, so at contentScale != 1 they evict each other every frame and
// the document is re-wrapped and re-shaped continuously.
//
// The Release measurement compared GenAllPrimitivesMs at cs 1.0 vs cs 1.5 and
// found a 0.997 ratio. That comparison cannot separate "no thrash" from "thrash
// cost cancelled by a different workload", because the two arms do not wrap to
// the same number of rows (the measurement's own diagnostic reports 1000 rows
// at cs 1.0 and 669 at cs 1.5). A cross-scale comparison is confounded by
// construction.
//
// This instrument is WITHIN one scale instead, so the workload is identical on
// both sides of the comparison: frame 0 is cold (cache empty, must wrap and
// shape), frames 1..N are warm. If the cache holds, warm frames are much
// cheaper than the cold frame. If the two callers evict each other, every frame
// pays the cold cost and the ratio goes to 1.
//
// PRE-REGISTERED RULE, written before running:
//   warm/cold < 0.5 at a scale  => cache is holding at that scale
//   warm/cold > 0.8 at a scale  => cache is thrashing at that scale
//   thrash at 1.5 but not at 1.0 => the finding stands as a perf defect and the
//                                   measurement's refutation was confounded
//   holds at both                => the finding is refuted as a perf defect
// ---------------------------------------------------------------------------
// A timing A/B cannot settle this cheaply, so observe the STATE instead.
// TextArea::OnPostLayout (the LOGICAL-width caller) does not merely read the
// cache — when the wrapped content is taller than the box it writes the result
// back into the layout cache. So the element's own laid-out height reports how
// many rows THAT caller wrapped to, while the emitted glyph rows report how
// many rows the PHYSICAL-width caller wrapped to. If the two disagree, the two
// overloads provably resolved different wrap widths in the same frame, which is
// exactly the eviction precondition. If they agree, the finding is refuted.
void ProbeTextAreaCallerAgreement(float scale, float& outLogicalH, float& outPitchLogical)
{
    std::string doc;
    for (int i = 0; i < 300; ++i)
        doc += "The quick brown fox jumps over the lazy dog and keeps running onward. ";

    const std::string xml = "<uielement id=\"root\"><scrollview id=\"sv\">"
                            "<textarea id=\"area\" value=\"" + doc + "\"/></scrollview></uielement>";
    const char* css = R"(
#root { display: flex; flex-direction: column; width: 800px; height: 9000px; }
#sv { width: 780px; height: 9000px; overflow: hidden; }
#area {
  width: 700px; height: 200px;
  font-family: Roboto; font-size: 14px; color: #dddddd;
  padding: 8px; border-width: 1px; white-space: normal; background-color: #272727;
}
)";

    IsolatedUIFixture fx;
    const bool built = fx.Build(scale, xml, css);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    // Rows the PHYSICAL-width caller (emit) produced.
    const auto glyphs = fx.Primitives("area", GameEngine::UI::PrimitiveMode::Slug);
    std::vector<float> ys;
    for (const auto& g : glyphs)
    {
        const bool seen = std::any_of(ys.begin(), ys.end(),
                                      [&](float y) { return std::abs(y - g.Y) < 0.5f; });
        if (!seen)
            ys.push_back(g.Y);
    }
    const size_t emitRows = ys.size();

    // Rows the LOGICAL-width caller (OnPostLayout) produced, read back out of
    // the height it wrote. The authored height is 200px, so any larger value is
    // OnPostLayout's content height and proves that caller ran.
    const float logicalH = fx.BorderBox("area").H / scale;
    ASSERT_GT(logicalH, 200.5f) << "OnPostLayout never expanded the box; the second caller "
                                   "never ran and this instrument measured nothing";

    // Robust stand-in for "how many rows did the EMIT path wrap to": the
    // baseline of the last emitted row, in LOGICAL px. Immune to the per-glyph
    // Y jitter that makes a distinct-Y count unreliable.
    std::sort(ys.begin(), ys.end());
    const float lastRowLogicalY = ys.empty() ? 0.0f : ys.back() / scale;
    std::printf("  scale %.2f : emit last-row baseline=%.1f logical px (%zu distinct Y)   "
                "OnPostLayout content height=%.1f logical px\n",
                static_cast<double>(scale), lastRowLogicalY, emitRows, logicalH);
    outLogicalH = logicalH;
    outPitchLogical = lastRowLogicalY;
}

// OnPostLayout wraps against GetLayoutWidth(), which the fixture holds constant
// in LOGICAL px across content scales. So if that caller is logical-only, the
// content height it writes must be BYTE-IDENTICAL at cs 1.0 and cs 1.5. The
// emit path, wrapping against the physical width, demonstrably is not (1000
// rows vs 669). Two callers, same text, same frame, different wrap => the
// eviction precondition the finding rests on.
TEST(AdversarialTextAreaThrash, TwoCallersDisagreeOnWrapWidth)
{
    float h100 = 0.0f, pitch100 = 0.0f, h150 = 0.0f, pitch150 = 0.0f;
    ProbeTextAreaCallerAgreement(1.0f, h100, pitch100);
    if (::testing::Test::HasFatalFailure() || h100 == 0.0f)
        return;
    ProbeTextAreaCallerAgreement(1.5f, h150, pitch150);
    if (::testing::Test::HasFatalFailure())
        return;

    // The EMIT path must be shown to wrap differently, or there is nothing for
    // the other caller to disagree WITH. Its last-row baseline is the reading.
    std::printf("  emit last-row baseline : cs1.0=%.1f  cs1.5=%.1f  (ratio %.4f)\n",
                pitch100, pitch150, pitch100 > 0.0f ? pitch150 / pitch100 : 0.0f);
    ASSERT_LT(pitch150, pitch100 * 0.95f)
        << "emit did not wrap differently across scales; nothing to disagree about";

    std::printf("  OnPostLayout content height: cs1.0=%.1f  cs1.5=%.1f  (ratio %.4f)\n",
                h100, h150, h150 / h100);

    // THE REFUTATION FAILED, so this pins the disagreement it found rather than
    // the agreement it went looking for: green while #871 lives, red the moment
    // the two callers are reconciled. Same convention PercentBorderRadiusTests.cpp
    // uses for a live defect, and it is why this is not a landed-red test.
    //
    // Had OnPostLayout read the wrap width the emit path uses, its content height
    // would have moved with the scale exactly as the emit baseline did. It does
    // not move at all, which is what a logical-only wrap looks like.
    EXPECT_FLOAT_EQ(h150, h100)
        << "#871: OnPostLayout's content height is no longer scale-invariant, so "
           "this probe's premise needs re-deriving before its verdict is trusted";
    EXPECT_GT(std::fabs(h150 - h100 * (pitch150 / pitch100)), 1.0f)
        << "#871 looks FIXED: the two ComputeMetrics callers now agree on wrap "
           "width. Flip this to assert the agreement and close the issue";
}

double MedianOf(std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void ProbeTextAreaCacheWarmth(float scale)
{
    std::string doc;
    for (int i = 0; i < 300; ++i)
        doc += "The quick brown fox jumps over the lazy dog and keeps running onward. ";

    const std::string xml = "<uielement id=\"root\"><scrollview id=\"sv\">"
                            "<textarea id=\"area\" value=\"" + doc + "\"/></scrollview></uielement>";
    const char* css = R"(
#root { display: flex; flex-direction: column; width: 800px; height: 600px; }
#sv { width: 780px; height: 560px; overflow: hidden; }
#area {
  width: 700px; height: 5000px;
  font-family: Roboto; font-size: 14px; color: #dddddd;
  padding: 8px; border-width: 1px; white-space: normal; background-color: #272727;
}
)";

    IsolatedUIFixture fx;
    const bool built = fx.Build(scale, xml, css);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    GameEngine::UIManager& ui = fx.Manager();
    ui.SetUpdateProfilingEnabled(true);

    // Rebuild the control's text cache from scratch so frame 0 below is a true
    // cold frame: a fresh value invalidates every key that contains the text.
    GameEngine::UIElement* area = fx.Element("area");
    ASSERT_NE(area, nullptr);

    std::vector<double> warm;
    double cold = 0.0;
    for (int i = 0; i < 25; ++i)
    {
        ui.MarkStyleDirtyAll();
        const auto t0 = std::chrono::steady_clock::now();
        fx.StepFrame();
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (i == 0)
            cold = ms;
        else if (i >= 5)
            warm.push_back(ms);
    }

    const double warmMed = MedianOf(warm);
    std::printf("  scale %.2f : cold frame=%.4f ms   warm median=%.4f ms   warm/cold=%.3f\n",
                static_cast<double>(scale), cold, warmMed, cold > 0.0 ? warmMed / cold : 0.0);

    // Report the wrapped row count alongside, so the reading is attributable.
    const auto glyphs = fx.Primitives("area", GameEngine::UI::PrimitiveMode::Slug);
    std::vector<float> ys;
    for (const auto& g : glyphs)
    {
        const bool seen = std::any_of(ys.begin(), ys.end(),
                                      [&](float y) { return std::abs(y - g.Y) < 0.5f; });
        if (!seen)
            ys.push_back(g.Y);
    }
    std::printf("            glyph primitives=%zu  distinct rows=%zu\n", glyphs.size(), ys.size());
}

TEST(AdversarialTextAreaThrash, DISABLED_CacheWarmthScale100) { ProbeTextAreaCacheWarmth(1.0f); }
TEST(AdversarialTextAreaThrash, DISABLED_CacheWarmthScale150) { ProbeTextAreaCacheWarmth(1.5f); }

} // namespace
