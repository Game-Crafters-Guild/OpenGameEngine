// Focus release when the focused element leaves the tree.
//
// A control that commits its edit on FocusOut (a text field, a path field, a preference
// row) loses the edit when it is destroyed or unmounted without hearing FocusOut. These
// pin the two ways it can leave:
//   * destruction (RemoveChild of it or of an ancestor, a root replace): FocusOut is
//     dispatched synchronously, before the detach events and while the tree is whole, and
//     the focus id stops naming the dead element;
//   * unmount (a panel taken off its Mount): the element survives, so the next focus
//     notification reaches it although the root no longer does.
// The edge rule holds throughout: an element never told FocusIn is never told FocusOut.
//
// CPU only. A UIManager without a device never reaches NotifyFocusChange from Update, so
// UIManagerFocusAccess announces focus changes at the point a frame would.
#include <gtest/gtest.h>

#include "UIManagerFocusAccess.h"

#include "UI/Controls/Mount.h"
#include "UI/Controls/TextField.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

std::unique_ptr<UIElement> MakeElement(const char* id)
{
    auto el = std::make_unique<UIElement>();
    el->SetId(id);
    return el;
}

struct FocusLog
{
    std::vector<std::string> Order;

    int CountOf(const std::string& entry) const
    {
        int n = 0;
        for (const std::string& e : Order)
            if (e == entry)
                ++n;
        return n;
    }
};

void WatchFocus(UIElement& el, const std::string& name, FocusLog& log)
{
    el.RegisterEventHandler(kEventFocusIn, [&log, name](UIEvent&) { log.Order.push_back("in:" + name); });
    el.RegisterEventHandler(kEventFocusOut, [&log, name](UIEvent&) { log.Order.push_back("out:" + name); });
}

// root
//  +- panel
//  |   +- field
//  +- other
struct PanelTree
{
    std::unique_ptr<UIElement> Root;
    UIElement* Panel = nullptr;
    UIElement* Field = nullptr;
    UIElement* Other = nullptr;
};

PanelTree BuildPanelTree()
{
    PanelTree t;
    t.Root = MakeElement("root");
    auto panel = MakeElement("panel");
    auto field = MakeElement("field");
    auto other = MakeElement("other");
    t.Panel = panel.get();
    t.Field = field.get();
    t.Other = other.get();
    panel->AddChild(std::move(field));
    t.Root->AddChild(std::move(panel));
    t.Root->AddChild(std::move(other));
    return t;
}

} // namespace

TEST(FocusReleaseOnRemoval, RemovingTheFocusedElementClearsTheFocusId)
{
    UIManager ui{nullptr};
    PanelTree t = BuildPanelTree();
    UIElement* panel = t.Panel;
    UIElement* field = t.Field;
    ui.SetRoot(std::move(t.Root));
    ui.SetFocusById("field");

    panel->RemoveChild(field);

    EXPECT_EQ(ui.GetFocusedElementId(), "") << "focus still names an element that no longer exists";
}

TEST(FocusReleaseOnRemoval, RemovingAnAncestorOfTheFocusedElementClearsTheFocusId)
{
    UIManager ui{nullptr};
    PanelTree t = BuildPanelTree();
    UIElement* root = t.Root.get();
    UIElement* panel = t.Panel;
    ui.SetRoot(std::move(t.Root));
    ui.SetFocusById("field");

    root->RemoveChild(panel);

    EXPECT_EQ(ui.GetFocusedElementId(), "");
}

// The modal shape: the backdrop holds focus, and closing the modal removes the backdrop's
// parent. The backdrop dies with it, so it hears FocusOut and focus stops naming it.
TEST(FocusReleaseOnRemoval, ClosingAModalReleasesFocusFromItsDestroyedBackdrop)
{
    UIManager ui{nullptr};
    auto root = MakeElement("root");
    auto modalOwned = MakeElement("modal");
    auto backdropOwned = MakeElement("backdrop");
    UIElement* rootEl = root.get();
    UIElement* modal = modalOwned.get();
    FocusLog log;
    WatchFocus(*backdropOwned, "backdrop", log);
    modalOwned->AddChild(std::move(backdropOwned));
    root->AddChild(std::move(modalOwned));
    ui.SetRoot(std::move(root));
    ui.SetFocusById("backdrop");
    UIManagerFocusAccess::AnnounceFocusChange(ui);

    rootEl->RemoveChild(modal);

    EXPECT_EQ(log.CountOf("out:backdrop"), 1);
    EXPECT_EQ(ui.GetFocusedElementId(), "") << "focus still names the destroyed backdrop";
}

TEST(FocusReleaseOnRemoval, RemovingASiblingKeepsFocus)
{
    UIManager ui{nullptr};
    PanelTree t = BuildPanelTree();
    UIElement* root = t.Root.get();
    UIElement* other = t.Other;
    FocusLog log;
    WatchFocus(*t.Field, "field", log);
    ui.SetRoot(std::move(t.Root));
    ui.SetFocusById("field");
    UIManagerFocusAccess::AnnounceFocusChange(ui);

    root->RemoveChild(other);

    EXPECT_EQ(ui.GetFocusedElementId(), "field");
    EXPECT_EQ(log.CountOf("out:field"), 0);
}

// Focus is resolved by id, and the manager acts on the element that id resolves to. A
// doomed element elsewhere that happens to carry the same id is not the focused one.
TEST(FocusReleaseOnRemoval, RemovingADifferentElementWithTheSameIdKeepsFocus)
{
    UIManager ui{nullptr};
    PanelTree t = BuildPanelTree();
    UIElement* root = t.Root.get();
    auto twinOwned = MakeElement("field");
    UIElement* twin = twinOwned.get();
    root->AddChild(std::move(twinOwned));
    ui.SetRoot(std::move(t.Root));
    ui.SetFocusById("field");
    ASSERT_NE(root->FindById("field"), twin) << "the precondition is that the id resolves to the first field";

    root->RemoveChild(twin);

    EXPECT_EQ(ui.GetFocusedElementId(), "field");
}

TEST(FocusReleaseOnRemoval, DestroyingTheFocusedElementDispatchesFocusOutBeforeRemovalReturns)
{
    UIManager ui{nullptr};
    PanelTree t = BuildPanelTree();
    UIElement* root = t.Root.get();
    UIElement* panel = t.Panel;
    FocusLog log;
    WatchFocus(*t.Field, "field", log);
    ui.SetRoot(std::move(t.Root));
    ui.SetFocusById("field");
    UIManagerFocusAccess::AnnounceFocusChange(ui);
    ASSERT_EQ(log.CountOf("in:field"), 1);

    root->RemoveChild(panel);

    EXPECT_EQ(log.CountOf("out:field"), 1) << "the field died without hearing FocusOut";
    EXPECT_EQ(ui.GetFocusedElementId(), "");

    // Announced once: the next frame has nothing left to tell anyone.
    UIManagerFocusAccess::AnnounceFocusChange(ui);
    EXPECT_EQ(log.CountOf("out:field"), 1);
}

// The commit handler reads state the detach handlers may tear down, so it runs first.
TEST(FocusReleaseOnRemoval, FocusOutPrecedesTheDestructionDetach)
{
    UIManager ui{nullptr};
    PanelTree t = BuildPanelTree();
    UIElement* root = t.Root.get();
    UIElement* panel = t.Panel;
    FocusLog log;
    WatchFocus(*t.Field, "field", log);
    t.Field->RegisterEventHandler(kEventDetachedFromPanel, [&log](UIEvent&) { log.Order.push_back("detach:field"); });
    ui.SetRoot(std::move(t.Root));
    ui.SettleAttachTransitions();
    ui.SetFocusById("field");
    UIManagerFocusAccess::AnnounceFocusChange(ui);
    log.Order.clear();

    root->RemoveChild(panel);

    EXPECT_EQ(log.Order, (std::vector<std::string>{"out:field", "detach:field"}));
}

// The edge rule: focus that was set but never announced is dropped without a FocusOut.
TEST(FocusReleaseOnRemoval, AnElementNeverToldFocusInIsNotToldFocusOut)
{
    UIManager ui{nullptr};
    PanelTree t = BuildPanelTree();
    UIElement* root = t.Root.get();
    UIElement* panel = t.Panel;
    FocusLog log;
    WatchFocus(*t.Field, "field", log);
    ui.SetRoot(std::move(t.Root));
    ui.SetFocusById("field");

    root->RemoveChild(panel);

    EXPECT_TRUE(log.Order.empty());
    EXPECT_EQ(ui.GetFocusedElementId(), "");
}

// Focus moved this frame and not yet announced: the announced element is the one that
// dies, so it hears FocusOut now, and the new focus still gets its FocusIn next frame.
TEST(FocusReleaseOnRemoval, FocusMovedBeforeTheAnnouncementStillReachesTheNewTarget)
{
    UIManager ui{nullptr};
    PanelTree t = BuildPanelTree();
    UIElement* root = t.Root.get();
    UIElement* panel = t.Panel;
    FocusLog log;
    WatchFocus(*t.Field, "field", log);
    WatchFocus(*t.Other, "other", log);
    ui.SetRoot(std::move(t.Root));
    ui.SetFocusById("field");
    UIManagerFocusAccess::AnnounceFocusChange(ui);
    ui.SetFocusById("other");

    root->RemoveChild(panel);
    UIManagerFocusAccess::AnnounceFocusChange(ui);

    EXPECT_EQ(log.Order, (std::vector<std::string>{"in:field", "out:field", "in:other"}));
    EXPECT_EQ(ui.GetFocusedElementId(), "other");
}

// A FocusOut handler that moves focus keeps it.
TEST(FocusReleaseOnRemoval, AFocusOutHandlerThatMovesFocusKeepsIt)
{
    UIManager ui{nullptr};
    PanelTree t = BuildPanelTree();
    UIElement* root = t.Root.get();
    UIElement* panel = t.Panel;
    FocusLog log;
    WatchFocus(*t.Other, "other", log);
    t.Field->RegisterEventHandler(kEventFocusOut, [&ui](UIEvent&) { ui.SetFocusById("other"); });
    ui.SetRoot(std::move(t.Root));
    ui.SetFocusById("field");
    UIManagerFocusAccess::AnnounceFocusChange(ui);

    root->RemoveChild(panel);
    UIManagerFocusAccess::AnnounceFocusChange(ui);

    EXPECT_EQ(ui.GetFocusedElementId(), "other");
    EXPECT_EQ(log.CountOf("in:other"), 1);
}

TEST(FocusReleaseOnRemoval, ReplacingTheRootDispatchesFocusOut)
{
    UIManager ui{nullptr};
    PanelTree t = BuildPanelTree();
    FocusLog log;
    WatchFocus(*t.Field, "field", log);
    ui.SetRoot(std::move(t.Root));
    ui.SetFocusById("field");
    UIManagerFocusAccess::AnnounceFocusChange(ui);

    ui.SetRoot(MakeElement("replacement"));

    EXPECT_EQ(log.CountOf("out:field"), 1);
    EXPECT_EQ(ui.GetFocusedElementId(), "");
}

// The production shape: a text field whose value commits on blur, edited, then destroyed
// with its panel. The edit reaches the commit callback before the field dies.
TEST(FocusReleaseOnRemoval, AnEditedTextFieldCommitsWhenItsPanelIsDestroyed)
{
    UIManager ui{nullptr};
    auto root = MakeElement("root");
    auto panelOwned = MakeElement("panel");
    auto fieldOwned = std::make_unique<TextField>();
    fieldOwned->SetId("field");
    TextField* field = fieldOwned.get();
    std::vector<std::string> committed;
    field->SetOnValueChanged([&committed](const std::string& value) { committed.push_back(value); });
    UIElement* panel = panelOwned.get();
    UIElement* rootEl = root.get();
    panelOwned->AddChild(std::move(fieldOwned));
    root->AddChild(std::move(panelOwned));
    ui.SetRoot(std::move(root));
    ui.SetFocusById("field");
    UIManagerFocusAccess::AnnounceFocusChange(ui);
    ASSERT_TRUE(ui.OnChar('a'));
    ASSERT_TRUE(committed.empty()) << "a string field commits on blur, not per keystroke";

    rootEl->RemoveChild(panel);

    EXPECT_EQ(committed, (std::vector<std::string>{"a"}));
}

// A panel taken off its Mount is not destroyed; it is unreachable from the root. The
// element that heard FocusIn still hears FocusOut when focus leaves it.
TEST(FocusReleaseOnRemoval, AnUnmountedPanelStillHearsFocusOut)
{
    UIManager ui{nullptr};
    auto root = MakeElement("root");
    auto mountOwned = std::make_unique<Mount>();
    Mount* mount = mountOwned.get();
    root->AddChild(std::move(mountOwned));
    ui.SetRoot(std::move(root));

    // Externally owned, like a docked panel: the Mount does not own its target.
    auto panel = MakeElement("panel");
    auto fieldOwned = MakeElement("field");
    UIElement* field = fieldOwned.get();
    panel->AddChild(std::move(fieldOwned));
    mount->SetTarget(panel.get());
    FocusLog log;
    WatchFocus(*field, "field", log);
    ui.SetFocusById("field");
    UIManagerFocusAccess::AnnounceFocusChange(ui);
    ASSERT_EQ(log.CountOf("in:field"), 1);

    // The editor's close-tab order: clear focus, then take the panel off its Mount, then
    // the frame announces the change.
    ui.ClearFocus();
    mount->SetTarget(nullptr);
    UIManagerFocusAccess::AnnounceFocusChange(ui);

    EXPECT_EQ(log.CountOf("out:field"), 1) << "the unmounted field never heard FocusOut";
}
