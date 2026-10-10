#pragma once

#include "Panels/LaneClipModel.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include <chrono>
#include <functional>
#include <utility>
#include <vector>

namespace GameEngine
{

// Draws lane-based clip editor: lanes as rows, clips as horizontal blocks.
class LaneClipEditorView : public UIElement
{
public:
    using ClipMovedFn          = std::function<void(size_t laneIndex, size_t clipIndex, float newStartTime)>;
    using ClipResizedFn        = std::function<void(size_t laneIndex, size_t clipIndex, float newDuration)>;
    using ClipStartResizedFn   = std::function<void(size_t laneIndex, size_t clipIndex, float newStartTime, float newDuration)>;
    using ClipFadeInChangedFn  = std::function<void(size_t laneIndex, size_t clipIndex, float newFadeInDuration)>;
    using ClipFadeOutChangedFn = std::function<void(size_t laneIndex, size_t clipIndex, float newFadeOutDuration)>;
    using ClipSelectedFn = std::function<void(size_t laneIndex, size_t clipIndex)>;
    // Lane-level marker callbacks (time = absolute timeline time).
    using LaneMarkerAddedFn   = std::function<void(size_t laneIdx, float time)>;
    using LaneMarkerMovedFn   = std::function<void(size_t laneIdx, size_t markerIdx, float newTime)>;
    using LaneMarkerRemovedFn = std::function<void(size_t laneIdx, size_t markerIdx)>;
    // Clip-level marker callbacks (time = offset relative to clip instance start).
    using ClipMarkerAddedFn   = std::function<void(size_t laneIdx, size_t clipIdx, float relTime)>;
    using ClipMarkerMovedFn   = std::function<void(size_t laneIdx, size_t clipIdx, size_t markerIdx, float newRelTime)>;
    using ClipMarkerRemovedFn = std::function<void(size_t laneIdx, size_t clipIdx, size_t markerIdx)>;
    LaneClipEditorView();
    void SetModel(const LaneClipModel* model);
    void SetTimeRange(float rangeStart, float rangeEnd);
    void SetCurrentTime(float t);
    void SetOnClipMoved(ClipMovedFn fn)               { m_OnClipMoved         = std::move(fn); }
    void SetOnClipResized(ClipResizedFn fn)           { m_OnClipResized       = std::move(fn); }
    void SetOnClipStartResized(ClipStartResizedFn fn) { m_OnClipStartResized  = std::move(fn); }
    void SetOnClipFadeInChanged(ClipFadeInChangedFn fn)   { m_OnClipFadeInChanged   = std::move(fn); }
    void SetOnClipFadeOutChanged(ClipFadeOutChangedFn fn) { m_OnClipFadeOutChanged  = std::move(fn); }
    void SetOnClipSelected(ClipSelectedFn fn) { m_OnClipSelected = std::move(fn); }
    void SetSelectedClip(size_t laneIndex, size_t clipIndex);
    void ClearSelection();
    bool IsBoxSelecting() const { return m_BoxSelecting; }
    const std::vector<std::pair<size_t,size_t>>& GetSelectedClips() const { return m_SelectedClips; }
    void SetOnSeekToTime(std::function<void(float)> fn) { m_OnSeekToTime = std::move(fn); }
    void SetOnPan(std::function<void(float deltaTimeSeconds)> fn) { m_OnPan = std::move(fn); }
    void SetRowHeight(float h) { m_RowHeight = h; MarkDirty(VisualDirty); }
    void SetHeaderOffset(float offset) { m_HeaderOffset = offset; MarkDirty(VisualDirty); }
    void SetOnLaneMarkerAdded(LaneMarkerAddedFn fn)      { m_OnLaneMarkerAdded    = std::move(fn); }
    void SetOnLaneMarkerMoved(LaneMarkerMovedFn fn)      { m_OnLaneMarkerMoved    = std::move(fn); }
    void SetOnLaneMarkerRemoved(LaneMarkerRemovedFn fn)  { m_OnLaneMarkerRemoved  = std::move(fn); }
    void SetOnClipMarkerAdded(ClipMarkerAddedFn fn)      { m_OnClipMarkerAdded    = std::move(fn); }
    void SetOnClipMarkerMoved(ClipMarkerMovedFn fn)      { m_OnClipMarkerMoved    = std::move(fn); }
    void SetOnClipMarkerRemoved(ClipMarkerRemovedFn fn)  { m_OnClipMarkerRemoved  = std::move(fn); }
    void OnEvent(UIEvent& e) override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

private:
    bool HitTestLaneMarker(float localX, float localY, float W, size_t& laneIdx, size_t& markerIdx) const;
    bool HitTestClipMarker(float localX, float localY, float W, size_t& laneIdx, size_t& clipIdx, size_t& markerIdx) const;
    // Returns true if a clip fade handle was hit (top portion of clip block, at fade tip);
    // fills laneIdx/clipIdx, sets isLeft true for fade-in and false for fade-out.
    bool HitTestClipFadeHandle(float localX, float localY, float W, size_t& laneIdx, size_t& clipIdx, bool& isLeft) const;

    const LaneClipModel* m_Model = nullptr;
    float m_RangeStart = 0.0f;
    float m_RangeEnd = 10.0f;
    float m_CurrentTime = 0.0f;
    bool m_DraggingClip = false;
    size_t m_DragLaneIdx = 0;
    size_t m_DragClipIdx = 0;
    size_t m_SelectedLaneIdx = static_cast<size_t>(-1);
    size_t m_SelectedClipIdx = static_cast<size_t>(-1);
    float m_DragOffsetTime = 0.0f;
    std::vector<std::pair<std::pair<size_t,size_t>, float>> m_DragClipRelOffsets; // (lane,clip) → startTime relative to anchor at drag-start
    bool m_ResizingClip = false;
    bool m_ResizeLeft = false;
    size_t m_ResizeLaneIdx = 0;
    size_t m_ResizeClipIdx = 0;
    float m_ResizeDragOffset = 0.0f;
    bool m_HoveringResize = false;
    bool m_HoverResizeLeft = false;
    size_t m_HoverResizeLaneIdx = 0;
    size_t m_HoverResizeClipIdx = 0;
    // Fade-handle drag state (top-corner triangle drag adjusts fadeIn/fadeOutDuration).
    bool m_DraggingFade = false;
    bool m_FadeDragLeft = false;
    size_t m_FadeDragLaneIdx = 0;
    size_t m_FadeDragClipIdx = 0;
    bool m_HoveringFade = false;
    bool m_HoverFadeLeft = false;
    size_t m_HoverFadeLaneIdx = 0;
    size_t m_HoverFadeClipIdx = 0;
    ClipMovedFn         m_OnClipMoved;
    ClipResizedFn       m_OnClipResized;
    ClipStartResizedFn  m_OnClipStartResized;
    ClipFadeInChangedFn  m_OnClipFadeInChanged;
    ClipFadeOutChangedFn m_OnClipFadeOutChanged;
    ClipSelectedFn m_OnClipSelected;
    std::function<void(float)> m_OnSeekToTime;
    std::function<void(float)> m_OnPan;
    bool m_Panning = false;
    float m_PanLastGlobalX = 0.0f;
    bool m_Seeking = false;
    bool m_BoxSelecting = false;
    bool m_PendingEmptyAction = false;
    float m_EmptyActionStartX = 0.0f;
    float m_EmptyActionStartY = 0.0f;
    float m_BoxStartX = 0.0f;
    float m_BoxStartY = 0.0f;
    float m_BoxEndX = 0.0f;
    float m_BoxEndY = 0.0f;
    std::vector<std::pair<size_t,size_t>> m_SelectedClips;
    std::vector<std::pair<size_t,size_t>> m_BoxSelectionSeed;
    float m_RowHeight = 0.0f;
    float m_HeaderOffset = 0.0f;

    // Marker drag state
    enum class MarkerDragKind { None, Lane, Clip };
    MarkerDragKind m_MarkerDragKind = MarkerDragKind::None;
    size_t m_MarkerDragLaneIdx = 0;
    size_t m_MarkerDragClipIdx = 0;
    size_t m_MarkerDragIdx = 0;
    float m_MarkerDragStartY = 0.0f;
    float m_MarkerDragTimeOffset = 0.0f;
    LaneMarkerAddedFn    m_OnLaneMarkerAdded;
    LaneMarkerMovedFn    m_OnLaneMarkerMoved;
    LaneMarkerRemovedFn  m_OnLaneMarkerRemoved;
    ClipMarkerAddedFn    m_OnClipMarkerAdded;
    ClipMarkerMovedFn    m_OnClipMarkerMoved;
    ClipMarkerRemovedFn  m_OnClipMarkerRemoved;
    bool   m_HoveringMarker2      = false;
    bool   m_HoverMarker2IsClip   = false;
    size_t m_HoverMarker2LaneIdx  = 0;
    size_t m_HoverMarker2ClipIdx  = 0;
    size_t m_HoverMarker2Idx      = 0;

    // Double-click detection
    std::chrono::steady_clock::time_point m_LastClickTime{};
    float m_LastClickX = 0.0f;
    float m_LastClickY = 0.0f;
};

} // namespace GameEngine
