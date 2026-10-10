#include "EditorContextMenu/UIContextMenu.h"
#include "UI/UIManager.h"
#include "UI/UIElement.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/SearchFieldWithFilter.h"
#include "UI/Layout/PopupPlacement.h"
#include "UI/Interaction/DismissablePopup.h"
#include "UI/StyleProperties.h"
#include "Platform/Window.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <functional>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GameEngine
{

namespace {
std::unordered_map<Platform::Window*, UIManager*> s_WindowRegistry;

constexpr float kContextMenuViewportMargin = 4.0f;
constexpr float kContextMenuMinWidth = 150.0f;
// Border (1px) + top padding (4px) of .context-menu: the submenu panel rises by
// this so its FIRST ROW top-aligns with the parent item, the way NSMenu aligns
// a submenu. Must track the .context-menu padding in widgets.css.
constexpr float kSubmenuAlignInset = 5.0f;

// Stable id so the query field can be focused by id and driven by automation.
// One context menu is open at a time, so one id suffices.
constexpr const char* kSearchFieldId = "context-menu-search-field";
// Separates submenu titles in a filtered row's origin path.
constexpr const char* kMenuPathSeparator = " > ";

// Menu titles are editor UI strings, so ASCII folding is enough to make the
// filter case-insensitive.
std::string ToLower(std::string_view text)
{
    std::string lowered;
    lowered.reserve(text.size());
    for (char c : text)
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return lowered;
}

// Each menu surface — the root panel and every open submenu — registers as a
// DismissablePopup. That is what makes the manager gate pointer targets to the
// menu while it is open: without it, hover and clicks resolve against whatever
// sits underneath, so rows highlight only where nothing competes and an outside
// press falls through to the content it was meant to dismiss over.
//
// Registering per PANEL, not once for the whole menu: the gate admits input
// that lands inside any open popup's root, and a submenu is not a descendant of
// the panel that opened it — both hang off the shared blocker.
class BoundedContextMenuPanel final : public UIElement, public DismissablePopup
{
public:
    BoundedContextMenuPanel() : DismissablePopup(this) {}

    void OnOwnerManagerChanged(UIManager* owner) override { UpdatePopupRegistration(owner); }

    // Not "still parented": RemoveChild DEFERS while an event is dispatching,
    // so a panel stays parented for the rest of the frame that dismissed it. The
    // manager dismisses every popup reporting open, so answering from parentage
    // re-dismisses this one every frame — closing the menu, reopening nothing,
    // and stalling the editor. The flag latches the moment dismissal is asked
    // for, which is the thing the manager actually needs to know.
    bool IsPopupOpen() const override { return !m_Dismissed && GetParent() != nullptr; }

    void DismissPopup() override
    {
        if (m_Dismissed)
            return;
        m_Dismissed = true;
        if (m_OnDismiss)
            m_OnDismiss();
    }

    /// The root panel closes the whole menu; a submenu closes its own level
    /// and deeper (a press inside a surviving panel must not lose the menu
    /// it is still inside).
    void SetOnDismiss(std::function<void()> cb) { m_OnDismiss = std::move(cb); }

    /// The menu's panels are one logical popup: a press on any open panel's
    /// row must not dismiss its siblings (the root's teardown would destroy
    /// the pressed row before its click dispatches).
    void SetGroupContainment(std::function<bool(const UIElement*)> cb)
    {
        m_GroupContainment = std::move(cb);
    }

    bool PressWithinPopupGroup(const UIElement* pressTarget) const override
    {
        return m_GroupContainment && m_GroupContainment(pressTarget);
    }

    // Yoga resolves the panel's fit-content size in one pass when it has the
    // viewport constraints from the start; setting them post-layout is what
    // forced the extra measuring passes.
    void SetViewportConstraints(float viewportWidth, float viewportHeight)
    {
        const float availableWidth =
            std::max(1.0f, viewportWidth - 2.0f * kContextMenuViewportMargin);
        const float availableHeight =
            std::max(1.0f, viewportHeight - 2.0f * kContextMenuViewportMargin);
        Overrides()
            .Set(Style::MinWidth, StyleLength::Px(std::min(kContextMenuMinWidth, availableWidth)))
            .Set(Style::MaxWidth, StyleLength::Px(availableWidth))
            .Set(Style::MaxHeight, StyleLength::Px(availableHeight));
        m_LastViewportWidth = viewportWidth;
        m_LastViewportHeight = viewportHeight;
    }

    void SetPreferredPosition(float x, float y)
    {
        m_PreferredX = x;
        m_PreferredY = y;
        m_Anchor = nullptr;
        // Applied immediately, not just recorded: the first layout runs before
        // the fit-content size settles, and a clamp computed from an oversized
        // panel renders one frame at the viewport margin before snapping here.
        // Starting at the cursor makes the first frame right; the post-layout
        // pass then only moves the panel when clamping actually demands it.
        Overrides()
            .Set(Style::PositionLeft, StyleLength::Px(x))
            .Set(Style::PositionTop, StyleLength::Px(y))
            .Set(Style::PositionRight, StyleLength::Auto())
            .Set(Style::PositionBottom, StyleLength::Auto());
        m_LastAppliedX = x;
        m_LastAppliedY = y;
    }

    void SetSubmenuAnchor(UIElement* anchor)
    {
        m_Anchor = anchor;
        if (anchor)
        {
            // Same reason SetPreferredPosition applies immediately: the first
            // frame renders before the clamp pass can run on a settled size,
            // and a submenu must not spend it at the panel default position.
            const float x = anchor->GetLayoutX() + anchor->GetLayoutWidth();
            const float y = anchor->GetLayoutY() - kSubmenuAlignInset;
            Overrides()
                .Set(Style::PositionLeft, StyleLength::Px(x))
                .Set(Style::PositionTop, StyleLength::Px(y))
                .Set(Style::PositionRight, StyleLength::Auto())
                .Set(Style::PositionBottom, StyleLength::Auto());
            m_LastAppliedX = x;
            m_LastAppliedY = y;
        }
    }

    void OnPostLayout() override
    {
        UIElement::OnPostLayout();

        UIManager* manager = GetOwnerManager();
        UIElement* root = manager ? manager->GetRootElement() : nullptr;
        if (!root)
            return;

        const UI::Layout::PopupRect viewport{
            root->GetLayoutX(),
            root->GetLayoutY(),
            root->GetLayoutWidth(),
            root->GetLayoutHeight(),
        };
        if (viewport.Width <= 0.0f || viewport.Height <= 0.0f)
            return;

        // Place once per (viewport, anchor), not every layout. Applying a
        // position marks this panel dirty; the relayout resolves a slightly
        // different size, so the next placement lands a hair away and marks it
        // dirty again. The loop is invisible on screen but not to hit-testing:
        // the element under the cursor alternates between the row and the panel
        // frame by frame, so :hover never settles and no row stays lit.
        const float anchorX = m_Anchor ? m_Anchor->GetLayoutX() : m_PreferredX;
        const float anchorY = m_Anchor ? m_Anchor->GetLayoutY() : m_PreferredY;
        // The clamp works from GetLayoutWidth/Height, and the first layouts
        // run before the fit-content size settles. Clamping against an
        // oversized panel drags it to the viewport margin and it visibly
        // travels back as the size shrinks — so while the size is still
        // moving, hold the position the panel opened with and just ask for
        // another pass. The clamp runs once, on the settled size.
        const bool sizeChanged =
            std::abs(m_LastPanelWidth - GetLayoutWidth()) > 0.5f ||
            std::abs(m_LastPanelHeight - GetLayoutHeight()) > 0.5f;
        m_LastPanelWidth = GetLayoutWidth();
        m_LastPanelHeight = GetLayoutHeight();
        if (sizeChanged && m_MeasurePasses < kMaxMeasurePasses)
        {
            // Invisible AND pointer-transparent while measuring: the pre-settle
            // frames carry the wrong size — rendering them reads as the menu
            // collapsing into place, and hit-testing them lets an oversized
            // ghost swallow hover meant for rows beneath it. Bounded so a
            // size that never stops oscillating cannot keep the menu hidden.
            ++m_MeasurePasses;
            Overrides().Set(Style::Opacity, 0.0f).Set(Style::PointerEvents, false);
            MarkDirty(LayoutDirty);
            // The converge loop dispatches OnPostLayout a bounded number of
            // times per frame; when that budget runs out mid-measure, the
            // frame ends with the panel hidden and no further layout queued —
            // it would stay invisible and pointer-dead forever. A deferred
            // re-mark guarantees the next frame resumes the measure until the
            // size settles or the pass cap forces the reveal below.
            PostSafeAction([this] { MarkDirty(LayoutDirty); });
            return;
        }
        Overrides().Set(Style::Opacity, 1.0f).Set(Style::PointerEvents, true);
        const bool inputsUnchanged = m_Placed &&
            std::abs(m_LastViewportWidth - viewport.Width) <= 0.5f &&
            std::abs(m_LastViewportHeight - viewport.Height) <= 0.5f &&
            std::abs(m_LastAnchorX - anchorX) <= 0.5f &&
            std::abs(m_LastAnchorY - anchorY) <= 0.5f;
        if (inputsUnchanged)
            return;
        m_LastAnchorX = anchorX;
        m_LastAnchorY = anchorY;

        const float availableWidth =
            std::max(1.0f, viewport.Width - 2.0f * kContextMenuViewportMargin);
        const float availableHeight =
            std::max(1.0f, viewport.Height - 2.0f * kContextMenuViewportMargin);

        bool changed = false;
        if (std::abs(m_LastViewportWidth - viewport.Width) > 0.5f ||
            std::abs(m_LastViewportHeight - viewport.Height) > 0.5f)
        {
            Overrides()
                .Set(Style::MinWidth,
                     StyleLength::Px(std::min(kContextMenuMinWidth, availableWidth)))
                .Set(Style::MaxWidth, StyleLength::Px(availableWidth))
                .Set(Style::MaxHeight, StyleLength::Px(availableHeight));
            m_LastViewportWidth = viewport.Width;
            m_LastViewportHeight = viewport.Height;
            changed = true;
        }

        UI::Layout::PopupPosition position{};
        if (m_Anchor)
        {
            const UI::Layout::PopupRect anchor{
                m_Anchor->GetLayoutX(),
                m_Anchor->GetLayoutY(),
                m_Anchor->GetLayoutWidth(),
                m_Anchor->GetLayoutHeight(),
            };
            position = UI::Layout::PlaceSubmenuInViewport(
                viewport,
                anchor,
                GetLayoutWidth(),
                GetLayoutHeight(),
                kContextMenuViewportMargin,
                kSubmenuAlignInset);
        }
        else
        {
            position = UI::Layout::ClampPopupToViewport(
                viewport,
                m_PreferredX,
                m_PreferredY,
                GetLayoutWidth(),
                GetLayoutHeight(),
                kContextMenuViewportMargin);
        }

        const float localX = position.X - viewport.X;
        const float localY = position.Y - viewport.Y;
        if (std::abs(m_LastAppliedX - localX) > 0.5f ||
            std::abs(m_LastAppliedY - localY) > 0.5f)
        {
            Overrides()
                .Set(Style::PositionLeft, StyleLength::Px(localX))
                .Set(Style::PositionTop, StyleLength::Px(localY))
                .Set(Style::PositionRight, StyleLength::Auto())
                .Set(Style::PositionBottom, StyleLength::Auto());
            m_LastAppliedX = localX;
            m_LastAppliedY = localY;
            changed = true;
        }

        m_Placed = true;
        if (changed)
            MarkDirty(LayoutDirty | VisualDirty);
    }

private:
    std::function<void()> m_OnDismiss;
    std::function<bool(const UIElement*)> m_GroupContainment;
    bool m_Dismissed = false;
    bool m_Placed = false;
    float m_LastAnchorX = std::numeric_limits<float>::infinity();
    float m_LastAnchorY = std::numeric_limits<float>::infinity();
    float m_LastPanelWidth = std::numeric_limits<float>::infinity();
    float m_LastPanelHeight = std::numeric_limits<float>::infinity();
    static constexpr int kMaxMeasurePasses = 4;
    int m_MeasurePasses = 0;
    float m_PreferredX = 0.0f;
    float m_PreferredY = 0.0f;
    UIElement* m_Anchor = nullptr;
    float m_LastViewportWidth = std::numeric_limits<float>::infinity();
    float m_LastViewportHeight = std::numeric_limits<float>::infinity();
    float m_LastAppliedX = std::numeric_limits<float>::infinity();
    float m_LastAppliedY = std::numeric_limits<float>::infinity();
};
} // namespace


std::unique_ptr<INativeContextMenu> CreateSelectedContextMenuBackend()
{
    if (GetContextMenuBackend() == ContextMenuBackend::Native && NativeContextMenuAvailable())
        return CreateNativeContextMenu();
    return std::make_unique<UIContextMenu>();
}

void UIContextMenu::Register(Platform::Window* window, UIManager* uiManager)
{
    s_WindowRegistry[window] = uiManager;
}

void UIContextMenu::Unregister(Platform::Window* window)
{
    s_WindowRegistry.erase(window);
    ReleaseShowingContextMenuForWindow(window);
}

UIContextMenu::UIContextMenu() = default;

    UIContextMenu::~UIContextMenu()
    {
        *m_Alive = false;
        Clear();
    }

    void UIContextMenu::Clear()
    {
        CloseAll();
        m_RootItems.clear();
        m_NextSubMenuId = kFirstSubMenuId;
        m_SearchEnabled = false;
        m_SearchPlaceholder.clear();
        m_SearchInitialText.clear();
        m_SearchCallback = nullptr;
    }

    std::vector<std::unique_ptr<UIContextMenu::MenuItem>>* UIContextMenu::GetListForParent(uint32_t parentId)
    {
        if (parentId == 0) return &m_RootItems;

        std::function<std::vector<std::unique_ptr<MenuItem>>*(std::vector<std::unique_ptr<MenuItem>>&)> search =
            [&](std::vector<std::unique_ptr<MenuItem>>& list) -> std::vector<std::unique_ptr<MenuItem>>*
        {
            for (auto& item : list)
            {
                if (item->isSubMenu && item->id == parentId) return &item->children;
                if (item->isSubMenu)
                {
                    auto* res = search(item->children);
                    if (res) return res;
                }
            }
            return nullptr;
        };

        return search(m_RootItems);
    }

    UIContextMenu::MenuItem* UIContextMenu::FindItem(uint32_t id, std::vector<std::unique_ptr<MenuItem>>& list)
    {
        for (auto& item : list)
        {
            if (item->id == id) return item.get();
            if (item->isSubMenu)
            {
                auto* res = FindItem(id, item->children);
                if (res) return res;
            }
        }
        return nullptr;
    }

    uint32_t UIContextMenu::AddSubMenu(uint32_t parentId, const std::string& title)
    {
        auto* list = GetListForParent(parentId);
        if (!list) return 0;

        auto item = std::make_unique<MenuItem>();
        item->title = title;
        item->id = m_NextSubMenuId++;
        item->isSubMenu = true;
        list->push_back(std::move(item));

        return list->back()->id;
    }

    void UIContextMenu::AddItem(uint32_t parentId, const std::string& title, uint32_t commandId, uint32_t itemFlags)
    {
        auto* list = GetListForParent(parentId);
        if (!list) return;

        auto item = std::make_unique<MenuItem>();
        item->title = title;
        item->id = commandId;
        item->flags = itemFlags;
        item->staticEnabled = ((itemFlags & MenuItemFlag_Disabled) == 0);
        item->staticChecked = ((itemFlags & MenuItemFlag_Checked) != 0);
        list->push_back(std::move(item));
    }

    void UIContextMenu::AddSeparator(uint32_t parentId)
    {
        auto* list = GetListForParent(parentId);
        if (!list) return;

        auto item = std::make_unique<MenuItem>();
        item->isSeparator = true;
        list->push_back(std::move(item));
    }

    bool UIContextMenu::AddSearchField(uint32_t parentId, const std::string& placeholder,
                                       const std::string& initialText, SearchCallback onSearch)
    {
        // The query filters the whole item tree into one flat result list, so it
        // belongs to the panel that owns that tree — the root.
        if (parentId != 0)
            return false;

        m_SearchEnabled = true;
        m_SearchPlaceholder = placeholder;
        m_SearchInitialText = initialText;
        m_SearchCallback = std::move(onSearch);
        return true;
    }

    void UIContextMenu::SetCommandHandler(CommandCallback cb)
    {
        m_CommandHandler = std::move(cb);
    }

    void UIContextMenu::SetStateProvider(StateProviderCallback cb)
    {
        m_StateProvider = std::move(cb);
    }

    void UIContextMenu::SetItemEnabled(uint32_t commandId, bool enabled)
    {
        auto* item = FindItem(commandId, m_RootItems);
        if (item) item->staticEnabled = enabled;
    }

    void UIContextMenu::SetItemChecked(uint32_t commandId, bool checked)
    {
        auto* item = FindItem(commandId, m_RootItems);
        if (item) item->staticChecked = checked;
    }

    void UIContextMenu::SetItemColor(uint32_t commandId, const std::string& hexColor)
    {
        if (MenuItem* item = FindItem(commandId, m_RootItems))
            item->colorHex = hexColor;
    }

    void UIContextMenu::SetItemIcon(uint32_t commandId, const std::string& imagePath)
    {
        auto* item = FindItem(commandId, m_RootItems);
        if (item) item->iconPath = imagePath;
    }

    // Submenu rows draw from the same icon field as leaves; ids never collide
    // because submenus number from kFirstSubMenuId.
    void UIContextMenu::SetSubMenuIcon(uint32_t submenuId, const std::string& imagePath)
    {
        auto* item = FindItem(submenuId, m_RootItems);
        if (item) item->iconPath = imagePath;
    }

    void UIContextMenu::CloseAll()
    {
        /* Every dismissal lands here — outside click, Escape, or an item firing —
           so this is where a caller hears that its menu is gone. Show also calls
           CloseAll to tear down a previous menu, so the notification is gated on
           one actually having been open; otherwise opening a menu would report
           it closed in the same breath. */
        const bool wasOpen = m_OverlayRoot != nullptr || !m_OpenMenus.empty();
        // A menu can outlive the UIManager it last opened in (a panel's or the
        // application's menu, destroyed after a window's UI). That manager took
        // the overlay, which lives under its root, with it.
        if (m_UIManager && !m_UIManagerLifetime.Get())
        {
            m_UIManager = nullptr;
            m_OverlayRoot = nullptr;
        }
        if (m_UIManager && m_OverlayRoot)
        {
            if (auto* root = m_UIManager->GetRootElement())
            {
                root->RemoveChild(m_OverlayRoot);
            }
            m_OverlayRoot = nullptr;
        }
        m_OpenMenus.clear();
        m_OpenMenuParentIds.clear();
        m_OpenMenuAnchors.clear();
        m_RootPanel = nullptr;
        m_RowsHost = nullptr;
        m_SearchField = nullptr;
        if (m_UIManager && !m_SavedFocusId.empty())
        {
            m_UIManager->SetFocusById(m_SavedFocusId);
            m_SavedFocusId.clear();
        }
        if (m_UIManager)
            m_UIManager->RequestHoverRefresh();
        /* After teardown, and by value: a handler is free to open another menu. */
        if (wasOpen && m_CloseHandler)
        {
            auto handler = m_CloseHandler;
            handler();
        }
    }

    void UIContextMenu::Show(Platform::Window* window, int x, int y)
    {
        // Both bails below are indistinguishable from "the user right-clicked
        // somewhere without a menu": no menu, no error, nothing in the log. Say
        // which link is missing instead of leaving the caller to guess.
        auto it = s_WindowRegistry.find(window);
        if (it == s_WindowRegistry.end())
        {
            Logger::Log::Warning(
                "UIContextMenu: window {} is not registered, so no menu can open for it. "
                "UIContextMenu::Register(window, uiManager) runs at editor startup for every "
                "window that draws its own menus.",
                static_cast<const void*>(window));
            return;
        }
        m_UIManager = it->second;
        m_UIManagerLifetime = UIManagerRef(m_UIManager);

        auto* root = m_UIManager->GetRootElement();
        if (!root)
        {
            Logger::Log::Warning("UIContextMenu: the registered UIManager has no root element yet; "
                                 "the menu is dropped.");
            return;
        }

        // Saved after the close, not before: CloseAll consumes the saved id to
        // restore focus, so saving first would hand the slot straight back and
        // leave nothing to return to once this menu closes.
        CloseAll();
        m_SavedFocusId = m_UIManager->GetFocusedElementId();

        // Create a full-screen blocker
        auto blocker = std::make_unique<UIElement>();
        m_OverlayRoot = blocker.get();
        blocker->AddClass("context-menu-blocker");
        // Submenus mount under this blocker, so the whole menu rides this layer.
        blocker->SetOverlayLayer(OverlayLayer::ContextMenu);

        blocker->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e)
        {
            CloseAll();
            e.Stop();
        });

        // Create the root menu panel
        auto menuPanel = std::make_unique<BoundedContextMenuPanel>();
        menuPanel->SetOnDismiss([this, alive = m_Alive]()
        {
            if (*alive)
                CloseAll();
        });
        // alive-guarded: CloseAll's RemoveChild defers while an event is
        // dispatching, so a panel (and this callback) can outlive the menu
        // for the rest of that frame — and this runs on every press.
        menuPanel->SetGroupContainment([this, alive = m_Alive](const UIElement* target)
        {
            if (!*alive)
                return false;
            for (UIElement* panel : m_OpenMenus)
                for (const UIElement* cur = target; cur; cur = cur->GetParent())
                    if (cur == panel)
                        return true;
            return false;
        });
        UIElement* panelRaw = menuPanel.get();
        menuPanel->SetViewportConstraints(root->GetLayoutWidth(), root->GetLayoutHeight());
        menuPanel->SetPreferredPosition(static_cast<float>(x), static_cast<float>(y));
        // The dropdown classes carry the surface — background, border, shadow.
        // context-menu then overrides the metrics on top, which are a combo
        // box's row rhythm and not a menu's.
        menuPanel->AddClass("dropdown-items");
        menuPanel->AddClass("context-menu");
        menuPanel->AddClass("fit-content");
        menuPanel->AddClass("context-menu-panel");
        menuPanel->AddClass("open");
        menuPanel->Overrides().Set(Style::PositionLeft, StyleLength::Px(static_cast<float>(x)));
        menuPanel->Overrides().Set(Style::PositionTop, StyleLength::Px(static_cast<float>(y)));

        m_RootPanel = panelRaw;
        if (m_SearchEnabled)
            AddSearchRow(panelRaw);
        // Rows live in their own host so they can scroll under a search field
        // that stays put. Scrolling the panel itself would carry the field off
        // the top the moment a filter returns more than a screenful.
        auto rowsHost = std::make_unique<UIElement>();
        rowsHost->AddClass("context-menu-rows");
        m_RowsHost = rowsHost.get();
        panelRaw->AddChild(std::move(rowsHost));
        BuildRootRows(m_SearchEnabled ? m_SearchInitialText : std::string{});

        m_OpenMenus.push_back(panelRaw);
        m_OpenMenuParentIds.push_back(0);
        m_OpenMenuAnchors.push_back(nullptr);
        blocker->AddChild(std::move(menuPanel));
        root->AddChild(std::move(blocker));

        // Typing filters immediately, without a click into the field first —
        // the way the native menu's search behaves.
        if (m_SearchEnabled)
            m_UIManager->SetFocusById(kSearchFieldId);
    }

    void UIContextMenu::UpdateSelectionVisuals(UIElement* clickedRow, bool radio)
    {
        auto setRowSelected = [](UIElement* row, bool selected)
        {
            row->RemoveClass(selected ? "dim-unselected" : "row-selected");
            row->AddClass(selected ? "row-selected" : "dim-unselected");
            for (const auto& ch : row->GetChildren())
            {
                if (!ch)
                    continue;
                if (ch->HasClass("dropdown-item-check") || ch->HasClass("dropdown-item-check-slot"))
                {
                    ch->RemoveClass(selected ? "dropdown-item-check-slot" : "dropdown-item-check");
                    ch->AddClass(selected ? "dropdown-item-check" : "dropdown-item-check-slot");
                }
            }
            row->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);
        };

        if (!radio)
        {
            // Toggle group: flip only the clicked row. The real state is what
            // the command just set; siblings are independent toggles.
            setRowSelected(clickedRow, !clickedRow->HasClass("row-selected"));
            return;
        }

        // Radio group: the check moves to the clicked row. The group spans
        // the separator-delimited run of siblings around it.
        UIElement* parent = clickedRow->GetParent();
        if (!parent)
            return;
        const auto& siblings = parent->GetChildren();
        int idx = -1;
        for (size_t i = 0; i < siblings.size(); ++i)
            if (siblings[i].get() == clickedRow) { idx = static_cast<int>(i); break; }
        if (idx < 0)
            return;
        int start = idx;
        while (start > 0 && !siblings[start - 1]->HasClass("dropdown-separator"))
            --start;
        int end = idx;
        while (end + 1 < static_cast<int>(siblings.size()) &&
               !siblings[end + 1]->HasClass("dropdown-separator"))
            ++end;
        for (int i = start; i <= end; ++i)
        {
            UIElement* row = siblings[i].get();
            if (!row->HasClass("dropdown-item") || row->HasClass("disabled"))
                continue;
            if (!row->HasClass("row-selected") && !row->HasClass("dim-unselected"))
                continue; // folders and dialog commands carry no selection state
            setRowSelected(row, row == clickedRow);
        }
    }

    void UIContextMenu::AddSearchRow(UIElement* container)
    {
        auto search = std::make_unique<SearchFieldWithFilter>();
        m_SearchField = search.get();
        m_SearchField->AddClass("context-menu-search");
        // Focused from the moment the menu opens, so typing filters without a
        // click — but not *looking* focused until the user actually engages.
        // An accent ring on a field nobody has touched reads as a prompt to
        // type when the menu's own commands are the point.
        m_SearchField->AddClass("awaiting-input");
        m_SearchField->SetFieldId(kSearchFieldId);
        m_SearchField->SetPlaceholder(m_SearchPlaceholder);
        m_SearchField->SetValue(m_SearchInitialText);
        m_SearchField->SetRefocusAfterClear(true);
        m_SearchField->SetOnQueryChanging([this](const std::string& query)
        {
            if (m_SearchField)
                m_SearchField->RemoveClass("awaiting-input");
            ApplySearchQuery(query);
            if (m_SearchCallback)
                m_SearchCallback(query);
        });
        // The blocker closes the menu on any press that reaches it, and a press
        // aimed at the query field is not a press outside the menu.
        UIElement* fieldRaw = m_SearchField;
        // The blocker closes the menu on any press that reaches it, and a press
        // aimed at the query field is not a press outside the menu. Stopping it
        // here is load-bearing: without it the click dismisses the menu.
        UIManager* manager = m_UIManager;
        m_SearchField->RegisterEventHandler(kEventMouseDown, [fieldRaw, manager](UIEvent& e)
        {
            fieldRaw->RemoveClass("awaiting-input");
            // Keep focus pinned to the query input: the release would otherwise
            // hand focus to whatever child the press hit (placeholder label,
            // container), and the accent ring blinks off with it.
            if (manager)
                if (UIElement* root = manager->GetRootElement())
                    if (UIElement* input = root->FindById(kSearchFieldId))
                        manager->FocusElement(input);
            e.Stop();
        });
        m_SearchField->RegisterEventHandler(kEventMouseUp, [manager](UIEvent& e)
        {
            if (manager)
                if (UIElement* root = manager->GetRootElement())
                    if (UIElement* input = root->FindById(kSearchFieldId))
                        manager->FocusElement(input);
            e.Stop();
        });
        container->AddChild(std::move(search));
    }

    void UIContextMenu::BuildRootRows(const std::string& query)
    {
        if (!m_RootPanel)
            return;

        UIElement* host = m_RowsHost ? m_RowsHost : m_RootPanel;
        if (query.empty())
            BuildMenuLevel(host, m_RootItems, 0);
        else
            BuildFilteredRows(host, query);
    }

    void UIContextMenu::ApplySearchQuery(const std::string& query)
    {
        if (!m_RootPanel)
            return;

        // Deferred past the keystroke that typed it: RemoveChild defers while an
        // event is dispatching, so removing the rows inline would leave them
        // parented for the rest of the frame with the new rows appended below.
        m_RootPanel->PostSafeAction([this, alive = m_Alive, query]()
        {
            if (!*alive || !m_RootPanel)
                return;

            // A submenu opened before the query was typed outlives its parent row.
            OpenSubMenu(0, nullptr, 1);

            UIElement* host = m_RowsHost ? m_RowsHost : m_RootPanel;
            std::vector<UIElement*> rows;
            rows.reserve(host->GetChildren().size());
            for (const auto& child : host->GetChildren())
            {
                if (child.get() != static_cast<UIElement*>(m_SearchField))
                    rows.push_back(child.get());
            }
            for (UIElement* row : rows)
                host->RemoveChild(row);

            BuildRootRows(query);
        });
    }

    void UIContextMenu::CollectLeafMatches(const std::vector<std::unique_ptr<MenuItem>>& items,
                                           const std::string& lowerQuery, const std::string& pathPrefix,
                                           std::vector<LeafMatch>& matches) const
    {
        for (const auto& item : items)
        {
            if (item->isSeparator || item->title.empty())
                continue;

            const std::string path =
                pathPrefix.empty() ? item->title : pathPrefix + kMenuPathSeparator + item->title;

            if (item->isSubMenu)
            {
                CollectLeafMatches(item->children, lowerQuery, path, matches);
                continue;
            }

            // The path counts as well as the title, so a query naming the group
            // ("transform") lists the commands inside it.
            if (ToLower(path).find(lowerQuery) == std::string::npos)
                continue;

            matches.push_back(LeafMatch{item.get(), pathPrefix});
        }
    }

    void UIContextMenu::BuildFilteredRows(UIElement* container, const std::string& query)
    {
        std::vector<LeafMatch> matches;
        CollectLeafMatches(m_RootItems, ToLower(query), {}, matches);

        // Commands only: a filtered menu has no submenus to open and no groups to
        // rule off, so separators and submenu parents have nothing left to mark.
        if (matches.empty())
        {
            auto empty = std::make_unique<Label>();
            empty->AddClass("context-menu-no-matches");
            empty->SetText("No matches");
            container->AddChild(std::move(empty));
            return;
        }

        for (const LeafMatch& match : matches)
            AppendItemRow(container, *match.item, 0, match.path);
    }

    void UIContextMenu::OpenSubMenu(uint32_t parentId, UIElement* anchorElement, int level)
    {
        // Re-entering the row of an already-open submenu keeps that submenu:
        // the cursor crosses this row on its way INTO the submenu, and a
        // rebuild would delete the panel mid-crossing — hover lands on rows
        // of a half-settled clone. Native menus keep it open too; only
        // deeper levels close.
        const bool sameSubMenuOpen = parentId != 0 &&
            m_OpenMenuParentIds.size() > static_cast<size_t>(level) &&
            m_OpenMenuParentIds[level] == parentId;

        // Close any menus at this level or deeper (one level deeper when the
        // requested submenu is already open and stays).
        const size_t keep = static_cast<size_t>(level) + (sameSubMenuOpen ? 1u : 0u);
        const bool closedAny = m_OpenMenus.size() > keep;
        while (m_OpenMenus.size() > keep)
        {
            UIElement* panel = m_OpenMenus.back();
            if (panel && m_OverlayRoot)
                m_OverlayRoot->RemoveChild(panel);
            m_OpenMenus.pop_back();
            m_OpenMenuParentIds.pop_back();
            if (!m_OpenMenuAnchors.empty())
            {
                if (UIElement* anchor = m_OpenMenuAnchors.back())
                    anchor->RemoveClass("submenu-open");
                m_OpenMenuAnchors.pop_back();
            }
        }

        if (parentId == 0 || sameSubMenuOpen)
        {
            // The panels just removed may have sat under the cursor, and the
            // close lands a frame after the last mouse move — without a
            // refresh, the row now under the pointer stays unlit until the
            // mouse moves again (a fast sweep hits this constantly).
            if (closedAny && m_UIManager)
                m_UIManager->RequestHoverRefresh();
            return;
        }

        auto* list = GetListForParent(parentId);
        if (!list || list->empty()) return;

        auto menuPanel = std::make_unique<BoundedContextMenuPanel>();
        // Dismissing a submenu closes its level and deeper, not the whole
        // menu: a press inside the ROOT panel (the search field, say) is
        // outside every open submenu, so per-popup dismissal fires for the
        // submenu alone — CloseAll here tore down the menu the user was
        // still inside. A press outside everything dismisses each panel
        // individually, and the root's own CloseAll still closes the lot.
        menuPanel->SetOnDismiss([this, level, alive = m_Alive]()
        {
            if (*alive)
                OpenSubMenu(0, nullptr, level);
        });
        // alive-guarded: CloseAll's RemoveChild defers while an event is
        // dispatching, so a panel (and this callback) can outlive the menu
        // for the rest of that frame — and this runs on every press.
        menuPanel->SetGroupContainment([this, alive = m_Alive](const UIElement* target)
        {
            if (!*alive)
                return false;
            for (UIElement* panel : m_OpenMenus)
                for (const UIElement* cur = target; cur; cur = cur->GetParent())
                    if (cur == panel)
                        return true;
            return false;
        });
        UIElement* panelRaw = menuPanel.get();
        if (auto* root = m_UIManager ? m_UIManager->GetRootElement() : nullptr)
            menuPanel->SetViewportConstraints(root->GetLayoutWidth(), root->GetLayoutHeight());
        menuPanel->SetSubmenuAnchor(anchorElement);
        menuPanel->AddClass("dropdown-items");
        menuPanel->AddClass("context-menu");
        menuPanel->AddClass("fit-content");
        menuPanel->AddClass("context-menu-panel");
        menuPanel->AddClass("open");

        // GetLayoutX/Y return absolute window coordinates, so use them directly.
        float ax = anchorElement->GetLayoutX();
        float ay = anchorElement->GetLayoutY();
        float aw = anchorElement->GetLayoutWidth();

        menuPanel->Overrides().Set(Style::PositionLeft, StyleLength::Px(ax + aw));
        menuPanel->Overrides().Set(Style::PositionTop, StyleLength::Px(ay - kSubmenuAlignInset));

        BuildMenuLevel(panelRaw, *list, level);

        m_OpenMenus.push_back(panelRaw);
        m_OpenMenuParentIds.push_back(parentId);
        if (anchorElement)
            anchorElement->AddClass("submenu-open");
        m_OpenMenuAnchors.push_back(anchorElement);
        m_OverlayRoot->AddChild(std::move(menuPanel));
        // Drop pointer capture once this press finishes dispatching. Capture
        // pins hover to the capturing element, so every row would sit under a
        // pointer whose hover belongs elsewhere — and releasing inline is too
        // early, because an ancestor later in the same dispatch captures again.
        m_OverlayRoot->PostSafeAction([manager = m_UIManager]() {
            if (manager)
                manager->ReleaseMouseCapture();
        });
        // Same reason as the close-only path above: the swap of submenu
        // panels happened after the last mouse move, so hover under the
        // stationary cursor is stale until re-evaluated.
        m_UIManager->RequestHoverRefresh();
    }

    void UIContextMenu::BuildMenuLevel(UIElement* container, const std::vector<std::unique_ptr<MenuItem>>& items,
                                       int level)
    {
        // A separator-delimited group holding radio items is a selection: its
        // unselected rows dim so the chosen one reads at a glance, and every
        // selectable row carries a mark. Submenu parents never dim — a folder
        // is navigation, not a rejected choice. Independent checkboxes live
        // outside all of that: they show a tick when on and nothing when off.
        std::vector<bool> groupIsSelection;
        {
            bool current = false;
            for (const auto& item : items)
            {
                if (item->isSeparator)
                {
                    groupIsSelection.push_back(current);
                    current = false;
                    continue;
                }
                if (item->isSubMenu)
                    continue;
                if ((item->flags & MenuItemFlag_Radio) != 0)
                    current = true;
            }
            groupIsSelection.push_back(current);
        }
        size_t group = 0;
        for (const auto& item : items)
        {
            if (item->isSeparator)
            {
                auto sep = std::make_unique<UIElement>();
                sep->AddClass("dropdown-separator");
                container->AddChild(std::move(sep));
                ++group;
                continue;
            }

            const bool inSelectionGroup = groupIsSelection[group] && !item->isSubMenu;
            AppendItemRow(container, *item, level, {}, inSelectionGroup);
        }
    }

    void UIContextMenu::AppendItemRow(UIElement* container, const MenuItem& item, int level,
                                      const std::string& path, bool inSelectionGroup)
    {
        bool enabled = item.staticEnabled;
        bool checked = item.staticChecked;
        if (!item.isSubMenu && m_StateProvider)
        {
            auto state = m_StateProvider(item.id);
            enabled = enabled && state.Enabled;
            checked = checked || state.Checked;
        }

        // Row container — this is what receives hover/click events and carries
        // the dropdown-item styling. Using a UIElement rather than a Label so
        // we can place the text label and optional icon side by side.
        auto row = std::make_unique<UIElement>();
        UIElement* raw = row.get();
        raw->AddClass("dropdown-item");
        // Disabled rows keep only their disabled styling: the selection
        // classes carry hover rules that would out-rank the disabled hover
        // suppression and light an inert row's text.
        if (inSelectionGroup && enabled)
            raw->AddClass(checked ? "row-selected" : "dim-unselected");
        raw->Overrides().Set(Style::Display, DisplayMode::Flex);
        raw->Overrides().Set(Style::FlexDir, FlexDirection::Row);
        raw->Overrides().Set(Style::AlignItems, AlignItems::Center);

        // The icon column is reserved on every row, iconed or not, so labels
        // line up down the menu instead of stepping right wherever an icon
        // happens to resolve.
        auto icon = std::make_unique<UIElement>();
        icon->AddClass("context-menu-item-icon");
        icon->Overrides().Set(Style::PointerEvents, false);
        const std::string& iconPath = item.iconPath;
        if (!iconPath.empty())
        {
            BackgroundImageSource source{};
            source.Kind = BackgroundImageSource::SourceKind::Path;
            source.Value = iconPath;
            const auto colon = iconPath.find(':');
            if (colon != std::string::npos)
            {
                source.SourceAlias = iconPath.substr(0, colon);
                source.Value = iconPath.substr(colon + 1);
            }

            icon->Overrides()
                .Set(Style::BackgroundImage, std::move(source))
                .Set(Style::BackgroundRepeatProp, BackgroundRepeat::NoRepeat)
                .Set(Style::BackgroundSize, BackgroundSizeValue{BackgroundSizeMode::Contain})
                .Set(Style::BackgroundPosition, BackgroundPositionValue{50.0f, true, 50.0f, true});
        }
        raw->AddChild(std::move(icon));

        auto lbl = std::make_unique<Label>();
        // The row carries .dropdown-item and therefore the :hover rule. A
        // child under the cursor becomes the hover target instead, and the
        // row never lights up — so the contents stay out of hit-testing.
        lbl->Overrides().Set(Style::PointerEvents, false);
        lbl->Overrides().Set(Style::FlexGrow, 1.0f);
        lbl->SetText(item.title);
        raw->AddChild(std::move(lbl));

        // Two commands can share a name under different submenus, so a filtered
        // row says where it came from.
        // Filtered rows show icon + command name only; the submenu the match
        // came from stays available as a tooltip rather than a trailing label.
        if (!path.empty())
            raw->SetTooltip(path + kMenuPathSeparator + item.title);

        if (!item.colorHex.empty())
        {
            // Trailing swatch: the row's current color (tag dot, picker rows).
            uint32_t rgb = 0;
            const std::string& hx = item.colorHex;
            if (hx.size() == 7 && hx[0] == '#')
                rgb = static_cast<uint32_t>(std::strtoul(hx.c_str() + 1, nullptr, 16));
            else if (hx.size() == 4 && hx[0] == '#')
            {
                auto d = [&](int i) { return std::strtoul(std::string(2, hx[i]).c_str(), nullptr, 16); };
                rgb = static_cast<uint32_t>((d(1) << 16) | (d(2) << 8) | d(3));
            }
            auto swatch = std::make_unique<UIElement>();
            swatch->AddClass("dropdown-item-swatch");
            swatch->Overrides()
                .Set(Style::BackgroundColor, 0xFF000000u | rgb)
                .Set(Style::PointerEvents, false);
            raw->AddChild(std::move(swatch));
        }

        // A trailing "..." is the dialog convention: the row is a command,
        // not a selectable option, so it carries no selection slot even when
        // it shares a group with checked rows. (A MenuItemFlag_Checkable
        // would make this explicit per call site if the convention ever
        // proves too coarse.)
        const bool isDialogCommand = item.title.size() >= 3 &&
                                     item.title.compare(item.title.size() - 3, 3, "...") == 0;
        if (inSelectionGroup && enabled && !item.isSubMenu && !isDialogCommand)
        {
            // Selection groups mark every selectable row on the right: the
            // chosen one with the check, the rest with an empty outline slot
            // that says "selectable, not selected".
            auto mark = std::make_unique<UIElement>();
            mark->AddClass(checked ? "dropdown-item-check" : "dropdown-item-check-slot");
            mark->Overrides().Set(Style::PointerEvents, false);
            raw->AddChild(std::move(mark));
        }
        else if (checked && !item.isSubMenu)
        {
            auto check = std::make_unique<UIElement>();
            check->AddClass("dropdown-item-check");
            check->Overrides().Set(Style::PointerEvents, false);
            raw->AddChild(std::move(check));
        }

        if (item.isSubMenu)
        {
            // A text glyph, not the arrow icon: NSMenu's chevron is a bold
            // compact angle, and U+203A at menu size matches it where the
            // thin-stroke arrow art cannot.
            auto arrow = std::make_unique<Label>();
            arrow->SetText("\xE2\x80\xBA"); // U+203A as UTF-8; \u escape trips MSVC C4566
            arrow->AddClass("dropdown-item-arrow");
            arrow->AddClass("dropdown-item-arrow-glyph");
            arrow->Overrides().Set(Style::PointerEvents, false);
            raw->AddChild(std::move(arrow));
        }

        if (!enabled)
        {
            raw->AddClass("disabled");
        }
        else
        {
            raw->RegisterEventHandler(kEventMouseEnter,
                                      [this, isSubMenu = item.isSubMenu, itemId = item.id, raw, level](UIEvent&)
                                      {
                                          // Coalesce: a moving cursor fires enters far faster than the
                                          // deferred actions drain (one per frame), so without the serial
                                          // the backlog keeps opening and closing submenus for stale rows
                                          // long after the cursor stops — wiping hover each frame and
                                          // finishing on a submenu for a row the cursor already left.
                                          const uint64_t serial = ++m_SubMenuRequestSerial;
                                          raw->PostSafeAction([this, isSubMenu, itemId, raw, level, serial]()
                                          {
                                              if (serial != m_SubMenuRequestSerial)
                                                  return;
                                              if (isSubMenu)
                                              {
                                                  OpenSubMenu(itemId, raw, level + 1);
                                              }
                                              else
                                              {
                                                  OpenSubMenu(0, nullptr, level + 1);
                                              }
                                          });
                                      });

            if (!item.isSubMenu)
            {
                // A selection click can stay open so several choices can be
                // made; an independent toggle closes the menu the way any other
                // command does.
                const bool stickyEligible = inSelectionGroup && !isDialogCommand;
                const bool radio = (item.flags & MenuItemFlag_Radio) != 0;
                raw->RegisterEventHandler(kEventMouseDown,
                                          [this, id = item.id, raw, stickyEligible, radio](UIEvent& e)
                {
                    if (e.Button != 0) return;
                    if (const char* v = std::getenv("GE_UI_HOVER_TRACE"); v && v[0] == '1')
                        Logger::Log::Info("[MenuTrace] command id={} dispatched={}", id,
                                          m_CommandHandler != nullptr);
                    if (m_CommandHandler) m_CommandHandler(id);
                    if (stickyEligible && GetContextMenuKeepOpenOnToggle())
                    {
                        // A selection click keeps the menu open so several
                        // choices can be made; outside click or Escape closes.
                        UpdateSelectionVisuals(raw, radio);
                    }
                    else
                    {
                        CloseAll();
                    }
                    e.Stop();
                });
            }
        }

        container->AddChild(std::move(row));
    }
} // namespace GameEngine
