#pragma once

#include "Editor/Settings/SettingsStore.h"
#include "Events/Event.h"

#include <cstdint>

namespace GameEngine {

/**
 * User-tweakable animation window settings persisted in editor preferences.
 * Colors are ARGB (0xAARRGGBB).
 */
struct AnimationWindowSettings {
    // Curve channel colors (X/Y/Z/W)
    uint32_t ColorX = 0xFFF25959u;
    uint32_t ColorY = 0xFF59D966u;
    uint32_t ColorZ = 0xFF598FF2u;
    uint32_t ColorW = 0xFFF2CC59u;

    // Keyframe point colors
    uint32_t KeyframeColor         = 0xFF33CC80u; // default (unselected)
    uint32_t KeyframeSelectedColor = 0xFFFF9933u; // selected

    // Grid line colors (horizontal / vertical / baseline zero line)
    uint32_t GridHLineColor = 0xFF888888u;
    uint32_t GridVLineColor = 0xFFD9D9D9u;
    uint32_t BaselineColor  = 0xFFDFDFDFu;

    // Grid line thickness (px) — applies to both H and V lines
    float GridLineThickness = 1.0f;

    // Baseline (zero line) thickness in px
    float BaselineThickness = 1.0f;

    // Curve line width (px)
    float CurveLineWidth = 2.0f;

    // Frames per second an Animation window's timeline starts with
    float DefaultFps = 60.0f;

    // Whether the curves grid is shown by default. This and the snap fields below
    // seed an Animation window's toolbar when it binds; the toolbar then owns them.
    bool ShowGrid = true;

    // Snap time/value toggles default state
    bool SnapTime = false;
    bool SnapValue = false;
    float SnapTimeStep  = 0.0f; // 0 = snap to frame grid, >0 = custom time step in seconds
    float SnapValueStep = 0.0f; // 0 = auto from grid, >0 = custom value step

    // Properties tree row height
    float PropertiesTreeRowHeight = 20.0f;

    // Whether bone icons are shown in the properties tree
    bool ShowBoneIcons = true;

    // Whether the properties tree is on the right side (default: left)
    bool PropertiesPaneOnRight = false;

    // Whether the time ruler bar appears at the top of the content area instead of the bottom
    bool RulerAtTop = true;

    // Raised after any setting changes so live views can refresh immediately.
    Event<> Changed;

    void NotifyChanged() { Changed.Invoke(); }

    void Load()
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);

        int64_t c;
        if (prefs.TryGetInt64("anim.colorX", c)) ColorX = static_cast<uint32_t>(c);
        if (prefs.TryGetInt64("anim.colorY", c)) ColorY = static_cast<uint32_t>(c);
        if (prefs.TryGetInt64("anim.colorZ", c)) ColorZ = static_cast<uint32_t>(c);
        if (prefs.TryGetInt64("anim.colorW", c)) ColorW = static_cast<uint32_t>(c);
        if (prefs.TryGetInt64("anim.keyframeColor", c))         KeyframeColor         = static_cast<uint32_t>(c);
        if (prefs.TryGetInt64("anim.keyframeSelectedColor", c)) KeyframeSelectedColor = static_cast<uint32_t>(c);

        // Load new per-axis grid colors; fall back to legacy "anim.gridColor" only if it was saved.
        // If neither key exists, keep the struct default values.
        int64_t legacyGrid = 0;
        const bool hasLegacyGrid = prefs.TryGetInt64("anim.gridColor", legacyGrid);
        if (prefs.TryGetInt64("anim.gridHLineColor", c)) GridHLineColor = static_cast<uint32_t>(c);
        else if (hasLegacyGrid)                          GridHLineColor = static_cast<uint32_t>(legacyGrid);
        if (prefs.TryGetInt64("anim.gridVLineColor", c)) GridVLineColor = static_cast<uint32_t>(c);
        else if (hasLegacyGrid)                          GridVLineColor = static_cast<uint32_t>(legacyGrid);
        if (prefs.TryGetInt64("anim.baselineColor", c)) BaselineColor = static_cast<uint32_t>(c);

        double d;
        if (prefs.TryGetDouble("anim.gridLineThickness", d)) GridLineThickness = static_cast<float>(d);
        if (prefs.TryGetDouble("anim.baselineThickness", d)) BaselineThickness = static_cast<float>(d);
        if (prefs.TryGetDouble("anim.curveLineWidth", d)) CurveLineWidth = static_cast<float>(d);
        if (prefs.TryGetDouble("anim.defaultFps", d)) DefaultFps = static_cast<float>(d);

        prefs.TryGetBool("anim.showGrid", ShowGrid);
        prefs.TryGetBool("anim.snapTime", SnapTime);
        prefs.TryGetBool("anim.snapValue", SnapValue);
        if (prefs.TryGetDouble("anim.snapTimeStep", d)) SnapTimeStep = static_cast<float>(d);
        if (prefs.TryGetDouble("anim.snapValueStep", d)) SnapValueStep = static_cast<float>(d);
        prefs.TryGetBool("anim.showBoneIcons", ShowBoneIcons);
        prefs.TryGetBool("anim.propertiesPaneOnRight", PropertiesPaneOnRight);
        prefs.TryGetBool("anim.rulerAtTop", RulerAtTop);
        if (prefs.TryGetDouble("anim.propertiesTreeRowHeight", d)) PropertiesTreeRowHeight = static_cast<float>(d);
    }

    void Save()
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);

        prefs.SetInt64("anim.colorX", static_cast<int64_t>(ColorX));
        prefs.SetInt64("anim.colorY", static_cast<int64_t>(ColorY));
        prefs.SetInt64("anim.colorZ", static_cast<int64_t>(ColorZ));
        prefs.SetInt64("anim.colorW", static_cast<int64_t>(ColorW));
        prefs.SetInt64("anim.keyframeColor",         static_cast<int64_t>(KeyframeColor));
        prefs.SetInt64("anim.keyframeSelectedColor", static_cast<int64_t>(KeyframeSelectedColor));
        prefs.SetInt64("anim.gridHLineColor", static_cast<int64_t>(GridHLineColor));
        prefs.SetInt64("anim.gridVLineColor", static_cast<int64_t>(GridVLineColor));
        prefs.SetInt64("anim.baselineColor", static_cast<int64_t>(BaselineColor));
        prefs.SetDouble("anim.gridLineThickness", GridLineThickness);
        prefs.SetDouble("anim.baselineThickness", BaselineThickness);
        prefs.SetDouble("anim.curveLineWidth", CurveLineWidth);
        prefs.SetDouble("anim.defaultFps", DefaultFps);
        prefs.SetBool("anim.showGrid", ShowGrid);
        prefs.SetBool("anim.snapTime", SnapTime);
        prefs.SetBool("anim.snapValue", SnapValue);
        prefs.SetDouble("anim.snapTimeStep", static_cast<double>(SnapTimeStep));
        prefs.SetDouble("anim.snapValueStep", static_cast<double>(SnapValueStep));
        prefs.SetBool("anim.showBoneIcons", ShowBoneIcons);
        prefs.SetBool("anim.propertiesPaneOnRight", PropertiesPaneOnRight);
        prefs.SetBool("anim.rulerAtTop", RulerAtTop);
        prefs.SetDouble("anim.propertiesTreeRowHeight", PropertiesTreeRowHeight);

        prefs.Save(&err);
    }

    static AnimationWindowSettings& Get()
    {
        static AnimationWindowSettings instance;
        return instance;
    }

private:
    AnimationWindowSettings() { Load(); }
};

} // namespace GameEngine
