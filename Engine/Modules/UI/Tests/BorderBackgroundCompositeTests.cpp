// Chrome-parity pixel pins for the border/background COMPOSITE model (#805).
//
// CSS paints an element's background to the BORDER-BOX edge (background-clip:
// border-box is the default), so a translucent border composites over the
// element's own background — not over whatever is behind the element. The
// discriminating observable: a border band pixel must read
//     border.a * border.rgb + (1 - border.a) * (fill over backdrop)
// and never
//     border.a * border.rgb + (1 - border.a) * backdrop.
// Opaque borders cannot tell the two models apart (the over-composite is
// invisible at border.a == 1), which is why every pin here uses a translucent
// border and why the opaque probe is pinned as the invariant instead.
//
// Ground truth is real Chrome 150.0.7871.186 (chrome.exe --headless=new
// --force-device-scale-factor=1 --window-size=800,600), measuring the mirror
// fixture Engine/Modules/UI/Tests/ChromeReference/background-clip/chrome_fixture.html — the same geometry, colors and
// backdrop as the engine fixture below, sampled as 5x5-uniform patches
// (Engine/Modules/UI/Tests/ChromeReference/background-clip/sample_pixels.ps1):
//
//   backdrop control            ( 39, 39, 39)
//   hud   band / interior       ( 81,132,202) / (22,25,35)
//   tt    band / interior       (201, 73, 73) / (147,147,147)
//   rad   straight = corner45   ( 81,132,202)   interior (22,25,35)
//   op    band / interior       ( 92,152,232) / (22,25,35)
//   nb    centre                (147,147,147)
//
// The engine arm renders under UITargetSpace::EncodedSrgb — the shipped
// declaration since #767 — where the blend operates on encoded bytes, the
// space Chrome composites in, so the pins carry no blend-space residual: any
// disagreement beyond +-1 is a paint-model defect. The +-1 allowance is
// quantization only: the engine packs rgba(_,_,_,0.85) to alpha 217/255 =
// 0.85098 and rounds to nearest, Chrome's premultiplied 8-bit pipeline lands
// one below on these operands.
//
// Changed-pin ledger (#805): no prior test pinned the old partition's bytes —
// the old model (fill clipped to the padding box, aInner + aBorder == aOuter
// in ui_sdf.frag) had exactly zero pixel pins, so this file's pins are all
// additions. The old model's measured bytes at the fix's base commit, for the
// record: hud.band (84,135,203), tt.band (147,19,19), rad straight and
// corner45 (84,135,203) — border over BACKDROP, 3..54 levels from Chrome.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "IsolatedUIFixture.h"
#include "UIPixelReadback.h"

#include "UI/UITargetSpace.h"

using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;
using GameEngine::UITesting::RenderUiToBytes;
using GameEngine::UITesting::Rgb;
using GameEngine::UITesting::UniformCentre;
using GameEngine::UITesting::UniformPatch;
using namespace GameEngine;

namespace
{

// Mirror of Engine/Modules/UI/Tests/ChromeReference/background-clip/chrome_fixture.html: 200x120 probes, 20px margins,
// 10px borders, on an opaque #272727 root. Probe rects land on integer pixel
// edges so band and interior patches carry no partial coverage.
constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="row1">
    <uielement id="hud"/><uielement id="tt"/><uielement id="rad"/>
  </uielement>
  <uielement id="row2">
    <uielement id="op"/><uielement id="nb"/>
  </uielement>
</uielement>)";

constexpr char kCss[] = R"(
#root { display: flex; flex-direction: column; width: 800px; height: 600px; background-color: #272727; }
#row1 { display: flex; flex-direction: row; }
#row2 { display: flex; flex-direction: row; }
#hud  { width: 200px; height: 120px; margin: 20px; background-color: #161923; border: 10px solid rgba(92,152,232,0.85); }
#tt   { width: 200px; height: 120px; margin: 20px; background-color: rgba(255,255,255,0.5); border: 10px solid rgba(255,0,0,0.5); }
#rad  { width: 200px; height: 120px; margin: 20px; background-color: #161923; border: 10px solid rgba(92,152,232,0.85); border-radius: 20px; }
#op   { width: 200px; height: 120px; margin: 20px; background-color: #161923; border: 10px solid #5c98e8; }
#nb   { width: 200px; height: 120px; margin: 20px; background-color: rgba(255,255,255,0.5); }
)";

constexpr float kBorderPx = 10.0f;

bool BuildOrSkip(IsolatedUIFixture& fx, std::string& why)
{
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
    {
        why = "No Vulkan device available";
        return false;
    }
    EXPECT_TRUE(built) << fx.Diagnostic();
    return built;
}

// The sample offsets below assume the Chrome mirror's layout; a drift between
// the two fixtures would silently move every patch, so pin the geometry first.
void AssertMirrorGeometry(const IsolatedUIFixture& fx)
{
    const PhysicalRect hud = fx.BorderBox("hud");
    ASSERT_FLOAT_EQ(hud.X, 20.0f);
    ASSERT_FLOAT_EQ(hud.Y, 20.0f);
    ASSERT_FLOAT_EQ(hud.W, 200.0f);
    ASSERT_FLOAT_EQ(hud.H, 120.0f);
    const PhysicalRect rad = fx.BorderBox("rad");
    ASSERT_FLOAT_EQ(rad.X, 500.0f);
    const PhysicalRect op = fx.BorderBox("op");
    ASSERT_FLOAT_EQ(op.Y, 180.0f);
}

// Centre of the left border band: 5px in, patch spans x+3..x+7, inside the
// 10px band with >2px of margin from both AA edges.
bool BandPatch(const std::vector<uint8_t>& rgba, const PhysicalRect& box, Rgb& out,
               std::string& why)
{
    return UniformPatch(rgba, static_cast<uint32_t>(box.X + kBorderPx * 0.5f),
                        static_cast<uint32_t>(box.Y + box.H * 0.5f), out, why);
}

void ExpectRgbNear(const Rgb& p, int r, int g, int b, int tol, const char* what)
{
    EXPECT_NEAR(p.R, r, tol) << what;
    EXPECT_NEAR(p.G, g, tol) << what;
    EXPECT_NEAR(p.B, b, tol) << what;
}

} // namespace

// The #805 defect class: translucent border over a fill. The tt probe is the
// strong discriminator (54 levels between the models on G/B); the hud probe is
// the shipped HUD's exact palette, pinned to the issue's measured numbers.
TEST(BorderBackgroundComposite, TranslucentBorderCompositesOverOwnBackground)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }
    AssertMirrorGeometry(fx);

    const std::vector<uint8_t> px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(px.empty()) << "render/readback produced no pixels";

    // Instrument: the backdrop must hold its raw byte before any pin is read.
    Rgb p{};
    ASSERT_TRUE(UniformPatch(px, 120, 10, p, why)) << why;
    ASSERT_EQ(p.R, 39) << "backdrop control (#272727 raw bytes)";

    // HUD palette: rgba(92,152,232,0.85) border over #161923 fill.
    ASSERT_TRUE(BandPatch(px, fx.BorderBox("hud"), p, why)) << why;
    ExpectRgbNear(p, 81, 132, 202, 1, "Chrome: border OVER the element's own background");
    ASSERT_TRUE(UniformCentre(px, fx.BorderBox("hud"), p, why)) << why;
    ExpectRgbNear(p, 22, 25, 35, 0, "opaque fill interior is byte-exact");

    // Translucent fill under a translucent border: both layers must stack.
    ASSERT_TRUE(BandPatch(px, fx.BorderBox("tt"), p, why)) << why;
    ExpectRgbNear(p, 201, 73, 73, 1,
                  "Chrome: rgba(255,0,0,0.5) border over rgba(255,255,255,0.5) fill over #272727");
    ASSERT_TRUE(UniformCentre(px, fx.BorderBox("tt"), p, why)) << why;
    ExpectRgbNear(p, 147, 147, 147, 1, "interior: fill over backdrop only");
}

// Rounded corners: the background under the border follows the OUTER radius,
// so the corner band composites exactly like the straight band — Chrome
// measures both at (81,132,202). A model that clips the fill to the inner
// contour reads border-over-backdrop at the corner instead.
TEST(BorderBackgroundComposite, RadiusBackgroundFollowsTheOuterContourUnderTheBorder)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }
    AssertMirrorGeometry(fx);

    const std::vector<uint8_t> px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(px.empty()) << "render/readback produced no pixels";

    const PhysicalRect box = fx.BorderBox("rad");
    Rgb straight{};
    ASSERT_TRUE(BandPatch(px, box, straight, why)) << why;
    ExpectRgbNear(straight, 81, 132, 202, 1, "straight band, radius element");

    // 45-degree corner band sample: 9px diagonal from the border-box corner.
    // The patch's distance to the corner-arc centre (box + 20,20) spans
    // 12.7..18.4px — inside the 10..20px band annulus with >2px AA margin.
    Rgb corner{};
    ASSERT_TRUE(UniformPatch(px, static_cast<uint32_t>(box.X + 9.0f),
                             static_cast<uint32_t>(box.Y + 9.0f), corner, why))
        << why;
    ExpectRgbNear(corner, 81, 132, 202, 1, "corner band, radius element");

    // The composite is position-invariant along the band.
    EXPECT_EQ(corner.R, straight.R);
    EXPECT_EQ(corner.G, straight.G);
    EXPECT_EQ(corner.B, straight.B);

    Rgb interior{};
    ASSERT_TRUE(UniformCentre(px, box, interior, why)) << why;
    ExpectRgbNear(interior, 22, 25, 35, 0, "radius interior is the authored fill");
}

// Invariants the border-box background model must NOT move: an opaque border
// covers the extended background completely (border.a == 1 makes the fill
// weight under the band exactly zero), and a borderless element never enters
// the border path at all. Both probes are byte-exact against their authored
// colors and against Chrome.
TEST(BorderBackgroundComposite, OpaqueBorderAndBorderlessElementsAreByteExact)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }
    AssertMirrorGeometry(fx);

    const std::vector<uint8_t> px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(px.empty()) << "render/readback produced no pixels";

    Rgb p{};
    ASSERT_TRUE(BandPatch(px, fx.BorderBox("op"), p, why)) << why;
    ExpectRgbNear(p, 92, 152, 232, 0, "opaque border band is the authored border color");
    ASSERT_TRUE(UniformCentre(px, fx.BorderBox("op"), p, why)) << why;
    ExpectRgbNear(p, 22, 25, 35, 0, "opaque-border interior is the authored fill");

    ASSERT_TRUE(UniformCentre(px, fx.BorderBox("nb"), p, why)) << why;
    ExpectRgbNear(p, 147, 147, 147, 1, "borderless translucent fill over backdrop");
}
