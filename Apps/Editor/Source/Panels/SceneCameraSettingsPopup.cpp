#include "Panels/SceneCameraSettingsPopup.h"
#include "UI/Layout/ElementOverrideHelpers.h"

#include "Editor/Settings/SceneViewSettings.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/PopoverArrow.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/Toggle.h"
#include "UI/Layout/PopupPlacement.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace GameEngine
{

namespace
{
// Wide enough for legible tick labels under the exposure sliders.
constexpr float kPanelWidth = 360.0f;
// Keep the popup stable while exposure-mode rows are swapped.
constexpr float kPanelHeight = 464.0f;
constexpr float kPanelHeightEstimate = kPanelHeight;
constexpr float kEdgePadding = 8.0f;
constexpr float kAnchorGap = 10.0f;
constexpr float kArrowWidth = 20.0f;
constexpr float kArrowHeight = 10.0f;
// The arrow's base overlaps the panel edge so the two read as one shape.
constexpr float kArrowPanelOverlap = 1.0f;
// Keep the arrow clear of the panel's rounded corners.
constexpr float kArrowCornerMargin = 10.0f;
// Above panel content; matches the other editor overlay popups (ColorPickerPopup).
constexpr int kPopupZIndex = 10000;

// Anchors each child label's CENTER at the same track x its paired Slider
// maps the tick value to (effective pad + t * (W - 2*pad), i.e. the
// thumb-travel geometry, queried live from the slider). A percent-positioned
// label ignores the track inset and drifts by up to the pad width near the
// ends. Positioned post-layout like the Slider's thumbs, so labels stay put
// through resizes without triggering relayout.
class TickLabelStrip : public UIElement
{
  public:
    explicit TickLabelStrip(Slider* slider) : m_Slider(slider) {}

    void AddTick(float value, const char* text)
    {
        auto label = std::make_unique<Label>();
        label->SetText(text);
        label->AddClass("popover-tick-label");
        label->Overrides()
            .Set(Style::Position, PositionType::Absolute)
            .Set(Style::PointerEvents, false);
        m_Ticks.push_back({label.get(), value});
        AddChild(std::move(label));
    }

    void OnPostLayout() override
    {
        if (!m_Slider)
            return;
        const float W = GetLayoutWidth();
        const float minValue = m_Slider->GetMin();
        const float range = m_Slider->GetMax() - minValue;
        if (W <= 0.0f || range <= 0.0f)
            return;
        const float pad = m_Slider->GetTrackPaddingPx();
        const float trackW = std::max(0.0f, W - 2.0f * pad);
        for (const TickEntry& tick : m_Ticks)
        {
            if (!tick.Text)
                continue;
            const float t = (tick.Value - minValue) / range;
            if (t < 0.0f || t > 1.0f)
            {
                // Match the slider: out-of-domain marks are skipped, so the
                // label collapses rather than pinning to a track end with no
                // tick line above it.
                UI::Layout::SetElementHidden(*tick.Text, true);
                continue;
            }
            UI::Layout::SetElementHidden(*tick.Text, false);
            const float labelW = tick.Text->GetLayoutWidth();
            const float labelH = tick.Text->GetLayoutHeight();
            const float localX = pad + trackW * t - labelW * 0.5f;
            // Not a virtualization mover, so the position-only fast path's
            // post-solve patch never covers these; full layout dirt keeps the
            // committed rect in sync (ticks move rarely, the cost is noise).
            UI::Layout::SetAbsolutePosition(*tick.Text,
                                            Mathematics::Rect{localX, 0.0f, labelW, labelH},
                                            /*positionOnlyFastPath=*/false);
        }
    }

  private:
    struct TickEntry
    {
        Label* Text = nullptr;
        float Value = 0.0f;
    };
    Slider* m_Slider = nullptr;
    std::vector<TickEntry> m_Ticks;
};
constexpr const char* kCameraSectionTooltip =
    "Editor-only projection settings; game Camera components are unchanged.";
constexpr const char* kFovTooltip =
    "Vertical field of view in degrees.";
constexpr const char* kNearClipTooltip =
    "Near clipping plane distance in world units.";
constexpr const char* kFarClipTooltip =
    "Far clipping plane distance in world units.";

constexpr const char* kNavigationSectionTooltip =
    "Editor-only Scene View fly-navigation settings.";
constexpr const char* kMoveSpeedTooltip =
    "Fly-camera speed in world units per second.";
constexpr const char* kFastMultiplierTooltip =
    "Move Speed multiplier while Move Faster is held (Left Shift by default).";
constexpr const char* kAccelerationTooltip =
    "Fly-camera velocity smoothing time in seconds (0 = instant).";

constexpr const char* kExposureSectionTooltip =
    "Editor-only Scene View exposure override.";
constexpr const char* kAutoExposureTooltip =
    "Histogram metering with eye adaptation. Disable to use Fixed EV100.";
constexpr const char* kAdaptationDelayTooltip =
    "Smooth exposure changes over time using the sensor's adaptation speeds. "
    "Disable to apply each newly metered exposure immediately.";
constexpr const char* kMinEvTooltip =
    "Auto mode: brightest the metering may expose to (lower EV = more brightening in dark scenes).";
constexpr const char* kMaxEvTooltip =
    "Auto mode: darkest the metering may expose to (higher EV = more darkening in bright scenes).";
constexpr const char* kCompensationTooltip =
    "+/- stops applied after auto metering (+ brightens).";
constexpr const char* kFixedEvTooltip =
    "Absolute photographic EV100. Sunny ~15, overcast ~12, interior ~7-9, night ~2-5.";
} // namespace

SceneCameraSettingsPopup::SceneCameraSettingsPopup()
    : DismissablePopup(this)
{
    // Escape the Scene View dock leaf's clipping/stacking context so the popup
    // stays above adjacent panels when the Scene View becomes smaller than it.
    SetOverlayLayer(OverlayLayer::Dropdown);

    // Zero-size anchor pinned to the Scene View's origin: it exists to give the
    // panel a coordinate space, not to cover anything. A zero-area box still
    // renders its absolutely-positioned children as long as it does not clip
    // them, and staying zero-area is what keeps a press in the viewport
    // *outside* the popup — the dismissal gate tests tree ancestry, so a
    // full-size box here would swallow every press as "inside".
    Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(0.0f)).Set(Style::PositionTop, StyleLength::Px(0.0f))
        .Set(Style::Width, StyleLength::Px(0.0f)).Set(Style::Height, StyleLength::Px(0.0f))
        .Set(Style::ZIndex, kPopupZIndex)
        .Set(Style::Display, DisplayMode::None)
        .Set(Style::PointerEvents, false);

    // Floating panel with the controls. Surface styling (colors, border,
    // shadow, spacing) comes from the shared .popover-panel theme class so it
    // stays in lockstep with the rest of the editor chrome; only geometry and
    // behavior are set here.
    auto panel = std::make_unique<UIElement>();
    panel->SetId("scene-camera-settings-panel");
    panel->AddClass("popover-panel");
    panel->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::Width, StyleLength::Px(kPanelWidth))
        .Set(Style::Height, StyleLength::Px(kPanelHeight))
        .Set(Style::PointerEvents, true);
    m_Panel = panel.get();

    using GameEngine::Editor::SceneViewSettings;

    AddSectionHeader(*panel, "Scene View Camera", kCameraSectionTooltip);
    m_FovRow = AddSliderRow(*panel, "SceneCameraFov", "Field of View",
                            SceneViewSettings::kMinFieldOfViewDeg, SceneViewSettings::kMaxFieldOfViewDeg, 1.0f,
                            [](float v, bool commit) { SceneViewSettings::Get().SetFieldOfViewDeg(v, commit); },
                            SceneViewSettings::kDefaultFieldOfViewDeg, kFovTooltip);
    m_NearRow = AddSliderRow(*panel, "SceneCameraNearClip", "Near Clip",
                             SceneViewSettings::kMinNearClip, SceneViewSettings::kMaxNearClip, 0.01f,
                             [](float v, bool commit) { SceneViewSettings::Get().SetNearClip(v, commit); },
                             SceneViewSettings::kDefaultNearClip, kNearClipTooltip);
    m_FarRow = AddSliderRow(*panel, "SceneCameraFarClip", "Far Clip",
                            SceneViewSettings::kMinFarClip, SceneViewSettings::kMaxFarClip, 10.0f,
                            [](float v, bool commit) { SceneViewSettings::Get().SetFarClip(v, commit); },
                            SceneViewSettings::kDefaultFarClip, kFarClipTooltip);

    AddSectionHeader(*panel, "Navigation", kNavigationSectionTooltip);
    m_SpeedRow = AddSliderRow(*panel, "SceneCameraMoveSpeed", "Move Speed",
                              SceneViewSettings::kMinMoveSpeed, SceneViewSettings::kMaxMoveSpeed, 0.1f,
                              [](float v, bool commit) { SceneViewSettings::Get().SetMoveSpeed(v, commit); },
                              SceneViewSettings::kDefaultMoveSpeed, kMoveSpeedTooltip);
    m_FastMultiplierRow = AddSliderRow(*panel, "SceneCameraFastMultiplier", "Fast Multiplier",
                                       SceneViewSettings::kMinFastMoveMultiplier,
                                       SceneViewSettings::kMaxFastMoveMultiplier, 0.5f,
                                       [](float v, bool commit) { SceneViewSettings::Get().SetFastMoveMultiplier(v, commit); },
                                       SceneViewSettings::kDefaultFastMoveMultiplier, kFastMultiplierTooltip);
    m_AccelerationRow = AddSliderRow(*panel, "SceneCameraAcceleration", "Acceleration",
                                     0.0f, SceneViewSettings::kMaxMoveAccelerationTime, 0.01f,
                                     [](float v, bool commit) { SceneViewSettings::Get().SetMoveAccelerationTime(v, commit); },
                                     SceneViewSettings::kDefaultMoveAccelerationTime, kAccelerationTooltip);

    AddSectionHeader(*panel, "Exposure", kExposureSectionTooltip);
    {
        auto row = std::make_unique<UIElement>();
        row->SetId("SceneCameraAutoExposureRow");
        row->AddClass("popover-row");
        row->SetTooltip(kAutoExposureTooltip);

        auto label = std::make_unique<Label>();
        label->SetText("Auto Exposure");
        label->AddClass("popover-row-label");
        Label* labelPtr = label.get();
        row->AddChild(std::move(label));

        auto control = std::make_unique<UIElement>();
        control->AddClass("popover-row-toggle");

        auto autoExposure = std::make_unique<Toggle>();
        autoExposure->SetId("SceneCameraAutoExposure");
        autoExposure->SetOnValueChanged([this](const bool& enabled)
                                        {
                                            Editor::SceneViewSettings::Get().SetPostFxAutoExposureEnabled(
                                                enabled, /*fromUserToggle=*/true);
                                            // Full re-sync, not just visibility: switching auto off
                                            // seeds Fixed EV100 from the metered exposure (AE-lock),
                                            // so the fixed-EV slider must pick up the new value.
                                            SyncFromSettings();
                                        });
        m_AutoExposureToggle = autoExposure.get();
        InspectorDrag::SetupLabelDragToggle(labelPtr, m_AutoExposureToggle);
        control->AddChild(std::move(autoExposure));
        row->AddChild(std::move(control));
        panel->AddChild(std::move(row));
    }
    {
        auto row = std::make_unique<UIElement>();
        row->SetId("SceneCameraUseAdaptationDelayRow");
        row->AddClass("popover-row");
        row->SetTooltip(kAdaptationDelayTooltip);

        auto label = std::make_unique<Label>();
        label->SetText("Adaptation Delay");
        label->AddClass("popover-row-label");
        Label* labelPtr = label.get();
        row->AddChild(std::move(label));

        auto control = std::make_unique<UIElement>();
        control->AddClass("popover-row-toggle");

        auto useAdaptationDelay = std::make_unique<Toggle>();
        useAdaptationDelay->SetId("SceneCameraUseAdaptationDelay");
        useAdaptationDelay->SetOnValueChanged([](const bool& enabled)
                                             {
                                                 Editor::SceneViewSettings::Get()
                                                     .SetPostFxAutoExposureUseAdaptationDelay(enabled);
                                             });
        m_UseAdaptationDelayToggle = useAdaptationDelay.get();
        InspectorDrag::SetupLabelDragToggle(labelPtr, m_UseAdaptationDelayToggle);
        control->AddChild(std::move(useAdaptationDelay));
        row->AddChild(std::move(control));
        m_AdaptationDelayRow = row.get();
        panel->AddChild(std::move(row));
    }
    m_AutoMinEvRow = AddSliderRow(*panel, "SceneCameraAutoMinEv", "Min EV100",
                                  SceneViewSettings::kMinExposureEv, SceneViewSettings::kMaxExposureEv,
                                  SceneViewSettings::kExposureSliderStep,
                                  [](float v, bool commit) { SceneViewSettings::Get().SetPostFxAutoExposureMinEv(v, commit); },
                                  SceneViewSettings::kDefaultAutoExposureMinEv, kMinEvTooltip);
    m_AutoMaxEvRow = AddSliderRow(*panel, "SceneCameraAutoMaxEv", "Max EV100",
                                  SceneViewSettings::kMinExposureEv, SceneViewSettings::kMaxExposureEv,
                                  SceneViewSettings::kExposureSliderStep,
                                  [](float v, bool commit) { SceneViewSettings::Get().SetPostFxAutoExposureMaxEv(v, commit); },
                                  SceneViewSettings::kDefaultAutoExposureMaxEv, kMaxEvTooltip);
    m_ExposureEvRow = AddSliderRow(*panel, "SceneCameraFixedEv", "Fixed EV100",
                                   SceneViewSettings::kMinExposureEv, SceneViewSettings::kMaxExposureEv,
                                   SceneViewSettings::kExposureSliderStep,
                                   [](float v, bool commit) { SceneViewSettings::Get().SetPostFxFixedExposureEv(v, commit); },
                                   Components::kDefaultManualExposureEv, kFixedEvTooltip);
    // Real-world EV100 anchors: tick lines on the track, named by the legend
    // beneath (per-tick numbers would crowd 12 and 15). The value reads "how
    // bright a scene the camera is set for" (photographic convention — higher
    // EV = darker image), so the anchors + legend teach the scale in place.
    if (m_ExposureEvRow.slider)
        m_ExposureEvRow.slider->SetTickMarks({3.0f, 8.0f, 12.0f, 15.0f});
    m_ExposureEvLegend = AddCaption(*panel, "SceneCameraFixedEvLegend",
                                    "3 night · 8 interior · 12 overcast · 15 sun");
    m_CompensationRow = AddSliderRow(*panel, "SceneCameraExposureComp", "EV Compensation",
                                     -SceneViewSettings::kMaxExposureCompensation,
                                     SceneViewSettings::kMaxExposureCompensation,
                                     SceneViewSettings::kExposureSliderStep,
                                     [](float v, bool commit) { SceneViewSettings::Get().SetPostFxExposureCompensation(v, commit); },
                                     0.0f, kCompensationTooltip);
    // The relative knob (camera ±EV dial / URP Post Exposure convention):
    // 0-centered ± stops, + brightens, active in both exposure modes.
    if (m_CompensationRow.slider)
        m_CompensationRow.slider->SetCentered(true);
    AddTickStrip(*panel, "SceneCameraExposureComp", m_CompensationRow.slider,
                 {{-4.0f, "-4"}, {-2.0f, "-2"}, {0.0f, "0"}, {2.0f, "+2"}, {4.0f, "+4"}});

    // Anchor arrow pointing up at the toolbar button; a child of the panel (not
    // a sibling) so a press on it counts as inside the popup — the dismissal
    // gate tests tree ancestry, and the arrow is drawn outside the panel box.
    // Added last so it draws over the panel's top border for a joined look.
    auto arrow = std::make_unique<PopoverArrow>();
    arrow->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionTop, StyleLength::Px(-kArrowHeight + kArrowPanelOverlap))
        .Set(Style::Width, StyleLength::Px(kArrowWidth))
        .Set(Style::Height, StyleLength::Px(kArrowHeight));
    m_Arrow = arrow.get();
    panel->AddChild(std::move(arrow));

    AddChild(std::move(panel));

    m_SettingsChangedListenerId = SceneViewSettings::Get().AddCameraSettingsChangedListener([this]()
    {
        if (m_Visible)
            SyncFromSettings();
    });
}

SceneCameraSettingsPopup::~SceneCameraSettingsPopup()
{
    Editor::SceneViewSettings::Get().RemoveCameraSettingsChangedListener(m_SettingsChangedListenerId);
}

SceneCameraSettingsPopup::SliderRow SceneCameraSettingsPopup::AddSliderRow(
    UIElement& parent, const char* idPrefix, const char* labelText, float minValue,
    float maxValue, float step, std::function<void(float value, bool commit)> onChanged,
    float defaultValue, const char* tooltip)
{
    SliderRow result;

    auto row = std::make_unique<UIElement>();
    row->SetId(std::string(idPrefix) + "Row");
    row->AddClass("popover-row");
    row->SetTooltip(tooltip ? tooltip : "");
    result.row = row.get();

    auto label = std::make_unique<Label>();
    label->SetText(labelText);
    label->AddClass("popover-row-label");
    result.label = label.get();
    row->AddChild(std::move(label));

    auto slider = std::make_unique<Slider>();
    slider->SetId(std::string(idPrefix) + "Slider");
    slider->SetMin(minValue);
    slider->SetMax(maxValue);
    slider->SetStep(step);
    // The popup's control column already supplies spacing around the slider.
    // Remove the control's default 12 px track inset so the visible track
    // begins at the same x-position as the Auto Exposure toggle.
    slider->SetTrackPaddingPx(0.0f);
    // The theme's .slider class has no intrinsic size; the shared
    // .property-slider class supplies the height/grow.
    slider->AddClass("property-slider");
    result.slider = slider.get();

    auto field = std::make_unique<FloatField>();
    field->SetId(std::string(idPrefix) + "Field");
    field->AddClass("popover-row-value");
    result.field = field.get();

    Slider* sliderPtr = result.slider;
    FloatField* fieldPtr = result.field;

    // Slider drives the field live; the controllers re-read SceneViewSettings
    // every frame, so drag ticks preview without saving and the release/field
    // commit persists once.
    auto apply = [onChanged = std::move(onChanged), fieldPtr, sliderPtr, minValue, maxValue](float value, bool commit)
    {
        const float clamped = std::clamp(value, minValue, maxValue);
        sliderPtr->SetValueWithoutNotify(clamped);
        fieldPtr->SetValueWithoutNotify(clamped);
        if (onChanged)
            onChanged(clamped, commit);
    };
    slider->SetOnValueChanging([apply](const float& value) { apply(value, false); });
    slider->SetOnValueChanged([apply](const float& value) { apply(value, true); });
    field->SetOnValueChanged([apply](const float& value) { apply(value, true); });

    // Match inspector behavior: double-clicking a setting label restores its
    // default, updating both controls and persisting the reset through apply.
    InspectorDrag::SetupLabelDragSlider(result.label, result.slider, nullptr, nullptr, defaultValue);

    row->AddChild(std::move(slider));
    row->AddChild(std::move(field));
    parent.AddChild(std::move(row));
    return result;
}

void SceneCameraSettingsPopup::AddSectionHeader(UIElement& parent, const char* text,
                                                const char* tooltip)
{
    auto header = std::make_unique<Label>();
    header->SetText(text);
    header->AddClass("popover-section-header");
    header->SetTooltip(tooltip ? tooltip : "");
    parent.AddChild(std::move(header));
}

UIElement* SceneCameraSettingsPopup::AddTickStrip(UIElement& parent, const char* idPrefix,
                                                  Slider* slider,
                                                  std::initializer_list<TickMark> ticks)
{
    // One tick list drives both halves: lines on the slider's track and the
    // matching labels beneath it.
    if (slider)
    {
        std::vector<float> values;
        values.reserve(ticks.size());
        for (const TickMark& tick : ticks)
            values.push_back(tick.Value);
        slider->SetTickMarks(std::move(values));
    }

    auto row = std::make_unique<UIElement>();
    row->SetId(std::string(idPrefix) + "Ticks");
    row->AddClass("popover-tick-row");

    // Spacer columns mirror the slider row's label/value widths so the strip
    // occupies exactly the slider's box; TickLabelStrip then applies the
    // slider's own track inset so labels land under the thumb positions.
    auto leftSpacer = std::make_unique<UIElement>();
    leftSpacer->AddClass("popover-row-label");
    row->AddChild(std::move(leftSpacer));

    auto strip = std::make_unique<TickLabelStrip>(slider);
    strip->AddClass("popover-row-slider");
    strip->AddClass("popover-tick-strip");
    for (const TickMark& tick : ticks)
        strip->AddTick(tick.Value, tick.Label);
    row->AddChild(std::move(strip));

    auto rightSpacer = std::make_unique<UIElement>();
    rightSpacer->AddClass("popover-row-value");
    row->AddChild(std::move(rightSpacer));

    UIElement* result = row.get();
    parent.AddChild(std::move(row));
    return result;
}

UIElement* SceneCameraSettingsPopup::AddCaption(UIElement& parent, const char* id, const char* text)
{
    // Mirrors the tick rows' LEFT spacer column so the caption starts at the
    // slider column; no right spacer, deliberately — the caption may run
    // under the value column rather than wrap.
    auto row = std::make_unique<UIElement>();
    row->SetId(id);
    row->AddClass("popover-tick-row");

    auto leftSpacer = std::make_unique<UIElement>();
    leftSpacer->AddClass("popover-row-label");
    row->AddChild(std::move(leftSpacer));

    auto caption = std::make_unique<Label>();
    caption->SetText(text);
    caption->AddClass("popover-tick-legend");
    row->AddChild(std::move(caption));

    UIElement* result = row.get();
    parent.AddChild(std::move(row));
    return result;
}

void SceneCameraSettingsPopup::SyncFromSettings()
{
    using GameEngine::Editor::SceneViewSettings;
    const SceneViewSettings& settings = SceneViewSettings::Get();

    auto sync = [](const SliderRow& row, float value)
    {
        if (row.slider)
            row.slider->SetValueWithoutNotify(value);
        if (row.field)
            row.field->SetValueWithoutNotify(value);
    };
    sync(m_FovRow, settings.GetFieldOfViewDeg());
    sync(m_NearRow, settings.GetNearClip());
    sync(m_FarRow, settings.GetFarClip());
    sync(m_SpeedRow, settings.GetMoveSpeed());
    sync(m_FastMultiplierRow, settings.GetFastMoveMultiplier());
    sync(m_AccelerationRow, settings.GetMoveAccelerationTime());
    sync(m_ExposureEvRow, settings.GetPostFxFixedExposureEv());
    sync(m_AutoMinEvRow, settings.GetPostFxAutoExposureMinEv());
    sync(m_AutoMaxEvRow, settings.GetPostFxAutoExposureMaxEv());
    sync(m_CompensationRow, settings.GetPostFxExposureCompensation());
    if (m_AutoExposureToggle)
        m_AutoExposureToggle->SetValueWithoutNotify(settings.GetPostFxAutoExposureEnabled());
    if (m_UseAdaptationDelayToggle)
        m_UseAdaptationDelayToggle->SetValueWithoutNotify(
            settings.GetPostFxAutoExposureUseAdaptationDelay());
    UpdateExposureRowVisibility();
}

void SceneCameraSettingsPopup::UpdateExposureRowVisibility()
{
    const bool autoOn = Editor::SceneViewSettings::Get().GetPostFxAutoExposureEnabled();
    auto setVisible = [](UIElement* el, bool visible)
    {
        if (el)
            el->Overrides().Set(Style::Display,
                                visible ? DisplayMode::Flex : DisplayMode::None);
    };
    setVisible(m_AdaptationDelayRow, autoOn);
    setVisible(m_AutoMinEvRow.row, autoOn);
    setVisible(m_AutoMaxEvRow.row, autoOn);
    setVisible(m_ExposureEvRow.row, !autoOn);
    setVisible(m_ExposureEvLegend, !autoOn);
    // Compensation (row + ticks) stays visible in both modes: it trims the
    // auto metering and the fixed EV pin alike.
}

void SceneCameraSettingsPopup::ShowAt(float anchorCenterX, float anchorBottomY,
                                      float parentWidth, float parentHeight)
{
    SyncFromSettings();
    m_Visible = true;
    m_HasPlacement = false;
    UpdatePlacement(anchorCenterX, anchorBottomY, parentWidth, parentHeight);

    Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::PointerEvents, true);
}

void SceneCameraSettingsPopup::UpdatePlacement(float anchorCenterX, float anchorBottomY,
                                                float parentWidth, float parentHeight)
{
    if (!m_Visible)
        return;

    constexpr float kPlacementEpsilon = 0.25f;
    const auto changed = [](float a, float b)
    {
        return std::fabs(a - b) > kPlacementEpsilon;
    };
    if (m_HasPlacement &&
        !changed(anchorCenterX, m_LastAnchorCenterX) &&
        !changed(anchorBottomY, m_LastAnchorBottomY) &&
        !changed(parentWidth, m_LastParentWidth) &&
        !changed(parentHeight, m_LastParentHeight))
    {
        return;
    }

    m_HasPlacement = true;
    m_LastAnchorCenterX = anchorCenterX;
    m_LastAnchorBottomY = anchorBottomY;
    m_LastParentWidth = parentWidth;
    m_LastParentHeight = parentHeight;

    // Clamp into the Scene View, which is this popup's parent — the panel never
    // leaves the panel it belongs to, even though dismissal is window-wide.
    const UI::Layout::PopupRect bounds{0.0f, 0.0f, parentWidth, parentHeight};
    const auto placed = UI::Layout::ClampPopupToViewport(
        bounds,
        anchorCenterX - kPanelWidth * 0.5f,
        anchorBottomY + kAnchorGap + kArrowHeight,
        kPanelWidth, kPanelHeightEstimate, kEdgePadding);
    const float posX = placed.X;
    const float posY = placed.Y;

    // The arrow tracks the anchor even when the panel gets clamped, but stays
    // clear of the rounded corners. Panel-relative: it is a child of the panel.
    const float arrowX = std::clamp(anchorCenterX - kArrowWidth * 0.5f,
                                    posX + kArrowCornerMargin,
                                    posX + kPanelWidth - kArrowWidth - kArrowCornerMargin) - posX;

    m_Panel->Overrides()
        .Set(Style::PositionLeft, StyleLength::Px(posX))
        .Set(Style::PositionTop, StyleLength::Px(posY));
    if (m_Arrow)
        m_Arrow->Overrides().Set(Style::PositionLeft, StyleLength::Px(arrowX));

    // Overlay descendants need an explicit invalidation while their dock
    // ancestor is being interactively resized.
    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void SceneCameraSettingsPopup::Hide()
{
    m_Visible = false;
    m_HasPlacement = false;
    Overrides()
        .Set(Style::Display, DisplayMode::None)
        .Set(Style::PointerEvents, false);
}

} // namespace GameEngine
