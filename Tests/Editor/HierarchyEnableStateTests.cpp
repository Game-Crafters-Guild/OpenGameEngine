#include <gtest/gtest.h>

#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Rendering/Light.h"
#include "Components/Transform.h"
#include "ECS/DisabledInHierarchySystem.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Editor/Hierarchy/HierarchyEnableState.h"
#include "Editor/Hierarchy/HierarchyLightIconTint.h"
#include "Editor/Hierarchy/HierarchyRowActivity.h"
#include "Inspectors/InspectorEntityActivity.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Toggle.h"
#include "UI/Controls/TreeView.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
using GameEngine::Label;
using GameEngine::Toggle;
using GameEngine::Components::Name;
using GameEngine::Components::Parent;
using GameEngine::Components::Transform;
using GameEngine::ECS::DisabledInHierarchySystem;
using GameEngine::ECS::Entity;
using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::World;
using GameEngine::Editor::CommitEntityEnabledToggle;
using GameEngine::Editor::DescribeEntityActivity;
using GameEngine::Editor::EntityActivityReason;
using GameEngine::Editor::InspectorEntityActivityPresenter;
using GameEngine::Editor::UndoRedoService;

// An entity as the editor creates one, with a Transform and a Name: an entity with no component at all
// has no archetype, and the ECS cannot switch it.
EntityHandle MakeNode(World& world, const char* name, EntityHandle parent = {})
{
    const EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate<Transform>(entity, Transform{});
    Name label{};
    std::strncpy(label.value, name, sizeof(label.value) - 1);
    world.AddComponentImmediate<Name>(entity, label);
    if (parent.IsValid())
        world.AddComponentImmediate<Parent>(entity, Parent{parent});
    return entity;
}

// A flat list of entities, one tree row each, tree id = index + 1.
class EntityRowsProvider final : public GameEngine::TreeChangeTrackingProvider
{
  public:
    EntityRowsProvider(World& world, std::vector<EntityHandle> entities)
        : m_World(world), m_Entities(std::move(entities))
    {
    }

    int GetRootCount() const override { return static_cast<int>(m_Entities.size()); }
    GameEngine::TreeId GetRootId(int index) const override { return static_cast<GameEngine::TreeId>(index) + 1; }
    int GetChildCount(GameEngine::TreeId) const override { return 0; }
    GameEngine::TreeId GetChildId(GameEngine::TreeId, int) const override { return 0; }
    const char* GetLabel(GameEngine::TreeId id) const override
    {
        return m_World.GetComponent<Name>(EntityOf(id))->value;
    }
    bool IsExpandable(GameEngine::TreeId) const override { return false; }

    EntityHandle EntityOf(GameEngine::TreeId id) const { return m_Entities[static_cast<std::size_t>(id - 1)]; }

  private:
    World& m_World;
    std::vector<EntityHandle> m_Entities;
};

// The rows the tree holds now: the children of its content element.
std::unordered_set<const GameEngine::UIElement*> LiveRows(GameEngine::UIElement& tree)
{
    std::unordered_set<const GameEngine::UIElement*> rows;
    std::vector<GameEngine::UIElement*> pending{&tree};
    while (!pending.empty())
    {
        GameEngine::UIElement* element = pending.back();
        pending.pop_back();
        if (element->HasClass("tree-item"))
        {
            rows.insert(element);
            continue;
        }
        for (const auto& child : element->GetChildren())
            pending.push_back(child.get());
    }
    return rows;
}

const GameEngine::UIElement* RowTitle(const GameEngine::UIElement& row)
{
    for (const auto& child : row.GetChildren())
    {
        if (child && child->HasClass("tree-title"))
            return child.get();
    }
    return nullptr;
}

GameEngine::UIElement* IconOf(GameEngine::UIElement& row)
{
    for (const auto& child : row.GetChildren())
    {
        if (child && child->HasClass("tree-icon-hit-target"))
            return child.get();
    }
    return nullptr;
}

// A Parent with 60 children in a real tree laid out on a headless device, each bound row shown through
// HierarchyRowActivity as the Hierarchy shows it, and every row the tree has ever bound recorded.
struct RowsInATree
{
    World World;
    DisabledInHierarchySystem Pass;
    EntityHandle Parent{};
    std::vector<EntityHandle> Entities;
    std::unique_ptr<EntityRowsProvider> Provider;
    GameEngine::Editor::HierarchyRowActivity RowActivity;
    std::unordered_set<const GameEngine::UIElement*> EverBound;
    std::unordered_set<GameEngine::TreeId> EverBoundIds;
    std::unordered_map<const GameEngine::UIElement*, GameEngine::TreeId> LastBoundId;
    std::unique_ptr<GameEngine::Rendering::IDevice> Device;
    std::unique_ptr<GameEngine::UIManager> Ui;
    GameEngine::TreeView* Tree = nullptr;

    bool Build()
    {
        Device = MakeHeadlessDevice();
        if (!Device)
            return false;
        Parent = MakeNode(World, "Parent");
        Entities.push_back(Parent);
        for (int i = 0; i < 60; ++i)
            Entities.push_back(MakeNode(World, ("Child " + std::to_string(i)).c_str(), Parent));
        Provider = std::make_unique<EntityRowsProvider>(World, Entities);

        auto root = std::make_unique<GameEngine::UIElement>();
        auto tree = std::make_unique<GameEngine::TreeView>();
        Tree = tree.get();
        Tree->SetDataProvider(Provider.get());
        Tree->SetOnRowBound([this](GameEngine::TreeId id, GameEngine::UIElement* row) { OnRowBound(id, *row); });
        Tree->Overrides()
            .Set(GameEngine::Style::Width, GameEngine::StyleLength::Px(320.0f))
            .Set(GameEngine::Style::Height, GameEngine::StyleLength::Px(900.0f));
        root->AddChild(std::move(tree));
        Ui = std::make_unique<GameEngine::UIManager>(Device.get());
        Ui->SetLayoutSizeOverride(400, 1000);
        Ui->SetRoot(std::move(root));
        Tree->RefreshFromProvider();
        Tick(3);
        return true;
    }

    void OnRowBound(GameEngine::TreeId id, GameEngine::UIElement& row)
    {
        EverBound.insert(&row);
        EverBoundIds.insert(id);
        LastBoundId[&row] = id;
        // The row's preview icon, as the Hierarchy gives each pooled row one: the tree latches an
        // icon click's hover preview off on it, and a Light's color tints it.
        if (!IconOf(row))
        {
            auto icon = std::make_unique<GameEngine::UIElement>();
            icon->AddClass("hierarchy-preview-icon");
            icon->AddClass("tree-icon-hit-target");
            row.AddChild(std::move(icon));
        }
        RowActivity.Present(World, Provider->EntityOf(id), id, row);
    }

    void Tick(int frames)
    {
        for (int i = 0; i < frames; ++i)
            Ui->Update(1.0f / 60.0f, /*interactive=*/false);
    }

    // The Hierarchy's frame: the panel's refresh, then the UI update.
    void Frame()
    {
        RowActivity.Refresh(World, *Tree, *Provider);
        Tick(1);
    }

    // Shrinks the tree until it trims its row pool, destroying rows, and returns how many rows it has
    // ever bound it no longer holds. The trim follows enough unchanged passes of its virtualization,
    // which runs when its provider's data changes, so the pass runs on every update here.
    std::size_t ShrinkUntilTrimmed()
    {
        Tree->Overrides().Set(GameEngine::Style::Height, GameEngine::StyleLength::Px(60.0f));
        for (int i = 0; i < 60; ++i)
        {
            Tree->EnqueuePumpWork(*Ui);
            Tick(1);
        }
        const std::unordered_set<const GameEngine::UIElement*> live = LiveRows(*Tree);
        std::size_t destroyed = 0;
        for (const GameEngine::UIElement* row : EverBound)
            destroyed += live.count(row) == 0 ? 1 : 0;
        return destroyed;
    }
};
} // namespace

// The toggle writes the switched entity's own state only. Its child keeps its own state and is
// inactive through the hierarchy pass while the parent is off, then active again when it comes back.
TEST(HierarchyEnableStateTests, SwitchingAParentOffWritesOnlyItsOwnState)
{
    World world;
    DisabledInHierarchySystem pass;
    const EntityHandle root = MakeNode(world, "Root");
    const EntityHandle child = MakeNode(world, "Child", root);
    UndoRedoService undo;

    const EntityHandle switched[] = {root};
    CommitEntityEnabledToggle(world, &undo, nullptr, switched, false);
    pass.Update(world, 0.0f);
    EXPECT_FALSE(Entity(&world, root).IsEnabled());
    EXPECT_TRUE(Entity(&world, child).IsEnabled()) << "the child's own state is untouched";
    EXPECT_FALSE(Entity(&world, child).IsEnabledInHierarchy()) << "the pass takes it out with its parent";
    ASSERT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_STREQ(undo.PeekUndoName(), "Switch Root off") << "the step names the entity it switched";

    undo.Undo();
    pass.Update(world, 0.0f);
    EXPECT_TRUE(Entity(&world, root).IsEnabled());
    EXPECT_TRUE(Entity(&world, child).IsEnabledInHierarchy());
}

// A click on a multi-selection is one undo step, and undo puts back each entity's own state: an
// entity that was already off stays off.
TEST(HierarchyEnableStateTests, AMultiSelectionIsOneUndoStepThatRestoresEachEntity)
{
    World world;
    const EntityHandle wasOn = MakeNode(world, "WasOn");
    const EntityHandle wasOff = MakeNode(world, "WasOff");
    Entity(&world, wasOff).SetEnabled(false);
    UndoRedoService undo;

    const EntityHandle selection[] = {wasOn, wasOff};
    CommitEntityEnabledToggle(world, &undo, nullptr, selection, false);
    EXPECT_FALSE(Entity(&world, wasOn).IsEnabled());
    ASSERT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_STREQ(undo.PeekUndoName(), "Switch 2 entities off");

    undo.Undo();
    EXPECT_TRUE(Entity(&world, wasOn).IsEnabled());
    EXPECT_FALSE(Entity(&world, wasOff).IsEnabled()) << "undo restores the state each entity had";
}

// A click that switches nothing records no undo step: an empty selection, or one whose entities
// already have the clicked state.
TEST(HierarchyEnableStateTests, AClickThatSwitchesNothingRecordsNoUndoStep)
{
    World world;
    const EntityHandle node = MakeNode(world, "Node");
    UndoRedoService undo;

    CommitEntityEnabledToggle(world, &undo, nullptr, {}, false);
    const EntityHandle alreadyOn[] = {node};
    CommitEntityEnabledToggle(world, &undo, nullptr, alreadyOn, true);
    EXPECT_EQ(undo.GetUndoCount(), 0u);
}

// The activity tells an entity switched off itself from one that is on under a switched-off
// ancestor, and names the nearest such ancestor; the words the hierarchy row and the inspector show
// follow from it.
TEST(HierarchyEnableStateTests, ActivityTellsSelfOffFromInactiveAndNamesTheNearestOffAncestor)
{
    World world;
    DisabledInHierarchySystem pass;
    const EntityHandle root = MakeNode(world, "Root");
    const EntityHandle middle = MakeNode(world, "Middle", root);
    const EntityHandle leaf = MakeNode(world, "Leaf", middle);
    const EntityHandle other = MakeNode(world, "Other");

    Entity(&world, root).SetEnabled(false);
    pass.Update(world, 0.0f);
    EXPECT_EQ(EntityActivityReason(world, DescribeEntityActivity(world, root)), "Off");
    EXPECT_EQ(EntityActivityReason(world, DescribeEntityActivity(world, leaf)), "Inactive: Root is off");
    EXPECT_TRUE(EntityActivityReason(world, DescribeEntityActivity(world, other)).empty());

    Entity(&world, middle).SetEnabled(false);
    pass.Update(world, 0.0f);
    EXPECT_EQ(DescribeEntityActivity(world, leaf).OffAncestor, middle) << "the nearest switched-off ancestor";
    EXPECT_EQ(EntityActivityReason(world, DescribeEntityActivity(world, leaf)), "Inactive: Middle is off");
}

// The inspector header follows the entity's own state and its derived state together: switching a
// child off and back on under a parent that stays off redraws the toggle and the reason line,
// although the derived state never changes.
TEST(HierarchyEnableStateTests, TheInspectorHeaderRedrawsWhenOnlyTheOwnStateChanges)
{
    World world;
    DisabledInHierarchySystem pass;
    const EntityHandle parent = MakeNode(world, "Parent");
    const EntityHandle child = MakeNode(world, "Child", parent);
    Entity(&world, parent).SetEnabled(false);
    pass.Update(world, 0.0f);

    Toggle toggle;
    Label reasonLine;
    InspectorEntityActivityPresenter presenter;
    presenter.Bind(&toggle, &reasonLine, false);
    presenter.Present(world, child);
    EXPECT_TRUE(toggle.HasClass("toggle-inactive"));
    EXPECT_EQ(toggle.GetTooltip(), "Switch the selected entity on or off. It is on, but inactive while Parent is "
                                   "off: switch Parent on to run it.");
    EXPECT_EQ(reasonLine.GetText(), "Inactive: Parent is off");
    EXPECT_FALSE(reasonLine.HasClass("hidden"));

    Entity(&world, child).SetEnabled(false);
    pass.Update(world, 0.0f);
    presenter.Present(world, child);
    EXPECT_FALSE(toggle.HasClass("toggle-inactive")) << "switched off itself, the toggle reads off, not muted";
    EXPECT_EQ(toggle.GetTooltip(), "Switch the selected entity on or off.");
    EXPECT_TRUE(reasonLine.HasClass("hidden"));

    Entity(&world, child).SetEnabled(true);
    pass.Update(world, 0.0f);
    presenter.Present(world, child);
    EXPECT_TRUE(toggle.HasClass("toggle-inactive")) << "back on under the parent that is off";
    EXPECT_FALSE(reasonLine.HasClass("hidden"));
}

// The derived state lands with the hierarchy pass a frame after a parent's switch, and the Hierarchy
// refreshes every frame. The rows follow the state through the tree: once the tree has shrunk and
// destroyed pooled rows, switching the parent off repaints, through the provider, the rows the tree
// still holds for its children, and binds no row it has let go of.
TEST(HierarchyEnableStateTests, RowsFollowTheDerivedStateThroughTheTreeAfterItDestroysPooledRows)
{
    RowsInATree rows;
    if (!rows.Build())
        GTEST_SKIP() << "no Vulkan device";
    ASSERT_GT(rows.ShrinkUntilTrimmed(), 0u) << "the tree destroyed no pooled row, so this test is vacuous";
    const std::unordered_set<const GameEngine::UIElement*> live = LiveRows(*rows.Tree);
    rows.Frame();

    Entity(&rows.World, rows.Parent).SetEnabled(false);
    rows.Frame();
    rows.Pass.Update(rows.World, 0.0f);
    rows.EverBound.clear();
    rows.Frame();
    rows.Tick(1);

    for (const GameEngine::UIElement* row : rows.EverBound)
        EXPECT_EQ(live.count(row), 1u) << "a row the tree no longer holds was bound";
    std::size_t childRows = 0;
    for (const GameEngine::UIElement* row : LiveRows(*rows.Tree))
    {
        const std::optional<bool> visible = row->Overrides().Get(GameEngine::Style::Visibility);
        const GameEngine::UIElement* title = RowTitle(*row);
        if ((visible.has_value() && !*visible) || !title || title->GetTextContent().rfind("Child", 0) != 0)
            continue;
        ++childRows;
        EXPECT_TRUE(title->HasClass("hierarchy-title-inactive"))
            << title->GetTextContent() << " still reads as active under its switched-off parent";
    }
    EXPECT_GT(childRows, 0u) << "no child row is left to check";
}

// An icon click latches the icon's hover preview off until the pointer leaves it. The latch belongs to
// the item it was clicked for: a pooled row that the tree binds to another item does not carry it.
TEST(HierarchyEnableStateTests, ARowBoundToAnotherItemDropsTheIconLatch)
{
    RowsInATree rows;
    if (!rows.Build())
        GTEST_SKIP() << "no Vulkan device";
    rows.ShrinkUntilTrimmed();
    rows.Frame();

    std::vector<GameEngine::UIElement*> latched;
    for (const GameEngine::UIElement* live : LiveRows(*rows.Tree))
    {
        auto* row = const_cast<GameEngine::UIElement*>(live);
        if (GameEngine::UIElement* icon = IconOf(*row))
        {
            icon->AddClass("tree-icon-hover-preview-suppressed");
            latched.push_back(row);
        }
    }
    ASSERT_FALSE(latched.empty()) << "no held row has an icon, so this test is vacuous";
    std::unordered_map<const GameEngine::UIElement*, GameEngine::TreeId> before;
    for (const GameEngine::UIElement* row : latched)
        before[row] = rows.LastBoundId[row];

    rows.Tree->ScrollToItem(static_cast<GameEngine::TreeId>(rows.Entities.size()));
    rows.Tick(3);

    std::size_t rebound = 0;
    const std::unordered_set<const GameEngine::UIElement*> live = LiveRows(*rows.Tree);
    for (GameEngine::UIElement* row : latched)
    {
        if (live.count(row) == 0 || rows.LastBoundId[row] == before[row])
            continue;
        ++rebound;
        EXPECT_FALSE(IconOf(*row)->HasClass("tree-icon-hover-preview-suppressed"))
            << "the row now bound to item " << rows.LastBoundId[row] << " kept item " << before[row]
            << "'s latch";
    }
    ASSERT_GT(rebound, 0u) << "no latched row was bound to another item, so this test is vacuous";
}

// A caller asks the tree for a row by the item it shows, never keeps the row: once the tree has
// trimmed its pool, every held item finds its own row and an item the tree let go of finds none.
TEST(HierarchyEnableStateTests, TheTreeFindsOnlyTheRowsItHolds)
{
    RowsInATree rows;
    if (!rows.Build())
        GTEST_SKIP() << "no Vulkan device";
    ASSERT_GT(rows.ShrinkUntilTrimmed(), 0u) << "the tree destroyed no pooled row, so this test is vacuous";
    rows.Frame();

    std::vector<GameEngine::TreeId> held;
    rows.Tree->CollectBoundIds(held);
    ASSERT_FALSE(held.empty());
    const std::unordered_set<const GameEngine::UIElement*> live = LiveRows(*rows.Tree);
    for (const GameEngine::TreeId id : held)
    {
        const GameEngine::UIElement* row = rows.Tree->FindBoundRow(id);
        ASSERT_NE(row, nullptr) << "held item " << id << " found no row";
        EXPECT_EQ(live.count(row), 1u);
        EXPECT_EQ(rows.Tree->BoundIdOf(row), id);
    }
    std::size_t letGo = 0;
    for (const GameEngine::TreeId id : rows.EverBoundIds)
    {
        if (std::find(held.begin(), held.end(), id) != held.end())
            continue;
        ++letGo;
        EXPECT_EQ(rows.Tree->FindBoundRow(id), nullptr) << "item " << id << " the tree let go of found a row";
    }
    EXPECT_GT(letGo, 0u) << "every item the tree bound is still held, so this test is vacuous";
}

// The refresh walks the rows the tree holds, not every id it was ever shown: once the tree has let a
// child's row go, switching that child off marks nothing on the provider.
TEST(HierarchyEnableStateTests, ARefreshForgetsTheRowsTheTreeNoLongerHolds)
{
    RowsInATree rows;
    if (!rows.Build())
        GTEST_SKIP() << "no Vulkan device";
    ASSERT_GT(rows.ShrinkUntilTrimmed(), 0u) << "the tree destroyed no pooled row, so this test is vacuous";
    rows.Frame();

    std::vector<GameEngine::TreeId> held;
    rows.Tree->CollectBoundIds(held);
    GameEngine::TreeId letGo = 0;
    for (const GameEngine::TreeId id : rows.EverBoundIds)
    {
        if (std::find(held.begin(), held.end(), id) == held.end())
            letGo = std::max(letGo, id);
    }
    ASSERT_NE(letGo, 0u) << "every row the tree bound is still held, so this test is vacuous";

    Entity(&rows.World, rows.Provider->EntityOf(letGo)).SetEnabled(false);
    const std::uint64_t before = rows.Provider->GetChangeVersion();
    rows.RowActivity.Refresh(rows.World, *rows.Tree, *rows.Provider);
    EXPECT_EQ(rows.Provider->GetChangeVersion(), before) << "a row the tree let go of was marked";
}

// A Light edit retints its row in place. The Inspector notifies it from inside its pointer event, so
// one pointer move of a color drag rebinds no row and leaves the other rows' tint alone.
TEST(HierarchyEnableStateTests, ALightEditInsideAnEventRetintsItsRowWithoutRebinding)
{
    RowsInATree rows;
    if (!rows.Build())
        GTEST_SKIP() << "no Vulkan device";
    rows.Ui->GetRootElement()->AddClass("hierarchy-icons-colored");
    const GameEngine::TreeId lightId = 2;
    const EntityHandle light = rows.Provider->EntityOf(lightId);
    GameEngine::Components::Light color{};
    color.Color[0] = 1.0f;
    color.Color[1] = 0.0f;
    color.Color[2] = 0.0f;
    rows.World.AddComponentImmediate<GameEngine::Components::Light>(light, color);
    GameEngine::UIElement* row = rows.Tree->FindBoundRow(lightId);
    ASSERT_NE(row, nullptr) << "the Light's row is not held, so this test is vacuous";
    std::vector<GameEngine::TreeId> held;
    rows.Tree->CollectBoundIds(held);
    ASSERT_GT(held.size(), 1u);
    rows.Frame();
    rows.EverBound.clear();

    GameEngine::UIElement::SetInEventDispatch(true);
    GameEngine::Editor::RetintHeldHierarchyLightRow(rows.World, light, *rows.Tree, lightId);
    GameEngine::UIElement::SetInEventDispatch(false);
    rows.Tick(2);

    EXPECT_EQ(rows.EverBound.size(), 0u) << "a tint change rebound rows";
    const std::optional<uint32_t> tint = IconOf(*row)->Overrides().Get(GameEngine::Style::BackgroundTint);
    ASSERT_TRUE(tint.has_value()) << "the Light's row was not tinted";
    EXPECT_EQ(*tint, 0xFFC80000u);
}
