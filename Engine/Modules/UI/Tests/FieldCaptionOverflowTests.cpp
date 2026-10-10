// A captioned field in a narrow inspector column: the caption ("(below the horizon, axis side)")
// stays on the field's one line, is cut with an ellipsis at the field's edge, and gives up its width
// before the value gives up any; a value wider than the field still stays inside it, at rest and
// while it is typed. A unit suffix ("%") lays out as it always has. The cut caption takes the
// pointer, so resting on it shows its whole text as the tooltip when nothing above it carries a
// tooltip of its own (a truncated label is its own tooltip, TooltipOverlay::FindTooltipText).
//
// Laid out and emitted over the SHIPPED sheets (theme/tokens.css, theme/core.css, theme/widgets.css
// and controls/FieldSuffix/FieldSuffix.css, staged beside this executable): the fix is CSS, and CSS
// a test wrote proves nothing about the CSS the editor loads. The editor attaches FieldSuffix.css to
// the suffix label's own subtree; here it is concatenated last into one sheet, the order the editor's
// cascade gives it.

#include "IsolatedUIFixture.h"

#include "Core/Application.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

using GameEngine::FloatField;
using GameEngine::Label;
using GameEngine::PathUtils;
using GameEngine::SuffixAlignment;
using GameEngine::SuffixRole;
using GameEngine::UIElement;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

constexpr char kCaption[] = "(below the horizon, axis side)";
constexpr float kValue = 200.0f;
// Layout rounds to a fraction of a pixel; geometry is compared to this tolerance.
constexpr float kLayoutTolerance = 0.5f;

std::string ReadShippedCss(const char* relative)
{
    std::ifstream in(PathUtils::GetExecutableDirectory() / "Assets" / "UI" / relative, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

UIElement* ChildWithClass(UIElement& parent, const char* className)
{
    for (const auto& child : parent.GetChildren())
    {
        if (child && child->HasClass(className))
            return child.get();
    }
    return nullptr;
}

float Bottom(const UIElement& element)
{
    return element.GetLayoutY() + element.GetLayoutHeight();
}

float Right(const UIElement& element)
{
    return element.GetLayoutX() + element.GetLayoutWidth();
}

// Where the field's content box ends: its right edge less its right padding and border.
float ContentRight(const UIElement& field)
{
    const auto& layout = field.GetResolvedStyle().Layout;
    return Right(field) - layout.Padding.Right - layout.BorderWidth.Right;
}

void Caption(FloatField& field, float value)
{
    field.SetSuffixRole(SuffixRole::Caption);
    field.SetSuffixAlignment(SuffixAlignment::AfterText);
    field.SetValueWithoutNotify(value);
    field.SetSuffix(kCaption);
}

// One FloatField per column, each column `Width` px wide and named `Id`, laid out and emitted over
// the shipped sheets, plus any markup and geometry of the test's own.
struct Columns
{
    struct Column
    {
        const char* Id;
        int Width;
    };

    IsolatedUIFixture Fixture;
    std::string Failure;
    std::string Skip;

    bool Build(std::initializer_list<Column> columns, const std::string& extraMarkup = {},
               const std::string& extraCss = {})
    {
        std::string css;
        for (const char* sheet : {"theme/tokens.css", "theme/core.css", "theme/widgets.css",
                                  "controls/FieldSuffix/FieldSuffix.css"})
        {
            const std::string text = ReadShippedCss(sheet);
            if (text.empty())
            {
                Failure = std::string("shipped stylesheet not staged beside the test: ") + sheet;
                return false;
            }
            css += text;
            css += '\n';
        }
        std::string xml = "<UIElement>";
        for (const Column& column : columns)
        {
            const std::string id = column.Id;
            css += "#" + id + " { display: flex; flex-direction: column; width: " + std::to_string(column.Width) +
                   "px; }\n";
            xml += "<UIElement id=\"" + id + "\"><FloatField id=\"" + id + "-field\" /></UIElement>";
        }
        xml += extraMarkup + "</UIElement>";
        css += extraCss;
        if (!Fixture.Build(1.0f, xml, css))
        {
            (Fixture.DeviceAvailable() ? Failure : Skip) = Fixture.Diagnostic();
            return false;
        }
        return true;
    }

    FloatField* Field(const char* columnId)
    {
        return dynamic_cast<FloatField*>(Fixture.Element(std::string(columnId) + "-field"));
    }

    void MoveTo(const UIElement& element)
    {
        Fixture.Manager().OnMouseMove(element.GetLayoutX() + element.GetLayoutWidth() * 0.5f,
                                      element.GetLayoutY() + element.GetLayoutHeight() * 0.5f);
        Fixture.StepFrame();
    }

    // Rests the pointer on `element` for a second of frames (past the hover delay and the frame the
    // bubble takes to measure its text) and returns what the manager's own tooltip shows, or "" when it
    // shows none (no bubble, or the bubble parked off screen).
    std::string RestOn(const UIElement& element)
    {
        MoveTo(element);
        constexpr int kFrames = 60;
        for (int i = 0; i < kFrames; ++i)
            Fixture.StepFrame();
        UIElement* root = Fixture.Manager().GetRootElement();
        const UIElement* bubble = root->FindById("ui-tooltip");
        const auto* text = dynamic_cast<const Label*>(root->FindById("ui-tooltip-text"));
        if (!bubble || !text || bubble->GetLayoutX() < 0.0f || bubble->GetLayoutY() < 0.0f)
            return {};
        return text->GetText();
    }
};

#define BUILD_OR_SKIP(columns, ...)                                                                   \
    if (!(columns).Build(__VA_ARGS__))                                                                \
    {                                                                                                 \
        if (!(columns).Skip.empty())                                                                  \
            GTEST_SKIP() << (columns).Skip;                                                           \
        FAIL() << (columns).Failure;                                                                  \
    }

} // namespace

// The caption is cut, on one line, inside the field; the value keeps the width it has in a column
// wide enough for both.
TEST(FieldCaptionOverflow, ANarrowFieldCutsItsCaptionOnOneLineBeforeItsValue)
{
    Columns columns;
    BUILD_OR_SKIP(columns, {{"narrow", 140}, {"wide", 400}});
    FloatField* narrow = columns.Field("narrow");
    FloatField* wide = columns.Field("wide");
    ASSERT_NE(narrow, nullptr);
    ASSERT_NE(wide, nullptr);
    Caption(*narrow, kValue);
    Caption(*wide, kValue);
    columns.Fixture.Settle();

    auto* caption = dynamic_cast<Label*>(ChildWithClass(*narrow, "field-caption"));
    auto* wideCaption = dynamic_cast<Label*>(ChildWithClass(*wide, "field-caption"));
    UIElement* value = ChildWithClass(*narrow, "field-editor");
    UIElement* wideValue = ChildWithClass(*wide, "field-editor");
    ASSERT_NE(caption, nullptr);
    ASSERT_NE(wideCaption, nullptr);
    ASSERT_NE(value, nullptr);
    ASSERT_NE(wideValue, nullptr);
    ASSERT_GT(wideValue->GetLayoutWidth(), 0.0f) << "no value measured, so a cut value cannot be told apart";
    ASSERT_FALSE(wideCaption->WasLastRunEllipsized()) << "the wide column is meant to hold the whole caption";

    EXPECT_GE(caption->GetLayoutY(), narrow->GetLayoutY() - kLayoutTolerance) << "the caption rises out of its field";
    EXPECT_LE(Bottom(*caption), Bottom(*narrow) + kLayoutTolerance)
        << "the caption wrapped below its field (" << caption->GetLayoutHeight() << " px tall in a "
        << narrow->GetLayoutHeight() << " px field)";
    EXPECT_LE(Right(*caption), ContentRight(*narrow) + kLayoutTolerance) << "the caption runs past its field";
    EXPECT_TRUE(caption->WasLastRunEllipsized()) << "the cut caption carries no ellipsis";
    EXPECT_NEAR(value->GetLayoutWidth(), wideValue->GetLayoutWidth(), kLayoutTolerance)
        << "the value gave up width while its caption still had some";
}

// A value wider than a very narrow field: once the caption has given what it can, the value gives way
// too, inside the field, as an uncaptioned field's value does.
TEST(FieldCaptionOverflow, AValueWiderThanItsFieldStaysInsideIt)
{
    Columns columns;
    BUILD_OR_SKIP(columns, {{"tiny", 80}});
    FloatField* field = columns.Field("tiny");
    ASSERT_NE(field, nullptr);
    Caption(*field, -123456.789f);
    columns.Fixture.Settle();

    UIElement* value = ChildWithClass(*field, "field-editor");
    ASSERT_NE(value, nullptr);
    ASSERT_GT(value->GetLayoutWidth(), 0.0f);
    EXPECT_LE(Right(*value), ContentRight(*field) + kLayoutTolerance)
        << "the value runs past its field: right edge " << Right(*value) << ", content edge " << ContentRight(*field);
}

// Typing hides the caption (it describes the stored value), so the value being typed has the whole
// field, and scrolls inside it rather than running past it.
TEST(FieldCaptionOverflow, AValueBeingTypedStaysInsideItsField)
{
    Columns columns;
    BUILD_OR_SKIP(columns, {{"typed", 120}});
    FloatField* field = columns.Field("typed");
    ASSERT_NE(field, nullptr);
    Caption(*field, kValue);
    columns.Fixture.Settle();

    UIElement* value = ChildWithClass(*field, "field-editor");
    ASSERT_NE(value, nullptr);
    columns.MoveTo(*value);
    auto& ui = columns.Fixture.Manager();
    ui.OnMouseButton(0, true);
    ui.OnMouseButton(0, false);
    columns.Fixture.StepFrame();
    ASSERT_FALSE(ui.GetFocusedElementId().empty()) << "the press did not start an edit";
    for (char c : std::string("1234567890.12345"))
        ui.OnChar(static_cast<unsigned int>(c));
    columns.Fixture.Settle();

    EXPECT_LE(Right(*value), ContentRight(*field) + kLayoutTolerance)
        << "the value being typed runs past its field: right edge " << Right(*value) << ", content edge "
        << ContentRight(*field);
}

// Every "%" and "px" field reads the same value rule a caption's field does, so a unit field keeps its
// layout: the value takes the field's free width and gives it up in a narrow field, and the unit stays
// at the field's end.
TEST(FieldCaptionOverflow, AUnitSuffixFieldLaysOutAsBefore)
{
    Columns columns;
    BUILD_OR_SKIP(columns, {{"unit", 140}, {"unit-tiny", 60}});
    for (const char* id : {"unit", "unit-tiny"})
    {
        FloatField* field = columns.Field(id);
        ASSERT_NE(field, nullptr);
        field->SetValueWithoutNotify(123456.0f);
        field->SetSuffix("%");
    }
    columns.Fixture.Settle();

    for (const char* id : {"unit", "unit-tiny"})
    {
        FloatField* field = columns.Field(id);
        UIElement* value = ChildWithClass(*field, "field-editor");
        UIElement* unit = ChildWithClass(*field, "field-suffix");
        ASSERT_NE(value, nullptr);
        ASSERT_NE(unit, nullptr);
        EXPECT_NEAR(Right(*unit), ContentRight(*field), kLayoutTolerance)
            << id << ": the unit is not at the field's end";
        EXPECT_LE(Right(*value), unit->GetLayoutX() + kLayoutTolerance) << id << ": the value overlaps its unit";
    }
    UIElement* tinyValue = ChildWithClass(*columns.Field("unit-tiny"), "field-editor");
    UIElement* wideValue = ChildWithClass(*columns.Field("unit"), "field-editor");
    EXPECT_LT(tinyValue->GetLayoutWidth(), wideValue->GetLayoutWidth())
        << "the value did not give way in a narrow field";
}

// Resting the pointer on the cut caption lands on the caption, and the tooltip shown is the caption's
// whole text; resting on a caption that fits shows none.
TEST(FieldCaptionOverflow, TheCutCaptionShowsItsWholeTextAsTheTooltip)
{
    Columns columns;
    BUILD_OR_SKIP(columns, {{"narrow", 140}, {"wide", 400}});
    FloatField* narrow = columns.Field("narrow");
    FloatField* wide = columns.Field("wide");
    ASSERT_NE(narrow, nullptr);
    ASSERT_NE(wide, nullptr);
    Caption(*narrow, kValue);
    Caption(*wide, kValue);
    columns.Fixture.Settle();
    UIElement* caption = ChildWithClass(*narrow, "field-caption");
    UIElement* fitting = ChildWithClass(*wide, "field-caption");
    ASSERT_NE(caption, nullptr);
    ASSERT_NE(fitting, nullptr);
    ASSERT_GT(caption->GetLayoutWidth(), 0.0f);

    auto& ui = columns.Fixture.Manager();
    const std::string fittingTooltip = columns.RestOn(*fitting);
    ASSERT_EQ(ui.GetHoveredElement(), fitting) << "the pointer on the fitting caption lands on "
                                               << ui.GetHoveredElementDebugName();
    EXPECT_EQ(fittingTooltip, "") << "a caption that fits brings up a tooltip";

    const std::string cutTooltip = columns.RestOn(*caption);
    ASSERT_EQ(ui.GetHoveredElement(), caption) << "the pointer on the caption lands on "
                                               << ui.GetHoveredElementDebugName();
    EXPECT_EQ(cutTooltip, kCaption);
}

// The inspector's row puts its tooltip on the row's label, beside the field rather than above it, so
// the cut caption in such a row is still its own tooltip. A tooltip on the field, or on any element
// above it, would answer first (TooltipOverlay::FindTooltipText).
TEST(FieldCaptionOverflow, InAnInspectorRowTheCutCaptionIsItsOwnTooltip)
{
    Columns columns;
    const std::string row = "<UIElement id=\"row\"><Label id=\"row-label\" /><UIElement id=\"row-field\">"
                            "<FloatField id=\"row-field-value\" /></UIElement></UIElement>";
    const std::string rowCss = "#row { display: flex; flex-direction: row; width: 260px; }\n"
                               "#row-label { width: 120px; }\n"
                               "#row-field { display: flex; flex-direction: column; flex-grow: 1; min-width: 0; }\n";
    BUILD_OR_SKIP(columns, {}, row, rowCss);
    auto* label = dynamic_cast<Label*>(columns.Fixture.Element("row-label"));
    auto* field = dynamic_cast<FloatField*>(columns.Fixture.Element("row-field-value"));
    ASSERT_NE(label, nullptr);
    ASSERT_NE(field, nullptr);
    label->SetText("Height at noon");
    label->SetTooltip("How high the sun stands at noon, in degrees.");
    Caption(*field, kValue);
    columns.Fixture.Settle();

    auto* caption = dynamic_cast<Label*>(ChildWithClass(*field, "field-caption"));
    ASSERT_NE(caption, nullptr);
    ASSERT_TRUE(caption->WasLastRunEllipsized()) << "the row is meant to cut the caption";
    EXPECT_EQ(columns.RestOn(*caption), kCaption);
}
