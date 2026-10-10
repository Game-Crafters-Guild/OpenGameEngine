#include "UI/Controls/CubicBezierField.h"

#include "Editor/Settings/CurveEditorSettings.h"
#include "Platform/SystemMetrics.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIPrimitive.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace GameEngine
{
namespace
{

constexpr float kHandlePickPx = 13.0f;
constexpr float kDragThresholdPx = 3.0f;
constexpr float kDoubleClickDistancePx = 5.0f;
constexpr float kFieldHeightPx = 220.0f;
constexpr float kNormalHandleSizePx = 8.0f;
constexpr float kActiveHandleSizePx = 11.0f;
constexpr float kHandleEdgeInsetPx = 1.0f;
constexpr float kEndpointAxisInsetPx = kActiveHandleSizePx * 0.5f + kHandleEdgeInsetPx;
constexpr float kCompactEndpointHeightThresholdPx = 160.0f;
constexpr float kPadLeftPx = 14.0f;
constexpr float kPadTopPx = 12.0f;
constexpr float kPadRightPx = 14.0f;
constexpr float kPadBottomPx = 16.0f;

const uint32_t kCurveLineColor = UI::PackColor(0.32f, 0.68f, 1.0f, 1.0f);
const uint32_t kConnectorColor = UI::PackColor(0.36f, 0.95f, 0.50f, 0.78f);
const uint32_t kEndpointHandleColor = UI::PackColor(0.78f, 0.92f, 1.00f, 1.0f);
const uint32_t kControlHandleColor = UI::PackColor(1.0f, 0.74f, 0.28f, 1.0f);
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

} // namespace

CubicBezierField::CubicBezierField()
{
    AddClass("curve-field");
    Overrides()
        .Set(Style::Height, StyleLength::Px(kFieldHeightPx))
        .Set(Style::MinHeight, StyleLength::Px(kFieldHeightPx))
        .Set(Style::MarginTop, StyleLength::Px(8.0f));
}

void CubicBezierField::SetConfig(const Config& config)
{
    m_Config = config;
    MarkDirty(VisualDirty);
}

void CubicBezierField::SetShape(const CubicBezierShape& shape)
{
    m_Shape = shape;
    m_Shape.C1X = std::clamp(m_Shape.C1X, 0.0f, 1.0f);
    m_Shape.C2X = std::clamp(m_Shape.C2X, 0.0f, 1.0f);
    MarkDirty(VisualDirty);
}

void CubicBezierField::SetScrubberTime(float time)
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

void CubicBezierField::SetPlaybackIndicator(float time, float value, bool visible)
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

void CubicBezierField::SetOnChanging(std::function<void(const CubicBezierShape&)> callback)
{
    m_OnChanging = std::move(callback);
}

void CubicBezierField::SetOnChanged(std::function<void(const CubicBezierShape&)> callback)
{
    m_OnChanged = std::move(callback);
}

void CubicBezierField::SetOnPlaybackScrub(std::function<void(float)> callback)
{
    m_OnPlaybackScrub = std::move(callback);
}

void CubicBezierField::SetOnPlaybackScrubActiveChanged(std::function<void(bool)> callback)
{
    m_OnPlaybackScrubActiveChanged = std::move(callback);
}

void CubicBezierField::SetOnCurveDoubleClick(std::function<void()> callback)
{
    m_OnCurveDoubleClick = std::move(callback);
}

float CubicBezierField::EvaluateShape(const CubicBezierShape& shape, float normalizedTime)
{
    const float x = std::clamp(normalizedTime, 0.0f, 1.0f);
    const float c1x = std::clamp(shape.C1X, 0.0f, 1.0f);
    const float c2x = std::clamp(shape.C2X, 0.0f, 1.0f);

    auto sampleX = [c1x, c2x](float u) {
        const float inv = 1.0f - u;
        return 3.0f * inv * inv * u * c1x +
               3.0f * inv * u * u * c2x +
               u * u * u;
    };
    auto sampleY = [&shape](float u) {
        const float inv = 1.0f - u;
        const float inv3 = inv * inv * inv;
        const float u3 = u * u * u;
        return inv3 * shape.AnchorStartY +
               3.0f * inv * inv * u * shape.C1Y +
               3.0f * inv * u * u * shape.C2Y +
               u3 * shape.AnchorEndY;
    };

    float lo = 0.0f;
    float hi = 1.0f;
    float u = x;
    for (int i = 0; i < 16; ++i)
    {
        u = (lo + hi) * 0.5f;
        if (sampleX(u) < x)
            lo = u;
        else
            hi = u;
    }
    return sampleY(u);
}

void CubicBezierField::OnEvent(UIEvent& e)
{
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
            if (m_Config.AllowPlaybackScrub && PointInGraph(CurrentGraphRect(), e.X, e.Y))
            {
                m_PlaybackScrubActive = true;
                m_PlaybackScrubStarted = false;
                m_PlaybackScrubStartX = e.X;
                m_PlaybackScrubStartY = e.Y;
                if (m_OnPlaybackScrubActiveChanged)
                    m_OnPlaybackScrubActiveChanged(true);
                e.Capture(this);
                e.Stop();
            }
            return;
        }

        const GraphRect r = CurrentGraphRect();
        float hx = 0.0f;
        float hy = 0.0f;
        HandleScreen(r, hit, hx, hy);
        m_Drag = hit;
        m_DragMoved = false;
        m_DragStartX = e.X;
        m_DragStartY = e.Y;
        m_GrabOffsetX = hx - e.X;
        m_GrabOffsetY = hy - e.Y;
        UpdateCursor();
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
            m_OnChanged(m_Shape);
        e.Stop();
    }
}

void CubicBezierField::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle&,
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
    for (int i = 0; i <= 4; ++i)
    {
        const float t = static_cast<float>(i) / 4.0f;
        const float gx = r.X + t * r.W;
        const float gy = r.Y + t * r.H;
        const float thick = (i == 0 || i == 4) ? 1.4f * cs : 1.0f * cs;
        const uint32_t color = (i == 2) ? gridMajor : gridMinor;
        ctx.Emit(UI::MakeLine(gx, r.Y, gx, r.Y + r.H, thick, color));
        ctx.Emit(UI::MakeLine(r.X, gy, r.X + r.W, gy, thick, color));
    }

    const float startX = ToScreenX(r, 0.0f);
    const float startY = ToScreenY(r, m_Shape.AnchorStartY);
    const float endX = ToScreenX(r, 1.0f);
    const float endY = ToScreenY(r, m_Shape.AnchorEndY);
    const float c1x = ToScreenX(r, m_Shape.C1X);
    const float c1y = ToScreenY(r, m_Shape.C1Y);
    const float c2x = ToScreenX(r, m_Shape.C2X);
    const float c2y = ToScreenY(r, m_Shape.C2Y);

    ctx.Emit(UI::MakeLine(startX, startY, c1x, c1y, 1.5f * cs, kConnectorColor));
    ctx.Emit(UI::MakeLine(endX, endY, c2x, c2y, 1.5f * cs, kConnectorColor));
    ctx.Emit(UI::MakeBezier(startX, startY, c1x, c1y, c2x, c2y, endX, endY, 3.0f * cs, kCurveLineColor));

    DrawScrubber(ctx, r);
    DrawPlaybackIndicator(ctx, r);

    DrawHandle(ctx, c1x, c1y, kControlHandleColor, HandleIsActive(0));
    DrawHandle(ctx, c2x, c2y, kControlHandleColor, HandleIsActive(1));
    const float endpointMaxSize = compactEndpointHandles ? kNormalHandleSizePx : 0.0f;
    const float startSize = HandleLogicalSize(HandleIsActive(2), endpointMaxSize);
    const float endSize = HandleLogicalSize(HandleIsActive(3), endpointMaxSize);
    DrawHandle(ctx,
               startX,
               ClampHandleCenter(r.Y, r.Y + r.H, startY, startSize, cs),
               kEndpointHandleColor, HandleIsActive(2), endpointMaxSize);
    DrawHandle(ctx,
               endX,
               ClampHandleCenter(r.Y, r.Y + r.H, endY, endSize, cs),
               kEndpointHandleColor, HandleIsActive(3), endpointMaxSize);

    if (m_Drag >= 0 && m_DragMoved && ctx.FontAtlas && ctx.Textures)
    {
        constexpr float kHudW = 128.0f;
        constexpr float kHudH = 22.0f;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "t %.2f  v %.3f",
                      static_cast<double>(m_HudTime),
                      static_cast<double>(m_HudValue));
        float boxX = m_DragHudX + 14.0f * cs;
        float boxY = m_DragHudY - (kHudH + 14.0f) * cs;
        boxX = std::clamp(boxX, x + 4.0f * cs, std::max(x + 4.0f * cs, x + w - (kHudW + 4.0f) * cs));
        boxY = std::clamp(boxY, y + 4.0f * cs, std::max(y + 4.0f * cs, y + h - (kHudH + 4.0f) * cs));
        UI::UIPrimitive hud = UI::MakeRect(boxX, boxY, kHudW * cs, kHudH * cs,
                                           UI::PackColor(0.06f, 0.07f, 0.08f, 0.92f),
                                           4.0f * cs, 4.0f * cs, 4.0f * cs, 4.0f * cs);
        UI::AddBorder(hud, 1.0f * cs, UI::PackColor(0.40f, 0.44f, 0.48f, 0.90f));
        ctx.Emit(hud);
        ctx.EmitText(buf, boxX + 8.0f * cs, boxY + 4.0f * cs, 12.0f,
                     UI::PackColor(0.92f, 0.94f, 0.96f, 1.0f), ctx.FontAtlas);
    }
}

CubicBezierField::GraphRect CubicBezierField::CurrentGraphRect() const
{
    return ComputeGraphRect(GetLayoutX(), GetLayoutY(), GetLayoutWidth(), GetLayoutHeight());
}

CubicBezierField::GraphRect CubicBezierField::ComputeGraphRect(float x, float y, float w, float h, float scale)
{
    return {x + (kPadLeftPx + kEndpointAxisInsetPx) * scale,
            y + kPadTopPx * scale,
            std::max(1.0f, w - (kPadLeftPx + kPadRightPx + 2.0f * kEndpointAxisInsetPx) * scale),
            std::max(1.0f, h - (kPadTopPx + kPadBottomPx) * scale)};
}

bool CubicBezierField::PointInGraph(const GraphRect& r, float x, float y)
{
    return x >= r.X && x <= r.X + r.W && y >= r.Y && y <= r.Y + r.H;
}

float CubicBezierField::ToNormalizedTime(float time) const
{
    const float span = SpanOrEpsilon(m_Config.TimeMin, m_Config.TimeMax);
    return std::clamp((time - m_Config.TimeMin) / span, 0.0f, 1.0f);
}

float CubicBezierField::FromNormalizedTime(float normalizedTime) const
{
    return m_Config.TimeMin + std::clamp(normalizedTime, 0.0f, 1.0f) *
        SpanOrEpsilon(m_Config.TimeMin, m_Config.TimeMax);
}

float CubicBezierField::ToScreenX(const GraphRect& r, float normalizedTime) const
{
    return r.X + std::clamp(normalizedTime, 0.0f, 1.0f) * r.W;
}

float CubicBezierField::ToScreenY(const GraphRect& r, float value) const
{
    const float normalized = (value - m_Config.ValueMin) / SpanOrEpsilon(m_Config.ValueMin, m_Config.ValueMax);
    return r.Y + (1.0f - std::clamp(normalized, 0.0f, 1.0f)) * r.H;
}

float CubicBezierField::FromScreenX(const GraphRect& r, float px) const
{
    return std::clamp((px - r.X) / r.W, 0.0f, 1.0f);
}

float CubicBezierField::FromScreenY(const GraphRect& r, float py) const
{
    const float normalized = std::clamp(1.0f - (py - r.Y) / r.H, 0.0f, 1.0f);
    return m_Config.ValueMin + normalized * SpanOrEpsilon(m_Config.ValueMin, m_Config.ValueMax);
}

float CubicBezierField::SampleAt(float normalizedTime) const
{
    return EvaluateShape(m_Shape, normalizedTime);
}

void CubicBezierField::HandleScreen(const GraphRect& r, int handle, float& outX, float& outY) const
{
    switch (handle)
    {
    case 0:
        outX = ToScreenX(r, m_Shape.C1X);
        outY = ToScreenY(r, m_Shape.C1Y);
        return;
    case 1:
        outX = ToScreenX(r, m_Shape.C2X);
        outY = ToScreenY(r, m_Shape.C2Y);
        return;
    case 2:
        outX = ToScreenX(r, 0.0f);
        outY = ToScreenY(r, m_Shape.AnchorStartY);
        return;
    case 3:
        outX = ToScreenX(r, 1.0f);
        outY = ToScreenY(r, m_Shape.AnchorEndY);
        return;
    default:
        outX = outY = 0.0f;
        return;
    }
}

int CubicBezierField::HitTestHandle(float globalX, float globalY) const
{
    const GraphRect r = CurrentGraphRect();
    const float pickSq = kHandlePickPx * kHandlePickPx;
    float bestSq = pickSq + 1.0f;
    int best = -1;
    for (int i = 0; i < 4; ++i)
    {
        float hx = 0.0f;
        float hy = 0.0f;
        HandleScreen(r, i, hx, hy);
        const float dx = globalX - hx;
        const float dy = globalY - hy;
        const float dsq = dx * dx + dy * dy;
        if (dsq <= pickSq && dsq < bestSq)
        {
            bestSq = dsq;
            best = i;
        }
    }
    return best;
}

bool CubicBezierField::HandleIsActive(int handle) const
{
    return (m_Drag == handle) || (m_Drag < 0 && m_Hover == handle);
}

void CubicBezierField::UpdateHover(float globalX, float globalY)
{
    const int next = (globalX >= 0.0f && globalY >= 0.0f) ? HitTestHandle(globalX, globalY) : -1;
    if (next == m_Hover)
        return;
    m_Hover = next;
    UpdateCursor();
    MarkDirty(VisualDirty);
}

void CubicBezierField::UpdateCursor()
{
    const CursorStyle cursor = (m_Drag >= 0) ? CursorStyle::Grabbing
                             : (m_Hover >= 0) ? CursorStyle::Grab
                                              : CursorStyle::Auto;
    Overrides().Set(Style::Cursor, cursor);
}

bool CubicBezierField::TryHandleCurveDoubleClick(float globalX, float globalY)
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

void CubicBezierField::ApplyPlaybackScrub(float globalX)
{
    const GraphRect r = CurrentGraphRect();
    const float normalizedTime = FromScreenX(r, globalX);
    const float time = FromNormalizedTime(normalizedTime);
    m_PlaybackTime = time;
    m_PlaybackValue = SampleAt(normalizedTime);
    m_PlaybackIndicatorVisible = true;
    if (m_OnPlaybackScrub)
        m_OnPlaybackScrub(time);
    MarkDirty(VisualDirty);
}

void CubicBezierField::ApplyDrag(float globalX, float globalY)
{
    if (m_Drag < 0)
        return;
    const GraphRect r = CurrentGraphRect();
    const float anchorX = globalX + m_GrabOffsetX;
    const float anchorY = globalY + m_GrabOffsetY;
    const float tx = FromScreenX(r, anchorX);
    const float ty = FromScreenY(r, anchorY);

    if (m_Drag == 0)
    {
        m_Shape.C1X = tx;
        m_Shape.C1Y = ty;
    }
    else if (m_Drag == 1)
    {
        m_Shape.C2X = tx;
        m_Shape.C2Y = ty;
    }
    else if (m_Drag == 2)
    {
        m_Shape.AnchorStartY = ty;
    }
    else if (m_Drag == 3)
    {
        m_Shape.AnchorEndY = ty;
    }

    m_DragMoved = true;
    m_HudTime = (m_Drag == 2) ? m_Config.TimeMin : (m_Drag == 3) ? m_Config.TimeMax : FromNormalizedTime(tx);
    m_HudValue = ty;
    m_DragHudX = anchorX;
    m_DragHudY = anchorY;
    if (m_OnChanging)
        m_OnChanging(m_Shape);
    MarkDirty(VisualDirty);
}

void CubicBezierField::DrawHandle(UI::PrimitiveEmitContext& ctx, float x, float y, uint32_t color, bool active,
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

void CubicBezierField::DrawPlaybackIndicator(UI::PrimitiveEmitContext& ctx, const GraphRect& r) const
{
    if (!m_Config.ShowPlaybackIndicator || !m_PlaybackIndicatorVisible)
        return;

    const float cs = ctx.ContentScale;
    const float px = ToScreenX(r, ToNormalizedTime(m_PlaybackTime));
    const float py = ToScreenY(r, m_PlaybackValue);
    const float outputDotX = r.X - 0.5f * (kPadLeftPx + kEndpointAxisInsetPx) * cs;
    const uint32_t dotArgb = Editor::CurveEditorSettings::Get().PlaybackIndicatorDotColor;
    const uint32_t dotColor = UI::PackFromARGB(dotArgb);
    const uint32_t glowColor = UI::PackFromARGB(WithARGBAlpha(dotArgb, 0x66u));

    ctx.Emit(UI::MakeLine(px, r.Y, px, r.Y + r.H, 1.75f * cs,
                          UI::PackColor(0.20f, 0.92f, 0.82f, 0.85f)));

    UI::UIPrimitive outputDot = UI::MakeRect(outputDotX - 4.0f * cs, py - 4.0f * cs,
                                             8.0f * cs, 8.0f * cs,
                                             UI::PackColor(0.78f, 0.98f, 0.86f, 1.0f),
                                             4.0f * cs, 4.0f * cs, 4.0f * cs, 4.0f * cs);
    UI::AddBorder(outputDot, 1.5f * cs, UI::PackColor(0.90f, 1.0f, 0.94f, 1.0f));
    ctx.Emit(outputDot);

    ctx.Emit(UI::MakeRect(px - 7.0f * cs, py - 7.0f * cs,
                          14.0f * cs, 14.0f * cs,
                          glowColor,
                          7.0f * cs, 7.0f * cs, 7.0f * cs, 7.0f * cs));

    UI::UIPrimitive dot = UI::MakeRect(px - 6.0f * cs, py - 6.0f * cs,
                                       12.0f * cs, 12.0f * cs,
                                       dotColor,
                                       6.0f * cs, 6.0f * cs, 6.0f * cs, 6.0f * cs);
    UI::AddBorder(dot, 2.0f * cs, UI::PackColor(0.94f, 1.0f, 0.96f, 1.0f));
    ctx.Emit(dot);
}

void CubicBezierField::DrawScrubber(UI::PrimitiveEmitContext& ctx, const GraphRect& r) const
{
    if (!m_Config.ShowScrubber)
        return;
    const float cs = ctx.ContentScale;
    const float normalizedTime = ToNormalizedTime(m_ScrubberTime);
    const float px = ToScreenX(r, normalizedTime);
    const float py = ToScreenY(r, SampleAt(normalizedTime));
    ctx.Emit(UI::MakeLine(px, r.Y, px, r.Y + r.H, 1.5f * cs, kScrubberColor));
    UI::UIPrimitive dot = UI::MakeRect(px - 4.5f * cs, py - 4.5f * cs,
                                       9.0f * cs, 9.0f * cs,
                                       UI::PackColor(1.0f, 0.88f, 0.36f, 0.95f),
                                       4.5f * cs, 4.5f * cs, 4.5f * cs, 4.5f * cs);
    UI::AddBorder(dot, 1.5f * cs, UI::PackColor(1.0f, 0.96f, 0.64f, 1.0f));
    ctx.Emit(dot);
}

} // namespace GameEngine
