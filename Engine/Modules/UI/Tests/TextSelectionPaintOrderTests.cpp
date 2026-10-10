// GitHub #779 — the text-selection highlight composites OVER the glyphs.
//
// CSS paints a selection background BELOW the text it covers; this engine
// emitted it after the glyph run, and emission order IS composite order inside
// an element's primitive slot range. With the highlight at 0x66 alpha the
// selected glyphs came out as 0.6*glyph + 0.4*accent — legible, so it read as a
// theme choice rather than a bug, in every TextField, TextArea and
// ScriptTextArea.
//
// What is asserted is the ORDER of the emitted primitives, not a pixel. This
// target declares the UI draw pass but never executes it (UIRgTestHarness.h),
// so there is no blended result to read back; the composite order is the last
// observable thing before that boundary. The two ends of the run are pinned
// separately for a reason: the selection must move BELOW the glyphs without the
// caret following it down. A fix that moved the whole overlay block would pass a
// selection-only assertion and silently bury the caret.
//
// The single-line and multiline controls are two independent emitters — the
// generic path in UIManager_PrimitiveGen for TextInput, TextArea's own
// OnGeneratePrimitives (it returns true from HandlesOwnTextRendering) — and each
// carried its own copy of the defect, so each is tested. ScriptTextArea has a
// third copy and lives in Apps/Editor, which has no test target; it is fixed
// with the same shape and is out of reach from here.
//
// SPECIMEN NOTE — the selection colour is authored, not defaulted. Both
// emitters fill the highlight with `selection-color` (UI::PackedTextSelectionFill).
// Authoring a pure red makes the highlight rect identifiable by fill alone, and
// distinct from the caret (which takes `color`, white here) and from the element
// background (which is never emitted: no background-color is set, so the chrome
// rect is skipped).

#include "IsolatedUIFixture.h"

#include "UI/Controls/TextArea.h"
#include "UI/Controls/TextField.h"
#include "UI/UIElement.h"
#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using GameEngine::TextArea;
using GameEngine::TextInput;
using GameEngine::UIElement;
using GameEngine::UI::PrimitiveMode;
using GameEngine::UI::UIPrimitive;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

constexpr char kXml[] = R"(<uielement id="root">
  <textinput id="field"/>
  <textarea id="area"/>
</uielement>)";

constexpr char kCss[] = R"(
#root { display: flex; flex-direction: column; width: 600px; height: 400px; }
#field, #area {
  width: 300px;
  height: 40px;
  font-family: Roboto;
  font-size: 16px;
  white-space: nowrap;
  padding: 0px;
  border-width: 0px;
  selection-color: #ff0000;
  color: #ffffff;
}
)";

// Latin only, no wrap opportunity taken at 300px, one visual line in both
// controls so the highlight is a single rect.
constexpr char kValue[] = "Hamburgefons";
constexpr int kValueLength = 12;

// Colours as the primitives carry them: packed RGBA8, R in the low byte
// (UIPrimitive.h). The highlight is the authored red at the 0x66 alpha the
// focused-selection path applies; the caret is the authored text colour.
constexpr uint32_t kHighlightFill = 0x660000FFu;
constexpr uint32_t kCaretFill = 0xFFFFFFFFu;

constexpr size_t kNotFound = static_cast<size_t>(-1);

bool IsGlyph(const UIPrimitive& p)
{
    return GameEngine::UI::GetMode(p.ModeAndFlags) == PrimitiveMode::Slug;
}

bool IsRectWithFill(const UIPrimitive& p, uint32_t fill)
{
    return GameEngine::UI::GetMode(p.ModeAndFlags) == PrimitiveMode::Rect &&
           p.FillColor == fill;
}

size_t IndexOfFirstGlyph(const std::vector<UIPrimitive>& prims)
{
    for (size_t i = 0; i < prims.size(); ++i)
        if (IsGlyph(prims[i]))
            return i;
    return kNotFound;
}

size_t IndexOfLastGlyph(const std::vector<UIPrimitive>& prims)
{
    size_t found = kNotFound;
    for (size_t i = 0; i < prims.size(); ++i)
        if (IsGlyph(prims[i]))
            found = i;
    return found;
}

size_t IndexOfRect(const std::vector<UIPrimitive>& prims, uint32_t fill)
{
    for (size_t i = 0; i < prims.size(); ++i)
        if (IsRectWithFill(prims[i], fill))
            return i;
    return kNotFound;
}

// `textinput` and `textarea` take no `value` XML attribute, so the specimen is
// set through the shared BaseField API and the tree re-settled.
bool GiveBothControlsText(IsolatedUIFixture& fx)
{
    UIElement* field = fx.Element("field");
    UIElement* area = fx.Element("area");
    if (!field || !area)
        return false;

    if (TextInput* input = field->GetAsTextInput())
        input->SetValue(std::string(kValue));
    else
        return false;

    auto* textArea = dynamic_cast<TextArea*>(area);
    if (!textArea)
        return false;
    textArea->SetValue(std::string(kValue));

    fx.Settle();
    return true;
}

// Gives `id` focus and selects the whole specimen in it. TextInput and TextArea
// are unrelated types with unrelated selection APIs — the same split that gave
// the defect two independent copies.
//
// Focus lands first and is settled before the selection is applied: FocusIn is
// dispatched a frame after the focus id changes, and both controls' handlers
// touch caret state on the way in. Selecting afterwards means nothing can
// silently undo it between here and the assertions.
bool SelectAllAndFocus(IsolatedUIFixture& fx, const std::string& id)
{
    UIElement* el = fx.Element(id);
    if (!el)
        return false;

    fx.Manager().FocusElement(el);
    fx.Settle();

    if (TextInput* input = el->GetAsTextInput())
        input->SelectAll();
    else if (auto* area = dynamic_cast<TextArea*>(el))
        area->SetSelection(0, kValueLength);
    else
        return false;

    fx.Settle();
    return true;
}

} // namespace

// The single-line editor: the generic glyph path in UIManager_PrimitiveGen plus
// BuildTextInputOverlays.
TEST(TextSelectionPaintOrder, SingleLineHighlightPrecedesGlyphs)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_TRUE(GiveBothControlsText(fx));
    ASSERT_TRUE(SelectAllAndFocus(fx, "field"));

    const auto prims = fx.Primitives("field");
    const size_t firstGlyph = IndexOfFirstGlyph(prims);
    const size_t highlight = IndexOfRect(prims, kHighlightFill);

    // Instrument check before the claim: with no glyphs emitted (font did not
    // resolve) or no highlight emitted (selection or focus did not take), the
    // ordering assertion below would be vacuous.
    ASSERT_NE(firstGlyph, kNotFound) << "no glyphs emitted";
    ASSERT_NE(highlight, kNotFound) << "no selection highlight emitted";

    EXPECT_LT(highlight, firstGlyph)
        << "the selection highlight must be emitted before the glyph run so it "
           "composites under the text";
}

// The caret is the other end of the same run and must NOT move under the text.
TEST(TextSelectionPaintOrder, SingleLineCaretFollowsGlyphs)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_TRUE(GiveBothControlsText(fx));
    ASSERT_TRUE(SelectAllAndFocus(fx, "field"));

    const auto prims = fx.Primitives("field");
    const size_t lastGlyph = IndexOfLastGlyph(prims);
    const size_t caret = IndexOfRect(prims, kCaretFill);

    ASSERT_NE(lastGlyph, kNotFound) << "no glyphs emitted";
    ASSERT_NE(caret, kNotFound) << "no caret emitted";

    EXPECT_GT(caret, lastGlyph) << "the caret paints over the text, not under it";
}

// TextArea emits its own glyphs, its own highlight and its own caret: the same
// contract, a separate implementation.
TEST(TextSelectionPaintOrder, MultilineHighlightPrecedesGlyphs)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_TRUE(GiveBothControlsText(fx));
    ASSERT_TRUE(SelectAllAndFocus(fx, "area"));

    const auto prims = fx.Primitives("area");
    const size_t firstGlyph = IndexOfFirstGlyph(prims);
    const size_t highlight = IndexOfRect(prims, kHighlightFill);

    ASSERT_NE(firstGlyph, kNotFound) << "no glyphs emitted";
    ASSERT_NE(highlight, kNotFound) << "no selection highlight emitted";

    EXPECT_LT(highlight, firstGlyph)
        << "the selection highlight must be emitted before the glyph run so it "
           "composites under the text";
}

TEST(TextSelectionPaintOrder, MultilineCaretFollowsGlyphs)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_TRUE(GiveBothControlsText(fx));
    ASSERT_TRUE(SelectAllAndFocus(fx, "area"));

    const auto prims = fx.Primitives("area");
    const size_t lastGlyph = IndexOfLastGlyph(prims);
    const size_t caret = IndexOfRect(prims, kCaretFill);

    ASSERT_NE(lastGlyph, kNotFound) << "no glyphs emitted";
    ASSERT_NE(caret, kNotFound) << "no caret emitted";

    EXPECT_GT(caret, lastGlyph) << "the caret paints over the text, not under it";
}

// Nothing selected and nothing focused: the glyph run must still be emitted.
// Both controls route the no-overlay case around the selection block, and a
// restructure that made the glyphs conditional on the overlays would erase the
// text of every unfocused field in the editor.
TEST(TextSelectionPaintOrder, GlyphsSurviveWithNoSelectionAndNoFocus)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_TRUE(GiveBothControlsText(fx));

    EXPECT_NE(IndexOfFirstGlyph(fx.Primitives("field")), kNotFound);
    EXPECT_NE(IndexOfFirstGlyph(fx.Primitives("area")), kNotFound);
    EXPECT_EQ(IndexOfRect(fx.Primitives("field"), kHighlightFill), kNotFound);
    EXPECT_EQ(IndexOfRect(fx.Primitives("area"), kHighlightFill), kNotFound);
}
