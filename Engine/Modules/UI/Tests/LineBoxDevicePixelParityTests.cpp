// A text line box must be the same number of DEVICE pixels tall as Chrome's,
// at every content scale — not just at scale 1 where a device pixel and a
// logical pixel are the same thing.
//
// WHY THIS IS A SEPARATE FILE FROM THE OTHER CHROME-PARITY TESTS: those pin the
// font-metric layer (Engine/Modules/Text), which resolves a line box in device
// px and already agrees with Blink. The defect these tests exist for is one
// layer down the pipe, where that device-px height is divided into logical px
// and handed to Yoga. Asserting only the metric would leave the divergence the
// editor actually shows completely uncovered, so every case below reads the
// laid-out element back, and the metric assertion is kept alongside as the
// CONTROL that says which of the two layers moved.
//
// REFERENCE NUMBERS: Chrome 150.0.7871.186 launched as
//   chrome.exe --headless=new --force-device-scale-factor=N
// over the staged Roboto-Regular.ttf, measured after `document.fonts.ready`
// (measuring earlier silently reports a fallback face), reading
// getBoundingClientRect().height * devicePixelRatio. Never Playwright's
// emulated deviceScaleFactor: it reports a dpr the layout did not use.
//
// Roboto 2.001047: unitsPerEm 2048, usWinAscent 2146, usWinDescent 555,
// lineGap 0. Blink rounds ascent and descent to whole DEVICE px independently
// and sums them, at the font size multiplied by the device scale factor — so
// `font-size: 16px` at dpr 2 is a 32-device-px face, 34 + 9 = 43 device px, and
// the SAME CSS at dpr 1 is a 16-device-px face, 17 + 4 = 21. The line box is
// not proportional to the scale; that is the whole reason this table is
// measured per scale rather than derived from the scale-1 row.

#include "IsolatedUIFixture.h"
#include "RobotoTestFont.h"

#include "Rendering/Text/FontAtlas.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::LoadRobotoAtlas;
using GameEngine::UITesting::PhysicalRect;

namespace
{

// `line-height: normal` box height in DEVICE px, straight out of Chrome.
struct ChromeLineBox
{
    int FontSizeCss;
    int DeviceHeightPx;
};

// Each row is one --force-device-scale-factor run.
struct ChromeScaleRow
{
    float Scale;
    std::vector<ChromeLineBox> Boxes;
};

const std::vector<ChromeScaleRow>& ChromeReference()
{
    static const std::vector<ChromeScaleRow> kRows = {
        {1.0f,
         {{8, 10}, {10, 13}, {11, 15}, {12, 16}, {13, 18}, {14, 19}, {15, 20}, {16, 21}, {17, 23},
          {18, 24}, {20, 26}, {24, 32}, {28, 37}, {32, 43}, {40, 53}, {48, 63}}},
        {1.25f,
         {{8, 13}, {10, 16}, {11, 18}, {12, 20}, {13, 21}, {14, 23}, {15, 25}, {16, 26}, {17, 28},
          {18, 30}, {20, 33}, {24, 39}, {28, 46}, {32, 53}, {40, 66}, {48, 79}}},
        {1.5f,
         {{8, 16}, {10, 20}, {11, 21}, {12, 24}, {13, 25}, {14, 28}, {15, 30}, {16, 32}, {17, 34},
          {18, 35}, {20, 39}, {24, 48}, {28, 55}, {32, 63}, {40, 79}, {48, 95}}},
        {2.0f,
         {{8, 21}, {10, 26}, {11, 29}, {12, 32}, {13, 34}, {14, 37}, {15, 39}, {16, 43}, {17, 45},
          {18, 48}, {20, 53}, {24, 63}, {28, 74}, {32, 84}, {40, 106}, {48, 127}}},
    };
    return kRows;
}

// Font metrics are whole device pixels, with no rounding stage between the
// tables and the number asserted; this covers float round-trip only.
constexpr float kExactPx = 0.01f;

// Laid-out geometry gets one more term: Yoga rounds onto Chrome's own layout
// quantum of 1/64 of a device pixel (YogaAdapter::SetContentScale), and Yoga
// rounds to nearest where Chrome floors, so one grid step is the floor on any
// comparison between the two. Asserting tighter than the grid would assert
// something the grid cannot deliver. It is still 64x below the defect this file
// exists for, which is a whole device pixel.
constexpr float kLayoutGridPx = 1.0f / 64.0f + 1.0e-4f;

std::string ItemId(int fontSizeCss)
{
    return "s" + std::to_string(fontSizeCss);
}

// One column of labels, one per font size — a single fixture per scale rather
// than one per case. `flex-shrink: 0` is load-bearing: a column whose content
// outgrows it would otherwise shrink the items and measure the shrink instead
// of the line box.
void BuildSizeColumn(const ChromeScaleRow& row, std::string& outXml, std::string& outCss)
{
    outXml = "<uielement id=\"root\">\n";
    outCss = "#root { display: flex; flex-direction: column; align-items: flex-start; }\n";
    for (const ChromeLineBox& box : row.Boxes)
    {
        const std::string id = ItemId(box.FontSizeCss);
        outXml += "  <label id=\"" + id + "\">Hxg</label>\n";
        outCss += "#" + id + " { font-family: Roboto; font-size: " +
                  std::to_string(box.FontSizeCss) +
                  "px; line-height: normal; flex-shrink: 0; color: #ffffff; }\n";
    }
    outXml += "</uielement>";
}

class LineBoxDevicePixelParity : public ::testing::TestWithParam<size_t>
{
};

// The CONTROL. The font-metric layer resolves the line box in device px, and
// this asserts it against Chrome directly — no Yoga, no content-scale
// division. It must stay green whatever happens to layout: a failure here says
// the divergence is in the metrics, and every layout expectation below is
// therefore unattributable.
TEST_P(LineBoxDevicePixelParity, FontMetricLineBoxMatchesChromeInDevicePixels)
{
    const ChromeScaleRow& row = ChromeReference()[GetParam()];
    auto atlas = LoadRobotoAtlas();
    if (!atlas)
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    for (const ChromeLineBox& box : row.Boxes)
    {
        const float devicePx = (float)box.FontSizeCss * row.Scale;
        const auto lm = atlas->GetFontLineMetrics(devicePx);
        EXPECT_NEAR(lm.height, (float)box.DeviceHeightPx, kExactPx)
            << "scale=" << row.Scale << " font-size=" << box.FontSizeCss
            << "px (device face " << devicePx << "px)";
    }
}

// The defect. A laid-out `line-height: normal` label must occupy Chrome's
// number of device pixels. At scale 1 a logical pixel IS a device pixel and
// this has always held; at 1.25 / 1.5 / 2 it is the assertion that the
// device-px line box survives the trip through layout.
TEST_P(LineBoxDevicePixelParity, LaidOutLineBoxMatchesChromeInDevicePixels)
{
    const ChromeScaleRow& row = ChromeReference()[GetParam()];

    std::string xml, css;
    BuildSizeColumn(row, xml, css);

    IsolatedUIFixture fx;
    if (!fx.Build(row.Scale, xml, css))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << "no Vulkan device";
        FAIL() << fx.Diagnostic();
    }
    // A silent fall-through to a system face would leave every number below
    // unattributable.
    ASSERT_EQ(fx.ResolvedFontFamily(ItemId(row.Boxes.front().FontSizeCss)), "Roboto");

    for (const ChromeLineBox& box : row.Boxes)
    {
        const PhysicalRect r = fx.BorderBox(ItemId(box.FontSizeCss));
        EXPECT_NEAR(r.H, (float)box.DeviceHeightPx, kLayoutGridPx)
            << "scale=" << row.Scale << " font-size=" << box.FontSizeCss << "px";
    }
}

std::string ScaleParamName(const ::testing::TestParamInfo<size_t>& info)
{
    static const char* const kNames[] = {"s1", "s1_25", "s1_5", "s2"};
    return kNames[info.param];
}

INSTANTIATE_TEST_SUITE_P(Scales, LineBoxDevicePixelParity,
                         ::testing::Values(size_t(0), size_t(1), size_t(2), size_t(3)),
                         ScaleParamName);

// The single case the scale-2 baseline divergence was traced to, spelled out on
// its own so a regression names itself. 16px Roboto at content scale 2 is a
// 32-device-px face: ascent 34 + descent 9 = 43 device px, which is 21.5
// logical px — a value a whole-logical-pixel layout grid cannot hold.
TEST(LineBoxDevicePixelParityScale2, SixteenPxNormalLineHeightIsFortyThreeDevicePx)
{
    constexpr float kScale = 2.0f;
    constexpr float kChromeDeviceH = 43.0f;

    IsolatedUIFixture fx;
    const char* xml = R"(<uielement id="root">
  <label id="a">Hxg</label>
</uielement>)";
    const char* css = "#root { display: flex; flex-direction: column; align-items: flex-start; }\n"
                      "#a { font-family: Roboto; font-size: 16px; line-height: normal; "
                      "flex-shrink: 0; color: #ffffff; }\n";
    if (!fx.Build(kScale, xml, css))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << "no Vulkan device";
        FAIL() << fx.Diagnostic();
    }
    ASSERT_EQ(fx.ResolvedFontFamily("a"), "Roboto");

    // Layer 1 — the metric. Device px, no layout involved.
    auto atlas = LoadRobotoAtlas();
    ASSERT_TRUE(atlas != nullptr);
    EXPECT_NEAR(atlas->GetFontLineMetrics(16.0f * kScale).height, kChromeDeviceH, kExactPx)
        << "font metrics diverged from Chrome; the layout expectation below is not attributable";

    // Layer 2 — the laid-out box. Same number, after the physical -> logical
    // division and Yoga's solve.
    const PhysicalRect r = fx.BorderBox("a");
    EXPECT_NEAR(r.H, kChromeDeviceH, kLayoutGridPx)
        << "laid-out line box is " << r.H << " device px (" << (r.H / kScale) << " logical)";
}

} // namespace
