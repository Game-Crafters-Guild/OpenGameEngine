#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include "Editor/Settings/SettingsStore.h"
#include "Components/Rendering/PostProcessVolume.h" // kDefaultManualExposureEv

namespace GameEngine
{
namespace Editor
{

enum class HoverHighlightMode : uint8_t
{
    Always,
    WithModifier,
    Never
};

// Which key must be held when HoverHighlightMode::WithModifier is selected.
enum class HoverHighlightModifier : uint8_t
{
    Control = 0,
    Shift   = 1,
    Grave   = 2 // ` (backtick)
};

// Corner placement for the viewport axis rotation gizmo overlay.
enum class RotationGizmoCorner : uint8_t
{
    TopRight = 0,
    TopLeft = 1,
    BottomRight = 2,
    BottomLeft = 3
};

// Drag-select marquee shape used by the Scene View selection tool.
enum class MarqueeShape : uint8_t
{
    Rectangle = 0,
    Lasso     = 1
};

// How selected (and hovered) entities are highlighted in the Scene View.
enum class SelectionHighlightStyle : uint8_t
{
    Box     = 0, // Wireframe bounding box (legacy)
    Outline = 1, // Screen-space silhouette outline
    Both    = 2,
};

// User-level settings for the editor Scene View camera.
// Values are persisted in the Editor preferences (Preferences.json).
class SceneViewSettings
{
public:
    static constexpr float kDefaultNearClip = 0.01f;
    static constexpr float kMinNearClip = 0.001f;
    static constexpr float kMaxNearClip = 10.0f;
    static constexpr float kDefaultFarClip = 3000.0f;
    static constexpr float kMinFarClip = 10.0f;
    static constexpr float kMaxFarClip = 100000.0f;
    static constexpr float kDefaultFieldOfViewDeg = 60.0f;
    static constexpr float kMinFieldOfViewDeg = 20.0f;
    static constexpr float kMaxFieldOfViewDeg = 120.0f;
    static constexpr float kDefaultMoveSpeed = 5.0f;
    static constexpr float kMinMoveSpeed = 0.1f;
    static constexpr float kMaxMoveSpeed = 100.0f;
    static constexpr float kDefaultFastMoveMultiplier = 3.0f;
    static constexpr float kMinFastMoveMultiplier = 1.0f;
    static constexpr float kMaxFastMoveMultiplier = 20.0f;
    static constexpr float kDefaultMoveAccelerationTime = 0.15f;
    // Scene View auto-exposure clamp defaults — keep in lockstep with Components::Camera.
    static constexpr float kDefaultAutoExposureMinEv = 4.0f;
    static constexpr float kDefaultAutoExposureMaxEv = 18.0f;
    // Valid ranges shared by Validate() and the editing UIs (Settings panel rows +
    // the scene camera popup), so sliders and the clamp can never diverge.
    static constexpr float kMinExposureEv = -5.0f;
    static constexpr float kMaxExposureEv = 20.0f;
    static constexpr float kMaxExposureCompensation = 5.0f;
    // Detent size of the popup's exposure sliders — also the granularity the
    // Auto->Fixed EV handoff seed is quantized to, so the seed lands on a
    // slider detent (kEvSeedDecimalGrid mirrors Slider's decimal-grid snap).
    static constexpr float kExposureSliderStep = 0.1f;
    static constexpr double kEvSeedDecimalGrid = 1e6;
    static constexpr float kMaxMoveAccelerationTime = 1.0f;

    static SceneViewSettings& Get()
    {
        static SceneViewSettings instance;
        return instance;
    }

    using CameraSettingsChangedCallback = std::function<void()>;

    uint64_t AddCameraSettingsChangedListener(CameraSettingsChangedCallback callback)
    {
        if (!callback)
            return 0;
        const uint64_t id = m_NextCameraSettingsListenerId++;
        m_CameraSettingsChangedListeners.emplace(id, std::move(callback));
        return id;
    }

    void RemoveCameraSettingsChangedListener(uint64_t id)
    {
        if (id != 0)
            m_CameraSettingsChangedListeners.erase(id);
    }

    void Load()
    {
        auto prefs = OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);

        double nearClip = static_cast<double>(m_NearClip);
        double farClip = static_cast<double>(m_FarClip);
        double fovDeg = static_cast<double>(m_FieldOfViewDeg);
        double moveSpeed = static_cast<double>(m_MoveSpeed);
        double fastMoveMultiplier = static_cast<double>(m_FastMoveMultiplier);
        double maxDistance = static_cast<double>(m_MaxDistance);

        prefs.TryGetDouble("sceneView.nearClip", nearClip);
        prefs.TryGetDouble("sceneView.farClip", farClip);
        prefs.TryGetDouble("sceneView.fieldOfViewDeg", fovDeg);
        prefs.TryGetDouble("sceneView.moveSpeed", moveSpeed);
        prefs.TryGetDouble("sceneView.fastMoveMultiplier", fastMoveMultiplier);
        prefs.TryGetDouble("sceneView.maxDistance", maxDistance);
        {
            double accel = static_cast<double>(m_MoveAccelerationTime);
            prefs.TryGetDouble("sceneView.moveAccelerationTime", accel);
            m_MoveAccelerationTime = static_cast<float>(accel);
        }
        {
            double sens = static_cast<double>(m_LookSensitivity);
            prefs.TryGetDouble("sceneView.lookSensitivity", sens);
            m_LookSensitivity = static_cast<float>(sens);
        }
        prefs.TryGetBool("sceneView.invertY", m_InvertY);
        prefs.TryGetBool("sceneView.scrollWheelDollyEnabled", m_ScrollWheelDollyEnabled);
        prefs.TryGetBool("sceneView.scrollWheelDollyReversed", m_ScrollWheelDollyReversed);
        prefs.TryGetBool("sceneView.gridVisibleOnStartup", m_GridVisibleOnStartup);
        prefs.TryGetBool("sceneView.exactPickMode", m_ExactPickMode);
        prefs.TryGetBool("sceneView.lightGizmoDetails.directionalOnlyOnSelection", m_DirectionalLightGizmoDetailsOnlyOnSelection);
        prefs.TryGetBool("sceneView.lightGizmoDetails.pointOnlyOnSelection", m_PointLightGizmoDetailsOnlyOnSelection);
        prefs.TryGetBool("sceneView.lightGizmoDetails.spotOnlyOnSelection", m_SpotLightGizmoDetailsOnlyOnSelection);
        prefs.TryGetBool("sceneView.showRotationGizmo", m_ShowRotationGizmo);
        prefs.TryGetBool("sceneView.showRotationGizmo.splitPerspective", m_ShowRotationGizmoInSplitViews[0]);
        prefs.TryGetBool("sceneView.showRotationGizmo.splitTop", m_ShowRotationGizmoInSplitViews[1]);
        prefs.TryGetBool("sceneView.showRotationGizmo.splitFront", m_ShowRotationGizmoInSplitViews[2]);
        prefs.TryGetBool("sceneView.showRotationGizmo.splitSide", m_ShowRotationGizmoInSplitViews[3]);
        prefs.TryGetBool("sceneView.hoverNamePillEnabled", m_HoverNamePillEnabled);
        prefs.TryGetBool("sceneView.hoverNamePillNearCursor", m_HoverNamePillNearCursor);
        prefs.TryGetBool("sceneView.showRulers", m_ShowRulers);
        prefs.TryGetBool("sceneView.showGameUI", m_ShowGameUI);
        {
            double op = static_cast<double>(m_RulerOpacity);
            prefs.TryGetDouble("sceneView.rulerOpacity", op);
            float opF = static_cast<float>(op);
            if (opF < 0.0f) opF = 0.0f;
            if (opF > 1.0f) opF = 1.0f;
            m_RulerOpacity = opF;
        }
        {
            double v = static_cast<double>(m_RulerIndicatorColor);
            prefs.TryGetDouble("sceneView.rulerIndicatorColor", v);
            m_RulerIndicatorColor = static_cast<uint32_t>(v);
        }
        {
            double v = static_cast<double>(m_MeasureColor);
            prefs.TryGetDouble("sceneView.measureColor", v);
            m_MeasureColor = static_cast<uint32_t>(v);
        }
        {
            double v = static_cast<double>(m_RulerIndicatorThickness);
            prefs.TryGetDouble("sceneView.rulerIndicatorThickness", v);
            float t = static_cast<float>(v);
            if (t < 0.5f) t = 0.5f;
            if (t > 6.0f) t = 6.0f;
            m_RulerIndicatorThickness = t;
        }
        {
            double v = static_cast<double>(m_GridColor3D);
            prefs.TryGetDouble("sceneView.gridColor3D", v);
            m_GridColor3D = static_cast<uint32_t>(v);
        }
        {
            double v = static_cast<double>(m_GridColor2D);
            prefs.TryGetDouble("sceneView.gridColor2D", v);
            m_GridColor2D = static_cast<uint32_t>(v);
        }
        {
            double v = static_cast<double>(m_GridOpacity3D);
            prefs.TryGetDouble("sceneView.gridOpacity3D", v);
            float opF = static_cast<float>(v);
            if (opF < 0.0f) opF = 0.0f;
            if (opF > 1.0f) opF = 1.0f;
            m_GridOpacity3D = opF;
        }
        {
            double v = static_cast<double>(m_GridOpacity2D);
            prefs.TryGetDouble("sceneView.gridOpacity2D", v);
            float opF = static_cast<float>(v);
            if (opF < 0.0f) opF = 0.0f;
            if (opF > 1.0f) opF = 1.0f;
            m_GridOpacity2D = opF;
        }
        prefs.TryGetBool("sceneView.toolOverlayBorderless", m_ToolOverlayBorderless);
        prefs.TryGetBool("sceneView.pixelPerfect2D", m_PixelPerfect2D);
        prefs.TryGetBool("sceneView.cameraFrameGuide", m_CameraFrameGuide);
        prefs.TryGetBool("sceneView.postProcessingEnabled", m_PostProcessingEnabled);
        prefs.TryGetBool("sceneView.postFxBloomEnabled", m_PostFxBloomEnabled);
        prefs.TryGetBool("sceneView.postFxTonemapEnabled", m_PostFxTonemapEnabled);
        prefs.TryGetBool("sceneView.postFxColorFilterEnabled", m_PostFxColorFilterEnabled);
        prefs.TryGetBool("sceneView.postFxCasEnabled", m_PostFxCasEnabled);
        prefs.TryGetBool("sceneView.postFxCrtEnabled", m_PostFxCrtEnabled);
        prefs.TryGetBool("sceneView.postFxAutoExposureEnabled", m_PostFxAutoExposureEnabled);
        prefs.TryGetBool("sceneView.postFxAutoExposureUseAdaptationDelay",
                         m_PostFxAutoExposureUseAdaptationDelay);
        {
            double ev = static_cast<double>(m_PostFxFixedExposureEv);
            prefs.TryGetDouble("sceneView.postFxFixedExposureEv", ev);
            m_PostFxFixedExposureEv = static_cast<float>(ev);
        }
        {
            double v = static_cast<double>(m_PostFxAutoExposureMinEv);
            prefs.TryGetDouble("sceneView.postFxAutoExposureMinEv", v);
            m_PostFxAutoExposureMinEv = static_cast<float>(v);
        }
        {
            double v = static_cast<double>(m_PostFxAutoExposureMaxEv);
            prefs.TryGetDouble("sceneView.postFxAutoExposureMaxEv", v);
            m_PostFxAutoExposureMaxEv = static_cast<float>(v);
        }
        {
            double v = static_cast<double>(m_PostFxExposureCompensation);
            prefs.TryGetDouble("sceneView.postFxExposureCompensation", v);
            m_PostFxExposureCompensation = static_cast<float>(v);
        }
        {
            double scaleD = static_cast<double>(m_PixelPerfectScale);
            prefs.TryGetDouble("sceneView.pixelPerfectScale", scaleD);
            int scaleInt = static_cast<int>(scaleD);
            if (scaleInt >= 1 && scaleInt <= 6)
                m_PixelPerfectScale = scaleInt;
        }
        {
            double cornerD = static_cast<double>(m_RotationGizmoCorner);
            prefs.TryGetDouble("sceneView.rotationGizmoCorner", cornerD);
            int cornerInt = static_cast<int>(cornerD);
            if (cornerInt >= 0 && cornerInt <= 3)
                m_RotationGizmoCorner = static_cast<RotationGizmoCorner>(cornerInt);
        }
        {
            double hoverMode = static_cast<double>(m_HoverHighlightMode);
            prefs.TryGetDouble("sceneView.hoverHighlightMode", hoverMode);
            int modeInt = static_cast<int>(hoverMode);
            if (modeInt >= 0 && modeInt <= 2)
                m_HoverHighlightMode = static_cast<HoverHighlightMode>(modeInt);
        }
        {
            double modD = static_cast<double>(m_HoverHighlightModifier);
            prefs.TryGetDouble("sceneView.hoverHighlightModifier", modD);
            const int modInt = static_cast<int>(modD);
            if (modInt >= 0 && modInt <= 2)
                m_HoverHighlightModifier = static_cast<HoverHighlightModifier>(modInt);
        }
        {
            double modD = static_cast<double>(m_RevealHiddenHoverModifier);
            prefs.TryGetDouble("sceneView.revealHiddenHoverModifier", modD);
            const int modInt = static_cast<int>(modD);
            if (modInt >= 0 && modInt <= 2)
                m_RevealHiddenHoverModifier = static_cast<HoverHighlightModifier>(modInt);
        }
        {
            double shape = static_cast<double>(m_MarqueeShape);
            prefs.TryGetDouble("sceneView.marqueeShape", shape);
            const int shapeInt = static_cast<int>(shape);
            if (shapeInt >= 0 && shapeInt <= 1)
                m_MarqueeShape = static_cast<MarqueeShape>(shapeInt);
        }
        {
            double color = static_cast<double>(m_MarqueeColor);
            prefs.TryGetDouble("sceneView.marqueeColor", color);
            m_MarqueeColor = static_cast<uint32_t>(color);
        }
        {
            double thick = static_cast<double>(m_MarqueeThickness);
            prefs.TryGetDouble("sceneView.marqueeThickness", thick);
            m_MarqueeThickness = static_cast<float>(thick);
        }
        {
            double style = static_cast<double>(m_SelectionHighlightStyle);
            prefs.TryGetDouble("sceneView.selectionHighlightStyle", style);
            const int styleInt = static_cast<int>(style);
            if (styleInt >= 0 && styleInt <= 2)
                m_SelectionHighlightStyle = static_cast<SelectionHighlightStyle>(styleInt);
        }
        {
            double v = static_cast<double>(m_SelectionBoxColor);
            prefs.TryGetDouble("sceneView.selectionBoxColor", v);
            m_SelectionBoxColor = static_cast<uint32_t>(v);
        }
        {
            double v = static_cast<double>(m_SelectionBoxThickness);
            prefs.TryGetDouble("sceneView.selectionBoxThickness", v);
            m_SelectionBoxThickness = static_cast<float>(v);
        }
        {
            double v = static_cast<double>(m_SelectionOutlineColor);
            prefs.TryGetDouble("sceneView.selectionOutlineColor", v);
            m_SelectionOutlineColor = static_cast<uint32_t>(v);
        }
        {
            double v = static_cast<double>(m_SelectionOutlineThickness);
            prefs.TryGetDouble("sceneView.selectionOutlineThickness", v);
            m_SelectionOutlineThickness = static_cast<float>(v);
        }
        prefs.TryGetBool("sceneView.selectionOutlinesVisible", m_SelectionOutlinesVisible);
        {
            double v = static_cast<double>(m_BackgroundColor);
            prefs.TryGetDouble("sceneView.backgroundColor", v);
            m_BackgroundColor = static_cast<uint32_t>(v);
        }
        prefs.TryGetBool("sceneView.hdrTestPatternEnabled", m_HdrTestPatternEnabled);
        {
            double scale = static_cast<double>(m_RenderScale);
            prefs.TryGetDouble("sceneView.renderScale", scale);
            m_RenderScale = static_cast<float>(scale);
        }

        m_NearClip = static_cast<float>(nearClip);
        m_FarClip = static_cast<float>(farClip);
        m_FieldOfViewDeg = static_cast<float>(fovDeg);
        m_MoveSpeed = static_cast<float>(moveSpeed);
        m_FastMoveMultiplier = static_cast<float>(fastMoveMultiplier);
        m_MaxDistance = static_cast<float>(maxDistance);
        Validate();
        NotifyCameraSettingsChanged();
    }

    void Save()
    {
        auto prefs = OpenEditorPreferences();
        std::string err;
        prefs.Load(&err); // Preserve other settings

        prefs.SetDouble("sceneView.nearClip", static_cast<double>(m_NearClip));
        prefs.SetDouble("sceneView.farClip", static_cast<double>(m_FarClip));
        prefs.SetDouble("sceneView.fieldOfViewDeg", static_cast<double>(m_FieldOfViewDeg));
        prefs.SetDouble("sceneView.moveSpeed", static_cast<double>(m_MoveSpeed));
        prefs.SetDouble("sceneView.fastMoveMultiplier", static_cast<double>(m_FastMoveMultiplier));
        prefs.SetDouble("sceneView.maxDistance", static_cast<double>(m_MaxDistance));
        prefs.SetDouble("sceneView.moveAccelerationTime", static_cast<double>(m_MoveAccelerationTime));
        prefs.SetDouble("sceneView.lookSensitivity", static_cast<double>(m_LookSensitivity));
        prefs.SetBool("sceneView.invertY", m_InvertY);
        prefs.SetBool("sceneView.scrollWheelDollyEnabled", m_ScrollWheelDollyEnabled);
        prefs.SetBool("sceneView.scrollWheelDollyReversed", m_ScrollWheelDollyReversed);
        prefs.SetBool("sceneView.gridVisibleOnStartup", m_GridVisibleOnStartup);
        prefs.SetBool("sceneView.exactPickMode", m_ExactPickMode);
        prefs.SetBool("sceneView.lightGizmoDetails.directionalOnlyOnSelection", m_DirectionalLightGizmoDetailsOnlyOnSelection);
        prefs.SetBool("sceneView.lightGizmoDetails.pointOnlyOnSelection", m_PointLightGizmoDetailsOnlyOnSelection);
        prefs.SetBool("sceneView.lightGizmoDetails.spotOnlyOnSelection", m_SpotLightGizmoDetailsOnlyOnSelection);
        prefs.SetBool("sceneView.showRotationGizmo", m_ShowRotationGizmo);
        prefs.SetBool("sceneView.showRotationGizmo.splitPerspective", m_ShowRotationGizmoInSplitViews[0]);
        prefs.SetBool("sceneView.showRotationGizmo.splitTop", m_ShowRotationGizmoInSplitViews[1]);
        prefs.SetBool("sceneView.showRotationGizmo.splitFront", m_ShowRotationGizmoInSplitViews[2]);
        prefs.SetBool("sceneView.showRotationGizmo.splitSide", m_ShowRotationGizmoInSplitViews[3]);
        prefs.SetBool("sceneView.hoverNamePillEnabled", m_HoverNamePillEnabled);
        prefs.SetBool("sceneView.hoverNamePillNearCursor", m_HoverNamePillNearCursor);
        prefs.SetBool("sceneView.showRulers", m_ShowRulers);
        prefs.SetBool("sceneView.showGameUI", m_ShowGameUI);
        prefs.SetDouble("sceneView.rulerOpacity", static_cast<double>(m_RulerOpacity));
        prefs.SetDouble("sceneView.rulerIndicatorColor", static_cast<double>(m_RulerIndicatorColor));
        prefs.SetDouble("sceneView.measureColor", static_cast<double>(m_MeasureColor));
        prefs.SetDouble("sceneView.rulerIndicatorThickness", static_cast<double>(m_RulerIndicatorThickness));
        prefs.SetDouble("sceneView.gridColor3D", static_cast<double>(m_GridColor3D));
        prefs.SetDouble("sceneView.gridColor2D", static_cast<double>(m_GridColor2D));
        prefs.SetDouble("sceneView.gridOpacity3D", static_cast<double>(m_GridOpacity3D));
        prefs.SetDouble("sceneView.gridOpacity2D", static_cast<double>(m_GridOpacity2D));
        prefs.SetBool("sceneView.toolOverlayBorderless", m_ToolOverlayBorderless);
        prefs.SetBool("sceneView.pixelPerfect2D", m_PixelPerfect2D);
        prefs.SetBool("sceneView.cameraFrameGuide", m_CameraFrameGuide);
        prefs.SetBool("sceneView.postProcessingEnabled", m_PostProcessingEnabled);
        prefs.SetBool("sceneView.postFxBloomEnabled", m_PostFxBloomEnabled);
        prefs.SetBool("sceneView.postFxTonemapEnabled", m_PostFxTonemapEnabled);
        prefs.SetBool("sceneView.postFxColorFilterEnabled", m_PostFxColorFilterEnabled);
        prefs.SetBool("sceneView.postFxCasEnabled", m_PostFxCasEnabled);
        prefs.SetBool("sceneView.postFxCrtEnabled", m_PostFxCrtEnabled);
        prefs.SetBool("sceneView.postFxAutoExposureEnabled", m_PostFxAutoExposureEnabled);
        prefs.SetBool("sceneView.postFxAutoExposureUseAdaptationDelay",
                      m_PostFxAutoExposureUseAdaptationDelay);
        prefs.SetDouble("sceneView.postFxFixedExposureEv", static_cast<double>(m_PostFxFixedExposureEv));
        prefs.SetDouble("sceneView.postFxAutoExposureMinEv", static_cast<double>(m_PostFxAutoExposureMinEv));
        prefs.SetDouble("sceneView.postFxAutoExposureMaxEv", static_cast<double>(m_PostFxAutoExposureMaxEv));
        prefs.SetDouble("sceneView.postFxExposureCompensation", static_cast<double>(m_PostFxExposureCompensation));
        prefs.SetDouble("sceneView.pixelPerfectScale", static_cast<double>(m_PixelPerfectScale));
        prefs.SetDouble("sceneView.hoverHighlightMode", static_cast<double>(m_HoverHighlightMode));
        prefs.SetDouble("sceneView.hoverHighlightModifier", static_cast<double>(m_HoverHighlightModifier));
        prefs.SetDouble("sceneView.revealHiddenHoverModifier", static_cast<double>(m_RevealHiddenHoverModifier));
        prefs.SetDouble("sceneView.rotationGizmoCorner", static_cast<double>(m_RotationGizmoCorner));
        prefs.SetDouble("sceneView.marqueeShape", static_cast<double>(m_MarqueeShape));
        prefs.SetDouble("sceneView.marqueeColor", static_cast<double>(m_MarqueeColor));
        prefs.SetDouble("sceneView.marqueeThickness", static_cast<double>(m_MarqueeThickness));
        prefs.SetDouble("sceneView.selectionHighlightStyle", static_cast<double>(m_SelectionHighlightStyle));
        prefs.SetDouble("sceneView.selectionBoxColor", static_cast<double>(m_SelectionBoxColor));
        prefs.SetDouble("sceneView.selectionBoxThickness", static_cast<double>(m_SelectionBoxThickness));
        prefs.SetDouble("sceneView.selectionOutlineColor", static_cast<double>(m_SelectionOutlineColor));
        prefs.SetDouble("sceneView.selectionOutlineThickness", static_cast<double>(m_SelectionOutlineThickness));
        prefs.SetBool("sceneView.selectionOutlinesVisible", m_SelectionOutlinesVisible);
        prefs.SetDouble("sceneView.backgroundColor", static_cast<double>(m_BackgroundColor));
        prefs.SetBool("sceneView.hdrTestPatternEnabled", m_HdrTestPatternEnabled);
        prefs.SetDouble("sceneView.renderScale", static_cast<double>(m_RenderScale));

        prefs.Save(&err);
    }

    float GetLookSensitivity() const { return m_LookSensitivity; }
    bool  GetInvertY() const { return m_InvertY; }

    float GetNearClip() const { return m_NearClip; }
    float GetFarClip() const { return m_FarClip; }
    float GetFieldOfViewDeg() const { return m_FieldOfViewDeg; }
    float GetMoveSpeed() const { return m_MoveSpeed; }
    float GetFastMoveMultiplier() const { return m_FastMoveMultiplier; }
    // Seconds for the fly camera to ease toward its target velocity (exponential
    // time constant); 0 = instant response (no easing).
    float GetMoveAccelerationTime() const { return m_MoveAccelerationTime; }
    float GetMaxDistance() const { return m_MaxDistance; }
    bool GetScrollWheelDollyEnabled() const { return m_ScrollWheelDollyEnabled; }
    bool GetScrollWheelDollyReversed() const { return m_ScrollWheelDollyReversed; }
    bool GetGridVisibleOnStartup() const { return m_GridVisibleOnStartup; }
    // Exact pick: clicking selects the entity actually under the cursor.
    // Off (default): clicking selects the model-instance root (Unity-style).
    bool GetExactPickMode() const { return m_ExactPickMode; }
    bool GetDirectionalLightGizmoDetailsOnlyOnSelection() const { return m_DirectionalLightGizmoDetailsOnlyOnSelection; }
    bool GetPointLightGizmoDetailsOnlyOnSelection() const { return m_PointLightGizmoDetailsOnlyOnSelection; }
    bool GetSpotLightGizmoDetailsOnlyOnSelection() const { return m_SpotLightGizmoDetailsOnlyOnSelection; }
    bool GetShowRotationGizmo() const { return m_ShowRotationGizmo; }
    bool GetShowRotationGizmoForViewport(size_t viewportIndex) const
    {
        return viewportIndex < m_ShowRotationGizmoInSplitViews.size()
            ? m_ShowRotationGizmoInSplitViews[viewportIndex]
            : true;
    }
    bool GetHoverNamePillEnabled() const { return m_HoverNamePillEnabled; }
    // When enabled, the hover name pill is placed beside the pointer while the pointer is
    // inside the scene viewport and a hover highlight is active (same entity as the outline).
    bool GetHoverNamePillNearCursor() const { return m_HoverNamePillNearCursor; }
    bool GetShowRulers() const { return m_ShowRulers; }
    bool GetShowGameUI() const { return m_ShowGameUI; }
    float GetRulerOpacity() const { return m_RulerOpacity; }
    uint32_t GetRulerIndicatorColor() const { return m_RulerIndicatorColor; }
    uint32_t GetMeasureColor() const { return m_MeasureColor; }
    float GetRulerIndicatorThickness() const { return m_RulerIndicatorThickness; }
    bool GetToolOverlayBorderless() const { return m_ToolOverlayBorderless; }
    HoverHighlightMode GetHoverHighlightMode() const { return m_HoverHighlightMode; }
    HoverHighlightModifier GetHoverHighlightModifier() const { return m_HoverHighlightModifier; }
    // Modifier key that, when held while hovering, reveals outline highlights
    // for hidden (disabled) objects and their descendants in the scene view.
    HoverHighlightModifier GetRevealHiddenHoverModifier() const { return m_RevealHiddenHoverModifier; }
    RotationGizmoCorner GetRotationGizmoCorner() const { return m_RotationGizmoCorner; }
    bool GetPixelPerfect2D() const { return m_PixelPerfect2D; }
    // Overlay the active game camera's visible frame as a blue line box in the
    // Scene View (rect in 2D/ortho, frustum in 3D/perspective). Defaults off.
    bool GetCameraFrameGuide() const { return m_CameraFrameGuide; }
    int GetPixelPerfectScale() const { return m_PixelPerfectScale; }
    bool GetPostProcessingEnabled() const { return m_PostProcessingEnabled; }
    bool GetPostFxBloomEnabled() const { return m_PostFxBloomEnabled; }
    // When false, Scene View forces a raw Linear tonemap (no curve, HDR hard-clips) while other post
    // settings follow the world — "tonemapping off" means off, not a different curve.
    bool GetPostFxTonemapEnabled() const { return m_PostFxTonemapEnabled; }
    bool GetPostFxColorFilterEnabled() const { return m_PostFxColorFilterEnabled; }
    bool GetPostFxCasEnabled() const { return m_PostFxCasEnabled; }
    bool GetPostFxCrtEnabled() const { return m_PostFxCrtEnabled; }
    // Scene View exposure mode: true = metered Auto, false = the fixed EV100 pin,
    // applied via the per-view post-process override.
    bool GetPostFxAutoExposureEnabled() const { return m_PostFxAutoExposureEnabled; }
    // When false, metered exposure snaps to the current target instead of using
    // the world sensor's eye-adaptation speeds.
    bool GetPostFxAutoExposureUseAdaptationDelay() const
    {
        return m_PostFxAutoExposureUseAdaptationDelay;
    }
    // EV100 the Scene View is pinned to while Auto Exposure is off.
    float GetPostFxFixedExposureEv() const { return m_PostFxFixedExposureEv; }
    // Auto-mode adaptation clamps for the Scene View's metering (EV100).
    float GetPostFxAutoExposureMinEv() const { return m_PostFxAutoExposureMinEv; }
    float GetPostFxAutoExposureMaxEv() const { return m_PostFxAutoExposureMaxEv; }
    // ± stops applied on top of the Scene View's exposure in BOTH modes: folds
    // into the metering key under Auto, multiplies the pinned EV's scale under
    // Fixed (the ResolveExposureScale convention — + brightens).
    float GetPostFxExposureCompensation() const { return m_PostFxExposureCompensation; }
    MarqueeShape GetMarqueeShape() const { return m_MarqueeShape; }
    uint32_t GetMarqueeColor() const { return m_MarqueeColor; }
    float    GetMarqueeThickness() const { return m_MarqueeThickness; }
    SelectionHighlightStyle GetSelectionHighlightStyle() const { return m_SelectionHighlightStyle; }
    uint32_t GetSelectionBoxColor() const { return m_SelectionBoxColor; }
    float    GetSelectionBoxThickness() const { return m_SelectionBoxThickness; }
    uint32_t GetSelectionOutlineColor() const { return m_SelectionOutlineColor; }
    float    GetSelectionOutlineThickness() const { return m_SelectionOutlineThickness; }
    bool GetSelectionOutlinesVisible() const { return m_SelectionOutlinesVisible; }
    // Color used to clear the Scene View when there is no SkyEnvironment
    // covering the background. Packed ARGB; alpha is ignored on output.
    uint32_t GetBackgroundColor() const { return m_BackgroundColor; }
    bool GetHdrTestPatternEnabled() const { return m_HdrTestPatternEnabled; }
    // Runtime only: set from the Enable HDR settings toggle (not persisted).
    bool GetHdrOutputEnabled() const { return m_HdrOutputEnabled; }
    // Viewport render-scale [0.25, 1.0]. 1.0 = native; <1 makes the pipeline
    // rasterize the world half at the reduced extent and upscale back to the
    // viewport extent internally. Published per view as pipeline state
    // (ViewRegistry::SetViewRenderScale) — the render target itself stays 1:1
    // with the viewport rect at every scale, so UI compositing never magnifies.
    float GetRenderScale() const { return m_RenderScale; }

    // Setters persist immediately by default; continuous edits (slider drags)
    // pass save=false per tick and commit with one saving call on release.
    void SetLookSensitivity(float value, bool save = true)
    {
        m_LookSensitivity = value;
        Validate();
        if (save)
            Save();
    }

    void SetInvertY(bool value)
    {
        m_InvertY = value;
        Save();
    }

    void SetNearClip(float value, bool save = true)
    {
        const float before = m_NearClip;
        m_NearClip = value;
        Validate();
        if (save)
            Save();
        if (m_NearClip != before)
            NotifyCameraSettingsChanged();
    }

    void SetFarClip(float value, bool save = true)
    {
        const float before = m_FarClip;
        m_FarClip = value;
        Validate();
        if (save)
            Save();
        if (m_FarClip != before)
            NotifyCameraSettingsChanged();
    }

    void SetFieldOfViewDeg(float value, bool save = true)
    {
        const float before = m_FieldOfViewDeg;
        m_FieldOfViewDeg = value;
        Validate();
        if (save)
            Save();
        if (m_FieldOfViewDeg != before)
            NotifyCameraSettingsChanged();
    }

    void SetMoveSpeed(float value, bool save = true)
    {
        const float before = m_MoveSpeed;
        m_MoveSpeed = value;
        Validate();
        if (save)
            Save();
        if (m_MoveSpeed != before)
            NotifyCameraSettingsChanged();
    }

    void SetFastMoveMultiplier(float value, bool save = true)
    {
        const float before = m_FastMoveMultiplier;
        m_FastMoveMultiplier = value;
        Validate();
        if (save)
            Save();
        if (m_FastMoveMultiplier != before)
            NotifyCameraSettingsChanged();
    }

    void SetMoveAccelerationTime(float value, bool save = true)
    {
        const float before = m_MoveAccelerationTime;
        m_MoveAccelerationTime = value;
        Validate();
        if (save)
            Save();
        if (m_MoveAccelerationTime != before)
            NotifyCameraSettingsChanged();
    }

    void SetMaxDistance(float value, bool save = true)
    {
        m_MaxDistance = value;
        Validate();
        if (save)
            Save();
    }

    void SetScrollWheelDollyEnabled(bool value)
    {
        m_ScrollWheelDollyEnabled = value;
        Save();
    }

    void SetScrollWheelDollyReversed(bool value)
    {
        m_ScrollWheelDollyReversed = value;
        Save();
    }

    void SetGridVisibleOnStartup(bool value)
    {
        m_GridVisibleOnStartup = value;
        Save();
    }

    void SetExactPickMode(bool value)
    {
        m_ExactPickMode = value;
        Save();
    }

    void SetDirectionalLightGizmoDetailsOnlyOnSelection(bool value)
    {
        m_DirectionalLightGizmoDetailsOnlyOnSelection = value;
        Save();
    }

    void SetPointLightGizmoDetailsOnlyOnSelection(bool value)
    {
        m_PointLightGizmoDetailsOnlyOnSelection = value;
        Save();
    }

    void SetSpotLightGizmoDetailsOnlyOnSelection(bool value)
    {
        m_SpotLightGizmoDetailsOnlyOnSelection = value;
        Save();
    }

    void SetShowRotationGizmo(bool value)
    {
        m_ShowRotationGizmo = value;
        Save();
    }

    void SetShowRotationGizmoForViewport(size_t viewportIndex, bool value)
    {
        if (viewportIndex >= m_ShowRotationGizmoInSplitViews.size())
            return;
        m_ShowRotationGizmoInSplitViews[viewportIndex] = value;
        Save();
    }

    void SetHoverNamePillEnabled(bool value)
    {
        m_HoverNamePillEnabled = value;
        Save();
    }

    void SetHoverNamePillNearCursor(bool value)
    {
        m_HoverNamePillNearCursor = value;
        Save();
    }

    void SetToolOverlayBorderless(bool value)
    {
        m_ToolOverlayBorderless = value;
        Save();
    }

    void SetHoverHighlightMode(HoverHighlightMode value)
    {
        m_HoverHighlightMode = value;
        Save();
    }

    void SetHoverHighlightModifier(HoverHighlightModifier value)
    {
        m_HoverHighlightModifier = value;
        Validate();
        Save();
    }

    void SetRevealHiddenHoverModifier(HoverHighlightModifier value)
    {
        m_RevealHiddenHoverModifier = value;
        Validate();
        Save();
    }

    void SetRotationGizmoCorner(RotationGizmoCorner value)
    {
        m_RotationGizmoCorner = value;
        Save();
    }

    void SetPixelPerfect2D(bool value)
    {
        m_PixelPerfect2D = value;
        Save();
    }

    void SetCameraFrameGuide(bool value)
    {
        m_CameraFrameGuide = value;
        Save();
    }

    void SetShowRulers(bool value)
    {
        m_ShowRulers = value;
        Save();
    }

    void SetShowGameUI(bool value)
    {
        m_ShowGameUI = value;
        Save();
    }

    void SetRulerOpacity(float value)
    {
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
        m_RulerOpacity = value;
        Save();
    }

    void SetRulerIndicatorColor(uint32_t value)
    {
        m_RulerIndicatorColor = value;
        Save();
    }

    void SetMeasureColor(uint32_t value)
    {
        m_MeasureColor = value;
        Save();
    }

    void SetRulerIndicatorThickness(float value)
    {
        if (value < 0.5f) value = 0.5f;
        if (value > 6.0f) value = 6.0f;
        m_RulerIndicatorThickness = value;
        Save();
    }

    void SetGridColor3D(uint32_t value) { m_GridColor3D = value; Save(); }
    void SetGridColor2D(uint32_t value) { m_GridColor2D = value; Save(); }
    uint32_t GetGridColor3D() const { return m_GridColor3D; }
    uint32_t GetGridColor2D() const { return m_GridColor2D; }

    void SetGridOpacity3D(float value)
    {
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
        m_GridOpacity3D = value;
        Save();
    }
    void SetGridOpacity2D(float value)
    {
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
        m_GridOpacity2D = value;
        Save();
    }
    float GetGridOpacity3D() const { return m_GridOpacity3D; }
    float GetGridOpacity2D() const { return m_GridOpacity2D; }

    void SetPixelPerfectScale(int value)
    {
        if (value < 1) value = 1;
        if (value > 64) value = 64;
        m_PixelPerfectScale = value;
        Save();
    }

    void SetPostProcessingEnabled(bool value)
    {
        m_PostProcessingEnabled = value;
        Save();
    }

    void SetPostFxBloomEnabled(bool value)
    {
        m_PostFxBloomEnabled = value;
        Save();
    }

    void SetPostFxColorFilterEnabled(bool value)
    {
        m_PostFxColorFilterEnabled = value;
        Save();
    }

    void SetPostFxCasEnabled(bool value)
    {
        m_PostFxCasEnabled = value;
        Save();
    }

    void SetPostFxCrtEnabled(bool value)
    {
        m_PostFxCrtEnabled = value;
        Save();
    }

    // fromUserToggle threads the caller's intent: only an in-session user
    // toggle may run the AE-lock handoff below. Pass false when applying
    // persisted or programmatic state — there the stored fixed EV is
    // authoritative and must not be overwritten by whatever the meter last
    // saw (a startup metering transient seeded this way pins a wildly wrong
    // EV and blacks out the viewport).
    void SetPostFxAutoExposureEnabled(bool value, bool fromUserToggle)
    {
        const bool beforeEnabled = m_PostFxAutoExposureEnabled;
        const float beforeFixedEv = m_PostFxFixedExposureEv;
        // Auto -> Fixed handoff (AE-lock): pin the fixed EV to what the meter
        // was showing so the image does not jump at the moment of the toggle.
        // The cached seed already folds compensation in (see
        // SetLastMeteredExposureEv); Validate() clamps it into slider range.
        // Quantized to the slider's detent in double + decimal grid, exactly
        // like Slider::QuantizeToStep: float round(ev/step)*step lands
        // off-detent (168 * 0.1f == 16.800001, not float("16.8")), which the
        // round-trip-exact edit display would surface and persist.
        if (fromUserToggle && m_PostFxAutoExposureEnabled && !value && m_LastMeteredExposureEvValid)
        {
            const double step = static_cast<double>(kExposureSliderStep);
            const double snapped = std::round(m_LastMeteredExposureEv / step) * step;
            m_PostFxFixedExposureEv = static_cast<float>(
                std::round(snapped * kEvSeedDecimalGrid) / kEvSeedDecimalGrid);
            m_LastMeteredExposureEvValid = false; // stale once metering stops
        }
        m_PostFxAutoExposureEnabled = value;
        Validate();
        Save();
        if (m_PostFxAutoExposureEnabled != beforeEnabled ||
            m_PostFxFixedExposureEv != beforeFixedEv)
            NotifyCameraSettingsChanged();
    }

    void SetPostFxAutoExposureUseAdaptationDelay(bool value)
    {
        if (m_PostFxAutoExposureUseAdaptationDelay == value)
            return;
        m_PostFxAutoExposureUseAdaptationDelay = value;
        Save();
        NotifyCameraSettingsChanged();
    }

    // Runtime only (not persisted): the EV100 that reproduces the Scene View's
    // current auto-metered image if pinned as Fixed EV100. SceneViewController
    // refreshes it every metered frame from the GPU exposure readback, with
    // compensation folded in so Fixed mode (which re-applies compensation)
    // resolves to the identical image. With several scene views the last
    // writer wins — this is a toggle-time convenience seed, not per-view state.
    void SetLastMeteredExposureEv(float ev)
    {
        // Finiteness is guaranteed by the one producer: the exposure readback
        // gates on the shader's valid flag and a finite positive scale.
        m_LastMeteredExposureEv = ev;
        m_LastMeteredExposureEvValid = true;
    }

    void SetPostFxFixedExposureEv(float value, bool save = true)
    {
        const float before = m_PostFxFixedExposureEv;
        m_PostFxFixedExposureEv = value;
        Validate();
        if (save)
            Save();
        if (m_PostFxFixedExposureEv != before)
            NotifyCameraSettingsChanged();
    }

    void SetPostFxAutoExposureMinEv(float value, bool save = true)
    {
        const float beforeMin = m_PostFxAutoExposureMinEv;
        const float beforeMax = m_PostFxAutoExposureMaxEv;
        m_PostFxAutoExposureMinEv = value;
        Validate();
        if (save)
            Save();
        if (m_PostFxAutoExposureMinEv != beforeMin || m_PostFxAutoExposureMaxEv != beforeMax)
            NotifyCameraSettingsChanged();
    }

    void SetPostFxAutoExposureMaxEv(float value, bool save = true)
    {
        const float beforeMin = m_PostFxAutoExposureMinEv;
        const float beforeMax = m_PostFxAutoExposureMaxEv;
        m_PostFxAutoExposureMaxEv = value;
        Validate();
        if (save)
            Save();
        if (m_PostFxAutoExposureMinEv != beforeMin || m_PostFxAutoExposureMaxEv != beforeMax)
            NotifyCameraSettingsChanged();
    }

    void SetPostFxExposureCompensation(float value, bool save = true)
    {
        const float before = m_PostFxExposureCompensation;
        m_PostFxExposureCompensation = value;
        Validate();
        if (save)
            Save();
        if (m_PostFxExposureCompensation != before)
            NotifyCameraSettingsChanged();
    }

    void SetMarqueeShape(MarqueeShape value)
    {
        m_MarqueeShape = value;
        Save();
    }

    void SetMarqueeColor(uint32_t value)
    {
        m_MarqueeColor = value;
        Save();
    }

    void SetMarqueeThickness(float value)
    {
        if (value < 0.5f) value = 0.5f;
        if (value > 10.0f) value = 10.0f;
        m_MarqueeThickness = value;
        Save();
    }

    void SetSelectionHighlightStyle(SelectionHighlightStyle value)
    {
        m_SelectionHighlightStyle = value;
        Save();
    }

    void SetSelectionBoxColor(uint32_t value)
    {
        m_SelectionBoxColor = value;
        Save();
    }

    void SetSelectionBoxThickness(float value)
    {
        if (value < 0.5f) value = 0.5f;
        if (value > 10.0f) value = 10.0f;
        m_SelectionBoxThickness = value;
        Save();
    }

    void SetSelectionOutlineColor(uint32_t value)
    {
        m_SelectionOutlineColor = value;
        Save();
    }

    void SetSelectionOutlineThickness(float value)
    {
        if (value < 0.25f) value = 0.25f;
        if (value > 5.0f) value = 5.0f;
        m_SelectionOutlineThickness = value;
        Save();
    }

    void SetSelectionOutlinesVisible(bool value)
    {
        m_SelectionOutlinesVisible = value;
        Save();
    }

    void SetBackgroundColor(uint32_t value)
    {
        m_BackgroundColor = value;
        Save();
    }

    void SetHdrTestPatternEnabled(bool value)
    {
        m_HdrTestPatternEnabled = value;
        Save();
    }

    void SetHdrOutputEnabled(bool value) { m_HdrOutputEnabled = value; }

    void SetRenderScale(float value, bool save = true)
    {
        if (value < 0.25f) value = 0.25f;
        if (value > 2.0f) value = 2.0f; // above 1.0 = supersampling (SSAA)
        m_RenderScale = value;
        if (save)
            Save();
    }

private:
    SceneViewSettings()
    {
        Load();
    }

    void Validate()
    {
        constexpr float kDefaultMaxDistance = 2000.0f;

        if (!(m_NearClip > 0.0f))
        {
            m_NearClip = kDefaultNearClip;
        }

        if (m_NearClip < kMinNearClip)
        {
            m_NearClip = kMinNearClip;
        }
        if (m_NearClip > kMaxNearClip)
        {
            m_NearClip = kMaxNearClip;
        }

        if (!(m_FarClip > m_NearClip))
        {
            m_FarClip = kDefaultFarClip;
        }

        const float minFar = m_NearClip * 2.0f;
        if (m_FarClip <= minFar)
        {
            m_FarClip = minFar;
        }
        if (m_FarClip > kMaxFarClip)
        {
            m_FarClip = kMaxFarClip;
        }

        // Field of view in degrees (keep within a sane range)
        if (!(m_FieldOfViewDeg > 0.0f))
        {
            m_FieldOfViewDeg = kDefaultFieldOfViewDeg;
        }
        if (m_FieldOfViewDeg < kMinFieldOfViewDeg)
        {
            m_FieldOfViewDeg = kMinFieldOfViewDeg;
        }
        if (m_FieldOfViewDeg > kMaxFieldOfViewDeg)
        {
            m_FieldOfViewDeg = kMaxFieldOfViewDeg;
        }

        // Movement speed (must be positive)
        if (!(m_MoveSpeed > 0.0f))
        {
            m_MoveSpeed = kDefaultMoveSpeed;
        }
        if (m_MoveSpeed < kMinMoveSpeed)
        {
            m_MoveSpeed = kMinMoveSpeed;
        }
        if (m_MoveSpeed > kMaxMoveSpeed)
        {
            m_MoveSpeed = kMaxMoveSpeed;
        }

        // Fast-move multiplier (>= 1)
        if (!(m_FastMoveMultiplier >= 1.0f))
        {
            m_FastMoveMultiplier = kDefaultFastMoveMultiplier;
        }
        if (m_FastMoveMultiplier < kMinFastMoveMultiplier)
        {
            m_FastMoveMultiplier = kMinFastMoveMultiplier;
        }
        if (m_FastMoveMultiplier > kMaxFastMoveMultiplier)
        {
            m_FastMoveMultiplier = kMaxFastMoveMultiplier;
        }

        // Look sensitivity (degrees per pixel, must be positive)
        if (!(m_LookSensitivity > 0.0f))
            m_LookSensitivity = 0.2f;
        if (m_LookSensitivity < 0.01f)
            m_LookSensitivity = 0.01f;
        if (m_LookSensitivity > 2.0f)
            m_LookSensitivity = 2.0f;

        // Fly-camera acceleration time constant (seconds; 0 = instant)
        if (!(m_MoveAccelerationTime >= 0.0f))
            m_MoveAccelerationTime = kDefaultMoveAccelerationTime;
        if (m_MoveAccelerationTime > kMaxMoveAccelerationTime)
            m_MoveAccelerationTime = kMaxMoveAccelerationTime;

        // Scene View exposure ranges (EV100 pin, auto adaptation clamps, ± stop trim).
        auto clampEv = [](float& ev, float fallback)
        {
            if (!std::isfinite(ev))
                ev = fallback;
            if (ev < kMinExposureEv)
                ev = kMinExposureEv;
            if (ev > kMaxExposureEv)
                ev = kMaxExposureEv;
        };
        clampEv(m_PostFxFixedExposureEv, Components::kDefaultManualExposureEv);
        clampEv(m_PostFxAutoExposureMinEv, kDefaultAutoExposureMinEv);
        clampEv(m_PostFxAutoExposureMaxEv, kDefaultAutoExposureMaxEv);
        if (m_PostFxAutoExposureMaxEv < m_PostFxAutoExposureMinEv)
            m_PostFxAutoExposureMaxEv = m_PostFxAutoExposureMinEv;
        if (!std::isfinite(m_PostFxExposureCompensation))
            m_PostFxExposureCompensation = 0.0f;
        if (m_PostFxExposureCompensation < -kMaxExposureCompensation)
            m_PostFxExposureCompensation = -kMaxExposureCompensation;
        if (m_PostFxExposureCompensation > kMaxExposureCompensation)
            m_PostFxExposureCompensation = kMaxExposureCompensation;

        // Max orbit distance (how far camera can be from pivot)
        if (!(m_MaxDistance > 0.0f))
        {
            m_MaxDistance = kDefaultMaxDistance;
        }
        if (m_MaxDistance < 10.0f)
        {
            m_MaxDistance = 10.0f;
        }
        if (m_MaxDistance > 100000.0f)
        {
            m_MaxDistance = 100000.0f;
        }

        const int modInt = static_cast<int>(m_HoverHighlightModifier);
        if (modInt < 0 || modInt > 2)
            m_HoverHighlightModifier = HoverHighlightModifier::Control;

        const int revealInt = static_cast<int>(m_RevealHiddenHoverModifier);
        if (revealInt < 0 || revealInt > 2)
            m_RevealHiddenHoverModifier = HoverHighlightModifier::Shift;

        // Render-scale clamp ([0.25, 2.0], above 1.0 = supersampling); guard
        // against corrupt/old prefs.
        if (!(m_RenderScale > 0.0f))
            m_RenderScale = 1.0f;
        if (m_RenderScale < 0.25f)
            m_RenderScale = 0.25f;
        if (m_RenderScale > 2.0f)
            m_RenderScale = 2.0f;
    }

    float m_LookSensitivity = 0.2f;
    bool  m_InvertY = false;
    void NotifyCameraSettingsChanged()
    {
        const auto listeners = m_CameraSettingsChangedListeners;
        for (const auto& [id, callback] : listeners)
        {
            (void)id;
            if (callback)
                callback();
        }
    }

    uint64_t m_NextCameraSettingsListenerId = 1;
    std::unordered_map<uint64_t, CameraSettingsChangedCallback> m_CameraSettingsChangedListeners;

    float m_NearClip = kDefaultNearClip;
    float m_FarClip = kDefaultFarClip;
    float m_FieldOfViewDeg = kDefaultFieldOfViewDeg;
    float m_MoveSpeed = kDefaultMoveSpeed;
    float m_FastMoveMultiplier = kDefaultFastMoveMultiplier;
    // Exponential time constant (seconds) easing fly-camera velocity toward the
    // input direction; 0 disables easing (instant response).
    float m_MoveAccelerationTime = kDefaultMoveAccelerationTime;
    float m_MaxDistance = 2000.0f;
    bool m_ScrollWheelDollyEnabled = true;
    bool m_ScrollWheelDollyReversed = true;
    bool m_GridVisibleOnStartup = false;
    bool m_ExactPickMode = false;
    bool m_DirectionalLightGizmoDetailsOnlyOnSelection = true;
    bool m_PointLightGizmoDetailsOnlyOnSelection = false;
    bool m_SpotLightGizmoDetailsOnlyOnSelection = false;
    bool m_ShowRotationGizmo = true;
    std::array<bool, 4> m_ShowRotationGizmoInSplitViews{true, true, true, true};
    bool m_HoverNamePillEnabled = true;
    bool m_HoverNamePillNearCursor = true;
    bool m_ShowRulers = true;
    bool m_ShowGameUI = true; // composite the world's UIDocument HUDs over the Scene View
    float m_RulerOpacity = 1.0f; // 0..1; multiplied into the ruler's primitive alpha
    // Cursor indicator (the warm line drawn on each ruler band where the
    // mouse is hovering). ARGB packed; default is the original warm red
    // (R=242 G=102 B=77 A=242 ≈ 0.95 * 255).
    uint32_t m_RulerIndicatorColor = 0xF2F2664Du;
    uint32_t m_MeasureColor = 0xFFFFC738u;
    float m_RulerIndicatorThickness = 2.0f;
    // Per-mode grid line color (non-axis lines). ARGB; alpha is currently
    // unused on the GPU side but preserved through prefs for future use.
    // Default 0xFF000000 keeps the legacy "darken-by-blend" look that the
    // shader produced before colors were configurable.
    uint32_t m_GridColor3D = 0xFF000000u;
    uint32_t m_GridColor2D = 0x7AFFFFFFu; // semi-transparent white default for 2D mode
    float m_GridOpacity3D = 0.3f;
    float m_GridOpacity2D = 0.15f;
    bool m_ToolOverlayBorderless = false;
    HoverHighlightMode m_HoverHighlightMode = HoverHighlightMode::WithModifier;
    HoverHighlightModifier m_HoverHighlightModifier = HoverHighlightModifier::Control;
    // Held to reveal outlines on hidden (disabled) objects while hovering.
    // Defaults to Shift so it's distinct from the Control hover-highlight modifier.
    HoverHighlightModifier m_RevealHiddenHoverModifier = HoverHighlightModifier::Shift;
    RotationGizmoCorner m_RotationGizmoCorner = RotationGizmoCorner::TopRight;
    bool m_PixelPerfect2D = false;
    bool m_CameraFrameGuide = false;
    int m_PixelPerfectScale = 2; // clamped to [1, 64], advanced by doubling/halving
    bool m_PostProcessingEnabled = true;
    // Per-effect toggles under the Post Processing master switch.
    bool m_PostFxBloomEnabled = true;
    bool m_PostFxTonemapEnabled = true;
    bool m_PostFxColorFilterEnabled = true;
    bool m_PostFxCasEnabled = true;
    bool m_PostFxCrtEnabled = true;
    bool m_PostFxAutoExposureEnabled = false;
    bool m_PostFxAutoExposureUseAdaptationDelay = true;
    // EV100 pin for the Scene View while Auto Exposure is off.
    float m_PostFxFixedExposureEv = Components::kDefaultManualExposureEv;
    // Auto-mode metering tune: adaptation clamps (EV100) + ± stop compensation.
    float m_PostFxAutoExposureMinEv = kDefaultAutoExposureMinEv;
    float m_PostFxAutoExposureMaxEv = kDefaultAutoExposureMaxEv;
    float m_PostFxExposureCompensation = 0.0f;
    // Runtime-only Auto->Fixed handoff seed (SetLastMeteredExposureEv); never persisted.
    float m_LastMeteredExposureEv = 0.0f;
    bool m_LastMeteredExposureEvValid = false;
    // Selection tool marquee drag-select appearance.
    MarqueeShape m_MarqueeShape      = MarqueeShape::Rectangle;
    uint32_t     m_MarqueeColor      = 0xFFFFFF33u; // ARGB, pale yellow
    float        m_MarqueeThickness  = 1.25f;

    // Selection / hover highlight style + appearance.
    SelectionHighlightStyle m_SelectionHighlightStyle = SelectionHighlightStyle::Outline;
    // ARGB packed colors. Default selection box: white at 50% alpha.
    uint32_t m_SelectionBoxColor      = 0x80FFFFFFu;
    float    m_SelectionBoxThickness  = 1.5f;
    // Default outline: warm orange at 60% alpha (0x99 = 153/255).
    uint32_t m_SelectionOutlineColor  = 0x99FFB300u;
    float    m_SelectionOutlineThickness = 3.0f; // outline radius in pixels
    bool     m_SelectionOutlinesVisible = true;

    // Default background: dark gray (matches the legacy hardcoded clear).
    uint32_t m_BackgroundColor = 0xFF1A1A1Au; // ARGB
    bool m_HdrTestPatternEnabled = false;
    bool m_HdrOutputEnabled = false;
    // Viewport render-scale: 1.0 = native resolution; <1.0 rasterizes the world
    // half at the reduced extent and crosses back to the viewport extent inside
    // the pipeline. Cheap GPU-side perf knob, especially on macOS at 2x Retina
    // where the pixel count is 4x logical. Clamped to [0.25, 1.0]; the clustered
    // light grid, depth min/max and the rest of the world half follow the reduced
    // extent, while overlays, gizmos and UI composite at full resolution.
    float m_RenderScale = 1.0f;
};

} // namespace Editor
} // namespace GameEngine
