#include "UI/Controls/TableView.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "UI/Controls/AxisHeaderBar.h"
#include "UI/Controls/ListView.h"

namespace GameEngine
{

TableView::TableView()
{
    AddClass("table-view");
    // The control owns its structural styling: the UIManager attaches this stylesheet to
    // our subtree on owner-attach, so every consumer gets dividers/headers/cells for free
    // (matches Foldout/Accordion/Button). Per-panel colours stay in the consumer's CSS.
    RequestSubtreeStyleAssetPath("UI/controls/table-view.css", "editor");

    auto header = std::make_unique<AxisHeaderBar>();
    m_Header    = header.get();
    m_Header->SetOnSort([this](StringId key) {
        if (m_SortKey == key)
            m_SortDir = (m_SortDir == SortDirection::Ascending) ? SortDirection::Descending
                                                                : SortDirection::Ascending;
        else
        {
            m_SortKey = key;
            m_SortDir = SortDirection::Ascending;
        }
        m_Header->SetSortIndicator(m_SortKey, m_SortDir);
        if (m_OnSort)
            m_OnSort(m_SortKey, m_SortDir);
    });
    m_Header->SetOnResize([this]() {
        if (m_List)
            m_List->ForEachVisibleCell([this](UIElement* row) { ApplyTrackSizes(row, m_Columns); });
        UpdateContentWidth();
    });
    m_Header->SetOnResizeCommit([this]() {
        ApplyColumnSizesToBody();
        UpdateContentWidth();
        if (m_List)
            m_List->RefreshFromProvider();
        if (m_OnLayoutChanged)
            m_OnLayoutChanged();
    });

    auto list = std::make_unique<ListView>();
    m_List    = list.get();
    m_List->AddClass("table-view-body");
    // ListView takes ownership of the header; m_Header stays valid for the table's
    // lifetime (TableView owns the ListView, which owns the header).
    m_List->SetHeader(std::move(header));
    m_List->SetItemFactory([this](ListId, IListDataProvider*) {
        auto row = std::make_unique<UIElement>();
        row->AddClass("table-view-row");
        BuildRowCells(row.get());
        return row;
    });
    m_List->SetItemBinder([this](UIElement* cell, ListId id, int index, IListDataProvider*) {
        BindRow(cell, id, index);
    });
    AddChild(std::move(list));

    m_Header->SetAxis(&m_Columns);
}

TableView::~TableView() = default;

void TableView::SetColumns(std::vector<TrackDef> columns)
{
    m_Columns.Set(std::move(columns));
    RefreshColumnLayout();
}

void TableView::RefreshColumnLayout()
{
    if (m_Header)
    {
        m_Header->Rebuild();
        // Rebuild() recreates header cells with blank sort labels, so re-paint the active sort
        // glyph — otherwise a column show/hide or title change wipes the arrow (the data stays
        // sorted via the provider). Key 0 means "never sorted", so no phantom arrow.
        if (m_SortKey != 0)
            m_Header->SetSortIndicator(m_SortKey, m_SortDir);
    }
    UpdateContentWidth();
    if (m_List && m_Provider)
        m_List->RefreshFromProvider();
}

void TableView::SetRowProvider(IListDataProvider* provider)
{
    m_Provider = provider;
    if (m_List)
        m_List->SetDataProvider(provider);
}

void TableView::SetSortIndicator(StringId key, SortDirection dir)
{
    m_SortKey = key;
    m_SortDir = dir;
    if (m_Header)
        m_Header->SetSortIndicator(key, dir);
}

void TableView::Refresh()
{
    if (m_List)
        m_List->RefreshFromProvider();
}

void TableView::BuildRowCells(UIElement* row)
{
    const std::size_t count = m_Columns.Tracks().size();
    for (std::size_t i = 0; i < count; ++i)
    {
        auto cell = std::make_unique<UIElement>();
        cell->AddClass("table-view-cell");
        row->AddChild(std::move(cell));
    }
    ApplyTrackSizes(row, m_Columns);
}

void TableView::BindRow(UIElement* row, ListId id, int rowIndex)
{
    // Row-level bind runs first so per-column cells can rely on row state/classes.
    if (m_RowBinder)
        m_RowBinder(row, id, rowIndex);
    if (!m_CellBinder)
        return;
    const auto&       tracks = m_Columns.Tracks();
    const auto&       kids   = row->GetChildren();
    const std::size_t n      = std::min(tracks.size(), kids.size());
    for (std::size_t i = 0; i < n; ++i)
    {
        // The fill track carries no data: its (empty) cell keeps the row aligned with the
        // header, but the consumer's binder never sees it — the control owns fill behavior.
        if (tracks[i].Fill)
            continue;
        m_CellBinder(kids[i].get(), static_cast<int>(i), tracks[i], id, rowIndex, m_Provider);
    }
    ApplyTrackSizes(row, m_Columns);
}

void TableView::ApplyColumnSizesToBody()
{
    if (m_List)
        m_List->ForEachVisibleCell([this](UIElement* row) { ApplyTrackSizes(row, m_Columns); });
}

void TableView::OnPostLayout()
{
    // The viewport width is only known after layout, and it changes as the panel resizes,
    // so the overflow decision (scroll vs fill) is re-evaluated here. The early-out in
    // UpdateContentWidth keeps this cheap when nothing changed.
    UpdateContentWidth();
}

void TableView::UpdateContentWidth()
{
    if (!m_List)
        return;

    const float viewport = m_List->GetViewportWidth();
    if (viewport <= 0.0f)
        return;  // pre-layout; OnPostLayout retries once the viewport is real (don't latch m_LastContentWidth)

    const float target = ComputeTableContentWidthTarget(m_Columns, viewport);
    if (std::abs(target - m_LastContentWidth) < 0.5f)
        return;  // converged — avoid re-running SetContentWidth (+ its RequestRelayout) every frame
    m_LastContentWidth = target;
    m_List->SetContentWidth(target);  // <= 0 resets the body to auto (viewport) width
}

float ComputeTableContentWidthTarget(const AxisModel& columns, float viewportWidth, float overflowMarginPx)
{
    if (viewportWidth <= 0.0f)
        return 0.0f;

    // Only fixed tracks contribute to the minimum content width; fill/flex tracks are the slack
    // that collapses (to MinSize) when the fixed columns overflow.
    float fixedSum = 0.0f;
    for (const TrackDef& t : columns.Tracks())
    {
        if (t.Hidden || t.Fill || t.Flex)
            continue;
        fixedSum += t.Size;
    }

    // Overflow → pin to the total (the body scrolls horizontally); otherwise 0 = auto, so the
    // trailing fill/flex track fills the viewport.
    return (fixedSum > viewportWidth + overflowMarginPx) ? fixedSum : 0.0f;
}

} // namespace GameEngine
