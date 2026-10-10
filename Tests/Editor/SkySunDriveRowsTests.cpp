// The sky inspector's drive set as an author meets it: a "Custom illuminance curve" switch, and the
// switch for the light's colour shown only under the curve, where the colour is the author's to choose;
// from the light the sky always sets it, so no switch is shown for it. Turning the curve on is the
// same undo step as before.

#include <gtest/gtest.h>

#include "Components/Rendering/Light.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "InspectorRegistry.h"
#include "Inspectors/SkySunDriveRows.h"
#include "UI/Controls/EnumField.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Toggle.h"
#include "UI/UIElement.h"
#include "UndoRedo/UndoRedoService.h"

#include <string>

using namespace GameEngine;

namespace
{
constexpr const char* kCustomCurve = "Custom illuminance curve";
constexpr const char* kSkySetsColour = "Sky sets the light's colour";

// The text of the first label under `node`, or empty.
std::string FirstLabelText(UIElement& node)
{
    for (const auto& child : node.GetChildren())
    {
        if (auto* label = dynamic_cast<Label*>(child.get()))
            return label->GetText();
        const std::string found = FirstLabelText(*child);
        if (!found.empty())
            return found;
    }
    return {};
}

// The inspector row whose label reads `text`, or null.
UIElement* RowOf(UIElement& node, const std::string& text)
{
    for (const auto& child : node.GetChildren())
    {
        if (child->HasClass("inspector-row") && FirstLabelText(*child) == text)
            return child.get();
        if (UIElement* found = RowOf(*child, text))
            return found;
    }
    return nullptr;
}

template <typename TControl>
TControl* ControlUnder(UIElement& node)
{
    for (const auto& child : node.GetChildren())
    {
        if (auto* control = dynamic_cast<TControl*>(child.get()))
            return control;
        if (TControl* found = ControlUnder<TControl>(*child))
            return found;
    }
    return nullptr;
}

class SkySunDriveRowsTests : public ::testing::Test
{
  protected:
    ECS::World World;
    Editor::UndoRedoService Undo;
    UIElement Root;
    ECS::EntityHandle Sky;

    // A sky linked to a directional light it drives (or to none), with its illuminance from `source`,
    // and its rows.
    void Build(Components::SkySunIlluminanceSource source, bool linked = true)
    {
        const ECS::EntityHandle light = World.CreateEntity();
        Components::Light directional{};
        directional.Type = Components::LightType::Directional;
        World.AddComponentImmediate(light, directional);
        World.AddComponentImmediate(light, Components::Transform{});
        Components::SkyEnvironment authored{};
        authored.SunLight = linked ? light : ECS::EntityHandle{};
        authored.TimeOfDayDrivesSunLight = true;
        authored.SunIlluminanceSource = source;
        Sky = World.CreateEntity();
        World.AddComponentImmediate(Sky, authored);
        InspectorContext ctx{};
        ctx.Parent = &Root;
        ctx.World = &World;
        ctx.Entity = Sky;
        ctx.Undo = &Undo;
        ctx.GetWorld = [this] { return &World; };
        AddSkySunDriveRows(&Root, ctx);
        if (source == Components::SkySunIlluminanceSource::Curve)
            AddSkySunCurveRows(&Root, ctx, {}, std::make_shared<bool>(false));
    }
};
} // namespace

TEST_F(SkySunDriveRowsTests, FromTheLightTheColourSwitchIsNotShown)
{
    Build(Components::SkySunIlluminanceSource::Light);
    UIElement* curveRow = RowOf(Root, kCustomCurve);
    ASSERT_NE(curveRow, nullptr) << "the custom curve is a switch of its own";
    Toggle* curveSwitch = ControlUnder<Toggle>(*curveRow);
    ASSERT_NE(curveSwitch, nullptr);
    EXPECT_FALSE(curveSwitch->GetValue());
    EXPECT_TRUE(curveSwitch->IsEnabled());
    EXPECT_EQ(RowOf(Root, kSkySetsColour), nullptr) << "from the light the sky always sets the colour";
    EXPECT_EQ(ControlUnder<EnumField<Components::SkySunIlluminanceSource>>(Root), nullptr)
        << "no dropdown in the drive set";
}

TEST_F(SkySunDriveRowsTests, UnderTheCurveTheColourIsTheAuthorsSwitch)
{
    Build(Components::SkySunIlluminanceSource::Curve);
    UIElement* colourRow = RowOf(Root, kSkySetsColour);
    ASSERT_NE(colourRow, nullptr) << "under the curve the colour is a choice";
    Toggle* colour = ControlUnder<Toggle>(*colourRow);
    ASSERT_NE(colour, nullptr);
    EXPECT_TRUE(colour->GetValue()) << "on by default";
    EXPECT_TRUE(colour->IsEnabled());
    UIElement* curveRow = RowOf(Root, kCustomCurve);
    ASSERT_NE(curveRow, nullptr);
    EXPECT_TRUE(ControlUnder<Toggle>(*curveRow)->GetValue());
}

TEST_F(SkySunDriveRowsTests, TurningTheCurveOnIsOneUndoStep)
{
    Build(Components::SkySunIlluminanceSource::Light);
    UIElement* curveRow = RowOf(Root, kCustomCurve);
    ASSERT_NE(curveRow, nullptr);
    Toggle* curveSwitch = ControlUnder<Toggle>(*curveRow);
    ASSERT_NE(curveSwitch, nullptr);
    curveSwitch->SetValue(true);
    EXPECT_EQ(World.GetComponent<Components::SkyEnvironment>(Sky)->SunIlluminanceSource,
              Components::SkySunIlluminanceSource::Curve);
    Undo.Undo();
    EXPECT_EQ(World.GetComponent<Components::SkyEnvironment>(Sky)->SunIlluminanceSource,
              Components::SkySunIlluminanceSource::Light);
}

// The curve opens with no key selected: Hour and Value read the curve at the playhead, greyed, and
// take no input until a click selects a key.
TEST_F(SkySunDriveRowsTests, TheCurveOpensWithNoKeySelected)
{
    Build(Components::SkySunIlluminanceSource::Curve);
    for (const char* label : {"Hour", "Value"})
    {
        UIElement* row = RowOf(Root, label);
        ASSERT_NE(row, nullptr) << label;
        FloatField* field = ControlUnder<FloatField>(*row);
        ASSERT_NE(field, nullptr) << label;
        EXPECT_FALSE(field->IsEnabled()) << label << " takes input with no key selected";
        EXPECT_FALSE(ControlUnder<Label>(*row)->IsEnabled()) << label << "'s label reads as editable";
    }
}

// A row the sky cannot use (no light to drive) reads as disabled: its label greys with its control.
TEST_F(SkySunDriveRowsTests, ARowWithNoLightToDriveGreysItsLabel)
{
    Build(Components::SkySunIlluminanceSource::Light, false);
    UIElement* curveRow = RowOf(Root, kCustomCurve);
    ASSERT_NE(curveRow, nullptr);
    Label* label = ControlUnder<Label>(*curveRow);
    ASSERT_NE(label, nullptr);
    EXPECT_FALSE(ControlUnder<Toggle>(*curveRow)->IsEnabled());
    EXPECT_FALSE(label->IsEnabled()) << "the label of a disabled row keeps its enabled colour";
}
