// The CSS box model INSIDE flex layout, pinned against Chrome.
//
// BorderBoxModelTests proves a single bordered box insets its child.
// BorderPaintPrimitiveTests proves the renderer draws that box. Neither
// exercises the box model where it actually lives in shipped UI: a flex
// container with border AND padding distributing free space to several items,
// items that carry their own border+padding, percentage children resolving
// against a bordered container, absolutes anchored inside one, and containers
// nested two deep. Those are the shapes the border merge (dc05f3286, border
// width joins the Yoga box model) can regress without touching a single
// assertion in the two existing files.
//
// GROUND TRUTH. Real headless Chrome, not an emulated device ratio:
//   chrome.exe --headless=new --disable-gpu --force-device-scale-factor=1
//              --virtual-time-budget=3000 --dump-dom boxmodel-in-flex.html
// and the same command with --force-device-scale-factor=1.5. Every element is
// measured with getBoundingClientRect and reported relative to its fixture
// container; the raw numbers are quoted per fixture below.
//
// The Chrome fixture sets `box-sizing: border-box` on every element, which is
// the engine's only box model (there is no box-sizing property, and Yoga's
// width is always the border box). It also declares flex-direction, flex-wrap,
// align-items, align-content, flex-grow, flex-shrink and flex-basis explicitly
// on every node, because Yoga's native defaults differ from CSS's — leaving any
// of them implicit would let a default divergence contaminate a fixture that is
// testing something else.
//
// SCALES. Every fixture here runs at content scale 1.0 and 1.5. Yoga solves in
// logical px while primitives are emitted in physical px, so an inset applied
// in the wrong space is invisible at 1.0. Chrome's CSS-px layout for these
// fixtures is byte-identical at device scale 1 and 1.5 (verified, not assumed),
// which is why one set of literals serves both arms — every value here is a
// whole logical px, and both renderers quantise onto a device-pixel grid that a
// whole logical px always lands on. Scale-invariance is a property of these
// fixtures, not of CSS: a fractional value (thirds of 100) moves between the two
// scales in Chrome exactly as it does in the engine. Getting there required
// choosing border widths that are whole device pixels at 1.5 — Chrome floors
// the USED border-width to whole device px and the engine does not. That
// divergence is not swept under the fixture; it is the subject of the last test
// in this file.

#include "IsolatedUIFixture.h"

#include "UI/UIElement.h"
#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using GameEngine::UI::PrimitiveMode;
using GameEngine::UI::UIPrimitive;
using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;

namespace
{

// Offset of `id` from `containerId`, in LOGICAL px — the space Yoga solves in
// and the space Chrome's getBoundingClientRect deltas are quoted in.
struct RelRect
{
    float X = 0.0f;
    float Y = 0.0f;
    float W = 0.0f;
    float H = 0.0f;
};

RelRect Rel(const IsolatedUIFixture& fx, const char* containerId, const char* id)
{
    const GameEngine::UIElement* c = fx.Element(containerId);
    const GameEngine::UIElement* e = fx.Element(id);
    if (!c || !e)
        return {-1.0f, -1.0f, -1.0f, -1.0f};
    return {e->GetLayoutX() - c->GetLayoutX(), e->GetLayoutY() - c->GetLayoutY(), e->GetLayoutWidth(),
            e->GetLayoutHeight()};
}

// Every fixture's root, byte-identical to the Chrome arm's `.root`.
constexpr char kRootCss[] = R"(
#root { display: flex; flex-direction: column; flex-wrap: nowrap; align-items: flex-start;
        align-content: flex-start; justify-content: flex-start;
        width: 500px; height: 400px; }
)";

// A skip-or-fail preamble every test repeats: no Vulkan device is a skip, a
// fixture that failed to parse is an authoring error and must fail loudly.
#define GE_BUILD_OR_BAIL(fx, scale, xml, css)                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(fx).Build((scale), (xml), (css)))                                                                        \
        {                                                                                                              \
            if (!(fx).DeviceAvailable())                                                                               \
                GTEST_SKIP() << (fx).Diagnostic();                                                                     \
            FAIL() << (fx).Diagnostic();                                                                               \
        }                                                                                                              \
    } while (false)

// Chrome reports positions to 4 decimals; every number in this file is exact in
// both arms, so the tolerance exists only to absorb float32 accumulation in the
// engine's logical->physical multiply, not to hide a real delta.
constexpr float kEps = 1.0e-3f;

void ExpectRel(const RelRect& got, float x, float y, float w, float h, const char* what)
{
    EXPECT_NEAR(got.X, x, kEps) << what << " relX";
    EXPECT_NEAR(got.Y, y, kEps) << what << " relY";
    EXPECT_NEAR(got.W, w, kEps) << what << " width";
    EXPECT_NEAR(got.H, h, kEps) << what << " height";
}

} // namespace

class FlexBoxModelInFlex : public ::testing::TestWithParam<float>
{
};

INSTANTIATE_TEST_SUITE_P(Scales, FlexBoxModelInFlex, ::testing::Values(1.0f, 1.5f));

// ---------------------------------------------------------------------------
// F1 - border + padding on the CONTAINER.
//
// Three items in a row inside a 300x120 border-box container with border 6 and
// padding 12. The content box is 264x84, so every item is inset 18 on both
// leading edges, stretched to 84 tall, and the flex-grow item receives the free
// space of the CONTENT box (264 - 40 - 60 = 164) rather than of the border box.
// That last number is the discriminator this fixture exists for: a container
// whose border never reached Yoga hands out 264 + 12 = 176 instead, and both
// existing border tests would still pass, because neither has a growing item.
//
// Chrome (identical at device scale 1 and 1.5), relative to #f1:
//   f1a  (18, 18)  40 x 84
//   f1b  (58, 18)  60 x 84
//   f1c  (118, 18) 164 x 84
// ---------------------------------------------------------------------------
namespace
{

constexpr char kF1Xml[] = R"(<uielement id="root">
  <uielement id="f1">
    <uielement id="f1a"/>
    <uielement id="f1b"/>
    <uielement id="f1c"/>
  </uielement>
</uielement>)";

constexpr char kF1Css[] = R"(
#f1  { flex-grow: 0; flex-shrink: 0; flex-basis: auto;
       display: flex; flex-direction: row; flex-wrap: nowrap; align-items: stretch;
       align-content: flex-start; justify-content: flex-start;
       width: 300px; height: 120px; padding: 12px; border: 6px solid #ff0000;
       background-color: #00ff00; }
#f1a { flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 40px; background-color: #0000ff; }
#f1b { flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 60px; background-color: #00ffff; }
#f1c { flex-grow: 1; flex-shrink: 1; flex-basis: 0px; background-color: #ffff00; }
)";

} // namespace

TEST_P(FlexBoxModelInFlex, ContainerBorderAndPaddingInsetEveryItem)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_BAIL(fx, GetParam(), kF1Xml, std::string(kRootCss) + kF1Css);

    ASSERT_NE(fx.Element("f1"), nullptr);
    // The container's own border box is untouched: Yoga's width IS the border
    // box, so the authored 300x120 must survive the inset.
    ExpectRel(Rel(fx, "f1", "f1"), 0.0f, 0.0f, 300.0f, 120.0f, "f1");

    ExpectRel(Rel(fx, "f1", "f1a"), 18.0f, 18.0f, 40.0f, 84.0f, "f1a");
    ExpectRel(Rel(fx, "f1", "f1b"), 58.0f, 18.0f, 60.0f, 84.0f, "f1b");
    // Free space comes out of the CONTENT box. 176 here means the trailing
    // border+padding never left the flex line's available space.
    ExpectRel(Rel(fx, "f1", "f1c"), 118.0f, 18.0f, 164.0f, 84.0f, "f1c");
}

// The paint side of F1, read off the emitted primitives rather than the layout
// rects: a correct solve emitted at the wrong origin, or a ring emitted with
// the wrong widths, passes every assertion above and still draws wrong.
// Physical px = logical * contentScale. Yoga quantises every solved edge onto
// a 1/64-device-px grid (YogaAdapter::SetContentScale), Chrome's own layout
// quantum; every number in this fixture is a whole logical px at both scales
// and so lands exactly on that grid, which is why the expected physical numbers
// are exact rather than merely close.
TEST_P(FlexBoxModelInFlex, ContainerInsetSurvivesIntoThePrimitives)
{
    const float cs = GetParam();

    IsolatedUIFixture fx;
    GE_BUILD_OR_BAIL(fx, cs, kF1Xml, std::string(kRootCss) + kF1Css);

    const auto containerRects = fx.Primitives("f1", PrimitiveMode::Rect);
    ASSERT_EQ(containerRects.size(), 1u);
    const UIPrimitive& cp = containerRects[0];
    EXPECT_NEAR(cp.W, 300.0f * cs, kEps);
    EXPECT_NEAR(cp.H, 120.0f * cs, kEps);
    // Ring widths L, T, R, B in physical px.
    for (int e = 0; e < 4; ++e)
        EXPECT_NEAR(cp.BorderWidths[e], 6.0f * cs, kEps) << "edge " << e;

    struct Expect
    {
        const char* Id;
        float X, Y, W, H;
    };
    // Chrome's logical numbers, scaled. The growing item is the one that moves
    // if the border leaves the free-space arithmetic.
    constexpr Expect kItems[] = {
        {"f1a", 18.0f, 18.0f, 40.0f, 84.0f},
        {"f1b", 58.0f, 18.0f, 60.0f, 84.0f},
        {"f1c", 118.0f, 18.0f, 164.0f, 84.0f},
    };
    for (const Expect& e : kItems)
    {
        const auto rects = fx.Primitives(e.Id, PrimitiveMode::Rect);
        ASSERT_EQ(rects.size(), 1u) << e.Id;
        const UIPrimitive& p = rects[0];
        EXPECT_NEAR(p.X - cp.X, e.X * cs, kEps) << e.Id << " X, cs=" << cs;
        EXPECT_NEAR(p.Y - cp.Y, e.Y * cs, kEps) << e.Id << " Y, cs=" << cs;
        EXPECT_NEAR(p.W, e.W * cs, kEps) << e.Id << " W, cs=" << cs;
        EXPECT_NEAR(p.H, e.H * cs, kEps) << e.Id << " H, cs=" << cs;

        // The item's fill must clear the container's painted ring on both sides.
        EXPECT_GE(p.X, cp.X + cp.BorderWidths[0]) << e.Id;
        EXPECT_LE(p.X + p.W, cp.X + cp.W - cp.BorderWidths[2]) << e.Id;
    }
}

// ---------------------------------------------------------------------------
// F2 - border + padding on the ITEMS.
//
// Under border-box the item's OUTER size is what it was authored: 120x90 holds,
// and the next item starts at 120 rather than at 120 + 2*15. What shrinks is
// the content box. #f2asym carries per-edge widths so a leading/trailing mix-up
// fails differently from a uniform one: left 16+8, top 4+2, right 8+4,
// bottom 12+6 => content 64 x 36 at (+24, +6).
//
// Chrome (identical at device scale 1 and 1.5), relative to #f2:
//   f2item  (0, 0)     120 x 90
//   f2inner (15, 15)    90 x 20
//   f2asym  (120, 0)   100 x 60
//   f2asymc (144, 6)    64 x 36
//   f2sib   (220, 0)    50 x 40
// ---------------------------------------------------------------------------
namespace
{

constexpr char kF2Xml[] = R"(<uielement id="root">
  <uielement id="f2">
    <uielement id="f2item"><uielement id="f2inner"/></uielement>
    <uielement id="f2asym"><uielement id="f2asymc"/></uielement>
    <uielement id="f2sib"/>
  </uielement>
</uielement>)";

constexpr char kF2Css[] = R"(
#f2      { flex-grow: 0; flex-shrink: 0; flex-basis: auto;
           display: flex; flex-direction: row; flex-wrap: nowrap; align-items: flex-start;
           align-content: flex-start; justify-content: flex-start;
           width: 400px; height: 150px; }
#f2item  { flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 120px; height: 90px;
           padding: 9px; border: 6px solid #008000;
           display: flex; flex-direction: column; flex-wrap: nowrap; align-items: stretch;
           align-content: flex-start; justify-content: flex-start; }
#f2inner { flex-grow: 0; flex-shrink: 0; flex-basis: auto; height: 20px; }
#f2asym  { flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 100px; height: 60px;
           padding: 2px 4px 6px 8px; border-width: 4px 8px 12px 16px;
           border-style: solid; border-color: #0000ff;
           display: flex; flex-direction: column; flex-wrap: nowrap; align-items: stretch;
           align-content: flex-start; justify-content: flex-start; }
#f2asymc { flex-grow: 1; flex-shrink: 1; flex-basis: 0px; }
#f2sib   { flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 50px; height: 40px; }
)";

} // namespace

TEST_P(FlexBoxModelInFlex, ItemBorderShrinksContentAndHoldsTheOuterSize)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_BAIL(fx, GetParam(), kF2Xml, std::string(kRootCss) + kF2Css);

    ASSERT_NE(fx.Element("f2"), nullptr);

    // Outer sizes hold, and the main-axis advance is the BORDER box: f2asym
    // starts at 120, f2sib at 220. A content-box model would put them at 150
    // and 274.
    ExpectRel(Rel(fx, "f2", "f2item"), 0.0f, 0.0f, 120.0f, 90.0f, "f2item");
    ExpectRel(Rel(fx, "f2", "f2asym"), 120.0f, 0.0f, 100.0f, 60.0f, "f2asym");
    ExpectRel(Rel(fx, "f2", "f2sib"), 220.0f, 0.0f, 50.0f, 40.0f, "f2sib");

    // Content shrinks by border+padding. f2inner is stretched across the
    // content box of a column container, so its width is the discriminator.
    ExpectRel(Rel(fx, "f2", "f2inner"), 15.0f, 15.0f, 90.0f, 20.0f, "f2inner");

    // Per-edge: leading (16, 4) via the Yoga inset, trailing (8, 12) via the
    // content size. Swapping left/right or top/bottom changes both numbers.
    ExpectRel(Rel(fx, "f2", "f2asymc"), 144.0f, 6.0f, 64.0f, 36.0f, "f2asymc");
}

// ---------------------------------------------------------------------------
// F3 - percentage children against a bordered container.
//
// CSS resolves a flex item's percentage width/height against the containing
// block's CONTENT box, not its border box or padding box. Container 300x200,
// border 10, padding 20 => content 240x140, so 50% width is 120 (not 150, the
// border box; not 140, the padding box) and 25% height is 35 (not 50, not 45).
// The three candidate boxes give three different numbers on both axes, which is
// what makes this fixture able to say WHICH box the engine used.
//
// #f3b additionally carries its own border+padding: under border-box its 25%
// still resolves to a 60px OUTER width, and its own child gets 60 - 2*10 = 40.
//
// Chrome (identical at device scale 1 and 1.5), relative to #f3:
//   f3a  (30, 30)  120 x 35
//   f3b  (150, 30)  60 x 70
//   f3bc (160, 40)  40 x 50
// ---------------------------------------------------------------------------
namespace
{

constexpr char kF3Xml[] = R"(<uielement id="root">
  <uielement id="f3">
    <uielement id="f3a"/>
    <uielement id="f3b"><uielement id="f3bc"/></uielement>
  </uielement>
</uielement>)";

constexpr char kF3Css[] = R"(
#f3   { flex-grow: 0; flex-shrink: 0; flex-basis: auto;
        display: flex; flex-direction: row; flex-wrap: nowrap; align-items: flex-start;
        align-content: flex-start; justify-content: flex-start;
        width: 300px; height: 200px; padding: 20px; border: 10px solid #0000ff; }
#f3a  { flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 50%; height: 25%; }
#f3b  { flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 25%; height: 50%;
        padding: 6px; border: 4px solid #800080;
        display: flex; flex-direction: column; flex-wrap: nowrap; align-items: stretch;
        align-content: flex-start; justify-content: flex-start; }
#f3bc { flex-grow: 1; flex-shrink: 1; flex-basis: 0px; }
)";

} // namespace

TEST_P(FlexBoxModelInFlex, PercentChildrenResolveAgainstTheContentBox)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_BAIL(fx, GetParam(), kF3Xml, std::string(kRootCss) + kF3Css);

    ASSERT_NE(fx.Element("f3"), nullptr);

    // 50% of the 240px content box. 150 => border box, 140 => padding box.
    // 25% of the 140px content height. 50 => border box, 45 => padding box.
    ExpectRel(Rel(fx, "f3", "f3a"), 30.0f, 30.0f, 120.0f, 35.0f, "f3a");

    // A percentage item that has its OWN border: the percentage sizes the
    // border box, so 25% of 240 is a 60px outer width whatever the child holds.
    ExpectRel(Rel(fx, "f3", "f3b"), 150.0f, 30.0f, 60.0f, 70.0f, "f3b");
    ExpectRel(Rel(fx, "f3", "f3bc"), 160.0f, 40.0f, 40.0f, 50.0f, "f3bc");
}

// ---------------------------------------------------------------------------
// F4 - absolutely positioned children inside a bordered flex container.
//
// CSS anchors an absolutely positioned box to its containing block's PADDING
// box, and resolves its percentage sizes and percentage offsets against that
// same padding box. Container 300x200 with border 10 and padding 20:
//   padding box = (10, 10) .. (290, 190), i.e. 280 x 180
// so `left: 0` lands at +10 (not 0, the border box; not 30, the content box),
// `right: 0` puts a 50-wide box at +240, `width: 50%` is 140 (not 150, not 120)
// and `left: 25%` is 10 + 70 = 80 (not 75, not 60+10).
//
// Every one of those three candidate boxes yields a different number for every
// assertion below, so a failure names the box the engine actually used.
//
// Chrome (identical at device scale 1 and 1.5), relative to #f4:
//   f4tl  (10, 10)   50 x 30
//   f4br  (240, 160) 50 x 30
//   f4pct (10, 110) 140 x 90
//   f4off (80, 10)   20 x 20
// ---------------------------------------------------------------------------
namespace
{

constexpr char kF4Xml[] = R"(<uielement id="root">
  <uielement id="f4">
    <uielement id="f4tl"/>
    <uielement id="f4br"/>
    <uielement id="f4pct"/>
    <uielement id="f4off"/>
  </uielement>
</uielement>)";

constexpr char kF4Css[] = R"(
#f4    { position: relative; flex-grow: 0; flex-shrink: 0; flex-basis: auto;
         display: flex; flex-direction: row; flex-wrap: nowrap;
         align-items: flex-start; align-content: flex-start; justify-content: flex-start;
         width: 300px; height: 200px; padding: 20px; border: 10px solid #ff8000; }
#f4tl  { position: absolute; left: 0px; top: 0px; width: 50px; height: 30px; }
#f4br  { position: absolute; right: 0px; bottom: 0px; width: 50px; height: 30px; }
#f4pct { position: absolute; left: 0px; top: 100px; width: 50%; height: 50%; }
#f4off { position: absolute; left: 25%; top: 0px; width: 20px; height: 20px; }
)";

} // namespace

TEST_P(FlexBoxModelInFlex, AbsoluteChildrenAnchorToThePaddingBox)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_BAIL(fx, GetParam(), kF4Xml, std::string(kRootCss) + kF4Css);

    ASSERT_NE(fx.Element("f4"), nullptr);

    // left/top 0 -> the padding-box origin, which is the border box inset by
    // border only. 0 would mean the border box, 30 the content box.
    ExpectRel(Rel(fx, "f4", "f4tl"), 10.0f, 10.0f, 50.0f, 30.0f, "f4tl");
    // right/bottom 0 -> the padding box's far edge (290, 190).
    ExpectRel(Rel(fx, "f4", "f4br"), 240.0f, 160.0f, 50.0f, 30.0f, "f4br");
    // 50% of the 280x180 padding box. 150x100 => border box, 120x70 => content.
    ExpectRel(Rel(fx, "f4", "f4pct"), 10.0f, 110.0f, 140.0f, 90.0f, "f4pct");
    // Percentage OFFSET: 25% of 280 = 70, from the padding-box origin at 10.
    ExpectRel(Rel(fx, "f4", "f4off"), 80.0f, 10.0f, 20.0f, 20.0f, "f4off");
}

// ---------------------------------------------------------------------------
// F5 - nested bordered flex containers.
//
// Insets compose: the middle container is stretched across the outer content
// box (320 - 2*4 - 2*8 = 296) and sits at +12; the leaf is inset a further
// 2 + 6 and sized from the middle's content box. A border counted once at the
// wrong level shows up as a 2px error at the leaf and nowhere else, which is
// exactly the failure a single-level fixture cannot see.
//
// Chrome (identical at device scale 1 and 1.5), relative to #f5:
//   f5mid  (12, 12) 296 x 100
//   f5leaf (20, 20) 280 x 84
// ---------------------------------------------------------------------------
namespace
{

constexpr char kF5Xml[] = R"(<uielement id="root">
  <uielement id="f5">
    <uielement id="f5mid"><uielement id="f5leaf"/></uielement>
  </uielement>
</uielement>)";

constexpr char kF5Css[] = R"(
#f5     { flex-grow: 0; flex-shrink: 0; flex-basis: auto;
          display: flex; flex-direction: column; flex-wrap: nowrap; align-items: stretch;
          align-content: flex-start; justify-content: flex-start;
          width: 320px; height: 220px; padding: 8px; border: 4px solid #ff0000; }
#f5mid  { flex-grow: 0; flex-shrink: 0; flex-basis: auto; height: 100px;
          padding: 6px; border: 2px solid #00ff00;
          display: flex; flex-direction: row; flex-wrap: nowrap; align-items: stretch;
          align-content: flex-start; justify-content: flex-start; }
#f5leaf { flex-grow: 1; flex-shrink: 1; flex-basis: 0px; }
)";

} // namespace

TEST_P(FlexBoxModelInFlex, NestedBorderedContainersComposeTheirInsets)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_BAIL(fx, GetParam(), kF5Xml, std::string(kRootCss) + kF5Css);

    ASSERT_NE(fx.Element("f5"), nullptr);

    // align-items: stretch sizes the middle container to the OUTER content box.
    ExpectRel(Rel(fx, "f5", "f5mid"), 12.0f, 12.0f, 296.0f, 100.0f, "f5mid");
    // And the leaf to the MIDDLE's content box, from an origin inset twice.
    ExpectRel(Rel(fx, "f5", "f5leaf"), 20.0f, 20.0f, 280.0f, 84.0f, "f5leaf");
}

// ---------------------------------------------------------------------------
// F6 - asymmetric border AND asymmetric padding on the container.
//
// The discriminating shape for edge confusion. border 2/4/6/8 (T/R/B/L) with
// padding 5/9/13/17 gives four distinct insets: left 8+17=25, top 2+5=7,
// right 4+9=13, bottom 6+13=19, so the grown item is 262 x 124 at (+25, +7).
// Every plausible edge permutation - swapping L/R, transposing the box, reading
// the padding shorthand in the wrong order - lands on a different number.
//
// Chrome (identical at device scale 1 and 1.5), relative to #f6:
//   f6a (25, 7) 262 x 124
// ---------------------------------------------------------------------------
namespace
{

constexpr char kF6Xml[] = R"(<uielement id="root">
  <uielement id="f6"><uielement id="f6a"/></uielement>
</uielement>)";

constexpr char kF6Css[] = R"(
#f6  { flex-grow: 0; flex-shrink: 0; flex-basis: auto;
       display: flex; flex-direction: row; flex-wrap: nowrap; align-items: stretch;
       align-content: flex-start; justify-content: flex-start;
       width: 300px; height: 150px; padding: 5px 9px 13px 17px;
       border-width: 2px 4px 6px 8px; border-style: solid; border-color: #ff0000;
       background-color: #00ff00; }
#f6a { flex-grow: 1; flex-shrink: 1; flex-basis: 0px; background-color: #0000ff; }
)";

} // namespace

TEST_P(FlexBoxModelInFlex, AsymmetricContainerInsetsMatchChromePerEdge)
{
    const float cs = GetParam();

    IsolatedUIFixture fx;
    GE_BUILD_OR_BAIL(fx, cs, kF6Xml, std::string(kRootCss) + kF6Css);

    ASSERT_NE(fx.Element("f6"), nullptr);
    ExpectRel(Rel(fx, "f6", "f6a"), 25.0f, 7.0f, 262.0f, 124.0f, "f6a");

    // The painted ring must carry the same four widths the solve used - the two
    // are computed by different code (AddBorderLTRB vs the Yoga inset) and only
    // agree because both read the resolved per-edge widths.
    const auto rects = fx.Primitives("f6", PrimitiveMode::Rect);
    ASSERT_EQ(rects.size(), 1u);
    const UIPrimitive& p = rects[0];
    EXPECT_NEAR(p.BorderWidths[0], 8.0f * cs, kEps) << "left";
    EXPECT_NEAR(p.BorderWidths[1], 2.0f * cs, kEps) << "top";
    EXPECT_NEAR(p.BorderWidths[2], 4.0f * cs, kEps) << "right";
    EXPECT_NEAR(p.BorderWidths[3], 6.0f * cs, kEps) << "bottom";
}

// ---------------------------------------------------------------------------
// F7 - a DIVERGENCE, pinned rather than hidden.
//
// Chrome floors the USED border-width to whole DEVICE pixels. At device scale
// 1.5 a declared `border: 7px` computes to 6.66667px (10 device px), so the
// content box grows by 2/3px on each axis and every child shifts. Measured, not
// inferred: getComputedStyle().borderLeftWidth reports "7px" at scale 1 and
// "6.66667px" at scale 1.5, and only border does this - padding 12px stays 12px
// at both scales, as does every even border width (already whole device px).
//
//   chrome --headless=new --force-device-scale-factor=1     f7a (19, 19)             262 x 82
//   chrome --headless=new --force-device-scale-factor=1.5   f7a (18.6667, 18.6667)   262.6667 x 82.6667
//                                                    device  (28, 28)                394 x 124
//
// The engine keeps the declared 7 logical px as the USED width, so its content
// box stays 262 logical and the child lands at 19 * 1.5 = 28.5 device px where
// Chrome paints 28.
//
// This is a STYLE-level divergence, not a layout-rounding one, and the
// distinction is load-bearing: Yoga quantises solved edges onto a 1/64-device-px
// grid (YogaAdapter::SetContentScale), 28.5 is exactly representable on it, and
// so the grid neither causes this gap nor closes it. Closing it means flooring
// the used border-width to whole device px in resolved style, before any number
// reaches Yoga - a fix lane's call, not this file's.
//
// This test asserts the ENGINE's arithmetic, so it goes red the moment that
// floor lands, and the comment carries what Chrome does meanwhile.
// ---------------------------------------------------------------------------
namespace
{

constexpr char kF7Xml[] = R"(<uielement id="root">
  <uielement id="f7"><uielement id="f7a"/></uielement>
</uielement>)";

constexpr char kF7Css[] = R"(
#f7  { flex-grow: 0; flex-shrink: 0; flex-basis: auto;
       display: flex; flex-direction: row; flex-wrap: nowrap; align-items: stretch;
       align-content: flex-start; justify-content: flex-start;
       width: 300px; height: 120px; padding: 12px; border: 7px solid #ff0000; }
#f7a { flex-grow: 1; flex-shrink: 1; flex-basis: 0px; }
)";

} // namespace

TEST_P(FlexBoxModelInFlex, OddBorderWidthIsNotDeviceFlooredUnlikeChrome)
{
    const float cs = GetParam();

    IsolatedUIFixture fx;
    GE_BUILD_OR_BAIL(fx, cs, kF7Xml, std::string(kRootCss) + kF7Css);

    ASSERT_NE(fx.Element("f7"), nullptr);

    // Logical px: the engine's used border-width is the declared 7 at every
    // content scale, so the logical layout is scale-invariant. Chrome agrees at
    // scale 1 (19, 262 x 82) and diverges at 1.5 (18.6667, 262.6667 x 82.6667).
    ExpectRel(Rel(fx, "f7", "f7a"), 19.0f, 19.0f, 262.0f, 82.0f, "f7a");

    // Physical px: an exact logical * scale. The 1/64-device-px grid represents
    // 28.5 exactly, so it survives to paint - at cs 1.5 that is 28.5 where
    // Chrome, having floored the border first, paints 28.
    const PhysicalRect f7 = fx.BorderBox("f7");
    const PhysicalRect f7a = fx.BorderBox("f7a");
    EXPECT_NEAR(f7a.X - f7.X, 19.0f * cs, kEps) << "cs=" << cs;
    EXPECT_NEAR(f7a.W, 262.0f * cs, kEps) << "cs=" << cs;
}
