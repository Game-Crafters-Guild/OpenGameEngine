#include "EditorContextMenu/InterceptableContextMenu.h"

#include "EditorContextMenu/ContextMenuBackendPolicy.h"

#include <cassert>
#include <thread>
#include <utility>
#include <vector>

namespace GameEngine
{

namespace
{
InterceptableContextMenu::Interceptor s_Interceptor;

// Everything below is main-thread-only state, and deliberately unsynchronized.
// Menus are built and shown from UI event callbacks and released from window
// teardown, all of which run on the thread that owns the UIManager. A mutex
// would advertise a threading contract the UI does not have: a menu shown off
// the UI thread would already be building elements the UIManager is laying out
// concurrently. The assert states the invariant instead of hiding a violation
// behind a lock.
std::thread::id s_MainThread{};

void AssertMainThread()
{
    if (s_MainThread == std::thread::id{})
        s_MainThread = std::this_thread::get_id();
    assert(s_MainThread == std::this_thread::get_id() &&
           "Context menus are main-thread only: built and shown from UI callbacks.");
}

// One menu on screen at a time, which is what the UI already assumes: the
// blocker overlay is full-screen, so a second menu could not be reached anyway.
// Showing a menu displaces whatever held the slot.
//
// The slot holds the backend, not the InterceptableContextMenu that configured
// it. That is the point: a caller keeps its menu in a local and lets it
// destruct on return, and only Win32's Show() blocks long enough for that to be
// correct on its own. Every other backend returns before the menu is on screen
// — the editor-drawn one builds its elements and returns, the macOS one defers
// popUpMenuPositioningItem: to the next main-queue turn behind a liveness flag
// its destructor clears. Parking the backend keeps it alive while the menu is
// up, so the caller-local wrapper can die at the end of its scope.
std::shared_ptr<INativeContextMenu> s_ShowingMenu;
Platform::Window* s_ShowingWindow = nullptr;

// Menus displaced while a Show() is on the stack. Win32 dispatches the command
// inside TrackPopupMenuEx, so a command that opens another menu runs *inside*
// the menu it displaces: releasing it there frees the object owning the live
// frame. These wait until the outermost Show() returns.
std::vector<std::shared_ptr<INativeContextMenu>> s_RetiredMenus;

// Menus displaced after their Show() already returned. The editor-drawn backend
// dispatches from a row handler and macOS from a deferred tracking loop, both
// with s_ShowDepth == 0. These wait for DrainShowingContextMenus(), which the
// editor runs at the start of the next Update.
std::vector<std::shared_ptr<INativeContextMenu>> s_DeferredMenus;
int s_ShowDepth = 0;

void DrainList(std::vector<std::shared_ptr<INativeContextMenu>>& menus)
{
    // Destructors can retire further menus; swap so the vector is never mutated
    // while it is being cleared.
    while (!menus.empty())
    {
        std::vector<std::shared_ptr<INativeContextMenu>> draining;
        draining.swap(menus);
    }
}

void RetireShowingMenu()
{
    if (!s_ShowingMenu)
        return;
    s_ShowingWindow = nullptr;
    if (s_ShowDepth > 0)
        s_RetiredMenus.push_back(std::move(s_ShowingMenu));
    else
        s_DeferredMenus.push_back(std::move(s_ShowingMenu));
    s_ShowingMenu.reset();
}
} // namespace

void DrainShowingContextMenus()
{
    AssertMainThread();
    if (s_ShowDepth != 0)
        return;
    DrainList(s_RetiredMenus);
    DrainList(s_DeferredMenus);
}

void ReleaseShowingContextMenu()
{
    AssertMainThread();
    RetireShowingMenu();
    DrainShowingContextMenus();
}

void ReleaseShowingContextMenuForWindow(Platform::Window* window)
{
    AssertMainThread();
    if (!s_ShowingMenu || s_ShowingWindow != window)
        return;
    ReleaseShowingContextMenu();
}

void InterceptableContextMenu::SetInterceptor(Interceptor interceptor)
{
    s_Interceptor = std::move(interceptor);
}

InterceptableContextMenu::InterceptableContextMenu(std::unique_ptr<INativeContextMenu> inner)
    : m_Inner(std::move(inner)), m_BackendGeneration(GetContextMenuBackendGeneration())
{
}

void InterceptableContextMenu::Clear()
{
    // Panels cache their menu and Clear() it before every rebuild, so this is
    // the seam where a changed backend setting takes effect: swap the inner
    // implementation and re-apply the handlers that normally survive Clear().
    // The displaced backend may still be on screen; Show()'s parking slot owns
    // that lifetime, not this pointer.
    if (const unsigned generation = GetContextMenuBackendGeneration();
        generation != m_BackendGeneration)
    {
        m_BackendGeneration = generation;
        m_Inner = CreateSelectedContextMenuBackend();
        if (m_CommandHandler)
            m_Inner->SetCommandHandler(m_CommandHandler);
        if (m_StateProvider)
            m_Inner->SetStateProvider(m_StateProvider);
        if (m_CloseHandler)
            m_Inner->SetCloseHandler(m_CloseHandler);
    }
    m_Inner->Clear();
    // Command/state/close handlers survive Clear() in the platform backends; the
    // shadow model mirrors only the item tree.
    m_Items.clear();
}

uint32_t InterceptableContextMenu::AddSubMenu(uint32_t parentId, const std::string& title)
{
    const uint32_t id = m_Inner->AddSubMenu(parentId, title);
    if (id != 0)
    {
        ShadowItem item;
        item.ParentId = parentId;
        item.Title = title;
        item.SubMenuId = id;
        m_Items.push_back(std::move(item));
    }
    return id;
}

void InterceptableContextMenu::AddItem(uint32_t parentId, const std::string& title,
                                       uint32_t commandId, uint32_t itemFlags)
{
    m_Inner->AddItem(parentId, title, commandId, itemFlags);
    ShadowItem item;
    item.ParentId = parentId;
    item.Title = title;
    item.CommandId = commandId;
    item.StaticEnabled = (itemFlags & MenuItemFlag_Disabled) == 0;
    item.StaticChecked = (itemFlags & MenuItemFlag_Checked) != 0;
    m_Items.push_back(std::move(item));
}

void InterceptableContextMenu::AddSeparator(uint32_t parentId)
{
    m_Inner->AddSeparator(parentId);
    ShadowItem item;
    item.ParentId = parentId;
    item.IsSeparator = true;
    m_Items.push_back(std::move(item));
}

bool InterceptableContextMenu::AddSearchField(uint32_t parentId, const std::string& placeholder,
                                              const std::string& initialText,
                                              SearchCallback onSearch)
{
    // Not represented in the capture — search fields are host-editable
    // controls, not invocable commands.
    return m_Inner->AddSearchField(parentId, placeholder, initialText, std::move(onSearch));
}

void InterceptableContextMenu::SetCommandHandler(CommandCallback cb)
{
    m_CommandHandler = cb;
    m_Inner->SetCommandHandler(std::move(cb));
}

void InterceptableContextMenu::SetCloseHandler(std::function<void()> cb)
{
    m_CloseHandler = std::move(cb);
    m_Inner->SetCloseHandler(m_CloseHandler);
}

void InterceptableContextMenu::SetStateProvider(StateProviderCallback cb)
{
    m_StateProvider = cb;
    m_Inner->SetStateProvider(std::move(cb));
}

// Command id 0 addresses no row. Separators and submenu rows both carry it, and a
// caller may register a real row with it for a non-invocable label (the version
// control panel's disabled "Locked by <user>" entry). A zero-id stamp would smear
// across every one of them, so the four setters below refuse it before the backend
// sees it — the drawn backend matches on id with no zero exclusion either, and the
// capture has to stay in step with the menu the user sees. Refusing zero is what
// makes the remaining match a plain id comparison: no non-leaf row holds a nonzero
// CommandId.
void InterceptableContextMenu::SetItemEnabled(uint32_t commandId, bool enabled)
{
    if (commandId == 0)
        return;
    m_Inner->SetItemEnabled(commandId, enabled);
    for (ShadowItem& item : m_Items)
        if (item.CommandId == commandId)
            item.StaticEnabled = enabled;
}

void InterceptableContextMenu::SetItemChecked(uint32_t commandId, bool checked)
{
    if (commandId == 0)
        return;
    m_Inner->SetItemChecked(commandId, checked);
    for (ShadowItem& item : m_Items)
        if (item.CommandId == commandId)
            item.StaticChecked = checked;
}

void InterceptableContextMenu::SetItemColor(uint32_t commandId, const std::string& hexColor)
{
    if (commandId == 0)
        return;
    m_Inner->SetItemColor(commandId, hexColor);
    for (ShadowItem& item : m_Items)
        if (item.CommandId == commandId)
            item.ColorHex = hexColor;
}

void InterceptableContextMenu::SetItemIcon(uint32_t commandId, const std::string& imagePath)
{
    if (commandId == 0)
        return;
    m_Inner->SetItemIcon(commandId, imagePath);
    for (ShadowItem& item : m_Items)
    {
        if (item.CommandId == commandId)
            item.IconPath = imagePath;
    }
}

// Zero is AddSubMenu's failure return, and every leaf and separator carries SubMenuId
// 0 — stamping it would put the icon on all of them.
void InterceptableContextMenu::SetSubMenuIcon(uint32_t submenuId, const std::string& imagePath)
{
    if (submenuId == 0)
        return;
    m_Inner->SetSubMenuIcon(submenuId, imagePath);
    for (ShadowItem& item : m_Items)
    {
        if (item.SubMenuId == submenuId)
            item.IconPath = imagePath;
    }
}

std::string InterceptableContextMenu::PathFor(const ShadowItem& item) const
{
    // Walk the parent chain by submenu id; menus are tiny, linear scans are fine.
    std::vector<const std::string*> segments;
    if (!item.IsSeparator)
        segments.push_back(&item.Title);
    uint32_t parent = item.ParentId;
    int guard = 0;
    while (parent != 0 && guard++ < 64)
    {
        const ShadowItem* parentItem = nullptr;
        for (const ShadowItem& candidate : m_Items)
        {
            if (candidate.SubMenuId == parent)
            {
                parentItem = &candidate;
                break;
            }
        }
        if (!parentItem)
            break;
        segments.push_back(&parentItem->Title);
        parent = parentItem->ParentId;
    }

    std::string path;
    for (size_t i = segments.size(); i-- > 0;)
    {
        if (!path.empty())
            path += '/';
        path += *segments[i];
    }
    return path;
}

void InterceptableContextMenu::Show(Platform::Window* window, int x, int y)
{
    // Before anything else, and before the slot is touched: a menu with no
    // backend cannot be shown, and must not displace the one that is up.
    if (!m_Inner)
        return;

    if (s_Interceptor)
    {
        Capture capture;
        capture.Items.reserve(m_Items.size());
        for (const ShadowItem& shadow : m_Items)
        {
            CapturedItem item;
            item.Path = PathFor(shadow);
            item.IsSeparator = shadow.IsSeparator;
            item.IsSubMenu = shadow.SubMenuId != 0;
            if (!shadow.IsSeparator && !item.IsSubMenu)
            {
                item.CommandId = shadow.CommandId;
                // Same resolution the Win32 backend applies at show time:
                // static disable wins; the provider refines enabled and owns
                // checked when present.
                bool enabled = shadow.StaticEnabled;
                bool checked = shadow.StaticChecked;
                if (m_StateProvider)
                {
                    const MenuItemState state = m_StateProvider(shadow.CommandId);
                    enabled = enabled && state.Enabled;
                    checked = state.Checked;
                }
                item.Enabled = enabled;
                item.Checked = checked;
            }
            item.Icon = shadow.IconPath;
            item.Color = shadow.ColorHex;
            capture.Items.push_back(std::move(item));
        }
        capture.Invoke = m_CommandHandler;
        capture.X = x;
        capture.Y = y;
        if (s_Interceptor(std::move(capture)))
            return;
    }

    AssertMainThread();

    // Park the backend before showing: on every backend but Win32 this call
    // returns before the menu is on screen, and the caller's wrapper is
    // typically a local that destructs on return.
    RetireShowingMenu();
    s_ShowingMenu = m_Inner;
    s_ShowingWindow = window;

    struct DepthScope
    {
        DepthScope() { ++s_ShowDepth; }
        ~DepthScope()
        {
            --s_ShowDepth;
            if (s_ShowDepth == 0)
                DrainList(s_RetiredMenus);
        }
    } depthScope;

    m_Inner->Show(window, x, y);
}

} // namespace GameEngine
