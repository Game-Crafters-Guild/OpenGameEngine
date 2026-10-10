// ContextMenuManipulator — the arming, the propagation, and the two lifetimes.
//
// The arming is the reason this class exists rather than an "on right mouse up" callback:
// a release over an element whose press began somewhere else must open nothing. Most of
// this file is that state machine, driven event by event.
//
// The revocation half publishes THIS TEST BINARY'S image range as if it were a
// hot-swappable module, the model ModuleOwnedHandlerRevocationTests establishes and
// documents. It is the load-bearing case for this class: every one of its subscriptions is
// an Engine.dll lambda wrapping the caller's callable, so the ownership stamp read off the
// outer lambda would say Engine.dll and revoke nothing — the manipulator has to adopt the
// stamp of the callback it was handed, or a menu closure outlives the module that built it.

#include <gtest/gtest.h>

#include "Input/InputSystem.h"
#include "Mathematics/Vector2.h"
#include "UI/Controls/Button.h"
#include "UI/Interaction/ContextMenuManipulator.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/ModuleOwnedHandlers.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UiContext.h"
#include "UI/UiDispatcher.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

using namespace GameEngine;

namespace
{

constexpr int kLeftButton = 0;
constexpr int kRightButton = 1;

// A SECOND manipulator type, so the double-attach rule can be tested for what it actually is
// (per concrete type) rather than "one manipulator per element". Deliberately the thinnest
// possible PointerManipulator: it exists to be a different typeid that claims a different
// button, not to be a feature.
class LeftClickManipulator : public PointerManipulator
{
  public:
    using Handler = std::function<void(UIElement&, float, float)>;

    static std::shared_ptr<LeftClickManipulator> Create(Handler handler)
    {
        std::shared_ptr<LeftClickManipulator> self(new LeftClickManipulator(std::move(handler)));
        self->AddActivationFilter(ManipulatorActivationFilter{kLeftButton, 0});
        return self;
    }

  private:
    explicit LeftClickManipulator(Handler handler)
        : m_Handler(std::move(handler))
    {
    }

    UIElement::EventHandlerToken SubscribeStamped(UIElement& target, EventId id,
                                                  UIElement::EventHandler handler) override
    {
        return Subscribe(target, id, std::move(handler), m_Handler);
    }

    void OnActivated(UIElement& target, float x, float y) override
    {
        if (m_Handler)
            m_Handler(target, x, y);
    }

    Handler m_Handler;
};

// Build-and-attach in one step, for the gesture tests below — they are about the state
// machine, not about the construction forms, which have their own tests further down. The
// menu is a single "Probe" item (a menu with nothing to invoke is refused); `path` names it
// when a test needs to tell two manipulators' menus apart. Returns the handle so a test
// that wants to remove the manipulator again can.
std::shared_ptr<ContextMenuManipulator> Attach(UIElement& el, std::function<void()> onActivate = {},
                                               std::string path = "Probe")
{
    if (!onActivate)
        onActivate = [] {};
    auto manipulator = ContextMenuManipulator::Create(
        {ContextMenuManipulator::Item{.Path = std::move(path), .OnActivate = std::move(onActivate)}});
    el.AddManipulator(manipulator);
    return manipulator;
}

// Dispatches one pointer event AT an element, the way UIManager's bubble walk would with
// this element as the innermost target. Returns whether anything claimed it.
bool Send(UIElement& el, EventId id, float x, float y, int button)
{
    UIEvent e{};
    e.Id = id;
    e.X = x;
    e.Y = y;
    e.Button = button;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
    return e.Handled;
}

// The same, but reporting what the event asked to capture — the manipulator has to take
// the pointer on arming or it never sees a release that lands outside.
UIElement* SendForCapture(UIElement& el, EventId id, float x, float y, int button)
{
    UIEvent e{};
    e.Id = id;
    e.X = x;
    e.Y = y;
    e.Button = button;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
    return e.CaptureRequested;
}

// A cancel carries no position: the pointer is off the surface. Mirrors
// UIManager::DispatchBubbleEvent's sentinel rather than inventing coordinates.
void SendCancel(UIElement& el)
{
    UIEvent e{};
    e.Id = kEventMouseCancel;
    e.X = -1.0e9f;
    e.Y = -1.0e9f;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
}

void SendLifecycle(UIElement& el, EventId id)
{
    UIEvent e{};
    e.Id = id;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
}

// The bubble walk itself, so the propagation tests exercise the real rule (stop at the
// first element that claims the event) rather than a restatement of it.
bool SendBubbling(UIElement& innermost, EventId id, float x, float y, int button)
{
    UIEvent e{};
    e.Id = id;
    e.X = x;
    e.Y = y;
    e.Button = button;
    e.Target = &innermost;
    for (UIElement* p = &innermost; p != nullptr; p = p->GetParent())
    {
        e.CurrentTarget = p;
        p->DispatchEvent(e);
        if (e.Handled)
            break;
    }
    return e.Handled;
}

// One row as the fake backend received it — the observation channel for the declarative
// forms, standing where the editor's factory-made menu would.
struct BuiltRow
{
    uint32_t ParentId = 0;
    std::string Title;
    uint32_t CommandId = 0; // 0 for submenus and separators
    uint32_t SubMenuId = 0; // non-zero marks a submenu row
    uint32_t Flags = MenuItemFlag_None;
    bool Separator = false;
    std::string Icon;
    std::string Color;
};

struct ShowCall
{
    Platform::Window* Window = nullptr;
    int X = 0;
    int Y = 0;
};

class RecordingMenu final : public INativeContextMenu
{
  public:
    std::vector<BuiltRow> Rows;
    std::vector<ShowCall> Shows;
    int ClearCount = 0;
    CommandCallback Handler;

    void Clear() override
    {
        ++ClearCount;
        Rows.clear();
        m_NextSubMenuId = 100000;
    }
    uint32_t AddSubMenu(uint32_t parentId, const std::string& title) override
    {
        BuiltRow row;
        row.ParentId = parentId;
        row.Title = title;
        row.SubMenuId = m_NextSubMenuId++;
        Rows.push_back(row);
        return Rows.back().SubMenuId;
    }
    void AddItem(uint32_t parentId, const std::string& title, uint32_t commandId,
                 uint32_t itemFlags) override
    {
        BuiltRow row;
        row.ParentId = parentId;
        row.Title = title;
        row.CommandId = commandId;
        row.Flags = itemFlags;
        Rows.push_back(row);
    }
    void AddSeparator(uint32_t parentId) override
    {
        BuiltRow row;
        row.ParentId = parentId;
        row.Separator = true;
        Rows.push_back(row);
    }
    void SetCommandHandler(CommandCallback cb) override { Handler = std::move(cb); }
    void SetStateProvider(StateProviderCallback) override {}
    void SetItemEnabled(uint32_t, bool) override {}
    void SetItemChecked(uint32_t, bool) override {}
    void SetItemColor(uint32_t commandId, const std::string& hexColor) override
    {
        for (BuiltRow& row : Rows)
            if (row.CommandId == commandId && !row.Separator && row.SubMenuId == 0)
                row.Color = hexColor;
    }
    void SetItemIcon(uint32_t commandId, const std::string& imagePath) override
    {
        for (BuiltRow& row : Rows)
            if (row.CommandId == commandId && !row.Separator && row.SubMenuId == 0)
                row.Icon = imagePath;
    }
    void SetSubMenuIcon(uint32_t submenuId, const std::string& imagePath) override
    {
        for (BuiltRow& row : Rows)
            if (row.SubMenuId == submenuId)
                row.Icon = imagePath;
    }
    void Show(Platform::Window* window, int x, int y) override
    {
        Shows.push_back({window, x, y});
    }

    const BuiltRow* FindLeaf(std::string_view title) const
    {
        for (const BuiltRow& row : Rows)
            if (!row.Separator && row.SubMenuId == 0 && row.Title == title)
                return &row;
        return nullptr;
    }

  private:
    uint32_t m_NextSubMenuId = 100000;
};

// Installs a factory that hands out RecordingMenus and keeps observing pointers to them;
// restores the no-factory state on the way out so no test inherits another's factory.
// The pointers stay valid for the scope: each menu is owned by its manipulator, which the
// tests keep alive.
struct ScopedMenuFactory
{
    std::vector<RecordingMenu*> Created;

    ScopedMenuFactory()
    {
        ContextMenuManipulator::SetMenuFactory(
            [this]() -> std::unique_ptr<INativeContextMenu>
            {
                auto menu = std::make_unique<RecordingMenu>();
                Created.push_back(menu.get());
                return menu;
            });
    }
    ~ScopedMenuFactory() { ContextMenuManipulator::SetMenuFactory(nullptr); }
    ScopedMenuFactory(const ScopedMenuFactory&) = delete;
    ScopedMenuFactory& operator=(const ScopedMenuFactory&) = delete;
};

// The declarative show is deferred through PostAction (menu building mutates the UI tree),
// so these tests pump an explicit dispatcher — the same one the editor drains per frame.
struct DispatcherScope
{
    UI::UiDispatcher Dispatcher;
    UI::UiContextScope Scope{&Dispatcher, nullptr};

    void Pump() { Dispatcher.Drain(); }
};

// Right-click through the arming machine (at the element's centre, so the release lands
// inside wherever the element sits), then drain the deferred build-and-show.
void OpenMenu(DispatcherScope& ui, UIElement& el)
{
    const float x = el.GetLayoutX() + el.GetLayoutWidth() * 0.5f;
    const float y = el.GetLayoutY() + el.GetLayoutHeight() * 0.5f;
    Send(el, kEventMouseDown, x, y, kRightButton);
    Send(el, kEventMouseUp, x, y, kRightButton);
    ui.Pump();
}


// The observable that replaced the raw menu callback when the manipulator started owning
// its menu: "did a menu open" is now "did a factory-made menu get Shown". Created pointers
// are valid while their owning manipulators are — an arm that destroys a manipulator must
// not read menus (or call Opens) afterwards.
struct GestureFixture
{
    DispatcherScope Ui;
    ScopedMenuFactory Factory;

    size_t Opens()
    {
        Ui.Pump();
        size_t n = 0;
        for (RecordingMenu* menu : Factory.Created)
            n += menu->Shows.size();
        return n;
    }
    // The single menu a one-manipulator arm created so far, or null before the first open.
    RecordingMenu* Menu() { return Factory.Created.empty() ? nullptr : Factory.Created.front(); }
};

std::unique_ptr<UIElement> MakeElement(float x, float y, float w, float h)
{
    auto el = std::make_unique<UIElement>();
    UILayoutAccess::SetLastLayoutRect(*el, x, y, w, h);
    return el;
}

#if defined(_WIN32)
struct Range
{
    std::uint64_t Base = 0;
    std::uint64_t Size = 0;
};

Range ThisBinaryRange()
{
    Range r;
    auto* handle = ::GetModuleHandleW(nullptr);
    const auto* base = reinterpret_cast<const unsigned char*>(handle);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return r;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return r;
    r.Base = reinterpret_cast<std::uint64_t>(base);
    r.Size = nt->OptionalHeader.SizeOfImage;
    return r;
}

// Publishes this binary as a hot-swappable image for the duration of a test and retracts it
// afterwards; the state is process-wide, so one test's range must never leak into the next.
class PretendModuleImage
{
  public:
    PretendModuleImage()
        : m_Range(ThisBinaryRange())
    {
        UI::OpenImageAttribution();
        UI::CloseImageAttribution(m_Range.Base, m_Range.Size);
    }
    ~PretendModuleImage()
    {
        UI::RevokeHandlersOwnedByImage(m_Range.Base, m_Range.Size);
        UI::RetractHotSwappableImage(m_Range.Base);
    }
    PretendModuleImage(const PretendModuleImage&) = delete;
    PretendModuleImage& operator=(const PretendModuleImage&) = delete;

    std::size_t Revoke() const { return UI::RevokeHandlersOwnedByImage(m_Range.Base, m_Range.Size); }
    std::size_t Pins() const
    {
        return UI::CountHandlersOwnedByImage(m_Range.Base, m_Range.Size);
    }

  private:
    Range m_Range;
};
#endif

} // namespace

// ---------------------------------------------------------------------------
// The arming state machine.
// ---------------------------------------------------------------------------

TEST(ContextMenuManipulatorTests, PressAndReleaseInsideOpensTheMenu)
{
    GestureFixture fx;
    auto el = MakeElement(0, 0, 50, 20);
    Attach(*el);

    EXPECT_TRUE(Send(*el, kEventMouseDown, 10, 10, kRightButton));
    EXPECT_TRUE(Send(*el, kEventMouseUp, 12, 11, kRightButton));

    ASSERT_EQ(fx.Opens(), 1u);
    // Anchored to the ELEMENT (below it, like a dropdown), not to the release position.
    ASSERT_EQ(fx.Menu()->Shows.size(), 1u);
    EXPECT_EQ(fx.Menu()->Shows[0].X, 0);
    EXPECT_EQ(fx.Menu()->Shows[0].Y, 20) << "the menu did not anchor below its element";
}

// The whole point of arming: this release is not on a press of ours.
TEST(ContextMenuManipulatorTests, ReleaseWithoutAPressOpensNothingAndIsNotClaimed)
{
    GestureFixture fx;
    auto el = MakeElement(0, 0, 50, 20);
    Attach(*el);

    EXPECT_FALSE(Send(*el, kEventMouseUp, 10, 10, kRightButton))
        << "an unarmed manipulator claimed a release, so an ancestor never saw it";
    EXPECT_EQ(fx.Opens(), 0u);
}

TEST(ContextMenuManipulatorTests, PressInsideThenReleaseOutsideOpensNothing)
{
    GestureFixture fx;
    auto el = MakeElement(0, 0, 50, 20);
    Attach(*el);

    EXPECT_TRUE(Send(*el, kEventMouseDown, 10, 10, kRightButton));
    // Claimed even so: the press was ours, so the release that ends it is ours to consume.
    EXPECT_TRUE(Send(*el, kEventMouseUp, 1000, 1000, kRightButton));
    EXPECT_EQ(fx.Opens(), 0u);

    // And the gesture is over — a second release opens nothing either.
    EXPECT_FALSE(Send(*el, kEventMouseUp, 10, 10, kRightButton));
    EXPECT_EQ(fx.Opens(), 0u);
}

// Leaving and coming back is not a cancellation, exactly as it is not for a left click.
TEST(ContextMenuManipulatorTests, PointerLeavingAndReturningStillOpensTheMenu)
{
    GestureFixture fx;
    auto el = MakeElement(0, 0, 50, 20);
    Attach(*el);

    Send(*el, kEventMouseDown, 10, 10, kRightButton);
    EXPECT_TRUE(el->HasClass("pressed"));

    Send(*el, kEventMouseMove, 1000, 1000, kLeftButton);
    EXPECT_FALSE(el->HasClass("pressed")) << "the pressed visual did not follow the pointer out";

    Send(*el, kEventMouseMove, 10, 10, kLeftButton);
    EXPECT_TRUE(el->HasClass("pressed")) << "the pressed visual did not come back";

    Send(*el, kEventMouseUp, 10, 10, kRightButton);
    EXPECT_EQ(fx.Opens(), 1u);
    EXPECT_FALSE(el->HasClass("pressed"));
}

TEST(ContextMenuManipulatorTests, CancelDisarmsAndOpensNothing)
{
    GestureFixture fx;
    auto el = MakeElement(0, 0, 50, 20);
    Attach(*el);

    Send(*el, kEventMouseDown, 10, 10, kRightButton);
    ASSERT_TRUE(el->HasClass("pressed"));

    SendCancel(*el);
    EXPECT_FALSE(el->HasClass("pressed"));
    EXPECT_EQ(fx.Opens(), 0u);

    // Disarmed, so the release that follows the cancel is not ours and opens nothing.
    EXPECT_FALSE(Send(*el, kEventMouseUp, 10, 10, kRightButton));
    EXPECT_EQ(fx.Opens(), 0u);
}

TEST(ContextMenuManipulatorTests, ArmingTakesThePointerCapture)
{
    auto el = MakeElement(0, 0, 50, 20);
    Attach(*el);

    EXPECT_EQ(SendForCapture(*el, kEventMouseDown, 10, 10, kRightButton), el.get())
        << "without capture a release outside the element never comes back here";
}

TEST(ContextMenuManipulatorTests, LeftClicksAreLeftAlone)
{
    GestureFixture fx;
    auto el = MakeElement(0, 0, 50, 20);
    Attach(*el);

    EXPECT_FALSE(Send(*el, kEventMouseDown, 10, 10, kLeftButton));
    EXPECT_FALSE(el->HasClass("pressed"));
    EXPECT_FALSE(Send(*el, kEventMouseUp, 10, 10, kLeftButton));
    EXPECT_EQ(fx.Opens(), 0u);
}

// A migrated toolbar button still clicks. The manipulator subscribes; it does not replace
// what the control already does with a left press.
TEST(ContextMenuManipulatorTests, LeftClickOnAMigratedButtonStillFires)
{
    GestureFixture fx;
    Button btn;
    UILayoutAccess::SetLastLayoutRect(btn, 0, 0, 50, 20);
    Attach(btn);

    int clicks = 0;
    btn.RegisterEventHandler(kEventButtonClick, [&clicks](UIEvent&) { ++clicks; });

    Send(btn, kEventMouseDown, 10, 10, kLeftButton);
    Send(btn, kEventMouseUp, 10, 10, kLeftButton);
    EXPECT_EQ(clicks, 1);
    EXPECT_EQ(fx.Opens(), 0u);

    Send(btn, kEventMouseDown, 10, 10, kRightButton);
    Send(btn, kEventMouseUp, 10, 10, kRightButton);
    EXPECT_EQ(clicks, 1) << "a right-click activated the button";
    EXPECT_EQ(fx.Opens(), 1u);
}

// ---------------------------------------------------------------------------
// Activation filters (PointerManipulator) and the double-attach contract.
// ---------------------------------------------------------------------------

// Modifiers match EXACTLY, so a modified right-click is a different gesture and this
// manipulator leaves it alone — which is what keeps the combination free for a second
// filter, and what stops the menu appearing on a chord the user meant for something else.
TEST(ContextMenuManipulatorTests, ModifiedRightClickDoesNotActivate)
{
    GestureFixture fx;
    auto el = MakeElement(0, 0, 50, 20);
    Attach(*el);

    UIEvent down{};
    down.Id = kEventMouseDown;
    down.X = 10; down.Y = 10;
    down.Button = kRightButton;
    down.Mods = Input::kModControl;
    down.Target = el.get();
    down.CurrentTarget = el.get();
    el->DispatchEvent(down);

    EXPECT_FALSE(down.Handled) << "a modified press was claimed by a plain-right-click filter";
    EXPECT_FALSE(el->HasClass("pressed"));

    UIEvent up = down;
    up.Id = kEventMouseUp;
    up.Handled = false;
    el->DispatchEvent(up);
    EXPECT_EQ(fx.Opens(), 0u);
}

// The far edge is HALF-OPEN, matching Button::IsPointInside. UIElement::ContainsPoint is
// closed, and using it meant the first pixel of the neighbouring control opened this
// element's menu.
TEST(ContextMenuManipulatorTests, ThePixelPastTheFarEdgeIsOutside)
{
    GestureFixture fx;
    auto el = MakeElement(0, 0, 50, 20);
    Attach(*el);

    Send(*el, kEventMouseDown, 10, 10, kRightButton);
    Send(*el, kEventMouseUp, 50, 10, kRightButton); // x == left + width
    EXPECT_EQ(fx.Opens(), 0u) << "the pixel one past the right edge opened the menu";

    Send(*el, kEventMouseDown, 10, 10, kRightButton);
    Send(*el, kEventMouseUp, 49, 19, kRightButton); // last pixel actually inside
    EXPECT_EQ(fx.Opens(), 1u) << "the last pixel inside the element did not open the menu";
}

// Attaching a second manipulator of the same type is REFUSED, and says so by returning false.
// It could never have fired — the first one's press handler claims the event and the dispatch
// stops there — so the old contract silently ate the second callback. The element's manipulator
// list is what makes the clash visible at attach time instead.
TEST(ContextMenuManipulatorTests, AttachingTwiceIsRefusedRatherThanSilentlyIgnored)
{
    GestureFixture fx;
    auto el = MakeElement(0, 0, 50, 20);

    auto one = ContextMenuManipulator::Create(
        {ContextMenuManipulator::Item{.Path = "First", .OnActivate = [] {}}});
    auto two = ContextMenuManipulator::Create(
        {ContextMenuManipulator::Item{.Path = "Second", .OnActivate = [] {}}});
    EXPECT_TRUE(el->AddManipulator(one));
    EXPECT_FALSE(el->AddManipulator(two))
        << "the second attach was accepted, so its menu is silently dead";

    Send(*el, kEventMouseDown, 10, 10, kRightButton);
    Send(*el, kEventMouseUp, 10, 10, kRightButton);

    ASSERT_EQ(fx.Opens(), 1u);
    EXPECT_TRUE(fx.Menu()->FindLeaf("First"));
    EXPECT_FALSE(fx.Menu()->FindLeaf("Second"));

    // Refused means NOT attached, not merely outvoted: removing the first leaves an element
    // with no menu at all, rather than promoting the second.
    EXPECT_TRUE(el->RemoveManipulator(one));
    Send(*el, kEventMouseDown, 10, 10, kRightButton);
    Send(*el, kEventMouseUp, 10, 10, kRightButton);
    EXPECT_EQ(fx.Opens(), 1u) << "the refused manipulator was attached after all";
}

// Refusal is per concrete TYPE, not per element: a second manipulator claiming a different
// gesture is exactly what the list is meant to allow.
TEST(ContextMenuManipulatorTests, ADifferentManipulatorTypeIsNotRefused)
{
    GestureFixture fx;
    auto el = MakeElement(0, 0, 50, 20);
    int leftClicks = 0;

    Attach(*el);
    EXPECT_TRUE(el->AddManipulator(
        LeftClickManipulator::Create([&leftClicks](UIElement&, float, float) { ++leftClicks; })));

    Send(*el, kEventMouseDown, 10, 10, kRightButton);
    Send(*el, kEventMouseUp, 10, 10, kRightButton);
    Send(*el, kEventMouseDown, 10, 10, kLeftButton);
    Send(*el, kEventMouseUp, 10, 10, kLeftButton);

    EXPECT_EQ(fx.Opens(), 1u);
    EXPECT_EQ(leftClicks, 1) << "a manipulator of a different type was refused or starved";
}

// ONE INSTANCE, ONE ELEMENT. The type rule above is per element, so it says nothing about the
// same object attached to two — and that case corrupts rather than merely wasting a callback:
// m_Tokens records subscriptions without naming an element, and EventHandlerToken::Key is a
// PER-ELEMENT counter, so the second element's teardown unregisters {id,key} pairs minted on
// the first. Colliding keys make that a live stranger's handler, not just a dead lookup.
TEST(ContextMenuManipulatorTests, AttachingOneInstanceToASecondElementIsRefused)
{
    GestureFixture fx;
    auto first = MakeElement(0, 0, 50, 20);
    auto second = MakeElement(0, 0, 50, 20);

    auto manipulator = ContextMenuManipulator::Create(
        {ContextMenuManipulator::Item{.Path = "Shared", .OnActivate = [] {}}});
    ASSERT_TRUE(first->AddManipulator(manipulator));
    EXPECT_FALSE(second->AddManipulator(manipulator))
        << "one instance was attached to two elements, so a removal will unregister the "
           "other element's handlers";

    // Refused means nothing was recorded on the second element: it neither opens a menu nor
    // claims the gesture, and there is nothing there to remove.
    EXPECT_FALSE(Send(*second, kEventMouseDown, 10, 10, kRightButton));
    EXPECT_FALSE(Send(*second, kEventMouseUp, 10, 10, kRightButton));
    EXPECT_EQ(fx.Opens(), 0u) << "the refused attach opened a menu on the second element";
    EXPECT_FALSE(second->RemoveManipulator(manipulator));

    // And the first element is untouched by the attempt.
    Send(*first, kEventMouseDown, 10, 10, kRightButton);
    Send(*first, kEventMouseUp, 10, 10, kRightButton);
    EXPECT_EQ(fx.Opens(), 1u);
    EXPECT_TRUE(fx.Menu()->FindLeaf("Shared"));

    // Removal frees the instance rather than retiring it: the flag that refused the second
    // attach has to clear, or a removed manipulator could never be attached anywhere again.
    EXPECT_TRUE(first->RemoveManipulator(manipulator));
    EXPECT_TRUE(second->AddManipulator(manipulator));
    Send(*second, kEventMouseDown, 10, 10, kRightButton);
    Send(*second, kEventMouseUp, 10, 10, kRightButton);
    EXPECT_EQ(fx.Opens(), 2u) << "a removed manipulator stayed marked as attached";
}

// ---------------------------------------------------------------------------
// Starvation: the defect that made three editor drag features dead, kept dead.
// ---------------------------------------------------------------------------

// A manipulator must not consume the one same-element dispatch slot that a control's own
// OnEvent used to leave for the first table subscriber.
//
// The mechanism this pins is the dispatcher's, not this class's: Button::OnEvent claims an
// armed LEFT press, and the same-element walk used to test Handled as a LEVEL after each
// handler — so exactly one subscriber ran and registration order picked it. Attaching a
// manipulator put its handler first, and every mouse-down subscriber registered afterwards
// (the toolbar reorder drag, the tool-overlay drag) stopped running. The walk now breaks only
// on a false->true TRANSITION during the walk, so a handler that consumed nothing suppresses
// nothing.
//
// Registration order is the whole point of the arm: the manipulator is attached FIRST here,
// which is the order the editor's call sites actually use and the order that used to fail.
TEST(ContextMenuManipulatorTests, ALeftPressStillReachesSubscribersRegisteredAfterTheManipulator)
{
    GestureFixture fx;
    Button btn;
    UILayoutAccess::SetLastLayoutRect(btn, 0, 0, 50, 20);

    Attach(btn);

    int dragPresses = 0;
    btn.RegisterEventHandler(kEventMouseDown,
                             [&dragPresses](UIEvent& e)
                             {
                                 if (e.Button == kLeftButton)
                                     ++dragPresses;
                             });

    Send(btn, kEventMouseDown, 10, 10, kLeftButton);
    EXPECT_EQ(dragPresses, 1)
        << "the manipulator starved a mouse-down subscriber registered after it — this is the "
           "three-dead-drag-features defect";

    // And the manipulator itself is unaffected by sharing the id: its own gesture still works.
    Send(btn, kEventMouseUp, 10, 10, kLeftButton);
    EXPECT_EQ(fx.Opens(), 0u) << "a left click opened a context menu";

    Send(btn, kEventMouseDown, 10, 10, kRightButton);
    Send(btn, kEventMouseUp, 10, 10, kRightButton);
    EXPECT_EQ(fx.Opens(), 1u) << "the right-click menu stopped working";
    EXPECT_EQ(dragPresses, 1) << "a right press counted as a drag press";
}

// ---------------------------------------------------------------------------
// Propagation. The reason Button stopped claiming right-clicks at all.
// ---------------------------------------------------------------------------

TEST(ContextMenuManipulatorTests, RightClickOnAChildWithoutOneReachesTheAncestorsMenu)
{
    GestureFixture fx;
    auto parent = MakeElement(0, 0, 200, 100);
    auto childOwned = MakeElement(10, 10, 50, 20);
    UIElement* child = childOwned.get();
    parent->AddChild(std::move(childOwned));

    Attach(*parent, {}, "Parent");

    // The child has no manipulator, so it must not consume the gesture.
    EXPECT_TRUE(SendBubbling(*child, kEventMouseDown, 20, 15, kRightButton));
    EXPECT_TRUE(SendBubbling(*child, kEventMouseUp, 20, 15, kRightButton));

    ASSERT_EQ(fx.Opens(), 1u) << "the ancestor's context menu never opened";
    EXPECT_TRUE(fx.Menu()->FindLeaf("Parent"));
}

TEST(ContextMenuManipulatorTests, PlainButtonChildDoesNotSwallowTheAncestorsRightClick)
{
    GestureFixture fx;
    auto parent = MakeElement(0, 0, 200, 100);
    auto btnOwned = std::make_unique<Button>();
    UILayoutAccess::SetLastLayoutRect(*btnOwned, 10, 10, 50, 20);
    UIElement* btn = btnOwned.get();
    parent->AddChild(std::move(btnOwned));

    Attach(*parent);

    SendBubbling(*btn, kEventMouseDown, 20, 15, kRightButton);
    SendBubbling(*btn, kEventMouseUp, 20, 15, kRightButton);
    EXPECT_EQ(fx.Opens(), 1u)
        << "a Button consumed a right-click it has no handler for, hiding the container menu";
}

TEST(ContextMenuManipulatorTests, TheInnermostManipulatorWinsAndTheAncestorStaysShut)
{
    GestureFixture fx;
    auto parent = MakeElement(0, 0, 200, 100);
    auto childOwned = MakeElement(10, 10, 50, 20);
    UIElement* child = childOwned.get();
    parent->AddChild(std::move(childOwned));

    Attach(*parent, {}, "Parent");
    Attach(*child, {}, "Child");

    SendBubbling(*child, kEventMouseDown, 20, 15, kRightButton);
    SendBubbling(*child, kEventMouseUp, 20, 15, kRightButton);

    ASSERT_EQ(fx.Opens(), 1u) << "both menus opened for one right-click, or neither did";
    EXPECT_TRUE(fx.Factory.Created[0]->FindLeaf("Child"))
        << "the ancestor's menu opened instead of the innermost one's";
}

// ---------------------------------------------------------------------------
// Lifecycle: attach/detach composition, and destruction.
// ---------------------------------------------------------------------------

TEST(ContextMenuManipulatorTests, DetachWhileArmedDisarmsSoAReattachDoesNotResumeTheGesture)
{
    GestureFixture fx;
    auto el = MakeElement(0, 0, 50, 20);
    Attach(*el);

    Send(*el, kEventMouseDown, 10, 10, kRightButton);
    ASSERT_TRUE(el->HasClass("pressed"));

    SendLifecycle(*el, kEventDetachedFromPanel);
    EXPECT_FALSE(el->HasClass("pressed")) << "a detached element kept a pressed visual";

    SendLifecycle(*el, kEventAttachedToPanel);
    EXPECT_FALSE(Send(*el, kEventMouseUp, 10, 10, kRightButton))
        << "the gesture survived a detach and claimed a release from a press nobody remembers";
    EXPECT_EQ(fx.Opens(), 0u);
}

// Re-attachment needs no re-arming: the subscriptions live in the element's own table and
// travel with it. This is the property that lets the manipulator ignore
// kEventAttachedToPanel entirely.
TEST(ContextMenuManipulatorTests, TheManipulatorStillWorksAfterADetachAndReattach)
{
    GestureFixture fx;
    auto el = MakeElement(0, 0, 50, 20);
    Attach(*el);

    SendLifecycle(*el, kEventDetachedFromPanel);
    SendLifecycle(*el, kEventAttachedToPanel);

    Send(*el, kEventMouseDown, 10, 10, kRightButton);
    Send(*el, kEventMouseUp, 10, 10, kRightButton);
    EXPECT_EQ(fx.Opens(), 1u) << "the manipulator went quiet after its element came back";
}

// The manipulator holds no pointer to its element and nothing outside the element holds the
// manipulator, so destroying the element is the whole teardown. Under ASan this is also the
// pin for the manipulator outliving its element.
TEST(ContextMenuManipulatorTests, DestroyingTheElementReleasesTheManipulator)
{
    GestureFixture fx;
    bool callbackAlive = false;
    {
        auto el = MakeElement(0, 0, 50, 20);

        // A shared_ptr the item's action captures: its use count is the manipulator's
        // lifetime, observable from outside without reaching into the class.
        auto liveness = std::make_shared<int>(0);
        std::weak_ptr<int> weak = liveness;
        Attach(*el, [liveness] {});
        liveness.reset();
        callbackAlive = !weak.expired();
        EXPECT_TRUE(callbackAlive);

        Send(*el, kEventMouseDown, 10, 10, kRightButton);
        Send(*el, kEventMouseUp, 10, 10, kRightButton);
        // The owned RecordingMenu dies with the manipulator below, so read it before.
        ASSERT_EQ(fx.Opens(), 1u);

        el.reset();
        EXPECT_TRUE(weak.expired())
            << "the manipulator (and the caller's callback with it) outlived its element";
    }
}

// ---------------------------------------------------------------------------
// AddManipulator / RemoveManipulator.
// ---------------------------------------------------------------------------

TEST(ContextMenuManipulatorTests, AddAndRemoveRoundTrip)
{
    GestureFixture fx;
    auto el = MakeElement(0, 0, 50, 20);

    auto manipulator = ContextMenuManipulator::Create(
        {ContextMenuManipulator::Item{.Path = "First", .OnActivate = [] {}}});
    ASSERT_TRUE(manipulator);
    ASSERT_TRUE(el->AddManipulator(manipulator));

    Send(*el, kEventMouseDown, 10, 10, kRightButton);
    Send(*el, kEventMouseUp, 10, 10, kRightButton);
    ASSERT_EQ(fx.Opens(), 1u);

    EXPECT_TRUE(el->RemoveManipulator(manipulator));

    // Removed whole: the element behaves exactly like one that never had a manipulator — it
    // opens nothing AND stops claiming right-clicks, so an ancestor's menu can have them.
    EXPECT_FALSE(Send(*el, kEventMouseDown, 10, 10, kRightButton))
        << "a removed manipulator still armed and swallowed the gesture";
    EXPECT_FALSE(Send(*el, kEventMouseUp, 10, 10, kRightButton));
    EXPECT_EQ(fx.Opens(), 1u) << "a removed manipulator opened a menu";

    // Removing twice is a no-op that says so.
    EXPECT_FALSE(el->RemoveManipulator(manipulator));

    // And the element is attachable again, rather than poisoned by the removal.
    auto replacement = ContextMenuManipulator::Create(
        {ContextMenuManipulator::Item{.Path = "Second", .OnActivate = [] {}}});
    EXPECT_TRUE(el->AddManipulator(replacement));
    Send(*el, kEventMouseDown, 10, 10, kRightButton);
    Send(*el, kEventMouseUp, 10, 10, kRightButton);
    ASSERT_EQ(fx.Opens(), 2u);
    EXPECT_TRUE(fx.Factory.Created.back()->FindLeaf("Second"));
}

// The removal path's half of review's pressed-stuck nit, which was untestable while there was
// no way to remove a manipulator. An armed gesture holds the pressed class on an element that
// OUTLIVES the manipulator, so removing mid-press has to disarm or the control stays looking
// held down forever with nothing left alive to clear it.
TEST(ContextMenuManipulatorTests, RemovingWhileArmedDisarmsInsteadOfLeavingItPressed)
{
    GestureFixture fx;
    auto el = MakeElement(0, 0, 50, 20);

    auto manipulator = Attach(*el);

    Send(*el, kEventMouseDown, 10, 10, kRightButton);
    ASSERT_TRUE(el->HasClass("pressed")) << "the press did not arm, so this proves nothing";

    EXPECT_TRUE(el->RemoveManipulator(manipulator));
    EXPECT_FALSE(el->HasClass("pressed"))
        << "removed mid-press and left the element stuck in its pressed visual";

    // The release that would have completed the gesture activates nothing and is not claimed.
    EXPECT_FALSE(Send(*el, kEventMouseUp, 10, 10, kRightButton));
    EXPECT_EQ(fx.Opens(), 0u);
}

TEST(ContextMenuManipulatorTests, AddingNullAttachesNothing)
{
    auto el = MakeElement(0, 0, 50, 20);

    EXPECT_FALSE(el->AddManipulator(nullptr));
    EXPECT_FALSE(ContextMenuManipulator::Create(ContextMenuManipulator::ItemsProvider{}))
        << "a manipulator with no provider is inert and should not be built";
    EXPECT_FALSE(el->AddManipulator(
        ContextMenuManipulator::Create(ContextMenuManipulator::ItemsProvider{})));

    EXPECT_FALSE(Send(*el, kEventMouseDown, 10, 10, kRightButton));
}

// THE CASE THAT DECIDED THE STORAGE. A UIManager side table keyed by instance id would have
// been cleaned up on the destruction-detach announcement — which is never made for an element
// destroyed before it was ever attached to a panel (never told attached, never told detached),
// so that element's entry would leak. A member on the element cannot: it dies with the element,
// announcement or no announcement. This one is never attached to anything.
TEST(ContextMenuManipulatorTests, AnElementDestroyedBeforeEverBeingAttachedLeaksNothing)
{
    std::weak_ptr<int> weak;
    {
        auto el = MakeElement(0, 0, 50, 20);

        auto liveness = std::make_shared<int>(0);
        weak = liveness;
        auto manipulator = Attach(*el, [liveness] {});
        liveness.reset();
        manipulator.reset(); // the subscriptions are the only owner, exactly as documented
        ASSERT_FALSE(weak.expired());

        el.reset();
    }
    EXPECT_TRUE(weak.expired())
        << "the manipulator outlived an element that was never attached to a panel";
}

// The element's list is WEAK, and this is what that buys: the entry does not keep the
// manipulator alive once its subscriptions are gone. Nothing here holds a strong reference
// after the Remove, so the manipulator must be destroyed by it.
TEST(ContextMenuManipulatorTests, RemovingDropsTheLastReference)
{
    auto el = MakeElement(0, 0, 50, 20);

    auto liveness = std::make_shared<int>(0);
    std::weak_ptr<int> weak = liveness;
    auto manipulator = Attach(*el, [liveness] {});
    liveness.reset();
    ASSERT_FALSE(weak.expired());

    EXPECT_TRUE(el->RemoveManipulator(manipulator));
    manipulator.reset();
    EXPECT_TRUE(weak.expired()) << "something outside the handler table still owns it";
}

// ---------------------------------------------------------------------------
// Unload-time revocation.
// ---------------------------------------------------------------------------

#if defined(_WIN32)

TEST(ContextMenuManipulatorRevocationTests, SubscriptionsAdoptTheCallbacksModuleAndAreRevoked)
{
    GestureFixture fx;
    auto el = MakeElement(0, 0, 50, 20);

    PretendModuleImage image;
    Attach(*el);

    // Four permanent subscriptions — down, up, cancel, detach — every one attributed to the
    // callback's image rather than to Engine.dll, which is what makes them revocable at all.
    //
    // This count FALSIFIES the no-adoption case rather than merely agreeing with it: the
    // lambdas doing the registering are Engine.dll's, Engine.dll is not published here, and
    // an unadopted entry therefore stamps zero and pins nothing. That Engine.dll really is a
    // separate image in this binary is what ModuleOwnedHandlers.EngineRegisteredHandlersAre-
    // NeverStamped establishes.
    EXPECT_EQ(image.Pins(), 4u)
        << "a subscription was left stamped Engine.dll and would survive the unload";

    Send(*el, kEventMouseDown, 10, 10, kRightButton);
    Send(*el, kEventMouseUp, 10, 10, kRightButton);
    // Read before the revocation below destroys the manipulator and its owned menu.
    ASSERT_EQ(fx.Opens(), 1u);
    fx.Factory.Created.clear();

    EXPECT_EQ(image.Revoke(), 4u);
    EXPECT_EQ(image.Pins(), 0u);

    // Revoked whole: it neither opens menus nor claims right-clicks any more, so the element
    // behaves exactly like one that never had a manipulator.
    EXPECT_FALSE(Send(*el, kEventMouseDown, 10, 10, kRightButton))
        << "a revoked manipulator still armed and swallowed the gesture";
    EXPECT_FALSE(Send(*el, kEventMouseUp, 10, 10, kRightButton));
    EXPECT_EQ(fx.Opens(), 0u) << "a revoked manipulator opened a menu";
}

// The armed-only move subscription is stamped like the rest: revoking mid-gesture must not
// leave one live handler behind holding the module's callable.
TEST(ContextMenuManipulatorRevocationTests, RevokingWhileArmedTakesTheMoveSubscriptionToo)
{
    auto el = MakeElement(0, 0, 50, 20);

    PretendModuleImage image;
    Attach(*el);
    Send(*el, kEventMouseDown, 10, 10, kRightButton);
    EXPECT_EQ(image.Pins(), 5u) << "the armed move subscription was not stamped";

    EXPECT_EQ(image.Revoke(), 5u);
    EXPECT_EQ(image.Pins(), 0u);
}

// Rule, restated here because this class registers on behalf of someone else: an
// engine-owned callback is not stamped, so it is not revocable.
TEST(ContextMenuManipulatorRevocationTests, EngineOwnedCallbacksAreNotStamped)
{
    auto el = MakeElement(0, 0, 50, 20);
    Attach(*el);

    // No image published: nothing to attribute to, and nothing indexed.
    EXPECT_EQ(UI::IndexedElementCount(), 0u);
}

#endif

// ---------------------------------------------------------------------------
// The manipulator-owned menu: the factory seam and the declarative forms.
// ---------------------------------------------------------------------------

TEST(ContextMenuManipulatorDeclarativeTests, DeclarativeItemsBuildTheMenuThroughTheFactoryAndShowIt)
{
    DispatcherScope ui;
    ScopedMenuFactory factory;
    auto el = MakeElement(10, 20, 50, 22);

    int saved = 0;
    int nested = 0;
    ASSERT_TRUE(el->AddManipulator(ContextMenuManipulator::Create({
        {.Path = "Save", .IconPath = "icon.png", .OnActivate = [&saved] { ++saved; }},
        {.Separator = true},
        {.Path = "More", .IconPath = "sub.png"}, // submenu descriptor, for its icon
        {.Path = "More/Nested", .OnActivate = [&nested] { ++nested; }},
    })));

    OpenMenu(ui, *el);

    ASSERT_EQ(factory.Created.size(), 1u) << "the menu was not created through the factory";
    RecordingMenu& menu = *factory.Created.front();
    ASSERT_EQ(menu.Shows.size(), 1u);
    // The default anchor is below the element, like a dropdown.
    EXPECT_EQ(menu.Shows[0].X, 10);
    EXPECT_EQ(menu.Shows[0].Y, 42);

    // Declaration order: leaf, separator, submenu row, nested leaf.
    ASSERT_EQ(menu.Rows.size(), 4u);
    EXPECT_EQ(menu.Rows[0].Title, "Save");
    EXPECT_EQ(menu.Rows[0].Icon, "icon.png");
    EXPECT_TRUE(menu.Rows[1].Separator);
    EXPECT_NE(menu.Rows[2].SubMenuId, 0u);
    EXPECT_EQ(menu.Rows[2].Icon, "sub.png");
    EXPECT_EQ(menu.Rows[3].Title, "Nested");
    EXPECT_EQ(menu.Rows[3].ParentId, menu.Rows[2].SubMenuId)
        << "the path vocabulary did not nest the leaf under its submenu";

    ASSERT_TRUE(menu.Handler);
    menu.Handler(menu.Rows[0].CommandId);
    EXPECT_EQ(saved, 1);
    EXPECT_EQ(nested, 0);
    menu.Handler(menu.Rows[3].CommandId);
    EXPECT_EQ(nested, 1);
}

TEST(ContextMenuManipulatorDeclarativeTests, TheStateHookResolvesCheckedEnabledAndColourAtEveryShow)
{
    DispatcherScope ui;
    ScopedMenuFactory factory;
    auto el = MakeElement(0, 0, 50, 20);

    bool checked = false;
    bool enabled = true;
    std::string color = "#111111";
    ASSERT_TRUE(el->AddManipulator(ContextMenuManipulator::Create({
        {.Path = "Live",
         .OnActivate = [] {},
         .State =
             [&]
             {
                 ContextMenuManipulator::ItemState st;
                 st.Checked = checked;
                 st.Enabled = enabled;
                 st.ColorHex = color;
                 return st;
             }},
        // A baked disable wins over the hook saying enabled — the backends' rule.
        {.Path = "Baked",
         .Flags = MenuItemFlag_Disabled,
         .OnActivate = [] {},
         .State = [] { return ContextMenuManipulator::ItemState{.Checked = false, .Enabled = true}; }},
    })));

    OpenMenu(ui, *el);
    ASSERT_EQ(factory.Created.size(), 1u);
    RecordingMenu& menu = *factory.Created.front();
    {
        const BuiltRow* live = menu.FindLeaf("Live");
        ASSERT_TRUE(live);
        EXPECT_EQ(live->Flags & MenuItemFlag_Checked, 0u);
        EXPECT_EQ(live->Flags & MenuItemFlag_Disabled, 0u);
        EXPECT_EQ(live->Color, "#111111");
        const BuiltRow* baked = menu.FindLeaf("Baked");
        ASSERT_TRUE(baked);
        EXPECT_NE(baked->Flags & MenuItemFlag_Disabled, 0u) << "a baked disable must win";
    }

    checked = true;
    enabled = false;
    color = "#22cc44";
    OpenMenu(ui, *el);

    // Same owned menu, rebuilt — not a second menu, and not the first resolution cached.
    ASSERT_EQ(factory.Created.size(), 1u) << "reopening created a second menu instead of reusing";
    EXPECT_EQ(menu.ClearCount, 2);
    ASSERT_EQ(menu.Shows.size(), 2u);
    const BuiltRow* live = menu.FindLeaf("Live");
    ASSERT_TRUE(live);
    EXPECT_NE(live->Flags & MenuItemFlag_Checked, 0u) << "the hook's state was not re-resolved";
    EXPECT_NE(live->Flags & MenuItemFlag_Disabled, 0u);
    EXPECT_EQ(live->Color, "#22cc44") << "the colour did not come from the hook";
}

TEST(ContextMenuManipulatorDeclarativeTests, AnItemsProviderComputesTheSetPerShowAndAnEmptySetShowsNothing)
{
    DispatcherScope ui;
    ScopedMenuFactory factory;
    auto el = MakeElement(0, 0, 50, 20);

    int rows = 2;
    ASSERT_TRUE(el->AddManipulator(ContextMenuManipulator::Create(
        [&rows]
        {
            std::vector<ContextMenuManipulator::Item> items;
            for (int i = 0; i < rows; ++i)
                items.push_back({.Path = "Row " + std::to_string(i), .OnActivate = [] {}});
            return items;
        })));

    OpenMenu(ui, *el);
    ASSERT_EQ(factory.Created.size(), 1u);
    RecordingMenu& menu = *factory.Created.front();
    EXPECT_EQ(menu.Rows.size(), 2u);

    rows = 5;
    OpenMenu(ui, *el);
    EXPECT_EQ(menu.Rows.size(), 5u) << "the set was not recomputed at show time";
    ASSERT_EQ(menu.Shows.size(), 2u);

    // The provider's "not now": an empty set neither rebuilds nor shows.
    rows = 0;
    OpenMenu(ui, *el);
    EXPECT_EQ(menu.Shows.size(), 2u) << "an empty set still opened a menu";
    EXPECT_EQ(menu.Rows.size(), 5u) << "an empty set cleared the menu it then did not show";
}

TEST(ContextMenuManipulatorDeclarativeTests, EachManipulatorOwnsItsOwnMenuInstance)
{
    DispatcherScope ui;
    ScopedMenuFactory factory;
    auto first = MakeElement(0, 0, 50, 20);
    auto second = MakeElement(0, 40, 50, 20);

    ASSERT_TRUE(first->AddManipulator(
        ContextMenuManipulator::Create({{.Path = "A", .OnActivate = [] {}}})));
    ASSERT_TRUE(second->AddManipulator(
        ContextMenuManipulator::Create({{.Path = "B", .OnActivate = [] {}}})));

    OpenMenu(ui, *first);
    OpenMenu(ui, *second);
    ASSERT_EQ(factory.Created.size(), 2u) << "two manipulators shared one menu";
    EXPECT_TRUE(factory.Created[0]->FindLeaf("A"));
    EXPECT_TRUE(factory.Created[1]->FindLeaf("B"));

    // Reopening reuses the owned instance rather than minting a third.
    OpenMenu(ui, *first);
    EXPECT_EQ(factory.Created.size(), 2u);
    EXPECT_EQ(factory.Created[0]->Shows.size(), 2u);
}

TEST(ContextMenuManipulatorDeclarativeTests, TheAnchorHookOverridesTheDefaultDropdownAnchor)
{
    DispatcherScope ui;
    ScopedMenuFactory factory;
    auto el = MakeElement(10, 20, 50, 22);

    auto manipulator = ContextMenuManipulator::Create(
        {ContextMenuManipulator::Item{.Path = "A", .OnActivate = [] {}}});
    ASSERT_TRUE(manipulator);
    // A vertical tool row opens beside the button rather than below it.
    manipulator->SetAnchor(
        [](const UIElement& target)
        {
            return Mathematics::Vector2(target.GetLayoutX() + target.GetLayoutWidth(),
                                        target.GetLayoutY());
        });
    ASSERT_TRUE(el->AddManipulator(manipulator));

    OpenMenu(ui, *el);
    ASSERT_EQ(factory.Created.size(), 1u);
    ASSERT_EQ(factory.Created[0]->Shows.size(), 1u);
    EXPECT_EQ(factory.Created[0]->Shows[0].X, 60);
    EXPECT_EQ(factory.Created[0]->Shows[0].Y, 20);
}

// Contract pins, not red-green arms: these hold by construction of the forms.

TEST(ContextMenuManipulatorDeclarativeTests, WithoutAMenuFactoryActivationShowsNothing)
{
    DispatcherScope ui;
    // Deliberately no ScopedMenuFactory.
    auto el = MakeElement(0, 0, 50, 20);
    int activated = 0;
    ASSERT_TRUE(el->AddManipulator(
        ContextMenuManipulator::Create({{.Path = "A", .OnActivate = [&activated] { ++activated; }}})));

    OpenMenu(ui, *el); // logs once, shows nothing, must not crash
    EXPECT_EQ(activated, 0);
}

TEST(ContextMenuManipulatorDeclarativeTests, AFixedSetWithNothingToInvokeIsRefused)
{
    EXPECT_FALSE(ContextMenuManipulator::Create(std::vector<ContextMenuManipulator::Item>{}));
    EXPECT_FALSE(ContextMenuManipulator::Create({
        ContextMenuManipulator::Item{.Separator = true},
        ContextMenuManipulator::Item{.Path = "Sub", .IconPath = "sub.png"},
    })) << "a menu that can never act was built anyway";
    EXPECT_FALSE(ContextMenuManipulator::Create(ContextMenuManipulator::ItemsProvider{}));
}

// The shown backend — and the automation's copy of the command handler — can outlive the
// manipulator, so the handler owns its actions by value instead of reaching back into it.
TEST(ContextMenuManipulatorDeclarativeTests, TheCommandHandlerOutlivesTheManipulator)
{
    DispatcherScope ui;
    ScopedMenuFactory factory;
    auto el = MakeElement(0, 0, 50, 20);

    int activated = 0;
    auto manipulator = ContextMenuManipulator::Create(
        {ContextMenuManipulator::Item{.Path = "A", .OnActivate = [&activated] { ++activated; }}});
    ASSERT_TRUE(el->AddManipulator(manipulator));

    OpenMenu(ui, *el);
    ASSERT_EQ(factory.Created.size(), 1u);
    // Copy the handler the way the automation interceptor does, then destroy the owner.
    // The RecordingMenu itself dies with the manipulator, so the copy is all that is left.
    INativeContextMenu::CommandCallback handler = factory.Created[0]->Handler;
    ASSERT_TRUE(handler);

    ASSERT_TRUE(el->RemoveManipulator(manipulator));
    manipulator.reset();

    handler(1);
    EXPECT_EQ(activated, 1) << "the handler reached back into a destroyed manipulator";
}

#if defined(_WIN32)

// The declarative forms carry the same revocation contract as the raw callback: every
// subscription adopts the module of the caller's callable — the first item's action for a
// fixed set, the provider for a computed one.
TEST(ContextMenuManipulatorRevocationTests, DeclarativeSubscriptionsAdoptTheFirstActionsModule)
{
    DispatcherScope ui;
    ScopedMenuFactory factory;
    auto el = MakeElement(0, 0, 50, 20);

    PretendModuleImage image;
    ASSERT_TRUE(el->AddManipulator(
        ContextMenuManipulator::Create({{.Path = "A", .OnActivate = [] {}}})));
    EXPECT_EQ(image.Pins(), 4u)
        << "a declarative subscription was left stamped Engine.dll and would survive the unload";

    EXPECT_FALSE(el->AddManipulator(ContextMenuManipulator::Create(
        [] { return std::vector<ContextMenuManipulator::Item>{}; })))
        << "double attach must stay refused for the declarative forms too";

    EXPECT_EQ(image.Revoke(), 4u);
    // Revoked whole: no menu opens afterwards.
    OpenMenu(ui, *el);
    EXPECT_TRUE(factory.Created.empty());
}

TEST(ContextMenuManipulatorRevocationTests, ProviderSubscriptionsAdoptTheProvidersModule)
{
    auto el = MakeElement(0, 0, 50, 20);

    PretendModuleImage image;
    ASSERT_TRUE(el->AddManipulator(ContextMenuManipulator::Create(
        [] { return std::vector<ContextMenuManipulator::Item>{}; })));
    EXPECT_EQ(image.Pins(), 4u);
    EXPECT_EQ(image.Revoke(), 4u);
}

#endif
