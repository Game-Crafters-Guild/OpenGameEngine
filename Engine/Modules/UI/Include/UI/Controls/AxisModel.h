#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Types/StringId.h"

namespace GameEngine
{

class UIElement;

// Horizontal axis = columns (tracks sized by width); Vertical axis = rows (height).
enum class Axis : std::uint8_t
{
    Horizontal,
    Vertical
};

enum class SortDirection : std::uint8_t
{
    None,
    Ascending,
    Descending
};

// One definition for a column OR a row track (CSS-Grid "track"). Size is the width
// for a column track, the height for a row track.
struct TrackDef
{
    enum class Align : std::uint8_t
    {
        Start,
        Center,
        End
    };

    StringId    Key       = 0;       // stable id: persistence + sort + cell lookup
    std::string Title;
    float       Size      = 120.0f;  // px (current); width for a column, height for a row
    float       MinSize   = 60.0f;
    bool        Flex      = false;   // elastic: grows to fill the remaining space
    bool        Sortable  = true;
    bool        Resizable = true;    // draws a resize divider after the track
    bool        Hidden    = false;
    // The trailing slack/fill track: fills empty space and absorbs resize slack so the
    // grabbed boundary follows the cursor. It is sized to grow (implies flex sizing), has
    // no resize divider, gets no body-cell binding (the binder never sees it), and is not
    // persisted. A consumer adds exactly one to carry the table's right-side space.
    bool        Fill      = false;
    Align       Alignment = Align::Start;

    // One-liner for the trailing fill track: an unsortable, non-resizable, zero-min
    // grow track that the control treats specially (see Fill). Consumers add this
    // instead of hand-rolling a flex spacer.
    static TrackDef MakeFill(StringId key = "fill"_sid)
    {
        TrackDef t;
        t.Key       = key;
        t.Fill      = true;
        t.MinSize   = 0.0f;
        t.Sortable  = false;
        t.Resizable = false;
        return t;
    }
};

// An ordered axis of tracks. Pure data — serialization lives in the editor layer.
class AxisModel
{
  public:
    explicit AxisModel(Axis orientation) : m_Orientation(orientation) {}

    Axis Orientation() const { return m_Orientation; }

    void                         Set(std::vector<TrackDef> tracks);
    const std::vector<TrackDef>& Tracks() const { return m_Tracks; }
    std::vector<TrackDef>&       Tracks() { return m_Tracks; }

    const TrackDef* Find(StringId key) const;
    TrackDef*       Find(StringId key);
    float           SizeOf(StringId key) const;
    void            SetSize(StringId key, float px);  // clamps to the track's MinSize
    void            SetHidden(StringId key, bool hidden);

    // Sum of visible track sizes + inter-track gaps + padding.
    float TotalSize(float gapPx, float padPx) const;

    void ClampToMins();

  private:
    Axis                  m_Orientation;
    std::vector<TrackDef> m_Tracks;
};

// Sizes each child of `cell` to the matching track by position via flex overrides:
// fixed tracks get a pinned basis/min/max, a Flex track grows to fill, hidden tracks
// become display:none. The caller must build exactly one child per track in track
// order — child[i] is sized to track[i]. Applied to the header row and every body
// row, on either axis.
void ApplyTrackSizes(UIElement* cell, const AxisModel& axis);

} // namespace GameEngine
