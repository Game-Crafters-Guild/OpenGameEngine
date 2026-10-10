// Border edges paint on the device-pixel grid, symmetrically — pinned against
// real Chrome (channel chrome.exe, --force-device-scale-factor, NOT emulated
// deviceScaleFactor, whose renderer snaps in CSS space and diverges from real
// output; fixtures: Engine/Modules/UI/Tests/ChromeReference/border-edge/).
//
// Chrome's paint rules, measured at device scale 1 and 1.5 across fractional
// position sweeps (x+0/.25/.5/.75, fractional widths, 1/1.3/2/15px borders):
//   - each border-box edge snaps independently to the device grid:
//     round(edge), half away from zero (20.5 -> 21);
//   - a border's painted thickness is a whole number of device pixels:
//     max(1, floor(width_device)) — 1.5 -> 1, 1.95 -> 1, 22.5 -> 22, 3 -> 3;
//   - the band fills its columns at full intensity; no AA spread at any
//     fractional phase, and left/right profiles are mirror-identical.
//
// The engine defect this pins against: unsnapped fractional rects made each
// edge's ink depend on its own fractional phase — and the undilated Rect quad
// additionally DROPPED the outermost coverage column on edges whose adjacent
// pixel centre fell outside the quad, thinning one border by up to half its
// ink while the opposite edge kept all of it (the floating scene toolbar's
// visibly-thinner left border: measured 0.56px of ink left vs 1.02px right at
// layout x fraction 36/64).

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "IsolatedUIFixture.h"
#include "UIPixelReadback.h"

#include "UI/UIPrimitive.h"
#include "UI/UITargetSpace.h"

using GameEngine::UIElement;
using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;
using GameEngine::UITesting::PixelAt;
using GameEngine::UITesting::RenderUiToBytes;
using GameEngine::UITesting::Rgb;
using GameEngine::UI::PrimitiveMode;
using GameEngine::UI::UIPrimitive;
using namespace GameEngine;

namespace
{

// 36/64 device px: exactly representable on the 1/64 layout grid, and its
// fraction (> 0.5) is the phase class that put the toolbar's left border into
// the dropped-column case while the right border kept full ink.
constexpr float kFracX = 20.5625f;
constexpr float kBoxW = 160.0f;

bool BuildOrSkip(IsolatedUIFixture& fx, float cs, const char* xml, const char* css,
                 std::string& why)
{
    const bool built = fx.Build(cs, xml, css);
    if (!fx.DeviceAvailable())
    {
        why = "No Vulkan device available";
        return false;
    }
    EXPECT_TRUE(built) << fx.Diagnostic();
    return built;
}

void ExpectRgb(const Rgb& p, int r, int g, int b, int tol, const char* what)
{
    EXPECT_NEAR(p.R, r, tol) << what;
    EXPECT_NEAR(p.G, g, tol) << what;
    EXPECT_NEAR(p.B, b, tol) << what;
}

} // namespace

// ── Element paint snaps to the device grid (Chrome rules above) ─────────────

namespace
{

constexpr char kSnapXml[] = R"(<uielement id="root">
  <uielement id="probe"/>
</uielement>)";

constexpr char kSnapCss[] = R"(
#root  { display: flex; width: 800px; height: 600px; background-color: #000000; }
#probe { position: absolute; left: 20.5625px; top: 100px; width: 160px; height: 40px;
         background-color: #1e1e1e; border: 1px solid #ffffff; }
)";

} // namespace

// Left band [round(20.5625), +1) = [21,22); right outer edge
// round(180.5625) = 181 -> band [180,181). Full-intensity columns, clean
// page/interior on both flanks — Chrome's c-sweep rendering, byte-exact.
TEST(BorderEdgeSnap, FractionalXPaintsFullIntensityColumnsBothEdges)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, 1.0f, kSnapXml, kSnapCss, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }

    const std::vector<uint8_t> px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(px.empty());

    const uint32_t y = 120; // mid-height of the probe

    ExpectRgb(PixelAt(px, 20, y), 0, 0, 0, 1, "page left of the snapped band");
    ExpectRgb(PixelAt(px, 21, y), 255, 255, 255, 1, "left border column, full intensity");
    ExpectRgb(PixelAt(px, 22, y), 30, 30, 30, 1, "interior right of the left band");

    ExpectRgb(PixelAt(px, 179, y), 30, 30, 30, 1, "interior left of the right band");
    ExpectRgb(PixelAt(px, 180, y), 255, 255, 255, 1, "right border column, full intensity");
    ExpectRgb(PixelAt(px, 181, y), 0, 0, 0, 1, "page right of the snapped band");
}

// ── Primitive-level snap rules, both scales ─────────────────────────────────

namespace
{

constexpr char kPrimXml[] = R"(<uielement id="root">
  <uielement id="thin"/>
  <uielement id="thick"/>
</uielement>)";

constexpr char kPrimCss[] = R"(
#root  { display: flex; width: 800px; height: 600px; }
#thin  { position: absolute; left: 20.25px; top: 20px; width: 160px; height: 40px;
         background-color: #1e1e1e; border: 1px solid #ffffff; }
#thick { position: absolute; left: 20.25px; top: 80px; width: 160px; height: 40px;
         background-color: #1e1e1e; border: 5px solid #ffffff; }
)";

} // namespace

class BorderEdgeSnapPrimitives : public ::testing::TestWithParam<float>
{
};

INSTANTIATE_TEST_SUITE_P(Scales, BorderEdgeSnapPrimitives, ::testing::Values(1.0f, 1.5f));

TEST_P(BorderEdgeSnapPrimitives, RectEdgesRoundAndWidthsFloorInDevicePx)
{
    const float cs = GetParam();

    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, cs, kPrimXml, kPrimCss, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }

    // Edges: round(20.25 * cs) .. round(180.25 * cs), per edge — the painted
    // width is the difference of snapped edges, not a snapped width.
    const float x0 = std::round(20.25f * cs);
    const float x1 = std::round(180.25f * cs);

    const auto thin = fx.Primitives("thin", PrimitiveMode::Rect);
    ASSERT_EQ(thin.size(), 1u);
    EXPECT_FLOAT_EQ(thin[0].X, x0);
    EXPECT_FLOAT_EQ(thin[0].X + thin[0].W, x1);
    // 1 CSS px: 1.0 -> 1 and 1.5 -> 1 device px (Chrome floors, min 1).
    for (int i = 0; i < 4; ++i)
        EXPECT_FLOAT_EQ(thin[0].BorderWidths[i], 1.0f) << "edge " << i;

    const auto thick = fx.Primitives("thick", PrimitiveMode::Rect);
    ASSERT_EQ(thick.size(), 1u);
    // 5 CSS px: 5.0 -> 5; 7.5 -> 7 (floor — measured, not round).
    const float wantThick = std::floor(5.0f * cs);
    for (int i = 0; i < 4; ++i)
        EXPECT_FLOAT_EQ(thick[0].BorderWidths[i], wantThick) << "edge " << i;
}

// ── The quad carries the coverage it promises (raw fractional rects) ────────

namespace
{

// Raw primitives from control emitters bypass element-paint snapping, so the
// shader must still hold sdfCoverage's contract on its own: the ring's ink is
// exact at every sub-pixel phase. That needs the quad to reach every pixel
// centre the box filter touches — half a pixel beyond the outer contour.
class FractionalRectProbe : public UIElement
{
  public:
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle&,
                              float, float, float, float) override
    {
        UIPrimitive p = UI::MakeRect(kFracX, 300.0f, kBoxW, 40.0f,
                                     UI::PackColorU8(30, 30, 30, 255));
        UI::AddBorder(p, 1.0f, UI::PackFromARGB(0xFFFFFFFFu));
        ctx.Emit(p);
    }
};

constexpr char kProbeXml[] = R"(<uielement id="root"/>)";
constexpr char kProbeCss[] = R"(
#root  { display: flex; width: 800px; height: 600px; background-color: #000000; }
#probe { width: 800px; height: 600px; }
)";

} // namespace

// At x = 20.5625 the band [20.5625, 21.5625) splits 0.4375 / 0.5625 across
// columns 20 and 21; the mirrored split lands on columns 179 and 180. All four
// partial columns must render — the outer pair sits at pixel centres OUTSIDE
// the rect, which an undilated quad never rasterizes (that dropped column IS
// the toolbar's thin left border).
TEST(BorderEdgeSnap, UnsnappedRingKeepsFullInkOnBothEdges)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, 1.0f, kProbeXml, kProbeCss, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }
    auto probe = std::make_unique<FractionalRectProbe>();
    probe->SetId("probe");
    fx.Element("root")->AddChild(std::move(probe));
    fx.Settle();

    const std::vector<uint8_t> px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(px.empty());

    const uint32_t y = 320;

    // Left outer column: 0.4375 coverage of the white band over the black
    // page -> 112. Zero here means the quad clipped the coverage ramp.
    ExpectRgb(PixelAt(px, 20, y), 112, 112, 112, 3, "left outer partial column");
    // Left inner column: 0.5625 band + 0.4375 interior fill -> 157.
    ExpectRgb(PixelAt(px, 21, y), 157, 157, 157, 3, "left inner partial column");

    // Mirror on the right edge (rect right = 180.5625).
    ExpectRgb(PixelAt(px, 179, y), 128, 128, 128, 3, "right inner partial column");
    ExpectRgb(PixelAt(px, 180, y), 143, 143, 143, 3, "right outer partial column");
}

// ── Control-drawn hairlines keep their full ink at any phase ─────────────────

namespace
{

// A 1px filled MakeRect is the chart/slider/splitter hairline shape: controls
// emit it raw (no element-paint snapping), so at fractional x its ink is two
// partial columns. Both must render; the undilated quad dropped whichever
// column's pixel centre fell outside the rect — a half-intensity hairline.
class HairlineRectProbe : public UIElement
{
  public:
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle&,
                              float, float, float, float) override
    {
        ctx.Emit(UI::MakeRect(100.5f, 300.0f, 1.0f, 40.0f,
                              UI::PackColorU8(255, 255, 255, 255)));
    }
};

} // namespace

// At x = 100.5 the hairline [100.5, 101.5) splits 0.5 / 0.5 across columns
// 100 and 101 — column 101's centre (101.5) sits ON the undilated quad's
// right edge, which the top-left fill rule never shades: the old shader drew
// this hairline at half its ink. Total ink across the window must be one full
// pixel's worth, split evenly.
TEST(BorderEdgeSnap, HairlineFillAtHalfPixelKeepsFullInk)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, 1.0f, kProbeXml, kProbeCss, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }
    auto probe = std::make_unique<HairlineRectProbe>();
    probe->SetId("probe");
    fx.Element("root")->AddChild(std::move(probe));
    fx.Settle();

    const std::vector<uint8_t> px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(px.empty());

    const uint32_t y = 320;

    ExpectRgb(PixelAt(px, 99, y), 0, 0, 0, 1, "page left of the hairline");
    ExpectRgb(PixelAt(px, 100, y), 128, 128, 128, 3, "left partial column, half coverage");
    ExpectRgb(PixelAt(px, 101, y), 128, 128, 128, 3, "right partial column, half coverage");
    ExpectRgb(PixelAt(px, 102, y), 0, 0, 0, 1, "page right of the hairline");

    // Full-intensity restoration: the two partial columns together carry one
    // full pixel of ink (the undilated quad summed to ~128 here).
    const int totalInk = PixelAt(px, 100, y).R + PixelAt(px, 101, y).R;
    EXPECT_NEAR(totalInk, 255, 6) << "hairline total ink across both partial columns";
}
