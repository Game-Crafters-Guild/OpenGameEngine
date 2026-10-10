#pragma once

#include <string>
#include <vector>

#include "UI/Controls/BaseField.h"
#include "UI/UIEvents.h"

namespace GameEngine
{

// Material-inspired horizontal slider control supporting:
//  - Continuous or discrete values (via step)
//  - Optional range mode with two thumbs
//  - Optional centered rendering where 0 maps to the visual center
//
// The logical domain is [min, max]. In single-value mode GetValue() returns
// the current value. In range mode GetRangeStart()/GetValue() expose the
// lower/upper values respectively.
class Slider : public Field<float>
{
  public:
    using ValueType = float;

    Slider();

    // Domain configuration
    void SetMin(float min);
    float GetMin() const { return m_Min; }

    void SetMax(float max);
    float GetMax() const { return m_Max; }

    // Discrete step configuration. When step <= 0 the slider is continuous.
    void SetStep(float step);
    float GetStep() const { return m_Step; }

    // Optional tick mark rendering along the track. When enabled, the slider
    // will draw small ticks at discrete step positions (for step > 0).
    void SetShowTicks(bool show)
    {
        if (m_ShowTicks != show)
        {
            m_ShowTicks = show;
            MarkDirty(VisualDirty);
        }
    }
    bool GetShowTicks() const { return m_ShowTicks; }

    // Optional explicit tick marks (web range-input <datalist> style): small
    // lines drawn across the track at the given DOMAIN values, independent of
    // the step-based ShowTicks grid. Values outside [min, max] are skipped.
    void SetTickMarks(std::vector<float> values)
    {
        m_TickMarks = std::move(values);
        MarkDirty(VisualDirty);
    }

    // Optional value bubble above the active thumb. This is intended to be
    // shown while interacting (hover/drag/focus) and mirrors Material
    // sliders with value labels. Geometry is implemented in the .cpp.
    void SetShowValueBubble(bool show)
    {
        if (m_ShowValueBubble != show)
        {
            m_ShowValueBubble = show;
            MarkDirty(VisualDirty);
        }
    }
    bool GetShowValueBubble() const { return m_ShowValueBubble; }

    // Centered mode: when true, the active track is drawn from the logical 0
    // position toward the current value instead of from the domain minimum.
    void SetCentered(bool centered);
    bool GetCentered() const { return m_Centered; }

    // Range mode: when enabled the slider exposes two values within [min, max]
    // and draws an active segment between them.
    void SetRangeMode(bool isRange);
    bool IsRangeMode() const { return m_IsRange; }

    void SetRangeStart(float value);
    float GetRangeStart() const { return m_RangeStart; }

    // Convenience helper for configuring both ends of the range at once.
    void SetRangeValues(float start, float end);

    // Override SetValue to apply domain clamping, discrete quantization and
    // to fire value callbacks.
    void SetValue(const float& v) override;

    // Clamp and quantize like SetValue, but skip callbacks. Use for
    // programmatic updates that should not trigger user-facing handlers.
    void SetValueWithoutNotify(const float& v) override;

    // Vertical mode: track runs along Y; top = max value, bottom = min.
    void SetVertical(bool vertical);
    bool IsVertical() const { return m_Vertical; }

    /** Track padding (inset from element edges). When set >= 0, overrides the default (e.g. 0 for mixer faders). */
    void SetTrackPaddingPx(float px);
    /** The EFFECTIVE track inset (explicit override, else the default) — the
        value->x mapping companion UI (tick-label strips) must mirror. */
    float GetTrackPaddingPx() const
    {
        return m_TrackPaddingPx >= 0.0f ? m_TrackPaddingPx : kDefaultTrackPaddingPx;
    }

    /** Vertical track thickness in pixels. When set >= 0, overrides the default (e.g. 8 for mixer). */
    void SetVerticalTrackThicknessPx(float px);
    float GetVerticalTrackThicknessPx() const { return m_VerticalTrackThicknessPx; }

    // Keyboard interaction: left/right arrows nudge the value by one step (or
    // 5% of the domain when no explicit step is configured).
    bool OnKey(int key, int mods, UI::IPlatformApi* platform) override;

    // SDF primitive generation for track, active segment and thumb(s).
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                              const ResolvedStyle& style,
                              float x, float y, float W, float H) override;
    void OnPostLayout() override;

  protected:
    // Pointer interaction is routed via Field<T>::OnEvent; we implement the
    // hooks in terms of element-local geometry and ignore text metrics.
    void OnPointerDown(float mouseX, float mouseY,
                       float x, float y, float W, float H,
                       const ResolvedStyle& style,
                       Rendering::Text::FontAtlas* font) override;

    void OnPointerDrag(float mouseX, float mouseY,
                       float x, float y, float W, float H,
                       const ResolvedStyle& style,
                       Rendering::Text::FontAtlas* font) override;

    bool RequiresFontAtlasForPointerRouting() const override { return false; }

  private:
    static constexpr float kDefaultTrackPaddingPx = 12.0f;

    enum class ActiveThumb
    {
        None,
        Low,
        High
    };

    float GetDomainMin() const;
    float GetDomainMax() const;
    float ClampToDomain(float v) const;
    float QuantizeToStep(float v) const;
    void UpdateRangeInvariant();
    void UpdateThumbLayout();

    // Pointer drags bump the logical value continuously; callers like the material inspector
    // hook OnValueChanged to expensive persistence — defer that notification until mouse-up.
    void ApplyPrimaryAdjustmentFromPointer(float v);
    void TouchRangeLowFromPointer(float newLow);

    bool m_PendingCommittedValueNotifications = false;

    // Domain endpoints
    float m_Min = 0.0f;
    float m_Max = 1.0f;

    // Optional second value when operating in range mode; the Field<float>
    // base value represents the upper value.
    float m_RangeStart = 0.0f;

    // Discrete stepping; <= 0 means continuous.
    float m_Step = 0.0f;

    bool m_Centered = false;
    bool m_IsRange = false;
    ActiveThumb m_ActiveThumb = ActiveThumb::High;
    bool m_RangeDragSessionActive = false; // True after OnPointerDown for range sliders

    bool m_ShowTicks = false;
    bool m_ShowValueBubble = false;
    std::vector<float> m_TickMarks;

    bool m_Vertical = false;
    UIElement* m_LowThumb = nullptr;
    UIElement* m_HighThumb = nullptr;
    float m_TrackPaddingPx = -1.0f;  // < 0 = use kDefaultTrackPaddingPx (see GetTrackPaddingPx)
    float m_VerticalTrackThicknessPx = -1.0f;  // < 0 = use default kSliderVerticalTrackThicknessPx
};

} // namespace GameEngine
