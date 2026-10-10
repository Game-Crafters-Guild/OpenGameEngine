// `flex: <length>` has to reach the solve as a BASIS, not a grow factor.
//
// The parse itself is pinned in CSSParserTests; this file pins what the defect
// was actually reported as — laid-out widths. Under the defect `flex: 100px`
// became `flex-grow: 100`, so a 300px row holding `flex: 100px` and `flex: 1`
// distributed 100:1 and gave 297 / 3 instead of Chrome's 200 / 100.
//
// Ground truth is Chrome under
//   chrome --headless=new --force-device-scale-factor={1,2}
// with box-sizing: border-box, read from getBoundingClientRect. Both scales are
// measured because Chrome quantizes layout to 1/64 DEVICE px: the dpr-2 numbers
// are not the dpr-1 numbers doubled, and the two grids differ in the third
// decimal for any width that does not land on a 64th.

#include "IsolatedUIFixture.h"

#include "UI/UIElement.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="row">
    <uielement id="a"/>
    <uielement id="b"/>
  </uielement>
</uielement>)";

// #a carries the declaration under test, #b is the `flex: 1` control.
std::string MakeCss(const char* declA)
{
    return std::string(
               "#root { display: flex; flex-direction: column; width: 800px; height: 600px; }\n"
               "#row  { display: flex; flex-direction: row; width: 300px; height: 40px; }\n"
               "#b    { flex: 1; }\n"
               "#a    { flex: ") +
           declA + "; }\n";
}

// Chrome, 300px row, `flex: 100px` against `flex: 1`.
// Both widths land on integers, so the two device scales agree exactly.
constexpr float kChromeLoneLengthA = 200.0f;
constexpr float kChromeLoneLengthB = 100.0f;

// Chrome, `flex: 2 100px` against `flex: 1`: 100px basis, 200px free space
// split 2:1. 233.333... does not land on a 64th, so the grids diverge:
//   dpr 1: 1/64 CSS px   -> 14933/64  = 233.328125
//   dpr 2: 1/64 device px -> 29867/128 = 233.3359375
constexpr float kChromeGrowBasisA_Dpr1 = 233.328125f;
constexpr float kChromeGrowBasisB_Dpr1 = 66.671875f;
constexpr float kChromeGrowBasisA_Dpr2 = 233.3359375f;
constexpr float kChromeGrowBasisB_Dpr2 = 66.6640625f;

// Tight enough that the dpr-1 and dpr-2 literals cannot satisfy each other
// (they differ by ~0.0078), so a test that asserted the wrong grid would fail.
constexpr float kGridTolerance = 0.002f;

struct RowWidths
{
    float A = 0.0f;
    float B = 0.0f;
};

// Lays out the row at `contentScale` and returns both children's LOGICAL
// widths — the space Chrome's CSS px live in.
::testing::AssertionResult LayOutRow(float contentScale, const char* declA, RowWidths& out)
{
    auto fx = std::make_unique<IsolatedUIFixture>();
    if (!fx->Build(contentScale, kXml, MakeCss(declA)))
    {
        return ::testing::AssertionFailure() << fx->Diagnostic()
                                             << (fx->DeviceAvailable() ? "" : " [no device]");
    }

    const GameEngine::UIElement* a = fx->Element("a");
    const GameEngine::UIElement* b = fx->Element("b");
    if (!a || !b)
        return ::testing::AssertionFailure() << "row children missing";

    out.A = a->GetLayoutWidth();
    out.B = b->GetLayoutWidth();
    return ::testing::AssertionSuccess();
}

bool DeviceMissing()
{
    IsolatedUIFixture probe;
    probe.Build(1.0f, kXml, MakeCss("1"));
    return !probe.DeviceAvailable();
}

} // namespace

// The reported defect, at both device scales. 297/3 is the pre-fix engine.
TEST(FlexShorthandLayout, LoneLengthIsBasisNotGrow)
{
    if (DeviceMissing())
        GTEST_SKIP() << "no Vulkan device";

    for (const float scale : {1.0f, 2.0f})
    {
        RowWidths w{};
        ASSERT_TRUE(LayOutRow(scale, "100px", w)) << "scale " << scale;

        EXPECT_NEAR(w.A, kChromeLoneLengthA, kGridTolerance) << "scale " << scale;
        EXPECT_NEAR(w.B, kChromeLoneLengthB, kGridTolerance) << "scale " << scale;
        // The line still fills the row: a basis that leaked into grow would
        // keep this true too, so it is a sanity check, not the assertion.
        EXPECT_NEAR(w.A + w.B, 300.0f, kGridTolerance) << "scale " << scale;
    }
}

// `flex: 2 100px` exercises grow and basis together, and is the case whose
// widths sit off the 64th grid — so it is also the parity check on the grid
// itself at each scale.
TEST(FlexShorthandLayout, GrowWithLengthBasisMatchesChromeAtBothScales)
{
    if (DeviceMissing())
        GTEST_SKIP() << "no Vulkan device";

    RowWidths dpr1{};
    ASSERT_TRUE(LayOutRow(1.0f, "2 100px", dpr1));
    EXPECT_NEAR(dpr1.A, kChromeGrowBasisA_Dpr1, kGridTolerance);
    EXPECT_NEAR(dpr1.B, kChromeGrowBasisB_Dpr1, kGridTolerance);

    RowWidths dpr2{};
    ASSERT_TRUE(LayOutRow(2.0f, "2 100px", dpr2));
    EXPECT_NEAR(dpr2.A, kChromeGrowBasisA_Dpr2, kGridTolerance);
    EXPECT_NEAR(dpr2.B, kChromeGrowBasisB_Dpr2, kGridTolerance);
}

// `flex: 2 3` is two factors over a 0 basis. Both items start at 0, so the
// whole 300px is free space split by grow — 2 against the control's 1.
TEST(FlexShorthandLayout, TwoFactorsMatchChrome)
{
    if (DeviceMissing())
        GTEST_SKIP() << "no Vulkan device";

    for (const float scale : {1.0f, 2.0f})
    {
        RowWidths w{};
        ASSERT_TRUE(LayOutRow(scale, "2 3", w)) << "scale " << scale;
        EXPECT_NEAR(w.A, 200.0f, kGridTolerance) << "scale " << scale;
        EXPECT_NEAR(w.B, 100.0f, kGridTolerance) << "scale " << scale;
    }
}

// `flex: none` is `0 0 auto`, so #a collapses to its content (nothing) and the
// control takes the row.
TEST(FlexShorthandLayout, NoneAndInitialCollapse)
{
    if (DeviceMissing())
        GTEST_SKIP() << "no Vulkan device";

    for (const float scale : {1.0f, 2.0f})
    {
        RowWidths none{};
        ASSERT_TRUE(LayOutRow(scale, "none", none)) << "scale " << scale;
        EXPECT_NEAR(none.A, 0.0f, kGridTolerance) << "scale " << scale;
        EXPECT_NEAR(none.B, 300.0f, kGridTolerance) << "scale " << scale;

        // `initial` is `0 1 auto` — same laid-out result here, different
        // longhands (CSSParserTests pins the distinction).
        RowWidths initial{};
        ASSERT_TRUE(LayOutRow(scale, "initial", initial)) << "scale " << scale;
        EXPECT_NEAR(initial.A, 0.0f, kGridTolerance) << "scale " << scale;
        EXPECT_NEAR(initial.B, 300.0f, kGridTolerance) << "scale " << scale;
    }
}
