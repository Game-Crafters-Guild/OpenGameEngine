// Flexbox flexibility resolution against Chrome: flex-grow ratios, flex-shrink
// under overflow, flex-basis px/auto/0, min/max clamping inside the resolution
// loop, and auto margins absorbing leftover free space.
//
// Both arms lay out the SAME CSS text. Each fixture below was authored once and
// emitted into an HTML file and into the string constants here, so a delta can
// never be an authoring difference between the arms.
//
// Chrome arm, measured with real Chrome 150.0.7871.186 and a real device scale
// factor -- not an emulated devicePixelRatio, which cannot produce the 1/64
// DEVICE-pixel quantisation these numbers carry (66.671875 CSS px at dpr 1
// becomes 66.6640625 at dpr 2; both are k/64 device px):
//
//   chrome.exe --headless=new --disable-gpu --force-device-scale-factor=1 \
//              --window-size=800,600 --virtual-time-budget=2000 \
//              --dump-dom file:///<fixture>.html
//   chrome.exe ... --force-device-scale-factor=2 --window-size=1600,1200 ...
//
// with a page script reading getBoundingClientRect() off every element. The CSS
// viewport is 800x600 in both runs, matching IsolatedUIFixture's logical
// viewport. `box-sizing: border-box` appears in the CSS because Chrome's
// content-box default is not this engine's box model; the engine has no
// box-sizing property and ignores the declaration, which is the point.
//
// Chrome is measured at BOTH device scale factors and both results are stored,
// because they are not the same layout scaled: Chrome quantises to 1/64 device
// px, so at dpr 2 its grid is twice as fine and the dpr-1 number times two is
// off by up to 1/64 px. Deriving the dpr-2 expectation instead of measuring it
// would inject that error into every fractional row.
//
// Every expectation is anchored to a Chrome number. Where the engine cannot
// reproduce it the row carries an explicit delta, so the size of each
// divergence is stated in the test rather than hidden by a looser tolerance.
// Exactly one delta family survives:
//
//   Grid tie-breaks. Yoga rounds onto Chrome's quantum, 1/64 of a DEVICE px
//   (YogaAdapter::SetContentScale), so a fractional distribution survives the
//   solve instead of collapsing to a whole pixel: every grow, shrink and basis
//   row below is Chrome-exact at both scales but one. Both engines round
//   cumulative EDGES and take a width as the difference of two of them, so a
//   width carries its neighbour's rounding, not its own. What is left is where
//   the two break a tie: BasisPxAutoZero at content scale 2 puts one edge a
//   single grid step -- 0.015625 device px -- above Chrome's. Neither side is a
//   fixed rounding mode; Chrome's own two arms take the same .667-of-a-step
//   edge UP at dpr 1 (20906.667 -> 20907) and DOWN at dpr 2 (22186.667 ->
//   22186), while Yoga rounds half-up in both.
//
//   min/max clamping used to be a second family and no longer is. Yoga's
//   stock resolver clamped the flex base size first and then flexed on top of
//   the clamp, which grew a min-clamped item past its minimum and let a
//   max-clamped item under shrink overflow its container. The overlay port
//   cmake/ports/yoga replaces the two-pass resolver with the CSS Flexbox
//   section 9.7 freeze loop, so MinWidthClamp*, MaxWidthClampUnderShrink*,
//   MinHeightClampColumn, MinWidthClampCascade and
//   OppositeClampViolationsUnderShrink are Chrome-exact at both scales -- the
//   last two including the case that needs three distributions to settle and
//   the case where violations of both signs land in one.
//
//   Auto margins used to be a third family and no longer are: LayoutInputs
//   carries the Auto unit per edge and YogaAdapter::ApplyStyle pushes
//   YGNodeStyleSetMarginAuto, so every auto-margin fixture below is now
//   Chrome-exact at both scales. MarginAutoTests.cpp owns the wider battery,
//   including the one place the unit still does not reach the layout —
//   absolutely positioned boxes, where Yoga's absolute pass ignores it.

#include "IsolatedUIFixture.h"

#include "UI/UIElement.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;

namespace
{

// A getBoundingClientRect, relative to #fc, in DEVICE px: CSS px at dpr 1, and
// CSS px times two at dpr 2. Device px is what the engine's physical space is.
struct Rect
{
    float X = 0.0f;
    float Y = 0.0f;
    float W = 0.0f;
    float H = 0.0f;
};

// What the engine adds to the Chrome number beside it. Zero is a parity claim.
using Delta = Rect;

struct Row
{
    const char* Id;
    Rect Chrome1;  // real Chrome, --force-device-scale-factor=1
    Delta Engine1; // engine at content scale 1.0, minus Chrome1
    Rect Chrome2;  // real Chrome, --force-device-scale-factor=2
    Delta Engine2; // engine at content scale 2.0, minus Chrome2
};

constexpr char kXmlGrowRatio123[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
    <uielement id="b"/>
    <uielement id="c"/>
  </uielement>
</uielement>)";

constexpr char kCssGrowRatio123[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: row; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 400px; height: 60px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#b { box-sizing: border-box; flex-grow: 2; flex-shrink: 1; flex-basis: 0px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#c { box-sizing: border-box; flex-grow: 3; flex-shrink: 1; flex-basis: 0px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
)";

constexpr char kXmlFractionalGrowSumBelowOne[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
    <uielement id="b"/>
  </uielement>
</uielement>)";

constexpr char kCssFractionalGrowSumBelowOne[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: row; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 400px; height: 60px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 0.5; flex-shrink: 1; flex-basis: 50px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#b { box-sizing: border-box; flex-grow: 0.25; flex-shrink: 1; flex-basis: 50px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
)";

constexpr char kXmlGrowRatioFractionalSumAboveOne[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
    <uielement id="b"/>
    <uielement id="c"/>
  </uielement>
</uielement>)";

constexpr char kCssGrowRatioFractionalSumAboveOne[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: row; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 400px; height: 60px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 0.5; flex-shrink: 1; flex-basis: 0px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#b { box-sizing: border-box; flex-grow: 1.5; flex-shrink: 1; flex-basis: 0px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#c { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
)";

constexpr char kXmlShrinkWithFrozenItem[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
    <uielement id="b"/>
    <uielement id="c"/>
  </uielement>
</uielement>)";

constexpr char kCssShrinkWithFrozenItem[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: row; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 300px; height: 60px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 0; flex-shrink: 1; flex-basis: 200px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#b { box-sizing: border-box; flex-grow: 0; flex-shrink: 1; flex-basis: 200px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#c { box-sizing: border-box; flex-grow: 0; flex-shrink: 0; flex-basis: 200px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
)";

constexpr char kXmlShrinkFractionalRatio[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
    <uielement id="b"/>
    <uielement id="c"/>
  </uielement>
</uielement>)";

constexpr char kCssShrinkFractionalRatio[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: row; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 250px; height: 60px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 0; flex-shrink: 1; flex-basis: 100px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#b { box-sizing: border-box; flex-grow: 0; flex-shrink: 2; flex-basis: 100px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#c { box-sizing: border-box; flex-grow: 0; flex-shrink: 3; flex-basis: 100px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
)";

constexpr char kXmlBasisPxAutoZero[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
    <uielement id="b"/>
    <uielement id="c"/>
  </uielement>
</uielement>)";

constexpr char kCssBasisPxAutoZero[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: row; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 400px; height: 60px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 100px; width: 30px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#b { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: auto; width: 80px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#c { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; width: 60px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
)";

constexpr char kXmlMinWidthClampMidResolution[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
    <uielement id="b"/>
    <uielement id="c"/>
  </uielement>
</uielement>)";

constexpr char kCssMinWidthClampMidResolution[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: row; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 400px; height: 60px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; height: 20px; min-width: 200px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#b { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#c { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
)";

constexpr char kXmlMaxWidthClampMidResolution[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
    <uielement id="b"/>
    <uielement id="c"/>
  </uielement>
</uielement>)";

constexpr char kCssMaxWidthClampMidResolution[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: row; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 400px; height: 60px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; height: 20px; max-width: 50px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#b { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#c { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
)";

constexpr char kXmlMaxWidthClampUnderShrink[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
    <uielement id="b"/>
    <uielement id="c"/>
  </uielement>
</uielement>)";

constexpr char kCssMaxWidthClampUnderShrink[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: row; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 300px; height: 60px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 0; flex-shrink: 1; flex-basis: 200px; height: 20px; max-width: 90px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#b { box-sizing: border-box; flex-grow: 0; flex-shrink: 1; flex-basis: 200px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#c { box-sizing: border-box; flex-grow: 0; flex-shrink: 1; flex-basis: 200px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
)";

constexpr char kXmlMinHeightClampColumn[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
    <uielement id="b"/>
    <uielement id="c"/>
  </uielement>
</uielement>)";

constexpr char kCssMinHeightClampColumn[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 200px; height: 300px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; width: 40px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#b { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; width: 40px; min-height: 200px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#c { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; width: 40px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
)";

constexpr char kXmlMaxHeightClampColumn[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
    <uielement id="b"/>
    <uielement id="c"/>
  </uielement>
</uielement>)";

constexpr char kCssMaxHeightClampColumn[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 200px; height: 300px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; width: 40px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#b { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; width: 40px; max-height: 40px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#c { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; width: 40px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
)";

constexpr char kXmlMinWidthClampScaled[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
    <uielement id="b"/>
    <uielement id="c"/>
  </uielement>
</uielement>)";

constexpr char kCssMinWidthClampScaled[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: row; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 3200px; height: 60px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; height: 20px; min-width: 1600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#b { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#c { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
)";

constexpr char kXmlMaxWidthClampUnderShrinkScaled[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
    <uielement id="b"/>
    <uielement id="c"/>
  </uielement>
</uielement>)";

constexpr char kCssMaxWidthClampUnderShrinkScaled[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: row; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 2400px; height: 60px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 0; flex-shrink: 1; flex-basis: 1600px; height: 20px; max-width: 720px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#b { box-sizing: border-box; flex-grow: 0; flex-shrink: 1; flex-basis: 1600px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#c { box-sizing: border-box; flex-grow: 0; flex-shrink: 1; flex-basis: 1600px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
)";

constexpr char kXmlMinWidthClampCascade[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
    <uielement id="b"/>
    <uielement id="c"/>
    <uielement id="d"/>
  </uielement>
</uielement>)";

constexpr char kCssMinWidthClampCascade[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: row; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 600px; height: 60px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; height: 20px; min-width: 300px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#b { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; height: 20px; min-width: 150px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#c { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#d { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
)";

constexpr char kXmlOppositeClampViolationsUnderShrink[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
    <uielement id="b"/>
    <uielement id="c"/>
    <uielement id="d"/>
  </uielement>
</uielement>)";

constexpr char kCssOppositeClampViolationsUnderShrink[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: row; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 300px; height: 60px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 0; flex-shrink: 1; flex-basis: 150px; height: 20px; max-width: 30px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#b { box-sizing: border-box; flex-grow: 0; flex-shrink: 1; flex-basis: 150px; height: 20px; min-width: 85px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#c { box-sizing: border-box; flex-grow: 0; flex-shrink: 1; flex-basis: 150px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#d { box-sizing: border-box; flex-grow: 0; flex-shrink: 1; flex-basis: 150px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
)";

constexpr char kXmlAutoMarginLeftOnly[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
  </uielement>
</uielement>)";

constexpr char kCssAutoMarginLeftOnly[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: row; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 400px; height: 60px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 100px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: auto; padding: 0px; border-width: 0px; }
)";

constexpr char kXmlAutoMarginBothSides[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
  </uielement>
</uielement>)";

constexpr char kCssAutoMarginBothSides[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: row; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 400px; height: 60px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 100px; height: 20px; margin-top: 0px; margin-right: auto; margin-bottom: 0px; margin-left: auto; padding: 0px; border-width: 0px; }
)";

constexpr char kXmlAutoMarginCrossAxis[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
  </uielement>
</uielement>)";

constexpr char kCssAutoMarginCrossAxis[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: row; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 400px; height: 60px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 100px; height: 20px; margin-top: auto; margin-right: 0px; margin-bottom: auto; margin-left: 0px; padding: 0px; border-width: 0px; }
)";

constexpr char kXmlAutoMarginLosesToGrow[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
  </uielement>
</uielement>)";

constexpr char kCssAutoMarginLosesToGrow[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: row; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 400px; height: 60px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 1; flex-shrink: 1; flex-basis: 0px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: auto; padding: 0px; border-width: 0px; }
)";

constexpr char kXmlAutoMarginSplitAcrossItems[] = R"(<uielement id="root">
  <uielement id="fc">
    <uielement id="a"/>
    <uielement id="b"/>
  </uielement>
</uielement>)";

constexpr char kCssAutoMarginSplitAcrossItems[] = R"(
#root { box-sizing: border-box; display: flex; flex-direction: column; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 800px; height: 600px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#fc { box-sizing: border-box; display: flex; flex-direction: row; flex-wrap: nowrap; justify-content: flex-start; align-items: flex-start; align-content: flex-start; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 400px; height: 60px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: 0px; padding: 0px; border-width: 0px; }
#a { box-sizing: border-box; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 100px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: auto; padding: 0px; border-width: 0px; }
#b { box-sizing: border-box; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 100px; height: 20px; margin-top: 0px; margin-right: 0px; margin-bottom: 0px; margin-left: auto; padding: 0px; border-width: 0px; }
)";

// GrowRatio123 -- flex-grow 1:2:3 over 400px of free space -> 66.667 / 133.333 / 200.
const std::vector<Row> kRowsGrowRatio123 = {
    {"a",
     {0.0, 0.0, 66.671875f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 0.0, 133.328125f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"b",
     {66.671875f, 0.0, 133.328125f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {133.328125f, 0.0, 266.671875f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"c",
     {200.0f, 0.0, 200.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {400.0f, 0.0, 400.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
};

// FractionalGrowSumBelowOne -- grow 0.5 + 0.25 sums to 0.75 < 1: CSS distributes only 75% of free space.
const std::vector<Row> kRowsFractionalGrowSumBelowOne = {
    {"a",
     {0.0, 0.0, 200.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 0.0, 400.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"b",
     {200.0f, 0.0, 125.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {400.0f, 0.0, 250.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
};

// GrowRatioFractionalSumAboveOne -- grow 0.5 : 1.5 : 1 sums to 3 > 1: full free space in 1:3:2 ratio.
const std::vector<Row> kRowsGrowRatioFractionalSumAboveOne = {
    {"a",
     {0.0, 0.0, 66.671875f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 0.0, 133.328125f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"b",
     {66.671875f, 0.0, 200.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {133.328125f, 0.0, 400.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"c",
     {266.671875f, 0.0, 133.328125f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {533.328125f, 0.0, 266.671875f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
};

// ShrinkWithFrozenItem -- basis 200*3 into 300px; third item flex-shrink 0 keeps its 200.
const std::vector<Row> kRowsShrinkWithFrozenItem = {
    {"a",
     {0.0, 0.0, 50.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 0.0, 100.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"b",
     {50.0f, 0.0, 50.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {100.0f, 0.0, 100.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"c",
     {100.0f, 0.0, 200.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {200.0f, 0.0, 400.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
};

// ShrinkFractionalRatio -- basis 100*3 into 250px, shrink 1:2:3 weighted by basis -> 91.667 / 83.333 / 75.
const std::vector<Row> kRowsShrinkFractionalRatio = {
    {"a",
     {0.0, 0.0, 91.671875f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 0.0, 183.328125f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"b",
     {91.671875f, 0.0, 83.328125f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {183.328125f, 0.0, 166.671875f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"c",
     {175.0f, 0.0, 75.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {350.0f, 0.0, 150.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
};

// BasisPxAutoZero -- flex-basis px beats width; auto falls back to width; 0 contributes nothing.
const std::vector<Row> kRowsBasisPxAutoZero = {
    {"a",
     {0.0, 0.0, 173.328125f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 0.0, 346.65625f, 40.0f}, {0.0, 0.0, 0.015625f, 0.0}},
    {"b",
     {173.328125f, 0.0, 153.34375f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {346.65625f, 0.0, 306.671875f, 40.0f}, {0.015625f, 0.0, -0.015625f, 0.0}},
    {"c",
     {326.671875f, 0.0, 73.328125f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {653.328125f, 0.0, 146.671875f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
};

// MinWidthClampMidResolution -- even 1:1:1 grow would give 133.333 each; min-width freezes a at 200 and redistributes.
const std::vector<Row> kRowsMinWidthClampMidResolution = {
    {"a",
     {0.0, 0.0, 200.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 0.0, 400.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"b",
     {200.0f, 0.0, 100.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {400.0f, 0.0, 200.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"c",
     {300.0f, 0.0, 100.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {600.0f, 0.0, 200.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
};

// MaxWidthClampMidResolution -- max-width freezes a at 50 and the other two absorb the surplus.
const std::vector<Row> kRowsMaxWidthClampMidResolution = {
    {"a",
     {0.0, 0.0, 50.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 0.0, 100.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"b",
     {50.0f, 0.0, 175.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {100.0f, 0.0, 350.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"c",
     {225.0f, 0.0, 175.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {450.0f, 0.0, 350.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
};

// MaxWidthClampUnderShrink -- shrink pass with a max-width violation on an item that also wants to shrink.
const std::vector<Row> kRowsMaxWidthClampUnderShrink = {
    {"a",
     {0.0, 0.0, 90.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 0.0, 180.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"b",
     {90.0f, 0.0, 105.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {180.0f, 0.0, 210.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"c",
     {195.0f, 0.0, 105.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {390.0f, 0.0, 210.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
};

// MinHeightClampColumn -- same clamping loop on the block axis: min-height freezes b at 200.
const std::vector<Row> kRowsMinHeightClampColumn = {
    {"a",
     {0.0, 0.0, 40.0f, 50.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 0.0, 80.0f, 100.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"b",
     {0.0, 50.0f, 40.0f, 200.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 100.0f, 80.0f, 400.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"c",
     {0.0, 250.0f, 40.0f, 50.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 500.0f, 80.0f, 100.0f}, {0.0, 0.0, 0.0, 0.0}},
};

// MaxHeightClampColumn -- max-height freezes b at 40 on the block axis.
const std::vector<Row> kRowsMaxHeightClampColumn = {
    {"a",
     {0.0, 0.0, 40.0f, 130.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 0.0, 80.0f, 260.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"b",
     {0.0, 130.0f, 40.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 260.0f, 80.0f, 80.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"c",
     {0.0, 170.0f, 40.0f, 130.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 340.0f, 80.0f, 260.0f}, {0.0, 0.0, 0.0, 0.0}},
};

// MinWidthClampScaled -- MinWidthClampMidResolution x8, so grid rounding cannot account for any delta.
const std::vector<Row> kRowsMinWidthClampScaled = {
    {"a",
     {0.0, 0.0, 1600.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 0.0, 3200.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"b",
     {1600.0f, 0.0, 800.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {3200.0f, 0.0, 1600.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"c",
     {2400.0f, 0.0, 800.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {4800.0f, 0.0, 1600.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
};

// MaxWidthClampUnderShrinkScaled -- MaxWidthClampUnderShrink x8, same purpose.
const std::vector<Row> kRowsMaxWidthClampUnderShrinkScaled = {
    {"a",
     {0.0, 0.0, 720.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 0.0, 1440.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"b",
     {720.0f, 0.0, 840.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {1440.0f, 0.0, 1680.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"c",
     {1560.0f, 0.0, 840.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {3120.0f, 0.0, 1680.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
};

// MinWidthClampCascade -- two min violations that cannot both be found in one pass: 300 / 150 / 75 / 75.
const std::vector<Row> kRowsMinWidthClampCascade = {
    {"a",
     {0.0, 0.0, 300.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 0.0, 600.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"b",
     {300.0f, 0.0, 150.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {600.0f, 0.0, 300.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"c",
     {450.0f, 0.0, 75.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {900.0f, 0.0, 150.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"d",
     {525.0f, 0.0, 75.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {1050.0f, 0.0, 150.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
};

// OppositeClampViolationsUnderShrink -- a max and a min violation in the same distribution: 30 / 90 / 90 / 90.
const std::vector<Row> kRowsOppositeClampViolationsUnderShrink = {
    {"a",
     {0.0, 0.0, 30.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 0.0, 60.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"b",
     {30.0f, 0.0, 90.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {60.0f, 0.0, 180.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"c",
     {120.0f, 0.0, 90.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {240.0f, 0.0, 180.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"d",
     {210.0f, 0.0, 90.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {420.0f, 0.0, 180.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
};

// AutoMarginLeftOnly -- one auto main-axis margin absorbs all 300px of free space.
const std::vector<Row> kRowsAutoMarginLeftOnly = {
    {"a",
     {300.0f, 0.0, 100.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {600.0f, 0.0, 200.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
};

// AutoMarginBothSides -- two auto main-axis margins split the free space -> item centred at 150.
const std::vector<Row> kRowsAutoMarginBothSides = {
    {"a",
     {150.0f, 0.0, 100.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {300.0f, 0.0, 200.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
};

// AutoMarginCrossAxis -- auto margins on the cross axis centre the item vertically in a row.
const std::vector<Row> kRowsAutoMarginCrossAxis = {
    {"a",
     {0.0, 20.0f, 100.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 40.0f, 200.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
};

// AutoMarginLosesToGrow -- grow consumes the free space first, so the auto margin resolves to 0.
const std::vector<Row> kRowsAutoMarginLosesToGrow = {
    {"a",
     {0.0, 0.0, 400.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {0.0, 0.0, 800.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
};

// AutoMarginSplitAcrossItems -- free space is split equally across every auto margin on the line, not per item.
const std::vector<Row> kRowsAutoMarginSplitAcrossItems = {
    {"a",
     {100.0f, 0.0, 100.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {200.0f, 0.0, 200.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
    {"b",
     {300.0f, 0.0, 100.0f, 20.0f}, {0.0, 0.0, 0.0, 0.0},
     {600.0f, 0.0, 200.0f, 40.0f}, {0.0, 0.0, 0.0, 0.0}},
};


struct Fixture
{
    const char* Name;
    const char* Xml;
    const char* Css;
    float FcWidth;  // CSS px, as authored on #fc
    float FcHeight; // CSS px
    const std::vector<Row>* Rows;
};

// Everything is compared in PHYSICAL px, the space BorderBox returns: at
// content scale 1.0 that is the same number as Chrome's CSS px, and at 2.0 it
// is twice it. One code path, two scales.
void CheckAtScale(const Fixture& fixture, float scale)
{
    SCOPED_TRACE(std::string(fixture.Name) + " @ contentScale " + std::to_string(scale));

    IsolatedUIFixture fx;
    if (!fx.Build(scale, fixture.Xml, fixture.Css))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    // Every row below is relative to #fc, so #fc being where and what the CSS
    // asked for is a precondition, not a result: a wrong container turns every
    // item delta into a measurement against the wrong origin.
    ASSERT_NE(fx.Element("fc"), nullptr);
    const PhysicalRect fc = fx.BorderBox("fc");
    ASSERT_FLOAT_EQ(fc.X, 0.0f);
    ASSERT_FLOAT_EQ(fc.Y, 0.0f);
    ASSERT_FLOAT_EQ(fc.W, fixture.FcWidth * scale);
    ASSERT_FLOAT_EQ(fc.H, fixture.FcHeight * scale);

    for (const Row& row : *fixture.Rows)
    {
        SCOPED_TRACE(std::string("#") + row.Id);
        ASSERT_NE(fx.Element(row.Id), nullptr);
        const PhysicalRect box = fx.BorderBox(row.Id);
        const bool atOne = (scale == 1.0f);
        const Rect& c = atOne ? row.Chrome1 : row.Chrome2;
        const Delta& d = atOne ? row.Engine1 : row.Engine2;

        EXPECT_FLOAT_EQ(box.X - fc.X, c.X + d.X);
        EXPECT_FLOAT_EQ(box.Y - fc.Y, c.Y + d.Y);
        EXPECT_FLOAT_EQ(box.W, c.W + d.W);
        EXPECT_FLOAT_EQ(box.H, c.H + d.H);
    }
}

void CheckBothScales(const Fixture& fixture)
{
    CheckAtScale(fixture, 1.0f);
    CheckAtScale(fixture, 2.0f);
}

} // namespace

// Free space 400 split 1:2:3. The exact shares are 66.667 / 133.333 / 200, and
// both engines land them on the same 1/64-device-px edges: 66.671875 /
// 133.328125 / 200. Exact parity at both scales.
TEST(FlexGrowShrinkBasisChromeParity, GrowRatio123)
{
    CheckBothScales({"GrowRatio123", kXmlGrowRatio123, kCssGrowRatio123, 400.0f, 60.0f, &kRowsGrowRatio123});
}

// Grow factors summing below 1 distribute only that fraction of the free
// space: 0.5 and 0.25 of 300 give 200 and 125, and 75px stays unused. Yoga
// implements the same rule, so this is exact parity at both scales.
TEST(FlexGrowShrinkBasisChromeParity, FractionalGrowSumBelowOne)
{
    CheckBothScales({"FractionalGrowSumBelowOne", kXmlFractionalGrowSumBelowOne, kCssFractionalGrowSumBelowOne, 400.0f, 60.0f, &kRowsFractionalGrowSumBelowOne});
}

// Fractional factors that sum above 1 distribute all of it, in ratio. Exact
// parity, on the same edges as GrowRatio123.
TEST(FlexGrowShrinkBasisChromeParity, GrowRatioFractionalSumAboveOne)
{
    CheckBothScales({"GrowRatioFractionalSumAboveOne", kXmlGrowRatioFractionalSumAboveOne, kCssGrowRatioFractionalSumAboveOne, 400.0f, 60.0f, &kRowsGrowRatioFractionalSumAboveOne});
}

// Shrink is weighted by flex-basis, and flex-shrink: 0 keeps an item at its
// basis while the others absorb the whole overflow. Exact parity.
TEST(FlexGrowShrinkBasisChromeParity, ShrinkWithFrozenItem)
{
    CheckBothScales({"ShrinkWithFrozenItem", kXmlShrinkWithFrozenItem, kCssShrinkWithFrozenItem, 300.0f, 60.0f, &kRowsShrinkWithFrozenItem});
}

// Shrink factors 1:2:3 over equal bases: the overflow splits 1:2:3, not
// evenly. Exact parity -- shrink resolves on the same grid as grow.
TEST(FlexGrowShrinkBasisChromeParity, ShrinkFractionalRatio)
{
    CheckBothScales({"ShrinkFractionalRatio", kXmlShrinkFractionalRatio, kCssShrinkFractionalRatio, 250.0f, 60.0f, &kRowsShrinkFractionalRatio});
}

// flex-basis in px overrides width, auto falls back to width, and 0 makes the
// item contribute nothing to the consumed space. Exact parity at content scale
// 1, and the one place a grid tie-break survives: at scale 2 the first edge
// falls at 22186.667 grid steps and Yoga rounds it half-up to 22187 while
// Chrome reports 22186, so #a is one step (0.015625 device px) wide, #b starts
// one step late and is one step narrow, and #c is exact again. Sub-grid, and
// the whole of what is left between the two flex resolvers here.
TEST(FlexGrowShrinkBasisChromeParity, BasisPxAutoZero)
{
    CheckBothScales({"BasisPxAutoZero", kXmlBasisPxAutoZero, kCssBasisPxAutoZero, 400.0f, 60.0f, &kRowsBasisPxAutoZero});
}

// Exact parity. The first pass hands every item 133.33, #a violates its 200px
// minimum and freezes there, and the second pass splits the remaining 200
// between #b and #c: 200 / 100 / 100. Growing on top of the clamp instead --
// what a clamped-basis resolver does -- would read 266.67 / 66.67 / 66.67.
TEST(FlexGrowShrinkBasisChromeParity, MinWidthClampMidResolution)
{
    CheckBothScales({"MinWidthClampMidResolution", kXmlMinWidthClampMidResolution, kCssMinWidthClampMidResolution, 400.0f, 60.0f, &kRowsMinWidthClampMidResolution});
}

// Exact parity, and the control: a max violation in the GROW direction was
// already correct before the resolver was replaced, because a max-clamped
// zero basis and its hypothetical size differ, so the stock two-pass resolver
// froze it. Read with MinWidthClampMidResolution it localises what changed.
TEST(FlexGrowShrinkBasisChromeParity, MaxWidthClampMidResolution)
{
    CheckBothScales({"MaxWidthClampMidResolution", kXmlMaxWidthClampMidResolution, kCssMaxWidthClampMidResolution, 400.0f, 60.0f, &kRowsMaxWidthClampMidResolution});
}

// Exact parity, and the fixture that pins the line to its container. #a
// freezes at its 90px maximum and the other two shrink into what is left:
// 90 / 105 / 105, summing to exactly 300. The failure this replaces scaled
// every item by 1 - 190/600 (61.5 / 136.67 / 136.67) and overflowed the
// container by 34.83px, because the shortfall was measured from max-clamped
// bases but shared out in proportion to unclamped ones.
TEST(FlexGrowShrinkBasisChromeParity, MaxWidthClampUnderShrink)
{
    CheckBothScales({"MaxWidthClampUnderShrink", kXmlMaxWidthClampUnderShrink, kCssMaxWidthClampUnderShrink, 300.0f, 60.0f, &kRowsMaxWidthClampUnderShrink});
}

// Exact parity on the block axis: min-height freezes #b at 200 and #a and #c
// split the remaining 100. The resolver is axis-agnostic, so this passing and
// MinWidthClampMidResolution passing are one fact, not two.
TEST(FlexGrowShrinkBasisChromeParity, MinHeightClampColumn)
{
    CheckBothScales({"MinHeightClampColumn", kXmlMinHeightClampColumn, kCssMinHeightClampColumn, 200.0f, 300.0f, &kRowsMinHeightClampColumn});
}

// Exact parity, and the block-axis twin of MaxWidthClampMidResolution.
TEST(FlexGrowShrinkBasisChromeParity, MaxHeightClampColumn)
{
    CheckBothScales({"MaxHeightClampColumn", kXmlMaxHeightClampColumn, kCssMaxHeightClampColumn, 200.0f, 300.0f, &kRowsMaxHeightClampColumn});
}

// MinWidthClampMidResolution with every length multiplied by 8. It is here
// because the failure it replaces scaled by 8 too (66.67 -> 533.33): the grid
// quantum does not scale with the fixture, so rounding was never a candidate
// explanation and is not one for this fixture passing either.
TEST(FlexGrowShrinkBasisChromeParity, MinWidthClampScaled)
{
    CheckBothScales({"MinWidthClampScaled", kXmlMinWidthClampScaled, kCssMinWidthClampScaled, 3200.0f, 60.0f, &kRowsMinWidthClampScaled});
}

// MaxWidthClampUnderShrink x8, for the same reason: the failure it replaces
// reproduced the same 0.68333 factor at 8x (492 / 1093.33 / 1093.34 against
// 720 / 840 / 840) with the overflow scaling to 278.67px, so the mechanism
// was arithmetic and not a rounding artefact.
TEST(FlexGrowShrinkBasisChromeParity, MaxWidthClampUnderShrinkScaled)
{
    CheckBothScales({"MaxWidthClampUnderShrinkScaled", kXmlMaxWidthClampUnderShrinkScaled, kCssMaxWidthClampUnderShrinkScaled, 2400.0f, 60.0f, &kRowsMaxWidthClampUnderShrinkScaled});
}

// Two min violations that cannot both be found in one distribution, so the
// number of iterations is itself under test. Four items grow from a zero basis
// in a 600px box: the first distribution gives 150 each, so only #a (min 300)
// violates and freezes -- #b (min 150) is exactly at its floor. Freezing #a
// leaves 300 for three items, 100 each, and only now does #b violate. The
// third distribution splits the last 150 between #c and #d: 300 / 150 / 75 /
// 75. A resolver that detects violations a fixed number of times cannot reach
// this: stopping after one detection hands #b, #c and #d 100 each, and #b's
// minimum then either survives violated or is re-clamped without the 50px it
// takes back ever reaching its siblings.
TEST(FlexGrowShrinkBasisChromeParity, MinWidthClampCascade)
{
    CheckBothScales({"MinWidthClampCascade", kXmlMinWidthClampCascade, kCssMinWidthClampCascade, 600.0f, 60.0f, &kRowsMinWidthClampCascade});
}

// Both violation signs in one distribution, which is what makes the freeze
// rule observable. Four 150px items shrink into 300px: the first distribution
// gives 75 each, so #a is 45 above its 30px maximum and #b is 10 below its
// 85px minimum. The total violation is negative, so ONLY the max violator
// freezes; #b stays flexible despite currently violating. The second
// distribution gives the remaining three 90 each, #b no longer violates, and
// the line settles at 30 / 90 / 90 / 90. Freezing both violators together --
// the obvious reading of "clamp and freeze" -- pins #b at 85 and reads
// 30 / 85 / 92.5 / 92.5 instead.
TEST(FlexGrowShrinkBasisChromeParity, OppositeClampViolationsUnderShrink)
{
    CheckBothScales({"OppositeClampViolationsUnderShrink", kXmlOppositeClampViolationsUnderShrink, kCssOppositeClampViolationsUnderShrink, 300.0f, 60.0f, &kRowsOppositeClampViolationsUnderShrink});
}

// One auto main-axis margin absorbs all 300px of free space and puts the item
// at x=300.
TEST(FlexGrowShrinkBasisChromeParity, AutoMarginLeftOnly)
{
    CheckBothScales({"AutoMarginLeftOnly", kXmlAutoMarginLeftOnly, kCssAutoMarginLeftOnly, 400.0f, 60.0f, &kRowsAutoMarginLeftOnly});
}

// Two auto margins split the free space and centre the item at x=150.
TEST(FlexGrowShrinkBasisChromeParity, AutoMarginBothSides)
{
    CheckBothScales({"AutoMarginBothSides", kXmlAutoMarginBothSides, kCssAutoMarginBothSides, 400.0f, 60.0f, &kRowsAutoMarginBothSides});
}

// Cross axis. Auto block-axis margins centre the item at y=20 in a 60px row,
// so the unit is honoured on both axes and not only along the flex line.
TEST(FlexGrowShrinkBasisChromeParity, AutoMarginCrossAxis)
{
    CheckBothScales({"AutoMarginCrossAxis", kXmlAutoMarginCrossAxis, kCssAutoMarginCrossAxis, 400.0f, 60.0f, &kRowsAutoMarginCrossAxis});
}

// The control for the auto-margin family: flex-grow eats the free space first,
// so the correct auto margin here is 0 -- the same answer an engine that drops
// auto margins entirely would give. It is the fixture that keeps the others
// from passing for the wrong reason, and it was already at parity before auto
// margins worked.
TEST(FlexGrowShrinkBasisChromeParity, AutoMarginLosesToGrow)
{
    CheckBothScales({"AutoMarginLosesToGrow", kXmlAutoMarginLosesToGrow, kCssAutoMarginLosesToGrow, 400.0f, 60.0f, &kRowsAutoMarginLosesToGrow});
}

// The 200px of free space is split across BOTH auto margins on the line, not
// handed to whichever item declared one, so the items land at 100 and 300.
TEST(FlexGrowShrinkBasisChromeParity, AutoMarginSplitAcrossItems)
{
    CheckBothScales({"AutoMarginSplitAcrossItems", kXmlAutoMarginSplitAcrossItems, kCssAutoMarginSplitAcrossItems, 400.0f, 60.0f, &kRowsAutoMarginSplitAcrossItems});
}
