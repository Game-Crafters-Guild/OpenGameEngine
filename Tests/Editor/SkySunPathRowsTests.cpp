// The sky inspector's sun path rows, driven the way an author drives them: typing into a field and
// dragging its label, with the inspector's poll running in between. An edit is one step from where
// it started, so a value passed on the way never costs a stored one, and once an edit ends every
// field shows what the sky stores, with its caption.

#include <gtest/gtest.h>

#include "Components/Rendering/SkyEnvironment.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Input/KeyCodes.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/SkySunPathCaptions.h"
#include "Inspectors/SkySunPathRows.h"
#include "UI/Controls/CollapsibleInfoCard.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UndoRedo/UndoRedoService.h"

#include <functional>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{
constexpr int kControlModifier = 0x0002;
constexpr const char* kLatitude = "Latitude (\xC2\xB0)";
constexpr const char* kDayOfYear = "Day of year";
constexpr const char* kNorth = "North (\xC2\xB0)";
constexpr const char* kAxisHeading = "Axis heading (\xC2\xB0)";
constexpr const char* kAxisAltitude = "Axis altitude (\xC2\xB0)";
constexpr const char* kHeightAtNoon = "Height at noon (\xC2\xB0)";

Label* RowLabel(UIElement& node, const std::string& text)
{
    for (const auto& child : node.GetChildren())
    {
        auto* label = dynamic_cast<Label*>(child.get());
        if (label && label->GetText() == text)
            return label;
        if (Label* found = RowLabel(*child, text))
            return found;
    }
    return nullptr;
}

template <typename TField>
TField* FieldUnder(UIElement& node)
{
    for (const auto& child : node.GetChildren())
    {
        if (auto* field = dynamic_cast<TField*>(child.get()))
            return field;
        if (TField* found = FieldUnder<TField>(*child))
            return found;
    }
    return nullptr;
}

TextInput* EditorOf(UIElement& field)
{
    for (const auto& child : field.GetChildren())
    {
        if (auto* editor = dynamic_cast<TextInput*>(child.get()))
            return editor;
    }
    return nullptr;
}

std::string CaptionOf(UIElement& field)
{
    for (const auto& child : field.GetChildren())
    {
        auto* label = dynamic_cast<Label*>(child.get());
        if (label && label->HasClass("field-caption"))
            return label->GetText();
    }
    return "<no caption>";
}

void SendMouse(UIElement& element, EventId id, float x)
{
    UIEvent event{};
    event.Id = id;
    event.X = x;
    event.Target = &element;
    event.CurrentTarget = &element;
    element.DispatchEvent(event);
}

class SkySunPathRowsTests : public ::testing::Test
{
  protected:
    ECS::World World;
    Editor::UndoRedoService Undo;
    UIElement Root;
    std::vector<std::function<void()>> RefreshCallbacks;
    ECS::EntityHandle Sky;

    // The rows for a sky carrying `authored`, with `alsoSelected` selected beside it.
    void Build(const Components::SkyEnvironment& authored, const std::vector<ECS::EntityHandle>& alsoSelected = {})
    {
        Sky = World.CreateEntity();
        World.AddComponentImmediate(Sky, authored);
        InspectorContext ctx{};
        ctx.Parent = &Root;
        ctx.World = &World;
        ctx.Entity = Sky;
        if (!alsoSelected.empty())
        {
            ctx.Entities = {Sky};
            ctx.Entities.insert(ctx.Entities.end(), alsoSelected.begin(), alsoSelected.end());
        }
        ctx.Undo = &Undo;
        ctx.GetWorld = [this] { return &World; };
        ctx.SimulationRefreshCallbacks = &RefreshCallbacks;
        AddSkySunPathRows(&Root, ctx);
    }

    static Components::SkyEnvironment CustomSky(float axisAltitude, float noonHeight)
    {
        Components::SkyEnvironment authored{};
        authored.SunPath = Components::SkySunPathKind::Custom;
        authored.CustomAxisAltitude = axisAltitude;
        authored.CustomNoonHeight = noonHeight;
        return authored;
    }

    void BuildCustom(float axisAltitude, float noonHeight, const std::vector<ECS::EntityHandle>& alsoSelected = {})
    {
        Build(CustomSky(axisAltitude, noonHeight), alsoSelected);
    }

    const Components::SkyEnvironment& Stored(ECS::EntityHandle sky)
    {
        return *World.GetComponent<Components::SkyEnvironment>(sky);
    }
    const Components::SkyEnvironment& Stored() { return Stored(Sky); }

    // What the sky system does every frame while the day cycle runs: it moves the sky's hour on.
    void AdvanceTheDay(float hours)
    {
        World.GetComponentForWrite<Components::SkyEnvironment>(Sky)->TimeOfDayHours += hours;
    }

    // The Custom path's hint card, above its rows.
    const EditorUI::CollapsibleInfoCard* Hint() { return FieldUnder<EditorUI::CollapsibleInfoCard>(Root); }

    // The inspector's poll, which runs several times between an author's keystrokes.
    void Poll()
    {
        for (const auto& refresh : RefreshCallbacks)
            refresh();
    }

    // The field of the row `rowLabel` names: the nearest one around the label.
    template <typename TField>
    TField* Field(const char* rowLabel)
    {
        Label* label = RowLabel(Root, rowLabel);
        for (UIElement* around = label ? label->GetParent() : nullptr; around && around != &Root;
             around = around->GetParent())
        {
            if (TField* field = FieldUnder<TField>(*around))
                return field;
        }
        return nullptr;
    }

    // Click into the field, which selects its text, and type over it one key at a time.
    template <typename TField>
    void Type(TField& field, const std::string& text)
    {
        TextInput* editor = EditorOf(field);
        ASSERT_NE(editor, nullptr);
        field.OnFocusChanged(true);
        editor->OnKey(Input::kKeyCode_A, kControlModifier, nullptr);
        for (const char key : text)
        {
            editor->OnChar(static_cast<unsigned char>(key));
            Poll();
        }
    }

    template <typename TField>
    void PressEnter(TField& field)
    {
        TextInput* editor = EditorOf(field);
        ASSERT_NE(editor, nullptr);
        editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
        Poll();
    }

    template <typename TField>
    void PressEscape(TField& field)
    {
        TextInput* editor = EditorOf(field);
        ASSERT_NE(editor, nullptr);
        editor->OnKey(Input::kKeyCode_Escape, 0, nullptr);
        Poll();
    }

    template <typename TField>
    void TypeAndCommit(TField& field, const std::string& text)
    {
        Type(field, text);
        PressEnter(field);
    }
};
} // namespace

TEST_F(SkySunPathRowsTests, TypingAnAxisAltitudeKeepsTheHeightTheFinalAxisReaches)
{
    BuildCustom(75.0f, -50.0f);
    auto* altitude = Field<FloatField>(kAxisAltitude);
    auto* noon = Field<FloatField>(kHeightAtNoon);
    ASSERT_NE(altitude, nullptr);
    ASSERT_NE(noon, nullptr);

    // "7" on the way to "70": an axis 7 degrees up reaches no lower than -7.
    Type(*altitude, "7");
    EXPECT_EQ(Stored().CustomAxisAltitude, 7.0f);
    EXPECT_EQ(noon->GetValue(), -7.0f) << "the row shows the height the path uses meanwhile";
    EXPECT_EQ(CaptionOf(*noon), NoonHeightCaption(-7.0f, 7.0f));

    EditorOf(*altitude)->OnChar('0');
    Poll();
    EXPECT_EQ(noon->GetValue(), -50.0f);
    PressEnter(*altitude);

    EXPECT_EQ(Stored().CustomAxisAltitude, 70.0f);
    EXPECT_EQ(Stored().CustomNoonHeight, -50.0f) << "an axis 70 degrees up reaches -50: the edit must not cut it";
    EXPECT_EQ(noon->GetValue(), -50.0f);
    EXPECT_EQ(CaptionOf(*noon), NoonHeightCaption(-50.0f, 70.0f));
}

TEST_F(SkySunPathRowsTests, DraggingTheAxisAltitudeDownAndBackKeepsTheHeight)
{
    BuildCustom(75.0f, -50.0f);
    Label* label = RowLabel(Root, kAxisAltitude);
    ASSERT_NE(label, nullptr);
    constexpr float kPressX = 4000.0f;
    const float pixelsPerDegree = 1.0f / InspectorDrag::kInspectorDragFloatSensitivity;

    SendMouse(*label, kEventMouseDown, kPressX);
    SendMouse(*label, kEventMouseMove, kPressX - 70.0f * pixelsPerDegree);
    Poll();
    EXPECT_NEAR(Stored().CustomAxisAltitude, 5.0f, 1e-3f);
    SendMouse(*label, kEventMouseMove, kPressX);
    Poll();
    SendMouse(*label, kEventMouseUp, kPressX);
    Poll();

    EXPECT_EQ(Stored().CustomAxisAltitude, 75.0f);
    EXPECT_EQ(Stored().CustomNoonHeight, -50.0f);
    EXPECT_EQ(Undo.GetUndoCount(), 0u) << "a drag that ends where it started changed nothing";
}

// Escape in the middle of typing takes the edit back: the axis the edit started from, and the height.
TEST_F(SkySunPathRowsTests, EscapeInTheMiddleOfAnAltitudeEditLeavesTheSkyAsItWas)
{
    BuildCustom(75.0f, -50.0f);
    auto* altitude = Field<FloatField>(kAxisAltitude);
    auto* noon = Field<FloatField>(kHeightAtNoon);
    ASSERT_NE(altitude, nullptr);
    ASSERT_NE(noon, nullptr);

    Type(*altitude, "7");
    ASSERT_EQ(Stored().CustomAxisAltitude, 7.0f);
    PressEscape(*altitude);

    EXPECT_EQ(Stored().CustomAxisAltitude, 75.0f);
    EXPECT_EQ(Stored().CustomNoonHeight, -50.0f);
    EXPECT_EQ(altitude->GetValue(), 75.0f);
    EXPECT_EQ(noon->GetValue(), -50.0f);
    EXPECT_EQ(Undo.GetUndoCount(), 0u) << "an edit taken back changed nothing";
}

// With two skies selected the edit moves both axes, and each keeps its own height: the one the
// inspector shows and the one beside it.
TEST_F(SkySunPathRowsTests, AnAltitudeEditOnTwoSkiesKeepsEachSkysHeight)
{
    const ECS::EntityHandle second = World.CreateEntity();
    World.AddComponentImmediate(second, CustomSky(75.0f, -40.0f));
    BuildCustom(75.0f, -50.0f, {second});
    auto* altitude = Field<FloatField>(kAxisAltitude);
    ASSERT_NE(altitude, nullptr);

    Type(*altitude, "7");
    EXPECT_EQ(Stored(second).CustomAxisAltitude, 7.0f);
    EXPECT_EQ(Stored(second).CustomNoonHeight, -40.0f);
    EditorOf(*altitude)->OnChar('0');
    Poll();
    PressEnter(*altitude);

    EXPECT_EQ(Stored().CustomAxisAltitude, 70.0f);
    EXPECT_EQ(Stored().CustomNoonHeight, -50.0f);
    EXPECT_EQ(Stored(second).CustomAxisAltitude, 70.0f);
    EXPECT_EQ(Stored(second).CustomNoonHeight, -40.0f);
    EXPECT_EQ(Undo.GetUndoCount(), 1u);
    Undo.Undo();
    EXPECT_EQ(Stored(second).CustomAxisAltitude, 75.0f);
    EXPECT_EQ(Stored(second).CustomNoonHeight, -40.0f);
}

// The day cycle writes the sky's hour every frame. An edit made while it runs writes the axis over
// the hour the day has reached, never back to the hour the edit began at.
TEST_F(SkySunPathRowsTests, AnAltitudeEditWhileTheDayRunsKeepsTheHourTheDayHasReached)
{
    BuildCustom(75.0f, -50.0f);
    auto* altitude = Field<FloatField>(kAxisAltitude);
    ASSERT_NE(altitude, nullptr);
    const float startHours = Stored().TimeOfDayHours;

    Type(*altitude, "7");
    AdvanceTheDay(0.5f);
    EditorOf(*altitude)->OnChar('0');
    Poll();
    EXPECT_EQ(Stored().TimeOfDayHours, startHours + 0.5f) << "a preview keeps the hour the day has reached";
    AdvanceTheDay(0.5f);
    PressEnter(*altitude);

    EXPECT_EQ(Stored().CustomAxisAltitude, 70.0f);
    EXPECT_EQ(Stored().CustomNoonHeight, -50.0f);
    EXPECT_EQ(Stored().TimeOfDayHours, startHours + 1.0f) << "the commit keeps it too";
}

// The commit brings the stored height into the new axis's reach, and one undo takes both back.
TEST_F(SkySunPathRowsTests, CommittingAnAxisAltitudeBringsTheHeightIntoReachInOneUndoStep)
{
    BuildCustom(75.0f, -50.0f);
    auto* altitude = Field<FloatField>(kAxisAltitude);
    ASSERT_NE(altitude, nullptr);

    TypeAndCommit(*altitude, "7");
    EXPECT_EQ(Stored().CustomAxisAltitude, 7.0f);
    EXPECT_EQ(Stored().CustomNoonHeight, -7.0f);
    EXPECT_EQ(Undo.GetUndoCount(), 1u);

    Undo.Undo();
    EXPECT_EQ(Stored().CustomAxisAltitude, 75.0f);
    EXPECT_EQ(Stored().CustomNoonHeight, -50.0f);
}

TEST_F(SkySunPathRowsTests, ACommittedNorthShowsTheStoredHeadingAndItsCaption)
{
    Build(Components::SkyEnvironment{});
    auto* north = Field<FloatField>(kNorth);
    ASSERT_NE(north, nullptr);

    Type(*north, "365");
    EXPECT_EQ(Stored().NorthHeading, 5.0f);
    EXPECT_EQ(EditorOf(*north)->GetValue(), "365") << "what is being typed stays until it is committed";
    PressEnter(*north);
    EXPECT_EQ(north->GetValue(), 5.0f) << "a heading past the full turn is stored wrapped, and shown as stored";
    EXPECT_EQ(EditorOf(*north)->GetValue(), "5");

    TypeAndCommit(*north, "90");
    EXPECT_EQ(Stored().NorthHeading, 90.0f);
    EXPECT_EQ(CaptionOf(*north), HeadingCaption(90.0f));
    north->OnFocusChanged(false);
    EXPECT_EQ(CaptionOf(*north), "(along\xC2\xA0+X)");

    TypeAndCommit(*north, "-10");
    EXPECT_EQ(Stored().NorthHeading, 350.0f);
    EXPECT_EQ(north->GetValue(), 350.0f);

    TypeAndCommit(*north, "360");
    EXPECT_EQ(north->GetValue(), 0.0f);
    EXPECT_EQ(CaptionOf(*north), "(along\xC2\xA0+Z)");
}

TEST_F(SkySunPathRowsTests, ACommittedAxisHeadingShowsTheStoredHeadingAndItsCaption)
{
    BuildCustom(30.0f, 60.0f);
    auto* heading = Field<FloatField>(kAxisHeading);
    ASSERT_NE(heading, nullptr);

    TypeAndCommit(*heading, "365");
    EXPECT_EQ(Stored().CustomAxisHeading, 5.0f);
    EXPECT_EQ(heading->GetValue(), 5.0f);

    TypeAndCommit(*heading, "270");
    EXPECT_EQ(CaptionOf(*heading), HeadingCaption(270.0f));
}

TEST_F(SkySunPathRowsTests, ACommittedAxisAltitudeShowsItsCaption)
{
    BuildCustom(30.0f, 60.0f);
    auto* altitude = Field<FloatField>(kAxisAltitude);
    ASSERT_NE(altitude, nullptr);

    TypeAndCommit(*altitude, "90");
    ASSERT_FALSE(AxisAltitudeCaption(90.0f).empty());
    EXPECT_EQ(CaptionOf(*altitude), AxisAltitudeCaption(90.0f));
}

TEST_F(SkySunPathRowsTests, ACommittedHeightAtNoonShowsItsCaption)
{
    BuildCustom(75.0f, 60.0f);
    auto* noon = Field<FloatField>(kHeightAtNoon);
    ASSERT_NE(noon, nullptr);

    TypeAndCommit(*noon, "-20");
    EXPECT_EQ(Stored().CustomNoonHeight, -20.0f);
    EXPECT_EQ(CaptionOf(*noon), "(below, far side)");
}

TEST_F(SkySunPathRowsTests, ACommittedLatitudeAndDayShowTheirCaptions)
{
    Build(Components::SkyEnvironment{});
    auto* latitude = Field<FloatField>(kLatitude);
    auto* day = Field<IntField>(kDayOfYear);
    ASSERT_NE(latitude, nullptr);
    ASSERT_NE(day, nullptr);

    TypeAndCommit(*latitude, "45");
    ASSERT_FALSE(LatitudeCaption(45.0f).empty());
    EXPECT_EQ(CaptionOf(*latitude), LatitudeCaption(45.0f));

    TypeAndCommit(*day, "172");
    EXPECT_EQ(Stored().DayOfYear, 172);
    EXPECT_EQ(CaptionOf(*day), "(21 June)");
}

// A height the axis cannot reach, from a scene file or a tool, is shown as the path uses it.
TEST_F(SkySunPathRowsTests, AStoredHeightOutOfReachIsShownAsThePathUsesIt)
{
    BuildCustom(75.0f, 200.0f);
    auto* noon = Field<FloatField>(kHeightAtNoon);
    ASSERT_NE(noon, nullptr);

    EXPECT_EQ(noon->GetValue(), 105.0f) << "an axis 75 degrees up reaches from -75 to 105";
    EXPECT_EQ(CaptionOf(*noon), NoonHeightCaption(105.0f, 75.0f));
    EXPECT_EQ(Stored().CustomNoonHeight, 200.0f) << "showing the sky does not edit it";
}

// A Height at noon at either end of its reach puts the sun where the axis points, or straight opposite,
// where it does not move: the hint above the rows says so, and says the sun circles again once the
// height moves off the end.
TEST_F(SkySunPathRowsTests, TheCustomHintSaysAStillSunDoesNotMove)
{
    BuildCustom(60.0f, 120.0f);
    const EditorUI::CollapsibleInfoCard* hint = Hint();
    ASSERT_NE(hint, nullptr);
    EXPECT_EQ(hint->GetFullText(), "The sun sits where the axis points, or straight opposite, and does not move.");

    auto* noon = Field<FloatField>(kHeightAtNoon);
    ASSERT_NE(noon, nullptr);
    TypeAndCommit(*noon, "60");
    EXPECT_EQ(hint->GetFullText(),
              "The sun circles the axis once a day, and at 12:00 it stands where Height at noon puts it.");
}
