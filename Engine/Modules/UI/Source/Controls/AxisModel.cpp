#include "UI/Controls/AxisModel.h"

#include <algorithm>

#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIStyle.h"

namespace GameEngine
{

void AxisModel::Set(std::vector<TrackDef> tracks)
{
    m_Tracks = std::move(tracks);
    ClampToMins();
}

const TrackDef* AxisModel::Find(StringId key) const
{
    for (const auto& t : m_Tracks)
        if (t.Key == key)
            return &t;
    return nullptr;
}

TrackDef* AxisModel::Find(StringId key)
{
    for (auto& t : m_Tracks)
        if (t.Key == key)
            return &t;
    return nullptr;
}

float AxisModel::SizeOf(StringId key) const
{
    const TrackDef* t = Find(key);
    return t ? t->Size : 0.0f;
}

void AxisModel::SetSize(StringId key, float px)
{
    if (TrackDef* t = Find(key))
        t->Size = std::max(t->MinSize, px);
}

void AxisModel::SetHidden(StringId key, bool hidden)
{
    if (TrackDef* t = Find(key))
        t->Hidden = hidden;
}

float AxisModel::TotalSize(float gapPx, float padPx) const
{
    float total = padPx;
    int   visible = 0;
    for (const auto& t : m_Tracks)
    {
        if (t.Hidden)
            continue;
        total += t.Size;
        ++visible;
    }
    if (visible > 1)
        total += gapPx * static_cast<float>(visible - 1);
    return total;
}

void AxisModel::ClampToMins()
{
    for (auto& t : m_Tracks)
        if (t.Size < t.MinSize)
            t.Size = t.MinSize;
}

void ApplyTrackSizes(UIElement* cell, const AxisModel& axis)
{
    if (!cell)
        return;

    const auto&       tracks = axis.Tracks();
    const auto&       kids   = cell->GetChildren();
    const std::size_t n      = std::min(tracks.size(), kids.size());
    const bool        horiz  = axis.Orientation() == Axis::Horizontal;

    for (std::size_t i = 0; i < n; ++i)
    {
        UIElement* child = kids[i].get();
        if (!child)
            continue;
        const TrackDef& t = tracks[i];

        if (t.Hidden)
        {
            child->Overrides().Set(Style::Display, DisplayMode::None);
            continue;
        }
        child->Overrides().Reset(Style::Display);  // fall back to the CSS display value

        // A Fill track grows exactly like a Flex track — it is the trailing slack that
        // absorbs resize deltas — so both take the grow branch.
        if (t.Flex || t.Fill)
        {
            child->Overrides().Set(Style::FlexGrow, 1.0f);
            child->Overrides().Set(Style::FlexShrink, 1.0f);
            child->Overrides().Set(Style::FlexBasis, StyleLength::Px(0.0f));
            child->Overrides().Set(horiz ? Style::MinWidth : Style::MinHeight,
                                   StyleLength::Px(t.MinSize));
            child->Overrides().Reset(horiz ? Style::MaxWidth : Style::MaxHeight);
        }
        else
        {
            const StyleLength px = StyleLength::Px(t.Size);
            child->Overrides().Set(Style::FlexGrow, 0.0f);
            child->Overrides().Set(Style::FlexShrink, 0.0f);
            child->Overrides().Set(Style::FlexBasis, px);
            child->Overrides().Set(horiz ? Style::MinWidth : Style::MinHeight, px);
            child->Overrides().Set(horiz ? Style::MaxWidth : Style::MaxHeight, px);
        }
    }
}

} // namespace GameEngine
