#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Mathematics/Vector2.h"
#include "Platform/ContextMenu.h"
#include "UI/Interaction/PointerManipulator.h"

namespace GameEngine
{

// A right-click-opens-a-context-menu gesture, attachable to ANY element.
//
// Right-click is not a button behaviour. A button is a thing you activate; the fact that
// several of this editor's toolbar buttons also offer a menu is a property of those call
// sites, not of Button — which is why this is a manipulator rather than another member
// callback on a control.
//
// It is almost entirely PointerManipulator: a right-button activation filter plus the menu.
// The arming, the capture, the cancel handling, the detach cleanup and the ownership
// stamping all live in the base, which is what makes them shared rather than re-derived
// per gesture.
//
// THE MANIPULATOR OWNS ITS MENU. Construction declares the item set (or a provider that
// computes it per show); the manipulator creates one INativeContextMenu through the
// host-installed factory, rebuilds it on every show — the same clear-and-rebuild cadence
// every editor panel used — and shows it anchored to the element. The menu object lives
// and dies with the manipulator; the factory layer keeps a shown backend alive on screen
// after its owner is gone (see EditorContextMenu/InterceptableContextMenu.h).
class ContextMenuManipulator : public PointerManipulator
{
  public:
    // Show-time state for one item, resolved on EVERY open, so a menu built once at attach
    // time still reflects live settings.
    struct ItemState
    {
        bool Checked = false;
        bool Enabled = true;
        // "#RRGGBB" trailing swatch (color-picker rows). Empty keeps the item's baked color.
        std::string ColorHex;
    };

    // One declared row. Paths use the existing menu vocabulary ("Marquee Thickness/1.25"
    // — see Platform/ContextMenu.h), so submenus come free; rows are built in declaration
    // order, never sorted.
    struct Item
    {
        std::string Path;
        // Row icon. On a submenu descriptor (below) it becomes the submenu row's icon.
        std::string IconPath;
        // Baked trailing swatch; a State hook's non-empty ColorHex overrides it per show.
        std::string ColorHex;
        // Baked flags. A State hook refines them by the same rule the menu backends apply:
        // a baked disable wins, the hook refines Enabled and owns Checked.
        uint32_t Flags = MenuItemFlag_None;
        // The row's action, invoked when the menu item is chosen. An item with NO action
        // and Separator=false is a SUBMENU DESCRIPTOR: it declares the submenu row named
        // by Path (usually for its icon) without adding a leaf.
        std::function<void()> OnActivate;
        // Optional show-time state hook; see ItemState.
        std::function<ItemState()> State;
        // A separator INSIDE the submenu Path names ("" = the root level). The other
        // fields are ignored on a separator.
        bool Separator = false;
    };

    // Computes the item set at every show — for menus whose rows appear and vanish per
    // open (an ECS scan, submenus gated on a setting) or that must suppress themselves.
    // Returning an EMPTY set shows nothing: that is the provider's "not now".
    using ItemsProvider = std::function<std::vector<Item>()>;

    // Creates the menu object every manipulator builds into. The HOST installs this once
    // at startup — the editor installs CreateContextMenu(), which selects the configured
    // backend and wraps it in the automation interceptor. That is why nothing here may
    // reach for CreateNativeContextMenu() directly: it would silently drop the backend
    // policy and break menu automation. With no factory installed, activation logs once
    // and shows nothing.
    using MenuFactory = std::function<std::unique_ptr<INativeContextMenu>()>;
    static void SetMenuFactory(MenuFactory factory);

    // A fixed item set, declared where the manipulator is attached. State hooks keep it
    // live; use the ItemsProvider overload when the SET itself varies.
    //
    //   btn->AddManipulator(ContextMenuManipulator::Create({
    //       {.Path = "Save",       .IconPath = icon, .OnActivate = [] { ... }},
    //       {.Path = "Save As...", .IconPath = icon, .OnActivate = [] { ... }},
    //   }));
    //
    // Returns null when no item carries a callable (a menu that can never act is inert,
    // and AddManipulator refuses null). Attach with UIElement::AddManipulator, the only
    // way in; nothing happens until then. DOUBLE ATTACH is refused and logged by
    // AddManipulator, per concrete type.
    //
    // Revocation stamps every subscription to the module owning the FIRST item's
    // callable; an item set mixing callables from different hot-swappable modules is not
    // supported.
    static std::shared_ptr<ContextMenuManipulator> Create(std::vector<Item> items);

    // The item set is computed by `provider` on every show. Returns null for a null
    // provider. Revocation stamps to the module owning the provider.
    static std::shared_ptr<ContextMenuManipulator> Create(ItemsProvider provider);

    // Window-space anchor for the menu, in layout pixels with the origin at the top-left.
    // Default: below the element (GetLayoutX, GetLayoutY + GetLayoutHeight), so menus align
    // like dropdowns regardless of where the pointer fell. Vertical tool rows override this
    // to open beside the button.
    void SetAnchor(std::function<Mathematics::Vector2(const UIElement&)> anchor);

  private:
    ContextMenuManipulator(std::vector<Item> items, ItemsProvider provider);

    UIElement::EventHandlerToken SubscribeStamped(UIElement& target, EventId id,
                                                  UIElement::EventHandler handler) override;
    void OnActivated(UIElement& target, float x, float y) override;

    // Resolves the set (provider or fixed), rebuilds m_Menu from it — declaration order,
    // state hooks applied — and shows it. Runs from a PostAction: menu building mutates
    // the UI tree, so it is deferred exactly as a hand-written handler would.
    void BuildAndShow(Platform::Window* window, Mathematics::Vector2 anchor);

    std::vector<Item> m_Items;
    ItemsProvider m_Provider;
    std::function<Mathematics::Vector2(const UIElement&)> m_Anchor;

    // Created through the installed factory on first show, cleared and rebuilt per show.
    // The representative callable revocation stamps against (see Create); kept so
    // SubscribeStamped can attribute subscriptions made at attach time.
    std::function<void()> m_OwnerProbe;
    std::unique_ptr<INativeContextMenu> m_Menu;
};

} // namespace GameEngine
