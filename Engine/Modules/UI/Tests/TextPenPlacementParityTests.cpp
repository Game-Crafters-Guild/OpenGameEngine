// Pen placement of text runs vs Chrome (issue #813), pinned from the outside.
//
// The measured defect: HUD ability-slot captions (shrink-wrapped labels flex-
// centered inside fixed slots) sit as a rigid ~1px ink translation left of
// Chrome rendering the same Roboto TTF, while the slot boxes themselves are
// pixel-identical. Chrome computes a fractional advance for the caption
// (Roboto "Q" at 22px: 1408/2048*22 = 15.125), sizes the label box to exactly
// that advance, centers it at fractional x and rasterises subpixel.
//
// The engine's Yoga measure reported advance + 2 — an allowance for Slug's
// half-pixel quad dilation, added to the LAYOUT width — while emission
// left-anchors the run at the content-box origin. So a box positioned FROM its
// intrinsic width (flex centering, flex-end anchoring) parked the ink 1px (or,
// anchored right, 2px) left of Chrome's. Nothing in the chain rounds to whole
// pixels: both engines place the run at the same subpixel phase, which is why
// the defect measured as a rigid integer translation of identical ink.
//
// The pins below assert Chrome's model: a shrink-wrapped label's box IS the
// advance width, and the first-glyph pen of a flex-centered caption lands at
// the fractional ideal boxCenter - advance/2. The fixed-width control pins the
// alignment math that was already correct, so a regression there cannot hide
// behind the measure fix. The editable-field pins at the bottom carry the same
// geometry through the caret: what the pointer maps to and what the caret
// paints must be the one origin the glyphs use.

#include "IsolatedUIFixture.h"
#include "RobotoTestFont.h"

#include "Rendering/Text/FontAtlas.h"
#include "UI/Controls/TextField.h"
#include "UI/UIElement.h"
#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace GameEngine;
using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::LoadRobotoAtlas;
using GameEngine::UITesting::PhysicalRect;

namespace
{

// Yoga rounds solved positions onto a 1/64 device-px grid; everything after it
// is exact float math. 0.03px accepts two grid quanta and still falsifies the
// 1px (halo/2) and 2px (halo) placement errors by two orders of magnitude.
constexpr float kPlacementTolerancePx = 0.03f;

// The HUD geometry the defect was measured on (GameHudComplex.css .slot /
// .slot-key): fixed slot, 2px border, shrink-wrapped caption centered both
// ways, Roboto 22px.
constexpr float kSlotBorderPx = 2.0f;
constexpr float kCaptionFontPx = 22.0f;

struct PenProbe
{
    float QuadX = 0.0f;       // emitted Slug quad x (physical px)
    float Pen = 0.0f;         // derived first-glyph pen x (physical px)
    bool Valid = false;
};

// Derives the first-glyph pen from the emitted Slug quad: quadX = pen + gp.x
// where gp.x is the pen-relative quad offset ShapeText computes (xOffset plus
// the em-space min-x bearing). Shaping the same text at the same pixel size on
// the same staged face reproduces gp.x exactly.
PenProbe DeriveFirstGlyphPen(const IsolatedUIFixture& fx, const std::string& id,
                             Rendering::Text::FontAtlas& atlas, const std::string& text)
{
    PenProbe probe{};
    const auto prims = fx.Primitives(id, UI::PrimitiveMode::Slug);
    if (prims.empty())
        return probe;

    Rendering::Text::FontAtlas::ShapeResult shaped;
    atlas.ShapeText(text, kCaptionFontPx, shaped);
    if (shaped.glyphs.empty())
        return probe;

    probe.QuadX = prims.front().X;
    probe.Pen = prims.front().X - shaped.glyphs.front().x;
    probe.Valid = true;
    return probe;
}

// --- Editable-field helpers ---------------------------------------------------

// Caret fill for a focused field: the authored colour, packed RGBA8 with R in
// the low byte (UIPrimitive.h). The specimen fields below set colour #ff0000
// and no background, so exactly one Rect carries it.
constexpr uint32_t kCaretFill = 0xFF0000FFu;

constexpr size_t kNotFound = static_cast<size_t>(-1);

// The caret x the field actually PAINTS (physical px), or NaN when the field
// emitted none — an unfocused field paints a fully transparent caret in a
// different colour, so a missing match is a real "no caret", not a near miss.
float PaintedCaretX(const IsolatedUIFixture& fx, const std::string& id)
{
    size_t found = kNotFound;
    const auto prims = fx.Primitives(id, UI::PrimitiveMode::Rect);
    for (size_t i = 0; i < prims.size(); ++i)
    {
        if (prims[i].FillColor != kCaretFill)
            continue;
        if (found != kNotFound)
            return std::nanf(""); // ambiguous: more than one candidate
        found = i;
    }
    return (found == kNotFound) ? std::nanf("") : prims[found].X;
}

// Every primitive the element emitted, for the failure message when the caret
// probe finds none: "no caret" and "two candidates" are different defects and
// the fill values say which.
std::string DumpPrimitives(const IsolatedUIFixture& fx, const std::string& id)
{
    std::string out;
    char line[160];
    for (const UI::UIPrimitive& p : fx.Primitives(id))
    {
        std::snprintf(line, sizeof(line), "  mode=%d fill=0x%08X x=%.4f w=%.4f opacity=%.2f\n",
                      (int)UI::GetMode(p.ModeAndFlags), p.FillColor, p.X, p.W, p.Opacity);
        out += line;
    }
    return out.empty() ? std::string("  <none>\n") : out;
}

// A full press/release at a logical point, settled either side so focus
// changes and the post-layout convergence they trigger are done before the
// caller reads anything back.
void ClickAt(IsolatedUIFixture& fx, float logicalX, float logicalY)
{
    fx.Manager().OnMouseMove(logicalX, logicalY);
    fx.Settle();
    fx.Manager().OnMouseButton(0, true);
    fx.Settle();
    fx.Manager().OnMouseButton(0, false);
    fx.Settle();
}

} // namespace

// --- The QWER case: flex-centered shrink-wrapped captions ---------------------
//
// Four HUD slots, one caption each. Chrome sizes each caption box to its
// fractional advance and centers it, landing the pen at
// contentX + (contentW - advance) / 2. Every slot is placed at an integer x on
// purpose: with a fractional advance the ideal pen is then genuinely
// fractional, so an integer-snapping engine cannot pass by accident — the
// fixture asserts that discrimination below.

TEST(PenPlacement, FlexCenteredCaptionPenLandsAtTheChromeFractionalIdeal)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <uielement id="slotQ" class="slot"><label id="capQ">Q</label></uielement>
  <uielement id="slotW" class="slot"><label id="capW">W</label></uielement>
  <uielement id="slotE" class="slot"><label id="capE">E</label></uielement>
  <uielement id="slotR" class="slot"><label id="capR">R</label></uielement>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: row; gap: 14px; padding: 32px; width: 700px; height: 200px; }
.slot { width: 64px; height: 64px; display: flex; align-items: center; justify-content: center;
        border: 2px solid #78aaf0; }
label { font-family: Roboto; font-size: 22px; color: #ffffff; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_EQ(fx.ResolvedFontFamily("capQ"), "Roboto")
        << "staged Roboto missing; the fractional ideals below pin that face";

    auto atlas = LoadRobotoAtlas();
    ASSERT_TRUE(atlas);

    const char* caps[] = {"Q", "W", "E", "R"};
    const char* slotIds[] = {"slotQ", "slotW", "slotE", "slotR"};
    const char* capIds[] = {"capQ", "capW", "capE", "capR"};

    bool sawFractionalIdeal = false;
    for (int i = 0; i < 4; ++i)
    {
        const float advance = atlas->MeasureUtf8(caps[i], kCaptionFontPx).width;
        ASSERT_GT(advance, 0.0f) << caps[i];

        const PhysicalRect slot = fx.BorderBox(slotIds[i]);
        ASSERT_GT(slot.W, 0.0f) << slotIds[i];
        const float contentX = slot.X + kSlotBorderPx;
        const float contentW = slot.W - 2.0f * kSlotBorderPx;

        // Chrome's model: the caption box is the advance, centered.
        const float idealPen = contentX + (contentW - advance) * 0.5f;
        if (std::abs(idealPen - std::round(idealPen)) > 0.05f)
            sawFractionalIdeal = true;

        const PenProbe probe = DeriveFirstGlyphPen(fx, capIds[i], *atlas, caps[i]);
        ASSERT_TRUE(probe.Valid) << capIds[i];

        const PhysicalRect cap = fx.BorderBox(capIds[i]);

        // Localization chain for issue #813 — every layer, one line per slot.
        std::printf("[pen-x] %s: slot box [%.4f w %.4f] content [%.4f w %.4f] | "
                    "label box [%.4f w %.4f] advance %.4f (label-adv %.4f) | "
                    "quadX %.4f pen %.4f ideal %.4f delta %+.4f\n",
                    caps[i], slot.X, slot.W, contentX, contentW, cap.X, cap.W, advance,
                    cap.W - advance, probe.QuadX, probe.Pen, idealPen, probe.Pen - idealPen);

        EXPECT_NEAR(probe.Pen, idealPen, kPlacementTolerancePx) << caps[i];
    }

    // The sample must be able to disprove an integer-snapping engine: at least
    // one ideal pen sits away from every whole pixel.
    EXPECT_TRUE(sawFractionalIdeal)
        << "every ideal pen landed near a whole pixel; the case cannot discriminate";
}

// --- The box itself: a shrink-wrapped label is its advance wide ---------------

TEST(PenPlacement, ShrinkWrappedLabelBoxWidthIsTheAdvance)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <label id="cap">Q</label>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 400px; height: 300px; }
label { font-family: Roboto; font-size: 22px; color: #ffffff; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_EQ(fx.ResolvedFontFamily("cap"), "Roboto");

    auto atlas = LoadRobotoAtlas();
    ASSERT_TRUE(atlas);
    const float advance = atlas->MeasureUtf8("Q", kCaptionFontPx).width;

    // Chrome: getBoundingClientRect().width == measureText().width for a
    // shrink-wrapped inline-block. Roboto "Q" at 22px: 1408/2048*22 = 15.125.
    EXPECT_NEAR(advance, 15.125f, 0.1f);
    EXPECT_NEAR(fx.BorderBox("cap").W, advance, kPlacementTolerancePx);
}

// --- The 2px twin: flex-end anchors the box, ink must be flush ----------------

TEST(PenPlacement, FlexEndAnchoredCaptionPenLandsAtTheChromeIdeal)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <uielement id="slot"><label id="cap">R</label></uielement>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: row; padding: 32px; width: 400px; height: 200px; }
#slot { width: 64px; height: 64px; display: flex; align-items: center; justify-content: flex-end;
        border: 2px solid #78aaf0; }
label { font-family: Roboto; font-size: 22px; color: #ffffff; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_EQ(fx.ResolvedFontFamily("cap"), "Roboto");

    auto atlas = LoadRobotoAtlas();
    ASSERT_TRUE(atlas);
    const float advance = atlas->MeasureUtf8("R", kCaptionFontPx).width;

    const PhysicalRect slot = fx.BorderBox("slot");
    const float contentRight = slot.X + slot.W - kSlotBorderPx;
    const float idealPen = contentRight - advance;

    const PenProbe probe = DeriveFirstGlyphPen(fx, "cap", *atlas, "R");
    ASSERT_TRUE(probe.Valid);
    EXPECT_NEAR(probe.Pen, idealPen, kPlacementTolerancePx);
}

// --- Control: fixed-width text-align centering was never the defect -----------
//
// A label with an explicit width centers via alignOffsetX over the halo-free
// run width, which already matches Chrome. This must be green before AND after
// the measure fix — it separates "the measure reported the wrong intrinsic
// width" from "the alignment math is wrong", and catches a fix that
// overcorrects by touching emission instead of measurement.

TEST(PenPlacement, FixedWidthTextAlignCenterIsAlreadyAtTheChromeIdeal)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <label id="cap">Q</label>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: column; align-items: flex-start; padding: 32px; width: 400px; height: 200px; }
label { font-family: Roboto; font-size: 22px; color: #ffffff; width: 64px; text-align: center; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_EQ(fx.ResolvedFontFamily("cap"), "Roboto");

    auto atlas = LoadRobotoAtlas();
    ASSERT_TRUE(atlas);
    const float advance = atlas->MeasureUtf8("Q", kCaptionFontPx).width;

    const PhysicalRect cap = fx.BorderBox("cap");
    const float idealPen = cap.X + (cap.W - advance) * 0.5f;

    const PenProbe probe = DeriveFirstGlyphPen(fx, "cap", *atlas, "Q");
    ASSERT_TRUE(probe.Valid);
    EXPECT_NEAR(probe.Pen, idealPen, kPlacementTolerancePx);
}

// --- The editable twin: ink and caret share one origin ------------------------
//
// A shrink-wrapped TextInput centered in a slot is the QWER geometry with a
// caret in it. Symmetric padding cancels out of the centering — box width
// advance + 2p centered in the slot puts the CONTENT origin at
// slotCenter - advance/2 whatever p is — so the ideal below is the same
// fractional number the label pins use, and the pre-fix box (advance + 2 + 2p)
// misses it by exactly 1px.
//
// Ink and caret are asserted against that one ideal rather than against each
// other: a shared origin that is wrong in both would satisfy "they agree".

TEST(PenPlacement, ShrinkWrappedCentredFieldPutsInkAndCaretOnTheChromeIdeal)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <uielement id="slot"><textinput id="field"/></uielement>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: row; padding: 32px; width: 500px; height: 200px; }
#slot { width: 200px; height: 48px; display: flex; align-items: center; justify-content: center;
        border: 2px solid #78aaf0; }
#field { font-family: Roboto; font-size: 22px; color: #ff0000; padding: 4px;
         border-width: 0px; white-space: nowrap; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    UIElement* fieldEl = fx.Element("field");
    ASSERT_NE(fieldEl, nullptr);
    TextInput* input = fieldEl->GetAsTextInput();
    ASSERT_NE(input, nullptr);
    // A bare <textinput> is the inner editor of the composite fields and is not
    // focusable on its own (TextFieldBase's constructor is what opts in), so the
    // specimen says so explicitly rather than depending on focus policy: the
    // caret is only painted in the authored colour while the field has focus.
    fieldEl->SetFocusable(true);
    input->SetValue(std::string("Q"));
    fx.Settle();
    ASSERT_EQ(fx.ResolvedFontFamily("field"), "Roboto");

    auto atlas = LoadRobotoAtlas();
    ASSERT_TRUE(atlas);
    const float advance = atlas->MeasureUtf8("Q", kCaptionFontPx).width;

    const PhysicalRect slot = fx.BorderBox("slot");
    const float slotCentreX = slot.X + slot.W * 0.5f;
    const float idealPen = slotCentreX - advance * 0.5f;

    // Focus by clicking the field's left edge: the caret is only painted in the
    // authored colour while focused, and the leftmost caret position is the one
    // that sits on the run origin.
    const PhysicalRect field = fx.BorderBox("field");
    ClickAt(fx, field.X + 1.0f, field.Y + field.H * 0.5f);
    ASSERT_EQ(input->GetCaretIndex(), 0);

    const float caretX = PaintedCaretX(fx, "field");
    ASSERT_FALSE(std::isnan(caretX))
        << "no identifiable caret; focus=" << fx.Manager().GetFocusedElementId()
        << " primitives:\n" << DumpPrimitives(fx, "field");

    const PenProbe probe = DeriveFirstGlyphPen(fx, "field", *atlas, "Q");
    ASSERT_TRUE(probe.Valid);

    std::printf("[pen-x] field: slot [%.4f w %.4f] field box [%.4f w %.4f] advance %.4f | "
                "pen %.4f caret0 %.4f ideal %.4f (pen %+.4f, caret %+.4f)\n",
                slot.X, slot.W, field.X, field.W, advance, probe.Pen, caretX, idealPen,
                probe.Pen - idealPen, caretX - idealPen);

    EXPECT_NEAR(probe.Pen, idealPen, kPlacementTolerancePx) << "ink origin";
    EXPECT_NEAR(caretX, idealPen, kPlacementTolerancePx) << "caret origin";
}

// --- Click-to-caret maps onto the caret the user sees -------------------------
//
// Hit-testing rebuilds the run origin independently of the paint path
// (TextInput::BuildPointerGeometry vs BuildTextInputOverlays). Any constant
// offset between the two is invisible in a "click roughly there" test and is
// exactly what an origin-shifting change gets wrong, so the probe clicks a
// hair either side of the midpoint between two painted caret positions: with
// the two origins equal, which side of the midpoint the click falls on decides
// the answer, and an offset of even 0.1px reverses one of the two.

TEST(PenPlacement, ClickResolvesToTheCaretItLandsNearestInPaintedSpace)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                R"(<uielement id="root">
  <uielement id="slot"><textinput id="field"/></uielement>
</uielement>)",
                                R"(
#root { display: flex; flex-direction: row; padding: 32px; width: 500px; height: 200px; }
#slot { width: 300px; height: 48px; display: flex; align-items: center; justify-content: center;
        border: 2px solid #78aaf0; }
#field { font-family: Roboto; font-size: 22px; color: #ff0000; padding: 4px;
         border-width: 0px; white-space: nowrap; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    UIElement* fieldEl = fx.Element("field");
    ASSERT_NE(fieldEl, nullptr);
    TextInput* input = fieldEl->GetAsTextInput();
    ASSERT_NE(input, nullptr);
    fieldEl->SetFocusable(true); // see the sibling test: <textinput> opts in per-instance
    input->SetValue(std::string("Hamburgefons"));
    fx.Settle();
    ASSERT_EQ(fx.ResolvedFontFamily("field"), "Roboto");

    const ResolvedStyle* style = fx.Style("field");
    ASSERT_NE(style, nullptr);
    Rendering::Text::FontAtlas* font = fx.Manager().ResolveFontForStyle(*style);
    ASSERT_NE(font, nullptr);

    // Content scale 1: logical and physical px coincide, so a pointer
    // coordinate and a primitive x can be compared without conversion.
    ASSERT_FLOAT_EQ(fx.Manager().GetContentScale(), 1.0f);
    const auto& measure = input->EnsureMeasureCache(font, kCaptionFontPx, 0.0f);
    const std::vector<float>& caretMap = measure.caretXByByte;
    ASSERT_GT(caretMap.size(), 8u);

    const PhysicalRect field = fx.BorderBox("field");
    const float midY = field.Y + field.H * 0.5f;
    const float farLeftX = field.X + 1.0f;

    // Origin the caret is PAINTED from, read back rather than reconstructed.
    ClickAt(fx, farLeftX, midY);
    ASSERT_EQ(input->GetCaretIndex(), 0);
    const float originX = PaintedCaretX(fx, "field");
    ASSERT_FALSE(std::isnan(originX))
        << "no identifiable caret; focus=" << fx.Manager().GetFocusedElementId()
        << " primitives:\n" << DumpPrimitives(fx, "field");
    ASSERT_FLOAT_EQ(caretMap[0], 0.0f) << "caret map must start at the run origin";

    constexpr int kProbeIndex = 6;
    constexpr float kEpsPx = 0.05f;
    const float left = originX + caretMap[kProbeIndex];
    const float right = originX + caretMap[kProbeIndex + 1];
    const float midpoint = (left + right) * 0.5f;

    // The sample discriminates only while epsilon is well inside the advance it
    // straddles, and only while the two clicks are far enough from the reset
    // click for the double-click gesture (20px) not to swallow them.
    ASSERT_GT(right - left, 20.0f * kEpsPx) << "advance too narrow to straddle";
    ASSERT_GT(midpoint - farLeftX, 25.0f) << "probe within double-click range of the reset click";

    ClickAt(fx, midpoint + kEpsPx, midY);
    const int rightOfMid = input->GetCaretIndex();

    // Reset the double-click gesture with a click well outside its tolerance.
    ClickAt(fx, farLeftX, midY);
    ClickAt(fx, midpoint - kEpsPx, midY);
    const int leftOfMid = input->GetCaretIndex();

    std::printf("[pen-x] hit: origin %.4f caret[%d] %.4f caret[%d] %.4f mid %.4f -> "
                "left-of-mid %d, right-of-mid %d\n",
                originX, kProbeIndex, left, kProbeIndex + 1, right, midpoint, leftOfMid,
                rightOfMid);

    EXPECT_EQ(leftOfMid, kProbeIndex);
    EXPECT_EQ(rightOfMid, kProbeIndex + 1);
}
