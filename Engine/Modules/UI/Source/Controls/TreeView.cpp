#include "UI/Controls/TreeView.h"
#include "Rendering/Text/FontAtlas.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/VirtualWindowCore.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIManager.h"
#include "UI/StyleProperties.h"
#include "UI/VirtualizationCoordinator.h"
#include "UI/Interaction/DragDropManager.h"
#include "UI/Interaction/ItemResizeGesture.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Platform/SystemMetrics.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cmath>

#include "Logger/Logger.h"

#include "UI/Interaction/DropTarget.h"
#include "UI/UIManager.h"

namespace GameEngine
{

float TreeFoldoutGlyphSizePxForTreeIconSize(float treeIconSizePx)
{
    // Linear map: 14px icon → 14px chevron, 64px icon → 28px chevron (2× baseline at max hierarchy scale).
    constexpr float kChevronLowPx = 14.0f;
    constexpr float kIconAnchorLowPx = 14.0f;
    constexpr float kIconAnchorHighPx = 64.0f;
    constexpr float kChevronHighPx = 28.0f;

    const float icon = std::max(1.0f, treeIconSizePx);
    const float t = std::clamp(icon, kIconAnchorLowPx, kIconAnchorHighPx);
    const float span = kIconAnchorHighPx - kIconAnchorLowPx;
    const float u = (t - kIconAnchorLowPx) / span;
    return kChevronLowPx + u * (kChevronHighPx - kChevronLowPx);
}

namespace
{
void ApplyTreeFoldoutToggleLayout(Label* toggle, float iconSizePx)
{
    if (!toggle)
        return;
    const float s = TreeFoldoutGlyphSizePxForTreeIconSize(iconSizePx);
    // Runs on every row rebind. Every write below is a same-value no-op in
    // the override store, and real changes raise layout/visual dirt through
    // the wired dirty callback — pure recycling marks nothing.
    toggle->Styles().SetBackgroundSizeExplicit(s, false, s, false);
    toggle->Overrides()
        .Set(Style::Width, StyleLength::Px(s))
        .Set(Style::Height, StyleLength::Px(s))
        .Set(Style::MinWidth, StyleLength::Px(s))
        .Set(Style::MinHeight, StyleLength::Px(s));
}

/// A–Z / 0–9 only; Finder-style match on first label character (ASCII). Same rules as AssetsPanel.
int TypeAheadKeyToMatchChar(int key)
{
    if (key >= Input::kKeyCode_A && key <= Input::kKeyCode_Z)
        return std::tolower(key);
    if (key >= Input::kKeyCode_0 && key <= Input::kKeyCode_9)
        return key;
    return -1;
}

bool LabelFirstCharMatches(const char* label, char matchLowerOrDigit)
{
    if (!label || label[0] == '\0')
        return false;
    const unsigned char first = static_cast<unsigned char>(label[0]);
    if (first >= 128u)
        return false;
    return static_cast<char>(std::tolower(first)) == matchLowerOrDigit;
}

constexpr std::chrono::milliseconds kTreeTypeAheadRepeatWindow(750);
// Set on an icon hit target by a click until the pointer leaves it; a style sheet rule on it holds
// the icon's hover preview off meanwhile.
constexpr const char* kIconHoverPreviewSuppressedClass = "tree-icon-hover-preview-suppressed";
} // namespace

// VirtualWindowCore::Impact mirrors UIManager::VirtualizationImpact bit-for-bit
// so the aggregated impact forwards with a single cast. (Same checks live in
// GridView.cpp / ListView.cpp; static_assert produces no symbol, so repeating
// them per-TU is fine.)
static_assert(static_cast<std::uint32_t>(UI::VirtualWindowCore::Impact::Rebind) ==
                  UIManager::VirtualizationRebindChanged,
              "Impact::Rebind must match VirtualizationRebindChanged");
static_assert(static_cast<std::uint32_t>(UI::VirtualWindowCore::Impact::LayoutRects) ==
                  UIManager::VirtualizationLayoutRectsChanged,
              "Impact::LayoutRects must match VirtualizationLayoutRectsChanged");
static_assert(static_cast<std::uint32_t>(UI::VirtualWindowCore::Impact::Topology) ==
                  UIManager::VirtualizationTopologyChanged,
              "Impact::Topology must match VirtualizationTopologyChanged");

// Bridges the flat row pool to the shared window engine. TreeView is one lane (a
// line is a fixed-height row), so a flat slot equals the ring's line slot and an
// item index equals the flat-list index.
struct TreeView::HostAdapter final : UI::VirtualWindowCore::Host
{
    TreeView& Owner;

    explicit HostAdapter(TreeView& owner) : Owner(owner) {}

    void EnsurePool(int slotCount) override
    {
        Owner.EnsureRowPoolSize(slotCount);
        Owner.EnsureAttachedRows(slotCount);
    }
    int SlotCount() const override { return static_cast<int>(Owner.m_RowPool.size()); }
    UI::VirtualWindowCore::Impact Rebind(int slot, int itemIndex) override
    {
        return Owner.BindRow(slot, itemIndex) ? UI::VirtualWindowCore::Impact::Rebind
                                              : UI::VirtualWindowCore::Impact::None;
    }
    void UnbindSlot(int slot) override { Owner.UnbindRow(slot); }
    bool SlotBinds(int slot, int itemIndex) const override
    {
        if (slot < 0 || slot >= (int)Owner.m_RowPool.size())
            return false;
        const RowSlot& s = Owner.m_RowPool[(size_t)slot];
        return s.boundId != 0 && s.boundIndex == itemIndex;
    }
    void DestroyTailSlot() override { Owner.DestroyTailSlot(); }
};

TreeView::TreeView()
{
    SetFocusable(true);

    auto sv = std::make_unique<ScrollView>();
    m_Scroll = sv.get();
    auto content = std::make_unique<UIElement>();
    content->AddClass("tree-content");
    content->AddClass("tree");
    m_Content = content.get();
    m_Scroll->AddContent(std::move(content));
    AddChild(std::move(sv));

    // Every item view answers the one host-configured item resize gesture.
    if (m_Scroll)
    {
        m_Scroll->RegisterEventHandler(kEventScroll, [this](UIEvent& e) {
            if (UI::MatchesItemResizeGesture(e.Mods) && e.ScrollY != 0.0f && m_OnItemResizeGesture)
            {
                m_OnItemResizeGesture(e.ScrollY);
                e.Stop();
            }
        });
    }

    // Update virtualization while scrolling. ScrollView defers callbacks to UIManager
    // so this runs outside event dispatch and can safely rebind pooled rows.
    if (m_Scroll)
    {
        m_Scroll->SetOnScrollChanged([this](float /*scrollX*/, float scrollY)
                                     {
                                         if (UIElement::IsInEventDispatch())
                                         {
                                             if (UIManager* ui = GetOwnerManager())
                                             {
                                                 ui->EnqueueVirtualizationWork(this,
                                                                               [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason /*reason*/)
                                                                               {
                                                                                   static_cast<TreeView*>(ctx)->UpdateVirtualization(VirtualizationCoordinator::Reason::ScrollChanged);
                                                                               },
                                                                               VirtualizationCoordinator::Reason::ScrollChanged);
                                             }
                                             else
                                             {
                                                 UpdateVirtualization(VirtualizationCoordinator::Reason::ScrollChanged);
                                             }
                                         }
                                         else
                                         {
                                             // UpdateVirtualization reports its own impact iff rows were
                                             // rebound; a sub-row tick must not schedule a late relayout.
                                             UpdateVirtualization(VirtualizationCoordinator::Reason::ScrollChanged);
                                         }
                                         if (m_OnScrollChanged)
                                             m_OnScrollChanged(scrollY);
                                     });
    }

    // Context menu (empty space). GridView already supports this; TreeView needs it as well.
    if (m_Content)
    {
        m_Content->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
                                        {
            if (e.Button != 1) // RMB
                return;
            if (m_OnContextMenu)
                m_OnContextMenu(0, e.X, e.Y);
            e.Stop(); });

        // Left-click on empty space: clear selection (when enabled).
        m_Content->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e)
                                        {
            if (e.Button != 0)
                return;
            if (e.CurrentTarget != m_Content || e.Target != m_Content)
                return;
            if (m_ClearSelectionOnBackgroundClick && m_Selection)
            {
                m_Selection->Clear();
                UpdateSelectionClasses();
                if (m_OnSelectionChanged)
                    m_OnSelectionChanged(0);
            }
            m_PendingCollapseToSingleId = 0;
            e.Stop(); });
    }
}

TreeView::~TreeView()
{
    if (UIManager* registered = m_RegisteredVirtualizationManager.Get())
        registered->UnregisterVirtualizedControl(this);
}

// Widest bound row, measured from the row's own contents rather than from the
// ScrollView's label scan.
//
// The scan cannot serve here: its result feeds the content width, the content
// width sizes the rows, the row size moves the labels, and the labels feed the
// scan. That loop is what TreeView::LayoutRows warns against, and it settles on
// a width several times the real one. Everything below is independent of the
// row's width - indent comes from the item's depth, the text width from the
// string and its font, and the trailing reserve from the title's own margin,
// which is what the theme uses to hold the lock and render-layer strip. So the
// extent includes those affordances and horizontal scrolling reaches them.
float TreeView::MeasureNaturalRowWidth() const
{
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return 0.0f;

    float widest = 0.0f;
    for (const RowSlot& slot : m_RowPool)
    {
        if (!slot.attached || slot.boundIndex < 0 || !slot.title || !slot.row)
            continue;

        const ResolvedStyle& ts = slot.title->GetResolvedStyle();
        if (!ts.Visual.Visible || ts.Layout.DisplayMode == DisplayMode::None)
            continue;

        Rendering::Text::FontAtlas* font = ui->ResolveFontForStyle(ts);
        if (!font)
            continue;

        const std::string& text = slot.title->GetTextContent();
        if (text.empty())
            continue;

        const float pixelSize = std::max(1.0f, ts.Visual.FontSize);
        const float textW =
            font->MeasureText(text, pixelSize, ts.Visual.LetterSpacing).metrics.width;

        // Where the title starts inside the row. Taken as the title's offset from
        // the row rather than derived from pad + indent + foldout: hosts add their
        // own leading elements (the Hierarchy's preview icon), and a formula that
        // does not know about them measures every row short, leaving the row wider
        // than the scroll range so its tail can never be reached. The offset is
        // set by the widths of the siblings ahead of the title, not by the row's
        // own width, so unlike a width readback it stays correct mid-resize.
        const float titleStart = slot.title->GetLayoutX() - slot.row->GetLayoutX();
        // Before the first layout both rects read 0; measuring that would report
        // the content as narrow and drop the bar.
        if (titleStart <= 0.0f)
            continue;
        // The title's own right margin IS the strip the theme holds clear for the
        // row's trailing affordances (lock, VCS dot, layer dropdown), and it is
        // per row. Reading it here keeps one owner for that figure: a second copy
        // in the host drifts from the stylesheet and measures the row short, so
        // the tail of a long name can never be scrolled into view. A margin is a
        // style value, not a laid-out rect, so unlike a position readback it does
        // not describe the previous width mid-resize.
        //
        // A little trailing slack, so the bar shows just before the last glyphs
        // are actually touching the edge and there is somewhere to scroll to
        // rather than a one-pixel range.
        constexpr float kTrailingSlackPx = 12.0f;
        const ResolvedStyle& rs = slot.row->GetResolvedStyle();
        const float natural = titleStart + ts.Layout.Padding.Left + textW +
                              ts.Layout.Padding.Right + ts.Layout.Margin.Right +
                              rs.Layout.Padding.Right + kTrailingSlackPx;
        widest = std::max(widest, natural);
        // A row min-width is a floor the theme puts under a row that carries a
        // wide trailing control. Rows are all laid out to one width, so a floor
        // that only reaches layout makes those rows overhang the others and
        // their right-anchored affordances stop lining up. Folding it into the
        // measurement raises the width every row gets, and gives the scrollbar
        // the range to reach it.
        if (rs.Layout.MinWidth.IsPx())
            widest = std::max(widest, rs.Layout.MinWidth.Value);
    }
    return widest;
}

void TreeView::OnOwnerManagerChanged(UIManager* owner)
{
    if (UIManager* registered = m_RegisteredVirtualizationManager.Get())
        registered->UnregisterVirtualizedControl(this);
    m_RegisteredVirtualizationManager = UIManagerRef(owner);
    if (owner)
        owner->RegisterVirtualizedControl(this);
}

void TreeView::EnqueuePumpWork(UIManager& ui)
{
    ui.EnqueueVirtualizationWork(this,
                                 [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason /*reason*/)
                                 { static_cast<TreeView*>(ctx)->UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged); },
                                 VirtualizationCoordinator::Reason::DataChanged);
}

void TreeView::SetRowHeight(float px)
{
    px = std::max(1.0f, px);
    if (std::fabs(px - m_RowHeight) < 0.01f)
        return;

    // Defer if called during event dispatch to avoid layout issues
    if (UIElement::IsInEventDispatch())
    {
        this->PostSafeAction([this, px]()
                         { this->SetRowHeight(px); });
        return;
    }

    m_RowHeight = px;

    // Row height changes the window math for every row; drop the core's guard/ring
    // so the next update fully rebinds at the new positions, and reset the
    // OnPostLayout viewport-coalescing cache so that pass re-enqueues.
    m_WindowCore.Reset();
    m_LastViewportWPx = -1;
    m_LastViewportHPx = -1;

    // Ensure virtualization updates happen at a safe point (and not during event dispatch).
    if (UIManager* ui = GetOwnerManager())
    {
        ui->EnqueueVirtualizationWork(this,
                                      [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason /*reason*/)
                                      {
                                          static_cast<TreeView*>(ctx)->UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
                                      },
                                      VirtualizationCoordinator::Reason::DataChanged);
    }
    else
    {
        UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
    }

    // Mark dirty and request relayout so scroll metrics/layout update immediately.
    MarkDirty(LayoutDirty | VisualDirty);
    RequestRelayout();
}

void TreeView::ScrollToItem(TreeId id)
{
    if (!m_Scroll)
        return;

    if (m_FlatDirty)
        RebuildFlatList();

    const int flatIndex = FlatIndexOf(id);
    if (flatIndex < 0)
        return;

    // Ensure the ScrollView has a content size that reflects the current flat
    // list. UpdateVirtualization normally does this, but it drains AFTER
    // ConvergePostLayout — so when ScrollToItem is called during a panel's
    // OnPostLayout on first open, contentH is still 0 and SetScrollY would
    // clamp the target to 0 (because maxScroll = contentH - viewportH <= 0).
    // Explicitly priming contentH here makes the first scroll land correctly.
    const float viewportW = m_Scroll->GetViewportWidth();
    const float totalH = static_cast<float>(m_Flat.size()) * m_RowHeight;
    m_Scroll->SetContentSize(viewportW, totalH);

    const float itemCenter = (static_cast<float>(flatIndex) + 0.5f) * m_RowHeight;
    const float viewHeight = m_Scroll->GetViewportHeight();
    const float target = std::max(0.0f, itemCenter - viewHeight * 0.5f);
    m_Scroll->SetScrollY(target);
    m_Scroll->FlushPendingScrollChanged();
}

void TreeView::SetChildIndent(float px)
{
    px = std::max(0.0f, px);
    if (std::fabs(px - m_ChildIndentPx) < 0.01f)
        return;

    // Defer if called during event dispatch to avoid layout issues
    if (UIElement::IsInEventDispatch())
    {
        this->PostSafeAction([this, px]()
                         { this->SetChildIndent(px); });
        return;
    }

    m_ChildIndentPx = px;

    // Indent isn't part of the core guard, so drop the guard/ring to force a full
    // rebind that reapplies the new indent to every visible row, and reset the
    // OnPostLayout viewport-coalescing cache so that pass re-enqueues.
    m_WindowCore.Reset();
    m_LastViewportWPx = -1;
    m_LastViewportHPx = -1;

    // Ensure virtualization updates happen at a safe point (and not during event dispatch).
    if (UIManager* ui = GetOwnerManager())
    {
        ui->EnqueueVirtualizationWork(this,
                                      [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason /*reason*/)
                                      {
                                          static_cast<TreeView*>(ctx)->UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
                                      },
                                      VirtualizationCoordinator::Reason::DataChanged);
    }
    else
    {
        UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
    }

    // Mark dirty and request relayout so scroll metrics/layout update immediately.
    MarkDirty(LayoutDirty | VisualDirty);
    RequestRelayout();
}

void TreeView::SetIconSize(float px)
{
    px = std::max(1.0f, px);
    if (std::fabs(px - m_IconSizePx) < 0.01f)
        return;

    m_IconSizePx = px;
    
    // Update all pooled rows with new icon size and padding
    for (auto& slot : m_RowPool)
    {
        if (slot.toggle)
            ApplyTreeFoldoutToggleLayout(slot.toggle, px);
        if (slot.title)
        {
            // Only apply background size and padding if title uses background icon
            // (i.e., does NOT have tree-title-no-icon class which uses separate icon element)
            if (!slot.title->HasClass("tree-title-no-icon"))
            {
                slot.title->Styles().SetBackgroundSizeExplicit(px, false, px, false);
                
                // Add extra spacing after the icon: icon size + 8px gap
                slot.title->Overrides().Set(Style::PaddingLeft, StyleLength::Px(px + 8.0f));
            }
        }
    }
    // Padding affects layout; request relayout so new indent applies immediately.
    MarkDirty(LayoutDirty | VisualDirty);
    RequestRelayout();
}

void TreeView::SetHorizontalScrollEnabled(bool v)
{
    if (m_HorizontalScrollEnabled == v)
        return;
    m_HorizontalScrollEnabled = v;
    if (v)
        AddClass("tree-horizontal-scroll-enabled");
    else
        RemoveClass("tree-horizontal-scroll-enabled");
    MarkDirtySubtree(StyleDirty | LayoutDirty | VisualDirty);
    RequestRelayout();
}

void TreeView::SetOnLockClicked(std::function<void(TreeId, UIElement*)> cb)
{
    m_LockEnabled = true;
    m_OnLockClicked = std::move(cb);
    AddClass("tree-row-lock-enabled");
    MarkDirtySubtree(StyleDirty | LayoutDirty | VisualDirty);
    RequestRelayout();
}

void TreeView::EnsureAttachedRows(int desired)
{
    if (!m_Content)
        return;

    desired = std::clamp(desired, 0, (int)m_RowPool.size());
    for (int i = 0; i < (int)m_RowPool.size(); ++i)
    {
        RowSlot& slot = m_RowPool[(size_t)i];
        if (!slot.row)
            continue;

        if (i < desired)
        {
            if (!slot.attached)
            {
                if (slot.detached)
                {
                    m_Content->AddChild(std::move(slot.detached));
                }
                slot.attached = true;
            }
        }
        else
        {
            // Keep pooled rows attached during scroll-driven virtualization updates to avoid
            // mid-frame tree mutations (prevents flicker from stale traversal caches).
            if (slot.boundId != 0)
            {
                if (UIManager* ui = GetOwnerManager())
                    ui->ClearHoverForSubtree(slot.row);
            }
            slot.row->RemoveClass("selected");
            slot.row->RemoveClass("drop-onto-valid");
            slot.row->RemoveClass("drop-onto-invalid");
            slot.boundId = 0;
            slot.boundIndex = -1;
            UI::Layout::SetElementInvisible(*slot.row, true);
            slot.attached = true;
        }
    }
}

void TreeView::ExpandAll()
{
    if (!m_Provider)
    {
        return;
    }

    m_Expanded.clear();

    // Recursively expand all expandable nodes starting from each root.
    std::function<void(TreeId)> expand = [&](TreeId id)
    {
        if (!m_Provider->IsExpandable(id))
        {
            return;
        }
        SetExpanded(id, true);
        const int childCount = m_Provider->GetChildCount(id);
        for (int i = 0; i < childCount; ++i)
        {
            TreeId childId = m_Provider->GetChildId(id, i);
            expand(childId);
        }
    };

    const int rootCount = m_Provider->GetRootCount();
    for (int i = 0; i < rootCount; ++i)
    {
        TreeId rootId = m_Provider->GetRootId(i);
        expand(rootId);
    }

    RefreshFromProvider();
}

void TreeView::SetSubtreeExpanded(TreeId root, bool expanded)
{
    if (!m_Provider || root == 0)
        return;

    std::function<void(TreeId)> dfs = [&](TreeId node)
    {
        if (node == 0 || !m_Provider->IsExpandable(node))
            return;
        SetExpanded(node, expanded);
        const int cc = m_Provider->GetChildCount(node);
        for (int i = 0; i < cc; ++i)
            dfs(m_Provider->GetChildId(node, i));
    };

    dfs(root);
}

void TreeView::RebuildFlatList()
{
    m_Flat.clear();
    m_FlatIndexById.clear();
    ++m_FlatGeneration;
    if (!m_Provider)
    {
        m_FlatDirty = false;
        return;
    }

    std::function<void(TreeId, int)> add = [&](TreeId id, int depth)
    {
        if (id == 0)
            return;
        if (m_VisibilityFilter && !m_VisibilityFilter(id))
            return;
        m_FlatIndexById[id] = (int)m_Flat.size();
        m_Flat.push_back(FlatItem{id, depth});
        if (!IsExpanded(id))
            return;
        const int cc = m_Provider->GetChildCount(id);
        for (int i = 0; i < cc; ++i)
        {
            add(m_Provider->GetChildId(id, i), depth + 1);
        }
    };

    const int rootCount = m_Provider->GetRootCount();
    if (rootCount <= 0)
    {
        m_FlatDirty = false;
        return;
    }

    if (m_ShowRoot)
    {
        for (int i = 0; i < rootCount; ++i)
        {
            add(m_Provider->GetRootId(i), 0);
        }
    }
    else
    {
        if (rootCount == 1)
        {
            // Legacy behaviour: show the first root's children.
            TreeId rid = m_Provider->GetRootId(0);
            const int cc = m_Provider->GetChildCount(rid);
            for (int i = 0; i < cc; ++i)
            {
                add(m_Provider->GetChildId(rid, i), 0);
            }
        }
        else
        {
            // Multi-root behaviour: show all roots at depth 0.
            // This supports providers that expose multiple independent root sections
            // (e.g. Smart Folders + filesystem root) without forcing callers to show
            // a synthetic root wrapper.
            for (int i = 0; i < rootCount; ++i)
            {
                add(m_Provider->GetRootId(i), 0);
            }
        }
    }

    m_FlatDirty = false;
}

int TreeView::FlatIndexOf(TreeId id) const
{
    const auto it = m_FlatIndexById.find(id);
    return it != m_FlatIndexById.end() ? it->second : -1;
}

void TreeView::EnsureRowPoolSize(int desired)
{
    if (!m_Content || desired <= 0)
        return;

    desired = std::max(desired, 0);
    while ((int)m_RowPool.size() < desired)
    {
        const int slotIndex = (int)m_RowPool.size();

        auto row = std::make_unique<UIElement>();
        row->AddClass("tree-item");
        // Virtualized pool elements must keep their geometry even when fully clipped.
        // UIManager scrolls by translating cached geometry; if offscreen items are culled during
        // geometry generation, they can appear partially missing when scrolled into view
        // (this is especially visible on horizontal scrolling where long labels enter view).
        row->SetDisableClipCulling(true);

        auto toggle = std::make_unique<Label>();
        // Use a TreeView-specific class name to avoid clashing with the editor-wide
        // `.foldout` styling used by code-folding widgets.
        toggle->AddClass("tree-foldout");
        Label* toggleRaw = toggle.get();
        toggleRaw->SetDisableClipCulling(true);

        auto title = std::make_unique<Label>();
        title->AddClass("tree-title");
        Label* titleRaw = title.get();
        titleRaw->SetDisableClipCulling(true);
        titleRaw->Styles().SetBackgroundSizeExplicit(m_IconSizePx, false, m_IconSizePx, false);
        ApplyTreeFoldoutToggleLayout(toggleRaw, m_IconSizePx);

        const auto beginIconInteraction = [this, slotIndex](UIEvent& e)
        {
            if (!m_OnIconClicked || e.Button != 0)
                return false;
            if (slotIndex < 0 || slotIndex >= (int)m_RowPool.size())
                return false;

            RowSlot& slot = m_RowPool[slotIndex];
            const TreeId id = slot.boundId;
            if (id == 0 || !slot.row)
                return false;

            // Once the click commits a new state, keep showing that committed state
            // until the pointer leaves the explicit icon. Otherwise :hover immediately
            // previews the inverse state while the pointer is still stationary.
            if (e.Target && e.Target->HasClass("tree-icon-hit-target"))
                e.Target->AddClass(kIconHoverPreviewSuppressedClass);

            m_IconDragging = true;
            m_IconAppliedInCurrentDrag.clear();
            m_IconAppliedInCurrentDrag.insert(id);

            m_OnIconClicked(id, slot.row);

            if (m_OnIconStateCheck)
                m_IconDragTargetState = m_OnIconStateCheck(id);

            e.Capture(this);
            e.Stop();
            return true;
        };

        // Toggle expansion - handle MouseUp for the actual toggle
        toggleRaw->RegisterEventHandler(kEventMouseUp, [this, slotIndex](UIEvent& e)
                                        {
            if (!m_Provider) return;
            if (slotIndex < 0 || slotIndex >= (int)m_RowPool.size()) return;
            const TreeId id = m_RowPool[slotIndex].boundId;
            if (id == 0) return;
            if (!m_Provider->IsExpandable(id)) return;
            const bool willExpand = !IsExpanded(id);
            if (m_FoldoutTogglesEntireSubtree)
                SetSubtreeExpanded(id, willExpand);
            else
                SetExpanded(id, willExpand);
            m_FlatDirty = true;
            RefreshFromProvider();
            e.Stop(); });
        
        // Stop MouseDown on foldout to prevent it from bubbling to the row
        // (which would trigger selection change and interfere with foldout-only clicks)
        toggleRaw->RegisterEventHandler(kEventMouseDown, [this, slotIndex](UIEvent& e)
                                        {
            if (!m_Provider) return;
            if (slotIndex < 0 || slotIndex >= (int)m_RowPool.size()) return;
            const TreeId id = m_RowPool[slotIndex].boundId;
            if (id == 0) return;
            if (!m_Provider->IsExpandable(id)) return;
            e.Stop(); });

        // Selection + activation
        row->RegisterEventHandler(kEventMouseDown, [this, slotIndex, beginIconInteraction](UIEvent& e)
                                  {
	        // Callers that render an icon as a separate element mark that element as
	        // the hit target. Its MouseDown bubbles directly to the row (the title is
	        // a sibling), so handle it before the normal row-selection gesture.
	        if (e.Target && e.Target->HasClass("tree-icon-hit-target"))
	        {
	            if (beginIconInteraction(e))
	                return;
	        }

	        if (slotIndex < 0 || slotIndex >= (int)m_RowPool.size()) return;
            RowSlot& slot = m_RowPool[slotIndex];
	        const TreeId id = slot.boundId;
	        if (id == 0) return;
            const bool isLmb = (e.Button == 0);
            const bool isRmb = (e.Button == 1);
            bool deferSelect = false; // set when a plain LMB click is deferred to mouse-up

            // Selection gestures (LMB) + "select on right-click" (RMB).
            if (m_Selection)
            {
                if (isRmb)
                {
                    if (!m_Selection->IsSelected(id))
                        m_Selection->SetSingle(id);
                }
                else if (isLmb)
                {
                    const bool shift = (e.Mods & Input::kModShift) != 0;
                    const bool primaryMod = Input::IsPrimaryShortcutModifier(e.Mods);

                    // A mouse click reseats the keyboard cursor on the clicked
                    // row so subsequent Shift+Arrow extensions start from here.
                    m_KeyboardCursorId = id;

                    if (m_KeyboardSingleSelectOnly)
                    {
                        m_Selection->SetSingle(id);
                        m_PendingCollapseToSingleId = 0;
                    }
                    else if (shift)
                    {
                        TreeId anchor = static_cast<TreeId>(m_Selection->GetAnchor());
                        if (anchor == 0)
                            anchor = id;

                        const int a = FlatIndexOf(anchor);
                        const int b = FlatIndexOf(id);
                        if (a >= 0 && b >= 0)
                        {
                            const int lo = std::min(a, b);
                            const int hi = std::max(a, b);
                            std::vector<UI::Interaction::ItemId> ids;
                            ids.reserve((size_t)(hi - lo + 1));
                            for (int i = lo; i <= hi; ++i)
                            {
                                const TreeId tid = m_Flat[(size_t)i].id;
                                if (tid != 0)
                                    ids.push_back(tid);
                            }

                            if (primaryMod)
                            {
                                const auto cur = m_Selection->GetSelection();
                                ids.insert(ids.end(), cur.begin(), cur.end());
                            }

                            // Anchor remains the original anchor for continued shift gestures.
                            m_Selection->SetSelection(ids, anchor);
                        }
                        else
                        {
                            // Fallback if we can't find indices (stale anchor): behave like single select.
                            m_Selection->SetSingle(id);
                        }
                    }
                    else if (primaryMod)
                    {
                        m_Selection->Toggle(id);
                    }
                    else if (m_SelectOnMouseUp)
                    {
                        // Defer the commit to mouse-up. If the gesture becomes a drag, drag-start
                        // clears m_PendingSelectId so the current selection — and whatever it drives,
                        // e.g. the inspector — survives the gesture. This also subsumes the
                        // collapse-to-single case: dragging an item that's part of a multi-selection
                        // still carries the whole set (the payload builder sees the source id), while
                        // a plain click without a drag collapses to it on mouse-up.
                        m_PendingSelectId = id;
                        deferSelect = true;
                        // Responsiveness: light up the pressed row immediately. This is visual only —
                        // the model/inspector still commit on mouse-up (or revert if it becomes a
                        // drag). IsRowVisuallySelected shows ONLY this row while active, so the
                        // previously-selected rows clear and it reads as a real selection. Skipped if
                        // already selected (it's already highlighted via the model).
                        if (!m_Selection->IsSelected(id))
                        {
                            m_DragHighlightActive = true;
                            m_DragHighlightId = id;
                        }
                    }
                    else
                    {
                    // If the user clicks a currently-selected item and there are multiple selected,
                    // preserve the multi-selection so drag can operate on the full set.
                    //
                    // (Standard file-explorer UX: drag from any selected row drags the selection.)
                    if (m_Selection->IsSelected(id))
                    {
                        const auto cur = m_Selection->GetSelection();
                        if (cur.size() > 1)
                        {
                            m_Selection->SetAnchor(id);
                            m_PendingCollapseToSingleId = id;
                        }
                        else
                        {
                            m_Selection->SetSingle(id);
                            m_PendingCollapseToSingleId = 0;
                        }
                    }
                    else
                    {
                        m_Selection->SetSingle(id);
                        m_PendingCollapseToSingleId = 0;
                    }
                    }
                }

                UpdateSelectionClasses();
            }

            if (isLmb && m_OnSelectionChanged && !deferSelect)
                m_OnSelectionChanged(id);

            if (isLmb)
            {
                PostSafeAction([this]() {
                    if (UIManager* ui = GetOwnerManager())
                        ui->FocusElement(this);
                });
            }

            // Arm drag gesture for row drags (if enabled).
            // IMPORTANT: don't capture when clicking the expander toggle; capturing steals the mouse-up
            // and prevents the toggle's own handler from running (can't expand).
            const bool clickedToggle = (e.Target == slot.toggle);
            // Capture the mouse on press when a drag may follow OR when selection is deferred to
            // mouse-up (we need the captured mouse-up to commit the deferred selection).
            if (isLmb && (m_DragPayloadBuilder || m_SelectOnMouseUp) && !clickedToggle)
            {
                m_DragCandidate = true;
                m_DragCandidateId = id;
                m_DragStartX = e.X;
                m_DragStartY = e.Y;
                e.Capture(this);
            }
	        e.Stop(); });

        // Optional: icon click handling, constrained to the icon region on the left.
        titleRaw->RegisterEventHandler(kEventMouseDown, [this, titleRaw, beginIconInteraction](UIEvent& e)
                                       {
	        if (!m_OnIconClicked)
	            return;
	        if (e.Button != 0)
	            return; // only react to LMB
	        if (!titleRaw)
	            return;
	        // A separate icon element owns the interaction when the title has no
	        // background icon. Do not leave an invisible icon hitbox over the text.
	        if (titleRaw->HasClass("tree-title-no-icon"))
	            return;

	        // Convert from absolute mouse coordinates into the label's local space.
	        const float layoutX = titleRaw->GetLayoutX();
	        const float layoutW = titleRaw->GetLayoutWidth();
	        const float localX = e.X - layoutX;
	        if (localX < 0.0f || localX > layoutW)
	            return;

	        // Match tree title icon column (icon + gap, see BindRow padding).
	        const float iconClickMaxX = m_IconSizePx + 8.0f;
	        if (localX > iconClickMaxX)
	            return;

	        beginIconInteraction(e); });

        row->RegisterEventHandler(kEventMouseUp, [this, slotIndex](UIEvent& e)
                                  {
            if (slotIndex < 0 || slotIndex >= (int)m_RowPool.size()) return;
            const TreeId id = m_RowPool[slotIndex].boundId;
            if (id == 0) return;

            // RMB context menu
            if (e.Button == 1)
            {
                if (m_OnContextMenu) m_OnContextMenu(id, e.X, e.Y);
                e.Stop();
                return;
            }
            // Only LMB participates in activation/double-click.
            if (e.Button != 0)
                return;

            using clock = std::chrono::steady_clock;
            auto now = clock::now();
            bool isDouble =
                (m_LastClickedId == id) &&
                ((now - m_LastClickTime) < Platform::GetDoubleClickInterval());
            m_LastClickedId = id;
            m_LastClickTime = now;
            if (isDouble) {
                // Toggle expand/collapse on double-click
                if (m_Provider && m_Provider->IsExpandable(id))
                {
                    bool willExpand = !IsExpanded(id);
                    SetExpanded(id, willExpand);
                    m_FlatDirty = true;
                    RefreshFromProvider();
                }
                if (m_OnItemActivated) m_OnItemActivated(id);
                e.Stop();
            } });

        // Lock icon — only created for trees that opt in via SetOnLockClicked
        std::unique_ptr<UIElement> lock;
        UIElement* lockRaw = nullptr;
        if (m_LockEnabled)
        {
        lock = std::make_unique<UIElement>();
        lock->AddClass("tree-row-lock");
        lock->AddClass("icon-button");
        lock->SetDisableClipCulling(true);
        lockRaw = lock.get();

        lock->RegisterEventHandler(kEventMouseDown, [this, slotIndex](UIEvent& e)
        {
            if (e.Button != 0) return;
            if (!m_OnLockClicked) return;
            if (slotIndex < 0 || slotIndex >= (int)m_RowPool.size()) return;
            RowSlot& slot = m_RowPool[slotIndex];
            const TreeId id = slot.boundId;
            if (id == 0) return;

            m_LockDragging = true;
            m_LockAppliedInCurrentDrag.clear();
            m_LockAppliedInCurrentDrag.insert(id);

            m_OnLockClicked(id, slot.row);

            if (m_OnLockStateCheck)
                m_LockDragTargetState = m_OnLockStateCheck(id);

            if (slot.lock)
            {
                if (m_LockDragTargetState)
                    slot.lock->AddClass("active");
                else
                    slot.lock->RemoveClass("active");
            }

            e.Capture(this);
            e.Stop();
        });
        } // end if (m_LockEnabled)

        UIElement* rowRaw = row.get();
        rowRaw->AddChild(std::move(toggle));
        rowRaw->AddChild(std::move(title));
        if (lock)
            rowRaw->AddChild(std::move(lock));

        if (m_Content)
        {
            m_Content->AddChild(std::move(row));
        }

        RowSlot slot{};
        slot.row = rowRaw;
        slot.toggle = toggleRaw;
        slot.title = titleRaw;
        slot.lock = lockRaw;
        slot.boundId = 0;
        slot.boundIndex = -1;
        slot.boundDepth = 0;
        slot.attached = true;
        m_RowPool.push_back(std::move(slot));
    }
}

void TreeView::UnbindRow(int slot)
{
    if (slot < 0 || slot >= (int)m_RowPool.size())
        return;
    RowSlot& s = m_RowPool[(size_t)slot];
    if (!s.row)
        return;
    if (s.boundId != 0)
    {
        if (UIManager* ui = GetOwnerManager())
            ui->ClearHoverForSubtree(s.row);
    }
    s.row->RemoveClass("selected");
    s.row->RemoveClass("drop-onto-valid");
    s.row->RemoveClass("drop-onto-invalid");
    s.boundId = 0;
    s.boundIndex = -1;
    s.boundDepth = 0;
    UI::Layout::SetElementInvisible(*s.row, true);
}

void TreeView::DestroyTailSlot()
{
    if (m_RowPool.empty())
        return;
    RowSlot& slot = m_RowPool.back();
    if (m_Content && slot.row)
        (void)m_Content->TakeChild(slot.row);
    m_RowPool.pop_back();
}

bool TreeView::BindRow(int slotIndex, int itemIndex)
{
    if (slotIndex < 0 || slotIndex >= (int)m_RowPool.size())
        return false;

    RowSlot& slot = m_RowPool[slotIndex];
    if (!slot.row || !slot.toggle || !slot.title)
        return false;
    const TreeId prevId = slot.boundId;
    const int prevIndex = slot.boundIndex;

    if (!m_Provider || itemIndex < 0 || itemIndex >= (int)m_Flat.size())
    {
        UnbindRow(slotIndex);
        return false;
    }

    const FlatItem& fi = m_Flat[(size_t)itemIndex];
    const bool bindingChanged = (prevId != fi.id) || (prevIndex != itemIndex);
    if (bindingChanged && prevId != 0)
    {
        if (UIManager* ui = GetOwnerManager())
            ui->ClearHoverForSubtree(slot.row);
    }
    // An icon click latches its hover preview off until the pointer leaves the icon. The latch
    // belongs to the item it was clicked for, so a pooled row taking another item drops it.
    if (prevId != fi.id)
    {
        for (const auto& child : slot.row->GetChildren())
        {
            if (child && child->HasClass("tree-icon-hit-target"))
                child->RemoveClass(kIconHoverPreviewSuppressedClass);
        }
    }

    slot.boundId = fi.id;
    slot.boundIndex = itemIndex;
    slot.boundDepth = fi.depth;

    UI::Layout::SetElementInvisible(*slot.row, false);

    const int topPx = (int)std::lround((float)itemIndex * m_RowHeight);
    const int indentPx = (int)std::lround((float)fi.depth * m_ChildIndentPx);
    const int heightPx = (int)std::lround(m_RowHeight);
    int widthPx = 0;
    if (m_Scroll)
    {
        // IMPORTANT: rows must be sized to the *content* width, not the viewport width.
        //
        // ScrollView applies horizontal scroll by translating the scroll-content subtree left
        // by scrollX. If each virtualized row is only viewportW wide, then once scrollX > 0
        // the row's right edge moves left (viewportW - scrollX), and the row's intersection
        // with the viewport becomes a thin strip. That thin strip then clamps the label's
        // text batch scissor, producing the exact "only a narrow strip is visible" bug.
        //
        // By sizing rows to the content width, the row remains wider than the viewport and
        // the viewport intersection stays full-width while scrolling horizontally.
        const int viewportWPx = (int)std::lround(m_Scroll->GetViewportWidth());
        const int contentWPx = (int)std::ceil(std::max(0.0f, m_Scroll->GetContentWidth()));
        widthPx = std::max(viewportWPx, contentWPx);
    }
    if (widthPx <= 0)
        widthPx = (int)std::lround(GetLayoutWidth());

    UI::Layout::SetAbsolutePosition(*slot.row,
                                    Mathematics::Rect{
                                        0.0f,
                                        static_cast<float>(topPx),
                                        static_cast<float>(std::max(0, widthPx)),
                                        static_cast<float>(std::max(0, heightPx)),
                                    },
                                    /*positionOnlyFastPath=*/true);
    slot.row->Overrides().Set(Style::PaddingLeft, StyleLength::Px(8.0f + static_cast<float>(indentPx)));

    // Per-row customization hook for callers (e.g., to add state classes and/or
    // inject icon/dot elements into the row). Rows are virtualized and reused.
    if (m_OnRowBound && slot.row)
    {
        m_OnRowBound(fi.id, slot.row);
    }

    if (slot.title)
    {
        if (slot.title->HasClass("tree-title-no-icon"))
            slot.title->Overrides().Reset(Style::PaddingLeft);
        else
            slot.title->Overrides().Set(Style::PaddingLeft, StyleLength::Px(m_IconSizePx + 8.0f));
    }
    if (slot.toggle)
        ApplyTreeFoldoutToggleLayout(slot.toggle, m_IconSizePx);

    const char* text = m_Provider->GetLabel(fi.id);
    slot.title->SetText(text ? std::string(text) : std::string(""));

    const bool expandable = m_Provider->IsExpandable(fi.id);
    const bool expanded = expandable && IsExpanded(fi.id);
    if (expanded)
        slot.toggle->AddClass("expanded");
    else
        slot.toggle->RemoveClass("expanded");

    if (!expandable)
    {
        slot.toggle->AddClass("no-children");
        slot.toggle->SetText(std::string(""));
    }
    else
    {
        slot.toggle->RemoveClass("no-children");
        // Foldout uses background-image icons (see editor theme CSS).
        // Keep label text empty so it doesn't fight icon layout.
        slot.toggle->SetText(std::string(""));
    }

    // Selection class
    if (IsRowVisuallySelected(fi.id))
        slot.row->AddClass("selected");
    else
        slot.row->RemoveClass("selected");

    slot.row->RemoveClass("drop-onto-valid");
    slot.row->RemoveClass("drop-onto-invalid");
    if (m_DropPreview.Visible &&
        m_DropPreview.Hit.Location == UI::Interaction::DropLocation::OnItem &&
        fi.id != 0 &&
        fi.id == static_cast<TreeId>(m_DropPreview.Hit.TargetId))
    {
        slot.row->AddClass(m_DropPreview.Allowed ? "drop-onto-valid" : "drop-onto-invalid");
    }

    // Update lock icon active state
    if (slot.lock && m_OnLockStateCheck)
    {
        if (m_OnLockStateCheck(fi.id))
            slot.lock->AddClass("active");
        else
            slot.lock->RemoveClass("active");
    }

    // Re-evaluate the subtree's CSS when the binding changed OR when this
    // bind is part of a data refresh (same id/index can carry changed
    // content) — descendant labels/icons may depend on the row's classes via
    // CSS selectors. No blanket LayoutDirty: the mutations above (SetText,
    // class flips, override writes) mark their own precise dirt including
    // LayoutDirty where measurement or position changed, keeping pure
    // recycling out of the whole-subtree Yoga path. The returned flag lets the
    // Host report a Rebind impact for exactly the runs that touched the tree.
    const bool markDirty = (bindingChanged || m_BindingFromDataRefresh);
    if (markDirty)
        slot.row->MarkDirtySubtree(StyleDirty | VisualDirty);
    return markDirty;
}

bool TreeView::IsRowVisuallySelected(TreeId id) const
{
    if (id == 0)
        return false;
    if (m_DragHighlightActive)
        return id == m_DragHighlightId;
    return m_Selection && m_Selection->IsSelected(id);
}

void TreeView::ClearDragHighlight()
{
    if (!m_DragHighlightActive)
        return;
    m_DragHighlightActive = false;
    m_DragHighlightId = 0;
    UpdateSelectionClasses();
}

void TreeView::UpdateSelectionClasses()
{
    // m_DragHighlightActive is only ever set when m_Selection exists, so no live tree needs the
    // class loop when there's no selection model.
    if (!m_Selection)
        return;
    for (RowSlot& slot : m_RowPool)
    {
        if (!slot.row)
            continue;
        if (IsRowVisuallySelected(slot.boundId))
            slot.row->AddClass("selected");
        else
            slot.row->RemoveClass("selected");
    }
}

void TreeView::SyncSelectionVisuals()
{
    UpdateSelectionClasses();
    if (m_OnSelectionChanged && m_Selection)
    {
        m_OnSelectionChanged(static_cast<TreeId>(m_Selection->GetAnchor()));
    }
}

TreeView::DebugRowLayoutInfo TreeView::GetDebugRowLayoutInfo() const
{
    DebugRowLayoutInfo out{};
    // Collect bound, non-invisible rows and sort by boundIndex (visual order).
    struct RowRec
    {
        int BoundIndex = -1;
        float Y = 0.0f;
        float H = 0.0f;
    };
    std::vector<RowRec> rows;
    rows.reserve(m_RowPool.size());
    for (const RowSlot& slot : m_RowPool)
    {
        UIElement* row = slot.row;
        if (!row)
            continue;
        if (slot.boundIndex < 0 || slot.boundId == 0)
            continue;
        // Skip paint-only hidden pooled rows.
        // We detect them via resolved style rather than RTTI to keep this debug helper simple.
        {
            const auto& vis = row->GetResolvedStyle().Visual;
            if (!vis.Visible || vis.Opacity <= 0.001f)
                continue;
        }
        const float h = row->GetLayoutHeight();
        if (h <= 0.0f)
            continue;
        rows.push_back(RowRec{slot.boundIndex, row->GetLayoutY(), h});
    }
    if (rows.empty())
        return out;

    std::sort(rows.begin(), rows.end(), [](const RowRec& a, const RowRec& b) { return a.BoundIndex < b.BoundIndex; });
    out.boundVisibleRows = (int)rows.size();
    out.minDeltaY = 1e9f;
    out.minRowH = 1e9f;
    out.maxRowH = 0.0f;

    constexpr float kOverlapEps = 0.5f;
    for (size_t i = 0; i < rows.size(); ++i)
    {
        out.minRowH = std::min(out.minRowH, rows[i].H);
        out.maxRowH = std::max(out.maxRowH, rows[i].H);
        if (i == 0)
            continue;
        const float dy = rows[i].Y - rows[i - 1].Y;
        out.minDeltaY = std::min(out.minDeltaY, dy);
        if (rows[i].Y < (rows[i - 1].Y + rows[i - 1].H - kOverlapEps))
            out.overlaps += 1;
    }
    if (out.minDeltaY > 1e8f)
        out.minDeltaY = 0.0f;
    if (out.minRowH > 1e8f)
        out.minRowH = 0.0f;
    return out;
}

void TreeView::CollectBoundIds(std::vector<TreeId>& out) const
{
    out.clear();
    for (const RowSlot& slot : m_RowPool)
    {
        if (slot.row && slot.boundId != 0)
            out.push_back(slot.boundId);
    }
}

UIElement* TreeView::FindBoundRow(TreeId id) const
{
    if (id == 0)
        return nullptr;
    for (const RowSlot& slot : m_RowPool)
    {
        if (slot.row && slot.boundId == id)
            return slot.row;
    }
    return nullptr;
}

TreeId TreeView::BoundIdOf(const UIElement* row) const
{
    if (!row)
        return 0;
    for (const RowSlot& slot : m_RowPool)
    {
        if (slot.row == row)
            return slot.boundId;
    }
    return 0;
}

std::vector<TreeView::DebugBoundRowInfo> TreeView::DebugGetBoundRows(int maxRows) const
{
    std::vector<DebugBoundRowInfo> out;
    if (maxRows <= 0)
        return out;
    out.reserve(std::min<int>(maxRows, (int)m_RowPool.size()));

    for (int si = 0; si < (int)m_RowPool.size(); ++si)
    {
        const RowSlot& slot = m_RowPool[(size_t)si];
        if (!slot.row)
            continue;
        if (slot.boundIndex < 0 || slot.boundId == 0)
            continue;
        // Skip paint-only hidden pooled rows.
        {
            const auto& vis = slot.row->GetResolvedStyle().Visual;
            if (!vis.Visible || vis.Opacity <= 0.001f)
                continue;
        }

        DebugBoundRowInfo rec{};
        rec.slotIndex = si;
        rec.boundIndex = slot.boundIndex;
        rec.boundId = slot.boundId;
        rec.depth = slot.boundDepth;
        rec.attached = slot.attached;
        if (m_Provider)
        {
            const char* lbl = m_Provider->GetLabel(slot.boundId);
            if (lbl)
                rec.label = lbl;
        }
        out.push_back(std::move(rec));
    }

    std::sort(out.begin(), out.end(), [](const DebugBoundRowInfo& a, const DebugBoundRowInfo& b)
              { return a.boundIndex < b.boundIndex; });
    if ((int)out.size() > maxRows)
        out.resize((size_t)maxRows);
    return out;
}

void TreeView::UpdateVirtualization(VirtualizationCoordinator::Reason reason)
{
    if (!m_Content)
        return;

    // Flat-list rebuild is view policy; RebuildFlatList bumps m_FlatGeneration,
    // which the guard carries as StructureGeneration, so the core breaks its ring
    // shortcut and fully rebinds against the new mapping — no manual reset needed.
    const bool flatDirtyBefore = m_FlatDirty;
    m_BindingFromDataRefresh = flatDirtyBefore;
    if (m_FlatDirty)
        RebuildFlatList();

    if (!m_Scroll)
        return;

    const float viewportH = m_Scroll->GetViewportHeight();
    if (viewportH <= 0.0f || m_RowHeight <= 0.0f)
        return;

    // Update content height so ScrollView knows the scroll range.
    // Also ensure the content element covers the viewport so RMB on "empty space" works
    // even when there are few/no rows (without forcing scrollbars).
    const float totalH = std::max(0.0f, (float)m_Flat.size() * m_RowHeight);
    const int forcedHPx = (int)std::lround(std::max(totalH, (float)(int)viewportH));
    UI::Layout::SetForcedHeight(*m_Content, std::max(0, forcedHPx));

    const float viewportW = m_Scroll->GetViewportWidth();
    const int viewportWPx = (int)std::lround(viewportW);

    // Explicitly tell the ScrollView the content size so it can calculate correct scroll range.
    //
    // IMPORTANT:
    // Do NOT feed ScrollView::GetContentWidth() back into SetContentSize() here.
    // GetContentWidth() can reflect stale historical maxima and never shrink after docking/layout
    // changes, which causes phantom horizontal scrollbars. Prefer viewport width and let ScrollView
    // expand content width via its own measurement/theme rules when needed.
    float contentW = (viewportW > 0.0f) ? viewportW : 0.0f;
    if (m_HorizontalScrollEnabled)
    {
        // A pass can land with no row bound and measurable - the settle after a
        // splitter drag re-binds the pool - and measuring nothing there is not
        // the same as the content being narrow. Reading it that way collapses the
        // extent and the bar vanishes on mouse-up with nothing to bring it back.
        // Keep the last real measurement for those passes; any pass that can see
        // rows replaces it, so a genuine shrink still shrinks.
        const float natural = MeasureNaturalRowWidth();
        if (natural > 0.0f)
            m_LastNaturalWidth = natural;
        contentW = std::max(contentW, m_LastNaturalWidth);
    }
    else
    {
        m_LastNaturalWidth = 0.0f;
    }
    m_Scroll->SetContentSize(contentW, totalH);

    const int contentWPx = (int)std::ceil(std::max(0.0f, m_Scroll->GetContentWidth()));

    // Fixed-height window math (view policy). First visible row via the shared
    // uniform helper. Note (preserved): unlike GridView there is NO first-row
    // clamp to totalRows - desired here — near the tail the core simply unbinds
    // the rows whose item index exceeds the flat size.
    const float scrollY = m_Scroll->GetScrollY();
    const int first = UI::VirtualWindowCore::FirstFromUniform(scrollY, m_RowHeight, (int)m_Flat.size());

    const int visible = (int)std::ceil(viewportH / m_RowHeight);
    const int desired = std::max(0, visible + m_Overscan);

    TreeChangeSet changes{};
    if (m_PendingChangeSetValid)
    {
        changes = std::move(m_PendingChangeSet);
        m_PendingChangeSetValid = false;
    }
    else if (m_Provider)
    {
        m_Provider->ConsumeChanges(m_LastProviderChangeVersion, changes);
    }
    if (changes.Version != m_LastProviderChangeVersion)
        m_LastProviderChangeVersion = changes.Version;
    // C-7: a structural provider mutation (MarkStructureChanged) means the flat
    // row set is stale (items added/removed/reordered, expandability changed).
    // Setting m_FlatDirty rebuilds it on the next pass; the generation bump below
    // (gated on m_FlatDirty) breaks the guard so this pass can't ring-scroll a
    // window whose flat set is about to change.
    if (changes.StructureVersion != m_LastProviderStructureVersion)
    {
        m_LastProviderStructureVersion = changes.StructureVersion;
        m_FlatDirty = true;
    }
    const bool hasChanges = (changes.Kind != ChangeSetKind::None);

    // Normalize the typed changeset for the core, which never sees id types:
    // All -> full rebind; Subset -> the flat pool slots currently bound to a
    // changed id (the old Subset fast-path scan, reused).
    m_ChangedSlotScratch.clear();
    UI::VirtualWindowCore::ChangeSetView changesView{};
    if (changes.Kind == ChangeSetKind::All)
    {
        changesView.ChangeKind = UI::VirtualWindowCore::ChangeSetView::Kind::All;
    }
    else if (changes.Kind == ChangeSetKind::Subset && !changes.Ids.empty())
    {
        for (int slot = 0; slot < (int)m_RowPool.size(); ++slot)
        {
            const TreeId bound = m_RowPool[(size_t)slot].boundId;
            if (bound == 0)
                continue;
            if (std::find(changes.Ids.begin(), changes.Ids.end(), bound) != changes.Ids.end())
                m_ChangedSlotScratch.push_back(slot);
        }
        changesView.ChangeKind = UI::VirtualWindowCore::ChangeSetView::Kind::Subset;
        changesView.AffectedSlots = m_ChangedSlotScratch;
    }

    // A pending m_FlatDirty that was set WITHOUT a rebuild this pass (the C-7 hook
    // above) must still break the guard so the core doesn't ring-scroll a window
    // whose flat set is about to change — RebuildFlatList already moved the
    // generation in the rebuilt case, this covers the "set but not yet rebuilt"
    // one. Mirrors the old `!m_FlatDirty` guard on the early-out and ring paths.
    if (m_FlatDirty)
        ++m_FlatGeneration;

    UI::VirtualWindowCore::Guard guard{};
    guard.First = first;
    guard.Desired = desired;
    guard.ItemCount = (int)m_Flat.size();
    guard.ContentHPx = forcedHPx;
    guard.ViewportWPx = viewportWPx;
    guard.ContentWPx = contentWPx;
    guard.StructureGeneration = m_FlatGeneration;

    HostAdapter host(*this);
    const UI::VirtualWindowCore::Impact coreImpact =
        m_WindowCore.Update(guard, reason, /*lanes=*/1, changesView, host,
                            /*allowShrink=*/!UIElement::IsInEventDispatch());

    // Aggregate the impact once (TreeView's per-run model). The core reports
    // Rebind for any run that rebound rows and Topology on pool grow/shrink; the
    // view ADDS LayoutRects for data-affecting runs and Topology+LayoutRects for a
    // flat rebuild. A pure ring-scroll stays Rebind-only — it is a position-only
    // move serviced by the override-rect patch and must NOT arm the late
    // whole-tree relayout (whose gate keys on Topology|LayoutRects).
    uint32_t impact = static_cast<uint32_t>(coreImpact);
    const bool dataAffectingThisRun =
        (reason == VirtualizationCoordinator::Reason::DataChanged) || flatDirtyBefore || hasChanges;
    if (dataAffectingThisRun)
        impact |= UIManager::VirtualizationRebindChanged | UIManager::VirtualizationLayoutRectsChanged;
    if (flatDirtyBefore)
        impact |= UIManager::VirtualizationTopologyChanged | UIManager::VirtualizationLayoutRectsChanged;
    if (impact != 0u)
    {
        if (UIManager* ui = GetOwnerManager())
            ui->NotifyVirtualizationImpact(impact);
    }
}

void TreeView::OnPostLayout()
{
    // Coalesce viewport-changed work: if the viewport hasn't changed and the data isn't dirty,
    // avoid enqueuing a virtualization item every layout pass. Without this guard the heavy
    // path runs every frame on idle because OnPostLayout unconditionally re-enqueues work.
    const float viewportW = m_Scroll ? m_Scroll->GetViewportWidth() : 0.0f;
    const float viewportH = m_Scroll ? m_Scroll->GetViewportHeight() : 0.0f;
    const int viewportWPx = (int)std::lround(viewportW);
    const int viewportHPx = (int)std::lround(viewportH);
    const bool viewportChanged = (viewportWPx != m_LastViewportWPx) || (viewportHPx != m_LastViewportHPx);
    if (viewportChanged)
    {
        m_LastViewportWPx = viewportWPx;
        m_LastViewportHPx = viewportHPx;
    }

    // A pending Subset changeset can be stranded when the enqueued work
    // early-outs on a zero-height viewport (hidden tab); re-enqueue on the
    // next post-layout so it is consumed once the viewport is real again.
    if (!viewportChanged && !m_FlatDirty && !m_PendingChangeSetValid)
        return;

    if (UIManager* ui = GetOwnerManager())
    {
        ui->EnqueueVirtualizationWork(this,
                                      [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason /*reason*/)
                                      {
                                          static_cast<TreeView*>(ctx)->UpdateVirtualization(VirtualizationCoordinator::Reason::ViewportChanged);
                                      },
                                      VirtualizationCoordinator::Reason::ViewportChanged);
    }
    else
    {
        UpdateVirtualization(VirtualizationCoordinator::Reason::ViewportChanged);
    }
}

void TreeView::RefreshFromProvider()
{
    if (UIElement::IsInEventDispatch())
    {
        // PostSafeAction alone runs at the next dispatcher drain (typically start of the *next*
        // UIManager::Update). The chevron handler runs during DispatchEvents — after that point this
        // frame only drains the virtualization queue once (post-events). If we schedule nothing
        // here, rows stay stale until another drain (often the next mouse move), which matches the
        // "wrong until hover" bug. Kick the same work the non-dispatch path uses so the post-
        // DispatchEvents virtualization drain rebinds in the same frame as the click.
        m_FlatDirty = true;
        if (UIManager* uiKick = GetOwnerManager())
        {
            uiKick->NotifyVirtualizationImpact(UIManager::VirtualizationRebindChanged |
                                               UIManager::VirtualizationLayoutRectsChanged);
            uiKick->EnqueueVirtualizationWork(
                this,
                [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason /*reason*/)
                {
                    static_cast<TreeView*>(ctx)->UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
                },
                VirtualizationCoordinator::Reason::DataChanged);
        }
        else
        {
            UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
        }
        MarkDirty(LayoutDirty | VisualDirty);
        RequestRelayout();

        this->PostSafeAction([this]()
                         { this->RefreshFromProvider(); });
        return;
    }

    if (m_Provider)
    {
        TreeChangeSet changes;
        m_Provider->ConsumeChanges(m_LastProviderChangeVersion, changes);
        if (changes.Version != m_LastProviderChangeVersion)
            m_LastProviderChangeVersion = changes.Version;
        // C-7: a structural provider mutation (MarkStructureChanged) means the flat
        // row set is stale even if the reported changeset is a row Subset — force a
        // flat rebuild so the Subset stash below reflects the new item set.
        if (changes.StructureVersion != m_LastProviderStructureVersion)
        {
            m_LastProviderStructureVersion = changes.StructureVersion;
            m_FlatDirty = true;
        }

        if (changes.Kind == ChangeSetKind::Subset)
        {
            m_PendingChangeSet = std::move(changes);
            m_PendingChangeSetValid = true;
            if (UIManager* ui = GetOwnerManager())
            {
                ui->EnqueueVirtualizationWork(this,
                                              [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason /*reason*/)
                                              {
                                                  static_cast<TreeView*>(ctx)->UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
                                              },
                                              VirtualizationCoordinator::Reason::DataChanged);
            }
            else
            {
                UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
            }
            // Match the full refresh path: expansion / virtualized height may change even when the
            // provider only reports a row Subset. Visual-only dirty is not enough for Yoga + scroll
            // metrics to converge the same frame (stale tree layout after collapse).
            MarkDirty(LayoutDirty | VisualDirty);
            RequestRelayout();
            return;
        }
    }

    m_FlatDirty = true;
    m_PendingChangeSetValid = false;
    // Mark virtualization impact immediately so cached mode recomputes on the
    // subsequent frame if this refresh arrives late in the current update.
    if (UIManager* ui = GetOwnerManager())
    {
        ui->NotifyVirtualizationImpact(UIManager::VirtualizationRebindChanged | UIManager::VirtualizationLayoutRectsChanged);
    }
    // Try to update virtualization immediately. This handles the common case where the
    // refresh is triggered between frames or early in the UI update.
    if (UIManager* ui = GetOwnerManager())
    {
        ui->EnqueueVirtualizationWork(this,
                                      [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason /*reason*/)
                                      {
                                          static_cast<TreeView*>(ctx)->UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
                                      },
                                      VirtualizationCoordinator::Reason::DataChanged);
    }
    else
    {
        UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
    }
    // Refresh updates bindings / virtualized layout overrides.
    //
    // IMPORTANT:
    // RefreshFromProvider is commonly called from editor change notifications (e.g. scene load),
    // which can happen during the UIManager post-layout phase. In that situation, merely marking
    // LayoutDirty/VisualDirty is not guaranteed to trigger an immediate Yoga re-solve in the
    // same frame, leaving stale cached geometry until the next user input (click/resize).
    //
    // RequestRelayout() ensures UIManager performs a relayout pass promptly so row visuals match
    // hit-testing without requiring a manual resize.
    MarkDirty(LayoutDirty | VisualDirty);
    RequestRelayout();

    // Coalesce a follow-up virtualization pass to a safe point after this change notification.
    // This helps avoid cases where the refresh occurs late in a frame (e.g. native menu actions),
    // after UIManager's layout-override patch step has already run.
    if (!m_RefreshPosted)
    {
        m_RefreshPosted = true;
        this->PostSafeAction([this]()
                         {
                             m_RefreshPosted = false;
                             m_FlatDirty = true;
                             if (UIManager* ui = GetOwnerManager())
                             {
                                 ui->EnqueueVirtualizationWork(this,
                                                               [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason /*reason*/)
                                                               {
                                                                   static_cast<TreeView*>(ctx)->UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
                                                               },
                                                               VirtualizationCoordinator::Reason::DataChanged);
                             }
                             else
                             {
                                 this->UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
                             }
                         });
    }

    // If RefreshFromProvider happens before the TreeView has a meaningful viewport size,
    // UpdateVirtualization() can early-out and leave stale row geometry until the user
    // scrolls or another input triggers a layout pass. Ensure we get a post-layout pass.
    if (m_Scroll)
    {
        const float vh = m_Scroll->GetViewportHeight();
        const float vw = m_Scroll->GetViewportWidth();
        if (vh <= 1.0f || vw <= 1.0f)
        {
            RequestRelayout();
        }
    }
}

void TreeView::OnEvent(UIEvent& e)
{
    if (e.Id == kEventKeyDown && m_Selection && m_Provider)
    {
        // Only react to keyboard input when the tree itself owns focus.
        // Otherwise arrow keys typed into a value box / text field elsewhere
        // would bubble up and collapse / expand tree rows behind the user.
        if (UIManager* ui = GetOwnerManager())
        {
            if (!IsFocusTargetForId(ui->GetFocusedElementId()))
                return;
        }

        const bool primaryMod = Input::IsPrimaryShortcutModifier(e.Mods);
        const bool shift = (e.Mods & Input::kModShift) != 0;

        // Ctrl/Cmd+A: select all visible rows.
        if (primaryMod && e.Key == Input::kKeyCode_A && !m_KeyboardSingleSelectOnly)
        {
            std::vector<UI::Interaction::ItemId> ids;
            ids.reserve(m_Flat.size());
            for (const FlatItem& fi : m_Flat)
            {
                if (fi.id != 0)
                    ids.push_back(fi.id);
            }
            if (!ids.empty())
            {
                const TreeId anchor = static_cast<TreeId>(ids.front());
                m_Selection->SetSelection(ids, anchor);
                UpdateSelectionClasses();
                if (m_OnSelectionChanged)
                    m_OnSelectionChanged(anchor);
            }
            e.Stop();
            return;
        }

        // Finder-style type-ahead (A–Z / 0–9): jump to the next visible row whose label starts with that character.
        if (!primaryMod && e.Mods == 0)
        {
            const int matchCh = TypeAheadKeyToMatchChar(e.Key);
            if (matchCh >= 0 && !m_Flat.empty())
            {
                const auto now = std::chrono::steady_clock::now();
                const bool keyChanged = (e.Key != m_TypeAheadRepeatKey);
                const bool timedOut =
                    (m_TypeAheadRepeatKey < 0) || (now - m_TypeAheadLastKeyTime > kTreeTypeAheadRepeatWindow);
                const bool sameKeyRepeat = !keyChanged && !timedOut;

                m_TypeAheadLastKeyTime = now;
                m_TypeAheadRepeatKey = e.Key;

                const char matchChar = static_cast<char>(matchCh);

                UI::Interaction::ItemId anchorId = m_Selection->GetAnchor();
                const int selectedIndex = FlatIndexOf(static_cast<TreeId>(anchorId));

                const int count = (int)m_Flat.size();
                int found = -1;
                if (!sameKeyRepeat)
                {
                    for (int i = 0; i < count; ++i)
                    {
                        const TreeId tid = m_Flat[(size_t)i].id;
                        const char* lab = m_Provider->GetLabel(tid);
                        if (LabelFirstCharMatches(lab, matchChar))
                        {
                            found = i;
                            break;
                        }
                    }
                }
                else
                {
                    int start = selectedIndex + 1;
                    if (start < 0)
                        start = 0;
                    for (int i = start; i < count; ++i)
                    {
                        const TreeId tid = m_Flat[(size_t)i].id;
                        const char* lab = m_Provider->GetLabel(tid);
                        if (LabelFirstCharMatches(lab, matchChar))
                        {
                            found = i;
                            break;
                        }
                    }
                    if (found < 0)
                    {
                        for (int i = 0; i < start && i < count; ++i)
                        {
                            const TreeId tid = m_Flat[(size_t)i].id;
                            const char* lab = m_Provider->GetLabel(tid);
                            if (LabelFirstCharMatches(lab, matchChar))
                            {
                                found = i;
                                break;
                            }
                        }
                    }
                }

                if (found >= 0)
                {
                    const TreeId targetId = m_Flat[(size_t)found].id;
                    if (targetId != 0)
                    {
                        m_Selection->SetSingle(targetId);
                        UpdateSelectionClasses();
                        if (m_OnSelectionChanged)
                            m_OnSelectionChanged(targetId);

                        if (m_Scroll)
                        {
                            const float rowH = m_RowHeight;
                            const float rowTop = (float)found * rowH;
                            const float rowBottom = rowTop + rowH;
                            const float viewportH = m_Scroll->GetViewportHeight();
                            const float scrollY = m_Scroll->GetScrollY();

                            float newScrollY = scrollY;
                            if (rowTop < scrollY)
                                newScrollY = rowTop;
                            else if (rowBottom > scrollY + viewportH)
                                newScrollY = std::max(0.0f, rowBottom - viewportH);

                            if (std::fabs(newScrollY - scrollY) > 0.5f)
                                m_Scroll->SetScrollY(newScrollY);
                        }

                        e.Stop();
                        return;
                    }
                }
            }
        }

        // Left/Right: collapse/expand, or jump to parent/first child when
        // already in the appropriate state. Matches IDE tree conventions.
        if (!primaryMod && (!shift || m_KeyboardSingleSelectOnly) &&
            (e.Key == Input::kKeyCode_Left || e.Key == Input::kKeyCode_Right))
        {
            if (m_Flat.empty() || !m_Provider)
            {
                e.Stop();
                return;
            }

            UI::Interaction::ItemId anchorId = m_Selection->GetAnchor();
            const int anchorIndex = FlatIndexOf(static_cast<TreeId>(anchorId));
            if (anchorIndex < 0)
            {
                e.Stop();
                return;
            }

            const TreeId currentId = m_Flat[(size_t)anchorIndex].id;
            const int currentDepth = m_Flat[(size_t)anchorIndex].depth;
            const bool expandable = m_Provider->IsExpandable(currentId);
            const bool expanded = expandable && IsExpanded(currentId);

            int targetIndex = -1;
            if (e.Key == Input::kKeyCode_Right)
            {
                if (expandable && !expanded)
                {
                    SetExpanded(currentId, true);
                    RefreshFromProvider();
                    e.Stop();
                    return;
                }
                if (expandable && expanded && anchorIndex + 1 < (int)m_Flat.size() &&
                    m_Flat[(size_t)(anchorIndex + 1)].depth > currentDepth)
                {
                    targetIndex = anchorIndex + 1;
                }
            }
            else // Left
            {
                if (expandable && expanded)
                {
                    SetExpanded(currentId, false);
                    RefreshFromProvider();
                    e.Stop();
                    return;
                }
                // Jump to parent: nearest preceding row with smaller depth.
                for (int i = anchorIndex - 1; i >= 0; --i)
                {
                    if (m_Flat[(size_t)i].depth < currentDepth)
                    {
                        targetIndex = i;
                        break;
                    }
                }
            }

            if (targetIndex >= 0)
            {
                const TreeId targetId = m_Flat[(size_t)targetIndex].id;
                if (targetId != 0)
                {
                    m_Selection->SetSingle(targetId);
                    UpdateSelectionClasses();
                    if (m_OnSelectionChanged)
                        m_OnSelectionChanged(targetId);

                    if (m_Scroll)
                    {
                        const float rowH = m_RowHeight;
                        const float rowTop = targetIndex * rowH;
                        const float rowBottom = rowTop + rowH;
                        const float viewportH = m_Scroll->GetViewportHeight();
                        const float scrollY = m_Scroll->GetScrollY();

                        float newScrollY = scrollY;
                        if (rowTop < scrollY)
                            newScrollY = rowTop;
                        else if (rowBottom > scrollY + viewportH)
                            newScrollY = std::max(0.0f, rowBottom - viewportH);

                        if (std::fabs(newScrollY - scrollY) > 0.5f)
                            m_Scroll->SetScrollY(newScrollY);
                    }
                }
            }

            e.Stop();
            return;
        }

        // Keyboard navigation through m_Flat. Standard list-control keys
        // (Win32/WPF/Qt/AppKit): PageUp/Down move the cursor by one viewport
        // of rows, Home/End go to the extremes, Shift extends from the anchor.
        if (!primaryMod &&
            (e.Key == Input::kKeyCode_Up ||
             e.Key == Input::kKeyCode_Down ||
             e.Key == Input::kKeyCode_PageUp ||
             e.Key == Input::kKeyCode_PageDown ||
             e.Key == Input::kKeyCode_Home ||
             e.Key == Input::kKeyCode_End))
        {
            if (m_Flat.empty())
            {
                e.Stop();
                return;
            }

            // Anchor index: fixed starting row for Shift-range extension.
            // Cursor index: moves with Up/Down. With Shift the range is
            // [anchor, cursor]; without Shift both collapse to the target.
            UI::Interaction::ItemId anchorId = m_Selection->GetAnchor();
            int anchorIndex = FlatIndexOf(static_cast<TreeId>(anchorId));
            if (anchorIndex < 0)
                anchorIndex = 0;

            int cursorIndex = FlatIndexOf(m_KeyboardCursorId);
            if (cursorIndex < 0)
                cursorIndex = anchorIndex;

            // One viewport of rows minus one row of overlap for context (the
            // Win32/Explorer paging convention).
            const int pageRows = (m_Scroll && m_RowHeight > 0.0f)
                ? std::max(1, (int)(m_Scroll->GetViewportHeight() / m_RowHeight) - 1)
                : 1;

            int targetIndex = cursorIndex;
            switch (e.Key)
            {
            case Input::kKeyCode_Up:      targetIndex = cursorIndex - 1; break;
            case Input::kKeyCode_Down:    targetIndex = cursorIndex + 1; break;
            case Input::kKeyCode_PageUp:  targetIndex = cursorIndex - pageRows; break;
            case Input::kKeyCode_PageDown:targetIndex = cursorIndex + pageRows; break;
            case Input::kKeyCode_Home:    targetIndex = 0; break;
            case Input::kKeyCode_End:     targetIndex = (int)m_Flat.size() - 1; break;
            default: break;
            }

            if (targetIndex < 0)
                targetIndex = 0;
            if (targetIndex >= (int)m_Flat.size())
                targetIndex = (int)m_Flat.size() - 1;

            const TreeId targetId = m_Flat[(size_t)targetIndex].id;
            if (targetId == 0)
            {
                e.Stop();
                return;
            }

            if (shift && !m_KeyboardSingleSelectOnly)
            {
                // Range selection between the fixed anchor and the new cursor.
                const int startIndex = anchorIndex;
                const int lo = std::min(startIndex, targetIndex);
                const int hi = std::max(startIndex, targetIndex);

                std::vector<UI::Interaction::ItemId> ids;
                ids.reserve((size_t)(hi - lo + 1));
                for (int i = lo; i <= hi; ++i)
                {
                    const TreeId id = m_Flat[(size_t)i].id;
                    if (id != 0)
                        ids.push_back(id);
                }

                const TreeId anchorTreeId = m_Flat[(size_t)startIndex].id;
                m_Selection->SetSelection(ids, anchorTreeId);
                m_KeyboardCursorId = targetId;
            }
            else
            {
                m_Selection->SetSingle(targetId);
                m_KeyboardCursorId = targetId;
            }

            UpdateSelectionClasses();
            if (m_OnSelectionChanged)
                m_OnSelectionChanged(targetId);

            // Scroll into view if needed.
            if (m_Scroll)
            {
                const float rowH = m_RowHeight;
                const float rowTop = targetIndex * rowH;
                const float rowBottom = rowTop + rowH;
                const float viewportH = m_Scroll->GetViewportHeight();
                const float scrollY = m_Scroll->GetScrollY();

                float newScrollY = scrollY;
                if (rowTop < scrollY)
                    newScrollY = rowTop;
                else if (rowBottom > scrollY + viewportH)
                    newScrollY = std::max(0.0f, rowBottom - viewportH);

                if (std::fabs(newScrollY - scrollY) > 0.5f)
                    m_Scroll->SetScrollY(newScrollY);
            }

            e.Stop();
            return;
        }
    }

    // Drag gesture handling (row drags). This runs only when we captured the mouse on mousedown.
    if (m_DragCandidate && !m_IconDragging && !m_LockDragging)
    {
        if (e.Id == kEventMouseMove)
        {
            constexpr float kDragThresholdPx = 6.0f;
            const float dx = e.X - m_DragStartX;
            const float dy = e.Y - m_DragStartY;
            if ((dx * dx + dy * dy) >= (kDragThresholdPx * kDragThresholdPx))
            {
                bool beganDrag = false;
                if (UIManager* ui = GetOwnerManager())
                {
                    if (auto* dd = ui->GetDragDropManager())
                    {
                        const TreeId draggedId = m_DragCandidateId;
                        UI::Interaction::DragPayload payload = m_DragPayloadBuilder ? m_DragPayloadBuilder(draggedId) : UI::Interaction::DragPayload{};
                        if (payload.IsValid())
                        {
                            UI::Interaction::DragSessionContext ctx{};
                            ctx.SourceWidgetId = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this));

                            // The pressed row was already given a transient highlight on mouse-down
                            // (select-on-up trees only — m_DragHighlightActive is never set elsewhere).
                            // Persist it through the drag and clear it when the drag ends: a reorder
                            // commits the real selection; an EntityField drop reverts to the prior one.
                            if (m_DragHighlightActive)
                            {
                                ctx.OnDragEnded = [this, life = std::weak_ptr<int>(m_LifeToken)]() {
                                    if (life.expired())
                                        return; // TreeView (and its window/UIManager) destroyed mid-drag
                                    ClearDragHighlight();
                                };
                            }
                            dd->BeginDrag(std::move(payload), ctx);
                            beganDrag = true;
                        }
                    }
                }
                // Crossed the drag threshold but no drag actually began (no valid payload) — revert the
                // responsive press-highlight here, since OnDragEnded won't fire to clear it.
                if (!beganDrag)
                    ClearDragHighlight();
                m_DragCandidate = false;
                m_DragCandidateId = 0;
                m_PendingCollapseToSingleId = 0;
                m_PendingSelectId = 0; // it's a drag, not a click — don't change selection
            }
            e.Stop();
            return;
        }

        if (e.Id == kEventMouseUp)
        {
            // Deferred select-on-up: a plain press recorded a pending selection and we reached
            // mouse-up without a drag starting (drag-start clears m_PendingSelectId), so this was a
            // click — commit the selection now.
            if (m_Selection && m_SelectOnMouseUp && m_PendingSelectId != 0 && e.Button == 0)
            {
                const TreeId pendingId = m_PendingSelectId;
                m_PendingSelectId = 0;
                m_DragHighlightActive = false; // hand the transient press-highlight off to the model
                m_DragHighlightId = 0;
                m_KeyboardCursorId = pendingId;
                m_Selection->SetSingle(pendingId);
                UpdateSelectionClasses();
                if (m_OnSelectionChanged)
                    m_OnSelectionChanged(pendingId);
            }

            // If this was a click on an already-selected item within a multi-selection (and not a drag),
            // collapse to that single item on mouse-up.
            if (m_Selection && m_PendingCollapseToSingleId != 0 && e.Button == 0)
            {
                const bool shift = (e.Mods & Input::kModShift) != 0;
                const bool primaryMod = Input::IsPrimaryShortcutModifier(e.Mods);
                if (!shift && !primaryMod)
                {
                    m_Selection->SetSingle(m_PendingCollapseToSingleId);
                    UpdateSelectionClasses();
                    if (m_OnSelectionChanged)
                        m_OnSelectionChanged(m_PendingCollapseToSingleId);
                }
            }

            // Double-click detection for captured mouse-up: the row's own kEventMouseUp handler
            // is bypassed while capture is active, so we run the same check here.
            if (e.Button == 0 && m_DragCandidateId != 0)
            {
                using clock = std::chrono::steady_clock;
                const auto now = clock::now();
                const bool isDouble =
                    (m_LastClickedId == m_DragCandidateId) &&
                    ((now - m_LastClickTime) < Platform::GetDoubleClickInterval());
                m_LastClickedId = m_DragCandidateId;
                m_LastClickTime = now;
                if (isDouble)
                {
                    if (m_Provider && m_Provider->IsExpandable(m_DragCandidateId))
                    {
                        bool willExpand = !IsExpanded(m_DragCandidateId);
                        SetExpanded(m_DragCandidateId, willExpand);
                        m_FlatDirty = true;
                        RefreshFromProvider();
                    }
                    if (m_OnItemActivated)
                        m_OnItemActivated(m_DragCandidateId);
                }
            }

            m_DragCandidate = false;
            m_DragCandidateId = 0;
            m_PendingCollapseToSingleId = 0;
            m_PendingSelectId = 0;
            // Safety: a non-drag release that didn't commit must not leave a press-highlight stuck.
            ClearDragHighlight();
            e.Stop();
            return;
        }
    }

    // Handle icon dragging globally
    if (m_IconDragging)
    {
        if (e.Id == kEventMouseMove)
        {
            if (!m_OnIconStateSet)
                return;
            
            // Check each visible row to see if the mouse is over its icon region
            for (size_t i = 0; i < m_RowPool.size(); ++i)
            {
                RowSlot& slot = m_RowPool[i];
                if (!slot.row || !slot.title || slot.boundId == 0)
                    continue;
                
                // Check if mouse is within this row's bounds
                const float rowY = slot.row->GetLayoutY();
                const float rowH = slot.row->GetLayoutHeight();
                if (e.Y < rowY || e.Y >= rowY + rowH)
                    continue;
                
                UIElement* explicitIconTarget = nullptr;
                for (const auto& child : slot.row->GetChildren())
                {
                    if (child && child->HasClass("tree-icon-hit-target"))
                    {
                        explicitIconTarget = child.get();
                        break;
                    }
                }

                if (explicitIconTarget)
                {
                    float iconX = 0.0f;
                    float iconY = 0.0f;
                    float iconW = 0.0f;
                    float iconH = 0.0f;
                    explicitIconTarget->GetHitTestBounds(iconX, iconY, iconW, iconH);
                    if (e.X < iconX || e.X >= iconX + iconW ||
                        e.Y < iconY || e.Y >= iconY + iconH)
                        continue;
                }
                else
                {
                    // Legacy tree icons are backgrounds in the title's leading column.
                    constexpr float kIconClickWidth = 20.0f;
                    const float titleX = slot.title->GetLayoutX();
                    if (e.X < titleX || e.X >= titleX + kIconClickWidth)
                        continue;
                }
                
                // Found a row with mouse over its icon - set to target state if not already applied
                const TreeId id = slot.boundId;
                if (m_IconAppliedInCurrentDrag.find(id) == m_IconAppliedInCurrentDrag.end())
                {
                    m_IconAppliedInCurrentDrag.insert(id);
                    if (explicitIconTarget)
                        explicitIconTarget->AddClass(kIconHoverPreviewSuppressedClass);
                    m_OnIconStateSet(id, m_IconDragTargetState, slot.row);
                }
                
                e.Stop();
                return;
            }
            e.Stop();
            return;
        }
        
        if (e.Id == kEventMouseUp)
        {
            m_IconDragging = false;
            m_IconAppliedInCurrentDrag.clear();
            e.Stop();
            return;
        }
    }

    // Handle lock icon dragging globally
    if (m_LockDragging)
    {
        if (e.Id == kEventMouseMove)
        {
            if (!m_OnLockStateSet)
                return;

            for (size_t i = 0; i < m_RowPool.size(); ++i)
            {
                RowSlot& slot = m_RowPool[i];
                if (!slot.row || !slot.lock || slot.boundId == 0)
                    continue;

                const float rowY = slot.row->GetLayoutY();
                const float rowH = slot.row->GetLayoutHeight();
                if (e.Y < rowY || e.Y >= rowY + rowH)
                    continue;

                const float lockX = slot.lock->GetLayoutX();
                const float lockW = slot.lock->GetLayoutWidth();
                if (e.X < lockX || e.X >= lockX + lockW)
                    continue;

                const TreeId id = slot.boundId;
                if (m_LockAppliedInCurrentDrag.find(id) == m_LockAppliedInCurrentDrag.end())
                {
                    m_LockAppliedInCurrentDrag.insert(id);
                    m_OnLockStateSet(id, m_LockDragTargetState, slot.row);
                    if (slot.lock)
                    {
                        if (m_LockDragTargetState)
                            slot.lock->AddClass("active");
                        else
                            slot.lock->RemoveClass("active");
                    }
                }

                e.Stop();
                return;
            }
            e.Stop();
            return;
        }

        if (e.Id == kEventMouseUp)
        {
            m_LockDragging = false;
            m_LockAppliedInCurrentDrag.clear();
            e.Stop();
            return;
        }
    }

    UIElement::OnEvent(e);
}

bool TreeView::AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const
{
    return m_DropAccepts ? m_DropAccepts(typeId) : false;
}

bool TreeView::HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const
{
    // Absolute coordinates; compare against bound row rects (pool is small: visible rows only).
    UIElement* bestRow = nullptr;
    TreeId bestId = 0;
    int bestDepth = 0;
    float bestRowY = 0.0f;
    float bestRowH = 0.0f;

    // Track the topmost / bottommost bound rows (by screen Y) so a drag above the first or
    // below the last row — still inside the panel — snaps to a real insertion line instead of
    // falling through to an "empty space" drop that shows no indicator.
    TreeId firstId = 0, lastId = 0;
    int firstDepth = 0, lastDepth = 0;
    float firstY = 0.0f, lastY = 0.0f, lastH = 0.0f;
    bool haveFirst = false, haveLast = false;

    for (const auto& slot : m_RowPool)
    {
        if (!slot.row || slot.boundId == 0 || slot.boundIndex < 0)
            continue;
        const float rx = slot.row->GetLayoutX();
        const float ry = slot.row->GetLayoutY();
        const float rw = slot.row->GetLayoutWidth();
        const float rh = slot.row->GetLayoutHeight();

        if (!haveFirst || ry < firstY)
        {
            haveFirst = true;
            firstY = ry;
            firstId = slot.boundId;
            firstDepth = slot.boundDepth;
        }
        if (!haveLast || ry > lastY)
        {
            haveLast = true;
            lastY = ry;
            lastH = rh;
            lastId = slot.boundId;
            lastDepth = slot.boundDepth;
        }

        if (!bestRow && x >= rx && x < rx + rw && y >= ry && y < ry + rh)
        {
            bestRow = slot.row;
            bestId = slot.boundId;
            bestDepth = slot.boundDepth;
            bestRowY = ry;
            bestRowH = rh;
        }
    }

    // If we're inside the TreeView rect but not over a row, be forgiving at the ends: above the
    // first row inserts before it, below the last row inserts after it. Otherwise fall back to an
    // "empty space" (root) drop.
    const float tx = GetLayoutX();
    const float ty = GetLayoutY();
    const float tw = GetLayoutWidth();
    const float th = GetLayoutHeight();
    if (!bestRow)
    {
        if (x >= tx && x < tx + tw && y >= ty && y < ty + th)
        {
            if (haveFirst && y < firstY)
            {
                out.TargetId = firstId;
                out.Location = UI::Interaction::DropLocation::BeforeItem;
                out.IndentDepth = firstDepth;
                return true;
            }
            if (haveLast && y >= lastY + lastH)
            {
                out.TargetId = lastId;
                out.Location = UI::Interaction::DropLocation::AfterItem;
                out.IndentDepth = lastDepth;
                return true;
            }
            out.TargetId = 0;
            out.Location = UI::Interaction::DropLocation::OnEmptySpace;
            out.IndentDepth = 0;
            return true;
        }
        return false;
    }

    const float band = bestRowH * 0.25f;
    UI::Interaction::DropLocation loc = UI::Interaction::DropLocation::OnItem;
    if (y < bestRowY + band)
        loc = UI::Interaction::DropLocation::BeforeItem;
    else if (y >= bestRowY + bestRowH - band)
        loc = UI::Interaction::DropLocation::AfterItem;

    out.TargetId = bestId;
    out.Location = loc;
    out.IndentDepth = bestDepth;
    return true;
}

UI::Interaction::DropFeedback TreeView::CanDrop(const UI::Interaction::DropRequest& request) const
{
    return m_OnCanDrop ? m_OnCanDrop(request) : UI::Interaction::DropFeedback{false, "No drop handler"};
}

void TreeView::PerformDrop(const UI::Interaction::DropRequest& request)
{
    if (m_OnPerformDrop)
        m_OnPerformDrop(request);

    // UX: if we dropped "onto" a node and that node now has children, expand it so the result
    // is immediately visible (especially when the node was previously non-expandable).
    if (request.hit.Location == UI::Interaction::DropLocation::OnItem && request.hit.TargetId != 0)
    {
        const TreeId id = static_cast<TreeId>(request.hit.TargetId);
        if (m_Provider && m_Provider->IsExpandable(id) && !IsExpanded(id))
        {
            SetExpanded(id, true);
            m_FlatDirty = true;
            RefreshFromProvider();
        }
    }
}

void TreeView::SetDropPreview(const UI::Interaction::DropPreviewState& state)
{
    m_DropPreview = state;
    UpdateDropPreviewClasses();

    // Auto-expand folders/nodes when hovered during a drag.
    // This makes "drag into folder" feel natural without requiring precision.
    //
    // Only auto-expand when:
    // - we have an active preview,
    // - the hover is "onto" a row,
    // - the row is expandable and currently collapsed,
    // - the drop is allowed (so we don't expand invalid targets).
    constexpr auto kAutoExpandDelay = std::chrono::milliseconds(450);
    if (state.Visible &&
        state.Allowed &&
        state.Hit.Location == UI::Interaction::DropLocation::OnItem &&
        state.Hit.TargetId != 0)
    {
        const TreeId hoverId = static_cast<TreeId>(state.Hit.TargetId);
        const bool canExpand = (m_Provider && m_Provider->IsExpandable(hoverId) && !IsExpanded(hoverId));
        if (canExpand)
        {
            const auto now = std::chrono::steady_clock::now();
            if (m_AutoExpandHoverId != hoverId)
            {
                m_AutoExpandHoverId = hoverId;
                m_AutoExpandHoverStart = now;
            }
            else if (now - m_AutoExpandHoverStart >= kAutoExpandDelay)
            {
                SetExpanded(hoverId, true);
                m_FlatDirty = true;
                RefreshFromProvider();
                m_AutoExpandHoverId = 0;
            }
        }
        else
        {
            m_AutoExpandHoverId = 0;
        }
    }
    else
    {
        m_AutoExpandHoverId = 0;
    }

    // Insertion indicator: draw a horizontal line for Before/After.
    const bool wantsLine =
        state.Visible &&
        (state.Hit.Location == UI::Interaction::DropLocation::BeforeItem ||
         state.Hit.Location == UI::Interaction::DropLocation::AfterItem);

    UIManager* ui = GetOwnerManager();
    UIElement* root = ui ? ui->GetRootElement() : nullptr;
    if (wantsLine && root)
    {
        if (!m_DropIndicator)
        {
            auto ind = std::make_unique<UIElement>();
            ind->SetId("tree-drop-indicator");
            ind->AddClass("tree-drop-indicator");
            ind->Overrides()
                .Set(Style::Position, PositionType::Absolute)
                .Set(Style::PointerEvents, false)
                .Set(Style::Display, DisplayMode::None);
            m_DropIndicator = ind.get();
            root->AddChild(std::move(ind));
        }

        // Find target row rect.
        float yLine = 0.0f;
        float x0 = GetLayoutX();
        float w = GetLayoutWidth();
        for (const auto& slot : m_RowPool)
        {
            if (!slot.row || slot.boundId != state.Hit.TargetId)
                continue;
            const float ry = slot.row->GetLayoutY();
            const float rh = slot.row->GetLayoutHeight();
            yLine = (state.Hit.Location == UI::Interaction::DropLocation::BeforeItem) ? ry : (ry + rh);
            break;
        }

        const uint32_t color = state.Allowed ? 0xE600C8FFu : 0xE6FF5050u; // rgba(0,200,255,0.9) / rgba(255,80,80,0.9)
        m_DropIndicator->Overrides()
            .Set(Style::Position, PositionType::Absolute)
            .Set(Style::PointerEvents, false)
            .Set(Style::ZIndex, 9000)
            .Set(Style::PositionLeft, StyleLength::Px(x0))
            .Set(Style::PositionTop, StyleLength::Px(yLine))
            .Set(Style::Width, StyleLength::Px(w))
            .Set(Style::Height, StyleLength::Px(2.0f))
            .Set(Style::BackgroundColor, color)
            .Set(Style::Display, DisplayMode::Block);
    }
    else
    {
        if (m_DropIndicator)
        {
            m_DropIndicator->Overrides().Set(Style::Display, DisplayMode::None);
        }
    }
}

void TreeView::UpdateDropPreviewClasses()
{
    // Highlight the hovered target row for OnItem drops.
    const bool showOnto = m_DropPreview.Visible && (m_DropPreview.Hit.Location == UI::Interaction::DropLocation::OnItem);
    for (auto& slot : m_RowPool)
    {
        if (!slot.row || slot.boundId == 0)
            continue;
        slot.row->RemoveClass("drop-onto-valid");
        slot.row->RemoveClass("drop-onto-invalid");
        if (showOnto && slot.boundId == static_cast<TreeId>(m_DropPreview.Hit.TargetId))
        {
            slot.row->AddClass(m_DropPreview.Allowed ? "drop-onto-valid" : "drop-onto-invalid");
        }
    }
}

void TreeView::SetExpanded(TreeId id, bool expanded)
{
    const bool was = IsExpanded(id);
    if (expanded)
        m_Expanded.insert(id);
    else
        m_Expanded.erase(id);
    const bool now = IsExpanded(id);
    if (was != now)
        m_FlatDirty = true;
    if (m_OnExpansionChanged && was != now)
        m_OnExpansionChanged(id, now);
}

void TreeView::ClearExpansionState()
{
    m_Expanded.clear();
    m_FlatDirty = true;
}

template <typename Fn>
void TreeView::ForEachOwnedSlot(Fn&& fn)
{
    fn(m_OnItemResizeGesture);
    fn(m_OnExpansionChanged);
    fn(m_OnScrollChanged);
    fn(m_OnSelectionChanged);
    fn(m_OnItemActivated);
    fn(m_OnContextMenu);
    fn(m_OnRowBound);
    fn(m_OnIconClicked);
    fn(m_OnIconStateCheck);
    fn(m_OnIconStateSet);
    fn(m_OnLockClicked);
    fn(m_OnLockStateCheck);
    fn(m_OnLockStateSet);
    fn(m_OnCanDrop);
    fn(m_OnPerformDrop);
}

template <typename Fn>
void TreeView::ForEachOwnedSlot(Fn&& fn) const
{
    fn(m_OnItemResizeGesture);
    fn(m_OnExpansionChanged);
    fn(m_OnScrollChanged);
    fn(m_OnSelectionChanged);
    fn(m_OnItemActivated);
    fn(m_OnContextMenu);
    fn(m_OnRowBound);
    fn(m_OnIconClicked);
    fn(m_OnIconStateCheck);
    fn(m_OnIconStateSet);
    fn(m_OnLockClicked);
    fn(m_OnLockStateCheck);
    fn(m_OnLockStateSet);
    fn(m_OnCanDrop);
    fn(m_OnPerformDrop);
}

std::size_t TreeView::CountMemberSlotsOwnedByImage(std::uint64_t base, std::uint64_t size) const
{
    // Two slots sharing a bit would alias: one executing would read as the other
    // executing too, suppressing that one's revocation and leaving a module
    // callable live past the unmap. Verified against the real constants at compile
    // time, so a copy-paste in the header cannot get past a build.
    static_assert([] {
        constexpr std::uint32_t kSlotBits[] = {
            kOnItemResizeGestureSlot, kOnExpansionChangedSlot, kOnScrollChangedSlot,
            kOnSelectionChangedSlot, kOnItemActivatedSlot,    kOnContextMenuSlot,
            kOnRowBoundSlot,         kOnIconClickedSlot,      kOnIconStateCheckSlot,
            kOnIconStateSetSlot,     kOnLockClickedSlot,      kOnLockStateCheckSlot,
            kOnLockStateSetSlot,     kOnCanDropSlot,          kOnPerformDropSlot};
        std::uint32_t seen = 0;
        for (const std::uint32_t bit : kSlotBits)
        {
            if (bit == 0 || (seen & bit) != 0)
                return false;
            seen |= bit;
        }
        return true;
    }(), "TreeView slot bits must be distinct and non-zero");

    std::size_t n = 0;
    ForEachOwnedSlot([&](const auto& slot) {
        if (slot.OwnedByImage(base, size))
            ++n;
    });
    return n;
}

void TreeView::CollectMemberSlotsOwnedByImage(std::uint64_t base, std::uint64_t size,
                                              std::uint32_t& slotMask)
{
    ForEachOwnedSlot([&](auto& slot) {
        if (!slot.OwnedByImage(base, size))
            return;
        // A slot running its callable keeps it: the quiesce ledger then still
        // counts it, refuses the unmap, and the frame returns into a mapped
        // image. Same rule the handler table follows.
        if (slot.IsExecuting())
            return;
        slotMask |= slot.SlotBit();
    });
}

std::size_t TreeView::ReleaseCollectedMemberSlots(std::uint32_t slotMask)
{
    std::size_t revoked = 0;
    // Declared before the walk so it outlives it: a released callable's captures
    // are module code, and that code can clear another of these fifteen slots or
    // destroy this tree. Nothing holds a slot — or `this` — when the sink drops
    // them.
    UI::RevokedCallableSink doomed;
    // Re-checked rather than assumed: a release can still run module code in place
    // (the residual on ModuleOwnedCallback::Release), and that code can re-enter
    // and clear another slot. Counting a slot that is already gone would inflate
    // the number the unload log reports.
    ForEachOwnedSlot([&](auto& slot) {
        if ((slotMask & slot.SlotBit()) == 0)
            return;
        if (slot.Release(doomed))
            ++revoked;
    });
    return revoked;
}

std::size_t TreeView::DropMemberSlotStampsOutsideMappedImages()
{
    std::size_t dropped = 0;
    ForEachOwnedSlot([&](auto& slot) {
        if (slot.DropStampIfUnmapped())
            ++dropped;
    });
    return dropped;
}

} // namespace GameEngine
