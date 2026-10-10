#include "UI/Controls/Slider.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "Input/KeyCodes.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"
#include "UI/UIPrimitive.h"
#include "UI/UIStyle.h"
#ifdef _DEBUG
#include "Logger/Logger.h"
#endif

namespace GameEngine
{

using namespace Rendering::Geometry;

namespace
{
// Visual thickness of the track bar (horizontal).
constexpr float kSliderTrackThicknessPx = 4.0f;
// Vertical slider: wider rail so the track is clearly visible (e.g. mixer faders).
constexpr float kSliderVerticalTrackThicknessPx = 10.0f;
// Nominal thumb radius in pixels; actual radius is clamped to element size.
constexpr float kSliderThumbRadiusPx = 4.0f;
// Decimal grid for step snapping: quantized values land on the float nearest
// the decimal the step describes (float lo + n*step accumulation would store
// -0.099999905 for a -5..5 / 0.1 grid's -0.1 detent, which honest round-trip
// display then surfaces). Steps at or below the grid's resolution skip the
// snap — the grid would swallow them.
constexpr double kStepSnapDecimalGrid = 1e6;

// The track is painted here, not by a child element, so its colours are custom properties the
// slider reads from its resolved style: a theme sets them on the slider or on any ancestor. The UI
// module's default stylesheet (Assets/defaults.css) gives them their default values on :root.
const StringId kTrackFillVar = HashStringId("--slider-track-fill");
const StringId kTrackVar = HashStringId("--slider-track");
const StringId kTickVar = HashStringId("--slider-tick");

// An unset or unreadable colour property draws nothing.
uint32_t TrackColor(const ResolvedStyle& style, StringId var)
{
    return UI::PackFromARGB(style.GetCustomColor(var).value_or(0u));
}

// The override write keeps the Yoga style in sync and raises layout dirt, so
// the next solve lands the thumb at this same rect (the web editor renders
// from the solve). The direct rect commit makes the move visible NOW: quiet
// pointer frames run no solve — only the render drain — and a dragged thumb
// would otherwise paint at its stale rect until the next real layout pass.
void CommitThumbRect(UIElement* thumb, float absX, float absY, float w, float h,
                     float parentX, float parentY)
{
    // Pure moves take the position-only fast path: layout dirt here converts
    // every drag/scroll tick into a full Yoga solve, which is exactly what
    // drain-only frames exist to avoid. The stale window the fast path used to
    // leak (its post-solve patch never runs on quiet frames) is closed by the
    // direct rect commit below, not by forcing solves.
    const bool sizeChanged =
        thumb->GetLayoutWidth() != w || thumb->GetLayoutHeight() != h;
    UI::Layout::SetAbsolutePosition(*thumb,
                                    Mathematics::Rect{absX - parentX, absY - parentY, w, h},
                                    /*positionOnlyFastPath=*/!sizeChanged);
    if (thumb->GetLayoutX() == absX && thumb->GetLayoutY() == absY &&
        thumb->GetLayoutWidth() == w && thumb->GetLayoutHeight() == h)
        return;
    UILayoutAccess::SetLastLayoutRect(*thumb, absX, absY, w, h);
    thumb->MarkDirty(UIElement::VisualDirty);
}
} // namespace

#ifdef _DEBUG
// Enable very lightweight debugging of range-slider hit-testing for the
// demo range slider only. This helps diagnose which thumb is being selected
// on click without spamming logs from every slider instance.
static bool kSliderRangeDebugConsole = true;
#endif

Slider::Slider()
{
    // Sliders participate in keyboard focus navigation so arrow keys work.
    SetFocusable(true);
    AddClass("slider");

    auto createThumb = [this](const char* thumbClass) -> UIElement*
    {
        auto thumb = std::make_unique<UIElement>();
        UIElement* thumbPtr = thumb.get();
        thumbPtr->AddClass("slider-thumb");
        thumbPtr->AddClass(thumbClass);
        thumbPtr->Overrides()
            .Set(Style::Position, PositionType::Absolute)
            .Set(Style::FlexGrow, 0.0f)
            .Set(Style::FlexShrink, 0.0f)
            .Set(Style::PointerEvents, false);
        AddChild(std::move(thumb));
        return thumbPtr;
    };

    m_LowThumb = createThumb("slider-thumb-low");
    m_HighThumb = createThumb("slider-thumb-high");
    if (m_LowThumb)
        m_LowThumb->AddClass("hidden");

    // Reset range drag session when mouse is released so next click starts fresh.
    auto endDragSession = [this](UIEvent& /*e*/)
    {
        m_RangeDragSessionActive = false;
        if (m_PendingCommittedValueNotifications)
        {
            NotifyValueChanged();
            m_PendingCommittedValueNotifications = false;
        }
    };
    RegisterEventHandler(kEventMouseUp, endDragSession);
    // A pointer that leaves the surface mid-drag ends the session on the value it
    // reached, the same as a release would.
    RegisterEventHandler(kEventMouseCancel, endDragSession);
}

void Slider::SetTrackPaddingPx(float px)
{
    if (m_TrackPaddingPx != px)
    {
        m_TrackPaddingPx = px;
        MarkDirty(VisualDirty);
    }
}

void Slider::SetVerticalTrackThicknessPx(float px)
{
    if (m_VerticalTrackThicknessPx != px)
    {
        m_VerticalTrackThicknessPx = px;
        MarkDirty(VisualDirty);
    }
}

float Slider::GetDomainMin() const
{
    return (m_Min <= m_Max) ? m_Min : m_Max;
}

float Slider::GetDomainMax() const
{
    return (m_Min <= m_Max) ? m_Max : m_Min;
}

float Slider::ClampToDomain(float v) const
{
    const float lo = GetDomainMin();
    const float hi = GetDomainMax();
    if (hi <= lo)
        return lo;
    return std::clamp(v, lo, hi);
}

float Slider::QuantizeToStep(float v) const
{
    if (m_Step <= 0.0f)
        return v;

    const float lo = GetDomainMin();
    const float hi = GetDomainMax();
    if (hi <= lo)
        return lo;

    const double step = static_cast<double>(m_Step);
    const double n = std::round((static_cast<double>(v) - lo) / step);
    double snapped = static_cast<double>(lo) + n * step;
    if (step > 1.0 / kStepSnapDecimalGrid)
        snapped = std::round(snapped * kStepSnapDecimalGrid) / kStepSnapDecimalGrid;
    return std::clamp(static_cast<float>(snapped), lo, hi);
}

void Slider::ApplyPrimaryAdjustmentFromPointer(float v)
{
    float newValue = QuantizeToStep(ClampToDomain(v));

    if (m_IsRange && newValue < m_RangeStart)
        newValue = m_RangeStart;

    const float old = GetValue();
    if (newValue == old)
        return;

    Field<float>::SetValue(newValue);
    MarkDirty(VisualDirty);
    // Keep the visible thumb in lockstep before user callbacks run. A live
    // callback may invalidate a large editor surface, so leaving layout until
    // after it returns makes the handle appear to trail the pointer.
    UpdateThumbLayout();
    NotifyValueChanging();
    m_PendingCommittedValueNotifications = true;
}

void Slider::TouchRangeLowFromPointer(float newLow)
{
    const float quantized = QuantizeToStep(ClampToDomain(newLow));
    if (quantized != m_RangeStart)
    {
        m_RangeStart = quantized;
        MarkDirty(LayoutDirty | VisualDirty);
        UpdateThumbLayout();
        NotifyValueChanging();
        m_PendingCommittedValueNotifications = true;
    }
}

void Slider::UpdateRangeInvariant()
{
    if (!m_IsRange)
        return;

    const float lo = GetDomainMin();
    const float hi = GetDomainMax();
    if (hi <= lo)
    {
        m_RangeStart = lo;
        Field<float>::SetValue(lo);
        return;
    }

    float lowValue = ClampToDomain(m_RangeStart);
    float highValue = ClampToDomain(GetValue());
    if (lowValue > highValue)
        lowValue = highValue;

    m_RangeStart = lowValue;
    Field<float>::SetValue(highValue);
}

void Slider::SetMin(float min)
{
    if (m_Min == min)
        return;
    m_Min = min;
    UpdateRangeInvariant();
    MarkDirty(LayoutDirty | VisualDirty);
}

void Slider::SetMax(float max)
{
    if (m_Max == max)
        return;
    m_Max = max;
    UpdateRangeInvariant();
    MarkDirty(LayoutDirty | VisualDirty);
}

void Slider::SetStep(float step)
{
    const float newStep = (step > 0.0f) ? step : 0.0f;
    if (m_Step == newStep)
        return;
    m_Step = newStep;
    // Re-quantize current values to the new grid.
    float highValue = QuantizeToStep(GetValue());
    float lowValue = QuantizeToStep(m_RangeStart);
    if (m_IsRange && lowValue > highValue)
        lowValue = highValue;
    m_RangeStart = lowValue;
    Field<float>::SetValue(highValue);
    MarkDirty(LayoutDirty | VisualDirty);
}

void Slider::SetCentered(bool centered)
{
    if (m_Centered == centered)
        return;
    m_Centered = centered;
    MarkDirty(VisualDirty);
}

void Slider::SetVertical(bool vertical)
{
    if (m_Vertical == vertical)
        return;
    m_Vertical = vertical;
    if (m_Vertical)
        AddClass("vertical");
    else
        RemoveClass("vertical");
    MarkDirty(LayoutDirty | VisualDirty);
}

void Slider::SetRangeMode(bool isRange)
{
    if (m_IsRange == isRange)
        return;

    m_IsRange = isRange;
    if (m_IsRange)
    {
        // Initialize the secondary value to the current value so the range
        // collapses to a point until explicitly adjusted.
        m_RangeStart = ClampToDomain(GetValue());
    }
    m_ActiveThumb = ActiveThumb::High;
    UpdateRangeInvariant();
    MarkDirty(LayoutDirty | VisualDirty);
}

void Slider::SetRangeStart(float value)
{
    float newLow = QuantizeToStep(ClampToDomain(value));
    float high = GetValue();
    if (!m_IsRange)
    {
        // Enabling range implicitly when a distinct start is set.
        m_IsRange = true;
    }
    // If the requested low end lies above the current high end, expand the
    // high end so we preserve the invariant low <= high. This makes XML
    // attribute orders like range/rangestart/value behave intuitively.
    if (newLow > high)
    {
        Field<float>::SetValue(newLow);
        high = newLow;
    }
    if (m_RangeStart == newLow)
        return;

    m_RangeStart = newLow;
    MarkDirty(LayoutDirty | VisualDirty);
    NotifyValueChanging();
    NotifyValueChanged();
}

void Slider::SetRangeValues(float start, float end)
{
    const float lo = QuantizeToStep(ClampToDomain(std::min(start, end)));
    const float hi = QuantizeToStep(ClampToDomain(std::max(start, end)));

    m_IsRange = true;
    m_RangeStart = lo;
    Field<float>::SetValue(hi);
    MarkDirty(LayoutDirty | VisualDirty);
    NotifyValueChanging();
    NotifyValueChanged();
}

void Slider::SetValue(const float& v)
{
    float newValue = QuantizeToStep(ClampToDomain(v));

    if (m_IsRange && newValue < m_RangeStart)
        newValue = m_RangeStart;

    const float old = GetValue();
    if (newValue == old)
        return;

    m_PendingCommittedValueNotifications = false;
    Field<float>::SetValue(newValue);
    MarkDirty(VisualDirty);
    // Keep thumb geometry in lockstep for programmatic updates too.
    UpdateThumbLayout();
    NotifyValueChanging();
    NotifyValueChanged();
}

void Slider::SetValueWithoutNotify(const float& v)
{
    float newValue = QuantizeToStep(ClampToDomain(v));

    if (m_IsRange && newValue < m_RangeStart)
        newValue = m_RangeStart;

    const float old = GetValue();
    if (newValue == old)
        return;

    Field<float>::SetValueWithoutNotify(newValue);
    MarkDirty(VisualDirty);
    // Label-drag and other external sync paths use SetValueWithoutNotify;
    // update thumb immediately to avoid a one-frame visual lag.
    UpdateThumbLayout();
}

// An operable slider owns the arrow keys along its own axis: it reports those
// consumed even when the value is already clamped at an end, because the press
// was still the slider's to answer. Keys off its axis, and any key at all on a
// degenerate domain, are left for whoever is behind it.
bool Slider::OnKey(int key, int /*mods*/, UI::IPlatformApi* /*platform*/)
{
    // A disabled slider, or one in a disabled row, shows a value; it takes no input.
    if (!IsEnabledInHierarchy())
        return false;
    const float lo = GetDomainMin();
    const float hi = GetDomainMax();
    if (hi <= lo)
        return false;

    bool decrement = false;
    if (m_Vertical)
    {
        if (key != Input::kKeyCode_Up && key != Input::kKeyCode_Down)
            return false;
        decrement = (key == Input::kKeyCode_Down);
    }
    else
    {
        if (key != Input::kKeyCode_Left && key != Input::kKeyCode_Right)
            return false;
        decrement = (key == Input::kKeyCode_Left);
    }
    float step = (m_Step > 0.0f) ? m_Step : (hi - lo) * 0.05f;
    if (step <= 0.0f)
        step = 1.0f;

    if (!m_IsRange || m_ActiveThumb == ActiveThumb::High)
    {
        float v = GetValue() + (decrement ? -step : step);
        SetValue(v);
    }
    else
    {
        // Range mode with the low thumb active.
        float low = m_RangeStart + (decrement ? -step : step);
        float high = GetValue();
        low = QuantizeToStep(ClampToDomain(low));
        if (low > high)
            low = high;
        if (low != m_RangeStart)
        {
            m_RangeStart = low;
            MarkDirty(LayoutDirty | VisualDirty);
            NotifyValueChanging();
            NotifyValueChanged();
        }
    }
    return true;
}

void Slider::OnPostLayout()
{
    UpdateThumbLayout();
}

void Slider::UpdateThumbLayout()
{
    if (!m_LowThumb || !m_HighThumb)
        return;

    if (m_IsRange)
        m_LowThumb->RemoveClass("hidden");
    else
        m_LowThumb->AddClass("hidden");
    m_HighThumb->RemoveClass("hidden");

    const float x = GetLayoutX();
    const float y = GetLayoutY();
    const float W = GetLayoutWidth();
    const float H = GetLayoutHeight();
    if (W <= 0.0f || H <= 0.0f)
        return;

    const float lo = GetDomainMin();
    const float hi = GetDomainMax();
    if (hi <= lo)
        return;

    auto normalize = [&](float v) -> float
    {
        return (v - lo) / (hi - lo);
    };

    float lowValue = m_IsRange ? ClampToDomain(m_RangeStart) : lo;
    float highValue = ClampToDomain(GetValue());
    if (m_IsRange && lowValue > highValue)
        lowValue = highValue;

    auto resolveThumbSize = [](UIElement* thumb, float fallbackW, float fallbackH, float& outW, float& outH)
    {
        outW = thumb ? thumb->GetLayoutWidth() : 0.0f;
        outH = thumb ? thumb->GetLayoutHeight() : 0.0f;
        if (outW <= 0.0f)
            outW = fallbackW;
        if (outH <= 0.0f)
            outH = fallbackH;
    };

    if (m_Vertical)
    {
        const float pad = GetTrackPaddingPx();
        const float trackY0 = y + pad;
        const float trackH = std::max(0.0f, H - 2.0f * pad);
        if (trackH <= 0.0f)
            return;

        const float fallbackThumbSize = std::min(kSliderThumbRadiusPx, W * 0.5f) * 2.0f;
        const float thumbCenterX = x + W * 0.5f;

        auto placeThumb = [&](UIElement* thumb, float norm)
        {
            if (!thumb)
                return;

            float thumbW = fallbackThumbSize;
            float thumbH = fallbackThumbSize;
            resolveThumbSize(thumb, fallbackThumbSize, fallbackThumbSize, thumbW, thumbH);

            const float clampedNorm = std::clamp(norm, 0.0f, 1.0f);
            const float thumbCenterY = trackY0 + trackH * (1.0f - clampedNorm);
            CommitThumbRect(thumb, thumbCenterX - thumbW * 0.5f,
                            thumbCenterY - thumbH * 0.5f, thumbW, thumbH, x, y);
        };

        if (m_IsRange)
            placeThumb(m_LowThumb, normalize(lowValue));
        placeThumb(m_HighThumb, normalize(highValue));
        return;
    }

    const float pad = GetTrackPaddingPx();
    const float trackX = x + pad;
    const float trackW = std::max(0.0f, W - 2.0f * pad);
    if (trackW <= 0.0f)
        return;

    const float fallbackThumbSize = std::min(kSliderThumbRadiusPx, H * 0.5f) * 2.0f;
    const float thumbCenterY = y + H * 0.5f;

    auto placeThumb = [&](UIElement* thumb, float norm)
    {
        if (!thumb)
            return;

        float thumbW = fallbackThumbSize;
        float thumbH = fallbackThumbSize;
        resolveThumbSize(thumb, fallbackThumbSize, fallbackThumbSize, thumbW, thumbH);

        const float clampedNorm = std::clamp(norm, 0.0f, 1.0f);
        const float thumbCenterX = trackX + trackW * clampedNorm;
        CommitThumbRect(thumb, thumbCenterX - thumbW * 0.5f,
                        thumbCenterY - thumbH * 0.5f, thumbW, thumbH, x, y);
    };

    if (m_IsRange)
        placeThumb(m_LowThumb, normalize(lowValue));
    placeThumb(m_HighThumb, normalize(highValue));
}

void Slider::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                                  const ResolvedStyle& style,
                                  float x, float y, float W, float H)
{
    using namespace UI;

    if (W <= 0.0f || H <= 0.0f)
        return;

    const float lo = GetDomainMin();
    const float hi = GetDomainMax();
    if (hi <= lo)
        return;

    auto normalize = [&](float v) -> float
    {
        return (v - lo) / (hi - lo);
    };

    float lowValue = m_IsRange ? m_RangeStart : lo;
    float highValue = GetValue();
    lowValue = ClampToDomain(lowValue);
    highValue = ClampToDomain(highValue);
    if (m_IsRange && lowValue > highValue)
        lowValue = highValue;

    const float tLow = normalize(lowValue);
    const float tHigh = normalize(highValue);

    const uint32_t activeColor = TrackColor(style, kTrackFillVar);
    const uint32_t inactiveColor = TrackColor(style, kTrackVar);
    const uint32_t tickColor = TrackColor(style, kTickVar);

    // (x, y, W, H) arrive in physical px (scaled at the paint seam in
    // UIManager::GeneratePrimitivesForElement). CSS constants / m_*Px values
    // are logical, so scale them here for consistent space.
    const float cs = ctx.ContentScale;

    if (m_Vertical)
    {
        const float pad = GetTrackPaddingPx() * cs;
        const float trackY0 = y + pad;
        const float trackH = std::max(0.0f, H - 2.0f * pad);
        if (trackH <= 0.0f)
            return;

        const float vertThickness = (m_VerticalTrackThicknessPx >= 0.0f
                                        ? m_VerticalTrackThicknessPx
                                        : kSliderVerticalTrackThicknessPx) * cs;
        const float trackW = std::min(vertThickness, W * 0.5f);
        const float trackX = x + W * 0.5f - trackW * 0.5f;
        const float r = trackW * 0.5f;

        // One tick glyph for both the step grid and the explicit marks.
        auto emitTick = [&](float t01)
        {
            const float cy = trackY0 + trackH * (1.0f - t01);
            const float tw = trackW * 2.0f;
            const float th = 2.0f * cs;
            const float tx = trackX + trackW * 0.5f - tw * 0.5f;
            ctx.Emit(MakeRect(tx, cy - th * 0.5f, tw, th, tickColor));
        };

        if (m_ShowTicks && m_Step > 0.0f && hi > lo)
        {
            const int tickCount = std::min(64, std::max(1, (int)std::round((hi - lo) / m_Step)));
            for (int i = 0; i <= tickCount; ++i)
                emitTick((float)i / (float)tickCount);
        }
        for (float tickValue : m_TickMarks)
        {
            if (tickValue >= lo && tickValue <= hi)
                emitTick(normalize(tickValue));
        }

        float activeYStart = trackY0;
        float activeH = 0.0f;
        if (m_IsRange)
        {
            float a0 = trackY0 + trackH * (1.0f - tHigh);
            float a1 = trackY0 + trackH * (1.0f - tLow);
            if (a1 < a0)
                std::swap(a0, a1);
            activeYStart = a0;
            activeH = std::max(0.0f, a1 - a0);
        }
        else if (m_Centered)
        {
            const float centerNorm = std::clamp(normalize(0.0f), 0.0f, 1.0f);
            const float valueNorm = std::clamp(normalize(highValue), 0.0f, 1.0f);
            float a0 = trackY0 + trackH * (1.0f - valueNorm);
            float a1 = trackY0 + trackH * (1.0f - centerNorm);
            if (a1 < a0)
                std::swap(a0, a1);
            activeYStart = a0;
            activeH = std::max(0.0f, a1 - a0);
        }
        else
        {
            activeYStart = trackY0 + trackH * (1.0f - std::clamp(tHigh, 0.0f, 1.0f));
            activeH = trackH * std::clamp(tHigh, 0.0f, 1.0f);
        }

        if (activeH > 0.0f)
            ctx.Emit(MakeRect(trackX, activeYStart, trackW, activeH, activeColor, r, r, r, r));

        // Inactive rail on BOTH sides of the active segment: centered and
        // range modes leave track above and below it (a bottom-anchored
        // value only leaves the top part).
        auto emitRail = [&](float railY, float railH)
        {
            if (railH > 0.0f)
                ctx.Emit(MakeRect(trackX, railY, trackW, railH, inactiveColor, r, r, r, r));
        };
        emitRail(trackY0, activeYStart - trackY0);
        emitRail(activeYStart + activeH, (trackY0 + trackH) - (activeYStart + activeH));

        return;
    }

    // Horizontal track
    const float pad = GetTrackPaddingPx() * cs;
    const float trackX = x + pad;
    const float trackW = std::max(0.0f, W - 2.0f * pad);
    if (trackW <= 0.0f)
        return;

    const float trackH = std::min(kSliderTrackThicknessPx * cs, H * 0.25f);
    const float trackY0 = y + H * 0.5f - trackH * 0.5f;
    const float r = trackH * 0.5f;

    // One tick glyph for both the step grid and the explicit marks.
    auto emitTick = [&](float t01)
    {
        const float cx = trackX + trackW * t01;
        const float tw = 2.0f * cs;
        const float th = trackH * 2.0f;
        const float ty = trackY0 + trackH * 0.5f - th * 0.5f;
        ctx.Emit(MakeRect(cx - tw * 0.5f, ty, tw, th, tickColor));
    };

    if (m_ShowTicks && m_Step > 0.0f && hi > lo)
    {
        const int tickCount = std::min(64, std::max(1, (int)std::round((hi - lo) / m_Step)));
        for (int i = 0; i <= tickCount; ++i)
            emitTick((float)i / (float)tickCount);
    }
    for (float tickValue : m_TickMarks)
    {
        if (tickValue >= lo && tickValue <= hi)
            emitTick(normalize(tickValue));
    }

    float activeX = trackX;
    float activeW = 0.0f;

    if (m_IsRange)
    {
        float a0 = trackX + trackW * tLow;
        float a1 = trackX + trackW * tHigh;
        if (a1 < a0)
            std::swap(a0, a1);
        activeX = a0;
        activeW = std::max(0.0f, a1 - a0);
    }
    else if (m_Centered)
    {
        const float centerNorm = std::clamp(normalize(0.0f), 0.0f, 1.0f);
        const float valueNorm = std::clamp(normalize(highValue), 0.0f, 1.0f);
        float a0 = trackX + trackW * centerNorm;
        float a1 = trackX + trackW * valueNorm;
        if (a1 < a0)
            std::swap(a0, a1);
        activeX = a0;
        activeW = std::max(0.0f, a1 - a0);
    }
    else
    {
        float a0 = trackX;
        float a1 = trackX + trackW * std::clamp(tHigh, 0.0f, 1.0f);
        if (a1 < a0)
            std::swap(a0, a1);
        activeX = a0;
        activeW = std::max(0.0f, a1 - a0);
    }

    if (activeW > 0.0f)
        ctx.Emit(MakeRect(activeX, trackY0, activeW, trackH, activeColor, r, r, r, r));

    // Inactive rail on BOTH sides of the active segment: centered mode (value
    // below 0) and range mode leave track to the left of it (a left-anchored
    // value only leaves the right part).
    auto emitRail = [&](float railX, float railW)
    {
        if (railW > 0.0f)
            ctx.Emit(MakeRect(railX, trackY0, railW, trackH, inactiveColor, r, r, r, r));
    };
    emitRail(trackX, activeX - trackX);
    emitRail(activeX + activeW, (trackX + trackW) - (activeX + activeW));
}

void Slider::OnPointerDown(float mouseX, float mouseY,
                           float x, float y, float W, float H,
                           const ResolvedStyle& /*style*/,
                           Rendering::Text::FontAtlas* /*font*/)
{
    if (!IsEnabledInHierarchy())
        return;
    const float lo = GetDomainMin();
    const float hi = GetDomainMax();
    if (hi <= lo)
        return;

    float t;
    float trackX, trackW, trackY, trackH;
    float thumbCenterX = 0.0f;

    const float pad = GetTrackPaddingPx();
    if (m_Vertical)
    {
        trackY = y + pad;
        trackH = std::max(0.0f, H - 2.0f * pad);
        if (trackH <= 0.0f)
            return;
        const float vertThickness = m_VerticalTrackThicknessPx >= 0.0f ? m_VerticalTrackThicknessPx : kSliderVerticalTrackThicknessPx;
        trackW = std::min(vertThickness, W * 0.5f);
        trackX = x + W * 0.5f - trackW * 0.5f;
        t = 1.0f - (mouseY - trackY) / trackH;
        t = std::clamp(t, 0.0f, 1.0f);
        thumbCenterX = x + W * 0.5f;
    }
    else
    {
        trackX = x + pad;
        trackW = std::max(0.0f, W - 2.0f * pad);
        if (trackW <= 0.0f)
            return;
        trackY = y + H * 0.5f;
        trackH = 0.0f;
        t = (mouseX - trackX) / trackW;
        t = std::clamp(t, 0.0f, 1.0f);
    }

    float valueAtPointer = lo + t * (hi - lo);
    valueAtPointer = QuantizeToStep(valueAtPointer);

    if (m_IsRange)
    {
        m_RangeDragSessionActive = false;

        float low = ClampToDomain(m_RangeStart);
        float high = ClampToDomain(GetValue());
        if (low > high)
            low = high;

        const float tLow = (low - lo) / (hi - lo);
        const float tHigh = (high - lo) / (hi - lo);

        const float fallbackThumbRadius = std::min(kSliderThumbRadiusPx, m_Vertical ? (W * 0.5f) : (H * 0.5f));
        float lowHitRadius = fallbackThumbRadius * 1.75f;
        float highHitRadius = fallbackThumbRadius * 1.75f;

        float lowCenterXPos = m_Vertical ? thumbCenterX : (trackX + trackW * std::clamp(tLow, 0.0f, 1.0f));
        float highCenterXPos = m_Vertical ? thumbCenterX : (trackX + trackW * std::clamp(tHigh, 0.0f, 1.0f));
        float lowCenterYPos = m_Vertical ? (trackY + trackH * (1.0f - std::clamp(tLow, 0.0f, 1.0f))) : (y + H * 0.5f);
        float highCenterYPos = m_Vertical ? (trackY + trackH * (1.0f - std::clamp(tHigh, 0.0f, 1.0f))) : (y + H * 0.5f);

        auto useThumbLayout = [](const UIElement* thumb, float& centerX, float& centerY, float& hitRadius)
        {
            if (!thumb)
                return;

            const float thumbW = thumb->GetLayoutWidth();
            const float thumbH = thumb->GetLayoutHeight();
            if (thumbW <= 0.0f || thumbH <= 0.0f)
                return;

            centerX = thumb->GetLayoutX() + thumbW * 0.5f;
            centerY = thumb->GetLayoutY() + thumbH * 0.5f;

            const float radius = std::max(thumbW, thumbH) * 0.5f;
            if (radius > 0.0f)
                hitRadius = radius * 1.75f;
        };

        useThumbLayout(m_LowThumb, lowCenterXPos, lowCenterYPos, lowHitRadius);
        useThumbLayout(m_HighThumb, highCenterXPos, highCenterYPos, highHitRadius);

        auto dist2 = [](float ax, float ay, float bx, float by) -> float
        {
            const float dx = ax - bx;
            const float dy = ay - by;
            return dx * dx + dy * dy;
        };

        const float d2Low = dist2(mouseX, mouseY, lowCenterXPos, lowCenterYPos);
        const float d2High = dist2(mouseX, mouseY, highCenterXPos, highCenterYPos);
        const float lowHitR2 = lowHitRadius * lowHitRadius;
        const float highHitR2 = highHitRadius * highHitRadius;

        const bool nearLow = d2Low <= lowHitR2;
        const bool nearHigh = d2High <= highHitR2;

#ifdef _DEBUG
        if (kSliderRangeDebugConsole)
        {
            const std::string& id = GetId();
            const bool isDemoRange = (id == "UIDemoSliderRange");
            if (isDemoRange)
            {
                Logger::Log::Info(
                    "[SliderRangeDebug] id='{}' mouseX={}, mouseY={}, x={}, y={}, W={}, H={} "
                    "low={}, high={}, lowCx={}, highCx={}, cy={}, d2Low={}, d2High={}, "
                    "nearLow={}, nearHigh={}",
                    id.empty() ? "<no-id>" : id.c_str(),
                    mouseX, mouseY, x, y, W, H,
                    low, high,
                    lowCenterXPos, highCenterXPos,
                    lowCenterYPos,
                    d2Low, d2High,
                    nearLow, nearHigh);
            }
        }
#endif

        const bool thumbsOverlap = m_Vertical
            ? (std::abs(lowCenterYPos - highCenterYPos) < 1e-3f)
            : (std::abs(lowCenterXPos - highCenterXPos) < 1e-3f);

        if (nearLow && !nearHigh)
        {
            m_ActiveThumb = ActiveThumb::Low;
#ifdef _DEBUG
            if (kSliderRangeDebugConsole && GetId() == "UIDemoSliderRange")
                Logger::Log::Info("[SliderRangeDebug] Branch: nearLow && !nearHigh -> selecting LOW");
#endif
        }
        else if (nearHigh && !nearLow)
        {
            m_ActiveThumb = ActiveThumb::High;
#ifdef _DEBUG
            if (kSliderRangeDebugConsole && GetId() == "UIDemoSliderRange")
                Logger::Log::Info("[SliderRangeDebug] Branch: nearHigh && !nearLow -> selecting HIGH");
#endif
        }
        else if (nearLow && nearHigh && thumbsOverlap)
        {
            if (m_Vertical)
            {
                if (mouseY > lowCenterYPos)
                    m_ActiveThumb = ActiveThumb::Low;
                else if (mouseY < lowCenterYPos)
                    m_ActiveThumb = ActiveThumb::High;
            }
            else
            {
                if (mouseX < lowCenterXPos)
                    m_ActiveThumb = ActiveThumb::Low;
                else if (mouseX > lowCenterXPos)
                    m_ActiveThumb = ActiveThumb::High;
            }
#ifdef _DEBUG
            if (kSliderRangeDebugConsole && GetId() == "UIDemoSliderRange")
                Logger::Log::Info("[SliderRangeDebug] Branch: overlap -> selecting {}",
                                  m_ActiveThumb == ActiveThumb::Low ? "LOW" : "HIGH");
#endif
        }
        else
        {
            if (m_Vertical)
            {
                const float midY = (lowCenterYPos + highCenterYPos) * 0.5f;
                m_ActiveThumb = (mouseY > midY) ? ActiveThumb::Low : ActiveThumb::High;
            }
            else
            {
                const float midX = (lowCenterXPos + highCenterXPos) * 0.5f;
                m_ActiveThumb = (mouseX < midX) ? ActiveThumb::Low : ActiveThumb::High;
            }
#ifdef _DEBUG
            if (kSliderRangeDebugConsole && GetId() == "UIDemoSliderRange")
                Logger::Log::Info("[SliderRangeDebug] Branch: else -> selecting {}",
                                  m_ActiveThumb == ActiveThumb::Low ? "LOW" : "HIGH");
#endif
        }

        if (m_ActiveThumb == ActiveThumb::Low)
        {
            float newLow = ClampToDomain(valueAtPointer);
            if (newLow > high)
                newLow = high;
#ifdef _DEBUG
            if (kSliderRangeDebugConsole && GetId() == "UIDemoSliderRange")
                Logger::Log::Info("[SliderRangeDebug] Updating LOW: valueAtPointer={}, newLow={}, oldRangeStart={}, high={}",
                                  valueAtPointer, newLow, m_RangeStart, high);
#endif
            TouchRangeLowFromPointer(newLow);
        }
        else
        {
            float newHigh = ClampToDomain(valueAtPointer);
            if (newHigh < low)
                newHigh = low;
#ifdef _DEBUG
            if (kSliderRangeDebugConsole && GetId() == "UIDemoSliderRange")
                Logger::Log::Info("[SliderRangeDebug] Updating HIGH: valueAtPointer={}, newHigh={}, oldValue={}, low={}",
                                  valueAtPointer, newHigh, GetValue(), low);
#endif
            ApplyPrimaryAdjustmentFromPointer(newHigh);
        }
        // Mark the drag session as properly initiated so OnPointerDrag can proceed
        m_RangeDragSessionActive = true;
    }
    else
    {
        ApplyPrimaryAdjustmentFromPointer(valueAtPointer);
    }

    // Reposition thumbs immediately; OnPostLayout alone is a frame late during drags.
    UpdateThumbLayout();
}

void Slider::OnPointerDrag(float mouseX, float mouseY,
                           float x, float y, float W, float H,
                           const ResolvedStyle& /*style*/,
                           Rendering::Text::FontAtlas* /*font*/)
{
    if (!IsEnabledInHierarchy())
        return;
    const float lo = GetDomainMin();
    const float hi = GetDomainMax();
    if (hi <= lo)
        return;

    float t;
    const float pad = GetTrackPaddingPx();
    if (m_Vertical)
    {
        const float trackY = y + pad;
        const float trackH = std::max(0.0f, H - 2.0f * pad);
        if (trackH <= 0.0f)
            return;
        t = 1.0f - (mouseY - trackY) / trackH;
        t = std::clamp(t, 0.0f, 1.0f);
    }
    else
    {
        const float trackX = x + pad;
        const float trackW = std::max(0.0f, W - 2.0f * pad);
        if (trackW <= 0.0f)
            return;
        t = (mouseX - trackX) / trackW;
        t = std::clamp(t, 0.0f, 1.0f);
    }

    float valueAtPointer = lo + t * (hi - lo);
    valueAtPointer = QuantizeToStep(valueAtPointer);

    if (m_IsRange)
    {
        // If OnPointerDown hasn't been called yet for this drag sequence, skip.
        // This prevents using stale m_ActiveThumb from a previous interaction.
        if (!m_RangeDragSessionActive)
            return;

        float low = ClampToDomain(m_RangeStart);
        float high = ClampToDomain(GetValue());
        if (low > high)
            low = high;

#ifdef _DEBUG
        if (kSliderRangeDebugConsole && GetId() == "UIDemoSliderRange")
            Logger::Log::Info("[SliderRangeDebug] OnPointerDrag: activeThumb={}, low={}, high={}, valueAtPointer={}",
                              m_ActiveThumb == ActiveThumb::Low ? "LOW" : "HIGH", low, high, valueAtPointer);
#endif

        if (m_ActiveThumb == ActiveThumb::Low)
        {
            float newLow = ClampToDomain(valueAtPointer);
            if (newLow > high)
            {
                // Swap behavior: dragging the low thumb past the high thumb
                // causes the thumbs to swap roles so dragging remains smooth.
                const float oldLow = m_RangeStart;
                const float oldHigh = GetValue();

                m_RangeStart = high;
                Field<float>::SetValue(newLow);
                m_ActiveThumb = ActiveThumb::High;

#ifdef _DEBUG
                if (kSliderRangeDebugConsole && GetId() == "UIDemoSliderRange")
                    Logger::Log::Info("[SliderRangeDebug] OnPointerDrag: SWAP low->high, newRangeStart={}, newValue={}",
                                      m_RangeStart, GetValue());
#endif

                if (m_RangeStart != oldLow || GetValue() != oldHigh)
                {
                    MarkDirty(LayoutDirty | VisualDirty);
                    UpdateThumbLayout();
                    NotifyValueChanging();
                    m_PendingCommittedValueNotifications = true;
                }
            }
            else
                TouchRangeLowFromPointer(newLow);
        }
        else
        {
            float newHigh = ClampToDomain(valueAtPointer);
            if (newHigh < low)
            {
                // Swap behavior: dragging the high thumb past the low thumb
                // causes the thumbs to swap roles so dragging remains smooth.
                const float oldLow = m_RangeStart;
                const float oldHigh = GetValue();

                m_RangeStart = newHigh;
                Field<float>::SetValue(low);
                m_ActiveThumb = ActiveThumb::Low;

#ifdef _DEBUG
                if (kSliderRangeDebugConsole && GetId() == "UIDemoSliderRange")
                    Logger::Log::Info("[SliderRangeDebug] OnPointerDrag: SWAP high->low, newRangeStart={}, newValue={}",
                                      m_RangeStart, GetValue());
#endif

                if (m_RangeStart != oldLow || GetValue() != oldHigh)
                {
                    MarkDirty(LayoutDirty | VisualDirty);
                    UpdateThumbLayout();
                    NotifyValueChanging();
                    m_PendingCommittedValueNotifications = true;
                }
            }
            else
            {
                ApplyPrimaryAdjustmentFromPointer(newHigh);
            }
        }
    }
    else
    {
        ApplyPrimaryAdjustmentFromPointer(valueAtPointer);
    }

    UpdateThumbLayout();
}

} // namespace GameEngine
