#include "UI/Controls/CurveField.h"

#include "Editor/Settings/CurveEditorSettings.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include "Platform/SystemMetrics.h"
#include "Rendering/Text/FontAtlas.h"
#include "UI/Controls/CurveTangents.h"
#include "UI/UIPrimitive.h"
#include "UI/UIEvents.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

namespace GameEngine
{
namespace
{

// Logical sizing constants (multiplied by content scale when painting; used raw when
// hit-testing, mirroring SkyDayKeyGradientBar's logical-vs-physical split).
constexpr float kHandlePickPx = 13.0f;
constexpr float kTangentPickPx = 11.0f;
constexpr float kTangentReachPx = 34.0f;
constexpr float kDragThresholdPx = 3.0f;
constexpr float kDoubleClickDistancePx = 5.0f;
constexpr float kCurveLineThicknessPx = 2.0f;
constexpr float kFieldHeightPx = 110.0f;
constexpr float kNormalHandleSizePx = 8.0f;
constexpr float kActiveHandleSizePx = 11.0f;
constexpr float kHandleEdgeInsetPx = 1.0f;
constexpr float kEndpointAxisInsetPx = kActiveHandleSizePx * 0.5f + kHandleEdgeInsetPx;
constexpr float kCompactEndpointHeightThresholdPx = 160.0f;
constexpr float kPadLeftPx = 14.0f;
constexpr float kPadTopPx = 12.0f;
constexpr float kPadRightPx = 14.0f;
constexpr float kPadBottomPx = 16.0f;
// Smallest time gap kept between adjacent keys (fraction of the time range).
constexpr float kMinTimeGapFrac = 0.005f;

constexpr const char* kCurveFieldStyleAssetPath = "UI/controls/CurveField/CurveField.css";
// The time labels sit in a strip under the plot: a playback dot's radius and its clearance, one line
// of the label font, and a gap.
constexpr float kMinimumLabelFontSizePx = 12.0f;
constexpr float kLabelLineHeightRatio = 1.3f;
constexpr float kLabelGapPx = 2.0f;
// The playback indicator's dot on the curve. The time labels start its radius plus a clearance below
// the floor, so a dot on the floor keeps two clear pixels to the time pill past its anti-aliased edge.
constexpr float kPlaybackDotRadiusPx = 6.0f;
constexpr float kPlaybackDotClearancePx = 4.0f;
// The grid divides the plot into this many spans each way, with the middle line drawn stronger; the
// time labels sit on the vertical lines.
constexpr int kGridSpans = 4;
constexpr float kPlaybackLabelPaddingPx = 4.0f;
constexpr float kReferenceLineThicknessPx = 1.5f;
constexpr int kLabelMinutesPerHour = 60;
constexpr int kLabelMinutesPerDay = 24 * kLabelMinutesPerHour;

// Colours the control paints with, read from its resolved style: CurveField.css sets them from the
// theme. An unset property draws nothing, as the slider's track colours do.
const StringId kReferenceLineColorVar = HashStringId("--curve-field-reference-line-color");
const StringId kPlaybackLabelBackgroundVar = HashStringId("--curve-field-playback-label-background");
const StringId kPlaybackLabelColorVar = HashStringId("--curve-field-playback-label-color");
const StringId kShadedSpanColorVar = HashStringId("--curve-field-shaded-span-color");
const StringId kValueMarkColorVar = HashStringId("--curve-field-value-mark-color");
const StringId kPlotHeightVar = HashStringId("--curve-field-plot-height");

// Palette shared with the value-curve editor so the curve UIs read consistently.
const uint32_t kCurveLineColor = UI::PackColor(0.32f, 0.68f, 1.0f, 1.0f);
const uint32_t kEndpointHandleColor = UI::PackColor(0.78f, 0.92f, 1.00f, 1.0f);
const uint32_t kInteriorHandleColor = UI::PackColor(1.0f, 0.74f, 0.28f, 1.0f);
// Read-only scrubber playhead (e.g. the sky's current time of day): a soft amber vertical line.
const uint32_t kScrubberColor = UI::PackColor(1.0f, 0.82f, 0.30f, 0.62f);

float SpanOrEpsilon(float lo, float hi)
{
    const float span = hi - lo;
    return std::abs(span) < 1e-6f ? 1e-6f : span;
}

uint32_t WithARGBAlpha(uint32_t argb, uint8_t alpha)
{
    return (argb & 0x00FFFFFFu) | (static_cast<uint32_t>(alpha) << 24);
}

float HandleLogicalSize(bool active, float maxSizePx)
{
    float size = active ? kActiveHandleSizePx : kNormalHandleSizePx;
    if (maxSizePx > 0.0f)
        size = std::min(size, maxSizePx);
    return size;
}

float ClampHandleCenter(float min, float max, float value, float logicalSizePx, float scale)
{
    const float half = (logicalSizePx * 0.5f + kHandleEdgeInsetPx) * scale;
    if (max - min <= half * 2.0f)
        return (min + max) * 0.5f;
    return std::clamp(value, min + half, max - half);
}

float TimeLabelStripPx(CurveField::TimeLabels labels, float fontSizePx)
{
    return labels == CurveField::TimeLabels::None
               ? 0.0f
               : kPlaybackDotRadiusPx + kPlaybackDotClearancePx + fontSizePx * kLabelLineHeightRatio + kLabelGapPx;
}

// "06:00" for 6 hours, to the minute. 24:00 stays 24:00 so the axis end reads as the day's end.
std::string HoursOfDayLabel(float hours)
{
    const int minutes = std::clamp(static_cast<int>(std::lround(hours * kLabelMinutesPerHour)), 0, kLabelMinutesPerDay);
    char text[8];
    std::snprintf(text, sizeof(text), "%02d:%02d", minutes / kLabelMinutesPerHour, minutes % kLabelMinutesPerHour);
    return text;
}

// The colour a custom property holds, packed for a primitive.
uint32_t StyledColor(const ResolvedStyle& style, StringId var)
{
    return UI::PackFromARGB(style.GetCustomColor(var).value_or(0u));
}

} // namespace

CurveField::CurveField() : m_PlotHeightPx(kFieldHeightPx)
{
    AddClass("curve-field");
    RequestSubtreeStyleAssetPath(kCurveFieldStyleAssetPath, "editor");
    SetFocusable(true);
    Overrides()
        .Set(Style::Height, StyleLength::Px(kFieldHeightPx))
        .Set(Style::MinHeight, StyleLength::Px(kFieldHeightPx));
    // Cursor is set per hover/drag state in UpdateCursor (grab only over a point), not statically.
}

void CurveField::SetConfig(const Config& config)
{
    assert((!config.LogarithmicValues || config.LogarithmicFloor > 0.0f) &&
           "a logarithmic CurveField needs LogarithmicFloor above 0, the floor its keys are evaluated with");
    // The time-label strip is added under the plot, not taken out of it: the control grows by the
    // strip when labels are switched on and shrinks back when they are switched off. Only a change of
    // the option (or, with labels on, of their font size) touches the height, so a host that sets its
    // own height on a graph without labels keeps it.
    const bool labelsChanged = config.TimeAxisLabels != m_Config.TimeAxisLabels;
    m_Config = config;
    if (labelsChanged)
        ApplyLabelStripHeight();
    SetFocusable(!m_Config.ReadOnly);
    MarkDirty(VisualDirty);
}

void CurveField::ApplyLabelStripHeight()
{
    const float height = m_PlotHeightPx + TimeLabelStripPx(m_Config.TimeAxisLabels, m_TimeLabelFontSizePx);
    Overrides().Set(Style::Height, StyleLength::Px(height)).Set(Style::MinHeight, StyleLength::Px(height));
}

// The labels' font size and the plot's height come from the resolved style (CurveField.css, or a
// host's sheet for the plot height), which is known only once styles have resolved. They size the
// control's height, so they are read here, where a change can still reach this frame's layout, and
// nowhere else.
void CurveField::OnPostLayout()
{
    const ResolvedStyle& style = GetResolvedStyle();
    const float fontSize = std::max(style.Visual.FontSize, kMinimumLabelFontSizePx);
    const std::optional<StyleLength> plotHeight = style.GetCustomLength(kPlotHeightVar);
    const bool plotHeightInPx = plotHeight && plotHeight->Unit == StyleLength::UnitType::Px;
    const float plotHeightPx = plotHeightInPx ? plotHeight->Value : kFieldHeightPx;
    if (plotHeight && !plotHeightInPx && !m_ReportedPlotHeightUnit)
    {
        m_ReportedPlotHeightUnit = true;
        Logger::Log::Warning("CurveField: --curve-field-plot-height takes a length in px; the graph keeps its "
                             "{} px plot. Write the height as, for example, 200px.",
                             kFieldHeightPx);
    }
    if (fontSize == m_TimeLabelFontSizePx && plotHeightPx == m_PlotHeightPx)
        return;
    m_TimeLabelFontSizePx = fontSize;
    m_PlotHeightPx = plotHeightPx;
    if (m_Config.TimeAxisLabels != TimeLabels::None)
        ApplyLabelStripHeight();
    MarkDirty(VisualDirty);
}

void CurveField::SetKeys(const std::vector<Math::CurveKey>& keys)
{
    m_Keys = keys;
    std::sort(m_Keys.begin(), m_Keys.end(),
              [](const Math::CurveKey& a, const Math::CurveKey& b) { return a.Time < b.Time; });
    if (m_Keys.empty())
        m_Selected = -1;
    else if (m_Selected >= static_cast<int>(m_Keys.size()))
        m_Selected = static_cast<int>(m_Keys.size()) - 1;
    MarkDirty(VisualDirty);
}

void CurveField::SetReferenceLine(const std::vector<Math::CurveKey>& samples)
{
    m_ReferenceLine = samples;
    MarkDirty(VisualDirty);
}

void CurveField::SetShadedSpans(const std::vector<ShadedSpan>& spans)
{
    m_ShadedSpans = spans;
    MarkDirty(VisualDirty);
}

void CurveField::SetSelectedKey(int index)
{
    SetSelectedInternal(index, true);
}

void CurveField::SetSelectedInternal(int index, bool notify)
{
    const int clamped = (index >= 0 && index < static_cast<int>(m_Keys.size())) ? index : -1;
    if (clamped == m_Selected)
        return;
    m_Selected = clamped;
    MarkDirty(VisualDirty);
    if (notify && m_OnSelectionChanged)
        m_OnSelectionChanged(m_Selected);
}

int CurveField::SetSelectedKeyValue(float value)
{
    if (m_Selected < 0 || m_Selected >= static_cast<int>(m_Keys.size()))
        return m_Selected;
    if (m_Keys[m_Selected].Value != value)
    {
        m_Keys[m_Selected].Value = value;
        MarkDirty(VisualDirty);
    }
    return m_Selected;
}

int CurveField::SetSelectedKeyTime(float time)
{
    if (m_Selected < 0 || m_Selected >= static_cast<int>(m_Keys.size()))
        return m_Selected;
    const int i = m_Selected;
    const int n = static_cast<int>(m_Keys.size());
    const float gap = kMinTimeGapFrac * std::abs(SpanOrEpsilon(m_Config.TimeMin, m_Config.TimeMax));
    const float lo = (i > 0) ? m_Keys[i - 1].Time + gap : m_Config.TimeMin;
    const float hi = (i < n - 1) ? m_Keys[i + 1].Time - gap : m_Config.TimeMax;
    const float t = (lo <= hi) ? std::clamp(time, lo, hi) : m_Keys[i].Time;
    if (m_Keys[i].Time != t)
    {
        m_Keys[i].Time = t;
        MarkDirty(VisualDirty);
    }
    return m_Selected;
}

void CurveField::SetScrubberTime(float time)
{
    const float lo = std::min(m_Config.TimeMin, m_Config.TimeMax);
    const float hi = std::max(m_Config.TimeMin, m_Config.TimeMax);
    const float clamped = std::clamp(time, lo, hi);
    if (m_ScrubberTime == clamped)
        return;
    m_ScrubberTime = clamped;
    if (m_Config.ShowScrubber)
        MarkDirty(VisualDirty);
}

void CurveField::SetPlaybackIndicator(float time, float value, bool visible)
{
    const float lo = std::min(m_Config.TimeMin, m_Config.TimeMax);
    const float hi = std::max(m_Config.TimeMin, m_Config.TimeMax);
    const float clampedTime = std::clamp(time, lo, hi);
    if (m_PlaybackIndicatorVisible == visible && m_PlaybackTime == clampedTime && m_PlaybackValue == value)
        return;

    m_PlaybackIndicatorVisible = visible;
    m_PlaybackTime = clampedTime;
    m_PlaybackValue = value;
    if (m_Config.ShowPlaybackIndicator)
        MarkDirty(VisualDirty);
}

CurveField::GraphRect CurveField::CurrentGraphRect() const
{
    return ComputeGraphRect(GetLayoutX(), GetLayoutY(), GetLayoutWidth(), GetLayoutHeight());
}

CurveField::GraphRect CurveField::ComputeGraphRect(float x, float y, float w, float h, float scale) const
{
    // Padding is logical px; scale it when insetting the physical paint rect so the painted graph
    // and the hit-test graph (logical, scale=1) resolve to the same on-screen rectangle. At a
    // fractional content scale the two round their top and floor separately, so they can differ by
    // under a pixel, well inside the pick radius. Without the scaling, handles paint where the
    // hit-test does not expect them on HiDPI.
    // The top and the floor sit on whole pixels: a horizontal curve segment on a half pixel spreads
    // over three rows, and the overlap at each segment join beads the line.
    const float padBottom = kPadBottomPx + TimeLabelStripPx(m_Config.TimeAxisLabels, m_TimeLabelFontSizePx);
    const float top = std::round(y + kPadTopPx * scale);
    const float floorY = std::round(y + h - padBottom * scale);
    return {x + (kPadLeftPx + kEndpointAxisInsetPx) * scale,
            top,
            std::max(1.0f, w - (kPadLeftPx + kPadRightPx + 2.0f * kEndpointAxisInsetPx) * scale),
            std::max(1.0f, floorY - top)};
}

bool CurveField::PointInGraph(const GraphRect& r, float x, float y)
{
    return x >= r.X && x <= r.X + r.W && y >= r.Y && y <= r.Y + r.H;
}

bool CurveField::TangentEditingEnabled() const
{
    return m_Config.AllowTangentEditing && !m_Config.LogarithmicValues;
}

float CurveField::EffectiveValueMin() const
{
    if (m_Drag >= 0)
        return m_DragValueMin;
    // A logarithmic axis keeps its bottom: keys below it sit on the bottom edge rather than stretching
    // the axis down by decades the graph's subject never reaches.
    if (m_Config.LogarithmicValues)
        return std::max(m_Config.ValueMin, m_Config.LogarithmicFloor);
    float lo = m_Config.ValueMin;
    for (const Math::CurveKey& k : m_Keys)
        lo = std::min(lo, k.Value);
    return lo;
}

float CurveField::EffectiveValueMax() const
{
    if (m_Drag >= 0)
        return m_DragValueMax;
    float hi = m_Config.ValueMax;
    for (const Math::CurveKey& k : m_Keys)
        hi = std::max(hi, k.Value);
    return hi;
}

float CurveField::ToScreenX(const GraphRect& r, float time) const
{
    const float n = (time - m_Config.TimeMin) / SpanOrEpsilon(m_Config.TimeMin, m_Config.TimeMax);
    return r.X + std::clamp(n, 0.0f, 1.0f) * r.W;
}

float CurveField::ToScreenY(const GraphRect& r, float value) const
{
    const float lo = EffectiveValueMin();
    if (m_Config.LogarithmicValues)
    {
        const float logLo = std::log2(lo);
        const float logValue = std::log2(std::max(value, lo));
        const float n = (logValue - logLo) / SpanOrEpsilon(logLo, std::log2(std::max(EffectiveValueMax(), lo)));
        return r.Y + (1.0f - std::clamp(n, 0.0f, 1.0f)) * r.H;
    }
    const float n = (value - lo) / SpanOrEpsilon(lo, EffectiveValueMax());
    return r.Y + (1.0f - std::clamp(n, 0.0f, 1.0f)) * r.H;
}

float CurveField::FromScreenTime(const GraphRect& r, float px) const
{
    const float n = std::clamp((px - r.X) / r.W, 0.0f, 1.0f);
    return m_Config.TimeMin + n * SpanOrEpsilon(m_Config.TimeMin, m_Config.TimeMax);
}

float CurveField::FromScreenValue(const GraphRect& r, float py) const
{
    const float lo = EffectiveValueMin();
    const float n = std::clamp(1.0f - (py - r.Y) / r.H, 0.0f, 1.0f);
    if (m_Config.LogarithmicValues)
    {
        const float logLo = std::log2(lo);
        return std::exp2(logLo + n * SpanOrEpsilon(logLo, std::log2(std::max(EffectiveValueMax(), lo))));
    }
    return lo + n * SpanOrEpsilon(lo, EffectiveValueMax());
}

float CurveField::SampleAt(float time) const
{
    if (m_Keys.empty())
        return 0.0f;
    if (m_Config.LogarithmicValues)
        return Math::EvaluateCurveKeysLogarithmic(
            m_Keys.data(), static_cast<uint32_t>(m_Keys.size()), time, m_Config.LogarithmicFloor,
            m_Config.WrapAround ? SpanOrEpsilon(m_Config.TimeMin, m_Config.TimeMax) : 0.0f);
    // Cyclic curves (the sky day cycle) close the loop from the last key back to the first across the
    // [TimeMin, TimeMax) wrap gap; the shared evaluator keeps this preview identical to the runtime.
    if (m_Config.WrapAround)
        return Math::EvaluateCurveKeysWrapped(m_Keys.data(), static_cast<uint32_t>(m_Keys.size()), time,
                                              SpanOrEpsilon(m_Config.TimeMin, m_Config.TimeMax));
    return Math::EvaluateCurveKeys(m_Keys.data(), static_cast<uint32_t>(m_Keys.size()), time);
}

int CurveField::HitTestKey(float globalX, float globalY) const
{
    if (m_Keys.empty())
        return -1;
    const GraphRect r = CurrentGraphRect();
    const float pickSq = kHandlePickPx * kHandlePickPx;
    float bestSq = pickSq + 1.0f;
    int best = -1;
    for (size_t i = 0; i < m_Keys.size(); ++i)
    {
        const float hx = ToScreenX(r, m_Keys[i].Time);
        const float hy = ToScreenY(r, m_Keys[i].Value);
        const float dx = globalX - hx;
        const float dy = globalY - hy;
        const float dsq = dx * dx + dy * dy;
        if (dsq <= pickSq && dsq < bestSq)
        {
            bestSq = dsq;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void CurveField::TangentHandleScreen(const GraphRect& r, int keyIndex, bool incoming, float scale,
                                     float& outX, float& outY) const
{
    const Math::CurveKey& k = m_Keys[keyIndex];
    const float slope = Math::ResolveKeySlope(m_Keys.data(), static_cast<uint32_t>(m_Keys.size()),
                                              static_cast<uint32_t>(keyIndex), incoming);
    const float kx = ToScreenX(r, k.Time);
    const float ky = ToScreenY(r, k.Value);
    // Tangent direction in screen space: +time -> +x (sx px/unit), +value -> -y (sy px/unit).
    const float sx = r.W / SpanOrEpsilon(m_Config.TimeMin, m_Config.TimeMax);
    const float sy = r.H / SpanOrEpsilon(EffectiveValueMin(), EffectiveValueMax());
    float dx = sx;
    float dy = -slope * sy;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len > 1e-4f)
    {
        dx /= len;
        dy /= len;
    }
    const float reach = (incoming ? -1.0f : 1.0f) * kTangentReachPx * scale;
    outX = kx + dx * reach;
    outY = ky + dy * reach;
}

int CurveField::HitTestHandle(float globalX, float globalY) const
{
    // Tangent handles of the selected key win over points so the arms stay grabbable.
    if (TangentEditingEnabled() && m_Selected >= 0 && m_Selected < static_cast<int>(m_Keys.size()))
    {
        const GraphRect r = CurrentGraphRect();
        const float pickSq = kTangentPickPx * kTangentPickPx;
        const int n = static_cast<int>(m_Keys.size());
        const bool sides[2] = {m_Selected > 0, m_Selected + 1 < n};
        const bool incoming[2] = {true, false};
        for (int s = 0; s < 2; ++s)
        {
            if (!sides[s])
                continue;
            float hx = 0.0f;
            float hy = 0.0f;
            TangentHandleScreen(r, m_Selected, incoming[s], 1.0f, hx, hy);
            const float dx = globalX - hx;
            const float dy = globalY - hy;
            if (dx * dx + dy * dy <= pickSq)
                return incoming[s] ? CurveEdit::InTangentHandle(m_Selected)
                                   : CurveEdit::OutTangentHandle(m_Selected);
        }
    }
    return HitTestKey(globalX, globalY);
}

void CurveField::UpdateHover(float globalX, float globalY)
{
    const int next = (globalX >= 0.0f && globalY >= 0.0f) ? HitTestHandle(globalX, globalY) : -1;
    if (next == m_Hover)
        return;
    m_Hover = next;
    UpdateCursor();
    MarkDirty(VisualDirty);
}

void CurveField::UpdateCursor()
{
    // Grab hand only when over a draggable point/handle; grabbing while dragging; arrow elsewhere.
    const CursorStyle cursor = (m_Drag >= 0)      ? CursorStyle::Grabbing
                               : (m_Hover >= 0)   ? CursorStyle::Grab
                                                  : CursorStyle::Auto;
    Overrides().Set(Style::Cursor, cursor);
}

bool CurveField::TryHandleCurveDoubleClick(float globalX, float globalY)
{
    if (!m_OnCurveDoubleClick)
        return false;

    const float x = GetLayoutX();
    const float y = GetLayoutY();
    const float w = GetLayoutWidth();
    const float h = GetLayoutHeight();
    if (globalX < x || globalX > x + w || globalY < y || globalY > y + h)
    {
        m_HasLastCurveClick = false;
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const float dx = globalX - m_LastCurveClickX;
    const float dy = globalY - m_LastCurveClickY;
    const bool isDoubleClick =
        m_HasLastCurveClick &&
        (now - m_LastCurveClickTime) <= GameEngine::Platform::GetDoubleClickInterval() &&
        dx * dx + dy * dy <= kDoubleClickDistancePx * kDoubleClickDistancePx;

    m_LastCurveClickTime = now;
    m_LastCurveClickX = globalX;
    m_LastCurveClickY = globalY;
    m_HasLastCurveClick = !isDoubleClick;

    if (!isDoubleClick)
        return false;

    m_OnCurveDoubleClick();
    return true;
}

void CurveField::ApplyDrag(float globalX, float globalY)
{
    if (m_Drag < 0)
        return;
    const GraphRect r = CurrentGraphRect();

    if (CurveEdit::IsTangentHandle(m_Drag))
    {
        const int key = CurveEdit::TangentHandleKey(m_Drag);
        if (key < 0 || key >= static_cast<int>(m_Keys.size()))
            return;
        const bool incoming = CurveEdit::IsIncomingTangent(m_Drag);
        const float mTime = FromScreenTime(r, globalX);
        const float mValue = FromScreenValue(r, globalY);
        const float timeOffset =
            (incoming ? -1.0f : 1.0f) * std::max(std::abs(mTime - m_Keys[key].Time), 1e-3f);
        const float slope = (mValue - m_Keys[key].Value) / timeOffset;
        const bool broken = m_Keys[key].TangentMode == Math::CurveTangentMode::Broken;
        CurveEdit::ApplyTangentSlope(m_Keys, key, incoming, slope, broken);
        m_DragMoved = true;
        if (m_OnChanging)
            m_OnChanging(m_Keys);
        MarkDirty(VisualDirty);
        return;
    }

    if (m_Drag >= static_cast<int>(m_Keys.size()))
        return;
    const float anchorX = globalX + m_GrabOffsetX;
    const float anchorY = globalY + m_GrabOffsetY;

    Math::CurveKey& k = m_Keys[m_Drag];
    k.Value = FromScreenValue(r, anchorY);
    if (m_Config.AllowTimeDrag)
    {
        const int n = static_cast<int>(m_Keys.size());
        const float gap = kMinTimeGapFrac * std::abs(SpanOrEpsilon(m_Config.TimeMin, m_Config.TimeMax));
        const float lo = (m_Drag > 0) ? m_Keys[m_Drag - 1].Time + gap : m_Config.TimeMin;
        const float hi = (m_Drag < n - 1) ? m_Keys[m_Drag + 1].Time - gap : m_Config.TimeMax;
        k.Time = (lo <= hi) ? std::clamp(FromScreenTime(r, anchorX), lo, hi) : k.Time;
    }

    m_DragMoved = true;
    m_HudValue = k.Value;
    m_HudTime = k.Time;

    if (m_OnChanging)
        m_OnChanging(m_Keys);
    MarkDirty(VisualDirty);
}

void CurveField::AddKeyAt(float globalX, float globalY)
{
    if (static_cast<int>(m_Keys.size()) >= m_Config.MaxKeys)
        return;
    const GraphRect r = CurrentGraphRect();
    Math::CurveKey key;
    key.Time = FromScreenTime(r, globalX);
    key.Value = FromScreenValue(r, globalY);
    key.Interp = Math::CurveInterp::Linear;

    int pos = 0;
    while (pos < static_cast<int>(m_Keys.size()) && m_Keys[pos].Time < key.Time)
        ++pos;
    m_Keys.insert(m_Keys.begin() + pos, key);
    SetSelectedInternal(pos, true);
    if (m_OnChanged)
        m_OnChanged(m_Keys);
    MarkDirty(VisualDirty);
}

void CurveField::RemoveKey(int index)
{
    if (index < 0 || index >= static_cast<int>(m_Keys.size()))
        return;
    if (static_cast<int>(m_Keys.size()) <= m_Config.MinKeys)
        return;
    m_Keys.erase(m_Keys.begin() + index);
    int newSel = m_Selected;
    if (m_Selected == index)
        newSel = -1;
    else if (m_Selected > index)
        newSel = m_Selected - 1;
    SetSelectedInternal(newSel, true);
    if (m_OnChanged)
        m_OnChanged(m_Keys);
    MarkDirty(VisualDirty);
}

void CurveField::SetKeyMode(int index, Math::CurveInterp interp, Math::CurveTangentMode mode)
{
    if (index < 0 || index >= static_cast<int>(m_Keys.size()))
        return;
    CurveEdit::SetKeyMode(m_Keys, index, interp, mode);
    MarkDirty(VisualDirty);
    if (m_OnChanged)
        m_OnChanged(m_Keys);
}

void CurveField::DeleteKey(int index)
{
    RemoveKey(index);
}

void CurveField::ApplyPlaybackScrub(float globalX)
{
    const GraphRect r = CurrentGraphRect();
    const float time = FromScreenTime(r, globalX);
    m_PlaybackTime = time;
    m_PlaybackValue = SampleAt(time);
    m_PlaybackIndicatorVisible = true;
    if (m_OnPlaybackScrub)
        m_OnPlaybackScrub(time);
    MarkDirty(VisualDirty);
}

bool CurveField::ApplyRightClickAction(float globalX, float globalY, int preferredHit)
{
    const int hit = (preferredHit >= 0) ? preferredHit : HitTestKey(globalX, globalY);
    if (hit >= 0)
    {
        if (m_OnKeyContextMenu)
        {
            SetSelectedInternal(hit, true);
            m_OnKeyContextMenu(hit, globalX, globalY);
            return true;
        }
        return false;
    }

    if (m_Config.AllowAddRemove && PointInGraph(CurrentGraphRect(), globalX, globalY))
    {
        AddKeyAt(globalX, globalY);
        return true;
    }
    return false;
}

void CurveField::OnEvent(UIEvent& e)
{
    if (m_Config.ReadOnly)
        return;

    if (e.Id == kEventKeyDown && e.Mods == 0 &&
        (e.Key == Input::kKeyCode_Delete || e.Key == Input::kKeyCode_Backspace))
    {
        if (m_Config.AllowAddRemove && m_Selected >= 0 &&
            m_Selected < static_cast<int>(m_Keys.size()) &&
            static_cast<int>(m_Keys.size()) > m_Config.MinKeys)
        {
            RemoveKey(m_Selected);
        }
        e.Stop();
        return;
    }

    if (m_PlaybackScrubActive)
    {
        if (e.Id == kEventMouseMove)
        {
            const float dx = e.X - m_PlaybackScrubStartX;
            const float dy = e.Y - m_PlaybackScrubStartY;
            if (m_PlaybackScrubStarted || dx * dx + dy * dy >= kDragThresholdPx * kDragThresholdPx)
            {
                m_PlaybackScrubStarted = true;
                ApplyPlaybackScrub(e.X);
            }
            e.Stop();
            return;
        }

        if (e.Id == kEventMouseUp)
        {
            ApplyPlaybackScrub(e.X);

            m_PlaybackScrubActive = false;
            m_PlaybackScrubStarted = false;
            m_PlaybackScrubStartHit = -1;
            m_PlaybackScrubStartX = 0.0f;
            m_PlaybackScrubStartY = 0.0f;
            if (m_OnPlaybackScrubActiveChanged)
                m_OnPlaybackScrubActiveChanged(false);
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
    }

    if (e.Id == kEventMouseMove && m_Drag < 0)
    {
        UpdateHover(e.X, e.Y);
        return;
    }
    if (e.Id == kEventMouseLeave)
    {
        m_HasLastCurveClick = false;
        UpdateHover(-1.0f, -1.0f);
        return;
    }

    // Right-click: a key opens a per-key context menu when the host provides one; empty
    // graph space adds a key. Delete stays explicit through the context menu.
    if (e.Id == kEventMouseDown && e.Button == 1)
    {
        if (ApplyRightClickAction(e.X, e.Y, -1))
            e.Stop();
        return;
    }

    if (e.Button != 0)
        return;

    if (e.Id == kEventMouseDown)
    {
        if (TryHandleCurveDoubleClick(e.X, e.Y))
        {
            e.Stop();
            return;
        }

        const int hit = HitTestHandle(e.X, e.Y);
        if (hit < 0)
        {
            SetSelectedInternal(-1, true);
            if (m_Config.AllowPlaybackScrub && m_Config.ShowPlaybackIndicator && PointInGraph(CurrentGraphRect(), e.X, e.Y))
            {
                m_PlaybackScrubActive = true;
                m_PlaybackScrubStarted = false;
                m_PlaybackScrubStartHit = -1;
                m_PlaybackScrubStartX = e.X;
                m_PlaybackScrubStartY = e.Y;
                if (m_OnPlaybackScrubActiveChanged)
                    m_OnPlaybackScrubActiveChanged(true);
                e.Capture(this);
                e.Stop();
            }
            return;
        }

        m_DragValueMin = EffectiveValueMin();
        m_DragValueMax = EffectiveValueMax();
        m_Drag = hit;
        m_DragMoved = false;
        m_DragStartX = e.X;
        m_DragStartY = e.Y;
        UpdateCursor();

        if (CurveEdit::IsTangentHandle(hit))
        {
            // Tangent drag: slope derives from the mouse; selection stays on the owning key.
            e.Capture(this);
            e.Stop();
            return;
        }

        SetSelectedInternal(hit, true);
        const GraphRect r = CurrentGraphRect();
        const float hx = ToScreenX(r, m_Keys[hit].Time);
        const float hy = ToScreenY(r, m_Keys[hit].Value);
        m_GrabOffsetX = hx - e.X;
        m_GrabOffsetY = hy - e.Y;
        m_HudValue = m_Keys[hit].Value;
        m_HudTime = m_Keys[hit].Time;

        e.Capture(this);
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseMove && m_Drag >= 0)
    {
        if (!m_DragMoved)
        {
            const float dx = e.X - m_DragStartX;
            const float dy = e.Y - m_DragStartY;
            if (dx * dx + dy * dy < kDragThresholdPx * kDragThresholdPx)
            {
                e.Stop();
                return;
            }
        }
        ApplyDrag(e.X, e.Y);
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseUp && m_Drag >= 0)
    {
        const bool moved = m_DragMoved;
        if (moved)
            ApplyDrag(e.X, e.Y);
        m_Drag = -1;
        m_DragMoved = false;
        UpdateCursor();
        MarkDirty(VisualDirty);
        if (moved && m_OnChanged)
            m_OnChanged(m_Keys);
        e.Stop();
    }
}

void CurveField::DrawHandle(UI::PrimitiveEmitContext& ctx, float x, float y, uint32_t color, bool active,
                            float maxSizePx)
{
    const float cs = ctx.ContentScale;
    const float logicalSize = HandleLogicalSize(active, maxSizePx);
    const float size = logicalSize * cs;
    UI::UIPrimitive shadow = UI::MakeRect(x - size * 0.5f - 1.0f * cs, y - size * 0.5f + 1.0f * cs,
                                          size + 2.0f * cs, size + 2.0f * cs,
                                          UI::PackColor(0.0f, 0.0f, 0.0f, 0.35f),
                                          4.0f * cs, 4.0f * cs, 4.0f * cs, 4.0f * cs);
    ctx.Emit(shadow);
    UI::UIPrimitive handle = UI::MakeRect(x - size * 0.5f, y - size * 0.5f, size, size, color,
                                          4.0f * cs, 4.0f * cs, 4.0f * cs, 4.0f * cs);
    UI::AddBorder(handle, (active ? 2.0f : 1.0f) * cs, UI::PackColor(0.98f, 0.98f, 0.98f, 0.95f));
    ctx.Emit(handle);
}

void CurveField::DrawPlaybackIndicator(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                                       const GraphRect& r) const
{
    if (!m_Config.ShowPlaybackIndicator || !m_PlaybackIndicatorVisible)
        return;

    const float cs = ctx.ContentScale;
    const float px = ToScreenX(r, m_PlaybackTime);
    const float py = ToScreenY(r, std::clamp(m_PlaybackValue, EffectiveValueMin(), EffectiveValueMax()));
    const float outputDotX = r.X - 0.5f * (kPadLeftPx + kEndpointAxisInsetPx) * cs;
    const float outputDotY = py;

    const uint32_t lineColor = UI::PackColor(0.20f, 0.92f, 0.82f, 0.85f);
    const uint32_t dotArgb = Editor::CurveEditorSettings::Get().PlaybackIndicatorDotColor;
    const uint32_t dotColor = UI::PackFromARGB(dotArgb);
    const uint32_t glowColor = UI::PackFromARGB(WithARGBAlpha(dotArgb, 0x66u));
    const uint32_t dotBorderColor = UI::PackColor(0.94f, 1.0f, 0.96f, 1.0f);
    const uint32_t outputDotColor = UI::PackColor(0.78f, 0.98f, 0.86f, 1.0f);
    const uint32_t outputDotBorderColor = UI::PackColor(0.90f, 1.0f, 0.94f, 1.0f);

    ctx.Emit(UI::MakeLine(px, r.Y, px, r.Y + r.H, 1.75f * cs, lineColor));

    if (m_Config.ShowPlaybackValueMarker)
    {
        UI::UIPrimitive outputDot = UI::MakeRect(outputDotX - 4.0f * cs, outputDotY - 4.0f * cs,
                                                 8.0f * cs, 8.0f * cs, outputDotColor,
                                                 4.0f * cs, 4.0f * cs, 4.0f * cs, 4.0f * cs);
        UI::AddBorder(outputDot, 1.5f * cs, outputDotBorderColor);
        ctx.Emit(outputDot);
        // The marker says what it marks: the curve's value now, beside it inside the graph.
        if (m_Config.ValueLabel && ctx.FontAtlas)
        {
            const std::string text = m_Config.ValueLabel(m_PlaybackValue);
            const float textY = std::clamp(outputDotY - 0.5f * m_TimeLabelFontSizePx * kLabelLineHeightRatio * cs, r.Y,
                                           std::max(r.Y, r.Y + r.H - m_TimeLabelFontSizePx * kLabelLineHeightRatio * cs));
            ctx.EmitText(text, r.X + 6.0f * cs, textY, m_TimeLabelFontSizePx,
                         style.GetCustomColor(kPlaybackLabelColorVar).value_or(style.Visual.Color), ctx.FontAtlas);
        }
    }

    UI::UIPrimitive glow = UI::MakeRect(px - 7.0f * cs, py - 7.0f * cs,
                                        14.0f * cs, 14.0f * cs,
                                        glowColor,
                                        7.0f * cs, 7.0f * cs, 7.0f * cs, 7.0f * cs);
    ctx.Emit(glow);

    const float dotRadius = kPlaybackDotRadiusPx * cs;
    UI::UIPrimitive dot = UI::MakeRect(px - dotRadius, py - dotRadius, 2.0f * dotRadius, 2.0f * dotRadius, dotColor,
                                       dotRadius, dotRadius, dotRadius, dotRadius);
    UI::AddBorder(dot, 2.0f * cs, dotBorderColor);
    ctx.Emit(dot);
}

void CurveField::DrawReferenceLine(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                                   const GraphRect& r) const
{
    if (m_ReferenceLine.size() < 2)
        return;
    const uint32_t color = StyledColor(style, kReferenceLineColorVar);
    const float thickness = kReferenceLineThicknessPx * ctx.ContentScale;
    float prevX = ToScreenX(r, m_ReferenceLine.front().Time);
    float prevY = ToScreenY(r, m_ReferenceLine.front().Value);
    for (size_t i = 1; i < m_ReferenceLine.size(); ++i)
    {
        const float px = ToScreenX(r, m_ReferenceLine[i].Time);
        const float py = ToScreenY(r, m_ReferenceLine[i].Value);
        ctx.Emit(UI::MakeLine(prevX, prevY, px, py, thickness, color));
        prevX = px;
        prevY = py;
    }
}

void CurveField::DrawShadedSpans(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style, const GraphRect& r) const
{
    const uint32_t color = StyledColor(style, kShadedSpanColorVar);
    const float cs = ctx.ContentScale;
    for (const ShadedSpan& span : m_ShadedSpans)
    {
        const float left = ToScreenX(r, span.Start);
        const float right = ToScreenX(r, span.End);
        if (right - left < 1.0f)
            continue;
        ctx.Emit(UI::MakeRect(left, r.Y, right - left, r.H, color, 0.0f, 0.0f, 0.0f, 0.0f));
        if (span.Label.empty() || !ctx.FontAtlas)
            continue;
        const float pixelSize = std::max(1.0f, m_TimeLabelFontSizePx * cs);
        const float textW = ctx.FontAtlas->MeasureUtf8(span.Label, pixelSize).width;
        if (textW + 8.0f * cs > right - left)
            continue;
        ctx.EmitText(span.Label, 0.5f * (left + right - textW), r.Y + 2.0f * cs, m_TimeLabelFontSizePx,
                     style.Visual.Color, ctx.FontAtlas);
    }
}

void CurveField::DrawValueAxisMarks(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style, const GraphRect& r) const
{
    const uint32_t color = StyledColor(style, kValueMarkColorVar);
    const float cs = ctx.ContentScale;
    const float lo = EffectiveValueMin();
    const float hi = EffectiveValueMax();
    for (const float value : m_Config.ValueAxisMarks)
    {
        if (value < lo || value > hi)
            continue;
        const float py = ToScreenY(r, value);
        ctx.Emit(UI::MakeLine(r.X, py, r.X + r.W, py, 1.0f * cs, color));
        if (!m_Config.ValueLabel || !ctx.FontAtlas)
            continue;
        const float lineHeight = m_TimeLabelFontSizePx * kLabelLineHeightRatio * cs;
        ctx.EmitText(m_Config.ValueLabel(value), r.X + r.W - 4.0f * cs -
                         ctx.FontAtlas->MeasureUtf8(m_Config.ValueLabel(value), std::max(1.0f, m_TimeLabelFontSizePx * cs)).width,
                     std::max(r.Y, py - lineHeight), m_TimeLabelFontSizePx, style.Visual.Color, ctx.FontAtlas);
    }
}

// Hour labels follow the vertical grid, omitting alternate labels when space is tight.
// The playback time appears in a pill
// where the playback line meets them; an hour label the pill would cover is left out.
void CurveField::DrawTimeLabels(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style, const GraphRect& r,
                                float x, float w) const
{
    if (m_Config.TimeAxisLabels == TimeLabels::None || !ctx.FontAtlas)
        return;

    const float cs = ctx.ContentScale;
    const float fontSize = m_TimeLabelFontSizePx;
    const float pixelSize = std::max(1.0f, fontSize * cs);
    const float textY = r.Y + r.H + (kPlaybackDotRadiusPx + kPlaybackDotClearancePx) * cs;
    const float left = x + kLabelGapPx * cs;
    const float right = x + w - kLabelGapPx * cs;
    const uint32_t labelArgb = style.Visual.Color;

    float pillLeft = 0.0f;
    float pillRight = 0.0f;
    std::string playbackText;
    const bool showPlayback = m_Config.ShowPlaybackIndicator && m_PlaybackIndicatorVisible;
    if (showPlayback)
    {
        playbackText = HoursOfDayLabel(m_PlaybackTime);
        const float padding = kPlaybackLabelPaddingPx * cs;
        const float textW = ctx.FontAtlas->MeasureUtf8(playbackText, pixelSize).width;
        const float pillW = textW + 2.0f * padding;
        pillLeft = std::clamp(ToScreenX(r, m_PlaybackTime) - 0.5f * pillW, left, std::max(left, right - pillW));
        pillRight = pillLeft + pillW;
    }

    const float labelWidth = ctx.FontAtlas->MeasureUtf8("00:00", pixelSize).width;
    const float minimumGap = ctx.FontAtlas->MeasureUtf8("000", pixelSize).width;
    const int labelStep = r.W / static_cast<float>(kGridSpans) < labelWidth + minimumGap ? 2 : 1;
    for (int i = 0; i <= kGridSpans; i += labelStep)
    {
        const float time = m_Config.TimeMin + (m_Config.TimeMax - m_Config.TimeMin) * static_cast<float>(i) /
                                                  static_cast<float>(kGridSpans);
        const std::string text = HoursOfDayLabel(time);
        const float textW = ctx.FontAtlas->MeasureUtf8(text, pixelSize).width;
        const float textX = std::clamp(ToScreenX(r, time) - 0.5f * textW, left, std::max(left, right - textW));
        if (showPlayback && textX < pillRight && textX + textW > pillLeft)
            continue;
        ctx.EmitText(text, textX, textY, fontSize, labelArgb, ctx.FontAtlas);
    }

    if (!showPlayback)
        return;
    const float lineHeight = fontSize * kLabelLineHeightRatio * cs;
    UI::UIPrimitive pill = UI::MakeRect(pillLeft, textY, pillRight - pillLeft, lineHeight,
                                        StyledColor(style, kPlaybackLabelBackgroundVar),
                                        4.0f * cs, 4.0f * cs, 4.0f * cs, 4.0f * cs);
    ctx.Emit(pill);
    const uint32_t pillTextArgb = style.GetCustomColor(kPlaybackLabelColorVar).value_or(0u);
    ctx.EmitText(playbackText, pillLeft + kPlaybackLabelPaddingPx * cs, textY, fontSize, pillTextArgb,
                 ctx.FontAtlas);
}

void CurveField::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                                      float x, float y, float w, float h)
{
    // Parallel-drain thread contract: custom emission may touch shared
    // text/measure state, so it never runs on a JobSystem worker — escalate
    // and let the drain re-emit this element on the UI thread.
    if (ctx.OffThread)
    {
        if (ctx.EscalateFlag)
            *ctx.EscalateFlag = true;
        return;
    }

    if (w <= 1.0f || h <= 1.0f)
        return;

    const float cs = ctx.ContentScale;
    const GraphRect r = ComputeGraphRect(x, y, w, h, cs);
    const float logicalHeight = h / std::max(cs, 1e-6f);
    const bool compactEndpointHandles = logicalHeight < kCompactEndpointHeightThresholdPx;

    UI::UIPrimitive bg = UI::MakeRect(x, y, w, h, UI::PackColor(0.10f, 0.11f, 0.12f, 0.96f),
                                      6.0f * cs, 6.0f * cs, 6.0f * cs, 6.0f * cs);
    UI::AddBorder(bg, 1.0f * cs, UI::PackColor(0.28f, 0.31f, 0.34f, 0.95f));
    ctx.Emit(bg);

    const uint32_t gridMajor = UI::PackColor(0.35f, 0.38f, 0.42f, 0.36f);
    const uint32_t gridMinor = UI::PackColor(0.35f, 0.38f, 0.42f, 0.18f);
    for (int i = 0; i <= kGridSpans; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(kGridSpans);
        const float gx = r.X + t * r.W;
        const float gy = r.Y + t * r.H;
        const float thick = (i == 0 || i == kGridSpans) ? 1.4f * cs : 1.0f * cs;
        const uint32_t color = (i == kGridSpans / 2) ? gridMajor : gridMinor;
        ctx.Emit(UI::MakeLine(gx, r.Y, gx, r.Y + r.H, thick, color));
        ctx.Emit(UI::MakeLine(r.X, gy, r.X + r.W, gy, thick, color));
    }

    DrawShadedSpans(ctx, style, r);
    DrawValueAxisMarks(ctx, style, r);
    DrawReferenceLine(ctx, style, r);

    // Read-only scrubber playhead (e.g. the sky's current time of day), drawn behind the curve and
    // handles so it never obscures them: a vertical line + a small marker at the top edge.
    if (m_Config.ShowScrubber)
    {
        const float sx = ToScreenX(r, m_ScrubberTime);
        ctx.Emit(UI::MakeLine(sx, r.Y, sx, r.Y + r.H, 1.5f * cs, kScrubberColor));
        const float m = 4.0f * cs;
        ctx.Emit(UI::MakeTriangle(sx - m, r.Y, sx + m, r.Y, sx, r.Y + m * 1.4f, kScrubberColor));
    }

    const float lineThick = kCurveLineThicknessPx * cs;
    if (!m_Keys.empty())
    {
        // Sample the resolved curve so Smooth/Step keys render correctly. SampleAt closes
        // the cycle (WrapAround) or extends the ends flat (clamped) outside the key range.
        constexpr int kSamples = 72;
        const float span = m_Config.TimeMax - m_Config.TimeMin;
        float prevX = 0.0f;
        float prevY = 0.0f;
        for (int s = 0; s < kSamples; ++s)
        {
            const float t = m_Config.TimeMin + span * static_cast<float>(s) / static_cast<float>(kSamples - 1);
            const float px = ToScreenX(r, t);
            const float py = ToScreenY(r, SampleAt(t));
            if (s > 0)
                ctx.Emit(UI::MakeLine(prevX, prevY, px, py, lineThick, kCurveLineColor));
            prevX = px;
            prevY = py;
        }
    }

    if (!m_Config.ReadOnly)
    {
        for (size_t i = 0; i < m_Keys.size(); ++i)
        {
            const bool endpoint = (i == 0 || i + 1 == m_Keys.size());
            const bool active = (m_Drag == static_cast<int>(i)) ||
                                (m_Drag < 0 && m_Hover == static_cast<int>(i)) ||
                                (m_Drag < 0 && m_Selected == static_cast<int>(i));
            const float maxHandleSize = (endpoint && compactEndpointHandles) ? kNormalHandleSizePx : 0.0f;
            const float handleSize = HandleLogicalSize(active, maxHandleSize);
            const float hx = ToScreenX(r, m_Keys[i].Time);
            const float hy = ClampHandleCenter(r.Y, r.Y + r.H, ToScreenY(r, m_Keys[i].Value), handleSize, cs);
            DrawHandle(ctx, hx, hy,
                       endpoint ? kEndpointHandleColor : kInteriorHandleColor, active, maxHandleSize);
        }
    }

    if (!m_Config.ReadOnly && m_Config.WrapAround && !m_Keys.empty() &&
        std::abs(m_Keys.back().Time - m_Config.TimeMax) > 1e-4f)
    {
        const bool active = (m_Drag == 0) || (m_Drag < 0 && m_Selected == 0);
        const float maxHandleSize = compactEndpointHandles ? kNormalHandleSizePx : 0.0f;
        const float handleSize = HandleLogicalSize(active, maxHandleSize);
        const float hx = ToScreenX(r, m_Config.TimeMax);
        const float hy = ClampHandleCenter(r.Y, r.Y + r.H, ToScreenY(r, m_Keys.front().Value), handleSize, cs);
        DrawHandle(ctx, hx, hy, kEndpointHandleColor, active, maxHandleSize);
    }

    // Tangent arms on the selected key (smooth editing).
    if (TangentEditingEnabled() && m_Selected >= 0 && m_Selected < static_cast<int>(m_Keys.size()))
    {
        const int n = static_cast<int>(m_Keys.size());
        const float kx = ToScreenX(r, m_Keys[m_Selected].Time);
        const float ky = ToScreenY(r, m_Keys[m_Selected].Value);
        const uint32_t armColor = UI::PackColor(0.70f, 0.56f, 0.94f, 0.82f);
        const uint32_t handleColor = UI::PackColor(0.74f, 0.56f, 0.98f, 1.0f);
        const bool sides[2] = {m_Selected > 0, m_Selected + 1 < n};
        const bool incoming[2] = {true, false};
        const int ids[2] = {CurveEdit::InTangentHandle(m_Selected), CurveEdit::OutTangentHandle(m_Selected)};
        for (int s = 0; s < 2; ++s)
        {
            if (!sides[s])
                continue;
            float hx = 0.0f;
            float hy = 0.0f;
            TangentHandleScreen(r, m_Selected, incoming[s], cs, hx, hy);
            ctx.Emit(UI::MakeLine(kx, ky, hx, hy, 1.5f * cs, armColor));
            const bool active = (m_Drag == ids[s]) || (m_Drag < 0 && m_Hover == ids[s]);
            DrawHandle(ctx, hx, hy, handleColor, active);
        }
    }

    DrawPlaybackIndicator(ctx, style, r);
    DrawTimeLabels(ctx, style, r, x, w);

    if (m_Drag >= 0 && !CurveEdit::IsTangentHandle(m_Drag) && m_DragMoved && ctx.FontAtlas && ctx.Textures)
    {
        constexpr float kHudW = 128.0f;
        constexpr float kHudH = 22.0f;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "t %.2f  v %.3f", static_cast<double>(m_HudTime),
                      static_cast<double>(m_HudValue));

        // Anchor in physical space from the key's time/value; do not cache logical screen coords
        // across the event/paint boundary (they would misplace the HUD at content scale != 1).
        const float anchorX = ToScreenX(r, m_HudTime);
        const float anchorY = ToScreenY(r, m_HudValue);
        float boxX = anchorX + 14.0f * cs;
        float boxY = anchorY - (kHudH + 14.0f) * cs;
        boxX = std::clamp(boxX, x + 4.0f * cs, std::max(x + 4.0f * cs, x + w - (kHudW + 4.0f) * cs));
        boxY = std::clamp(boxY, y + 4.0f * cs, std::max(y + 4.0f * cs, y + h - (kHudH + 4.0f) * cs));

        UI::UIPrimitive hud = UI::MakeRect(boxX, boxY, kHudW * cs, kHudH * cs,
                                           UI::PackColor(0.06f, 0.07f, 0.08f, 0.92f),
                                           4.0f * cs, 4.0f * cs, 4.0f * cs, 4.0f * cs);
        UI::AddBorder(hud, 1.0f * cs, UI::PackColor(0.40f, 0.44f, 0.48f, 0.90f));
        ctx.Emit(hud);
        // EmitText scales fontSize by contentScale internally, so pass logical px.
        ctx.EmitText(buf, boxX + 8.0f * cs, boxY + 4.0f * cs, 12.0f,
                     style.Visual.Color, ctx.FontAtlas);
    }
}

} // namespace GameEngine
