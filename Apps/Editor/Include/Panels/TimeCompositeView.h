#pragma once

#include "Panels/TimeCompositeModel.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/Interaction/DropTarget.h"
#include <chrono>
#include <filesystem>
#include <functional>
#include <utility>
#include <vector>

namespace GameEngine
{

// Draws the time composite: tracks as rows, clips as blocks on a time axis.
// Dragging a clip moves it in time (callback notifies new offset).
class TimeCompositeView : public UIElement, public UI::Interaction::IDropTarget
{
public:
    using ClipOffsetChangedFn  = std::function<void(size_t trackIndex, size_t clipIndex, float newOffset)>;
    using ClipEndChangedFn    = std::function<void(size_t trackIndex, size_t clipIndex, float newOutTime)>;
    using ClipStartChangedFn  = std::function<void(size_t trackIndex, size_t clipIndex, float newOffset, float newInTime)>;
    using ClipFadeInChangedFn  = std::function<void(size_t trackIndex, size_t clipIndex, float newFadeInDuration)>;
    using ClipFadeOutChangedFn = std::function<void(size_t trackIndex, size_t clipIndex, float newFadeOutDuration)>;
    using ClipDuplicatedFn = std::function<bool(size_t trackIndex, size_t clipIndex, size_t& outNewClipIndex)>;
    using ClipSelectedFn = std::function<void(size_t trackIndex, size_t clipIndex)>;
    using ClipDragFn = std::function<void(size_t trackIndex, size_t clipIndex)>;
    using MediaDroppedFn = std::function<void(size_t trackHint, float timeOffset, const std::vector<std::filesystem::path>&)>;
    using TrackReorderedFn   = std::function<void(size_t fromIdx, size_t toIdx)>;
    using ClipMovedToTrackFn = std::function<void(size_t fromTrack, size_t clipIdx, size_t toTrack, float newOffset)>;
    // Track-level marker callbacks (time = absolute timeline time).
    using TrackMarkerAddedFn  = std::function<void(size_t trackIdx, float time)>;
    using TrackMarkerMovedFn  = std::function<void(size_t trackIdx, size_t markerIdx, float newTime)>;
    using TrackMarkerRemovedFn = std::function<void(size_t trackIdx, size_t markerIdx)>;
    using TrackValueKeyAddedFn = std::function<size_t(size_t trackIdx, float time)>;
    using TrackValueKeyMovedFn = std::function<void(size_t trackIdx, size_t keyIdx, float newTime)>;
    using TrackValueKeyRemovedFn = std::function<void(size_t trackIdx, size_t keyIdx)>;
    using TrackMethodKeyAddedFn = std::function<size_t(size_t trackIdx, float time)>;
    using TrackMethodKeyMovedFn = std::function<void(size_t trackIdx, size_t keyIdx, float newTime)>;
    using TrackMethodKeyRemovedFn = std::function<void(size_t trackIdx, size_t keyIdx)>;
    using TrackAudioKeyAddedFn = std::function<size_t(size_t trackIdx, float time)>;
    using TrackAudioKeyMovedFn = std::function<void(size_t trackIdx, size_t keyIdx, float newTime)>;
    using TrackAudioKeyRemovedFn = std::function<void(size_t trackIdx, size_t keyIdx)>;
    using TrackAnimationKeyAddedFn = std::function<size_t(size_t trackIdx, float time)>;
    using TrackAnimationKeyMovedFn = std::function<void(size_t trackIdx, size_t keyIdx, float newTime)>;
    using TrackAnimationKeyRemovedFn = std::function<void(size_t trackIdx, size_t keyIdx)>;
    struct TrackKeyRef
    {
        size_t trackIdx = 0;
        CompositeTrackType type = CompositeTrackType::Animation;
        size_t keyIdx = 0;
        bool operator==(const TrackKeyRef& other) const
        {
            return trackIdx == other.trackIdx && type == other.type && keyIdx == other.keyIdx;
        }
    };
    using TrackKeySelectedFn = std::function<void(size_t trackIdx, CompositeTrackType type, size_t keyIdx)>;
    using TrackContextMenuFn = std::function<void(size_t trackIdx, float time, float screenX, float screenY)>;
    using TrackDragFn = std::function<void(size_t trackIdx)>;
    using TrackTimeOffsetChangedFn = std::function<void(size_t trackIdx, float deltaSeconds)>;
    // Clip-level marker callbacks (time = offset relative to clip start).
    using ClipMarkerAddedFn   = std::function<void(size_t trackIdx, size_t clipIdx, float relTime)>;
    using ClipMarkerMovedFn   = std::function<void(size_t trackIdx, size_t clipIdx, size_t markerIdx, float newRelTime)>;
    using ClipMarkerRemovedFn = std::function<void(size_t trackIdx, size_t clipIdx, size_t markerIdx)>;

    TimeCompositeView();
    void SetModel(const TimeCompositeModel* model);
    void SetTimeRange(float rangeStart, float rangeEnd);
    void SetCurrentTime(float t);
    void SetOnClipOffsetChanged(ClipOffsetChangedFn fn)  { m_OnClipOffsetChanged  = std::move(fn); }
    void SetOnClipEndChanged(ClipEndChangedFn fn)        { m_OnClipEndChanged     = std::move(fn); }
    void SetOnClipStartChanged(ClipStartChangedFn fn)    { m_OnClipStartChanged   = std::move(fn); }
    void SetOnClipFadeInChanged(ClipFadeInChangedFn fn)   { m_OnClipFadeInChanged   = std::move(fn); }
    void SetOnClipFadeOutChanged(ClipFadeOutChangedFn fn) { m_OnClipFadeOutChanged  = std::move(fn); }
    void SetOnClipDuplicated(ClipDuplicatedFn fn) { m_OnClipDuplicated = std::move(fn); }
    void SetSelectedClip(size_t trackIndex, size_t clipIndex);
    void SetSelectedTrackKey(size_t trackIndex, CompositeTrackType keyType, size_t keyIndex);
    void ArmTrackKeyForImmediateDrag(size_t trackIndex, CompositeTrackType keyType, size_t keyIndex);
    void ClearSelection();
    void ClearMarkerInteractionState();
    void SetOnClipSelected(ClipSelectedFn fn) { m_OnClipSelected = std::move(fn); }
    void SetOnClipDragStarted(ClipDragFn fn) { m_OnClipDragStarted = std::move(fn); }
    void SetOnClipDragEnded(ClipDragFn fn) { m_OnClipDragEnded = std::move(fn); }
    bool IsBoxSelecting() const { return m_BoxSelecting; }
    const std::vector<std::pair<size_t,size_t>>& GetSelectedClips() const { return m_SelectedClips; }
    const std::vector<TrackKeyRef>& GetSelectedTrackKeys() const { return m_SelectedTrackKeys; }
    void SetOnSeekToTime(std::function<void(float)> fn) { m_OnSeekToTime = std::move(fn); }
    void SetOnPan(std::function<void(float deltaTimeSeconds)> fn) { m_OnPan = std::move(fn); }
    void SetOnMediaDropped(MediaDroppedFn fn) { m_OnMediaDropped = std::move(fn); }
    void SetRowHeight(float h) { m_RowHeight = h; MarkDirty(VisualDirty); }
    void SetHeaderOffset(float offset) { m_HeaderOffset = offset; MarkDirty(VisualDirty); }
    void SetOnTrackMarkerAdded(TrackMarkerAddedFn fn)    { m_OnTrackMarkerAdded   = std::move(fn); }
    void SetOnTrackMarkerMoved(TrackMarkerMovedFn fn)    { m_OnTrackMarkerMoved   = std::move(fn); }
    void SetOnTrackMarkerRemoved(TrackMarkerRemovedFn fn){ m_OnTrackMarkerRemoved = std::move(fn); }
    void SetOnTrackValueKeyAdded(TrackValueKeyAddedFn fn)     { m_OnTrackValueKeyAdded = std::move(fn); }
    void SetOnTrackValueKeyMoved(TrackValueKeyMovedFn fn)     { m_OnTrackValueKeyMoved = std::move(fn); }
    void SetOnTrackValueKeyRemoved(TrackValueKeyRemovedFn fn) { m_OnTrackValueKeyRemoved = std::move(fn); }
    void SetOnTrackMethodKeyAdded(TrackMethodKeyAddedFn fn)     { m_OnTrackMethodKeyAdded = std::move(fn); }
    void SetOnTrackMethodKeyMoved(TrackMethodKeyMovedFn fn)     { m_OnTrackMethodKeyMoved = std::move(fn); }
    void SetOnTrackMethodKeyRemoved(TrackMethodKeyRemovedFn fn) { m_OnTrackMethodKeyRemoved = std::move(fn); }
    void SetOnTrackAudioKeyAdded(TrackAudioKeyAddedFn fn)       { m_OnTrackAudioKeyAdded = std::move(fn); }
    void SetOnTrackAudioKeyMoved(TrackAudioKeyMovedFn fn)       { m_OnTrackAudioKeyMoved = std::move(fn); }
    void SetOnTrackAudioKeyRemoved(TrackAudioKeyRemovedFn fn)   { m_OnTrackAudioKeyRemoved = std::move(fn); }
    void SetOnTrackAnimationKeyAdded(TrackAnimationKeyAddedFn fn)       { m_OnTrackAnimationKeyAdded = std::move(fn); }
    void SetOnTrackAnimationKeyMoved(TrackAnimationKeyMovedFn fn)       { m_OnTrackAnimationKeyMoved = std::move(fn); }
    void SetOnTrackAnimationKeyRemoved(TrackAnimationKeyRemovedFn fn)   { m_OnTrackAnimationKeyRemoved = std::move(fn); }
    void SetOnTrackKeySelected(TrackKeySelectedFn fn) { m_OnTrackKeySelected = std::move(fn); }
    void SetOnTrackContextMenu(TrackContextMenuFn fn) { m_OnTrackContextMenu = std::move(fn); }
    void SetOnTrackTimeDragStarted(TrackDragFn fn) { m_OnTrackTimeDragStarted = std::move(fn); }
    void SetOnTrackTimeOffsetChanged(TrackTimeOffsetChangedFn fn) { m_OnTrackTimeOffsetChanged = std::move(fn); }
    void SetOnTrackTimeDragEnded(TrackDragFn fn) { m_OnTrackTimeDragEnded = std::move(fn); }
    void SetOnClipMarkerAdded(ClipMarkerAddedFn fn)      { m_OnClipMarkerAdded    = std::move(fn); }
    void SetOnClipMarkerMoved(ClipMarkerMovedFn fn)      { m_OnClipMarkerMoved    = std::move(fn); }
    void SetOnClipMarkerRemoved(ClipMarkerRemovedFn fn)  { m_OnClipMarkerRemoved  = std::move(fn); }
    void SetOnTrackReordered(TrackReorderedFn fn)        { m_OnTrackReordered     = std::move(fn); }
    void SetOnClipMovedToTrack(ClipMovedToTrackFn fn)   { m_OnClipMovedToTrack   = std::move(fn); }
    void OnEvent(UIEvent& e) override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

    // IDropTarget
    bool AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const override;
    bool HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const override;
    UI::Interaction::DropFeedback CanDrop(const UI::Interaction::DropRequest& request) const override;
    void PerformDrop(const UI::Interaction::DropRequest& request) override;
    void SetDropPreview(const UI::Interaction::DropPreviewState& state) override;
private:
    // Returns true if a track marker was hit; fills trackIdx/markerIdx.
    bool HitTestTrackMarker(float localX, float localY, float W, size_t& trackIdx, size_t& markerIdx) const;
    bool HitTestTrackValueKey(float localX, float localY, float W, size_t& trackIdx, size_t& keyIdx) const;
    bool HitTestTrackMethodKey(float localX, float localY, float W, size_t& trackIdx, size_t& keyIdx) const;
    bool HitTestTrackAudioKey(float localX, float localY, float W, size_t& trackIdx, size_t& keyIdx) const;
    bool HitTestTrackAnimationKey(float localX, float localY, float W, size_t& trackIdx, size_t& keyIdx) const;
    // Returns true if a clip marker was hit; fills trackIdx/clipIdx/markerIdx.
    bool HitTestClipMarker(float localX, float localY, float W, size_t& trackIdx, size_t& clipIdx, size_t& markerIdx) const;
    // Returns true if a clip fade handle was hit (top portion of clip block, at fade tip);
    // fills trackIdx/clipIdx, sets isLeft true for fade-in and false for fade-out.
    bool HitTestClipFadeHandle(float localX, float localY, float W, size_t& trackIdx, size_t& clipIdx, bool& isLeft) const;

    float GetTimelineContentScale() const;
    float GetTrackKeyEdgeInsetPx() const;
    float GetTrackKeyHitHalfPx() const;

    const TimeCompositeModel* m_Model = nullptr;
    float m_RangeStart = 0.0f;
    float m_RangeEnd = 10.0f;
    float m_CurrentTime = 0.0f;

    bool m_DraggingClip = false;
    size_t m_DragTrackIdx = 0;
    size_t m_DragClipIdx = 0;
    size_t m_SelectedTrackIdx = static_cast<size_t>(-1);
    size_t m_SelectedClipIdx = static_cast<size_t>(-1);
    float m_DragOffsetTime = 0.0f; // time_at_mouse - clip_offset when drag started (keeps clip under cursor)
    std::vector<std::pair<std::pair<size_t,size_t>, float>> m_DragClipRelOffsets; // (track,clip) → startTime relative to anchor at drag-start
    bool m_ResizingClip = false;
    bool m_ResizeLeft = false;     // true = left-edge trim, false = right-edge trim
    size_t m_ResizeTrackIdx = 0;
    size_t m_ResizeClipIdx = 0;
    float m_ResizeDragOffset = 0.0f;
    bool m_HoveringResize = false;
    bool m_HoverResizeLeft = false;
    size_t m_HoverResizeTrackIdx = 0;
    size_t m_HoverResizeClipIdx = 0;
    // Fade-handle drag state (top-corner triangle drag adjusts fadeIn/fadeOutDuration).
    bool m_DraggingFade = false;
    bool m_FadeDragLeft = false;
    size_t m_FadeDragTrackIdx = 0;
    size_t m_FadeDragClipIdx = 0;
    bool m_HoveringFade = false;
    bool m_HoverFadeLeft = false;
    size_t m_HoverFadeTrackIdx = 0;
    size_t m_HoverFadeClipIdx = 0;
    ClipOffsetChangedFn  m_OnClipOffsetChanged;
    ClipEndChangedFn     m_OnClipEndChanged;
    ClipStartChangedFn   m_OnClipStartChanged;
    ClipFadeInChangedFn  m_OnClipFadeInChanged;
    ClipFadeOutChangedFn m_OnClipFadeOutChanged;
    ClipDuplicatedFn m_OnClipDuplicated;
    ClipSelectedFn m_OnClipSelected;
    ClipDragFn m_OnClipDragStarted;
    ClipDragFn m_OnClipDragEnded;
    std::function<void(float)> m_OnSeekToTime;
    std::function<void(float)> m_OnPan;
    MediaDroppedFn m_OnMediaDropped;
    bool m_Panning = false;
    float m_PanLastGlobalX = 0.0f;
    bool m_Seeking = false;
    bool m_BoxSelecting = false;
    bool m_PendingEmptyAction = false;
    bool m_EmptyActionShiftHeld = false;
    float m_EmptyActionStartX = 0.0f;
    float m_EmptyActionStartY = 0.0f;
    float m_BoxStartX = 0.0f;
    float m_BoxStartY = 0.0f;
    float m_BoxEndX = 0.0f;
    float m_BoxEndY = 0.0f;
    std::vector<std::pair<size_t,size_t>> m_SelectedClips;
    std::vector<std::pair<size_t,size_t>> m_BoxSelectionSeed;
    std::vector<TrackKeyRef> m_SelectedTrackKeys;
    std::vector<TrackKeyRef> m_BoxKeySelectionSeed;
    TrackKeyRef m_ArmedTrackKeyDrag;
    bool m_HasArmedTrackKeyDrag = false;
    float m_RowHeight = 0.0f;
    float m_HeaderOffset = 0.0f;
    bool m_DropPreviewVisible = false;
    bool m_DropPreviewAllowed = false;
    mutable float m_DropLastHoverX = 0.0f;
    size_t m_DropPreviewTrack = 0;

    // Marker drag state
    enum class MarkerDragKind { None, Track, Clip, ValueKey, MethodKey, AudioKey, AnimationKey };
    enum class PendingDragKind { None, Clip, Resize, Fade, Marker };
    void BeginTrackKeyDrag(size_t trackIndex, CompositeTrackType keyType, MarkerDragKind dragKind,
                           size_t keyIndex, float keyTime, float localY, float timeAtMouse, UIEvent& e,
                           bool selectKey = true);
    bool ResolveTrackKey(const TrackKeyRef& keyRef, MarkerDragKind& outDragKind, float& outKeyTime) const;
    PendingDragKind m_PendingDragKind = PendingDragKind::None;
    MarkerDragKind m_PendingMarkerDragKind = MarkerDragKind::None;
    float m_PendingDragStartX = 0.0f;
    float m_PendingDragStartY = 0.0f;
    MarkerDragKind m_MarkerDragKind = MarkerDragKind::None;
    size_t m_MarkerDragTrackIdx = 0;
    size_t m_MarkerDragClipIdx = 0;
    size_t m_MarkerDragIdx = 0;
    float m_MarkerDragStartY = 0.0f;
    float m_MarkerDragTimeOffset = 0.0f; // cursor time - marker time at drag start
    TrackMarkerAddedFn   m_OnTrackMarkerAdded;
    TrackMarkerMovedFn   m_OnTrackMarkerMoved;
    TrackMarkerRemovedFn m_OnTrackMarkerRemoved;
    TrackValueKeyAddedFn   m_OnTrackValueKeyAdded;
    TrackValueKeyMovedFn   m_OnTrackValueKeyMoved;
    TrackValueKeyRemovedFn m_OnTrackValueKeyRemoved;
    TrackMethodKeyAddedFn   m_OnTrackMethodKeyAdded;
    TrackMethodKeyMovedFn   m_OnTrackMethodKeyMoved;
    TrackMethodKeyRemovedFn m_OnTrackMethodKeyRemoved;
    TrackAudioKeyAddedFn    m_OnTrackAudioKeyAdded;
    TrackAudioKeyMovedFn    m_OnTrackAudioKeyMoved;
    TrackAudioKeyRemovedFn  m_OnTrackAudioKeyRemoved;
    TrackAnimationKeyAddedFn    m_OnTrackAnimationKeyAdded;
    TrackAnimationKeyMovedFn    m_OnTrackAnimationKeyMoved;
    TrackAnimationKeyRemovedFn  m_OnTrackAnimationKeyRemoved;
    TrackKeySelectedFn m_OnTrackKeySelected;
    TrackContextMenuFn m_OnTrackContextMenu;
    ClipMarkerAddedFn    m_OnClipMarkerAdded;
    ClipMarkerMovedFn    m_OnClipMarkerMoved;
    ClipMarkerRemovedFn  m_OnClipMarkerRemoved;

    // Double-click detection
    std::chrono::steady_clock::time_point m_LastClickTime{};
    float m_LastClickX = 0.0f;
    float m_LastClickY = 0.0f;

    // Track reorder drag state
    bool m_ReorderingTrack = false;
    size_t m_ReorderDragTrackIdx = 0;
    size_t m_ReorderTargetIdx = 0;
    bool m_MovingTrackInTime = false;
    size_t m_TimeDragTrackIdx = 0;
    float m_TimeDragLastX = 0.0f;
    // Cross-track clip drag target row (updated during clip drag)
    size_t m_DragTargetTrack = 0;
    TrackReorderedFn   m_OnTrackReordered;
    ClipMovedToTrackFn m_OnClipMovedToTrack;
    TrackDragFn m_OnTrackTimeDragStarted;
    TrackTimeOffsetChangedFn m_OnTrackTimeOffsetChanged;
    TrackDragFn m_OnTrackTimeDragEnded;
    bool   m_HoveringMarker      = false;
    bool   m_HoverMarkerIsClip   = false;
    MarkerDragKind m_HoverMarkerKind = MarkerDragKind::None;
    size_t m_HoverMarkerTrackIdx = 0;
    size_t m_HoverMarkerClipIdx  = 0;
    size_t m_HoverMarkerIdx      = 0;
};

} // namespace GameEngine
