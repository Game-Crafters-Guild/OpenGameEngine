#include "Panels/TimelineBarElement.h"

#include "Input/InputSystem.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIEvents.h"
#include "UI/UiContext.h"
#include "UI/UIPrimitive.h"
#include "UI/ResolvedStyle.h"
#include "Rendering/Text/FontAtlas.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>


namespace GameEngine
{

namespace
{
// Choose major tick step (seconds) so we get ~5-25 ticks when zoomed out.
float ComputeTimeStep(float rangeDuration)
{
    if (rangeDuration <= 0.f)
        return 1.f;
    const float minStep = rangeDuration / 25.f;
    const float nice[] = {
        0.001f, 0.002f, 0.005f,
        0.01f, 0.02f, 0.05f,
        0.1f, 0.2f, 0.5f,
        1.f, 2.f, 5.f, 10.f, 15.f, 30.f, 60.f, 120.f, 300.f, 600.f, 3600.f, 7200.f
    };
    for (float s : nice)
    {
        if (s >= minStep)
            return s;
    }
    return 7200.f;
}

// Format time for label: seconds -> "0.0s"; minutes -> "1m" / "1.5m"; hours -> "1h" / "1.5h".
void FormatTimeLabel(float timeSeconds, float stepSeconds, std::ostringstream& oss)
{
    if (stepSeconds >= 3600.f)
    {
        const float h = timeSeconds / 3600.f;
        oss.precision(h == std::floor(h) ? 0 : 1);
        oss << std::fixed << h << "h";
    }
    else if (stepSeconds >= 60.f)
    {
        const float m = timeSeconds / 60.f;
        oss.precision(m == std::floor(m) ? 0 : 1);
        oss << std::fixed << m << "m";
    }
    else
    {
        if (timeSeconds == 0.f || std::fabs(timeSeconds) < 0.0001f)
        {
            oss << "0";
        }
        else if (stepSeconds < 1.0f)
        {
            oss.precision(1);
            oss << std::fixed << timeSeconds << "s";
        }
        else
        {
            oss.precision(timeSeconds == std::floor(timeSeconds) ? 0 : 1);
            oss << std::fixed << timeSeconds << "s";
        }
    }
}

} // namespace

namespace
{
// Data-driven tick labels in the thin strips above/below the ruler; uses the element font from ctx.
void EmitCenteredStripLabel(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                            float stripLeft, float stripTop, float stripW, float stripH,
                            float anchorX, std::string_view text)
{
    if (text.empty() || !ctx.FontAtlas)
        return;
    const float cs = ctx.ContentScale > 0.0f ? ctx.ContentScale : 1.0f;
    const VisualStyle& vs = style.Visual;
    // FontAtlas APIs take physical pixelSize; PrimitiveEmitContext::EmitText
    // takes LOGICAL fontSize and multiplies by contentScale internally.
    const float baseLogicalFontSize = vs.FontSize > 0.0f ? vs.FontSize : 11.0f;
    float fontSize = std::max(8.0f * cs, std::min(baseLogicalFontSize * cs, stripH - 2.0f * cs));
    const float pixelSize = std::max(1.0f, fontSize);
    // Logical equivalent of the (possibly clamped) physical fontSize.
    const float emitLogicalFontSize = (cs > 0.0f) ? (fontSize / cs) : fontSize;
    const float textW = ctx.FontAtlas->MeasureUtf8(text, pixelSize).width;
    float textX = anchorX - textW * 0.5f;
    const float kEdgePadPx = 1.0f * cs;
    textX = std::max(stripLeft + kEdgePadPx, std::min(textX, stripLeft + stripW - textW - kEdgePadPx));

    const auto lm = ctx.FontAtlas->GetFontLineMetrics(pixelSize);
    const float lineH = std::max(1.0f, lm.height);
    const float textY = stripTop + std::max(0.0f, (stripH - lineH) * 0.5f);

    uint32_t color = vs.Color;
    if ((color & 0xFF000000u) == 0)
        color = 0xFFB0B0B0u;

    ctx.EmitText(text, textX, textY, emitLogicalFontSize, color, ctx.FontAtlas);
}

constexpr float kPlayheadHitHalfWidthPx = 4.0f; // 8px total; used by ruler + time labels + frame labels

constexpr float kPlayheadPillHeightMaxPx = 16.0f;
constexpr float kPlayheadPillPadTopPx = 2.0f;
constexpr float kPlayheadPillPadBottomPx = 3.0f; // 1px more than top for visual balance
constexpr float kPlayheadPillVerticalNudgePx = 1.0f; // shift pill down slightly in the ruler strip
constexpr float kPlayheadPillPadHorizontalPx = 5.0f;
constexpr float kPlayheadPillRadiusMaxPx = 5.0f;
constexpr float kPlayheadPillDigitEstPx = 8.0f;

// Frame pill is drawn on the middle ruler strip (TimelineBarElement); inset from top/bottom (extra gap at bottom).
void ComputePlayheadFramePillBounds(float localW, float playheadLocalX, int frame,
                                    Rendering::Text::FontAtlas* font, float fontSize, float stripH,
                                    float& outLeft, float& outTop, float& outW, float& outH,
                                    float cs = 1.0f)
{
    const std::string text = std::to_string(frame);
    const float padTop    = kPlayheadPillPadTopPx        * cs;
    const float padBottom = kPlayheadPillPadBottomPx     * cs;
    const float nudge     = kPlayheadPillVerticalNudgePx * cs;
    const float padH      = kPlayheadPillPadHorizontalPx * cs;
    const float maxHCap   = kPlayheadPillHeightMaxPx     * cs;
    const float estDigit  = kPlayheadPillDigitEstPx      * cs;
    const float innerH    = std::max(0.0f, stripH - padTop - padBottom);
    const float maxH      = std::max(4.0f * cs, innerH);
    outH   = std::min(maxHCap, maxH);
    outTop = padTop + std::max(0.0f, (innerH - outH) * 0.5f);
    outTop += nudge;
    outTop = std::min(outTop, std::max(0.0f, stripH - padBottom - outH));
    outW   = std::max(26.0f * cs, padH * 2.0f + estDigit * static_cast<float>(text.size()));
    if (font && fontSize > 0.0f)
    {
        const float px = std::max(1.0f, fontSize);
        const float tw    = font->MeasureUtf8(text, px).width;
        outW              = std::max(outW, tw + padH * 2.0f);
    }
    outLeft = playheadLocalX - outW * 0.5f;
    if (outLeft < 0.0f)
        outLeft = 0.0f;
    if (outLeft + outW > localW)
        outLeft = localW - outW;
}
} // namespace

TimelineBarElement::TimelineBarElement()
{
    AddClass("animationwindow-timeline-ruler");
}

void TimelineBarElement::SetTimelineState(float currentTime, float rangeStart, float rangeEnd, float fps)
{
    if (m_CurrentTime != currentTime || m_RangeStart != rangeStart || m_RangeEnd != rangeEnd || m_Fps != fps)
    {
        m_CurrentTime = currentTime;
        m_RangeStart = rangeStart;
        m_RangeEnd = rangeEnd;
        m_Fps = fps;
        MarkDirty(VisualDirty);
    }
}

void TimelineBarElement::UpdateTimeFromPosition(float globalX)
{
    const float W = GetLayoutWidth();
    if (W <= 0.0f || !m_OnTimeChange)
        return;
    // Convert to local X (event is in root/window space; GetLayoutX is root-relative)
    float localX = globalX - GetLayoutX();
    localX = std::clamp(localX, 0.0f, W);
    float t = std::clamp(
        m_RangeStart + (localX / W) * (m_RangeEnd - m_RangeStart),
        m_RangeStart, m_RangeEnd);
    t = std::round(t * m_Fps) / m_Fps;
    t = std::clamp(t, m_RangeStart, m_RangeEnd);
    m_OnTimeChange(t);
}

bool TimelineBarElement::IsOverPlayheadStrip(float globalX) const
{
    const float W = GetLayoutWidth();
    if (W <= 0.0f)
        return false;
    const float localX = globalX - GetLayoutX();
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    float playheadPx = (m_CurrentTime - m_RangeStart) / rangeDuration * W;
    playheadPx = std::clamp(playheadPx, 0.0f, W);
    return std::fabs(localX - playheadPx) <= kPlayheadHitHalfWidthPx;
}

bool TimelineBarElement::IsOverPlayheadScrub(float globalX, float globalY) const
{
    const float W = GetLayoutWidth();
    const float H = GetLayoutHeight();
    if (W <= 0.0f || H <= 0.0f)
        return false;
    const float localX = globalX - GetLayoutX();
    const float localY = globalY - GetLayoutY();
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    float playheadLocalX = (m_CurrentTime - m_RangeStart) / rangeDuration * W;
    playheadLocalX = std::clamp(playheadLocalX, 0.0f, W);

    constexpr float kPillHitPad = 6.0f;
    if (m_PlayheadPillValid &&
        localX >= m_PlayheadPillLeft - kPillHitPad && localX <= m_PlayheadPillLeft + m_PlayheadPillW + kPillHitPad &&
        localY >= m_PlayheadPillTop  - kPillHitPad && localY <= m_PlayheadPillTop  + m_PlayheadPillH + kPillHitPad)
        return true;

    return std::fabs(localX - playheadLocalX) <= kPlayheadHitHalfWidthPx;
}

void TimelineBarElement::OnEvent(UIEvent& e)
{
    // Left (0) or middle (2) mouse: over playhead tip = scrub; elsewhere = pan.
    if (e.Id == kEventMouseDown && (e.Button == 0 || e.Button == 2))
    {
        if (IsOverPlayheadScrub(e.X, e.Y))
        {
            m_Scrubbing = true;
            m_ScrubButton = e.Button;
            e.Capture(this);
            e.Stop();
            AddClass("timeline-dragging");
            if (m_OnScrubBegin) m_OnScrubBegin();
            UpdateTimeFromPosition(e.X);
        }
        else if (m_OnPan)
        {
            m_Panning = true;
            m_PanButton = e.Button;
            m_PanLastGlobalX = e.X;
            e.Capture(this);
            e.Stop();
            AddClass("timeline-dragging");
        }
    }
    else if (e.Id == kEventMouseMove)
    {
        if (m_Scrubbing)
        {
            UpdateTimeFromPosition(e.X);
            e.Stop();
        }
        else if (m_Panning && m_OnPan)
        {
            const float W = GetLayoutWidth();
            if (W > 0.0f)
            {
                const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
                float deltaPx = e.X - m_PanLastGlobalX;
                float deltaTime = (deltaPx / W) * rangeDuration;
                m_OnPan(deltaTime);
            }
            m_PanLastGlobalX = e.X;
            e.Stop();
        }
        else
        {
            const bool overPill = IsOverPlayheadScrub(e.X, e.Y);
            if (overPill != m_PillHovered)
            {
                m_PillHovered = overPill;
                MarkDirty(VisualDirty);
            }
        }
    }
    else if (e.Id == kEventMouseLeave)
    {
        if (m_PillHovered)
        {
            m_PillHovered = false;
            MarkDirty(VisualDirty);
        }
    }
    else if (e.Id == kEventMouseUp && (e.Button == m_ScrubButton || e.Button == m_PanButton))
    {
        if (m_Scrubbing)
        {
            m_Scrubbing = false;
            m_ScrubButton = -1;
            UpdateTimeFromPosition(e.X);
            if (m_OnScrubEnd) m_OnScrubEnd();
        }
        if (m_Panning)
        {
            m_Panning = false;
            m_PanButton = -1;
        }
        RemoveClass("timeline-dragging");
        e.Stop();
    }
    else if (e.Id == kEventScroll && m_OnZoom)
    {
        m_OnZoom(e.ScrollY, e.X);
        e.Stop();
    }
}

void TimelineBarElement::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                                              const ResolvedStyle& style,
                                              float x, float y, float W, float H)
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

    using namespace UI;
    m_PlayheadPillValid = false;
    if (W <= 0.0f || H <= 0.0f)
        return;

    const float cs = ctx.ContentScale > 0.0f ? ctx.ContentScale : 1.0f;
    m_ContentScale = cs;

    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const float step = ComputeTimeStep(rangeDuration);
    const float minorStep = (step >= 60.f) ? step / 5.f : step / 2.f;

    const int startFrame = static_cast<int>(std::ceil(m_RangeStart * m_Fps));
    const int endFrame = static_cast<int>(std::floor(m_RangeEnd * m_Fps));
    const int frameCount = endFrame - startFrame + 1;
    const bool perFrameMode = (frameCount > 0 && frameCount <= 120);

    const float tickH = H * 0.4f;
    const float tickYBase = y + H;
    const float minorTickH = H * 0.25f;
    const float minorTickYBase = y + H;

    if (perFrameMode)
    {
        const uint32_t tickColor = PackColor(0.5f, 0.5f, 0.5f, 0.5f);
        for (int frame = startFrame; frame <= endFrame; ++frame)
        {
            const float t = static_cast<float>(frame) / m_Fps;
            const float px = x + (t - m_RangeStart) / rangeDuration * W;
            if (px < x || px > x + W)
                continue;
            ctx.Emit(MakeRect(px - 0.5f * cs, tickYBase, 1.0f * cs, tickH, tickColor));
        }
    }
    else
    {
        const uint32_t majorColor = PackColor(0.5f, 0.5f, 0.5f, 0.6f);
        for (float t = std::ceil(m_RangeStart / step) * step; t <= m_RangeEnd; t += step)
        {
            float px = x + (t - m_RangeStart) / rangeDuration * W;
            if (px < x || px > x + W)
                continue;
            ctx.Emit(MakeRect(px - 0.5f * cs, tickYBase, 1.0f * cs, tickH, majorColor));
        }

        const uint32_t minorColor = PackColor(0.5f, 0.5f, 0.5f, 0.35f);
        for (float t = std::ceil(m_RangeStart / minorStep) * minorStep; t <= m_RangeEnd; t += minorStep)
        {
            if (std::fabs(std::fmod(t, step)) < 0.001f * step)
                continue;
            float px = x + (t - m_RangeStart) / rangeDuration * W;
            if (px < x || px > x + W)
                continue;
            ctx.Emit(MakeRect(px - 0.5f * cs, minorTickYBase, 1.0f * cs, minorTickH, minorColor));
        }
    }

    float playheadLocalX = (m_CurrentTime - m_RangeStart) / rangeDuration * W;
    playheadLocalX = std::clamp(playheadLocalX, 0.0f, W);
    const float playheadPx = x + playheadLocalX;
    ctx.Emit(MakeRect(std::max(x, playheadPx - cs), y, 2.0f * cs, H, PackColor(1.0f, 0.4f, 0.0f, 0.95f)));

    const int frame = static_cast<int>(std::round(m_CurrentTime * m_Fps));
    // Use the resolved CSS font-size; .animationwindow-timeline-ruler sets it
    // explicitly so the inherited 14px root base doesn't overflow the pill.
    // FontAtlas APIs (Measure / GetFontLineMetrics / ComputePlayheadFramePillBounds)
    // take physical pixelSize. PrimitiveEmitContext::EmitText takes LOGICAL fontSize
    // and multiplies by contentScale internally — keep both around.
    const float kLogicalFontSize = style.Visual.FontSize > 0.0f ? style.Visual.FontSize : 11.0f;
    float fontSize = kLogicalFontSize * cs;
    fontSize = std::min(fontSize,
                        std::max(8.0f * cs, H - (kPlayheadPillPadTopPx + kPlayheadPillPadBottomPx + 3.0f) * cs));
    // Logical equivalent of the (possibly clamped) physical fontSize, for EmitText.
    const float emitLogicalFontSize = (cs > 0.0f) ? (fontSize / cs) : fontSize;
    float pillLeft = 0.0f;
    float pillTop = 0.0f;
    float pillW = 0.0f;
    float pillH = 0.0f;
    ComputePlayheadFramePillBounds(W, playheadLocalX, frame, ctx.FontAtlas, fontSize, H, pillLeft, pillTop, pillW,
                                     pillH, cs);
    m_PlayheadPillLeft = pillLeft / cs;
    m_PlayheadPillTop  = pillTop  / cs;
    m_PlayheadPillW    = pillW    / cs;
    m_PlayheadPillH    = pillH    / cs;
    m_PlayheadPillValid = true;

    const float cornerR = std::min(kPlayheadPillRadiusMaxPx * cs, pillH * 0.48f);
    const uint32_t pillFill = m_PillHovered ? PackColor(0.26f, 0.22f, 0.14f, 0.98f)
                                            : PackColor(0.16f, 0.16f, 0.16f, 0.98f);
    const uint32_t pillBorder = m_PillHovered ? PackColor(1.0f, 0.70f, 0.20f, 1.0f)
                                              : PackColor(1.0f, 0.42f, 0.08f, 0.98f);
    UIPrimitive pillPrim =
        MakeRect(x + pillLeft, y + pillTop, pillW, pillH, pillFill, cornerR, cornerR, cornerR, cornerR);
    AddBorder(pillPrim, 1.0f * cs, pillBorder);
    ctx.Emit(pillPrim);

    if (ctx.FontAtlas)
    {
        const std::string frameStr = std::to_string(frame);
        const float pixelSize = std::max(1.0f, fontSize);
        constexpr uint32_t kPillTextArgb = 0xFFF0F0F0u;

        // Match the engine's stock text rendering path (UIManager_PrimitiveGen):
        // vertical offset = (containerH - lm.height) * 0.5f, centered around the
        // line-box. This is what the IntField labels use and look correctly centered.
        const auto lm = ctx.FontAtlas->GetFontLineMetrics(pixelSize);
        const float textW = ctx.FontAtlas->MeasureUtf8(frameStr, pixelSize).width;
        const float textX = x + pillLeft + (pillW - textW) * 0.5f;
        const float textY = y + pillTop + std::max(0.0f, (pillH - lm.height) * 0.5f);

        ctx.EmitText(frameStr, textX, textY, emitLogicalFontSize, kPillTextArgb, ctx.FontAtlas);
    }
}

// --- TimelineMarkersElement ---
TimelineMarkersElement::TimelineMarkersElement()
{
    AddClass("animationwindow-timeline-markers");
}

void TimelineMarkersElement::SetMarkerTimes(std::vector<float> times)
{
    m_MarkerTimes = std::move(times);
    MarkDirty(VisualDirty);
}

void TimelineMarkersElement::SetTimeRange(float rangeStart, float rangeEnd)
{
    if (m_RangeStart != rangeStart || m_RangeEnd != rangeEnd)
    {
        m_RangeStart = rangeStart;
        m_RangeEnd = rangeEnd;
        MarkDirty(VisualDirty);
    }
}

void TimelineMarkersElement::SetCurrentTime(float currentTime)
{
    if (m_CurrentTime != currentTime)
    {
        m_CurrentTime = currentTime;
        MarkDirty(VisualDirty);
    }
}

int TimelineMarkersElement::IndexOfMarkerAt(float localX, float localY) const
{
    const float w = GetLayoutWidth();
    const float h = GetLayoutHeight();
    if (w <= 0.0f || h <= 0.0f)
        return -1;
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const float centerY = h * 0.5f;
    const float halfSizeX = 4.0f;
    const float halfSizeY = 8.0f;
    if (localY < centerY - halfSizeY || localY > centerY + halfSizeY)
        return -1;
    for (size_t i = 0; i < m_MarkerTimes.size(); ++i)
    {
        float t = m_MarkerTimes[i];
        if (t < m_RangeStart || t > m_RangeEnd)
            continue;
        float px = (t - m_RangeStart) / rangeDuration * w;
        if (localX >= px - halfSizeX && localX <= px + halfSizeX)
            return static_cast<int>(i);
    }
    return -1;
}

float TimelineMarkersElement::TimeFromGlobalX(float globalX) const
{
    const float w = GetLayoutWidth();
    if (w <= 0.0f)
        return m_RangeStart;
    const float localX = globalX - GetLayoutX();
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    float t = m_RangeStart + (localX / w) * rangeDuration;
    return std::clamp(t, m_RangeStart, m_RangeEnd);
}

void TimelineMarkersElement::OnEvent(UIEvent& e)
{
    if (e.Id == kEventMouseDown)
    {
        const float localX = e.X - GetLayoutX();
        const float localY = e.Y - GetLayoutY();
        int idx = IndexOfMarkerAt(localX, localY);
        if (idx >= 0 && static_cast<size_t>(idx) < m_MarkerTimes.size())
        {
            m_DragMarkerIndex = idx;
            m_DragStartY = e.Y;
            e.Capture(this);
            e.Stop();
        }
        return;
    }
    if (e.Id == kEventMouseMove)
    {
        if (m_DragMarkerIndex >= 0 && m_OnMarkerTimeChanged)
        {
            const float newTime = TimeFromGlobalX(e.X);
            m_OnMarkerTimeChanged(static_cast<size_t>(m_DragMarkerIndex), newTime);
            e.Stop();
        }
        return;
    }
    if (e.Id == kEventMouseUp)
    {
        if (m_DragMarkerIndex >= 0)
        {
            const float deltaY = e.Y - m_DragStartY;
            if (deltaY >= kDragDownRemoveThresholdPx && m_OnMarkerRemoved)
                m_OnMarkerRemoved(static_cast<size_t>(m_DragMarkerIndex));
            else if (m_OnMarkerTimeChanged)
                m_OnMarkerTimeChanged(static_cast<size_t>(m_DragMarkerIndex), TimeFromGlobalX(e.X));
            m_DragMarkerIndex = -1;
            e.Stop();
        }
        return;
    }
    UIElement::OnEvent(e);
}

void TimelineMarkersElement::OnPostLayout()
{
    UIElement::OnPostLayout();
}

void TimelineMarkersElement::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                                                  const ResolvedStyle& /*style*/,
                                                  float x, float y, float W, float H)
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

    using namespace UI;
    if (W <= 0.0f || H <= 0.0f)
        return;

    const float cs = ctx.ContentScale > 0.0f ? ctx.ContentScale : 1.0f;
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const float centerY   = y + H * 0.5f;
    const float halfSizeX = 4.0f * cs;
    const float halfSizeY = 8.0f * cs;
    const uint32_t markerColor = PackColor(0.2f, 0.5f, 0.95f, 0.95f);

    for (float t : m_MarkerTimes)
    {
        if (t < m_RangeStart || t > m_RangeEnd)
            continue;
        const float px = x + (t - m_RangeStart) / rangeDuration * W;
        if (px < x - halfSizeX || px > x + W + halfSizeX)
            continue;
        ctx.Emit(MakeRect(px - halfSizeX, centerY - halfSizeY, 8.0f * cs, 12.0f * cs, markerColor));
        ctx.Emit(MakeRect(px - 2.0f * cs, centerY + 4.0f * cs, 4.0f * cs, 4.0f * cs, markerColor));
    }

    if (m_CurrentTime >= m_RangeStart && m_CurrentTime <= m_RangeEnd)
    {
        float playheadPx = x + (m_CurrentTime - m_RangeStart) / rangeDuration * W;
        playheadPx = std::max(x, std::min(x + W, playheadPx));
        ctx.Emit(MakeRect(std::max(x, playheadPx - cs), y, 2.0f * cs, H, PackColor(1.0f, 0.4f, 0.0f, 0.95f)));
    }
}

// --- TimelineTimeLabelsElement ---
TimelineTimeLabelsElement::TimelineTimeLabelsElement()
{
    AddClass("animationwindow-timeline-time-labels");
}

void TimelineTimeLabelsElement::SetTimelineState(float currentTime, float rangeStart, float rangeEnd, float fps)
{
    if (m_CurrentTime != currentTime || m_RangeStart != rangeStart || m_RangeEnd != rangeEnd || m_Fps != fps)
    {
        m_CurrentTime = currentTime;
        m_RangeStart = rangeStart;
        m_RangeEnd = rangeEnd;
        m_Fps = fps;
        MarkDirty(VisualDirty);
    }
}

void TimelineTimeLabelsElement::UpdateTimeFromPosition(float globalX)
{
    const float W = GetLayoutWidth();
    if (W <= 0.0f || !m_OnTimeChange)
        return;
    float localX = globalX - GetLayoutX();
    localX = std::clamp(localX, 0.0f, W);
    float t = std::clamp(
        m_RangeStart + (localX / W) * (m_RangeEnd - m_RangeStart),
        m_RangeStart, m_RangeEnd);
    t = std::round(t * m_Fps) / m_Fps;
    t = std::clamp(t, m_RangeStart, m_RangeEnd);
    m_OnTimeChange(t);
}

bool TimelineTimeLabelsElement::IsOverPlayhead(float globalX) const
{
    const float W = GetLayoutWidth();
    if (W <= 0.0f)
        return false;
    float localX = globalX - GetLayoutX();
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    float playheadPx = (m_CurrentTime - m_RangeStart) / rangeDuration * W;
    return std::fabs(localX - playheadPx) <= kPlayheadHitHalfWidthPx;
}

void TimelineTimeLabelsElement::OnEvent(UIEvent& e)
{
    if (e.Id == kEventMouseDown && (e.Button == 0 || e.Button == 2))
    {
        // Left click anywhere seeks to that time; middle click (or no time callback) pans.
        if (e.Button == 0 && m_OnTimeChange)
        {
            m_Scrubbing = true;
            m_ScrubButton = e.Button;
            e.Capture(this);
            e.Stop();
            if (m_OnScrubBegin) m_OnScrubBegin();
            UpdateTimeFromPosition(e.X);
        }
        else if (m_OnPan)
        {
            m_Panning = true;
            m_PanButton = e.Button;
            m_PanLastGlobalX = e.X;
            e.Capture(this);
            e.Stop();
        }
    }
    else if (e.Id == kEventMouseMove)
    {
        if (m_Scrubbing)
        {
            UpdateTimeFromPosition(e.X);
            e.Stop();
        }
        else if (m_Panning && m_OnPan)
        {
            const float W = GetLayoutWidth();
            if (W > 0.0f)
            {
                const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
                float deltaPx = e.X - m_PanLastGlobalX;
                float deltaTime = (deltaPx / W) * rangeDuration;
                m_OnPan(deltaTime);
            }
            m_PanLastGlobalX = e.X;
            e.Stop();
        }
    }
    else if (e.Id == kEventMouseUp && (e.Button == m_ScrubButton || e.Button == m_PanButton))
    {
        if (m_Scrubbing)
        {
            m_Scrubbing = false;
            m_ScrubButton = -1;
            UpdateTimeFromPosition(e.X);
            if (m_OnScrubEnd) m_OnScrubEnd();
        }
        if (m_Panning)
        {
            m_Panning = false;
            m_PanButton = -1;
        }
        e.Stop();
    }
    else if (e.Id == kEventScroll && m_OnZoom)
    {
        m_OnZoom(e.ScrollY, e.X);
        e.Stop();
    }
}

void TimelineTimeLabelsElement::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                                                     const ResolvedStyle& style,
                                                     float x, float y, float W, float H)
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

    using namespace UI;
    if (W <= 0.0f || H <= 0.0f)
        return;

    const float cs = ctx.ContentScale > 0.0f ? ctx.ContentScale : 1.0f;
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const float step = ComputeTimeStep(rangeDuration);
    const float minLabelSpacingPx = 36.0f * cs;
    float lastLabelPx = x - minLabelSpacingPx - 1.0f;
    const uint32_t tickColor = PackColor(0.85f, 0.85f, 0.85f, 0.5f);

    // When the bar is taller than the standard ruler strip (ruler-at-top header mode),
    // confine tick marks and time labels to the bottom 14px so the rest of the bar
    // acts as an unobstructed column for the playhead indicator line.
    const float kStripH = std::min(H, 14.0f * cs);
    const float stripY  = y + H - kStripH;

    for (float t = std::ceil(m_RangeStart / step) * step; t <= m_RangeEnd; t += step)
    {
        const float px = x + (t - m_RangeStart) / rangeDuration * W;
        if (px < x - 1.0f || px > x + W + 1.0f)
            continue;
        if (px - lastLabelPx < minLabelSpacingPx)
            continue;
        lastLabelPx = px;
        ctx.Emit(MakeRect(px - 0.5f * cs, stripY + kStripH, 1.0f * cs, 6.0f * cs, tickColor));
        std::ostringstream labelOss;
        FormatTimeLabel(t, step, labelOss);
        EmitCenteredStripLabel(ctx, style, x, stripY, W, kStripH, px, labelOss.str());
    }

    float playheadPx = x + (m_CurrentTime - m_RangeStart) / rangeDuration * W;
    playheadPx = std::max(x, std::min(x + W, playheadPx));
    const uint32_t playheadColor = PackColor(1.0f, 0.4f, 0.0f, 0.95f);
    // Line spans full height so indicator is uninterrupted through the header area
    ctx.Emit(MakeRect(std::max(x, playheadPx - cs), y, 2.0f * cs, H, playheadColor));
    // Downward-pointing triangle sits at the top of the ruler strip
    const float triHalfW = 4.0f * cs;
    const float triH     = 5.0f * cs;
    ctx.Emit(MakeTriangle(
        playheadPx - triHalfW, stripY,
        playheadPx + triHalfW, stripY,
        playheadPx,            stripY + triH,
        playheadColor));
}

// --- TimelineFrameLabelsElement ---
TimelineFrameLabelsElement::TimelineFrameLabelsElement()
{
    AddClass("animationwindow-timeline-frame-labels");
}

void TimelineFrameLabelsElement::SetTimelineState(float currentTime, float rangeStart, float rangeEnd, float fps)
{
    if (m_CurrentTime != currentTime || m_RangeStart != rangeStart || m_RangeEnd != rangeEnd || m_Fps != fps)
    {
        m_CurrentTime = currentTime;
        m_RangeStart = rangeStart;
        m_RangeEnd = rangeEnd;
        m_Fps = fps;
        MarkDirty(VisualDirty);
    }
}

void TimelineFrameLabelsElement::UpdateTimeFromPosition(float globalX)
{
    const float W = GetLayoutWidth();
    if (W <= 0.0f || !m_OnTimeChange)
        return;
    float localX = globalX - GetLayoutX();
    localX = std::clamp(localX, 0.0f, W);
    float t = std::clamp(
        m_RangeStart + (localX / W) * (m_RangeEnd - m_RangeStart),
        m_RangeStart, m_RangeEnd);
    t = std::round(t * m_Fps) / m_Fps;
    t = std::clamp(t, m_RangeStart, m_RangeEnd);
    m_OnTimeChange(t);
}

bool TimelineFrameLabelsElement::IsOverScrubArea(float globalX, float globalY) const
{
    const float W = GetLayoutWidth();
    const float H = GetLayoutHeight();
    if (W <= 0.0f || H <= 0.0f)
        return false;
    const float localX = globalX - GetLayoutX();
    const float localY = globalY - GetLayoutY();
    if (localY < 0.0f || localY > H)
        return false;
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    float playheadPx = (m_CurrentTime - m_RangeStart) / rangeDuration * W;
    playheadPx = std::clamp(playheadPx, 0.0f, W);

    return std::fabs(localX - playheadPx) <= kPlayheadHitHalfWidthPx;
}

void TimelineFrameLabelsElement::OnEvent(UIEvent& e)
{
    if (e.Id == kEventMouseDown && (e.Button == 0 || e.Button == 2))
    {
        if (IsOverScrubArea(e.X, e.Y))
        {
            m_Scrubbing = true;
            m_ScrubButton = e.Button;
            e.Capture(this);
            e.Stop();
            AddClass("timeline-dragging");
            if (m_OnScrubBegin) m_OnScrubBegin();
            UpdateTimeFromPosition(e.X);
        }
        else if (m_OnPan)
        {
            m_Panning = true;
            m_PanButton = e.Button;
            m_PanLastGlobalX = e.X;
            e.Capture(this);
            e.Stop();
            AddClass("timeline-dragging");
        }
    }
    else if (e.Id == kEventMouseMove)
    {
        if (m_Scrubbing)
        {
            UpdateTimeFromPosition(e.X);
            e.Stop();
        }
        else if (m_Panning && m_OnPan)
        {
            const float W = GetLayoutWidth();
            if (W > 0.0f)
            {
                const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
                float deltaPx = e.X - m_PanLastGlobalX;
                float deltaTime = (deltaPx / W) * rangeDuration;
                m_OnPan(deltaTime);
            }
            m_PanLastGlobalX = e.X;
            e.Stop();
        }
    }
    else if (e.Id == kEventMouseUp && (e.Button == m_ScrubButton || e.Button == m_PanButton))
    {
        if (m_Scrubbing)
        {
            m_Scrubbing = false;
            m_ScrubButton = -1;
            UpdateTimeFromPosition(e.X);
            if (m_OnScrubEnd) m_OnScrubEnd();
        }
        if (m_Panning)
        {
            m_Panning = false;
            m_PanButton = -1;
        }
        RemoveClass("timeline-dragging");
        e.Stop();
    }
    else if (e.Id == kEventScroll && m_OnZoom)
    {
        m_OnZoom(e.ScrollY, e.X);
        e.Stop();
    }
}

void TimelineFrameLabelsElement::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                                                      const ResolvedStyle& style,
                                                      float x, float y, float W, float H)
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

    using namespace UI;
    if (W <= 0.0f || H <= 0.0f)
        return;

    const float cs = ctx.ContentScale > 0.0f ? ctx.ContentScale : 1.0f;
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const float minLabelSpacingPx = 24.0f * cs;
    float lastLabelPx = x - minLabelSpacingPx - 1.0f;
    const uint32_t tickColor = PackColor(0.55f, 0.55f, 0.55f, 0.5f);

    const int startFrame = static_cast<int>(std::ceil(m_RangeStart * m_Fps));
    const int endFrame = static_cast<int>(std::floor(m_RangeEnd * m_Fps));
    const int frameCount = endFrame - startFrame + 1;
    const bool perFrameMode = (frameCount > 0 && frameCount <= 120);

    if (perFrameMode)
    {
        for (int frame = startFrame; frame <= endFrame; ++frame)
        {
            const float t = static_cast<float>(frame) / m_Fps;
            const float px = x + (t - m_RangeStart) / rangeDuration * W;
            if (px < x - 1.0f || px > x + W + 1.0f)
                continue;
            if (px - lastLabelPx < minLabelSpacingPx)
                continue;
            lastLabelPx = px;
            ctx.Emit(MakeRect(px - 0.5f * cs, y, 1.0f * cs, 4.0f * cs, tickColor));
            EmitCenteredStripLabel(ctx, style, x, y, W, H, px, std::to_string(frame));
        }
    }
    else
    {
        const float step = ComputeTimeStep(rangeDuration);
        for (float t = std::ceil(m_RangeStart / step) * step; t <= m_RangeEnd; t += step)
        {
            const float px = x + (t - m_RangeStart) / rangeDuration * W;
            if (px < x - 1.0f || px > x + W + 1.0f)
                continue;
            if (px - lastLabelPx < minLabelSpacingPx)
                continue;
            lastLabelPx = px;
            ctx.Emit(MakeRect(px - 0.5f * cs, y, 1.0f * cs, 4.0f * cs, tickColor));
            const int frameNum = static_cast<int>(std::lround(t * m_Fps));
            EmitCenteredStripLabel(ctx, style, x, y, W, H, px, std::to_string(frameNum));
        }
    }

    float playheadLocalX = (m_CurrentTime - m_RangeStart) / rangeDuration * W;
    playheadLocalX = std::clamp(playheadLocalX, 0.0f, W);
    const float playheadPx = x + playheadLocalX;
    ctx.Emit(MakeRect(std::max(x, playheadPx - cs), y, 2.0f * cs, H, PackColor(1.0f, 0.4f, 0.0f, 0.95f)));
}

TimeRangeSliderElement::TimeRangeSliderElement()
{
    AddClass("animationwindow-time-range-slider");
}

void TimeRangeSliderElement::SetFullRange(float fullStart, float fullEnd)
{
    const float clampedEnd = std::max(fullStart + 0.001f, fullEnd);
    if (m_FullStart != fullStart || m_FullEnd != clampedEnd)
    {
        m_FullStart = fullStart;
        m_FullEnd = clampedEnd;
        MarkDirty(VisualDirty);
    }
}

void TimeRangeSliderElement::SetVisibleRange(float rangeStart, float rangeEnd)
{
    const float clampedEnd = std::max(rangeStart + 0.001f, rangeEnd);
    if (m_RangeStart != rangeStart || m_RangeEnd != clampedEnd)
    {
        m_RangeStart = rangeStart;
        m_RangeEnd = clampedEnd;
        MarkDirty(VisualDirty);
    }
}

void TimeRangeSliderElement::SetCurrentTime(float t)
{
    if (m_CurrentTime != t)
    {
        m_CurrentTime = t;
        MarkDirty(VisualDirty);
    }
}

void TimeRangeSliderElement::SetFps(float fps)
{
    const float clampedFps = std::max(0.001f, fps);
    if (m_Fps != clampedFps)
    {
        m_Fps = clampedFps;
        MarkDirty(VisualDirty);
    }
}

float TimeRangeSliderElement::LocalXForTime(float t, float w) const
{
    const float fullDur = std::max(0.001f, m_FullEnd - m_FullStart);
    return (t - m_FullStart) / fullDur * w;
}

float TimeRangeSliderElement::TimeForLocalX(float localX, float w) const
{
    if (w <= 0.0f) return m_FullStart;
    const float fullDur = std::max(0.001f, m_FullEnd - m_FullStart);
    return m_FullStart + (localX / w) * fullDur;
}

void TimeRangeSliderElement::OnEvent(UIEvent& e)
{
    const float W = GetLayoutWidth();
    if (W <= 0.0f) return;

    const float localX = e.X - GetLayoutX();

    if (e.Id == kEventMouseDown && e.Button == 0)
    {
        const float leftX  = LocalXForTime(m_RangeStart, W);
        const float rightX = LocalXForTime(m_RangeEnd, W);
        const float thumbHalfW = std::max(kThumbHalfW, GetLayoutHeight() * 0.5f - 2.0f);
        const float hitR       = thumbHalfW + kThumbHitExtra;
        // Use clamped positions to match the visual thumb locations exactly.
        const float leftHitX  = std::clamp(leftX,  thumbHalfW, W - thumbHalfW);
        const float rightHitX = std::clamp(rightX, thumbHalfW, W - thumbHalfW);


        if (std::fabs(localX - leftHitX) <= hitR)
            m_DragMode = DragMode::LeftThumb;
        else if (std::fabs(localX - rightHitX) <= hitR)
            m_DragMode = DragMode::RightThumb;
        else
            m_DragMode = DragMode::Pan;
        m_DragStartX          = localX;
        m_DragStartRangeStart = m_RangeStart;
        m_DragStartRangeEnd   = m_RangeEnd;
        m_DragFullStart       = m_FullStart;
        m_DragFullEnd         = m_FullEnd;
        m_DragLastPanTime     = 0.0f;
        m_DragSawButtonDown   = false;
        e.Capture(this);
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseMove && m_DragMode != DragMode::None && (m_OnRangeChanged || m_OnPan))
    {
        if (e.ButtonDown)
        {
            m_DragSawButtonDown = true;
        }
        else if (m_DragSawButtonDown)
        {
            m_DragMode = DragMode::None;
            m_DragSawButtonDown = false;
            e.Stop();
            return;
        }

        const float dx       = localX - m_DragStartX;
        const float fullStart = m_DragFullStart;
        const float fullEnd   = std::max(fullStart + 0.001f, m_DragFullEnd);
        const float fullDur  = std::max(0.001f, fullEnd - fullStart);
        const float dTime    = (dx / W) * fullDur;
        const float minDur   = 1.0f / m_Fps;
        float newStart = m_DragStartRangeStart;
        float newEnd   = m_DragStartRangeEnd;

        if (m_DragMode == DragMode::Pan)
        {
            if (m_OnRangeChanged)
            {
                const float dur = std::max(minDur, m_DragStartRangeEnd - m_DragStartRangeStart);
                newStart = std::clamp(m_DragStartRangeStart + dTime, fullStart, fullEnd - dur);
                newEnd = newStart + dur;
                m_OnRangeChanged(newStart, newEnd);
                e.Stop();
                return;
            }
            if (m_OnPan)
            {
                const float panDelta = dTime - m_DragLastPanTime;
                m_DragLastPanTime = dTime;
                m_OnPan(panDelta);
                e.Stop();
                return;
            }
        }

        if (m_DragMode == DragMode::LeftThumb)
            newStart = std::clamp(m_DragStartRangeStart + dTime, fullStart, newEnd - minDur);
        else if (m_DragMode == DragMode::RightThumb)
            newEnd = std::clamp(m_DragStartRangeEnd + dTime, newStart + minDur, fullEnd);
        else
        {
            const float dur = m_DragStartRangeEnd - m_DragStartRangeStart;
            newStart = std::max(fullStart, m_DragStartRangeStart + dTime);
            newEnd   = newStart + dur;
        }

        if (m_OnRangeChanged)
            m_OnRangeChanged(newStart, newEnd);
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseUp && m_DragMode != DragMode::None)
    {
        m_DragMode = DragMode::None;
        m_DragSawButtonDown = false;
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseMove || e.Id == kEventMouseEnter || e.Id == kEventMouseLeave)
    {
        HoverZone newZone = HoverZone::None;
        if (e.Id != kEventMouseLeave)
        {
            const float leftX  = LocalXForTime(m_RangeStart, W);
            const float rightX = LocalXForTime(m_RangeEnd, W);
            const float thumbHalfW = std::max(kThumbHalfW, GetLayoutHeight() * 0.5f - 2.0f);
            const float hitR = thumbHalfW + kThumbHitExtra;
            const float leftHitX  = std::clamp(leftX,  thumbHalfW, W - thumbHalfW);
            const float rightHitX = std::clamp(rightX, thumbHalfW, W - thumbHalfW);
            if (std::fabs(localX - leftHitX) <= hitR)
                newZone = HoverZone::LeftThumb;
            else if (std::fabs(localX - rightHitX) <= hitR)
                newZone = HoverZone::RightThumb;
        }
        if (newZone != m_HoverZone)
        {
            m_HoverZone = newZone;
            MarkDirty(VisualDirty);
        }
    }
}

void TimeRangeSliderElement::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                                                  const ResolvedStyle& style,
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

    if (w <= 0.0f || h <= 0.0f) return;

    const float cs = ctx.ContentScale > 0.0f ? ctx.ContentScale : 1.0f;
    const float leftX  = std::clamp(LocalXForTime(m_RangeStart, w), 0.0f, w);
    const float rightX = std::clamp(LocalXForTime(m_RangeEnd,   w), 0.0f, w);

    // Square thumbs: height minus a 2px margin on each side.
    const float thumbSize  = h - 4.0f * cs;
    const float thumbHalfW = thumbSize * 0.5f;
    const float thumbY     = y + 2.0f * cs;
    const float leftThumbX  = std::clamp(leftX,  thumbHalfW, w - thumbHalfW);
    const float rightThumbX = std::clamp(rightX, thumbHalfW, w - thumbHalfW);

    // Scroll track background
    ctx.Emit(UI::MakeRect(x, y, w, h, 0xFF1A1A1A));
    // Visible range fill
    ctx.Emit(UI::MakeRect(x + leftX, y, rightX - leftX, h, 0xFF2D2D2D));
    const bool leftActive  = m_DragMode == DragMode::LeftThumb  || m_HoverZone == HoverZone::LeftThumb;
    const bool rightActive = m_DragMode == DragMode::RightThumb || m_HoverZone == HoverZone::RightThumb;
    const uint32_t leftBg  = leftActive  ? 0xFF888888 : 0xFF606060;
    const uint32_t rightBg = rightActive ? 0xFF888888 : 0xFF606060;
    const uint32_t leftDot  = leftActive  ? 0xFFFFFFFF : 0xFFB0B0B0;
    const uint32_t rightDot = rightActive ? 0xFFFFFFFF : 0xFFB0B0B0;
    const float dotHalf = 3.0f * cs;
    const float dotSize = 6.0f * cs;
    // Left thumb
    ctx.Emit(UI::MakeRect(x + leftThumbX - thumbHalfW, thumbY, thumbSize, thumbSize, leftBg));
    ctx.Emit(UI::MakeRect(x + leftThumbX - dotHalf, thumbY + (thumbSize - dotSize) * 0.5f, dotSize, dotSize, leftDot));
    // Right thumb
    ctx.Emit(UI::MakeRect(x + rightThumbX - thumbHalfW, thumbY, thumbSize, thumbSize, rightBg));
    ctx.Emit(UI::MakeRect(x + rightThumbX - dotHalf, thumbY + (thumbSize - dotSize) * 0.5f, dotSize, dotSize, rightDot));

    // Playhead tick (only when inside the visible window)
    const float phX = LocalXForTime(m_CurrentTime, w);
    if (phX >= leftX && phX <= rightX)
        ctx.Emit(UI::MakeRect(x + phX - cs, y, 2.0f * cs, h, 0xFFE89020));

    if (!ctx.FontAtlas) return;

    // Resolved CSS font-size is in logical px; .animationwindow-time-range-slider
    // sets it explicitly (11px) so we don't inherit the root's larger base.
    // FontAtlas APIs take physical pixelSize; PrimitiveEmitContext::EmitText
    // takes LOGICAL fontSize and multiplies by contentScale internally.
    const float kLogicalFontSize = style.Visual.FontSize > 0.0f ? style.Visual.FontSize : 11.0f;
    const float kPixelSize = std::max(1.0f, kLogicalFontSize * cs);
    constexpr uint32 kLabelColor = 0xFFD0D0D0;
    const auto lm = ctx.FontAtlas->GetFontLineMetrics(kPixelSize);
    const float textY = y + std::max(0.0f, (h - lm.height) * 0.5f);
    auto emitLabel = [&](float anchorX, int frame, bool alignRight)
    {
        const std::string txt = std::to_string(frame);
        const float tw = ctx.FontAtlas->MeasureUtf8(txt, kPixelSize).width;
        float tx = x + anchorX;
        if (alignRight) tx -= tw;
        tx = std::clamp(tx, x + 1.0f * cs, x + w - tw - 1.0f * cs);
        ctx.EmitText(txt, tx, textY, kLogicalFontSize, kLabelColor, ctx.FontAtlas);
    };

    const int rangeStartFrame = static_cast<int>(std::round(m_RangeStart * m_Fps));
    const int rangeEndFrame   = static_cast<int>(std::round(m_RangeEnd   * m_Fps));

    // Suppress individual labels only when the two thumbs are too close to each other.
    const float kMinThumbSeparation = thumbSize + 44.0f * cs;
    const bool thumbsTooClose = (rightThumbX - leftThumbX) < kMinThumbSeparation;
    if (!thumbsTooClose)
    {
        emitLabel(leftThumbX  + thumbHalfW + 6.0f * cs, rangeStartFrame, false);
        emitLabel(rightThumbX - thumbHalfW - 6.0f * cs, rangeEndFrame,   true);
    }
}

} // namespace GameEngine

namespace RegisterAnimationWindow
{
static auto s_reg_timelineMarkers =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::TimelineMarkersElement>(
        "TimelineMarkersElement",
        []() { return std::make_unique<GameEngine::TimelineMarkersElement>(); })
        .TagAlias("timelinemarkerselement");
static auto s_reg_timelineTimeLabels =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::TimelineTimeLabelsElement>(
        "TimelineTimeLabelsElement",
        []() { return std::make_unique<GameEngine::TimelineTimeLabelsElement>(); })
        .TagAlias("timelinetimelabelselement");
static auto s_reg_timelineFrameLabels =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::TimelineFrameLabelsElement>(
        "TimelineFrameLabelsElement",
        []() { return std::make_unique<GameEngine::TimelineFrameLabelsElement>(); })
        .TagAlias("timelineframelabelselement");
static auto s_reg_timelineBar =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::TimelineBarElement>(
        "TimelineBarElement",
        []() { return std::make_unique<GameEngine::TimelineBarElement>(); })
        .TagAlias("timelinebarelement");
static auto s_reg_timeRangeSlider =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::TimeRangeSliderElement>(
        "TimeRangeSliderElement",
        []() { return std::make_unique<GameEngine::TimeRangeSliderElement>(); })
        .TagAlias("timerangesliderelement");
}
