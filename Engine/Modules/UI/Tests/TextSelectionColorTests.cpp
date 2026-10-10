// GitHub #3268: every text control fills selected text with `selection-color`, never with its
// border colour. A field whose border is a neutral grey used to select in that grey, on a grey
// plate, so a select-all looked like nothing happened.
//
// The single-line editor (the generic TextInput path in UIManager_PrimitiveGen) and TextArea
// (its own OnGeneratePrimitives) are separate emitters, so each is asserted. What is asserted is
// the fill of the emitted highlight rect: this target declares the UI draw pass but never
// executes it (UIRgTestHarness.h), so there is no pixel to read back.

#include "IsolatedUIFixture.h"

#include "UI/Controls/TextArea.h"
#include "UI/Controls/TextField.h"
#include "UI/UIElement.h"
#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using GameEngine::TextArea;
using GameEngine::TextInput;
using GameEngine::UIElement;
using GameEngine::UI::PackFromARGB;
using GameEngine::UI::PrimitiveMode;
using GameEngine::UI::UIPrimitive;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

constexpr char kXml[] = R"(<uielement id="root">
  <textinput id="field"/>
  <textarea id="area"/>
</uielement>)";

// Both controls carry a visible neutral grey border, the shape that selected in grey.
constexpr char kGreyBorderCss[] = R"(
#root { display: flex; flex-direction: column; width: 600px; height: 400px; }
#field, #area {
  width: 300px;
  height: 40px;
  font-family: Roboto;
  font-size: 16px;
  white-space: nowrap;
  padding: 0px;
  border-width: 1px;
  border-color: #808080;
  color: #ffffff;
}
)";

// The same controls under a root that restates the inherited selection colour.
constexpr char kGreenRootCss[] = R"(
#root { selection-color: #00ff00; }
)";

constexpr char kValue[] = "Hamburgefons";
constexpr int kValueLength = 12;

// The grey border at either focus alpha: what the border-derived fill produced.
const uint32_t kGreyFocusedFill = PackFromARGB(0x66808080u);
// The initial selection-color (ResolvedStyle.h) and the green override, at the focused alpha.
const uint32_t kDefaultFocusedFill = PackFromARGB(0x663A8FFFu);
const uint32_t kGreenFocusedFill = PackFromARGB(0x6600FF00u);
const uint32_t kGreenUnfocusedFill = PackFromARGB(0x4400FF00u);

bool HasRectWithFill(const std::vector<UIPrimitive>& prims, uint32_t fill)
{
    for (const UIPrimitive& p : prims)
        if (GameEngine::UI::GetMode(p.ModeAndFlags) == PrimitiveMode::Rect && p.FillColor == fill)
            return true;
    return false;
}

bool GiveBothControlsText(IsolatedUIFixture& fx)
{
    TextInput* input = fx.Element("field") ? fx.Element("field")->GetAsTextInput() : nullptr;
    auto* area = dynamic_cast<TextArea*>(fx.Element("area"));
    if (!input || !area)
        return false;
    input->SetValue(std::string(kValue));
    area->SetValue(std::string(kValue));
    fx.Settle();
    return true;
}

// Focus is settled before the selection is applied: both controls touch caret state on FocusIn.
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

TEST(TextSelectionColor, GreyBorderedFocusedControlsSelectInTheSelectionColorNotTheBorder)
{
    for (const char* id : {"field", "area"})
    {
        IsolatedUIFixture fx;
        const bool built = fx.Build(1.0f, kXml, kGreyBorderCss);
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(built) << fx.Diagnostic();
        ASSERT_TRUE(GiveBothControlsText(fx));
        ASSERT_TRUE(SelectAllAndFocus(fx, id)) << id;

        const auto prims = fx.Primitives(id);
        EXPECT_TRUE(HasRectWithFill(prims, kDefaultFocusedFill)) << id << ": no highlight in the selection colour";
        EXPECT_FALSE(HasRectWithFill(prims, kGreyFocusedFill)) << id << ": highlight follows the border";
    }
}

TEST(TextSelectionColor, AnInheritedSelectionColorIsHonored)
{
    const std::string css = std::string(kGreyBorderCss) + kGreenRootCss;
    for (const char* id : {"field", "area"})
    {
        IsolatedUIFixture fx;
        const bool built = fx.Build(1.0f, kXml, css);
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(built) << fx.Diagnostic();
        ASSERT_TRUE(GiveBothControlsText(fx));
        ASSERT_TRUE(SelectAllAndFocus(fx, id)) << id;

        EXPECT_TRUE(HasRectWithFill(fx.Primitives(id), kGreenFocusedFill)) << id;
    }
}

// An rgba() selection-color keeps its alpha in proportion: 0x80 scaled by the focused 0x66/0xFF
// is 0x33, not the opaque colour's 0x66.
TEST(TextSelectionColor, AnAuthoredAlphaIsScaledNotReplaced)
{
    const std::string css = std::string(kGreyBorderCss) + "#root { selection-color: rgba(0, 255, 0, 0.5); }";
    const uint32_t kHalfGreenFocusedFill = PackFromARGB(0x3300FF00u);
    for (const char* id : {"field", "area"})
    {
        IsolatedUIFixture fx;
        const bool built = fx.Build(1.0f, kXml, css);
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(built) << fx.Diagnostic();
        ASSERT_TRUE(GiveBothControlsText(fx));
        ASSERT_TRUE(SelectAllAndFocus(fx, id)) << id;

        const auto prims = fx.Primitives(id);
        EXPECT_TRUE(HasRectWithFill(prims, kHalfGreenFocusedFill)) << id;
        EXPECT_FALSE(HasRectWithFill(prims, kGreenFocusedFill)) << id << ": authored alpha dropped";
    }
}

// A text area keeps its selection painted after it loses the focus, at the dimmer alpha.
TEST(TextSelectionColor, AnUnfocusedSelectionTakesTheUnfocusedAlpha)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, std::string(kGreyBorderCss) + kGreenRootCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_TRUE(GiveBothControlsText(fx));
    auto* area = dynamic_cast<TextArea*>(fx.Element("area"));
    ASSERT_NE(area, nullptr);
    area->SetSelection(0, kValueLength);
    fx.Settle();

    const auto prims = fx.Primitives("area");
    EXPECT_TRUE(HasRectWithFill(prims, kGreenUnfocusedFill));
    EXPECT_FALSE(HasRectWithFill(prims, kGreenFocusedFill));
}
