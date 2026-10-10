#pragma once

#include "Editor/Settings/SettingsStore.h"

#include <cstdint>
#include <string>

namespace GameEngine
{
namespace Editor
{

struct CurveEditorSettings
{
    static constexpr uint32_t kDefaultPlaybackIndicatorDotColor = 0xFFFF0081u;

    uint32_t PlaybackIndicatorDotColor = kDefaultPlaybackIndicatorDotColor;

    void Load()
    {
        auto prefs = OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);

        int64_t color = 0;
        if (prefs.TryGetInt64("curveEditor.playbackIndicatorDotColor", color))
            PlaybackIndicatorDotColor = 0xFF000000u | (static_cast<uint32_t>(color) & 0x00FFFFFFu);
    }

    void Save()
    {
        auto prefs = OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        prefs.SetInt64("curveEditor.playbackIndicatorDotColor",
                       static_cast<int64_t>(0xFF000000u | (PlaybackIndicatorDotColor & 0x00FFFFFFu)));
        prefs.Save(&err);
    }

    static CurveEditorSettings& Get()
    {
        static CurveEditorSettings instance;
        return instance;
    }

  private:
    CurveEditorSettings() { Load(); }
};

} // namespace Editor
} // namespace GameEngine
