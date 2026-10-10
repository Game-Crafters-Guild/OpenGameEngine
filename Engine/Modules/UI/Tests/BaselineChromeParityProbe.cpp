// A MEASUREMENT harness, not a behaviour test. It lays out a fixed case table
// under `align-items: baseline` and again under `align-items: flex-end`, and
// prints every number a browser can be asked for the same markup: the row's
// height, each item's top and height, and each item's first glyph baseline.
//
// The flex-end arm is not decoration. Aligning box bottoms IS flex-end, so a
// case where the two arms print the SAME numbers is a case that cannot tell a
// real baseline function from no baseline function at all. Printing both makes
// that visible per case instead of leaving it to be assumed.
//
// Nothing here asserts a Chrome number. The comparison is done outside, against
// Chrome rendering the same Roboto-Regular.ttf this build stages.
//
// DISABLED by default: it builds 58 isolated UI fixtures per scale and costs
// about two minutes, which is not a price every UITextLayoutTests run should pay for a
// harness that asserts nothing. The parity it measures is pinned as real
// expectations in TextBaselineAlignmentTests.cpp; run this when those numbers
// need re-deriving or extending:
//
//   UITextLayoutTests.exe --gtest_also_run_disabled_tests \
//               --gtest_filter="*BaselineChromeParityProbe*" > engine.log

#include "IsolatedUIFixture.h"

#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;

namespace
{

// MakeSlugGlyph stores the undilated em-space bounds in UvRect and H spans
// them, so inverting the quad-top formula recovers the drawn baseline. Same
// readback BaselineSnapTests uses; see that file for the derivation.
float RecoveredBaseline(const GameEngine::UI::UIPrimitive& p)
{
    const float emSpan = p.UvRect[1] - p.UvRect[3];
    const float emScale = p.H / emSpan;
    return p.Y + p.UvRect[1] * emScale;
}

bool HasUsableEmSpan(const GameEngine::UI::UIPrimitive& p)
{
    return (p.UvRect[1] - p.UvRect[3]) > 1e-4f && p.H > 1e-4f;
}

// Smallest baseline among the element's glyphs — its FIRST line, the value CSS
// aligns on. Returns false when the element emitted no usable glyph.
bool FirstBaseline(const IsolatedUIFixture& fx, const std::string& id, float& out)
{
    bool found = false;
    for (const auto& p : fx.Primitives(id, GameEngine::UI::PrimitiveMode::Slug))
    {
        if (!HasUsableEmSpan(p))
            continue;
        const float b = RecoveredBaseline(p);
        if (!found || b < out)
        {
            out = b;
            found = true;
        }
    }
    return found;
}

struct Case
{
    const char* Name;
    const char* Xml;
    // Everything except the row's align-items, which the harness varies.
    const char* Css;
    // Extra declarations for the row itself — `flex-wrap` and the width that
    // decides where it wraps. Empty for the single-line cases.
    const char* RowExtra = "";
};

constexpr char kRootCss[] =
    "#root { display: flex; flex-direction: column; width: 400px; height: 300px; }\n";

// Two labels, the canonical shape. Font sizes are per-case.
constexpr char kTwoLabelXml[] = R"(<uielement id="root">
  <uielement id="row">
    <label id="a">Hxg</label>
    <label id="b">Hxg</label>
  </uielement>
</uielement>)";

// An item with no text at all: CSS gives it the bottom margin edge.
constexpr char kTextPlusBoxXml[] = R"(<uielement id="root">
  <uielement id="row">
    <label id="a">Hxg</label>
    <uielement id="b"></uielement>
  </uielement>
</uielement>)";

// The first item's baseline has to come from a descendant.
constexpr char kNestedXml[] = R"(<uielement id="root">
  <uielement id="row">
    <uielement id="a"><label id="ai">Hxg</label></uielement>
    <label id="b">Hxg</label>
  </uielement>
</uielement>)";

constexpr char kMultiLineXml[] = R"(<uielement id="root">
  <uielement id="row">
    <label id="a">Hxg
Hxg</label>
    <label id="b">Hxg</label>
  </uielement>
</uielement>)";

// Wrapped rows. Every item carries an explicit width so the wrap POINT is
// arithmetic rather than a consequence of text measurement — otherwise a
// font-metric difference would change which items share a flex line and the
// comparison would be between two different layouts.
constexpr char kFourLabelXml[] = R"(<uielement id="root">
  <uielement id="row">
    <label id="a">Hxg</label>
    <label id="b">Hxg</label>
    <label id="c">Hxg</label>
    <label id="d">Hxg</label>
  </uielement>
</uielement>)";

constexpr char kSixLabelXml[] = R"(<uielement id="root">
  <uielement id="row">
    <label id="a">Hxg</label>
    <label id="b">Hxg</label>
    <label id="c">Hxg</label>
    <label id="d">Hxg</label>
    <label id="e">Hxg</label>
    <label id="f">Hxg</label>
  </uielement>
</uielement>)";

// A two-line item on the SECOND flex line: that line aligns on its FIRST
// baseline, so taking the last would drop the item's partner a whole line box.
constexpr char kFourLabelMultiXml[] = R"(<uielement id="root">
  <uielement id="row">
    <label id="a">Hxg</label>
    <label id="b">Hxg</label>
    <label id="c">Hxg
Hxg</label>
    <label id="d">Hxg</label>
  </uielement>
</uielement>)";

constexpr char kWidth90[] =
    "#a, #b, #c, #d, #e, #f { width: 90px; }\n";
constexpr char kWrapRow[] = "flex-wrap: wrap; width: 200px;";
constexpr char kNoWrapRow[] = "flex-wrap: nowrap; width: 200px;";

const Case kCases[] = {
    // --- 1. two items, different font sizes -------------------------------
    {"C1_size_32_12", kTwoLabelXml,
     "#a { font-family: Roboto; font-size: 32px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 12px; color: #fff; }\n"},
    {"C2_size_12_24", kTwoLabelXml,
     "#a { font-family: Roboto; font-size: 12px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 24px; color: #fff; }\n"},
    {"C3_size_13_16", kTwoLabelXml,
     "#a { font-family: Roboto; font-size: 13px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 16px; color: #fff; }\n"},
    {"C4_size_11_48", kTwoLabelXml,
     "#a { font-family: Roboto; font-size: 11px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 48px; color: #fff; }\n"},

    // --- 2. different line-heights at the SAME font size -------------------
    {"C5_lh_unitless_1_vs_2", kTwoLabelXml,
     "#a { font-family: Roboto; font-size: 16px; line-height: 1; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 16px; line-height: 2; color: #fff; }\n"},
    {"C6_lh_length_24_vs_40", kTwoLabelXml,
     "#a { font-family: Roboto; font-size: 16px; line-height: 24px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 16px; line-height: 40px; color: #fff; }\n"},
    {"C7_lh_normal_vs_unitless_2", kTwoLabelXml,
     "#a { font-family: Roboto; font-size: 16px; line-height: normal; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 16px; line-height: 2; color: #fff; }\n"},
    {"C8_lh_63px_on_32_vs_12", kTwoLabelXml,
     "#a { font-family: Roboto; font-size: 32px; line-height: 63px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 12px; color: #fff; }\n"},

    // --- the IDENTICAL case: baseline and flex-end cannot differ here ------
    // Same face, same size, same line-height, one line each, no padding: the
    // items' ascents and descents match, so both keywords put both tops at 0.
    // A harness that cannot show these two arms agreeing is not measuring
    // alignment at all.
    {"C9_identical_16_16", kTwoLabelXml,
     "#a { font-family: Roboto; font-size: 16px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 16px; color: #fff; }\n"},

    // --- 3. multi-line: CSS aligns on the FIRST baseline -------------------
    {"C10_multiline_32_vs_12", kMultiLineXml,
     "#a { font-family: Roboto; font-size: 32px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 12px; color: #fff; }\n"},
    {"C11_multiline_lh30_vs_12", kMultiLineXml,
     "#a { font-family: Roboto; font-size: 16px; line-height: 30px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 12px; color: #fff; }\n"},

    // --- 4. padding on the items ------------------------------------------
    {"C12_pad_top_on_small", kTwoLabelXml,
     "#a { font-family: Roboto; font-size: 32px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 12px; padding-top: 6px; color: #fff; }\n"},
    {"C13_pad_both_items", kTwoLabelXml,
     "#a { font-family: Roboto; font-size: 32px; padding-top: 10px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 12px; padding-bottom: 8px; color: #fff; }\n"},
    {"C14_pad_bottom_only", kTwoLabelXml,
     "#a { font-family: Roboto; font-size: 32px; padding-bottom: 12px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 12px; color: #fff; }\n"},

    // --- 5. text mixed with a box that has no line boxes -------------------
    {"C15_text_plus_box", kTextPlusBoxXml,
     "#a { font-family: Roboto; font-size: 32px; color: #fff; }\n"
     "#b { width: 20px; height: 20px; background-color: #888; }\n"},
    {"C16_text_plus_box_margin", kTextPlusBoxXml,
     "#a { font-family: Roboto; font-size: 32px; color: #fff; }\n"
     "#b { width: 20px; height: 20px; margin-bottom: 7px; background-color: #888; }\n"},

    // --- line-height TIGHTER than the font: leading goes negative ----------
    // Roboto at 16px is 21px tall, so line-height 10px gives half-leading
    // -5.5 and line-height 13px gives exactly -4. The pair is a matched
    // control: if a divergence tracks the FRACTION rather than the sign, the
    // 13px row matches and the 10px row does not.
    {"C19_lh_10_frac_vs_normal", kTwoLabelXml,
     "#a { font-family: Roboto; font-size: 16px; line-height: 10px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 16px; color: #fff; }\n"},
    {"C20_lh_13_integral_vs_normal", kTwoLabelXml,
     "#a { font-family: Roboto; font-size: 16px; line-height: 13px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 16px; color: #fff; }\n"},

    // --- 6. nested text: the baseline comes from a descendant --------------
    {"C17_nested_32_vs_12", kNestedXml,
     "#a { display: flex; }\n"
     "#ai { font-family: Roboto; font-size: 32px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 12px; color: #fff; }\n"},
    {"C18_nested_pad5_32_vs_12", kNestedXml,
     "#a { display: flex; padding: 5px; }\n"
     "#ai { font-family: Roboto; font-size: 32px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 12px; color: #fff; }\n"},

    // --- 7. WRAPPED rows: a baseline is resolved per FLEX LINE --------------
    // Yoga resolves `align-items: baseline` against the line an item landed
    // on, so a wrapped row has as many baselines as it has lines. Nothing in
    // the single-line cases above can see that, and every shipped rule that
    // pairs `align-items: baseline` with `flex-wrap: wrap` depends on it.
    {"W1_wrap_sizes", kFourLabelXml,
     "#a { font-family: Roboto; font-size: 32px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 12px; color: #fff; }\n"
     "#c { font-family: Roboto; font-size: 24px; color: #fff; }\n"
     "#d { font-family: Roboto; font-size: 16px; color: #fff; }\n",
     kWrapRow},
    {"W2_wrap_lineheights", kFourLabelXml,
     "#a { font-family: Roboto; font-size: 16px; line-height: normal; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 16px; line-height: 2; color: #fff; }\n"
     "#c { font-family: Roboto; font-size: 16px; line-height: 30px; color: #fff; }\n"
     "#d { font-family: Roboto; font-size: 16px; line-height: normal; color: #fff; }\n",
     kWrapRow},
    {"W3_wrap_three_lines", kSixLabelXml,
     "#a { font-family: Roboto; font-size: 32px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 12px; color: #fff; }\n"
     "#c { font-family: Roboto; font-size: 24px; color: #fff; }\n"
     "#d { font-family: Roboto; font-size: 16px; color: #fff; }\n"
     "#e { font-family: Roboto; font-size: 48px; color: #fff; }\n"
     "#f { font-family: Roboto; font-size: 11px; color: #fff; }\n",
     kWrapRow},
    {"W4_wrap_multiline_second_line", kFourLabelMultiXml,
     "#a { font-family: Roboto; font-size: 32px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 12px; color: #fff; }\n"
     "#c { font-family: Roboto; font-size: 16px; color: #fff; }\n"
     "#d { font-family: Roboto; font-size: 32px; color: #fff; }\n",
     kWrapRow},
    // The control the wrapped arm needs: identical items and widths, but the
    // row may not wrap. One flex line cannot distinguish a per-line baseline
    // from a whole-container one, so W1 differing from this is what shows the
    // wrapped cases are exercising the per-line path at all.
    {"W5_nowrap_control", kFourLabelXml,
     "#a { font-family: Roboto; font-size: 32px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 12px; color: #fff; }\n"
     "#c { font-family: Roboto; font-size: 24px; color: #fff; }\n"
     "#d { font-family: Roboto; font-size: 16px; color: #fff; }\n",
     kNoWrapRow},

    // --- 8. `white-space` must not move a baseline --------------------------
    // In CSS it has nothing to do with vertical placement, and Chrome puts
    // N1 and N2 on the same pixel. This engine shapes wrapping and
    // non-wrapping text through different code, so the pair is the control
    // that catches a rounding applied to only one of them.
    {"N1_ws_normal_lh2", kTwoLabelXml,
     "#a { font-family: Roboto; font-size: 16px; line-height: 2; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 16px; color: #fff; }\n"},
    {"N2_ws_nowrap_lh2", kTwoLabelXml,
     "#a { font-family: Roboto; font-size: 16px; line-height: 2; white-space: nowrap; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 16px; color: #fff; }\n"},
    {"N3_ws_normal_lh25", kTwoLabelXml,
     "#a { font-family: Roboto; font-size: 16px; line-height: 25px; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 16px; color: #fff; }\n"},
    {"N4_ws_nowrap_lh25", kTwoLabelXml,
     "#a { font-family: Roboto; font-size: 16px; line-height: 25px; white-space: nowrap; color: #fff; }\n"
     "#b { font-family: Roboto; font-size: 16px; color: #fff; }\n"},
};

const char* const kProbedIds[] = {"a", "ai", "b", "c", "d", "e", "f"};

void EmitCase(const Case& c, const char* align, float scale)
{
    IsolatedUIFixture fx;
    std::string css = std::string(kRootCss) + "#row { display: flex; flex-direction: row; " +
                      c.RowExtra + " align-items: " + align + "; }\n" +
                      (c.RowExtra[0] != '\0' ? kWidth90 : "") + c.Css;
    if (!fx.Build(scale, c.Xml, css))
    {
        std::printf("@@CASE {\"name\":\"%s\",\"align\":\"%s\",\"scale\":%g,\"error\":\"%s\"}\n",
                    c.Name, align, scale, fx.Diagnostic().c_str());
        return;
    }

    const PhysicalRect row = fx.BorderBox("row");
    std::printf("@@CASE {\"name\":\"%s\",\"align\":\"%s\",\"scale\":%g,\"font\":\"%s\","
                "\"row\":{\"h\":%.4f}",
                c.Name, align, scale, fx.ResolvedFontFamily("a").c_str(), row.H);

    std::printf(",\"items\":{");
    bool first = true;
    for (const char* id : kProbedIds)
    {
        if (!fx.Element(id))
            continue;
        const PhysicalRect box = fx.BorderBox(id);
        float bl = 0.0f;
        const bool hasBl = FirstBaseline(fx, id, bl);
        std::printf("%s\"%s\":{\"top\":%.4f,\"h\":%.4f,", first ? "" : ",", id, box.Y - row.Y,
                    box.H);
        if (hasBl)
            std::printf("\"bl\":%.4f}", bl - row.Y);
        else
            std::printf("\"bl\":null}");
        first = false;
    }
    std::printf("}}\n");
}

class BaselineChromeParityProbe : public ::testing::TestWithParam<float>
{
};

TEST_P(BaselineChromeParityProbe, DISABLED_DumpGeometry)
{
    const float scale = GetParam();

    // One build up front so a missing Vulkan device is reported as a skip
    // rather than as eighteen identical error lines.
    {
        IsolatedUIFixture probe;
        const std::string css = std::string(kRootCss) +
                                "#row { display: flex; flex-direction: row; align-items: baseline; "
                                "}\n#a { font-family: Roboto; font-size: 32px; }\n";
        if (!probe.Build(scale, kTwoLabelXml, css) && !probe.DeviceAvailable())
            GTEST_SKIP() << "no Vulkan device";
        // The whole comparison is against Roboto. A silent fall-through to a
        // system face would leave every number below unattributable.
        ASSERT_EQ(probe.ResolvedFontFamily("a"), "Roboto")
            << "staged Roboto-Regular.ttf did not resolve; measurements would be against another "
               "face";
    }

    for (const Case& c : kCases)
    {
        EmitCase(c, "baseline", scale);
        EmitCase(c, "flex-end", scale);
    }
}

INSTANTIATE_TEST_SUITE_P(Scales, BaselineChromeParityProbe, ::testing::Values(1.0f, 2.0f));

} // namespace
