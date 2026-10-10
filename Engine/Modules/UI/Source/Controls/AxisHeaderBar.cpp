#include "UI/Controls/AxisHeaderBar.h"

#include <algorithm>
#include <utility>

#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"

namespace GameEngine
{

namespace
{

// A content track is a real, visible data column: not hidden, not the elastic fill
// track (and not a flex track, which has no fixed basis to resize). A divider belongs
// after track `index` only when a later content track exists to resize against — this
// keeps dividers strictly between content columns and off the trailing edge before the
// fill track.
bool IsContentTrack(const TrackDef& t)
{
    return !t.Hidden && !t.Fill && !t.Flex;
}

bool HasLaterContentTrack(const std::vector<TrackDef>& tracks, std::size_t index)
{
    for (std::size_t j = index + 1; j < tracks.size(); ++j)
    {
        if (IsContentTrack(tracks[j]))
            return true;
    }
    return false;
}

} // namespace

AxisHeaderBar::AxisHeaderBar()
{
    AddClass("axis-header");
    RegisterEventHandler(kEventMouseMove, [this](UIEvent& e) { UpdateResize(e); });
    RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) {
        if (m_Resizing)
        {
            EndResize();
            e.Stop();
        }
    });
}

void AxisHeaderBar::SetAxis(AxisModel* axis)
{
    m_Axis = axis;
    Rebuild();
}

float AxisHeaderBar::AxisPos(const UIEvent& e) const
{
    return (m_Axis && m_Axis->Orientation() == Axis::Vertical) ? e.Y : e.X;
}

void AxisHeaderBar::Rebuild()
{
    RemoveAllChildren();
    m_Cells.clear();
    // Structure changed — the next resize must re-sync sizes before capture.
    m_StartedResizeOnce = false;
    RemoveClass("horizontal");
    RemoveClass("vertical");
    if (!m_Axis)
        return;

    AddClass(m_Axis->Orientation() == Axis::Vertical ? "vertical" : "horizontal");

    const auto& tracks = m_Axis->Tracks();
    for (std::size_t i = 0; i < tracks.size(); ++i)
    {
        const TrackDef& t = tracks[i];

        auto cell = std::make_unique<UIElement>();
        cell->AddClass("axis-header-cell");
        switch (t.Alignment)
        {
        case TrackDef::Align::Center: cell->AddClass("align-center"); break;
        case TrackDef::Align::End:    cell->AddClass("align-end"); break;
        default: break;
        }

        auto title = std::make_unique<Label>();
        title->AddClass("axis-header-title");
        title->SetText(t.Title);
        cell->AddChild(std::move(title));

        // Sort-direction icon (empty until SetSortIndicator marks this the active column).
        // A plain element styled via background-image — the UI font has no glyph for the
        // U+25B2/U+25BC triangles (advance width only, no outline), so a text label renders blank.
        auto       sort    = std::make_unique<UIElement>();
        sort->AddClass("axis-header-sort");
        UIElement* sortPtr = sort.get();
        cell->AddChild(std::move(sort));

        const StringId   key      = t.Key;
        const bool       sortable = t.Sortable;
        UIElement*       cellPtr  = cell.get();
        cellPtr->RegisterEventHandler(kEventMouseDown, [this, sortable, key](UIEvent& e) {
            // A divider mousedown Stops before this fires; the guard covers the
            // case where the event still reaches the cell during an active drag.
            if (e.Button != 0 || m_Resizing)
                return;
            if (sortable && m_OnSort)
                m_OnSort(key);
            e.Stop();
        });

        // The fill track and flex tracks have no fixed basis to drag — resizing them is a
        // no-op — so they get no grab handle even if marked resizable. A divider also only
        // belongs BETWEEN two content columns: the last content track (the one before the
        // trailing fill track, or the final column when there's none) gets none, so no
        // separator hangs on the trailing edge.
        if (t.Resizable && !t.Fill && !t.Flex && HasLaterContentTrack(tracks, i))
        {
            const int index   = static_cast<int>(i);
            auto      divider = std::make_unique<UIElement>();
            divider->AddClass("axis-divider");

            auto line = std::make_unique<UIElement>();
            line->AddClass("axis-divider-line");
            divider->AddChild(std::move(line));

            divider->RegisterEventHandler(kEventMouseDown, [this, index](UIEvent& e) {
                if (e.Button != 0)
                    return;
                BeginResize(index, e);
                e.Stop();
            });
            cell->AddChild(std::move(divider));
        }

        AddChild(std::move(cell));
        m_Cells.push_back(Cell{cellPtr, sortPtr, key});
    }

    ApplyTrackSizes(this, *m_Axis);
}

void AxisHeaderBar::BeginResize(int trackIndex, UIEvent& e)
{
    if (!m_Axis)
        return;
    auto& tracks = m_Axis->Tracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;

    if (!m_StartedResizeOnce)
    {
        ApplyTrackSizes(this, *m_Axis);  // sync header before capture, avoids a jump
        m_StartedResizeOnce = true;
    }
    m_Resizing        = true;
    m_ResizeIndex     = trackIndex;
    m_ResizeStartPos  = AxisPos(e);
    m_ResizeStartSize = tracks[trackIndex].Size;
    e.Capture(this);
}

void AxisHeaderBar::UpdateResize(UIEvent& e)
{
    if (!m_Resizing || !m_Axis)
        return;
    auto& tracks = m_Axis->Tracks();
    if (m_ResizeIndex < 0 || m_ResizeIndex >= static_cast<int>(tracks.size()))
        return;

    TrackDef&   t    = tracks[m_ResizeIndex];
    const float pos  = AxisPos(e);
    const float next = std::max(t.MinSize, m_ResizeStartSize + (pos - m_ResizeStartPos));
    if (t.Size != next)
    {
        t.Size = next;
        ApplyTrackSizes(this, *m_Axis);
        if (m_OnResize)
            m_OnResize();
        // ApplyTrackSizes marks header + body cells layout-dirty, but virtualized
        // bodies (absolutely-positioned rows) won't reflow from dirty bits alone
        // mid-drag — they only rebuild on the mouse-up commit. Request a relayout so
        // the whole table tracks the handle live (matches Splitter::UpdateDrag).
        RequestRelayout();
    }
    // Incremental delta: the next move is relative to this position, so the first
    // move after capture never jumps.
    m_ResizeStartPos  = pos;
    m_ResizeStartSize = next;
    e.Stop();
}

void AxisHeaderBar::EndResize()
{
    m_Resizing    = false;
    m_ResizeIndex = -1;
    if (m_OnResizeCommit)
        m_OnResizeCommit();
}

void AxisHeaderBar::SetSortIndicator(StringId key, SortDirection dir)
{
    for (auto& c : m_Cells)
    {
        const bool active = (c.Key == key) && (dir != SortDirection::None);

        // The active column's title brightens (the legacy ".sorted" affordance).
        if (c.Root)
        {
            if (active)
                c.Root->AddClass("sorted");
            else
                c.Root->RemoveClass("sorted");
        }

        // The direction class selects the up/down icon in CSS; clearing both hides it.
        if (c.Sort)
        {
            c.Sort->RemoveClass("ascending");
            c.Sort->RemoveClass("descending");
            if (active)
                c.Sort->AddClass(dir == SortDirection::Ascending ? "ascending" : "descending");
        }
    }
}

} // namespace GameEngine
