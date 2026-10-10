// `font-size: 150%` and `gap: 5%` (#878), pinned from the outside in.
//
// Both declarations were accepted, kept their number and lost their unit:
// ParseFloatPx returns the numeric prefix only, and these four properties were
// registered with it, so `150%` reached the cascade as the float 150 and
// rendered at 150px while `5%` rendered as 5px. Same family as the percentage
// border-radius (#873) and the per-side border shorthands (#875).
//
// The pins below are laid-out geometry and emitted glyph quads, never a
// resolved-style field: a stored number proves the cascade kept a value, not
// that Yoga positioned a child differently or that a glyph was shaped at a
// different size. Where a percentage has an exact pixel equivalent, the test
// asserts the two arms are EQUAL rather than asserting a literal — a shared
// constant cannot make the percentage arm pass while the layout is wrong.
//
// The two bases are different and the tests keep them apart:
//   gap       - the element's own CONTENT box, in the gutter's axis (css-align-3 §8.1)
//   font-size - the PARENT's computed font size (css-fonts-4 §3.5), and at the
//               root, which has no parent, the engine's initial font-size

#include "IsolatedUIFixture.h"
#include "RobotoTestFont.h"

#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

using namespace GameEngine;
using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::LoadRobotoAtlas;

namespace
{

// Gap resolution is an exact float multiply against an integer content box, and
// text advances are exact float sums, so the two arms of every comparison below
// agree to well under a hundredth of a px when they agree at all. Anything
// looser would let a 25%-read-as-25px arm slip through on a small box.
constexpr float kExactPx = 0.01f;

// Min/max glyph ink over every Slug quad an element emitted, in physical px.
// Labels here are ASCII, so every glyph is a Slug quad.
struct InkBox
{
    float MinX = 1e30f;
    float MaxX = -1e30f;
    float MinY = 1e30f;
    float MaxY = -1e30f;

    float W() const { return MaxX - MinX; }
    float H() const { return MaxY - MinY; }
    bool Empty() const { return MaxX < MinX; }
};

InkBox GlyphInk(const IsolatedUIFixture& fx, const std::string& id)
{
    InkBox box{};
    for (const UI::UIPrimitive& p : fx.Primitives(id, UI::PrimitiveMode::Slug))
    {
        box.MinX = std::min(box.MinX, p.X);
        box.MaxX = std::max(box.MaxX, p.X + p.W);
        box.MinY = std::min(box.MinY, p.Y);
        box.MaxY = std::max(box.MaxY, p.Y + p.H);
    }
    return box;
}

} // namespace

// --- gap: the percentage resolves against the element's own content box ------
//
// #pctrow is 400px wide with no padding, so `gap: 25%` is a 100px gutter and the
// three 40px cells sit at 0 / 140 / 280. #pxrow declares that gutter directly.
// At base the percentage arm laid out with a 25px gutter (cells at 0 / 65 / 130)
// and the two rows disagreed.

TEST(PercentageGap, ColumnGutterIsAPercentageOfTheContentBoxWidth)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <uielement id="pctrow"><uielement id="p0"/><uielement id="p1"/><uielement id="p2"/></uielement>
  <uielement id="pxrow"><uielement id="x0"/><uielement id="x1"/><uielement id="x2"/></uielement>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: column; width: 800px; height: 600px; }
#pctrow, #pxrow { display: flex; flex-direction: row; width: 400px; height: 60px; }
#p0, #p1, #p2, #x0, #x1, #x2 { width: 40px; height: 40px; }
#pctrow { gap: 25%; }
#pxrow  { gap: 100px; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    // The px arm is the reference: if it moves, the fixture changed, not the fix.
    EXPECT_NEAR(fx.BorderBox("x1").X, 140.0f, kExactPx);
    EXPECT_NEAR(fx.BorderBox("x2").X, 280.0f, kExactPx);

    EXPECT_NEAR(fx.BorderBox("p1").X, fx.BorderBox("x1").X, kExactPx);
    EXPECT_NEAR(fx.BorderBox("p2").X, fx.BorderBox("x2").X, kExactPx);
}

// The content box, not the border box: padding shrinks the base. A 400px-wide
// box with 50px of horizontal padding has a 300px content box, so `gap: 25%` is
// 75px and the second cell starts at 50 (padding) + 40 (cell) + 75.

TEST(PercentageGap, PaddingShrinksTheBaseTheGutterResolvesAgainst)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <uielement id="row"><uielement id="c0"/><uielement id="c1"/></uielement>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: column; width: 800px; height: 600px; }
#row { display: flex; flex-direction: row; width: 400px; height: 100px; padding-left: 50px; padding-right: 50px; gap: 25%; }
#c0, #c1 { width: 40px; height: 40px; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    EXPECT_NEAR(fx.BorderBox("c0").X, 50.0f, kExactPx);
    EXPECT_NEAR(fx.BorderBox("c1").X, 50.0f + 40.0f + 75.0f, kExactPx);
}

// row-gap is the cross-axis gutter of a row flex container and the main-axis one
// of a column container. Declaring only `row-gap` must leave the column gutter
// alone: at base both longhands wrote a bare float, so a percentage on one could
// not be told from pixels on the other.

TEST(PercentageGap, RowGapLonghandLeavesTheColumnGutterUntouched)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <uielement id="col"><uielement id="a"/><uielement id="b"/></uielement>
  <uielement id="colpx"><uielement id="pa"/><uielement id="pb"/></uielement>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: row; width: 800px; height: 600px; }
#col, #colpx { display: flex; flex-direction: column; width: 200px; height: 400px; }
#a, #b, #pa, #pb { width: 40px; height: 40px; }
#col   { row-gap: 25%; }
#colpx { row-gap: 100px; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    EXPECT_NEAR(fx.BorderBox("pb").Y, 140.0f, kExactPx);
    EXPECT_NEAR(fx.BorderBox("b").Y, fx.BorderBox("pb").Y, kExactPx);
}

// --- gap: the two-value shorthand keeps each gutter's own unit ---------------
//
// `gap: <row> <column>` (css-align-3 §8). Mixed units are where a whole-string
// unit test fails: percent detected from the LAST character of the declaration
// with the number read from the FIRST turns `10px 25%` into Percent(10) on both
// gutters and `25% 10px` into Px(25). Both fixtures observe both gutters — the
// row container's item gutter is column-gap, the column container's is row-gap —
// against px controls, so a value fanned from the wrong token cannot pass.

TEST(PercentageGap, TwoValueShorthandKeepsEachGuttersOwnUnit)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <uielement id="row"><uielement id="r0"/><uielement id="r1"/><uielement id="r2"/></uielement>
  <uielement id="rowpx"><uielement id="x0"/><uielement id="x1"/><uielement id="x2"/></uielement>
  <uielement id="col"><uielement id="a"/><uielement id="b"/></uielement>
  <uielement id="colpx"><uielement id="pa"/><uielement id="pb"/></uielement>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: row; align-items: flex-start; width: 800px; height: 600px; }
#row, #rowpx { display: flex; flex-direction: row; width: 400px; height: 60px; flex-shrink: 0; }
#col, #colpx { display: flex; flex-direction: column; width: 40px; height: 400px; flex-shrink: 0; }
#r0, #r1, #r2, #x0, #x1, #x2, #a, #b, #pa, #pb { width: 40px; height: 40px; }
#row   { gap: 10px 25%; }
#rowpx { row-gap: 10px; column-gap: 100px; }
#col   { gap: 10px 25%; }
#colpx { row-gap: 10px; column-gap: 10px; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    // The px arms are the reference: if they move, the fixture changed, not the fix.
    EXPECT_NEAR(fx.BorderBox("x1").X - fx.BorderBox("x0").X, 140.0f, kExactPx);
    EXPECT_NEAR(fx.BorderBox("pb").Y - fx.BorderBox("pa").Y, 50.0f, kExactPx);

    // Column gutter: 25% of the 400px content width, not 10% of it.
    EXPECT_NEAR(fx.BorderBox("r1").X - fx.BorderBox("r0").X, fx.BorderBox("x1").X - fx.BorderBox("x0").X, kExactPx);
    EXPECT_NEAR(fx.BorderBox("r2").X - fx.BorderBox("r0").X, 280.0f, kExactPx);
    // Row gutter: the declared 10px, not 10% of the 400px height.
    EXPECT_NEAR(fx.BorderBox("b").Y - fx.BorderBox("a").Y, fx.BorderBox("pb").Y - fx.BorderBox("pa").Y, kExactPx);
}

TEST(PercentageGap, TwoValueShorthandReversedKeepsEachGuttersOwnUnit)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <uielement id="row"><uielement id="r0"/><uielement id="r1"/></uielement>
  <uielement id="rowpx"><uielement id="x0"/><uielement id="x1"/></uielement>
  <uielement id="col"><uielement id="a"/><uielement id="b"/></uielement>
  <uielement id="colpx"><uielement id="pa"/><uielement id="pb"/></uielement>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: row; align-items: flex-start; width: 800px; height: 600px; }
#row, #rowpx { display: flex; flex-direction: row; width: 400px; height: 60px; flex-shrink: 0; }
#col, #colpx { display: flex; flex-direction: column; width: 40px; height: 400px; flex-shrink: 0; }
#r0, #r1, #x0, #x1, #a, #b, #pa, #pb { width: 40px; height: 40px; }
#row   { gap: 25% 10px; }
#rowpx { row-gap: 100px; column-gap: 10px; }
#col   { gap: 25% 10px; }
#colpx { row-gap: 100px; column-gap: 100px; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    EXPECT_NEAR(fx.BorderBox("x1").X - fx.BorderBox("x0").X, 50.0f, kExactPx);
    EXPECT_NEAR(fx.BorderBox("pb").Y - fx.BorderBox("pa").Y, 140.0f, kExactPx);

    // Column gutter: the declared 10px, not the 25 read as Px(25).
    EXPECT_NEAR(fx.BorderBox("r1").X - fx.BorderBox("r0").X, fx.BorderBox("x1").X - fx.BorderBox("x0").X, kExactPx);
    // Row gutter: 25% of the 400px content height, not 25px.
    EXPECT_NEAR(fx.BorderBox("b").Y - fx.BorderBox("a").Y, fx.BorderBox("pb").Y - fx.BorderBox("pa").Y, kExactPx);
}

// An unreadable gap declaration is rejected whole, leaving the cascaded value
// standing (CSS 2.1 §4.2) — it must not apply whatever numeric prefix it can
// salvage. Both a junk second token and a third value are pinned; the earlier
// 20px rule is what must survive, this engine's cascade being file order.

TEST(PercentageGap, MalformedShorthandLeavesTheCascadedGapStanding)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <uielement id="junk"><uielement id="j0"/><uielement id="j1"/></uielement>
  <uielement id="three"><uielement id="t0"/><uielement id="t1"/></uielement>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: column; width: 800px; height: 600px; }
#junk, #three { display: flex; flex-direction: row; width: 400px; height: 60px; }
#j0, #j1, #t0, #t1 { width: 40px; height: 40px; }
#junk  { gap: 20px; }
#junk  { gap: 10px oops; }
#three { gap: 20px; }
#three { gap: 1px 2px 3px; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    EXPECT_NEAR(fx.BorderBox("j1").X - fx.BorderBox("j0").X, 60.0f, kExactPx);
    EXPECT_NEAR(fx.BorderBox("t1").X - fx.BorderBox("t0").X, 60.0f, kExactPx);
}

// --- font-size: the percentage resolves against the PARENT's computed size ---
//
// Three labels of the same text in the same face. `150%` inside a 20px parent
// and a declared `30px` must shape identically — same advance, same ink — and
// both must differ from the 150px the unit-dropping parser produced.

TEST(PercentageFontSize, PercentOfParentEqualsTheDeclaredEquivalentAndNotTheBareNumber)
{
    LoadRobotoAtlas();
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <uielement id="parent"><label id="pct">HEALTH</label></uielement>
  <label id="explicit">HEALTH</label>
  <label id="bare">HEALTH</label>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 800px; height: 600px; }
label { font-family: Roboto; color: #ffffff; }
#parent { display: flex; flex-direction: column; align-items: flex-start; font-size: 20px; }
#pct { font-size: 150%; }
#explicit { font-size: 30px; }
#bare { font-size: 150px; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_EQ(fx.ResolvedFontFamily("pct"), "Roboto")
        << "staged Roboto missing; the widths below would pin a system face";

    const float pct = fx.BorderBox("pct").W;
    const float explicitPx = fx.BorderBox("explicit").W;
    const float bare = fx.BorderBox("bare").W;

    ASSERT_GT(explicitPx, 0.0f);
    EXPECT_NEAR(pct, explicitPx, kExactPx);
    // The base behaviour, stated so the pin above cannot pass by both arms
    // collapsing to the same wrong value: 30px and 150px are far apart.
    EXPECT_GT(bare, explicitPx * 3.0f);
    EXPECT_GT(std::abs(pct - bare), 1.0f);
}

// The composited counterpart: glyph quads, not the measured box. A box width can
// come out right while the shaper ran at another size, so the ink the two arms
// emit has to match in both dimensions.

TEST(PercentageFontSize, PercentAndDeclaredEquivalentEmitTheSameGlyphInk)
{
    LoadRobotoAtlas();
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <uielement id="parent"><label id="pct">HEALTH</label></uielement>
  <label id="explicit">HEALTH</label>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 800px; height: 600px; }
label { font-family: Roboto; color: #ffffff; }
#parent { display: flex; flex-direction: column; align-items: flex-start; font-size: 20px; }
#pct { font-size: 150%; }
#explicit { font-size: 30px; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_EQ(fx.ResolvedFontFamily("pct"), "Roboto");

    const InkBox pct = GlyphInk(fx, "pct");
    const InkBox explicitPx = GlyphInk(fx, "explicit");
    ASSERT_FALSE(pct.Empty()) << "percentage arm emitted no glyph quads";
    ASSERT_FALSE(explicitPx.Empty()) << "declared arm emitted no glyph quads";

    EXPECT_NEAR(pct.W(), explicitPx.W(), kExactPx);
    EXPECT_NEAR(pct.H(), explicitPx.H(), kExactPx);
}

// The root has no parent, so its percentage resolves against the initial
// font-size (kInitialFontSizePx). This is the decision the fix makes explicit;
// the pin holds it against a later drift to "the viewport" or "zero".

TEST(PercentageFontSize, AtTheRootThePercentageResolvesAgainstTheInitialSize)
{
    LoadRobotoAtlas();
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <label id="inherited">HEALTH</label>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 800px; height: 600px; font-size: 150%; }
label { font-family: Roboto; color: #ffffff; }
)");
    // The control carries the base this test's 27px depends on rather than
    // asserting it: `undeclared` takes the engine's initial font-size, `is18`
    // declares 18px, and `is27` declares the 150% of it that the arm above
    // should have computed. If the initial value ever moves, the first pin
    // fails and says so, instead of the 27 silently becoming the wrong target.
    IsolatedUIFixture ctl;
    const bool ctlBuilt = ctl.Build(1.0f,
                                    R"(<uielement id="root">
  <label id="undeclared">HEALTH</label>
  <label id="is18">HEALTH</label>
  <label id="is27">HEALTH</label>
</uielement>)",
                                    R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 800px; height: 600px; }
label { font-family: Roboto; color: #ffffff; }
#is18 { font-size: 18px; }
#is27 { font-size: 27px; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_TRUE(ctlBuilt) << ctl.Diagnostic();
    ASSERT_EQ(fx.ResolvedFontFamily("inherited"), "Roboto");

    EXPECT_NEAR(ctl.BorderBox("undeclared").W, ctl.BorderBox("is18").W, kExactPx)
        << "the initial font-size is no longer 18px, so 27px is no longer 150% of it";
    EXPECT_NEAR(fx.BorderBox("inherited").W, ctl.BorderBox("is27").W, kExactPx);
}

// Nested percentages compose against the computed parent, and only once. 200% of
// the 18px initial is 36px; 50% of that is 18px again, which is what the control
// declares. This is also the compounding pin: the resolve walk runs over the
// same element many times a frame, and a resolver that rewrote FontSize in place
// instead of recomputing from the stored percentage would drift on every pass.

TEST(PercentageFontSize, NestedPercentagesComposeAgainstTheComputedParentOnlyOnce)
{
    LoadRobotoAtlas();
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <uielement id="outer"><uielement id="inner"><label id="leaf">HEALTH</label></uielement></uielement>
</uielement>)",
                                R"(
#root, #outer, #inner { display: flex; flex-direction: column; align-items: flex-start; }
#root { width: 800px; height: 600px; }
label { font-family: Roboto; color: #ffffff; }
#outer { font-size: 200%; }
#inner { font-size: 50%; }
)");
    IsolatedUIFixture ctl;
    const bool ctlBuilt = ctl.Build(1.0f,
                                    R"(<uielement id="root">
  <label id="leaf">HEALTH</label>
</uielement>)",
                                    R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 800px; height: 600px; }
label { font-family: Roboto; color: #ffffff; font-size: 18px; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_TRUE(ctlBuilt) << ctl.Diagnostic();
    ASSERT_EQ(fx.ResolvedFontFamily("leaf"), "Roboto");

    const float before = fx.BorderBox("leaf").W;
    EXPECT_NEAR(before, ctl.BorderBox("leaf").W, kExactPx);

    // Twenty more resolve passes over an unchanged tree must not move it.
    for (int i = 0; i < 20; ++i)
        fx.Settle();
    EXPECT_NEAR(fx.BorderBox("leaf").W, before, kExactPx);
}

// font-size takes exactly one component, so a second token makes the
// declaration unreadable and it is rejected whole — the label keeps the
// inherited size. The same last-character unit sniff that broke the gap
// shorthand would read `30px 50%` as Percent(30) and shape at 6px here.

TEST(PercentageFontSize, ASecondTokenRejectsTheDeclarationWhole)
{
    LoadRobotoAtlas();
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <uielement id="parent"><label id="junk">HEALTH</label><label id="plain">HEALTH</label></uielement>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 800px; height: 600px; }
label { font-family: Roboto; color: #ffffff; }
#parent { display: flex; flex-direction: column; align-items: flex-start; font-size: 20px; }
#junk { font-size: 30px 50%; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_EQ(fx.ResolvedFontFamily("junk"), "Roboto");

    EXPECT_NEAR(fx.BorderBox("junk").W, fx.BorderBox("plain").W, kExactPx);
}
