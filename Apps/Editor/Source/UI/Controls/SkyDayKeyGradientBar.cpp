#include "UI/Controls/SkyDayKeyGradientBar.h"

#include "UI/StyleProperties.h"
#include "UI/UIPrimitive.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace GameEngine
{

namespace
{
constexpr float kBarHeightPx = 12.0f;
constexpr float kMarkerSizePx = 8.0f;
constexpr float kMarkerEdgeInsetPx = 1.0f;
constexpr float kBarRadiusPx = 4.0f;
constexpr float kDragThresholdPx = 3.0f;
} // namespace

SkyDayKeyGradientBar::SkyDayKeyGradientBar()
{
    AddClass("sky-day-key-gradient-bar");
    Overrides()
        .Set(Style::Height, StyleLength::Px(18.0f))
        .Set(Style::MinHeight, StyleLength::Px(18.0f))
        .Set(Style::Cursor, CursorStyle::Pointer);
}

void SkyDayKeyGradientBar::SetStopColor(size_t stopIndex, uint32_t argb)
{
    if (stopIndex >= kStopCount)
        return;
    if (m_StopColors[stopIndex] == argb)
        return;
    m_StopColors[stopIndex] = argb;
    MarkDirty(VisualDirty);
}

void SkyDayKeyGradientBar::SetStopPosition(size_t stopIndex, float normalized01)
{
    if (stopIndex >= kStopCount)
        return;
    const float clamped = std::clamp(normalized01, 0.0f, 1.0f);
    if (std::fabs(m_StopPositions[stopIndex] - clamped) <= 1e-5f)
        return;
    m_StopPositions[stopIndex] = clamped;
    MarkDirty(VisualDirty);
}

void SkyDayKeyGradientBar::SetSelectedStop(size_t stopIndex)
{
    const size_t clamped = (stopIndex < kStopCount) ? stopIndex : kStopCount;
    if (m_SelectedStop == clamped)
        return;
    m_SelectedStop = clamped;
    MarkDirty(VisualDirty);
}

void SkyDayKeyGradientBar::ClearSelectedStop()
{
    SetSelectedStop(kStopCount);
}

size_t SkyDayKeyGradientBar::GetNearestStopIndex(float globalX) const
{
    const float w = std::max(1.0f, GetLayoutWidth());
    const float localX = std::clamp(globalX - GetLayoutX(), 0.0f, w);
    const float markerHalf = kMarkerSizePx * 0.5f;
    const float minX = markerHalf + kMarkerEdgeInsetPx;
    const float maxX = std::max(minX, w - markerHalf - kMarkerEdgeInsetPx);
    const float span = std::max(1e-3f, maxX - minX);
    size_t nearest = 0;
    float bestDist = std::numeric_limits<float>::max();
    for (size_t i = 0; i < kStopCount; ++i)
    {
        const float stopX = minX + span * std::clamp(m_StopPositions[i], 0.0f, 1.0f);
        const float d = std::fabs(localX - stopX);
        if (d < bestDist)
        {
            bestDist = d;
            nearest = i;
        }
    }
    return nearest;
}

float SkyDayKeyGradientBar::GetNormalizedPosition(float globalX) const
{
    const float w = std::max(1.0f, GetLayoutWidth());
    const float markerHalf = kMarkerSizePx * 0.5f;
    const float minX = markerHalf + kMarkerEdgeInsetPx;
    const float maxX = std::max(minX, w - markerHalf - kMarkerEdgeInsetPx);
    const float span = std::max(1e-3f, maxX - minX);
    const float localX = std::clamp(globalX - GetLayoutX(), 0.0f, w);
    return std::clamp((localX - minX) / span, 0.0f, 1.0f);
}

size_t SkyDayKeyGradientBar::GetHoveredStopIndex(float globalX, float globalY) const
{
    const float w = std::max(1.0f, GetLayoutWidth());
    const float h = std::max(1.0f, GetLayoutHeight());
    const float localX = std::clamp(globalX - GetLayoutX(), 0.0f, w);
    const float localY = std::clamp(globalY - GetLayoutY(), 0.0f, h);
    const float markerHalf = kMarkerSizePx * 0.5f;
    const float minX = markerHalf + kMarkerEdgeInsetPx;
    const float maxX = std::max(minX, w - markerHalf - kMarkerEdgeInsetPx);
    const float span = std::max(1e-3f, maxX - minX);
    const float markerCenterY = std::max(markerHalf, std::min(h - markerHalf, h * 0.5f));
    const float hoverRadius = markerHalf + 2.0f;

    std::array<float, kStopCount> stopPositions = m_StopPositions;
    for (size_t i = 0; i < kStopCount; ++i)
        stopPositions[i] = std::clamp(stopPositions[i], 0.0f, 1.0f);
    for (size_t i = 1; i < kStopCount; ++i)
        stopPositions[i] = std::max(stopPositions[i], stopPositions[i - 1]);

    size_t hovered = kStopCount;
    float bestDistSq = std::numeric_limits<float>::max();
    for (size_t i = 0; i < kStopCount; ++i)
    {
        const float stopX = minX + span * stopPositions[i];
        const float dx = localX - stopX;
        const float dy = localY - markerCenterY;
        const float distSq = dx * dx + dy * dy;
        if (distSq <= hoverRadius * hoverRadius && distSq < bestDistSq)
        {
            bestDistSq = distSq;
            hovered = i;
        }
    }
    return hovered;
}

void SkyDayKeyGradientBar::OnEvent(UIEvent& e)
{
    if (e.Id == kEventMouseDown && e.Button == 1 && m_OnContextMenu)
    {
        const size_t hovered = GetHoveredStopIndex(e.X, e.Y);
        const size_t stop = hovered < kStopCount ? hovered : GetNearestStopIndex(e.X);
        if (stop < kStopCount)
            SetSelectedStop(stop);
        m_OnContextMenu(e.X, e.Y, stop, GetNormalizedPosition(e.X));
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseDown && e.Button == 0)
    {
        m_PrimaryDown = true;
        m_Dragged = false;
        m_DownX = e.X;
        m_DownY = e.Y;

        const size_t nearest = GetNearestStopIndex(e.X);
        m_DownStop = nearest;
        if (nearest != m_SelectedStop)
        {
            SetSelectedStop(nearest);
            if (m_OnStopSelected)
                m_OnStopSelected(nearest);
        }
        e.Capture(this);
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseMove && m_PrimaryDown)
    {
        const float dx = e.X - m_DownX;
        const float dy = e.Y - m_DownY;
        if (!m_Dragged && (dx * dx + dy * dy) > (kDragThresholdPx * kDragThresholdPx))
            m_Dragged = true;
        if (m_Dragged)
        {
            const float markerHalf = kMarkerSizePx * 0.5f;
            const float w = std::max(1.0f, GetLayoutWidth());
            const float minX = markerHalf + kMarkerEdgeInsetPx;
            const float maxX = std::max(minX, w - markerHalf - kMarkerEdgeInsetPx);
            const float span = std::max(1e-3f, maxX - minX);
            const float localX = std::clamp(e.X - GetLayoutX(), 0.0f, w);
            const float normalized = std::clamp((localX - minX) / span, 0.0f, 1.0f);
            SetStopPosition(m_DownStop, normalized);
            if (m_OnStopPositionChanging)
                m_OnStopPositionChanging(m_DownStop, normalized);
        }
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseMove && !m_PrimaryDown)
    {
        const size_t hovered = GetHoveredStopIndex(e.X, e.Y);
        if (hovered != m_HoveredStop)
        {
            m_HoveredStop = hovered;
            MarkDirty(VisualDirty);
        }
        return;
    }

    if (e.Id == kEventMouseUp && e.Button == 0 && m_PrimaryDown)
    {
        if (m_Dragged)
        {
            if (m_OnStopPositionChanged)
                m_OnStopPositionChanged(m_DownStop, m_StopPositions[m_DownStop]);
        }
        else if (m_OnStopActivated)
        {
            m_OnStopActivated(m_DownStop);
        }

        ClearSelectedStop();
        m_PrimaryDown = false;
        m_Dragged = false;
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseLeave)
    {
        if (m_HoveredStop != kStopCount)
        {
            m_HoveredStop = kStopCount;
            MarkDirty(VisualDirty);
        }
    }
}

void SkyDayKeyGradientBar::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                                                const ResolvedStyle& /*style*/,
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

    if (w <= 0.0f || h <= 0.0f)
        return;

    const float cs = std::max(1e-3f, ctx.ContentScale);
    const float markerSize = kMarkerSizePx * cs;
    const float markerHalf = markerSize * 0.5f;
    const float barY = y + std::max(0.0f, (h - kBarHeightPx * cs) * 0.5f);
    const float barH = std::min(h, kBarHeightPx * cs);
    const float barRadius = kBarRadiusPx * cs;

    std::array<float, kStopCount> stopPositions = m_StopPositions;
    for (size_t i = 0; i < kStopCount; ++i)
        stopPositions[i] = std::clamp(stopPositions[i], 0.0f, 1.0f);
    for (size_t i = 1; i < kStopCount; ++i)
        stopPositions[i] = std::max(stopPositions[i], stopPositions[i - 1]);

    // Radii go through MakeRect rather than the primitive's arrays: a corner is
    // an ellipse now, so writing one array leaves the other semi-axis at zero
    // and squares the corner.
    auto segmentRect = [x, w, barY, barH](float t0, float t1,
                                          float rTL, float rTR, float rBR, float rBL)
    {
        const float sx = x + w * t0;
        const float ex = x + w * t1;
        return UI::MakeRect(sx, barY, std::max(0.0f, ex - sx), barH, 0xFFFFFFFFu,
                            rTL, rTR, rBR, rBL);
    };

    auto segWrapLeft = segmentRect(0.0f, stopPositions[0], barRadius, 0.0f, 0.0f, barRadius);
    UI::AddGradient(segWrapLeft, UI::GradientMode::Horizontal,
                    UI::PackFromARGB(m_StopColors[3]), UI::PackFromARGB(m_StopColors[0]));
    ctx.Emit(segWrapLeft);

    auto seg0 = segmentRect(stopPositions[0], stopPositions[1], 0.0f, 0.0f, 0.0f, 0.0f);
    UI::AddGradient(seg0, UI::GradientMode::Horizontal,
                    UI::PackFromARGB(m_StopColors[0]), UI::PackFromARGB(m_StopColors[1]));
    ctx.Emit(seg0);

    auto seg1 = segmentRect(stopPositions[1], stopPositions[2], 0.0f, 0.0f, 0.0f, 0.0f);
    UI::AddGradient(seg1, UI::GradientMode::Horizontal,
                    UI::PackFromARGB(m_StopColors[1]), UI::PackFromARGB(m_StopColors[2]));
    ctx.Emit(seg1);

    auto seg2 = segmentRect(stopPositions[2], stopPositions[3], 0.0f, 0.0f, 0.0f, 0.0f);
    UI::AddGradient(seg2, UI::GradientMode::Horizontal,
                    UI::PackFromARGB(m_StopColors[2]), UI::PackFromARGB(m_StopColors[3]));
    ctx.Emit(seg2);

    auto segWrapRight = segmentRect(stopPositions[3], 1.0f, 0.0f, barRadius, barRadius, 0.0f);
    UI::AddGradient(segWrapRight, UI::GradientMode::Horizontal,
                    UI::PackFromARGB(m_StopColors[3]), UI::PackFromARGB(m_StopColors[0]));
    ctx.Emit(segWrapRight);

    const float markerY = y + std::max(0.0f, (h - markerSize) * 0.5f);
    const float minMarkerX = x + markerHalf + kMarkerEdgeInsetPx * cs;
    const float maxMarkerX = std::max(minMarkerX, x + w - markerHalf - kMarkerEdgeInsetPx * cs);
    const float markerSpan = std::max(1e-3f, maxMarkerX - minMarkerX);
    for (size_t i = 0; i < kStopCount; ++i)
    {
        const float px = minMarkerX + markerSpan * stopPositions[i];
        const bool selected = (m_SelectedStop < kStopCount && i == m_SelectedStop);
        const bool hovered = (m_HoveredStop < kStopCount && i == m_HoveredStop);
        const uint32_t fill = selected
                                  ? UI::PackColor(0.78f, 0.78f, 0.78f, 1.0f)
                              : hovered
                                  ? UI::PackColor(0.66f, 0.66f, 0.66f, 1.0f)
                                  : UI::PackColor(0.55f, 0.55f, 0.55f, 1.0f);
        const uint32_t border = selected
                                    ? UI::PackColor(1.0f, 1.0f, 1.0f, 1.0f)
                                : hovered
                                    ? UI::PackColor(0.9f, 0.9f, 0.9f, 0.95f)
                                    : UI::PackColor(0.0f, 0.0f, 0.0f, 0.8f);
        UI::UIPrimitive marker = UI::MakeRect(px - markerHalf, markerY, markerSize, markerSize, fill,
                                              1.0f * cs, 1.0f * cs, 1.0f * cs, 1.0f * cs);
        UI::AddBorder(marker, 1.0f * cs, border);
        ctx.Emit(marker);
    }
}

} // namespace GameEngine
