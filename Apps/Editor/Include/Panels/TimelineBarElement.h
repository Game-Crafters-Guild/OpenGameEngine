#pragma once

#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <functional>
#include <vector>

namespace GameEngine
{

// Draws markers above the timeline ruler at given times.
// Drag left/right to change time; drag down past threshold to remove.
class TimelineMarkersElement : public UIElement
{
public:
    TimelineMarkersElement();
    void SetMarkerTimes(std::vector<float> times);
    void SetTimeRange(float rangeStart, float rangeEnd);
    void SetCurrentTime(float currentTime);
    void SetOnMarkerTimeChanged(std::function<void(size_t index, float newTime)> cb) { m_OnMarkerTimeChanged = std::move(cb); }
    void SetOnMarkerRemoved(std::function<void(size_t index)> cb) { m_OnMarkerRemoved = std::move(cb); }
    void OnEvent(UIEvent& e) override;
    void OnPostLayout() override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

private:
    int IndexOfMarkerAt(float localX, float localY) const;
    float TimeFromGlobalX(float globalX) const;

    std::vector<float> m_MarkerTimes;
    float m_RangeStart = 0.0f;
    float m_RangeEnd = 10.0f;
    float m_CurrentTime = 0.0f;
    std::function<void(size_t index, float newTime)> m_OnMarkerTimeChanged;
    std::function<void(size_t index)> m_OnMarkerRemoved;
    int m_DragMarkerIndex = -1;
    float m_DragStartY = 0.0f;
    static constexpr float kDragDownRemoveThresholdPx = 24.0f;
};

// Draws time labels (e.g. "0.0s") above the timeline ruler. State set by AnimationWindowPanel.
// Supports scrub/pan like the ruler when callbacks are set.
class TimelineTimeLabelsElement : public UIElement
{
public:
    TimelineTimeLabelsElement();
    void SetTimelineState(float currentTime, float rangeStart, float rangeEnd, float fps);
    void SetOnTimeChange(std::function<void(float)> cb) { m_OnTimeChange = std::move(cb); }
    void SetOnPan(std::function<void(float deltaTimeSeconds)> cb) { m_OnPan = std::move(cb); }
    void SetOnZoom(std::function<void(float scrollY, float mouseX)> cb) { m_OnZoom = std::move(cb); }
    void SetOnScrubBegin(std::function<void()> cb) { m_OnScrubBegin = std::move(cb); }
    void SetOnScrubEnd(std::function<void()> cb) { m_OnScrubEnd = std::move(cb); }
    void OnEvent(UIEvent& e) override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

private:
    void UpdateTimeFromPosition(float globalX);
    bool IsOverPlayhead(float globalX) const;
    float m_CurrentTime = 0.0f;
    float m_RangeStart = 0.0f;
    float m_RangeEnd = 10.0f;
    float m_Fps = 60.0f;
    std::function<void(float)> m_OnTimeChange;
    std::function<void(float)> m_OnPan;
    std::function<void(float, float)> m_OnZoom;
    std::function<void()> m_OnScrubBegin;
    std::function<void()> m_OnScrubEnd;
    bool m_Scrubbing = false;
    int m_ScrubButton = -1;
    bool m_Panning = false;
    int m_PanButton = -1;
    float m_PanLastGlobalX = 0.0f;
};

// Draws frame labels (e.g. "0", "60") below the timeline ruler. State set by AnimationWindowPanel.
class TimelineFrameLabelsElement : public UIElement
{
public:
    TimelineFrameLabelsElement();
    void SetTimelineState(float currentTime, float rangeStart, float rangeEnd, float fps);
    void SetOnTimeChange(std::function<void(float)> cb) { m_OnTimeChange = std::move(cb); }
    void SetOnPan(std::function<void(float deltaTimeSeconds)> cb) { m_OnPan = std::move(cb); }
    void SetOnZoom(std::function<void(float scrollY, float mouseX)> cb) { m_OnZoom = std::move(cb); }
    void SetOnScrubBegin(std::function<void()> cb) { m_OnScrubBegin = std::move(cb); }
    void SetOnScrubEnd(std::function<void()> cb) { m_OnScrubEnd = std::move(cb); }
    void OnEvent(UIEvent& e) override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

private:
    void UpdateTimeFromPosition(float globalX);
    bool IsOverScrubArea(float globalX, float globalY) const;
    float m_CurrentTime = 0.0f;
    float m_RangeStart = 0.0f;
    float m_RangeEnd = 10.0f;
    float m_Fps = 60.0f;
    std::function<void(float)> m_OnTimeChange;
    std::function<void(float)> m_OnPan;
    std::function<void(float, float)> m_OnZoom;
    std::function<void()> m_OnScrubBegin;
    std::function<void()> m_OnScrubEnd;
    bool m_Scrubbing = false;
    int m_ScrubButton = -1;
    bool m_Panning = false;
    int m_PanButton = -1;
    float m_PanLastGlobalX = 0.0f;
};

// Draws a timeline ruler (time ticks), playhead line, and frame-number pill (between time and frame strips).
class TimelineBarElement : public UIElement
{
public:
    TimelineBarElement();
    void SetTimelineState(float currentTime, float rangeStart, float rangeEnd, float fps);
    void SetOnTimeChange(std::function<void(float)> cb) { m_OnTimeChange = std::move(cb); }
    void SetOnZoom(std::function<void(float scrollY, float mouseX)> cb) { m_OnZoom = std::move(cb); }
    void SetOnPan(std::function<void(float deltaTimeSeconds)> cb) { m_OnPan = std::move(cb); }
    void SetOnScrubBegin(std::function<void()> cb) { m_OnScrubBegin = std::move(cb); }
    void SetOnScrubEnd(std::function<void()> cb) { m_OnScrubEnd = std::move(cb); }
    void OnEvent(UIEvent& e) override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

private:
    void UpdateTimeFromPosition(float globalX);
    bool IsOverPlayheadStrip(float globalX) const;
    bool IsOverPlayheadScrub(float globalX, float globalY) const;
    float m_CurrentTime = 0.0f;
    float m_RangeStart = 0.0f;
    float m_RangeEnd = 10.0f;
    float m_Fps = 60.0f;
    std::function<void(float)> m_OnTimeChange;
    std::function<void(float, float)> m_OnZoom;
    std::function<void(float)> m_OnPan;
    std::function<void()> m_OnScrubBegin;
    std::function<void()> m_OnScrubEnd;
    bool m_Scrubbing = false;
    int m_ScrubButton = -1; // 0 = left, 2 = middle; which button started the scrub
    bool m_Panning = false;
    int m_PanButton = -1;
    float m_PanLastGlobalX = 0.0f;

    // Pill bounds stored in logical (CSS) pixels so IsOverPlayheadScrub can
    // compare them against logical e.x/y from UIEvent without DPI conversion.
    float m_PlayheadPillLeft = 0.0f;
    float m_PlayheadPillTop = 0.0f;
    float m_PlayheadPillW = 0.0f;
    float m_PlayheadPillH = 0.0f;
    bool m_PlayheadPillValid = false;
    bool m_PillHovered = false;
    float m_ContentScale = 1.0f;
};

// Thin scrub bar showing the full clip range with a draggable visible-window region.
// The left and right thumbs adjust rangeStart / rangeEnd; dragging the fill pans.
class TimeRangeSliderElement : public UIElement
{
public:
    TimeRangeSliderElement();
    void SetFullRange(float fullStart, float fullEnd);
    void SetVisibleRange(float rangeStart, float rangeEnd);
    void SetCurrentTime(float t);
    void SetFps(float fps);
    void SetOnRangeChanged(std::function<void(float, float)> fn) { m_OnRangeChanged = std::move(fn); }
    void SetOnPan(std::function<void(float deltaTimeSeconds)> fn) { m_OnPan = std::move(fn); }
    void SetOnSeekToTime(std::function<void(float)> fn) { m_OnSeekToTime = std::move(fn); }
    void OnEvent(UIEvent& e) override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

private:
    enum class DragMode { None, LeftThumb, RightThumb, Pan };
    enum class HoverZone { None, LeftThumb, RightThumb };
    float LocalXForTime(float t, float w) const;
    float TimeForLocalX(float localX, float w) const;

    float m_FullStart = 0.0f;
    float m_FullEnd = 10.0f;
    float m_RangeStart = 0.0f;
    float m_RangeEnd = 10.0f;
    float m_CurrentTime = 0.0f;
    float m_Fps = 60.0f;
    DragMode m_DragMode = DragMode::None;
    HoverZone m_HoverZone = HoverZone::None;
    float m_DragStartX = 0.0f;
    float m_DragStartRangeStart = 0.0f;
    float m_DragStartRangeEnd = 0.0f;
    float m_DragFullStart = 0.0f;
    float m_DragFullEnd = 10.0f;
    float m_DragLastPanTime = 0.0f;
    bool m_DragSawButtonDown = false;
    std::function<void(float, float)> m_OnRangeChanged;
    std::function<void(float)> m_OnPan;
    std::function<void(float)> m_OnSeekToTime;

    static constexpr float kThumbHalfW = 8.0f;
    static constexpr float kThumbHitExtra = 4.0f;
};

} // namespace GameEngine
