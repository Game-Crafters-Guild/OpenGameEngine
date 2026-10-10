#pragma once

#include <functional>
#include <utility>
#include <vector>

#include "Types/StringId.h"
#include "UI/Controls/AxisModel.h"
#include "UI/Controls/ListView.h"  // IListDataProvider, ListId
#include "UI/UIElement.h"

namespace GameEngine
{

class AxisHeaderBar;

// Body content-width target for horizontal scroll, given the column model and the body's viewport
// width. Returns the sum of the visible fixed-track sizes when that overflows the viewport (so the
// table scrolls horizontally and the header tracks it), else 0 — meaning "auto", which lets the
// trailing fill/flex track absorb the slack and fill the viewport. A non-positive viewport
// (pre-layout) returns 0. Free + pure so the overflow/fits boundary is unit-testable.
float ComputeTableContentWidthTarget(const AxisModel& columns, float viewportWidth,
                                     float overflowMarginPx = 1.0f);

// Composes an AxisHeaderBar (column axis) over a virtualized ListView body.
// Consumers declare columns and supply a row data-provider + a per-column cell
// binder; the table owns header construction, the resize/sort wiring, and cell
// sizing. The row axis (matrix / row-headed) is a future body; this is column-
// headed / row-list.
class TableView : public UIElement
{
  public:
    TableView();
    ~TableView() override;

    void             SetColumns(std::vector<TrackDef> columns);
    AxisModel&       Columns() { return m_Columns; }
    const AxisModel& Columns() const { return m_Columns; }

    void SetRowProvider(IListDataProvider* provider);

    // Fills one already-created cell for (row, column); called per column on bind.
    // rowIndex is the provider's item index — the consumer's handle to its row data.
    using CellBinder = std::function<void(
        UIElement* cell, int colIndex, const TrackDef&, ListId id, int rowIndex, IListDataProvider*)>;
    void SetCellBinder(CellBinder binder) { m_CellBinder = std::move(binder); }

    // Optional row-level bind hook, invoked once per row before its column cells are
    // bound. Use it for whole-row state that the per-column binder can't own: row-level
    // CSS classes (folder/selected variants), per-row event handlers, or a row→data map.
    // The `row` is the reusable virtualized row element (pooled across binds).
    using RowBinder = std::function<void(UIElement* row, ListId id, int rowIndex)>;
    void SetRowBinder(RowBinder binder) { m_RowBinder = std::move(binder); }

    // Fired when a header is clicked. The table updates its own sort glyph + state
    // but does not reorder data: the consumer sorts its provider by (key, dir) and
    // then calls Refresh().
    void SetOnSort(std::function<void(StringId, SortDirection)> cb) { m_OnSort = std::move(cb); }
    void SetSortIndicator(StringId key, SortDirection dir);
    std::pair<StringId, SortDirection> Sort() const { return {m_SortKey, m_SortDir}; }
    void SetOnLayoutChanged(std::function<void()> cb) { m_OnLayoutChanged = std::move(cb); }

    void Refresh();              // rebind rows from the provider
    void RefreshColumnLayout();  // re-apply the header + rows after the column model was mutated externally

    ListView& Body() { return *m_List; }

  private:
    void OnPostLayout() override;
    void BuildRowCells(UIElement* row);
    void BindRow(UIElement* row, ListId id, int rowIndex);
    void ApplyColumnSizesToBody();
    // Recomputes the body's explicit content width from the visible fixed tracks so wide
    // tables scroll horizontally (and the header tracks the scroll). A no-op when the
    // total fits the viewport — the trailing fill track then absorbs the slack instead.
    void UpdateContentWidth();

    AxisModel          m_Columns{Axis::Horizontal};
    AxisHeaderBar*     m_Header   = nullptr;
    ListView*          m_List     = nullptr;
    IListDataProvider* m_Provider = nullptr;
    CellBinder         m_CellBinder;
    RowBinder          m_RowBinder;

    std::function<void(StringId, SortDirection)> m_OnSort;
    std::function<void()>                        m_OnLayoutChanged;

    StringId      m_SortKey = 0;
    SortDirection m_SortDir = SortDirection::None;

    // Last content width pushed to the body; the recompute early-outs on no change so a
    // per-frame OnPostLayout (or a resize drag) doesn't request a relayout every tick. Seeded to
    // a value no valid target (0 or a positive width) can equal, so the first computed target applies.
    float m_LastContentWidth = -2.0f;
};

} // namespace GameEngine
