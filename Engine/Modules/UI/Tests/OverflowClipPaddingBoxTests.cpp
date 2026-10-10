// Chrome-parity pins for the CSS OVERFLOW CLIP REGION.
//
// css-overflow-3: overflow:hidden clips an element's content — its children —
// to the PADDING box, with corners rounded by the INNER border-edge radii
// (border-radius reduced by the adjacent border widths). The engine instead
// pushed the child clip at the element rect (border box) with the OUTER radii
// (UIManager_PrimitiveGen.cpp, GeneratePrimitivesForElement), so any child of a
// bordered overflow:hidden container painted right up to the outer contour and
// erased the border ring wherever it overlapped it. On the shipped Save Scene
// modal (1px border, radius 8, square header child) that erased the ring along
// every corner arc; the leaf own-text clip already masked at the padding box,
// so the two writers of the same slot disagreed about which box a clip is.
//
// Ground truth is real Chrome 150.0.7871.186 (chrome.exe --headless=new
// --force-device-scale-factor=1 --window-size=800,600) rendering the mirror
// fixture Engine/Modules/UI/Tests/ChromeReference/overflow-clip/chrome_fixture.html — same geometry, same
// colors. Discriminating samples (measured from that fixture in Chrome 150):
//
//   ring probe (10px #cc3333 border, radius 40, green header child):
//     TL annulus 45deg (o+15,o+15)   (204, 51, 51)  border survives the child
//     top band        (o+150,o+5)    (204, 51, 51)  control, never covered
//     header interior (o+150,o+25)   ( 34,204, 34)  child paints inside only
//   icon probe (3px #444444 border, radius 0, orange child over the right band):
//     right band      (o+298,o+60)   ( 68, 68, 68)  child clipped at padding
//     child interior  (o+295,o+60)   (255,136, 32)  edge x=297, band intact
//
// A border-box clip reads the child's color at both discriminators (measured
// red, this engine, before the fix: annulus (34,204,34); band (255,136,32)).
//
// Changed-pin ledger: no existing test pinned the child-clip slot's SHAPE for
// a BORDERED container — ClipRadiiPhysicalTests' clipper is borderless, where
// padding box == border box and inner radii == outer radii, so its pins are
// invariant under this change (that equality is also why editor chrome, which
// is overwhelmingly borderless, is unaffected).

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "IsolatedUIFixture.h"
#include "UIPixelReadback.h"

#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"
#include "UI/UITargetSpace.h"

using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;
using GameEngine::UITesting::PixelAt;
using GameEngine::UITesting::RenderUiToBytes;
using GameEngine::UITesting::Rgb;
using namespace GameEngine;

namespace
{

constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="ring">
    <uielement id="ringheader"/>
  </uielement>
  <uielement id="icon">
    <uielement id="iconchild"/>
  </uielement>
  <uielement id="plain">
    <uielement id="plainchild"/>
  </uielement>
  <uielement id="uneven">
    <uielement id="unevenchild"/>
  </uielement>
  <uielement id="pctwide">
    <uielement id="pctwidechild"/>
  </uielement>
</uielement>)";

// Mirrors Engine/Modules/UI/Tests/ChromeReference/overflow-clip/chrome_fixture.html (the icon child is in-flow
// here — margin-left 277 from the content box lands its left edge 280px from
// the border box, the same span the mirror's absolutely-positioned child
// covers; the discriminating band pixel is identical either way).
// #plain is the borderless control: padding box == border box, so its clip
// slot must be byte-identical before and after the padding-box fix.
constexpr char kCss[] = R"(
#root { display: flex; flex-direction: column; width: 800px; height: 600px; background-color: #101010; }
#ring { margin: 40px; width: 300px; height: 120px; background-color: #2c2c2c;
        border: 10px solid #cc3333; border-radius: 40px; overflow: hidden;
        display: flex; flex-direction: column; flex-shrink: 0; }
#ringheader { height: 60px; background-color: #22cc22; flex-shrink: 0; }
#icon { margin-left: 40px; width: 300px; height: 120px; background-color: #2c2c2c;
        border: 3px solid #444444; overflow: hidden;
        display: flex; flex-direction: row; flex-shrink: 0; }
#iconchild { margin-left: 277px; margin-top: 37px; width: 40px; height: 40px;
             background-color: #ff8820; flex-shrink: 0; }
#plain { margin: 40px; width: 100px; height: 60px; background-color: #202020;
         overflow: hidden; border-radius: 8px; display: flex; flex-direction: column;
         flex-shrink: 0; }
#plainchild { width: 200px; height: 200px; background-color: #808080; flex-shrink: 0; }
#uneven { margin-left: 40px; width: 200px; height: 120px; background-color: #202020;
          border-style: solid; border-color: #888888; border-width: 2px 4px 6px 10px;
          border-radius: 20px; overflow: hidden; display: flex; flex-direction: column;
          flex-shrink: 0; }
#unevenchild { width: 400px; height: 400px; background-color: #808080; flex-shrink: 0; }
#pctwide { margin-left: 40px; width: 200px; height: 60px; background-color: #202020;
           border-style: solid; border-color: #888888; border-width: 4px 8px 6px 10px;
           border-radius: 50%; overflow: hidden; display: flex; flex-direction: column;
           flex-shrink: 0; }
#pctwidechild { width: 400px; height: 400px; background-color: #808080; flex-shrink: 0; }
)";

bool BuildOrSkip(IsolatedUIFixture& fx, float scale, std::string& why)
{
    const bool built = fx.Build(scale, kXml, kCss);
    if (!fx.DeviceAvailable())
    {
        why = "No Vulkan device available";
        return false;
    }
    EXPECT_TRUE(built) << fx.Diagnostic();
    return built;
}

const UI::UIClipRect* ClipSlotOf(const IsolatedUIFixture& fx, const std::string& id)
{
    UIElement* el = fx.Element(id);
    if (!el || el->m_ClipSlotIdx == UI::kNoClip)
        return nullptr;
    return fx.Manager().PeekClipRectForTesting(el->m_ClipSlotIdx);
}

void ExpectRgbNear(const Rgb& got, int r, int g, int b, int tol, const char* what)
{
    EXPECT_NEAR(static_cast<int>(got.R), r, tol) << what;
    EXPECT_NEAR(static_cast<int>(got.G), g, tol) << what;
    EXPECT_NEAR(static_cast<int>(got.B), b, tol) << what;
}

} // namespace

// The clip slot a bordered overflow:hidden container pushes for its children
// is the padding box, radii reduced to the inner border edge — the same shape
// the leaf own-text writer (EmitTextPrimitives) and the drain rewrite produce.
TEST(OverflowClipPaddingBox, ChildClipSlotIsPaddingBoxWithInnerRadii)
{
    for (const float scale : {1.0f, 1.5f, 2.0f})
    {
        IsolatedUIFixture fx;
        std::string why;
        if (!BuildOrSkip(fx, scale, why))
            GTEST_SKIP() << why;

        const UI::UIClipRect* cr = ClipSlotOf(fx, "ring");
        ASSERT_NE(cr, nullptr) << "overflow:hidden parent must own a clip slot (scale " << scale << ")";
        // Border box (40,40,300,120), border 10 -> padding box (50,50,280,100),
        // outer radius 40 -> inner 30. All CSS-logical, scaled to physical.
        EXPECT_FLOAT_EQ(cr->Rect[0], 50.0f * scale) << "scale " << scale;
        EXPECT_FLOAT_EQ(cr->Rect[1], 50.0f * scale) << "scale " << scale;
        EXPECT_FLOAT_EQ(cr->Rect[2], 280.0f * scale) << "scale " << scale;
        EXPECT_FLOAT_EQ(cr->Rect[3], 100.0f * scale) << "scale " << scale;
        for (int i = 0; i < 4; ++i)
            EXPECT_FLOAT_EQ(cr->Radii[i], 30.0f * scale) << "corner " << i << " scale " << scale;
    }
}

// Borderless control: with no border the padding box IS the border box, so the
// fix must not move this slot by a single unit.
TEST(OverflowClipPaddingBox, BorderlessClipSlotUnchanged)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, 1.0f, why))
        GTEST_SKIP() << why;

    const PhysicalRect box = fx.BorderBox("plain");
    const UI::UIClipRect* cr = ClipSlotOf(fx, "plain");
    ASSERT_NE(cr, nullptr);
    EXPECT_FLOAT_EQ(cr->Rect[0], box.X);
    EXPECT_FLOAT_EQ(cr->Rect[1], box.Y);
    EXPECT_FLOAT_EQ(cr->Rect[2], box.W);
    EXPECT_FLOAT_EQ(cr->Rect[3], box.H);
    for (int i = 0; i < 4; ++i)
        EXPECT_FLOAT_EQ(cr->Radii[i], 8.0f);
}

// Unequal adjacent border widths: the clip contour and the border ring's inner
// contour are the same curve, so the two places that derive it must derive it the
// same way. CSS's inner corner is an ELLIPSE — the horizontal radius loses the
// left/right border and the vertical the top/bottom one — and both places now
// express that: InnerClipRadii per axis on the CPU, radInnerX/radInnerY in
// ui_sdf.frag's Rect branch. This pins their agreement, and because the rule is
// the spec's, it is also what Chrome draws here.
//
// border-width: 2px 4px 6px 10px  (top right bottom left), border-radius 20:
//   TL (20-L=10, 20-T=18)     TR (20-R=16, 20-T=18)
//   BR (20-R=16, 20-B=14)     BL (20-L=10, 20-B=14)
// The old scalar collapse took min(adjacent) into both axes — 18/18/16/14 — so
// every corner but BR's horizontal axis moves, and a regression to either that
// collapse or a `max` one fails here.
TEST(OverflowClipPaddingBox, InnerRadiiPerAxisMatchRingInnerContour)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, 1.0f, why))
        GTEST_SKIP() << why;

    const UI::UIClipRect* cr = ClipSlotOf(fx, "uneven");
    ASSERT_NE(cr, nullptr);

    const PhysicalRect box = fx.BorderBox("uneven");
    EXPECT_FLOAT_EQ(cr->Rect[0], box.X + 10.0f) << "padding box starts past border-left";
    EXPECT_FLOAT_EQ(cr->Rect[1], box.Y + 2.0f) << "padding box starts past border-top";
    EXPECT_FLOAT_EQ(cr->Rect[2], box.W - 14.0f) << "width loses left+right borders";
    EXPECT_FLOAT_EQ(cr->Rect[3], box.H - 8.0f) << "height loses top+bottom borders";

    EXPECT_FLOAT_EQ(cr->Radii[0], 10.0f) << "TL horizontal loses border-left";
    EXPECT_FLOAT_EQ(cr->Radii[1], 16.0f) << "TR horizontal loses border-right";
    EXPECT_FLOAT_EQ(cr->Radii[2], 16.0f) << "BR horizontal loses border-right";
    EXPECT_FLOAT_EQ(cr->Radii[3], 10.0f) << "BL horizontal loses border-left";

    EXPECT_FLOAT_EQ(cr->RadiiY[0], 18.0f) << "TL vertical loses border-top";
    EXPECT_FLOAT_EQ(cr->RadiiY[1], 18.0f) << "TR vertical loses border-top";
    EXPECT_FLOAT_EQ(cr->RadiiY[2], 14.0f) << "BR vertical loses border-bottom";
    EXPECT_FLOAT_EQ(cr->RadiiY[3], 14.0f) << "BL vertical loses border-bottom";
}

// The seam under a TRUE ellipse, which every other clip test here misses: the
// others use a uniform pixel radius (circular whatever the box shape) or a square
// box (where the ellipse IS the circle), so none of them can tell a per-axis clip
// contour from a collapsed one.
//
// #pctwide is 200x60 at `border-radius: 50%` — a 100x30 corner, the axes 3.3x
// apart — with borders 4/8/6/10 (T R B L), so all four corners differ and the two
// axes of each differ:
//   TL (100-10, 30-4) = (90, 26)     TR (100-8, 30-4) = (92, 26)
//   BR (100-8,  30-6) = (92, 24)     BL (100-10, 30-6) = (90, 24)
// The old scalar collapse resolved the percentage to min(100,30)=30 and then
// subtracted min(adjacent), landing every corner near 26 on BOTH axes — nowhere
// near 90 horizontally, so a regression cannot pass this.
//
// The relationship, not just the numbers, is what matters: the clip radii must
// equal the PAINTED radii less the per-axis border, because that is the rule
// ui_sdf.frag applies to derive the ring's inner contour (radInnerX/radInnerY).
// Asserting it against the primitive the same frame emitted is what pins the two
// derivations together rather than pinning each to a constant independently.
TEST(OverflowClipPaddingBox, EllipticalClipContourMatchesThePaintedRingPerAxis)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, 1.0f, why))
        GTEST_SKIP() << why;

    const PhysicalRect box = fx.BorderBox("pctwide");
    ASSERT_FLOAT_EQ(box.W, 200.0f);
    ASSERT_FLOAT_EQ(box.H, 60.0f) << "the specimen must stay non-square";

    const UI::UIClipRect* cr = ClipSlotOf(fx, "pctwide");
    ASSERT_NE(cr, nullptr);

    // The painted outer contour: 50% per axis of a 200x60 box.
    const std::vector<UI::UIPrimitive> prims = fx.Primitives("pctwide");
    const UI::UIPrimitive* rect = nullptr;
    for (const UI::UIPrimitive& p : prims)
    {
        if (UI::GetMode(p.ModeAndFlags) == UI::PrimitiveMode::Rect)
        {
            rect = &p;
            break;
        }
    }
    ASSERT_NE(rect, nullptr) << "no background rect emitted";

    ASSERT_FLOAT_EQ(rect->Radii[0], 100.0f) << "painted horizontal semi-axis";
    ASSERT_FLOAT_EQ(rect->RadiiY[0], 30.0f) << "painted vertical semi-axis";

    // bw = (L, T, R, B) = (10, 4, 8, 6): horizontal loses left/right, vertical
    // loses top/bottom — the shader's rule, applied to the same painted radii.
    const float bwL = 10.0f, bwT = 4.0f, bwR = 8.0f, bwB = 6.0f;
    const float expectX[4] = {rect->Radii[0] - bwL, rect->Radii[1] - bwR,
                              rect->Radii[2] - bwR, rect->Radii[3] - bwL};
    const float expectY[4] = {rect->RadiiY[0] - bwT, rect->RadiiY[1] - bwT,
                              rect->RadiiY[2] - bwB, rect->RadiiY[3] - bwB};
    const char* corners[4] = {"TL", "TR", "BR", "BL"};

    for (int i = 0; i < 4; ++i)
    {
        EXPECT_FLOAT_EQ(cr->Radii[i], expectX[i])
            << corners[i] << " horizontal clip radius must be the painted one less the side border";
        EXPECT_FLOAT_EQ(cr->RadiiY[i], expectY[i])
            << corners[i] << " vertical clip radius must be the painted one less the top/bottom border";
        // The axes must stay apart: equal axes here would mean a collapse.
        EXPECT_GT(cr->Radii[i], cr->RadiiY[i] * 2.0f)
            << corners[i] << " clip corner collapsed to a circle";
    }
}

// The pixels Chrome renders: the border ring survives a child that reaches
// into the corner (the child is clipped along the inner border edge, not the
// outer contour).
TEST(OverflowClipPaddingBox, CornerAnnulusKeepsBorderUnderChild)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, 1.0f, why))
        GTEST_SKIP() << why;

    const std::vector<uint8_t> px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(px.empty());

    const PhysicalRect ring = fx.BorderBox("ring");
    const uint32_t ox = static_cast<uint32_t>(ring.X);
    const uint32_t oy = static_cast<uint32_t>(ring.Y);

    // 45-degree midpoint of the TL border annulus: centre (o+40,o+40), ring
    // midline radius 35 -> offset 40 - 35/sqrt(2) = 15.25. Chrome: (204,51,51).
    ExpectRgbNear(PixelAt(px, ox + 15, oy + 15), 204, 51, 51, 6,
                  "TL annulus 45deg: border must survive the header child");
    // All four corners; the fixture's radii are uniform so the mirrored
    // offsets sample the same annulus midline.
    const uint32_t w = static_cast<uint32_t>(ring.W), h = static_cast<uint32_t>(ring.H);
    ExpectRgbNear(PixelAt(px, ox + w - 16, oy + 15), 204, 51, 51, 6, "TR annulus 45deg");
    ExpectRgbNear(PixelAt(px, ox + w - 16, oy + h - 16), 204, 51, 51, 6, "BR annulus 45deg");
    ExpectRgbNear(PixelAt(px, ox + 15, oy + h - 16), 204, 51, 51, 6, "BL annulus 45deg");

    // Controls that must hold on both sides of the fix.
    ExpectRgbNear(PixelAt(px, ox + 150, oy + 5), 204, 51, 51, 3, "top band control");
    ExpectRgbNear(PixelAt(px, ox + 150, oy + 25), 34, 204, 34, 3, "header interior control");
}

// A child overlapping a straight border edge clips at the padding edge; the
// band stays intact. Radius 0 — the clip region is the padding box even with
// no rounding in play.
TEST(OverflowClipPaddingBox, ChildOverBandClipsAtPaddingEdge)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, 1.0f, why))
        GTEST_SKIP() << why;

    const std::vector<uint8_t> px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(px.empty());

    const PhysicalRect icon = fx.BorderBox("icon");
    const uint32_t ox = static_cast<uint32_t>(icon.X);
    const uint32_t oy = static_cast<uint32_t>(icon.Y);

    // Chrome: child visible up to the padding edge (x=o+297)...
    ExpectRgbNear(PixelAt(px, ox + 295, oy + 60), 255, 136, 32, 3,
                  "child interior: visible up to the padding edge");
    // ...and the 3px right border band (x = o+297..o+299) stays border-gray.
    ExpectRgbNear(PixelAt(px, ox + 298, oy + 60), 68, 68, 68, 3,
                  "right band: child must clip at the padding edge, not the outer one");
}
