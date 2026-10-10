#pragma once

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "Editor/Settings/SettingsStore.h"
#include "UI/Controls/AxisModel.h"

namespace GameEngine::Editor
{

// Only tracks whose size the user can actually change are worth persisting. The fill
// track carries transient slack (its size is meaningless), and a non-resizable track
// can't be dragged at all — persisting either just clutters prefs and risks reading a
// stale value back onto a column the user never sized.
inline bool IsPersistableTrack(const TrackDef& t)
{
    return t.Resizable && !t.Fill;
}

// Captures an axis's per-track size + visibility, keyed by TrackDef.Key so a
// reorder or insert never corrupts a saved layout.
inline nlohmann::json CaptureAxisLayout(const AxisModel& axis)
{
    nlohmann::json out = nlohmann::json::object();
    for (const TrackDef& t : axis.Tracks())
    {
        if (!IsPersistableTrack(t))
            continue;
        out[std::to_string(t.Key)] = {{"size", t.Size}, {"hidden", t.Hidden}};
    }
    return out;
}

// Applies a saved layout: clamp-on-load to each track's MinSize; unknown/missing
// keys keep their defaults. The owning view re-lays-out afterward.
inline void ApplyAxisLayout(AxisModel& axis, const nlohmann::json& saved)
{
    if (!saved.is_object())
        return;
    for (TrackDef& t : axis.Tracks())
    {
        if (!IsPersistableTrack(t))
            continue;
        const auto it = saved.find(std::to_string(t.Key));
        if (it == saved.end() || !it->is_object())
            continue;
        if (const auto s = it->find("size"); s != it->end() && s->is_number())
        {
            constexpr float kMaxTrackSize = 8192.0f;  // guard against corrupt prefs
            t.Size = std::clamp(s->get<float>(), t.MinSize, kMaxTrackSize);
        }
        if (const auto h = it->find("hidden"); h != it->end() && h->is_boolean())
            t.Hidden = h->get<bool>();
    }
}

inline void SaveAxisLayout(SettingsStore& store, std::string_view key, const AxisModel& axis)
{
    store.SetJson(key, CaptureAxisLayout(axis));
}

inline void LoadAxisLayout(SettingsStore& store, std::string_view key, AxisModel& axis)
{
    const nlohmann::json& root = store.Json();
    const auto            it   = root.find(std::string(key));
    if (it != root.end())
        ApplyAxisLayout(axis, *it);
}

} // namespace GameEngine::Editor
