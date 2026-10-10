#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "Components/Name.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/UnresolvedComponentStore.h"
#include "ECS/World.h"
#include "InspectorLayoutFixture.h"
#include "Inspectors/PreservedFieldNotice.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

using GameEngine::Button;
using GameEngine::kEventButtonClick;
using GameEngine::Label;
using GameEngine::UIElement;
using GameEngine::UIEvent;
using GameEngine::ECS::ComponentTypeId;
using GameEngine::ECS::Entity;
using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::PreservedField;
using GameEngine::ECS::World;
using GameEngine::EditorUI::InspectorNotice;
using InspectorLayoutTesting::FindByClass;

using GameEngine::Editor::AddPreservedComponentFieldsNotice;
using GameEngine::Editor::AddPreservedFieldNotice;

namespace
{

ComponentTypeId NameType()
{
    return GameEngine::ECS::GetComponentTypeId<GameEngine::Components::Name>();
}

ComponentTypeId TransformType()
{
    return GameEngine::ECS::GetComponentTypeId<GameEngine::Components::Transform>();
}

// Records one field of `component` as unreadable-but-preserved, exactly as a degraded load leaves
// it: authored spellings kept verbatim, the raw text alongside.
void PreserveField(World& world, EntityHandle entity, ComponentTypeId typeId,
                   const char* component, const char* field, const char* rawText)
{
    PreservedField pf;
    pf.Component = component;
    pf.Field = field;
    pf.RawText = rawText;
    pf.TypeId = typeId;
    world.GetUnresolvedComponents().AddField(entity, std::move(pf));
}

// The notices the host was given, in the order they were emitted.
std::vector<InspectorNotice*> NoticesIn(const UIElement& parent)
{
    std::vector<InspectorNotice*> out;
    for (const auto& child : parent.GetChildren())
    {
        if (auto* notice = dynamic_cast<InspectorNotice*>(child.get()))
            out.push_back(notice);
    }
    return out;
}

std::vector<std::string> NoticeTextsIn(const UIElement& parent)
{
    std::vector<std::string> out;
    for (InspectorNotice* notice : NoticesIn(parent))
    {
        if (const auto* label = dynamic_cast<const Label*>(FindByClass(notice, "inspector-notice-text")))
            out.push_back(label->GetText());
    }
    return out;
}

// The discard buttons, in the order the notices were emitted: each notice's action while it is shown.
std::vector<Button*> DiscardButtonsIn(const UIElement& parent)
{
    std::vector<Button*> out;
    for (InspectorNotice* notice : NoticesIn(parent))
    {
        auto* action = dynamic_cast<Button*>(FindByClass(notice, "inspector-notice-action"));
        if (action && !action->HasClass("hidden") && action->GetText() == "Discard preserved value")
            out.push_back(action);
    }
    return out;
}

void Click(Button& button)
{
    UIEvent e{};
    e.Id = kEventButtonClick;
    e.Target = &button;
    e.CurrentTarget = &button;
    button.DispatchEvent(e);
}

} // namespace

// The gap this notice closes. A component with a registered custom inspector never reaches the
// reflection-driven path, so the per-field notice below is never emitted for it: the row shows the
// fallback, looks chosen, and a save writes the authored text instead. Nothing said so.
TEST(PreservedFieldNotice, ComponentLevelNoticeCarriesTheAuthoredTextAndNamesTheField)
{
    World world;
    Entity fence = world.Create();
    fence.Set(GameEngine::Components::Name{});
    world.ProcessCommands();

    PreserveField(world, fence.GetHandle(), NameType(), "SplineFence", "SpanGrade", "Cantilevered");

    UIElement host;
    AddPreservedComponentFieldsNotice(&host, world, fence.GetHandle(), NameType(), {});

    const std::vector<std::string> warnings = NoticeTextsIn(host);
    ASSERT_EQ(warnings.size(), 1u) << "one notice per preserved field of this component";
    EXPECT_NE(warnings[0].find("Cantilevered"), std::string::npos)
        << "the authored text is the thing the user cannot see anywhere else:\n"
        << warnings[0];
    EXPECT_NE(warnings[0].find("SpanGrade"), std::string::npos)
        << "hoisted to the component, nothing else says which field is meant:\n"
        << warnings[0];
    EXPECT_NE(warnings[0].find("could not be read"), std::string::npos) << warnings[0];

    EXPECT_EQ(DiscardButtonsIn(host).size(), 1u) << "and the one action that resolves it";
}

// A healthy component must get no banner at all — one on ordinary content teaches the user to
// ignore every one of them.
TEST(PreservedFieldNotice, ComponentWithNothingPreservedGetsNoNotice)
{
    World world;
    Entity clean = world.Create();
    clean.Set(GameEngine::Components::Name{});
    world.ProcessCommands();

    UIElement host;
    AddPreservedComponentFieldsNotice(&host, world, clean.GetHandle(), NameType(), {});

    EXPECT_TRUE(host.GetChildren().empty());
}

// The store is keyed per entity, not per component, so a notice that did not filter would report
// another component's unreadable value inside this section.
TEST(PreservedFieldNotice, OnlyTheSectionsOwnComponentIsListed)
{
    World world;
    Entity e = world.Create();
    e.Set(GameEngine::Components::Name{});
    world.ProcessCommands();

    PreserveField(world, e.GetHandle(), NameType(), "SplineFence", "SpanGrade", "Cantilevered");
    PreserveField(world, e.GetHandle(), TransformType(), "HLODVolume", "CellSize", "notanumber");

    UIElement host;
    AddPreservedComponentFieldsNotice(&host, world, e.GetHandle(), NameType(), {});

    const std::vector<std::string> warnings = NoticeTextsIn(host);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("SpanGrade"), std::string::npos) << warnings[0];
    EXPECT_EQ(warnings[0].find("notanumber"), std::string::npos)
        << "another component's preserved value has no business in this section:\n"
        << warnings[0];
}

// Every preserved field of the component gets its own notice and its own discard: one button that
// discarded all of them would make the fine-grained decision impossible to express.
TEST(PreservedFieldNotice, EachPreservedFieldGetsItsOwnNoticeAndDiscard)
{
    World world;
    Entity e = world.Create();
    e.Set(GameEngine::Components::Name{});
    world.ProcessCommands();

    PreserveField(world, e.GetHandle(), NameType(), "SplineFence", "SpanGrade", "Cantilevered");
    PreserveField(world, e.GetHandle(), NameType(), "SplineFence", "Seed", "0xdecaf");

    UIElement host;
    AddPreservedComponentFieldsNotice(&host, world, e.GetHandle(), NameType(), {});

    EXPECT_EQ(NoticeTextsIn(host).size(), 2u);
    EXPECT_EQ(DiscardButtonsIn(host).size(), 2u);
}

// Discarding is the only way to say "the fallback is what I want" in a way a save can act on, so
// the button has to reach the store — and tell the panel to rebuild, or the notice it just retired
// stays on screen.
TEST(PreservedFieldNotice, DiscardingFromTheComponentNoticeRetiresTheOverride)
{
    World world;
    Entity fence = world.Create();
    fence.Set(GameEngine::Components::Name{});
    world.ProcessCommands();

    PreserveField(world, fence.GetHandle(), NameType(), "SplineFence", "SpanGrade", "Cantilevered");

    bool refreshed = false;
    UIElement host;
    AddPreservedComponentFieldsNotice(&host, world, fence.GetHandle(), NameType(),
                                      [&refreshed]() { refreshed = true; });

    std::vector<Button*> buttons = DiscardButtonsIn(host);
    ASSERT_EQ(buttons.size(), 1u);

    const auto* before = world.GetUnresolvedComponents().FieldsFor(fence.GetHandle());
    ASSERT_NE(before, nullptr) << "precondition: the override is in the store before the click";

    Click(*buttons[0]);

    EXPECT_EQ(world.GetUnresolvedComponents().FieldsFor(fence.GetHandle()), nullptr)
        << "the discard must reach the store, not just the widget";
    EXPECT_TRUE(refreshed) << "the panel has to rebuild or the retired notice stays on screen";
}

// The reflection-driven path is untouched: its per-field notice sits over the row it describes, so
// it does NOT repeat the field name the row already carries.
TEST(PreservedFieldNotice, PerFieldNoticeStillCoversTheReflectionPath)
{
    World world;
    Entity volume = world.Create();
    volume.Set(GameEngine::Components::Name{});
    world.ProcessCommands();

    PreserveField(world, volume.GetHandle(), NameType(), "HLODVolume", "CellSize", "notanumber");

    UIElement host;
    AddPreservedFieldNotice(&host, world, volume.GetHandle(), NameType(), "CellSize", {});

    const std::vector<std::string> warnings = NoticeTextsIn(host);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("notanumber"), std::string::npos) << warnings[0];
    EXPECT_NE(warnings[0].find("The field below"), std::string::npos)
        << "the inline form points at the row underneath it:\n"
        << warnings[0];
    EXPECT_EQ(DiscardButtonsIn(host).size(), 1u);
}

// Authored spelling is what the file and the inspector carry; the loader's own matching is
// case-insensitive, and a notice that was stricter would show a discard that finds nothing.
TEST(PreservedFieldNotice, PerFieldLookupMatchesTheAuthoredSpellingCaseInsensitively)
{
    World world;
    Entity e = world.Create();
    e.Set(GameEngine::Components::Name{});
    world.ProcessCommands();

    PreserveField(world, e.GetHandle(), NameType(), "SplineFence", "spangrade", "Cantilevered");

    UIElement host;
    AddPreservedFieldNotice(&host, world, e.GetHandle(), NameType(), "SpanGrade", {});

    EXPECT_EQ(NoticeTextsIn(host).size(), 1u);
}
