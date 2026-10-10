// Chrome parity for the wrapping half of flexbox: line breaking, cross-axis
// line placement (align-content), the two gutters, and per-item align-self
// inside a wrapped line.
//
// Every number below is a measurement, not a derivation — either of Chrome or,
// in the one block that says so in capitals, of this engine where it knowingly
// diverges. Ground truth came from REAL Chrome headless, one page per fixture:
//
//     "C:/Program Files/Google/Chrome/Application/chrome.exe" --headless=new
//       --disable-gpu --no-sandbox --no-first-run --user-data-dir=<tmp>
//       --force-device-scale-factor=1 --virtual-time-budget=3000
//       --dump-dom file:///<tmp>/pages/<fixture>.html
//
// One page per fixture rather than one page with prefixed selectors: the CSS
// text below is then byte-identical to what Chrome parsed, with no scoping
// selector and no duplicate ids to change matching. Each page's reset is
// `* { box-sizing: border-box; margin: 0; padding: 0; border: 0 solid
// transparent }` — border-box is the only box model this engine has (Yoga's
// width is always the border box), so a content-box fixture would be comparing
// two different questions. The pages carry no text, which keeps them clear of
// the known HalfLeadingPx text-ink rounding.
//
// Each container restates flex-direction, flex-wrap, justify-content,
// align-items, align-content, row-gap and column-gap, and each item restates
// flex-grow, flex-shrink and flex-basis. Yoga's native defaults are not CSS's
// (column vs row, shrink 0 vs 1), so a fixture that leans on either side's
// default is testing the default, not the feature.
//
// The same 20 pages measured at --force-device-scale-factor=2 return
// byte-identical CSS-px rects: 0 differing fields over 628. window.devicePixelRatio
// read back 1 and 2 respectively, which is what proves the flag took effect
// rather than being silently ignored. That invariance is what makes the
// contentScale 2.0 assertions below well-posed — the engine lays out in logical
// px, so a scale change must not move a logical rect.
//
// Rects are compared relative to the container's border-box origin, which is
// exactly what getBoundingClientRect differences give on the Chrome side.
//
// Shared content shape (widths x heights), reused by the align-content and gap
// families so that a line-assignment change shows up as a y difference:
//   i0 120x40  i1 90x25  i2 80x55  i3 110x30  i4 70x45  i5 60x20  i6 100x35
// In a 300px-wide container with no gutter that packs as [i0 i1 i2] (290),
// [i3 i4 i5] (240), [i6] (100) — three lines of cross size 55, 45, 35.

#include "IsolatedUIFixture.h"

#include "UI/UIElement.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>

using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

// One child's border box, relative to its flex container's border-box origin.
struct ItemRect
{
    const char* Id;
    float X;
    float Y;
    float W;
    float H;
};

constexpr char kXml7[] = R"(<uielement id="root">
  <uielement id="c">
    <uielement id="i0" class="it"/>
    <uielement id="i1" class="it"/>
    <uielement id="i2" class="it"/>
    <uielement id="i3" class="it"/>
    <uielement id="i4" class="it"/>
    <uielement id="i5" class="it"/>
    <uielement id="i6" class="it"/>
  </uielement>
</uielement>)";

constexpr char kXml6J[] = R"(<uielement id="root">
  <uielement id="c">
    <uielement id="j0" class="it"/>
    <uielement id="j1" class="it"/>
    <uielement id="j2" class="it"/>
    <uielement id="j3" class="it"/>
    <uielement id="j4" class="it"/>
    <uielement id="j5" class="it"/>
  </uielement>
</uielement>)";

constexpr char kXml6K[] = R"(<uielement id="root">
  <uielement id="c">
    <uielement id="k0" class="it"/>
    <uielement id="k1" class="it"/>
    <uielement id="k2" class="it"/>
    <uielement id="k3" class="it"/>
    <uielement id="k4" class="it"/>
    <uielement id="k5" class="it"/>
  </uielement>
</uielement>)";

// Everything except the container rule, which each fixture supplies whole. The
// varying properties are never split across two #c blocks: this engine's
// cascade is source order, not specificity, so a second #c block would be a
// second variable in every test.
constexpr char kCssCommon7[] = R"(
#root { display: flex; flex-direction: column; flex-wrap: nowrap;
        justify-content: flex-start; align-items: flex-start; align-content: flex-start;
        flex-grow: 0; flex-shrink: 0; flex-basis: auto;
        width: 800px; height: 600px; }
.it { flex-grow: 0; flex-shrink: 0; flex-basis: auto; }
#i0 { width: 120px; height: 40px; }
#i1 { width:  90px; height: 25px; }
#i2 { width:  80px; height: 55px; }
#i3 { width: 110px; height: 30px; }
#i4 { width:  70px; height: 45px; }
#i5 { width:  60px; height: 20px; }
#i6 { width: 100px; height: 35px; }
)";

std::string Css7(const std::string& containerRule)
{
    return std::string(kCssCommon7) + containerRule;
}

// Container rules for the seven-item shape. Only the property under test moves
// between them; every other flex property is restated so a fixture never reads
// a value from the one above it.
std::string Container7(const char* wrap, const char* alignContent, const char* rowGap,
                       const char* columnGap)
{
    return std::string("#c { display: flex; flex-direction: row; justify-content: flex-start; "
                       "align-items: flex-start; flex-wrap: ") +
           wrap + "; align-content: " + alignContent + "; row-gap: " + rowGap +
           "; column-gap: " + columnGap +
           "; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 300px; height: 195px; }";
}

// The same container written with the `gap` shorthand instead of the longhand
// pair. Kept separate so a shorthand-expansion defect cannot hide behind a
// longhand that happens to be set too.
std::string Container7Shorthand(const char* alignContent, const char* gapValue)
{
    return std::string("#c { display: flex; flex-direction: row; justify-content: flex-start; "
                       "align-items: flex-start; flex-wrap: wrap; align-content: ") +
           alignContent + "; gap: " + gapValue +
           "; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 300px; height: 195px; }";
}

// Compares every child of `containerId` against Chrome. Non-fatal by design:
// one wrong line assignment moves several items, and seeing all of them is what
// separates "the third line is misplaced" from "the break points differ".
void ExpectRects(const IsolatedUIFixture& fx, const char* containerId,
                 std::initializer_list<ItemRect> expected)
{
    const GameEngine::UIElement* container = fx.Element(containerId);
    ASSERT_NE(container, nullptr) << "container '" << containerId << "' missing";
    const float ox = container->GetLayoutX();
    const float oy = container->GetLayoutY();

    for (const ItemRect& want : expected)
    {
        const GameEngine::UIElement* item = fx.Element(want.Id);
        ASSERT_NE(item, nullptr) << "item '" << want.Id << "' missing";
        EXPECT_FLOAT_EQ(item->GetLayoutX() - ox, want.X) << want.Id << ".x";
        EXPECT_FLOAT_EQ(item->GetLayoutY() - oy, want.Y) << want.Id << ".y";
        EXPECT_FLOAT_EQ(item->GetLayoutWidth(), want.W) << want.Id << ".w";
        EXPECT_FLOAT_EQ(item->GetLayoutHeight(), want.H) << want.Id << ".h";
    }
}

// Build-or-skip, so a host without a Vulkan device skips instead of failing and
// an XML/CSS typo fails instead of silently passing on zero assertions.
#define GE_BUILD_OR_SKIP(fx, scale, xml, css)                                                      \
    do                                                                                             \
    {                                                                                              \
        if (!(fx).Build((scale), (xml), (css)))                                                    \
        {                                                                                          \
            if (!(fx).DeviceAvailable())                                                           \
                GTEST_SKIP() << (fx).Diagnostic();                                                 \
            FAIL() << (fx).Diagnostic();                                                           \
        }                                                                                          \
    } while (false)

// ---------------------------------------------------------------------------
// align-content family. Identical content and identical container box; the
// seven fixtures differ in one keyword and Chrome gives seven distinct sets of
// line origins (0/55/100, 30/85/130, 0/85/160, 0/75/140, 10/85/150, 15/85/145,
// 60/115/160), so a test that reported the same numbers for two of them could
// not be passing by accident.
// ---------------------------------------------------------------------------

constexpr std::initializer_list<ItemRect> kAcFlexStart = {
    {"i0", 0.0f, 0.0f, 120.0f, 40.0f},   {"i1", 120.0f, 0.0f, 90.0f, 25.0f},
    {"i2", 210.0f, 0.0f, 80.0f, 55.0f},  {"i3", 0.0f, 55.0f, 110.0f, 30.0f},
    {"i4", 110.0f, 55.0f, 70.0f, 45.0f}, {"i5", 180.0f, 55.0f, 60.0f, 20.0f},
    {"i6", 0.0f, 100.0f, 100.0f, 35.0f}};

constexpr std::initializer_list<ItemRect> kAcCenter = {
    {"i0", 0.0f, 30.0f, 120.0f, 40.0f},  {"i1", 120.0f, 30.0f, 90.0f, 25.0f},
    {"i2", 210.0f, 30.0f, 80.0f, 55.0f}, {"i3", 0.0f, 85.0f, 110.0f, 30.0f},
    {"i4", 110.0f, 85.0f, 70.0f, 45.0f}, {"i5", 180.0f, 85.0f, 60.0f, 20.0f},
    {"i6", 0.0f, 130.0f, 100.0f, 35.0f}};

constexpr std::initializer_list<ItemRect> kAcSpaceBetween = {
    {"i0", 0.0f, 0.0f, 120.0f, 40.0f},   {"i1", 120.0f, 0.0f, 90.0f, 25.0f},
    {"i2", 210.0f, 0.0f, 80.0f, 55.0f},  {"i3", 0.0f, 85.0f, 110.0f, 30.0f},
    {"i4", 110.0f, 85.0f, 70.0f, 45.0f}, {"i5", 180.0f, 85.0f, 60.0f, 20.0f},
    {"i6", 0.0f, 160.0f, 100.0f, 35.0f}};

// The LINES, not the items, take the 60px of free cross space: 20px each, so
// the line origins move to 0/75/140 while every item keeps its declared height.
constexpr std::initializer_list<ItemRect> kAcStretch = {
    {"i0", 0.0f, 0.0f, 120.0f, 40.0f},   {"i1", 120.0f, 0.0f, 90.0f, 25.0f},
    {"i2", 210.0f, 0.0f, 80.0f, 55.0f},  {"i3", 0.0f, 75.0f, 110.0f, 30.0f},
    {"i4", 110.0f, 75.0f, 70.0f, 45.0f}, {"i5", 180.0f, 75.0f, 60.0f, 20.0f},
    {"i6", 0.0f, 140.0f, 100.0f, 35.0f}};

constexpr std::initializer_list<ItemRect> kAcSpaceAround = {
    {"i0", 0.0f, 10.0f, 120.0f, 40.0f},  {"i1", 120.0f, 10.0f, 90.0f, 25.0f},
    {"i2", 210.0f, 10.0f, 80.0f, 55.0f}, {"i3", 0.0f, 85.0f, 110.0f, 30.0f},
    {"i4", 110.0f, 85.0f, 70.0f, 45.0f}, {"i5", 180.0f, 85.0f, 60.0f, 20.0f},
    {"i6", 0.0f, 150.0f, 100.0f, 35.0f}};

constexpr std::initializer_list<ItemRect> kAcSpaceEvenly = {
    {"i0", 0.0f, 15.0f, 120.0f, 40.0f},  {"i1", 120.0f, 15.0f, 90.0f, 25.0f},
    {"i2", 210.0f, 15.0f, 80.0f, 55.0f}, {"i3", 0.0f, 85.0f, 110.0f, 30.0f},
    {"i4", 110.0f, 85.0f, 70.0f, 45.0f}, {"i5", 180.0f, 85.0f, 60.0f, 20.0f},
    {"i6", 0.0f, 145.0f, 100.0f, 35.0f}};

constexpr std::initializer_list<ItemRect> kAcFlexEnd = {
    {"i0", 0.0f, 60.0f, 120.0f, 40.0f},   {"i1", 120.0f, 60.0f, 90.0f, 25.0f},
    {"i2", 210.0f, 60.0f, 80.0f, 55.0f},  {"i3", 0.0f, 115.0f, 110.0f, 30.0f},
    {"i4", 110.0f, 115.0f, 70.0f, 45.0f}, {"i5", 180.0f, 115.0f, 60.0f, 20.0f},
    {"i6", 0.0f, 160.0f, 100.0f, 35.0f}};

} // namespace

TEST(FlexWrapChromeParity, AlignContentFlexStart)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml7, Css7(Container7("wrap", "flex-start", "0px", "0px")));
    ExpectRects(fx, "c", kAcFlexStart);
}

TEST(FlexWrapChromeParity, AlignContentCenter)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml7, Css7(Container7("wrap", "center", "0px", "0px")));
    ExpectRects(fx, "c", kAcCenter);
}

TEST(FlexWrapChromeParity, AlignContentSpaceBetween)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml7, Css7(Container7("wrap", "space-between", "0px", "0px")));
    ExpectRects(fx, "c", kAcSpaceBetween);
}

TEST(FlexWrapChromeParity, AlignContentStretch)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml7, Css7(Container7("wrap", "stretch", "0px", "0px")));
    ExpectRects(fx, "c", kAcStretch);
}

TEST(FlexWrapChromeParity, AlignContentSpaceAround)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml7, Css7(Container7("wrap", "space-around", "0px", "0px")));
    ExpectRects(fx, "c", kAcSpaceAround);
}

TEST(FlexWrapChromeParity, AlignContentSpaceEvenly)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml7, Css7(Container7("wrap", "space-evenly", "0px", "0px")));
    ExpectRects(fx, "c", kAcSpaceEvenly);
}

TEST(FlexWrapChromeParity, AlignContentFlexEnd)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml7, Css7(Container7("wrap", "flex-end", "0px", "0px")));
    ExpectRects(fx, "c", kAcFlexEnd);
}

// The control for the whole family: same content, same container, nowrap. If a
// wrap fixture and this one ever agree, the wrap fixture stopped wrapping. All
// seven items are asserted, including the four that overflow the container to
// the right — with flex-shrink 0 nothing may shrink to make them fit.
TEST(FlexWrapChromeParity, NowrapControlKeepsOneLine)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml7, Css7(Container7("nowrap", "flex-start", "0px", "0px")));
    ExpectRects(fx, "c",
                {{"i0", 0.0f, 0.0f, 120.0f, 40.0f},
                 {"i1", 120.0f, 0.0f, 90.0f, 25.0f},
                 {"i2", 210.0f, 0.0f, 80.0f, 55.0f},
                 {"i3", 290.0f, 0.0f, 110.0f, 30.0f},
                 {"i4", 400.0f, 0.0f, 70.0f, 45.0f},
                 {"i5", 470.0f, 0.0f, 60.0f, 20.0f},
                 {"i6", 530.0f, 0.0f, 100.0f, 35.0f}});
}

// ---------------------------------------------------------------------------
// Gaps. column-gap changes which items share a line, so these fixtures assert
// line assignment as much as spacing: at 20px the first line loses i2 (120+20+
// 90+20+80 = 330 > 300) and the second line takes exactly 300 (80+20+110+20+70),
// which pins the break comparison as inclusive on both sides.
// ---------------------------------------------------------------------------

TEST(FlexGapChromeParity, ColumnGapOnlyRepacksLines)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml7, Css7(Container7("wrap", "flex-start", "0px", "20px")));
    ExpectRects(fx, "c",
                {{"i0", 0.0f, 0.0f, 120.0f, 40.0f},
                 {"i1", 140.0f, 0.0f, 90.0f, 25.0f},
                 {"i2", 0.0f, 40.0f, 80.0f, 55.0f},
                 {"i3", 100.0f, 40.0f, 110.0f, 30.0f},
                 {"i4", 230.0f, 40.0f, 70.0f, 45.0f},
                 {"i5", 0.0f, 95.0f, 60.0f, 20.0f},
                 {"i6", 80.0f, 95.0f, 100.0f, 35.0f}});
}

TEST(FlexGapChromeParity, RowGapOnlySpacesLinesWithoutRepacking)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml7, Css7(Container7("wrap", "flex-start", "12px", "0px")));
    // Same line assignment as the zero-gutter fixture, lines pushed to 0/67/124.
    ExpectRects(fx, "c",
                {{"i0", 0.0f, 0.0f, 120.0f, 40.0f},
                 {"i1", 120.0f, 0.0f, 90.0f, 25.0f},
                 {"i2", 210.0f, 0.0f, 80.0f, 55.0f},
                 {"i3", 0.0f, 67.0f, 110.0f, 30.0f},
                 {"i4", 110.0f, 67.0f, 70.0f, 45.0f},
                 {"i5", 180.0f, 67.0f, 60.0f, 20.0f},
                 {"i6", 0.0f, 124.0f, 100.0f, 35.0f}});
}

// `gap: 16px` must reach both gutters, not just one. The two axes are asserted
// together because a shorthand that only landed on RowGap would still produce
// the right y values.
TEST(FlexGapChromeParity, GapShorthandSetsBothGutters)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml7, Css7(Container7Shorthand("flex-start", "16px")));
    ExpectRects(fx, "c",
                {{"i0", 0.0f, 0.0f, 120.0f, 40.0f},
                 {"i1", 136.0f, 0.0f, 90.0f, 25.0f},
                 {"i2", 0.0f, 56.0f, 80.0f, 55.0f},
                 {"i3", 96.0f, 56.0f, 110.0f, 30.0f},
                 {"i4", 222.0f, 56.0f, 70.0f, 45.0f},
                 {"i5", 0.0f, 127.0f, 60.0f, 20.0f},
                 {"i6", 76.0f, 127.0f, 100.0f, 35.0f}});
}

// Different gutters on the two axes: 24px between lines, 10px between items.
// Written as the two longhands, which is the form the engine models.
TEST(FlexGapChromeParity, RowAndColumnGapDifferPerAxis)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml7, Css7(Container7("wrap", "flex-start", "24px", "10px")));
    ExpectRects(fx, "c",
                {{"i0", 0.0f, 0.0f, 120.0f, 40.0f},
                 {"i1", 130.0f, 0.0f, 90.0f, 25.0f},
                 {"i2", 0.0f, 64.0f, 80.0f, 55.0f},
                 {"i3", 90.0f, 64.0f, 110.0f, 30.0f},
                 {"i4", 210.0f, 64.0f, 70.0f, 45.0f},
                 {"i5", 0.0f, 143.0f, 60.0f, 20.0f},
                 {"i6", 70.0f, 143.0f, 100.0f, 35.0f}});
}

// Gap combined with free-space distribution. Chrome puts the two remaining
// lines at 72.5 and 160: 195 - (40+55+35) - 2*16 = 33 of free space over two
// slots is 16.5 each, a half pixel that no integer arithmetic produces by
// accident, so this is the fixture that would catch a solve that rounded line
// origins to whole logical pixels.
TEST(FlexGapChromeParity, GapWithSpaceBetweenProducesHalfPixelLineOrigins)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml7, Css7(Container7Shorthand("space-between", "16px")));
    ExpectRects(fx, "c",
                {{"i0", 0.0f, 0.0f, 120.0f, 40.0f},
                 {"i1", 136.0f, 0.0f, 90.0f, 25.0f},
                 {"i2", 0.0f, 72.5f, 80.0f, 55.0f},
                 {"i3", 96.0f, 72.5f, 110.0f, 30.0f},
                 {"i4", 222.0f, 72.5f, 70.0f, 45.0f},
                 {"i5", 0.0f, 160.0f, 60.0f, 20.0f},
                 {"i6", 76.0f, 160.0f, 100.0f, 35.0f}});
}

// ---------------------------------------------------------------------------
// Column direction. CSS row-gap is the gutter between items ALONG the main axis
// and column-gap the gutter between lines, so a column container swaps which
// physical axis each one controls. A gutter-axis mix-up survives every fixture
// above and dies here: with 15/25 the container packs four columns, with 0/0 it
// packs three, so the line assignment itself differs.
// ---------------------------------------------------------------------------

namespace
{
std::string ContainerCol(const char* rowGap, const char* columnGap)
{
    return std::string("#c { display: flex; flex-direction: column; justify-content: flex-start; "
                       "align-items: flex-start; flex-wrap: wrap; align-content: flex-start; "
                       "row-gap: ") +
           rowGap + "; column-gap: " + columnGap +
           "; flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 500px; height: 120px; }";
}
} // namespace

TEST(FlexGapChromeParity, ColumnDirectionMapsEachGutterToItsOwnAxis)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml7, Css7(ContainerCol("15px", "25px")));
    ExpectRects(fx, "c",
                {{"i0", 0.0f, 0.0f, 120.0f, 40.0f},
                 {"i1", 0.0f, 55.0f, 90.0f, 25.0f},
                 {"i2", 145.0f, 0.0f, 80.0f, 55.0f},
                 {"i3", 145.0f, 70.0f, 110.0f, 30.0f},
                 {"i4", 280.0f, 0.0f, 70.0f, 45.0f},
                 {"i5", 280.0f, 60.0f, 60.0f, 20.0f},
                 {"i6", 375.0f, 0.0f, 100.0f, 35.0f}});
}

TEST(FlexGapChromeParity, ColumnDirectionWrapsWithoutGutters)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml7, Css7(ContainerCol("0px", "0px")));
    ExpectRects(fx, "c",
                {{"i0", 0.0f, 0.0f, 120.0f, 40.0f},
                 {"i1", 0.0f, 40.0f, 90.0f, 25.0f},
                 {"i2", 0.0f, 65.0f, 80.0f, 55.0f},
                 {"i3", 120.0f, 0.0f, 110.0f, 30.0f},
                 {"i4", 120.0f, 30.0f, 70.0f, 45.0f},
                 {"i5", 120.0f, 75.0f, 60.0f, 20.0f},
                 {"i6", 230.0f, 0.0f, 100.0f, 35.0f}});
}

// ---------------------------------------------------------------------------
// align-self inside a wrapped line. The LINE, not the container, is the box an
// item aligns against, so j1/j2 align inside line 1's 60px and j5 inside line
// 2's 50px. j3 is `height: auto; align-self: stretch` and must come out at the
// line's cross size (50), not at 0 and not at the container height.
// ---------------------------------------------------------------------------

namespace
{

constexpr char kCssCommon6[] = R"(
#root { display: flex; flex-direction: column; flex-wrap: nowrap;
        justify-content: flex-start; align-items: flex-start; align-content: flex-start;
        flex-grow: 0; flex-shrink: 0; flex-basis: auto;
        width: 800px; height: 600px; }
.it { flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 100px; }
#j0 { height:  60px; align-self: auto; }
#j1 { height:  20px; align-self: center; }
#j2 { height:  20px; align-self: flex-end; }
#j3 { height: auto;  align-self: stretch; }
#j4 { height:  50px; align-self: auto; }
#j5 { height:  20px; align-self: flex-start; }
)";

// Same six items, but the container aligns center, so the two `auto` items
// (j0, j4) are the only ones whose y is allowed to move between the fixtures.
constexpr char kCssCommon6Center[] = R"(
#root { display: flex; flex-direction: column; flex-wrap: nowrap;
        justify-content: flex-start; align-items: flex-start; align-content: flex-start;
        flex-grow: 0; flex-shrink: 0; flex-basis: auto;
        width: 800px; height: 600px; }
.it { flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 100px; }
#j0 { height:  60px; align-self: auto; }
#j1 { height:  20px; align-self: flex-start; }
#j2 { height:  20px; align-self: flex-end; }
#j3 { height: auto;  align-self: stretch; }
#j4 { height:  50px; align-self: auto; }
#j5 { height:  20px; align-self: center; }
)";

// Heights are `auto` on k1/k3/k5, so the container's align-items actually moves
// something: with every height definite, `stretch` and `flex-start` produce
// identical rects and the pair would not discriminate.
constexpr char kCssCommonK[] = R"(
#root { display: flex; flex-direction: column; flex-wrap: nowrap;
        justify-content: flex-start; align-items: flex-start; align-content: flex-start;
        flex-grow: 0; flex-shrink: 0; flex-basis: auto;
        width: 800px; height: 600px; }
.it { flex-grow: 0; flex-shrink: 0; flex-basis: auto; width: 100px; }
#k0 { height:  60px; align-self: auto; }
#k1 { height: auto;  align-self: auto; }
#k2 { height:  20px; align-self: flex-end; }
#k3 { height: auto;  align-self: flex-start; }
#k4 { height:  50px; align-self: auto; }
#k5 { height: auto;  align-self: auto; }
)";

std::string AlignSelfCss(const char* common, const char* alignItems)
{
    return std::string(common) +
           "#c { display: flex; flex-direction: row; justify-content: flex-start; "
           "flex-wrap: wrap; align-content: flex-start; align-items: " +
           alignItems +
           "; row-gap: 0px; column-gap: 0px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; "
           "width: 320px; height: 195px; }\n";
}

} // namespace

TEST(FlexWrapChromeParity, AlignSelfResolvesAgainstItsOwnLine)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml6J, AlignSelfCss(kCssCommon6, "flex-start"));
    ExpectRects(fx, "c",
                {{"j0", 0.0f, 0.0f, 100.0f, 60.0f},
                 {"j1", 100.0f, 20.0f, 100.0f, 20.0f},
                 {"j2", 200.0f, 40.0f, 100.0f, 20.0f},
                 {"j3", 0.0f, 60.0f, 100.0f, 50.0f},
                 {"j4", 100.0f, 60.0f, 100.0f, 50.0f},
                 {"j5", 200.0f, 60.0f, 100.0f, 20.0f}});
}

TEST(FlexWrapChromeParity, AlignSelfAutoFollowsAlignItemsCenter)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml6J, AlignSelfCss(kCssCommon6Center, "center"));
    ExpectRects(fx, "c",
                {{"j0", 0.0f, 0.0f, 100.0f, 60.0f},
                 {"j1", 100.0f, 0.0f, 100.0f, 20.0f},
                 {"j2", 200.0f, 40.0f, 100.0f, 20.0f},
                 {"j3", 0.0f, 60.0f, 100.0f, 50.0f},
                 {"j4", 100.0f, 60.0f, 100.0f, 50.0f},
                 {"j5", 200.0f, 75.0f, 100.0f, 20.0f}});
}

// The stretch/flex-start pair. k1 and k5 are the discriminators: `auto` height
// under `stretch` takes the LINE's cross size (60 and 50), under `flex-start`
// it stays at the content height, which for an empty box is 0. k3 pins its own
// align-self against both containers.
TEST(FlexWrapChromeParity, AlignItemsStretchFillsEachLineCrossSize)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml6K, AlignSelfCss(kCssCommonK, "stretch"));
    ExpectRects(fx, "c",
                {{"k0", 0.0f, 0.0f, 100.0f, 60.0f},
                 {"k1", 100.0f, 0.0f, 100.0f, 60.0f},
                 {"k2", 200.0f, 40.0f, 100.0f, 20.0f},
                 {"k3", 0.0f, 60.0f, 100.0f, 0.0f},
                 {"k4", 100.0f, 60.0f, 100.0f, 50.0f},
                 {"k5", 200.0f, 60.0f, 100.0f, 50.0f}});
}

TEST(FlexWrapChromeParity, AlignItemsFlexStartLeavesAutoHeightsAtContent)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml6K, AlignSelfCss(kCssCommonK, "flex-start"));
    ExpectRects(fx, "c",
                {{"k0", 0.0f, 0.0f, 100.0f, 60.0f},
                 {"k1", 100.0f, 0.0f, 100.0f, 0.0f},
                 {"k2", 200.0f, 40.0f, 100.0f, 20.0f},
                 {"k3", 0.0f, 60.0f, 100.0f, 0.0f},
                 {"k4", 100.0f, 60.0f, 100.0f, 50.0f},
                 {"k5", 200.0f, 60.0f, 100.0f, 0.0f}});
}

// ---------------------------------------------------------------------------
// wrap-reverse — PINNED AT THE ENGINE'S VALUE, NOT CHROME'S.
//
// css-flexbox-1 §5.2: `wrap-reverse` reverses the CROSS axis. Lines stack from
// the container's bottom edge, `align-content: flex-start` packs them against
// that edge, and `align-items: flex-start` puts an item at the BOTTOM of its
// line. This engine reverses nothing: `wrap-reverse` lays out as plain `wrap`.
//
// The chain, all three layers verified at this commit:
//   ResolvedStyle.h:46            `bool FlexWrap = false` — one bit, so there is
//                                 no third state to carry the reversal.
//   CSSValueParsers.cpp:903-906   ParseFlexWrapValue returns `wrap ||
//                                 wrap-reverse`, collapsing the two.
//   YogaLayout.cpp:167            YGWrapWrap or YGWrapNoWrap; YGWrapWrapReverse
//                                 is never passed, though the vendored Yoga
//                                 declares it (yoga/enums/Wrap.h).
// CSSKeywordParsingTests.cpp:221 already pins the parser half of this on
// purpose (`EXPECT_TRUE(... .Layout.FlexWrap)` — it asserts the collapse); the
// three fixtures below pin what the collapse costs in geometry.
//
// WHY ACTIVE AND NOT DISABLED_. Same convention as
// PercentBorderRadiusTests.cpp:11-17: with no CI, a DISABLED_ test is a test
// that never runs again, so the day this changes nothing would report it. Each
// expectation below is the value the engine produces today; the comment above
// it carries the Chrome measurement it ought to produce. A fix MUST flip these
// three, and the failure is the report.
//
// WHY IT IS NOT FIXED HERE. FlexWrap is a bool across the style struct, the
// cascade, the CSS parser, StyleInterpolation and the Yoga adapter. Widening it
// to a tri-state enum is a style-model change with its own callers to migrate,
// not a parser fix, and it belongs to whoever owns that migration.
// ---------------------------------------------------------------------------

// Chrome, same CSS: y = 155 / 170 / 140 for the first line, 110 / 95 / 120 for
// the second, 60 for i6 — the reverse order, measured off the bottom edge. The
// engine's y values below are byte-identical to the plain-`wrap` fixture
// (AlignContentFlexStart), which is the defect stated as precisely as it can
// be: the keyword changes nothing.
TEST(FlexWrapChromeParity, WrapReverseLaysOutAsPlainWrap)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml7, Css7(Container7("wrap-reverse", "flex-start", "0px", "0px")));
    ExpectRects(fx, "c",
                {{"i0", 0.0f, 0.0f, 120.0f, 40.0f},
                 {"i1", 120.0f, 0.0f, 90.0f, 25.0f},
                 {"i2", 210.0f, 0.0f, 80.0f, 55.0f},
                 {"i3", 0.0f, 55.0f, 110.0f, 30.0f},
                 {"i4", 110.0f, 55.0f, 70.0f, 45.0f},
                 {"i5", 180.0f, 55.0f, 60.0f, 20.0f},
                 {"i6", 0.0f, 100.0f, 100.0f, 35.0f}});
}

// Kept separate from the flex-start case because a partial fix — reversing line
// order but not the per-line alignment, or the reverse — would flip one of the
// two and leave the other pinned.
//
// Chrome, same CSS: y = 155 / 170 / 140, then 90 / 75 / 100, then 20. The
// stretched lines are 20px taller each in both engines; only their order and
// the within-line direction differ.
TEST(FlexWrapChromeParity, WrapReverseUnderAlignContentStretchLaysOutAsPlainWrap)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml7, Css7(Container7("wrap-reverse", "stretch", "0px", "0px")));
    ExpectRects(fx, "c",
                {{"i0", 0.0f, 0.0f, 120.0f, 40.0f},
                 {"i1", 120.0f, 0.0f, 90.0f, 25.0f},
                 {"i2", 210.0f, 0.0f, 80.0f, 55.0f},
                 {"i3", 0.0f, 75.0f, 110.0f, 30.0f},
                 {"i4", 110.0f, 75.0f, 70.0f, 45.0f},
                 {"i5", 180.0f, 75.0f, 60.0f, 20.0f},
                 {"i6", 0.0f, 140.0f, 100.0f, 35.0f}});
}

// The gutters are the part that already survives: x is identical in both
// engines here, because reversing the cross axis leaves the main axis alone.
//
// Chrome, same CSS: y = 155 / 170, then 76 / 101 / 86, then 32 / 17. Every x
// below is Chrome's value unchanged, so this fixture goes red on a wrap-reverse
// fix in the y column only — and would go red on a gutter-axis regression in
// the x column whether or not wrap-reverse is ever implemented.
TEST(FlexWrapChromeParity, WrapReverseWithGuttersLaysOutAsPlainWrap)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml7, Css7(Container7("wrap-reverse", "flex-start", "24px", "10px")));
    ExpectRects(fx, "c",
                {{"i0", 0.0f, 0.0f, 120.0f, 40.0f},
                 {"i1", 130.0f, 0.0f, 90.0f, 25.0f},
                 {"i2", 0.0f, 64.0f, 80.0f, 55.0f},
                 {"i3", 90.0f, 64.0f, 110.0f, 30.0f},
                 {"i4", 210.0f, 64.0f, 70.0f, 45.0f},
                 {"i5", 0.0f, 143.0f, 60.0f, 20.0f},
                 {"i6", 70.0f, 143.0f, 100.0f, 35.0f}});
}

// ---------------------------------------------------------------------------
// `gap: <row> <column>` — css-align-3 §8, Gaps Between Boxes: `gap` is a
// shorthand for `row-gap column-gap`, and with two values the second sets the
// column gutter. ExpandGapValue (CSSValueParsers.cpp) emits the two longhands,
// each token keeping its own unit; the mixed-unit forms are pinned in
// PercentageUnitsFontGapTests.cpp.
// ---------------------------------------------------------------------------

// Chrome, same CSS — byte-identical to RowAndColumnGapDifferPerAxis, since the
// shorthand and the longhand pair describe the same layout. i4 is the sharpest
// witness against the fanned-first 24px/24px layout this engine used to
// produce, which put it on a third line at y 143 instead of (210, 64).
TEST(FlexGapChromeParity, GapShorthandTwoValuesSetRowThenColumnGutter)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 1.0f, kXml7, Css7(Container7Shorthand("flex-start", "24px 10px")));
    ExpectRects(fx, "c",
                {{"i0", 0.0f, 0.0f, 120.0f, 40.0f},
                 {"i1", 130.0f, 0.0f, 90.0f, 25.0f},
                 {"i2", 0.0f, 64.0f, 80.0f, 55.0f},
                 {"i3", 90.0f, 64.0f, 110.0f, 30.0f},
                 {"i4", 210.0f, 64.0f, 70.0f, 45.0f},
                 {"i5", 0.0f, 143.0f, 60.0f, 20.0f},
                 {"i6", 70.0f, 143.0f, 100.0f, 35.0f}});
}

// The shorthand and the longhand pair must be the same layout element for
// element, so an expansion defect cannot drift from the form the fixture above
// measures against Chrome.
TEST(FlexGapChromeParity, GapShorthandTwoValuesMatchesTheLonghandPair)
{
    IsolatedUIFixture shorthand;
    GE_BUILD_OR_SKIP(shorthand, 1.0f, kXml7, Css7(Container7Shorthand("flex-start", "24px 10px")));
    IsolatedUIFixture longhands;
    GE_BUILD_OR_SKIP(longhands, 1.0f, kXml7, Css7(Container7("wrap", "flex-start", "24px", "10px")));

    for (const char* id : {"i0", "i1", "i2", "i3", "i4", "i5", "i6"})
    {
        const GameEngine::UIElement* a = shorthand.Element(id);
        const GameEngine::UIElement* b = longhands.Element(id);
        ASSERT_NE(a, nullptr) << id;
        ASSERT_NE(b, nullptr) << id;
        EXPECT_FLOAT_EQ(a->GetLayoutX(), b->GetLayoutX()) << id << ".x";
        EXPECT_FLOAT_EQ(a->GetLayoutY(), b->GetLayoutY()) << id << ".y";
    }

    // ...and not the fanned-first layout: with the 10px column gutter i1 sits
    // at 130, where a 24px gutter put it at 144.
    const GameEngine::UIElement* i1 = shorthand.Element("i1");
    ASSERT_NE(i1, nullptr);
    EXPECT_FLOAT_EQ(i1->GetLayoutX() - shorthand.Element("c")->GetLayoutX(), 130.0f);
}

// ---------------------------------------------------------------------------
// contentScale 2.0. Chrome's CSS-px rects are identical at device scale 1 and 2
// (measured, see the file header), and this engine lays out in logical px, so
// the logical rects must not move either. The physical border box is checked
// alongside so a regression that scaled the layout instead of the emission
// cannot hide behind matching logical numbers.
// ---------------------------------------------------------------------------

TEST(FlexWrapChromeParity, WrapAndGapsAreScaleInvariantAtContentScale2)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 2.0f, kXml7, Css7(Container7("wrap", "flex-start", "24px", "10px")));
    ExpectRects(fx, "c",
                {{"i0", 0.0f, 0.0f, 120.0f, 40.0f},
                 {"i1", 130.0f, 0.0f, 90.0f, 25.0f},
                 {"i2", 0.0f, 64.0f, 80.0f, 55.0f},
                 {"i3", 90.0f, 64.0f, 110.0f, 30.0f},
                 {"i4", 210.0f, 64.0f, 70.0f, 45.0f},
                 {"i5", 0.0f, 143.0f, 60.0f, 20.0f},
                 {"i6", 70.0f, 143.0f, 100.0f, 35.0f}});

    const GameEngine::UITesting::PhysicalRect container = fx.BorderBox("c");
    const GameEngine::UITesting::PhysicalRect last = fx.BorderBox("i6");
    EXPECT_FLOAT_EQ(container.W, 600.0f);
    EXPECT_FLOAT_EQ(container.H, 390.0f);
    EXPECT_FLOAT_EQ(last.X - container.X, 140.0f);
    EXPECT_FLOAT_EQ(last.Y - container.Y, 286.0f);
    EXPECT_FLOAT_EQ(last.W, 200.0f);
    EXPECT_FLOAT_EQ(last.H, 70.0f);
}

TEST(FlexWrapChromeParity, AlignContentStretchIsScaleInvariantAtContentScale2)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 2.0f, kXml7, Css7(Container7("wrap", "stretch", "0px", "0px")));
    ExpectRects(fx, "c", kAcStretch);
}

TEST(FlexWrapChromeParity, AlignSelfIsScaleInvariantAtContentScale2)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 2.0f, kXml6J, AlignSelfCss(kCssCommon6, "flex-start"));
    ExpectRects(fx, "c",
                {{"j0", 0.0f, 0.0f, 100.0f, 60.0f},
                 {"j1", 100.0f, 20.0f, 100.0f, 20.0f},
                 {"j2", 200.0f, 40.0f, 100.0f, 20.0f},
                 {"j3", 0.0f, 60.0f, 100.0f, 50.0f},
                 {"j4", 100.0f, 60.0f, 100.0f, 50.0f},
                 {"j5", 200.0f, 60.0f, 100.0f, 20.0f}});
}

// The half-pixel line origin at scale 2 is the case where a logical-space
// rounding error would be doubled rather than absorbed: 72.5 logical is 145
// physical, an odd number no whole-logical-pixel solve can produce.
TEST(FlexGapChromeParity, HalfPixelLineOriginSurvivesContentScale2)
{
    IsolatedUIFixture fx;
    GE_BUILD_OR_SKIP(fx, 2.0f, kXml7, Css7(Container7Shorthand("space-between", "16px")));
    ExpectRects(fx, "c",
                {{"i0", 0.0f, 0.0f, 120.0f, 40.0f},
                 {"i1", 136.0f, 0.0f, 90.0f, 25.0f},
                 {"i2", 0.0f, 72.5f, 80.0f, 55.0f},
                 {"i3", 96.0f, 72.5f, 110.0f, 30.0f},
                 {"i4", 222.0f, 72.5f, 70.0f, 45.0f},
                 {"i5", 0.0f, 160.0f, 60.0f, 20.0f},
                 {"i6", 76.0f, 160.0f, 100.0f, 35.0f}});

    const GameEngine::UITesting::PhysicalRect container = fx.BorderBox("c");
    const GameEngine::UITesting::PhysicalRect second = fx.BorderBox("i2");
    EXPECT_FLOAT_EQ(second.Y - container.Y, 145.0f);
}
