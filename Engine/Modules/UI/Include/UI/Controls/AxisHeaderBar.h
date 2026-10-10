#pragma once

#include <functional>
#include <vector>

#include "Types/StringId.h"
#include "UI/Controls/AxisModel.h"
#include "UI/UIElement.h"

namespace GameEngine
{

struct UIEvent;

// Standalone, orientation-aware header strip: one header cell per track with a
// resize divider, plus the drag/sort state machine. Borrows an AxisModel and
// mutates its track sizes on drag. Used by TableView, or alone over any body.
class AxisHeaderBar : public UIElement
{
  public:
    AxisHeaderBar();

    void SetAxis(AxisModel* axis);  // borrows; rebuilds the header cells
    void Rebuild();
    void SetSortIndicator(StringId key, SortDirection dir);

    void SetOnSort(std::function<void(StringId)> cb) { m_OnSort = std::move(cb); }
    void SetOnResize(std::function<void()> cb) { m_OnResize = std::move(cb); }
    void SetOnResizeCommit(std::function<void()> cb) { m_OnResizeCommit = std::move(cb); }

  private:
    struct Cell
    {
        UIElement* Root = nullptr;
        UIElement* Sort = nullptr;  // sort-direction icon; styled by .axis-header-sort.{ascending,descending}
        StringId   Key  = 0;
    };

    void  BeginResize(int trackIndex, UIEvent& e);
    void  UpdateResize(UIEvent& e);
    void  EndResize();
    float AxisPos(const UIEvent& e) const;

    AxisModel*                    m_Axis = nullptr;
    std::vector<Cell>             m_Cells;
    std::function<void(StringId)> m_OnSort;
    std::function<void()>         m_OnResize;
    std::function<void()>         m_OnResizeCommit;

    bool  m_Resizing          = false;
    int   m_ResizeIndex       = -1;
    float m_ResizeStartPos    = 0.0f;
    float m_ResizeStartSize   = 0.0f;
    bool  m_StartedResizeOnce = false;
};

} // namespace GameEngine
