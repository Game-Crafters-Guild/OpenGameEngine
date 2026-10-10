#pragma once

#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include <functional>
#include <vector>

namespace GameEngine { namespace Animation { class AnimationClip; } }

namespace GameEngine
{

// Draws a dope sheet grid: channels as rows, keyframes as blocks on a time axis.
// Clicking a keyframe block selects it; dragging moves the keyframe in time.
class DopeSheetView : public UIElement
{
public:
    struct SelectedKey
    {
        int Channel = -1;
        float KeyTime = -1.0f;
    };

    using EditGestureFn = std::function<void()>;
    using KeyframeTimeChangedFn = std::function<bool(int channel, float currentTime, float newTime)>;
    using KeyframeSelectedFn = std::function<void(int channel, int keyIndex, float keyTime)>;

    struct TrackRow
    {
        int ChannelIndex = -1; // -1 = header/non-channel row; >= 0 = draw keys for this channel
        bool Selectable = true; // when false: keys render dimmed and reject hit/box selection
        bool operator==(const TrackRow& o) const { return ChannelIndex == o.ChannelIndex && Selectable == o.Selectable; }
        bool operator!=(const TrackRow& o) const { return !(*this == o); }
    };

    DopeSheetView();
    void SetClip(const Animation::AnimationClip* clip);
    void SetTimeRange(float rangeStart, float rangeEnd);
    void SetCurrentTime(float t);
    void SetActiveChannel(int channel);
    void SetVisibleChannels(const std::vector<int>& channels);
    /** Returns time of the selected keyframe, or -1.f if none selected. */
    float GetSelectedKeyframeTime() const;
    const std::vector<SelectedKey>& GetSelectedKeyframes() const { return m_SelectedKeys; }
    int GetSelectedChannel() const { return m_SelectedChannel; }
    int GetSelectedKeyIndex() const { return m_SelectedKeyIndex; }
    void ClearSelection();
    bool IsBoxSelecting() const { return m_BoxSelecting; }
    void SetOnKeyframeTimeChanged(KeyframeTimeChangedFn fn) { m_OnKeyframeTimeChanged = std::move(fn); }
    void SetOnKeyframeSelected(KeyframeSelectedFn fn) { m_OnKeyframeSelected = std::move(fn); }
    void SetOnSelectionChanged(std::function<void()> fn) { m_OnSelectionChanged = std::move(fn); }
    void SetOnSeekToTime(std::function<void(float)> fn) { m_OnSeekToTime = std::move(fn); }
    void SetOnSeekBegin(std::function<void()> fn) { m_OnSeekBegin = std::move(fn); }
    void SetOnSeekEnd(std::function<void()> fn) { m_OnSeekEnd = std::move(fn); }
    void SetOnPan(std::function<void(float deltaTimeSeconds)> fn) { m_OnPan = std::move(fn); }
    void SetOnEditStarted(EditGestureFn fn) { m_OnEditStarted = std::move(fn); }
    void SetOnEditFinished(EditGestureFn fn) { m_OnEditFinished = std::move(fn); }
    void SetRowHeight(float h) { m_RowHeight = h; MarkDirty(VisualDirty); }
    void SetTrackRows(const std::vector<TrackRow>& rows);
    void SetScrollOffset(float y);
    void SetRowTopOffset(float offset) { if (m_RowTopOffset != offset) { m_RowTopOffset = offset; MarkDirty(VisualDirty); } }
    void OnEvent(UIEvent& e) override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;
private:
    struct DragKey
    {
        int Channel = -1;
        float CurrentTime = -1.0f;
        float InitialTime = -1.0f;
    };

    const Animation::AnimationClip* m_Clip = nullptr;
    float m_RangeStart = 0.0f;
    float m_RangeEnd = 10.0f;
    float m_CurrentTime = 0.0f;
    int m_ActiveChannel = -1;
    std::vector<int> m_VisibleChannels;
    int m_SelectedChannel = -1;
    int m_SelectedKeyIndex = -1;
    float m_SelectedKeyTime = -1.0f;
    std::vector<SelectedKey> m_SelectedKeys;

    bool m_DraggingKey = false;
    bool m_ScalingSelection = false;
    bool m_BoxSelecting = false;
    bool m_PendingEmptyAction = false;
    bool m_BoxSelectModifierDown = false;
    int m_DragChannel = -1;
    float m_DragCurrentTime = 0.0f;
    float m_DragOffsetTime = 0.0f; // time_at_mouse - key_time when drag started (keeps key under cursor)
    float m_EmptyActionStartX = 0.0f;
    float m_EmptyActionStartY = 0.0f;
    float m_BoxStartX = 0.0f;
    float m_BoxStartY = 0.0f;
    float m_BoxEndX = 0.0f;
    float m_BoxEndY = 0.0f;
    float m_ScaleAnchorTime = 0.0f;
    float m_ScaleInitialDuration = 0.0f;
    std::vector<SelectedKey> m_BoxSelectionSeed;
    std::vector<DragKey> m_DragKeys;
    KeyframeTimeChangedFn m_OnKeyframeTimeChanged;
    KeyframeSelectedFn m_OnKeyframeSelected;
    std::function<void()> m_OnSelectionChanged;
    std::function<void(float)> m_OnSeekToTime;
    std::function<void()> m_OnSeekBegin;
    std::function<void()> m_OnSeekEnd;
    std::function<void(float)> m_OnPan;
    bool m_Panning = false;
    float m_PanLastGlobalX = 0.0f;
    EditGestureFn m_OnEditStarted;
    EditGestureFn m_OnEditFinished;
    bool m_Seeking = false;
    float m_RowHeight = 0.0f;
    float m_RowTopOffset = 0.0f;
    std::vector<TrackRow> m_TrackRows;
    float m_ScrollOffset = 0.0f;
};

} // namespace GameEngine
