// Chrome-parity pins for the background-image POSITIONING AREA.
//
// CSS `background-origin` has initial value padding-box: `background-size` and
// `background-position` resolve against the border box MINUS the border widths.
// `background-clip` keeps its own initial value border-box, so the image is
// still CLIPPED at the outer rounded contour and still extends under the border
// whenever its own box reaches there. Those are two separate boxes and the
// engine used the border box for both.
//
// The observable: a `background-size: contain` image in a bordered element.
// Against the padding box it stops at the border's inner edge, so the whole
// border ring stays visible; against the border box it fills the ring and
// (emission order being rect-then-image) hides it. That is the shipped
// thumbnail shape — `.texture-preview` 80x80/1px/r4, `.asset-field-preview`
// 40x40/1px/r4, `.settings-build-icon-slot` 96x96/1px/r8 — where a contain-fit
// texture erased the border it is supposed to sit inside.
//
// Ground truth is real Chrome 150.0.7871.186 (chrome.exe --headless=new
// --force-device-scale-factor=1 --window-size=800,600) measuring the mirror
// fixture Engine/Modules/UI/Tests/ChromeReference/background-origin/chrome_fixture.html — same geometry, same colors,
// same 16x16 intrinsic image size as the fixture below. Discriminating bytes
// for the top border band of #pad (border rgba(255,0,0,0.5), fill #00ff00,
// magenta image):
//
//   (128,127,  0)  border over FILL   <- CSS: positioning area = padding box
//   (255,  0,127)  border over IMAGE  <- border-box area, border over image
//   (255,  0,255)  IMAGE, no border   <- border-box area, image over border
//
// Chrome measured (Engine/Modules/UI/Tests/ChromeReference/background-origin/sample_pixels.ps1):
//   backdrop control            ( 39, 39, 39)
//   pad.top.band                (128,127,  0)   <- padding-box area
//   pad.left.band               (128,127,  0)
//   pad.interior                (255,  0,255)
//   pad.inside.padtop           (255,  0,255)   <- image reaches the padding-box edge
//   sq.top.band.straight        (128,127,  0)
//   sq.corner45.outer           (128,127,  0)
//   sq.corner45.inner           (255,  0,127)   <- border over image, see below
//   cov.top.band                (255,  0,127)   <- border over image, see below
//   opq.top.band / interior     (255,  0,  0) / (255,0,255)
//   tile.left.band              (255,  0,127)   <- tiles continue under the border
//   tile.first.yellow (x=30..39)(255,255,  0)   <- phase starts at the PADDING box
//   tile.first.magenta(x=40..49)(255,  0,255)
//
// Both fixtures size in BORDER-BOX terms: a `width` in this engine's CSS is the
// border box, so the mirror sets box-sizing: border-box and a 200px probe with a
// 10px border has a 180x100 positioning area, not 200x120.
//
// NOT pinned here, and still deviating: the two Chrome samples that read
// border-OVER-image. The engine emits the fill+border rect before the image
// (UIManager_PrimitiveGen.cpp), so where an image legitimately reaches into
// the border band — `cover`, explicit sizes past the padding box, offsets,
// tiling — it paints over the ring instead of under it. Correcting that is a
// paint-order change to the #805 coverage partition, not a positioning-area
// change, and it is deliberately out of this file's scope.
//
// These are geometry pins, not pixel pins: the defect is entirely in the
// emitted quad, and IsolatedUIFixture has no path to give a texture real
// content (external textures register a slot and a size, never pixels), so a
// pixel arm would need upload scaffolding that does not exist. The quad rect,
// the paint rect and the UVs fully determine what the shader can draw.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "IsolatedUIFixture.h"

#include "Rendering/Core/Device.h"
#include "UI/UIPrimitive.h"
#include "UI/UITextureSpace.h"

using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;
using namespace GameEngine;

namespace
{

// The external texture key every probe's background-image names, and its
// intrinsic size. 16x16 is square, so `contain` in a non-square box is decided
// by the box's shorter side and the fit is exact — no rounding slack to hide a
// one-box-too-big error.
constexpr char kTexKey[] = "bgorigin_probe";
constexpr uint32_t kTexW = 16;
constexpr uint32_t kTexH = 16;

constexpr float kBorder = 10.0f;

// Mirror of Engine/Modules/UI/Tests/ChromeReference/background-origin/chrome_fixture.html: 20px margins, 10px borders,
// on an opaque #272727 root. Probe rects land on integer pixel edges.
constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="row1"><uielement id="pad"/><uielement id="sq"/></uielement>
  <uielement id="row2"><uielement id="cov"/><uielement id="nb"/></uielement>
  <uielement id="row3"><uielement id="tile"/><uielement id="pct"/></uielement>
</uielement>)";

// The shared block comes FIRST: this parser resolves the cascade by source
// order within a sheet, so a later rule wins. #tile and #pct restate repeat and
// position, which the shared block also sets.
constexpr char kCss[] = R"(
#root { display: flex; flex-direction: column; width: 800px; height: 600px; background-color: #272727; }
#row1, #row2, #row3 { display: flex; flex-direction: row; }
#pad, #sq, #cov, #nb, #tile, #pct {
        margin: 20px; background-color: #00ff00; background-image: engine(bgorigin_probe);
        background-repeat: no-repeat; background-position: center; }
#pad  { width: 200px; height: 120px; border: 10px solid rgba(255,0,0,0.5); background-size: contain; }
#sq   { width: 100px; height: 100px; border: 10px solid rgba(255,0,0,0.5); border-radius: 20px;
        background-size: contain; }
#cov  { width: 200px; height: 120px; border: 10px solid rgba(255,0,0,0.5); background-size: cover; }
#nb   { width: 200px; height: 120px; background-size: contain; }
#tile { width: 200px; height: 120px; border: 10px solid rgba(255,0,0,0.5);
        background-size: 20px 20px; background-repeat: repeat; }
#pct  { width: 200px; height: 120px; border: 10px solid rgba(255,0,0,0.5);
        background-size: 50% 50%; background-position: 100% 100%; }
)";

bool BuildOrSkipAtScale(IsolatedUIFixture& fx, float contentScale, std::string& why)
{
    const bool built = fx.Build(contentScale, kXml, kCss);
    if (!fx.DeviceAvailable())
    {
        why = "No Vulkan device available";
        return false;
    }
    if (!built)
    {
        why.clear();
        EXPECT_TRUE(built) << fx.Diagnostic();
        return false;
    }
    // Registering the key resolves the texture slot and declares the intrinsic
    // size the size modes divide by; without it no probe emits a textured
    // primitive at all.
    fx.Manager().SetExternalTextureRG(kTexKey, kTexW, kTexH,
                                      UI::UITextureSpace::DisplayLinearSdr(),
                                      Rendering::TextureFormat::R16G16B16A16_FLOAT);
    fx.Settle();
    return true;
}

bool BuildOrSkip(IsolatedUIFixture& fx, std::string& why)
{
    return BuildOrSkipAtScale(fx, 1.0f, why);
}

// The one textured primitive an element emitted.
bool BgQuad(const IsolatedUIFixture& fx, const std::string& id, UI::UIPrimitive& out)
{
    const auto prims = fx.Primitives(id, UI::PrimitiveMode::Textured);
    if (prims.size() != 1u)
        return false;
    out = prims.front();
    return true;
}

// The positioning area CSS resolves background-size/position against.
PhysicalRect PaddingBox(const IsolatedUIFixture& fx, const std::string& id, float border)
{
    const PhysicalRect b = fx.BorderBox(id);
    return {b.X + border, b.Y + border, b.W - 2.0f * border, b.H - 2.0f * border};
}

void ExpectRect(const UI::UIPrimitive& p, const PhysicalRect& r, const char* what)
{
    EXPECT_FLOAT_EQ(p.X, r.X) << what;
    EXPECT_FLOAT_EQ(p.Y, r.Y) << what;
    EXPECT_FLOAT_EQ(p.W, r.W) << what;
    EXPECT_FLOAT_EQ(p.H, r.H) << what;
}

} // namespace

// #pad, the discriminator. contain against the padding box (180x100) fits the
// height: a 100x100 quad centred horizontally, its top and bottom flush with
// the border's inner edge. Against the border box (200x120) it would be
// 120x120 and cover the whole ring — the 20px difference in each axis is the
// defect, and Chrome's (128,127,0) top band is the pin it answers to.
TEST(BackgroundOriginPaddingBox, ContainFitsThePaddingBoxNotTheBorderBox)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }

    const PhysicalRect box = fx.BorderBox("pad");
    ASSERT_FLOAT_EQ(box.W, 200.0f) << "fixture drift: probe is not a 200x120 border box";
    ASSERT_FLOAT_EQ(box.H, 120.0f);

    const PhysicalRect area = PaddingBox(fx, "pad", kBorder);
    UI::UIPrimitive q{};
    ASSERT_TRUE(BgQuad(fx, "pad", q)) << "no single textured primitive for #pad";

    ExpectRect(q, {area.X + (area.W - area.H) * 0.5f, area.Y, area.H, area.H},
               "contain fits the padding box's shorter side");
}

// The shipped thumbnail shape: square, rounded, bordered. contain fills the
// padding box exactly, so the quad IS the padding box and the visible image
// stops at the border's inner edge — which is what makes the ring visible at
// all. background-clip stays border-box, so the paint rect the shader masks
// against is still the BORDER box with the border-box radii.
TEST(BackgroundOriginPaddingBox, RoundedSquareProbeFillsThePaddingBoxExactly)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }

    const PhysicalRect box = fx.BorderBox("sq");
    ASSERT_FLOAT_EQ(box.W, 100.0f) << "fixture drift: probe is not a 100x100 border box";
    ASSERT_FLOAT_EQ(box.H, 100.0f);

    const PhysicalRect area = PaddingBox(fx, "sq", kBorder);
    UI::UIPrimitive q{};
    ASSERT_TRUE(BgQuad(fx, "sq", q)) << "no single textured primitive for #sq";
    ExpectRect(q, area, "square contain fills the padding box exactly");

    EXPECT_FLOAT_EQ(q.BorderWidths[0], box.X) << "clip rect x is the border box";
    EXPECT_FLOAT_EQ(q.BorderWidths[1], box.Y) << "clip rect y is the border box";
    EXPECT_FLOAT_EQ(q.BorderWidths[2], box.W) << "clip rect w is the border box";
    EXPECT_FLOAT_EQ(q.BorderWidths[3], box.H) << "clip rect h is the border box";
    for (int i = 0; i < 4; ++i)
        EXPECT_FLOAT_EQ(q.Radii[i], 20.0f) << "clip radii stay the border-box radii, corner " << i;
}

// An element with no border has one box, so the change must be a no-op there —
// this is the arm that keeps every unbordered background-image in the editor
// byte-identical.
TEST(BackgroundOriginPaddingBox, BorderlessElementIsUnchanged)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }

    const PhysicalRect box = fx.BorderBox("nb");
    ASSERT_FLOAT_EQ(box.W, 200.0f);
    ASSERT_FLOAT_EQ(box.H, 120.0f);

    UI::UIPrimitive q{};
    ASSERT_TRUE(BgQuad(fx, "nb", q)) << "no single textured primitive for #nb";
    ExpectRect(q, {box.X + (box.W - box.H) * 0.5f, box.Y, box.H, box.H},
               "no border: positioning area IS the border box");
}

// cover scales to the padding box's LONGER side, so the quad overflows the
// positioning area and reaches into the border band — legitimately, and
// clipped to the border box by the paint rect. The pin is the size: 200x200
// from the padding box's 200 width, not 220x220 from the border box's.
TEST(BackgroundOriginPaddingBox, CoverScalesToThePaddingBoxAndOverflowsIt)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }

    const PhysicalRect box = fx.BorderBox("cov");
    const PhysicalRect area = PaddingBox(fx, "cov", kBorder);
    UI::UIPrimitive q{};
    ASSERT_TRUE(BgQuad(fx, "cov", q)) << "no single textured primitive for #cov";

    ExpectRect(q, {area.X, area.Y + (area.H - area.W) * 0.5f, area.W, area.W},
               "cover fills the padding box's longer side");
    EXPECT_LT(q.Y, box.Y + kBorder) << "cover legitimately overflows into the border band";
}

// Percentages — both size and position — resolve against the positioning area.
// 50% of 200x120 is 100x60, and 100% position puts it at the padding box's
// bottom-right corner, not the border box's.
TEST(BackgroundOriginPaddingBox, PercentSizeAndPositionResolveAgainstThePaddingBox)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }

    const PhysicalRect area = PaddingBox(fx, "pct", kBorder);
    UI::UIPrimitive q{};
    ASSERT_TRUE(BgQuad(fx, "pct", q)) << "no single textured primitive for #pct";

    const float sizeW = area.W * 0.5f;
    const float sizeH = area.H * 0.5f;
    ExpectRect(q, {area.X + area.W - sizeW, area.Y + area.H - sizeH, sizeW, sizeH},
               "50% size and 100% position both measured in the padding box");
}

// CSS border widths are LOGICAL px; the rect they inset is PHYSICAL. Mixing
// the two is the standing defect class in this file's neighbourhood (it is
// what left overflow-clip radii unscaled until 6723b8a85), so the inset is
// pinned at a non-unit content scale as well: every edge of the positioning
// area must move by border * contentScale, not by border.
TEST(BackgroundOriginPaddingBox, PositioningAreaInsetScalesWithContentScale)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkipAtScale(fx, 1.5f, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }

    constexpr float kScale = 1.5f;
    const PhysicalRect box = fx.BorderBox("sq");
    ASSERT_FLOAT_EQ(box.W, 100.0f * kScale) << "fixture drift at content scale " << kScale;
    ASSERT_FLOAT_EQ(box.H, 100.0f * kScale);

    const PhysicalRect area = PaddingBox(fx, "sq", kBorder * kScale);
    UI::UIPrimitive q{};
    ASSERT_TRUE(BgQuad(fx, "sq", q)) << "no single textured primitive for #sq";
    ExpectRect(q, area, "the border inset is physical px: border * contentScale");
}

// Tiling: the grid's phase comes from the padding box, the paint still covers
// the border box (Chrome's band reads border-over-image, so tiles continue
// under the border). The UV span carries both facts — it starts NEGATIVE by
// exactly one border width in tiles.
TEST(BackgroundOriginPaddingBox, RepeatAnchorsTheTileGridAtThePaddingBox)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }

    const PhysicalRect box = fx.BorderBox("tile");
    UI::UIPrimitive q{};
    ASSERT_TRUE(BgQuad(fx, "tile", q)) << "no single textured primitive for #tile";

    // Paint area is unchanged: the whole border box.
    ExpectRect(q, box, "tiling paints across the border box");

    constexpr float kTile = 20.0f;
    EXPECT_FLOAT_EQ(q.UvRect[0], -kBorder / kTile) << "u0: grid anchored at the padding box";
    EXPECT_FLOAT_EQ(q.UvRect[1], -kBorder / kTile) << "v0: grid anchored at the padding box";
    EXPECT_FLOAT_EQ(q.UvRect[2], (box.W - kBorder) / kTile) << "u1 spans the border box";
    EXPECT_FLOAT_EQ(q.UvRect[3], (box.H - kBorder) / kTile) << "v1 spans the border box";
}
