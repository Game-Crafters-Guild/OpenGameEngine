#include "UI/Interaction/ContextMenuManipulator.h"

#include "Logger/Logger.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/UIPlatform.h"

#include <utility>

namespace GameEngine
{

namespace
{
// UIEvent::Button indices, as the platform layer delivers them.
constexpr int kRightMouseButton = 1;

// Host-installed menu creation (see SetMenuFactory). Main-thread-only, like every menu.
ContextMenuManipulator::MenuFactory s_MenuFactory;

// Splits one path level off the end: "A/B/C" -> {"A/B", "C"}; "C" -> {"", "C"}.
std::pair<std::string_view, std::string_view> SplitLeaf(std::string_view path)
{
    const size_t slash = path.rfind('/');
    if (slash == std::string_view::npos)
        return {std::string_view{}, path};
    return {path.substr(0, slash), path.substr(slash + 1)};
}
} // namespace

void ContextMenuManipulator::SetMenuFactory(MenuFactory factory)
{
    s_MenuFactory = std::move(factory);
}

ContextMenuManipulator::ContextMenuManipulator(std::vector<Item> items, ItemsProvider provider)
    : m_Items(std::move(items)), m_Provider(std::move(provider))
{
    for (const Item& item : m_Items)
    {
        if (item.OnActivate)
        {
            m_OwnerProbe = item.OnActivate;
            break;
        }
    }
}

std::shared_ptr<ContextMenuManipulator> ContextMenuManipulator::Create(std::vector<Item> items)
{
    const bool anyCallable = [&items]
    {
        for (const Item& item : items)
            if (item.OnActivate)
                return true;
        return false;
    }();
    // A fixed set with nothing to invoke can never act; refuse it the way a null callback
    // is refused, instead of attaching a gesture that opens a dead menu.
    if (!anyCallable)
        return nullptr;

    // Private constructor, so no make_shared.
    std::shared_ptr<ContextMenuManipulator> self(
        new ContextMenuManipulator(std::move(items), nullptr));

    // Plain right-click, no modifiers. Modifiers match exactly, so Ctrl+right-click does not
    // open the menu — deliberate: it leaves that combination free to mean something else.
    self->AddActivationFilter(ManipulatorActivationFilter{kRightMouseButton, 0});
    return self;
}

std::shared_ptr<ContextMenuManipulator> ContextMenuManipulator::Create(ItemsProvider provider)
{
    if (!provider)
        return nullptr;

    std::shared_ptr<ContextMenuManipulator> self(
        new ContextMenuManipulator({}, std::move(provider)));
    self->AddActivationFilter(ManipulatorActivationFilter{kRightMouseButton, 0});
    return self;
}

void ContextMenuManipulator::SetAnchor(
    std::function<Mathematics::Vector2(const UIElement&)> anchor)
{
    m_Anchor = std::move(anchor);
}

UIElement::EventHandlerToken ContextMenuManipulator::SubscribeStamped(
    UIElement& target, EventId id, UIElement::EventHandler handler)
{
    // The caller's callable is the thing whose module must not outlive the subscription,
    // so it is what the entry is attributed to — the provider, or the first item's action.
    if (m_Provider)
        return Subscribe(target, id, std::move(handler), m_Provider);
    return Subscribe(target, id, std::move(handler), m_OwnerProbe);
}

void ContextMenuManipulator::OnActivated(UIElement& target, float x, float y)
{
    // Menus anchor to the element, not the pointer, so they align like dropdowns
    // regardless of where inside the control the release fell.
    (void)x;
    (void)y;
    const Mathematics::Vector2 anchor =
        m_Anchor ? m_Anchor(target)
                 : Mathematics::Vector2(target.GetLayoutX(),
                                        target.GetLayoutY() + target.GetLayoutHeight());

    Platform::Window* window = nullptr;
    if (UIManager* manager = target.GetOwnerManager())
        if (UI::IPlatformApi* platform = manager->GetPlatform())
            window = platform->GetNativeWindow();

    // Building a menu mutates the UI tree (the editor-drawn backend adds overlay
    // elements), so it is deferred to a PostAction. Weak: the manipulator may be removed
    // or revoked before the action drains, and the posted action must not be what keeps a
    // module's callables alive.
    std::weak_ptr<ContextMenuManipulator> weakSelf =
        std::static_pointer_cast<ContextMenuManipulator>(shared_from_this());
    target.PostAction(
        [weakSelf, window, anchor]
        {
            if (const std::shared_ptr<ContextMenuManipulator> self = weakSelf.lock())
                self->BuildAndShow(window, anchor);
        });
}

void ContextMenuManipulator::BuildAndShow(Platform::Window* window, Mathematics::Vector2 anchor)
{
    std::vector<Item> computed;
    if (m_Provider)
        computed = m_Provider();
    const std::vector<Item>& items = m_Provider ? computed : m_Items;
    // The provider's "not now": an empty set shows nothing (2D-only menus in 3D mode, a
    // controller that is not wired yet).
    if (items.empty())
        return;

    if (!s_MenuFactory)
    {
        static bool s_LoggedOnce = false;
        if (!s_LoggedOnce)
        {
            s_LoggedOnce = true;
            Logger::Log::Warning(
                "ContextMenuManipulator: no menu factory installed, so no menu can open. The "
                "host installs one at startup with ContextMenuManipulator::SetMenuFactory.");
        }
        return;
    }
    if (!m_Menu)
    {
        m_Menu = s_MenuFactory();
        if (!m_Menu)
            return;
    }
    m_Menu->Clear();

    // Submenu ids by full path, created on first reference so declaration order is build
    // order. Flat vector: menus are tiny, and a linear scan beats a map here.
    std::vector<std::pair<std::string, uint32_t>> submenus;
    const auto ensureSubMenuChain = [this, &submenus](std::string_view path) -> uint32_t
    {
        uint32_t parentId = 0;
        std::string prefix;
        size_t start = 0;
        while (start < path.size())
        {
            const size_t slash = path.find('/', start);
            const std::string_view segment =
                path.substr(start, slash == std::string_view::npos ? std::string_view::npos
                                                                   : slash - start);
            if (!prefix.empty())
                prefix += '/';
            prefix += segment;

            uint32_t id = 0;
            for (const auto& [existingPath, existingId] : submenus)
            {
                if (existingPath == prefix)
                {
                    id = existingId;
                    break;
                }
            }
            if (id == 0)
            {
                id = m_Menu->AddSubMenu(parentId, std::string(segment));
                submenus.emplace_back(prefix, id);
            }
            parentId = id;

            if (slash == std::string_view::npos)
                break;
            start = slash + 1;
        }
        return parentId;
    };

    // Command ids are 1..N in declaration order; the handler below dispatches by index,
    // so no site-owned id space exists to collide.
    std::vector<std::function<void()>> actions;
    for (const Item& item : items)
    {
        if (item.Separator)
        {
            m_Menu->AddSeparator(ensureSubMenuChain(item.Path));
            continue;
        }
        if (item.Path.empty())
            continue;

        if (!item.OnActivate)
        {
            // Submenu descriptor: declares the row (and its icon) without adding a leaf.
            const uint32_t submenuId = ensureSubMenuChain(item.Path);
            if (!item.IconPath.empty())
                m_Menu->SetSubMenuIcon(submenuId, item.IconPath);
            continue;
        }

        const auto [parentPath, title] = SplitLeaf(item.Path);
        const uint32_t parentId = ensureSubMenuChain(parentPath);

        uint32_t flags = item.Flags;
        std::string color = item.ColorHex;
        if (item.State)
        {
            // The rule every backend (and the automation interceptor) applies: a baked
            // disable wins, the hook refines Enabled and owns Checked.
            const ItemState state = item.State();
            const bool enabled = (flags & MenuItemFlag_Disabled) == 0 && state.Enabled;
            flags = (enabled ? MenuItemFlag_None : MenuItemFlag_Disabled) |
                    (state.Checked ? MenuItemFlag_Checked : MenuItemFlag_None);
            if (!state.ColorHex.empty())
                color = state.ColorHex;
        }

        actions.push_back(item.OnActivate);
        const uint32_t commandId = static_cast<uint32_t>(actions.size());
        m_Menu->AddItem(parentId, std::string(title), commandId, flags);
        if (!item.IconPath.empty())
            m_Menu->SetItemIcon(commandId, item.IconPath);
        if (!color.empty())
            m_Menu->SetItemColor(commandId, color);
    }

    // Decorative-only result (a provider that yielded separators and empty submenus):
    // nothing to invoke, nothing to show.
    if (actions.empty())
        return;

    // By value: the shown backend — and the automation's copy of this handler — can
    // outlive the manipulator, so the handler owns its actions rather than reaching back.
    m_Menu->SetCommandHandler(
        [actions = std::move(actions)](uint32_t commandId)
        {
            if (commandId >= 1 && commandId <= actions.size())
                actions[commandId - 1]();
        });

    m_Menu->Show(window, static_cast<int>(anchor.x), static_cast<int>(anchor.y));
}

} // namespace GameEngine
