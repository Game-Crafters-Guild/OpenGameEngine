// Flexbox parity vs Chrome, with the DEFAULT VALUE as the subject.
//
// Every fixture here omits exactly one flex property (or uses one shorthand
// form) and asserts what the omission resolves to. Four layers can each supply
// a different answer and only the last one wins:
//
//   1. CSS spec initial values  (flex-shrink: 1, flex-basis: auto,
//      align-items/align-content: normal -> behaves as stretch on a flex box)
//   2. Yoga's native defaults   (YGNodeNew, no web-defaults config:
//      flexDirection Column, alignContent FlexStart, flexShrink 0)
//   3. LayoutInputs' member initialisers, which YogaAdapter::ApplyStyle pushes
//      UNCONDITIONALLY for every property on every node -- so layer 3
//      overwrites layer 2 and Yoga's own defaults are never observable through
//      the CSS path.
//   4. The CONTAINER, for flex-shrink alone: a child of a block-flow container
//      (display: block / inline, and the omitted default) takes 0 rather than
//      layer 3's answer, because block layout has no shrink step
//      (ApplyBlockFlowShrinkDefault). An authored flex-shrink still wins.
//
// That is why these tests exist: nothing else in the suite pins the
// LayoutInputs initialisers against a browser, and a one-word edit there
// silently retargets every element in the editor that did not spell the
// property out.
//
// GROUND TRUTH. Real Chrome, not an emulated device scale:
//   "C:\Program Files\Google\Chrome\Application\chrome.exe" --headless=new
//     --disable-gpu --no-sandbox --force-device-scale-factor=1
//     --virtual-time-budget=4000 --dump-dom file:///<scratch>/defaults.html
// measured with getBoundingClientRect on every element, expressed relative to
// the specimen root's own rect. Raw values are quoted per test. Content scale
// is irrelevant to a default (it is a logical-px question), so these run at
// 1.0; the physical-px mapping is FlexPrimitiveSpotCheckTests' job. The
// block-flow fixtures were re-measured at --force-device-scale-factor=2 as
// well: every number below is a whole logical px and came back identical.
//
// THE WRAPPER IS PART OF THE FIXTURE. #root is a flex column, so every specimen
// root is a flex ITEM of it and inherits the CSS default flex-shrink: 1. With
// the specimen roots stacked on one HTML page that shrink is not hypothetical:
// it crushed d1root from 200px to 40px and dragged min-height:auto's
// content-based minimum in with it. Every specimen root therefore spells out
// flex-grow/shrink/basis and align-self, which is also what makes the HTML arm
// (all specimens on one page) and the engine arm (one specimen per fixture)
// measure the same thing.
//
// The fixtures are textless on purpose. A default-value divergence is a number
// like 50 vs 80; dragging glyph metrics in would add a tolerance wide enough to
// swallow smaller ones.

#include "IsolatedUIFixture.h"

#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

using GameEngine::UIElement;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

// Chrome reports integers for every specimen below except the three-way split
// in DistributionLandsOnChromesSixtyFourthGrid, whose thirds are fractional in
// both engines. 0.01 logical px is far tighter than any divergence a wrong
// default produces, and -- since the layout grid is 1/64 px = 0.015625 -- it is
// also tighter than one grid step, so a single-step error still fails.
constexpr float kEps = 0.01f;

// The wrapper exists only to give the specimen a parent whose own layout cannot
// perturb it. Every property on it is spelled out, so the wrapper never
// contributes a default to the thing under test.
constexpr char kWrapperCss[] =
    "#root { display: flex; flex-direction: column; flex-wrap: nowrap;"
    " align-items: flex-start; align-content: flex-start;"
    " justify-content: flex-start; width: 800px; height: 600px; }\n";

// Pasted verbatim into every specimen root's rule (see the wrapper note above).
#define SPEC_ROOT_ITEM "flex-grow: 0; flex-shrink: 0; flex-basis: auto; align-self: flex-start;"

struct Rect
{
    float X = 0.0f;
    float Y = 0.0f;
    float W = 0.0f;
    float H = 0.0f;
};

// Logical px, relative to the specimen root -- the same quantity the Chrome arm
// dumped (getBoundingClientRect minus the specimen root's rect).
Rect Rel(const IsolatedUIFixture& fx, const char* rootId, const char* id)
{
    const UIElement* root = fx.Element(rootId);
    const UIElement* el = fx.Element(id);
    if (!root || !el)
        return {-1.0f, -1.0f, -1.0f, -1.0f};
    return {el->GetLayoutX() - root->GetLayoutX(), el->GetLayoutY() - root->GetLayoutY(),
            el->GetLayoutWidth(), el->GetLayoutHeight()};
}

// The specimen root's own box. A specimen whose root did not come out at its
// authored size was perturbed by the wrapper and its child numbers mean
// nothing, so every test checks this first.
Rect Own(const IsolatedUIFixture& fx, const char* rootId)
{
    const UIElement* el = fx.Element(rootId);
    if (!el)
        return {-1.0f, -1.0f, -1.0f, -1.0f};
    return {0.0f, 0.0f, el->GetLayoutWidth(), el->GetLayoutHeight()};
}

void ExpectRect(const Rect& r, float x, float y, float w, float h, const char* what)
{
    EXPECT_NEAR(r.X, x, kEps) << what << " x";
    EXPECT_NEAR(r.Y, y, kEps) << what << " y";
    EXPECT_NEAR(r.W, w, kEps) << what << " w";
    EXPECT_NEAR(r.H, h, kEps) << what << " h";
}

void ExpectSize(const Rect& r, float w, float h, const char* what)
{
    EXPECT_NEAR(r.W, w, kEps) << what << " w";
    EXPECT_NEAR(r.H, h, kEps) << what << " h";
}

// Builds at content scale 1 and skips only when there is no device. A parse
// failure is the test's own bug and fails loudly.
bool BuildOrSkipReason(IsolatedUIFixture& fx, const std::string& xml, const std::string& css,
                       std::string& outSkip)
{
    if (fx.Build(1.0f, xml, kWrapperCss + css))
        return true;
    if (!fx.DeviceAvailable())
    {
        outSkip = fx.Diagnostic();
        return false;
    }
    ADD_FAILURE() << fx.Diagnostic();
    outSkip.clear();
    return false;
}

#define BUILD_OR_SKIP(fx, xml, css)                                                                \
    do                                                                                             \
    {                                                                                              \
        std::string skipReason;                                                                    \
        if (!BuildOrSkipReason(fx, xml, css, skipReason))                                          \
        {                                                                                          \
            if (!skipReason.empty())                                                               \
                GTEST_SKIP() << skipReason;                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (false)

} // namespace

// --- flex-direction ---------------------------------------------------------
//
// Chrome: d1root 300x200, d1a (0,0,60,40), d1b (60,0,50,30) -- row.
//
// The engine does not read Yoga's Column default here: ApplyStyle derives the
// axis from `display` when no flex-direction was authored
// (YogaLayout.cpp:149-164), so display:flex means row and display:block means
// column. OmittedDisplayIsBlockFlow pins the block half.
TEST(FlexDefaultsParity, OmittedFlexDirectionIsRowOnAFlexContainer)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d1root"><uielement id="d1a"/><uielement id="d1b"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d1root { display: flex; flex-wrap: nowrap; align-items: flex-start; align-content: flex-start; justify-content: flex-start; width: 300px; height: 200px; )" SPEC_ROOT_ITEM R"( }
#d1a { width: 60px; height: 40px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
#d1b { width: 50px; height: 30px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d1root"), 300.0f, 200.0f, "d1root");
    ExpectRect(Rel(fx, "d1root", "d1a"), 0.0f, 0.0f, 60.0f, 40.0f, "d1a");
    ExpectRect(Rel(fx, "d1root", "d1b"), 60.0f, 0.0f, 50.0f, 30.0f, "d1b");
}

// --- flex-wrap --------------------------------------------------------------
//
// Chrome: d2root 200x100, d2a (0,0,100,30), d2b (100,0,100,30),
//         d2c (200,0,100,30)
// -- nowrap, so the third child overflows the 200px container rather than
// starting a second line. Note the children spell flex-shrink: 0, or the
// omitted-shrink default would resize them and mask the wrap answer.
TEST(FlexDefaultsParity, OmittedFlexWrapIsNowrap)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d2root"><uielement id="d2a"/><uielement id="d2b"/><uielement id="d2c"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d2root { display: flex; flex-direction: row; align-items: flex-start; align-content: flex-start; justify-content: flex-start; width: 200px; height: 100px; )" SPEC_ROOT_ITEM R"( }
#d2a { width: 100px; height: 30px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
#d2b { width: 100px; height: 30px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
#d2c { width: 100px; height: 30px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d2root"), 200.0f, 100.0f, "d2root");
    ExpectRect(Rel(fx, "d2root", "d2a"), 0.0f, 0.0f, 100.0f, 30.0f, "d2a");
    ExpectRect(Rel(fx, "d2root", "d2b"), 100.0f, 0.0f, 100.0f, 30.0f, "d2b");
    ExpectRect(Rel(fx, "d2root", "d2c"), 200.0f, 0.0f, 100.0f, 30.0f, "d2c");
}

// --- align-items ------------------------------------------------------------
//
// Chrome: d3root 300x120, d3a (0,0,80,120), d3b (80,0,60,20)
// -- css-align-3 6.2.4: `normal` behaves as `stretch` on a flex item, so the
// height-less child fills the 120px cross axis. d3b spells align-self out and
// keeps its 20px, which discriminates "stretch happened" from "the child just
// has no height".
TEST(FlexDefaultsParity, OmittedAlignItemsIsStretch)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d3root"><uielement id="d3a"/><uielement id="d3b"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d3root { display: flex; flex-direction: row; flex-wrap: nowrap; align-content: flex-start; justify-content: flex-start; width: 300px; height: 120px; )" SPEC_ROOT_ITEM R"( }
#d3a { width: 80px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
#d3b { width: 60px; height: 20px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; align-self: flex-start; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d3root"), 300.0f, 120.0f, "d3root");
    ExpectRect(Rel(fx, "d3root", "d3a"), 0.0f, 0.0f, 80.0f, 120.0f, "d3a");
    ExpectRect(Rel(fx, "d3root", "d3b"), 80.0f, 0.0f, 60.0f, 20.0f, "d3b");
}

// --- align-content ----------------------------------------------------------
//
// Chrome: d4root 200x300, d4a (0,0,100,40), d4b (100,0,100,40),
//         d4c (0,150,100,40), d4d (100,150,100,40)
//
// The sharpest of the family. Two flex lines of 40px sit in 300px of cross
// space; `normal` behaves as `stretch`, so each line becomes 150 tall and the
// second line starts at 150. Yoga's NATIVE default for align-content is
// FlexStart, which would put the second line at y=40 -- a 110px error. It does
// not happen only because LayoutInputs::AlignContent is initialised to Stretch
// (ResolvedStyle.h:24) and ApplyStyle pushes it every time
// (YogaLayout.cpp:172-183). align-items:flex-start keeps the CHILDREN 40 tall
// inside the stretched lines, so the assertion reads line position, not child
// stretch.
TEST(FlexDefaultsParity, OmittedAlignContentIsStretch)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d4root"><uielement id="d4a"/><uielement id="d4b"/><uielement id="d4c"/><uielement id="d4d"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d4root { display: flex; flex-direction: row; flex-wrap: wrap; align-items: flex-start; justify-content: flex-start; width: 200px; height: 300px; )" SPEC_ROOT_ITEM R"( }
#d4a { width: 100px; height: 40px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
#d4b { width: 100px; height: 40px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
#d4c { width: 100px; height: 40px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
#d4d { width: 100px; height: 40px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d4root"), 200.0f, 300.0f, "d4root");
    ExpectRect(Rel(fx, "d4root", "d4a"), 0.0f, 0.0f, 100.0f, 40.0f, "d4a");
    ExpectRect(Rel(fx, "d4root", "d4b"), 100.0f, 0.0f, 100.0f, 40.0f, "d4b");
    ExpectRect(Rel(fx, "d4root", "d4c"), 0.0f, 150.0f, 100.0f, 40.0f, "d4c");
    ExpectRect(Rel(fx, "d4root", "d4d"), 100.0f, 150.0f, 100.0f, 40.0f, "d4d");
}

// --- flex-shrink ------------------------------------------------------------
//
// Chrome: d5root 240x100, d5a (0,0,80,30), d5b (80,0,80,30), d5c (160,0,80,30)
// -- 3x100 of basis in a 240px line, shrink factor 1 each, so each loses 20.
// Yoga's native default is 0 (no shrink, 100 each, overflowing); an undeclared
// LayoutInputs::FlexShrink resolves to the CSS initial 1 inside a flex
// container (kCssInitialFlexShrink), which is what d5root is.
TEST(FlexDefaultsParity, OmittedFlexShrinkIsOne)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d5root"><uielement id="d5a"/><uielement id="d5b"/><uielement id="d5c"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d5root { display: flex; flex-direction: row; flex-wrap: nowrap; align-items: flex-start; align-content: flex-start; justify-content: flex-start; width: 240px; height: 100px; )" SPEC_ROOT_ITEM R"( }
#d5a { flex-basis: 100px; flex-grow: 0; height: 30px; }
#d5b { flex-basis: 100px; flex-grow: 0; height: 30px; }
#d5c { flex-basis: 100px; flex-grow: 0; height: 30px; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d5root"), 240.0f, 100.0f, "d5root");
    ExpectRect(Rel(fx, "d5root", "d5a"), 0.0f, 0.0f, 80.0f, 30.0f, "d5a");
    ExpectRect(Rel(fx, "d5root", "d5b"), 80.0f, 0.0f, 80.0f, 30.0f, "d5b");
    ExpectRect(Rel(fx, "d5root", "d5c"), 160.0f, 0.0f, 80.0f, 30.0f, "d5c");
}

// The same default reached through `width` rather than `flex-basis`, because
// basis:auto has to defer to width before shrink can act on it.
//
// Chrome: d17root 200x100, d17a (0,0,100,20), d17b (100,0,100,20)
// -- 2x150 of width in a 200px line, 100 of overflow removed evenly.
TEST(FlexDefaultsParity, OmittedFlexShrinkAlsoShrinksAWidth)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d17root"><uielement id="d17a"/><uielement id="d17b"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d17root { display: flex; flex-direction: row; flex-wrap: nowrap; align-items: flex-start; align-content: flex-start; justify-content: flex-start; width: 200px; height: 100px; )" SPEC_ROOT_ITEM R"( }
#d17a { width: 150px; height: 20px; flex-grow: 0; flex-basis: auto; }
#d17b { width: 150px; height: 20px; flex-grow: 0; flex-basis: auto; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d17root"), 200.0f, 100.0f, "d17root");
    ExpectRect(Rel(fx, "d17root", "d17a"), 0.0f, 0.0f, 100.0f, 20.0f, "d17a");
    ExpectRect(Rel(fx, "d17root", "d17b"), 100.0f, 0.0f, 100.0f, 20.0f, "d17b");
}

// --- flex-basis -------------------------------------------------------------
//
// Chrome: d6root 400x100, d6a (0,0,120,30), d6b (120,0,280,30)
// -- basis `auto` defers to `width`, so d6a's basis is 120 and d6b's is 50;
// 400-170 = 230 of free space all goes to the one growable child (50+230=280).
// A basis that defaulted to 0 instead would give d6a 0 and d6b 400.
TEST(FlexDefaultsParity, OmittedFlexBasisIsAutoAndDefersToWidth)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d6root"><uielement id="d6a"/><uielement id="d6b"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d6root { display: flex; flex-direction: row; flex-wrap: nowrap; align-items: flex-start; align-content: flex-start; justify-content: flex-start; width: 400px; height: 100px; )" SPEC_ROOT_ITEM R"( }
#d6a { width: 120px; height: 30px; flex-grow: 0; flex-shrink: 0; }
#d6b { width: 50px; height: 30px; flex-grow: 1; flex-shrink: 1; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d6root"), 400.0f, 100.0f, "d6root");
    ExpectRect(Rel(fx, "d6root", "d6a"), 0.0f, 0.0f, 120.0f, 30.0f, "d6a");
    ExpectRect(Rel(fx, "d6root", "d6b"), 120.0f, 0.0f, 280.0f, 30.0f, "d6b");
}

// --- flex-grow --------------------------------------------------------------
//
// Chrome: d7root 300x60, d7a (0,0,100,20) -- 200px of free space stays free.
TEST(FlexDefaultsParity, OmittedFlexGrowIsZero)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d7root"><uielement id="d7a"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d7root { display: flex; flex-direction: row; flex-wrap: nowrap; align-items: flex-start; align-content: flex-start; justify-content: flex-start; width: 300px; height: 60px; )" SPEC_ROOT_ITEM R"( }
#d7a { width: 100px; height: 20px; flex-shrink: 0; flex-basis: auto; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d7root"), 300.0f, 60.0f, "d7root");
    ExpectRect(Rel(fx, "d7root", "d7a"), 0.0f, 0.0f, 100.0f, 20.0f, "d7a");
}

// --- justify-content --------------------------------------------------------
//
// Chrome: d14root 300x60, d14a (0,0,100,20) -- flex-start, all free space after.
TEST(FlexDefaultsParity, OmittedJustifyContentIsFlexStart)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d14root"><uielement id="d14a"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d14root { display: flex; flex-direction: row; flex-wrap: nowrap; align-items: flex-start; align-content: flex-start; width: 300px; height: 60px; )" SPEC_ROOT_ITEM R"( }
#d14a { width: 100px; height: 20px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d14root"), 300.0f, 60.0f, "d14root");
    ExpectRect(Rel(fx, "d14root", "d14a"), 0.0f, 0.0f, 100.0f, 20.0f, "d14a");
}

// --- align-self -------------------------------------------------------------
//
// Chrome: d8root 300x100, d8a (0,40,60,20) -- `auto` defers to the container's
// align-items:center. ToYGAlign's per-property fallback is what makes this
// work: align-self maps Auto to YGAlignAuto while align-items maps it to
// YGAlignStretch (YogaLayout.cpp:32-42, 170, 199). Collapsing those two onto
// one fallback puts the child at y=0 (stretch) instead.
TEST(FlexDefaultsParity, OmittedAlignSelfDefersToAlignItems)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d8root"><uielement id="d8a"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d8root { display: flex; flex-direction: row; flex-wrap: nowrap; align-items: center; align-content: flex-start; justify-content: flex-start; width: 300px; height: 100px; )" SPEC_ROOT_ITEM R"( }
#d8a { width: 60px; height: 20px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d8root"), 300.0f, 100.0f, "d8root");
    ExpectRect(Rel(fx, "d8root", "d8a"), 0.0f, 40.0f, 60.0f, 20.0f, "d8a");
}

// --- `flex: <number>` -------------------------------------------------------
//
// Chrome: d9root 300x60, d9a (0,0,150,20), d9b (150,0,150,20)
// -- css-flexbox-1 7.1.1: a lone <number> is the grow factor and the omitted
// basis becomes 0%, NOT auto. ExpandFlexValue seeds basis with Px(0)
// (CSSValueParsers.cpp:1707); 0px and 0% are the same length, so the two agree.
TEST(FlexDefaultsParity, FlexShorthandNumberGivesZeroBasis)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d9root"><uielement id="d9a"/><uielement id="d9b"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d9root { display: flex; flex-direction: row; flex-wrap: nowrap; align-items: flex-start; align-content: flex-start; justify-content: flex-start; width: 300px; height: 60px; )" SPEC_ROOT_ITEM R"( }
#d9a { flex: 1; height: 20px; }
#d9b { flex: 1; height: 20px; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d9root"), 300.0f, 60.0f, "d9root");
    ExpectRect(Rel(fx, "d9root", "d9a"), 0.0f, 0.0f, 150.0f, 20.0f, "d9a");
    ExpectRect(Rel(fx, "d9root", "d9b"), 150.0f, 0.0f, 150.0f, 20.0f, "d9b");
}

// --- `flex: <grow> <shrink>` ------------------------------------------------
//
// Chrome: d19root 300x60, d19a (0,0,200,20), d19b (200,0,100,20)
// -- two numbers are grow and shrink; the omitted basis is 0, so `width: 40px`
// on both children is inert and 300 of free space splits 2:1.
TEST(FlexDefaultsParity, FlexShorthandTwoNumbersAreGrowAndShrink)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d19root"><uielement id="d19a"/><uielement id="d19b"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d19root { display: flex; flex-direction: row; flex-wrap: nowrap; align-items: flex-start; align-content: flex-start; justify-content: flex-start; width: 300px; height: 60px; )" SPEC_ROOT_ITEM R"( }
#d19a { flex: 2 3; width: 40px; height: 20px; }
#d19b { flex: 1 1; width: 40px; height: 20px; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d19root"), 300.0f, 60.0f, "d19root");
    ExpectRect(Rel(fx, "d19root", "d19a"), 0.0f, 0.0f, 200.0f, 20.0f, "d19a");
    ExpectRect(Rel(fx, "d19root", "d19b"), 200.0f, 0.0f, 100.0f, 20.0f, "d19b");
}

// --- `flex: none` -----------------------------------------------------------
//
// Chrome: d18root 300x60, d18a (0,0,200,20), d18b (200,0,200,20)
// -- `none` is `0 0 auto`, so the widths are honoured and the pair overflows
// the 300px line rather than shrinking to fit.
TEST(FlexDefaultsParity, FlexNoneKeywordResolvesToZeroZeroAuto)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d18root"><uielement id="d18a"/><uielement id="d18b"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d18root { display: flex; flex-direction: row; flex-wrap: nowrap; align-items: flex-start; align-content: flex-start; justify-content: flex-start; width: 300px; height: 60px; )" SPEC_ROOT_ITEM R"( }
#d18a { flex: none; width: 200px; height: 20px; }
#d18b { flex: none; width: 200px; height: 20px; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d18root"), 300.0f, 60.0f, "d18root");
    ExpectRect(Rel(fx, "d18root", "d18a"), 0.0f, 0.0f, 200.0f, 20.0f, "d18a");
    ExpectRect(Rel(fx, "d18root", "d18b"), 200.0f, 0.0f, 200.0f, 20.0f, "d18b");
}

// --- `flex: initial` --------------------------------------------------------
//
// Chrome: d11root 300x60, d11a (0,0,80,20), d11b (80,0,60,20)
// -- `flex: initial` is `0 1 auto`, so d11a keeps its 80px width.
// The CSS-wide keyword never reaches ExpandFlexValue: the declaration driver
// intercepts it and fans Initial out to the three longhands named by
// kFlexKeywordTargets (CSSValueParsers.cpp:1783-1785), which then resolve to
// the LayoutInputs initialisers -- exactly 0 / 1 / auto.
TEST(FlexDefaultsParity, FlexInitialKeywordResolvesToZeroOneAuto)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d11root"><uielement id="d11a"/><uielement id="d11b"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d11root { display: flex; flex-direction: row; flex-wrap: nowrap; align-items: flex-start; align-content: flex-start; justify-content: flex-start; width: 300px; height: 60px; )" SPEC_ROOT_ITEM R"( }
#d11a { flex: initial; width: 80px; height: 20px; }
#d11b { width: 60px; height: 20px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d11root"), 300.0f, 60.0f, "d11root");
    ExpectRect(Rel(fx, "d11root", "d11a"), 0.0f, 0.0f, 80.0f, 20.0f, "d11a");
    ExpectRect(Rel(fx, "d11root", "d11b"), 80.0f, 0.0f, 60.0f, 20.0f, "d11b");
}

// --- `flex: <grow> <shrink> <basis>` ----------------------------------------
//
// Chrome: d20root 300x60, d20a (0,0,200,20), d20b (200,0,100,20)
// -- the fully spelled-out form, and the control for the single-token case
// below: 300 - 100 - 0 = 200 of free space splits evenly (100+100, 0+100).
TEST(FlexDefaultsParity, FlexShorthandThreeTokensAssignInOrder)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d20root"><uielement id="d20a"/><uielement id="d20b"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d20root { display: flex; flex-direction: row; flex-wrap: nowrap; align-items: flex-start; align-content: flex-start; justify-content: flex-start; width: 300px; height: 60px; )" SPEC_ROOT_ITEM R"( }
#d20a { flex: 1 1 100px; height: 20px; }
#d20b { flex: 1 1 0px; height: 20px; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d20root"), 300.0f, 60.0f, "d20root");
    ExpectRect(Rel(fx, "d20root", "d20a"), 0.0f, 0.0f, 200.0f, 20.0f, "d20a");
    ExpectRect(Rel(fx, "d20root", "d20b"), 200.0f, 0.0f, 100.0f, 20.0f, "d20b");
}

// --- `flex: <length>` --------------------------------------------------------
//
// Chrome: d10root 300x60, d10a (0,0,200,20), d10b (200,0,100,20)
// -- css-flexbox-1 7.1.1: a lone value WITH A UNIT is the BASIS, and the
// omitted grow/shrink both become 1. So d10a is `1 1 100px`, d10b is `1 1 0`,
// and Chrome's numbers are identical to FlexShorthandThreeTokensAssignInOrder
// directly above. ExpandFlexValue classifies the token by its unit and routes
// a length to the basis; the engine matches Chrome exactly.
TEST(FlexDefaultsParity, FlexShorthandSingleLengthIsTheBasis)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d10root"><uielement id="d10a"/><uielement id="d10b"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d10root { display: flex; flex-direction: row; flex-wrap: nowrap; align-items: flex-start; align-content: flex-start; justify-content: flex-start; width: 300px; height: 60px; )" SPEC_ROOT_ITEM R"( }
#d10a { flex: 100px; height: 20px; }
#d10b { flex: 1; height: 20px; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d10root"), 300.0f, 60.0f, "d10root");
    // Chrome: (0,0,200,20) and (200,0,100,20) — matched exactly.
    ExpectRect(Rel(fx, "d10root", "d10a"), 0.0f, 0.0f, 200.0f, 20.0f, "d10a");
    ExpectRect(Rel(fx, "d10root", "d10b"), 200.0f, 0.0f, 100.0f, 20.0f, "d10b");
}

// --- the layout GRID is a default too ---------------------------------------
//
// Chrome: d21root 100x60, d21a (0,0,33.3281,20), d21b (33.3281,0,33.3438,20),
//         d21c (66.6719,0,33.3281,20)
// -- three equal grow factors over 100px. Chrome distributes in LayoutUnits
// (1/64 px) and quantises to them, so the thirds are fractional and the middle
// width is one unit wider to keep the total exactly 100.
//
// The engine's grid is that same quantum: YogaAdapter::SetContentScale gives
// the shared config a pointScaleFactor of 64 per DEVICE pixel
// (YogaLayout.cpp:78-100), and YGNodeCalculateLayout rounds every solved edge
// onto it. The numbers below are therefore Chrome's exactly, to the last unit:
// 33.3281 is 2133/64, 33.3438 is 2134/64, 66.6719 is 4267/64.
//
// The grid is a DEFAULT nothing else in this file exercises, because every
// other specimen here distributes into integers and is byte-identical on any
// grid finer than 1px. This is the one fixture that can see the quantum at all.
//
// kEps (0.01) is smaller than one unit (0.015625), so a grid one step coarser
// or finer fails rather than passing on tolerance.
TEST(FlexDefaultsParity, DistributionLandsOnChromesSixtyFourthGrid)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d21root"><uielement id="d21a"/><uielement id="d21b"/><uielement id="d21c"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d21root { display: flex; flex-direction: row; flex-wrap: nowrap; align-items: flex-start; align-content: flex-start; justify-content: flex-start; width: 100px; height: 60px; )" SPEC_ROOT_ITEM R"( }
#d21a { flex: 1; height: 20px; }
#d21b { flex: 1; height: 20px; }
#d21c { flex: 1; height: 20px; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d21root"), 100.0f, 60.0f, "d21root");

    const Rect a = Rel(fx, "d21root", "d21a");
    const Rect b = Rel(fx, "d21root", "d21b");
    const Rect c = Rel(fx, "d21root", "d21c");

    // Chrome's own numbers, not a tolerance around a third.
    ExpectRect(a, 0.0f, 0.0f, 33.328125f, 20.0f, "d21a");
    ExpectRect(b, 33.328125f, 0.0f, 33.34375f, 20.0f, "d21b");
    ExpectRect(c, 66.671875f, 0.0f, 33.328125f, 20.0f, "d21c");

    // Every edge is an exact multiple of the quantum -- the claim the literals
    // above only imply, stated where a future retune of the specimen cannot
    // quietly drop it.
    constexpr float kUnitsPerPx = 64.0f;
    for (const float v : {a.X, a.W, b.X, b.W, c.X, c.W})
        EXPECT_NEAR(v * kUnitsPerPx, std::round(v * kUnitsPerPx), kEps)
            << "layout edge must land on the 1/64 px grid";

    // Gapless and exactly filling the line -- the property that rounding EDGES
    // rather than widths buys, and that a naive round-each-width would break.
    EXPECT_NEAR(b.X, a.X + a.W, kEps) << "no gap between d21a and d21b";
    EXPECT_NEAR(c.X, b.X + b.W, kEps) << "no gap between d21b and d21c";
    EXPECT_NEAR(c.X + c.W, 100.0f, kEps) << "the three children fill the line";
}

// --- omitted `display` ------------------------------------------------------
//
// Chrome: d15root 200x300, d15a (0,0,200,80), d15b (0,80,200,80)
// -- two block boxes stack, and here they fit.
TEST(FlexDefaultsParity, OmittedDisplayIsBlockFlow)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d15root"><uielement id="d15a"/><uielement id="d15b"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d15root { width: 200px; height: 300px; )" SPEC_ROOT_ITEM R"( }
#d15a { height: 80px; }
#d15b { height: 80px; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d15root"), 200.0f, 300.0f, "d15root");
    ExpectRect(Rel(fx, "d15root", "d15a"), 0.0f, 0.0f, 200.0f, 80.0f, "d15a");
    ExpectRect(Rel(fx, "d15root", "d15b"), 0.0f, 80.0f, 200.0f, 80.0f, "d15b");
}

// The same container with the children OVERFLOWING it.
//
// Chrome: d12root 200x100, d12a (0,0,200,80), d12b (0,80,200,80). Block flow
// has no shrink step, so 160px of children simply overflow a 100px box.
//
// The engine has no block formatting context -- display:block becomes a Yoga
// COLUMN -- so overflow along that column's main axis would be resolved as a
// flex shrink and hand back 50 each. What stops it is the container: a
// block-flow parent gives its children flex-shrink 0 unless they declare the
// property (ApplyBlockFlowShrinkDefault), which is the whole of block layout's
// "children keep their size and overflow" rule as far as a Yoga column can
// express it.
//
// OmittedDisplayIsBlockFlow directly above is the control -- identical markup
// with room to spare, and it agreed with Chrome before this fixture did, which
// is what placed the cause on the shrink step rather than the block-to-column
// mapping.
TEST(FlexDefaultsParity, OmittedDisplayBlockDoesNotShrinkOverflowingChildren)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d12root"><uielement id="d12a"/><uielement id="d12b"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d12root { width: 200px; height: 100px; )" SPEC_ROOT_ITEM R"( }
#d12a { height: 80px; }
#d12b { height: 80px; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d12root"), 200.0f, 100.0f, "d12root");
    ExpectRect(Rel(fx, "d12root", "d12a"), 0.0f, 0.0f, 200.0f, 80.0f, "d12a");
    ExpectRect(Rel(fx, "d12root", "d12b"), 0.0f, 80.0f, 200.0f, 80.0f, "d12b");
}

// Three of them, so the answer cannot be a two-child coincidence and the
// stacking offsets accumulate past the container by 140px.
//
// Chrome: d22root 200x100, d22a (0,0,200,80), d22b (0,80,200,80),
// d22c (0,160,200,80).
TEST(FlexDefaultsParity, BlockFlowOverflowsWithThreeChildren)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d22root"><uielement id="d22a"/><uielement id="d22b"/><uielement id="d22c"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d22root { width: 200px; height: 100px; )" SPEC_ROOT_ITEM R"( }
#d22a { height: 80px; }
#d22b { height: 80px; }
#d22c { height: 80px; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d22root"), 200.0f, 100.0f, "d22root");
    ExpectRect(Rel(fx, "d22root", "d22a"), 0.0f, 0.0f, 200.0f, 80.0f, "d22a");
    ExpectRect(Rel(fx, "d22root", "d22b"), 0.0f, 80.0f, 200.0f, 80.0f, "d22b");
    ExpectRect(Rel(fx, "d22root", "d22c"), 0.0f, 160.0f, 200.0f, 80.0f, "d22c");
}

// Block inside block: the overflow has to survive a level of nesting, and the
// intermediate box has to size to its CONTENT rather than to the room its own
// parent has left.
//
// Chrome: d23root 200x100, d23mid (0,0,200,160), d23a (0,0,200,80),
// d23b (0,80,200,80) -- d23mid is 160 tall inside a 100 tall parent.
TEST(FlexDefaultsParity, BlockFlowOverflowSurvivesNesting)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d23root"><uielement id="d23mid"><uielement id="d23a"/><uielement id="d23b"/></uielement></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d23root { width: 200px; height: 100px; )" SPEC_ROOT_ITEM R"( }
#d23a { height: 80px; }
#d23b { height: 80px; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d23root"), 200.0f, 100.0f, "d23root");
    ExpectRect(Rel(fx, "d23root", "d23mid"), 0.0f, 0.0f, 200.0f, 160.0f, "d23mid");
    ExpectRect(Rel(fx, "d23root", "d23a"), 0.0f, 0.0f, 200.0f, 80.0f, "d23a");
    ExpectRect(Rel(fx, "d23root", "d23b"), 0.0f, 80.0f, 200.0f, 80.0f, "d23b");
}

// `flex-shrink: 0` spelled out on a block-flow child: same numbers as the
// omitted case, which is the point -- 0 is now what the omission means, so the
// declaration cannot be what is producing the answer above.
//
// Chrome: d24root 200x100, d24a (0,0,200,80), d24b (0,80,200,80).
TEST(FlexDefaultsParity, BlockFlowChildDeclaringShrinkZeroMatchesTheOmission)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d24root"><uielement id="d24a"/><uielement id="d24b"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d24root { width: 200px; height: 100px; )" SPEC_ROOT_ITEM R"( }
#d24a { height: 80px; flex-shrink: 0; }
#d24b { height: 80px; flex-shrink: 0; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d24root"), 200.0f, 100.0f, "d24root");
    ExpectRect(Rel(fx, "d24root", "d24a"), 0.0f, 0.0f, 200.0f, 80.0f, "d24a");
    ExpectRect(Rel(fx, "d24root", "d24b"), 0.0f, 80.0f, 200.0f, 80.0f, "d24b");
}

// `flex-shrink: 1` spelled out on a block-flow child -- DELIBERATE DIVERGENCE.
//
// Chrome: d25root 200x100, d25a (0,0,200,80), d25b (0,80,200,80) -- i.e.
// IDENTICAL to the undeclared case. A block container's children are not flex
// items, so the declaration is dead text there and Chrome has no behaviour to
// copy, only an absence.
//
// This engine honours it and shrinks to 50 each. The reason is that Block is
// this engine's general-purpose container -- LayoutInputs::DisplayMode defaults
// to it, so every element that does not spell out `display: flex` is one -- and
// Block honours flex-grow and flex-basis on its children. Obeying Chrome's
// absence for shrink alone would leave the editor's own `flex-grow: 1;
// flex-shrink: 1` pairs (core.css, views.css, widgets.css, SettingsPanel.css)
// half-obeyed, with no way for an author to ask for the other half back. The
// default is where the divergence actually bit, and the default is what moved.
TEST(FlexDefaultsParity, BlockFlowChildDeclaringShrinkOneStillShrinks)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d25root"><uielement id="d25a"/><uielement id="d25b"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d25root { width: 200px; height: 100px; )" SPEC_ROOT_ITEM R"( }
#d25a { height: 80px; flex-shrink: 1; }
#d25b { height: 80px; flex-shrink: 1; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d25root"), 200.0f, 100.0f, "d25root");
    // Chrome: (0,0,200,80) and (0,80,200,80).
    ExpectRect(Rel(fx, "d25root", "d25a"), 0.0f, 0.0f, 200.0f, 50.0f, "d25a");
    ExpectRect(Rel(fx, "d25root", "d25b"), 0.0f, 50.0f, 200.0f, 50.0f, "d25b");
}

// The same divergence reached through the shorthand, which is how the editor's
// CSS actually spells it. `flex: 1` is grow 1 / shrink 1 / basis 0, so it
// declares shrink and the children split the container evenly.
//
// Chrome: d26root 200x100, d26a (0,0,200,80), d26b (0,80,200,80) -- the whole
// shorthand is inert on a block-flow child there.
TEST(FlexDefaultsParity, BlockFlowChildDeclaringFlexShorthandStillShrinks)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d26root"><uielement id="d26a"/><uielement id="d26b"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d26root { width: 200px; height: 100px; )" SPEC_ROOT_ITEM R"( }
#d26a { height: 80px; flex: 1; }
#d26b { height: 80px; flex: 1; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d26root"), 200.0f, 100.0f, "d26root");
    // Chrome: (0,0,200,80) and (0,80,200,80).
    ExpectRect(Rel(fx, "d26root", "d26a"), 0.0f, 0.0f, 200.0f, 50.0f, "d26a");
    ExpectRect(Rel(fx, "d26root", "d26b"), 0.0f, 50.0f, 200.0f, 50.0f, "d26b");
}

// The guard on the other side: a REAL flex column with the identical overflow
// still shrinks, so the block-flow default did not leak into flex containers.
//
// Chrome: d27root 200x100, d27a (0,0,200,50), d27b (0,50,200,50).
TEST(FlexDefaultsParity, FlexColumnStillShrinksOverflowingChildren)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d27root"><uielement id="d27a"/><uielement id="d27b"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d27root { display: flex; flex-direction: column; flex-wrap: nowrap; align-items: stretch; align-content: flex-start; justify-content: flex-start; width: 200px; height: 100px; )" SPEC_ROOT_ITEM R"( }
#d27a { height: 80px; }
#d27b { height: 80px; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d27root"), 200.0f, 100.0f, "d27root");
    ExpectRect(Rel(fx, "d27root", "d27a"), 0.0f, 0.0f, 200.0f, 50.0f, "d27a");
    ExpectRect(Rel(fx, "d27root", "d27b"), 0.0f, 50.0f, 200.0f, 50.0f, "d27b");
}

// Flipping the CONTAINER's display at runtime has to reach the children, whose
// own style never changes. Both halves of that are load bearing and neither is
// reachable from CSS alone:
//
//   - the children carry no dirty bit of their own, so the subtree-skip gate
//     would serve them from cache and never re-push;
//   - their LayoutInputs compare equal across the flip, so the Yoga re-push
//     gate would decline even once they are visited.
//
// A display override raises only the container's own override-dirty bits (it
// is not MarkDirtySubtree, the way a class toggle is), which is exactly the
// case the two mechanisms exist for. Flipping back proves it is not one-way.
//
// flex-direction is spelled out so the flip moves ONE thing. Omitted, it is
// itself display-derived -- Column under Block, Row under Flex -- and the
// children would change axis as well as shrink, which measures both at once.
TEST(FlexDefaultsParity, ContainerDisplayFlipRestylesItsChildren)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d28root"><uielement id="d28a"/><uielement id="d28b"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d28root { width: 200px; height: 100px; flex-direction: column; )" SPEC_ROOT_ITEM R"( }
#d28a { height: 80px; }
#d28b { height: 80px; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    UIElement* container = fx.Element("d28root");
    ASSERT_NE(container, nullptr);

    // Block flow: no shrink step, so the children overflow.
    ExpectRect(Rel(fx, "d28root", "d28a"), 0.0f, 0.0f, 200.0f, 80.0f, "d28a block");
    ExpectRect(Rel(fx, "d28root", "d28b"), 0.0f, 80.0f, 200.0f, 80.0f, "d28b block");

    container->Overrides().Set(GameEngine::Style::Display, GameEngine::DisplayMode::Flex);
    fx.Settle();

    // A flex column with the CSS initial shrink of 1 takes the 60px of overflow
    // off the two children evenly.
    ExpectRect(Rel(fx, "d28root", "d28a"), 0.0f, 0.0f, 200.0f, 50.0f, "d28a flex");
    ExpectRect(Rel(fx, "d28root", "d28b"), 0.0f, 50.0f, 200.0f, 50.0f, "d28b flex");

    container->Overrides().Set(GameEngine::Style::Display, GameEngine::DisplayMode::Block);
    fx.Settle();

    ExpectRect(Rel(fx, "d28root", "d28a"), 0.0f, 0.0f, 200.0f, 80.0f, "d28a block again");
    ExpectRect(Rel(fx, "d28root", "d28b"), 0.0f, 80.0f, 200.0f, 80.0f, "d28b block again");
}

// --- nothing authored at all ------------------------------------------------
//
// Chrome: d13root 300x100, d13a (0,0,60,100), d13b (60,0,50,100)
// -- row (direction), one line (wrap), full-height children (align-items),
// no growth (grow), widths honoured (basis auto). One fixture that fails if any
// single default moves.
TEST(FlexDefaultsParity, BareFlexContainerMatchesEveryCssInitial)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="d13root"><uielement id="d13a"/><uielement id="d13b"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#d13root { display: flex; width: 300px; height: 100px; )" SPEC_ROOT_ITEM R"( }
#d13a { width: 60px; }
#d13b { width: 50px; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, kXml, kCss);

    ExpectSize(Own(fx, "d13root"), 300.0f, 100.0f, "d13root");
    ExpectRect(Rel(fx, "d13root", "d13a"), 0.0f, 0.0f, 60.0f, 100.0f, "d13a");
    ExpectRect(Rel(fx, "d13root", "d13b"), 60.0f, 0.0f, 50.0f, 100.0f, "d13b");
}
