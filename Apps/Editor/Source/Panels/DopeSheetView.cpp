#include "Panels/DopeSheetView.h"

#include "Panels/Animation/KeyTimeMatch.h"

#include "Assets/AnimationClip.h"
#include "Input/InputSystem.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UiContext.h"
#include "UI/UIPrimitive.h"
#include "Rendering/Geometry/ShapeBuilder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace GameEngine
{

namespace
{
const float kMaxKeyBlockHeightPx = 28.0f;
constexpr float kSelectionHandleHalfSizePx = 5.0f;
constexpr float kBoxSelectThresholdSq = 16.0f;

bool ContainsSelection(const std::vector<DopeSheetView::SelectedKey>& selection, int channel, float keyTime)
{
    return std::any_of(selection.begin(), selection.end(),
                       [channel, keyTime](const DopeSheetView::SelectedKey& key)
                       {
                           return key.Channel == channel && KeyTimesMatch(key.KeyTime, keyTime);
                       });
}

struct SelectionBounds
{
    bool Valid = false;
    float Left = 0.0f;
    float Top = 0.0f;
    float Right = 0.0f;
    float Bottom = 0.0f;
    float HandleLeft = 0.0f;
    float HandleTop = 0.0f;
    float HandleRight = 0.0f;
    float HandleBottom = 0.0f;
    float MinTime = 0.0f;
    float MaxTime = 0.0f;
};

SelectionBounds ComputeSelectionBounds(const AnimationClip* clip,
                                       const std::vector<DopeSheetView::SelectedKey>& selection,
                                       float drawX, float drawY, float drawW, float drawH,
                                       float rangeStart, float rangeEnd,
                                       float rowH,
                                       const std::vector<DopeSheetView::TrackRow>& trackRows,
                                       float topOffset,
                                       float scrollOffset)
{
    SelectionBounds bounds;
    if (!clip || selection.empty() || drawW <= 0.0f || drawH <= 0.0f)
        return bounds;

    const std::vector<AnimChannel>& channels = clip->GetChannels();
    if (channels.empty())
        return bounds;

    const float rangeDuration = std::max(0.001f, rangeEnd - rangeStart);
    const bool useTrackRows = !trackRows.empty() && rowH > 0.0f;
    const int channelRows = useTrackRows ? static_cast<int>(trackRows.size())
                                         : std::max(1, static_cast<int>(channels.size()));
    const float effectiveRowH = useTrackRows ? rowH : drawH / static_cast<float>(channelRows);
    const float blockH = std::min(effectiveRowH * 0.7f, kMaxKeyBlockHeightPx);
    const float blockW = 6.0f;

    bool initialized = false;
    for (const DopeSheetView::SelectedKey& key : selection)
    {
        if (key.Channel < 0 || static_cast<size_t>(key.Channel) >= channels.size())
            continue;

        float rowY;
        if (useTrackRows)
        {
            int rowIdx = -1;
            for (size_t i = 0; i < trackRows.size(); ++i)
            {
                if (trackRows[i].ChannelIndex == key.Channel) { rowIdx = static_cast<int>(i); break; }
            }
            if (rowIdx < 0) continue;
            rowY = drawY + topOffset + static_cast<float>(rowIdx) * effectiveRowH - scrollOffset + effectiveRowH * 0.15f;
        }
        else
        {
            rowY = drawY + (static_cast<float>(key.Channel) + 0.15f) * effectiveRowH;
        }

        const float keyX = drawX + (key.KeyTime - rangeStart) / rangeDuration * drawW;
        const float left = keyX - blockW * 0.5f;
        const float right = keyX + blockW * 0.5f;
        const float top = rowY;
        const float bottom = rowY + blockH;

        if (!initialized)
        {
            bounds.Left = left; bounds.Top = top; bounds.Right = right; bounds.Bottom = bottom;
            bounds.MinTime = bounds.MaxTime = key.KeyTime;
            initialized = true;
            continue;
        }
        bounds.Left = std::min(bounds.Left, left);
        bounds.Top = std::min(bounds.Top, top);
        bounds.Right = std::max(bounds.Right, right);
        bounds.Bottom = std::max(bounds.Bottom, bottom);
        bounds.MinTime = std::min(bounds.MinTime, key.KeyTime);
        bounds.MaxTime = std::max(bounds.MaxTime, key.KeyTime);
    }

    if (!initialized)
        return bounds;

    bounds.Valid = true;
    bounds.Left -= 4.0f; bounds.Top -= 4.0f;
    bounds.Right += 4.0f; bounds.Bottom += 4.0f;
    const float handleCenterY = (bounds.Top + bounds.Bottom) * 0.5f;
    bounds.HandleLeft = bounds.Right - kSelectionHandleHalfSizePx;
    bounds.HandleRight = bounds.Right + kSelectionHandleHalfSizePx;
    bounds.HandleTop = handleCenterY - kSelectionHandleHalfSizePx;
    bounds.HandleBottom = handleCenterY + kSelectionHandleHalfSizePx;
    return bounds;
}

std::vector<DopeSheetView::SelectedKey> BuildBoxSelection(const AnimationClip* clip,
                                                          float drawW, float drawH,
                                                          float rangeStart, float rangeEnd,
                                                          float boxStartX, float boxStartY,
                                                          float boxEndX, float boxEndY,
                                                          const std::vector<DopeSheetView::SelectedKey>& seed,
                                                          float rowH,
                                                          const std::vector<DopeSheetView::TrackRow>& trackRows,
                                                          float topOffset,
                                                          float scrollOffset)
{
    std::vector<DopeSheetView::SelectedKey> selection = seed;
    if (!clip || drawW <= 0.0f || drawH <= 0.0f)
        return selection;

    const float boxLeft = std::min(boxStartX, boxEndX);
    const float boxRight = std::max(boxStartX, boxEndX);
    const float boxTop = std::min(boxStartY, boxEndY);
    const float boxBottom = std::max(boxStartY, boxEndY);
    const float rangeDuration = std::max(0.001f, rangeEnd - rangeStart);
    const std::vector<AnimChannel>& channels = clip->GetChannels();
    const float blockW = 6.0f;

    const bool useTrackRows = !trackRows.empty() && rowH > 0.0f;
    if (useTrackRows)
    {
        const float blockH = std::min(rowH * 0.7f, kMaxKeyBlockHeightPx);
        for (size_t rowIdx = 0; rowIdx < trackRows.size(); ++rowIdx)
        {
            if (!trackRows[rowIdx].Selectable) continue;
            const int ch = trackRows[rowIdx].ChannelIndex;
            if (ch < 0 || static_cast<size_t>(ch) >= channels.size()) continue;
            const float rowY = topOffset + static_cast<float>(rowIdx) * rowH - scrollOffset + rowH * 0.15f;
            if (rowY + blockH < boxTop || rowY > boxBottom) continue;
            const AnimChannel& channel = channels[static_cast<size_t>(ch)];
            for (const AnimKeyframe& keyframe : channel.keys)
            {
                const float keyX = (keyframe.time - rangeStart) / rangeDuration * drawW;
                const float keyLeft = keyX - blockW * 0.5f;
                const float keyRight = keyX + blockW * 0.5f;
                if (keyRight < boxLeft || keyLeft > boxRight) continue;
                if (!ContainsSelection(selection, ch, keyframe.time))
                    selection.push_back({ch, keyframe.time});
            }
        }
    }
    else
    {
        const int channelRows = std::max(1, static_cast<int>(channels.size()));
        const float effectiveRowH = drawH / static_cast<float>(channelRows);
        const float blockH = std::min(effectiveRowH * 0.7f, kMaxKeyBlockHeightPx);
        for (size_t channelIndex = 0; channelIndex < channels.size(); ++channelIndex)
        {
            const AnimChannel& channel = channels[channelIndex];
            const float rowY = (static_cast<float>(channelIndex) + 0.15f) * effectiveRowH;
            if (rowY + blockH < boxTop || rowY > boxBottom) continue;
            for (const AnimKeyframe& keyframe : channel.keys)
            {
                const float keyX = (keyframe.time - rangeStart) / rangeDuration * drawW;
                const float keyLeft = keyX - blockW * 0.5f;
                const float keyRight = keyX + blockW * 0.5f;
                if (keyRight < boxLeft || keyLeft > boxRight) continue;
                if (!ContainsSelection(selection, static_cast<int>(channelIndex), keyframe.time))
                    selection.push_back({static_cast<int>(channelIndex), keyframe.time});
            }
        }
    }

    return selection;
}
}

DopeSheetView::DopeSheetView()
{
    AddClass("animationwindow-dopesheet");
}

void DopeSheetView::SetClip(const AnimationClip* clip)
{
    if (m_Clip != clip)
    {
        m_Clip = clip;
        m_SelectedChannel = -1;
        m_SelectedKeyIndex = -1;
        m_SelectedKeyTime = -1.0f;
        m_SelectedKeys.clear();
        m_DragKeys.clear();
        m_ScalingSelection = false;
        m_BoxSelecting = false;
        m_PendingEmptyAction = false;
        m_BoxSelectModifierDown = false;
        MarkDirty(VisualDirty);
    }
}

void DopeSheetView::SetActiveChannel(int channel)
{
    if (m_ActiveChannel != channel)
    {
        m_ActiveChannel = channel;
        MarkDirty(VisualDirty);
    }
}

void DopeSheetView::SetVisibleChannels(const std::vector<int>& channels)
{
    if (m_VisibleChannels != channels)
    {
        m_VisibleChannels = channels;
        MarkDirty(VisualDirty);
    }
}

void DopeSheetView::ClearSelection()
{
    if (m_SelectedChannel != -1 || m_SelectedKeyIndex != -1 || !m_SelectedKeys.empty())
    {
        m_SelectedChannel = -1;
        m_SelectedKeyIndex = -1;
        m_SelectedKeyTime = -1.0f;
        m_SelectedKeys.clear();
        m_DragKeys.clear();
        m_ScalingSelection = false;
        m_BoxSelecting = false;
        m_PendingEmptyAction = false;
        m_BoxSelectModifierDown = false;
        MarkDirty(VisualDirty);
    }
}

static void HitTestKeyframes(const AnimationClip* clip,
                             float localX, float localY,
                             float drawX, float drawY, float drawW, float drawH,
                             float rangeStart, float rangeEnd,
                             int& outChannel, int& outKeyIndex, float& outKeyTime,
                             float rowH,
                             const std::vector<DopeSheetView::TrackRow>& trackRows,
                             float topOffset,
                             float scrollOffset)
{
    outChannel = -1;
    outKeyIndex = -1;
    outKeyTime = 0.0f;
    if (!clip || drawW <= 0.0f || drawH <= 0.0f)
        return;
    const float rangeDuration = std::max(0.001f, rangeEnd - rangeStart);
    const std::vector<AnimChannel>& channels = clip->GetChannels();
    const float blockW = 6.0f;
    const float relX = localX - drawX;
    const float relY = localY - drawY;
    if (relX < 0.0f || relX > drawW || relY < 0.0f || relY > drawH)
        return;

    const bool useTrackRows = !trackRows.empty() && rowH > 0.0f;
    if (useTrackRows)
    {
        const float blockH = std::min(rowH * 0.7f, kMaxKeyBlockHeightPx);
        for (size_t rowIdx = 0; rowIdx < trackRows.size(); ++rowIdx)
        {
            if (!trackRows[rowIdx].Selectable) continue;
            const int ch = trackRows[rowIdx].ChannelIndex;
            if (ch < 0 || static_cast<size_t>(ch) >= channels.size()) continue;
            const float rowBase = topOffset + static_cast<float>(rowIdx) * rowH - scrollOffset;
            if (relY < rowBase || relY > rowBase + rowH) continue;
            const float keyRowY = rowBase + rowH * 0.15f;
            const AnimChannel& channel = channels[static_cast<size_t>(ch)];
            for (size_t ki = 0; ki < channel.keys.size(); ++ki)
            {
                float kfTime = channel.keys[ki].time;
                float kx = (kfTime - rangeStart) / rangeDuration * drawW;
                float left = kx - blockW * 0.5f;
                if (relX >= left && relX <= left + blockW && relY >= keyRowY && relY <= keyRowY + blockH)
                {
                    outChannel = ch;
                    outKeyIndex = static_cast<int>(ki);
                    outKeyTime = kfTime;
                    return;
                }
            }
        }
    }
    else
    {
        const int channelRows = std::max(1, static_cast<int>(channels.size()));
        const float effectiveRowH = drawH / static_cast<float>(channelRows);
        const float blockH = std::min(effectiveRowH * 0.7f, kMaxKeyBlockHeightPx);
        for (size_t ch = 0; ch < channels.size(); ++ch)
        {
            const AnimChannel& channel = channels[ch];
            float keyRowY = (static_cast<float>(ch) + 0.15f) * effectiveRowH;
            for (size_t ki = 0; ki < channel.keys.size(); ++ki)
            {
                float kfTime = channel.keys[ki].time;
                float kx = (kfTime - rangeStart) / rangeDuration * drawW;
                float left = kx - blockW * 0.5f;
                if (relX >= left && relX <= left + blockW && relY >= keyRowY && relY <= keyRowY + blockH)
                {
                    outChannel = static_cast<int>(ch);
                    outKeyIndex = static_cast<int>(ki);
                    outKeyTime = kfTime;
                    return;
                }
            }
        }
    }
}

void DopeSheetView::OnEvent(UIEvent& e)
{
    const float W = GetLayoutWidth();
    const float H = GetLayoutHeight();
    const float localX = e.X - GetLayoutX();
    const float clampedLocalX = std::clamp(localX, 0.0f, W);
    const float localY = e.Y - GetLayoutY();
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const bool shiftHeld = (e.Mods & Input::kModShift) != 0;
    const bool controlHeld = (e.Mods & Input::kModControl) != 0;
    const bool insideGrid = (W > 0.0f && H > 0.0f &&
                             localX >= 0.0f && localX <= W &&
                             localY >= 0.0f && localY <= H);
    const float timeAtMouse = (W > 0.0f && H > 0.0f)
                                  ? m_RangeStart + (clampedLocalX / W) * rangeDuration
                                  : m_RangeStart;

    const SelectionBounds selectionBounds =
        ComputeSelectionBounds(m_Clip, m_SelectedKeys, 0.0f, 0.0f, W, H, m_RangeStart, m_RangeEnd,
                               m_RowHeight, m_TrackRows, m_RowTopOffset, m_ScrollOffset);

    if (e.Id == kEventMouseUp)
    {
        if (m_Panning)
        {
            m_Panning = false;
            e.Stop();
            return;
        }
        const bool hadDraggingKey = m_DraggingKey;
        const bool hadScalingSelection = m_ScalingSelection;
        const bool hadBoxSelecting = m_BoxSelecting;
        m_DraggingKey = false;
        m_ScalingSelection = false;
        m_BoxSelecting = false;
        m_PendingEmptyAction = false;
        m_BoxSelectModifierDown = false;
        m_DragKeys.clear();
        if (hadDraggingKey)
        {
            // After dragging, the key array may have been re-sorted by
            // SetKeyframeTime, so m_SelectedKeyIndex is potentially stale.
            // Find the keyframe at the final drag time to keep the selection valid.
            if (m_Clip && m_DragChannel >= 0)
            {
                const auto& channels = m_Clip->GetChannels();
                if (static_cast<size_t>(m_DragChannel) < channels.size())
                {
                    const auto& keys = channels[static_cast<size_t>(m_DragChannel)].keys;
                    int bestIdx = -1;
                    float bestDist = std::numeric_limits<float>::max();
                    for (size_t ki = 0; ki < keys.size(); ++ki)
                    {
                        float dist = std::fabs(keys[ki].time - m_DragCurrentTime);
                        if (dist < bestDist)
                        {
                            bestDist = dist;
                            bestIdx = static_cast<int>(ki);
                        }
                    }
                    m_SelectedChannel = m_DragChannel;
                    m_SelectedKeyIndex = bestIdx;
                    MarkDirty(VisualDirty);
                }
            }
        }
        if ((hadDraggingKey || hadScalingSelection) && m_OnEditFinished)
            m_OnEditFinished();
        if (hadBoxSelecting)
        {
            MarkDirty(VisualDirty);
            if (m_OnSelectionChanged) m_OnSelectionChanged();
        }
        if (m_Seeking)
        {
            m_Seeking = false;
            if (m_OnSeekEnd) m_OnSeekEnd();
        }
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseMove)
    {
        if (m_Panning && m_OnPan && W > 0.0f)
        {
            const float deltaPx = e.X - m_PanLastGlobalX;
            const float deltaTime = (deltaPx / W) * rangeDuration;
            m_OnPan(deltaTime);
            m_PanLastGlobalX = e.X;
            e.Stop();
            return;
        }
        if ((m_BoxSelecting || m_PendingEmptyAction) && m_Clip)
        {
            const float dx = localX - m_EmptyActionStartX;
            const float dy = localY - m_EmptyActionStartY;
            const bool shouldBoxSelect = m_BoxSelecting || (dx * dx + dy * dy) >= kBoxSelectThresholdSq;
            if (shouldBoxSelect)
            {
                m_BoxSelecting = true;
                m_BoxStartX = m_EmptyActionStartX;
                m_BoxStartY = m_EmptyActionStartY;
                m_BoxEndX = localX;
                m_BoxEndY = localY;
                m_SelectedKeys = BuildBoxSelection(m_Clip,
                                                   W,
                                                   H,
                                                   m_RangeStart,
                                                   m_RangeEnd,
                                                   m_BoxStartX,
                                                   m_BoxStartY,
                                                   m_BoxEndX,
                                                   m_BoxEndY,
                                                   shiftHeld ? m_BoxSelectionSeed : std::vector<SelectedKey>{},
                                                   m_RowHeight, m_TrackRows, m_RowTopOffset, m_ScrollOffset);
                if (m_SelectedKeys.empty())
                {
                    m_SelectedChannel = -1;
                    m_SelectedKeyIndex = -1;
                    m_SelectedKeyTime = -1.0f;
                }
                else
                {
                    const SelectedKey& primaryKey = m_SelectedKeys.front();
                    m_SelectedChannel = primaryKey.Channel;
                    m_SelectedKeyTime = primaryKey.KeyTime;
                    m_SelectedKeyIndex = -1;
                }
                if (m_OnSelectionChanged) m_OnSelectionChanged();
                MarkDirty(VisualDirty);
                e.Stop();
                return;
            }
        }

        if (m_DraggingKey && m_Clip && m_OnKeyframeTimeChanged && W > 0.0f)
        {
            float newTime = timeAtMouse - m_DragOffsetTime;
            newTime = std::max(0.0f, std::min(newTime, m_Clip->GetDuration() * 1.1f));
            const float deltaTime = newTime - m_DragCurrentTime;
            if (std::abs(deltaTime) > 0.0f)
            {
                std::vector<size_t> order(m_DragKeys.size());
                for (size_t index = 0; index < order.size(); ++index)
                    order[index] = index;
                std::sort(order.begin(), order.end(),
                          [this, deltaTime](size_t left, size_t right)
                          {
                              return deltaTime >= 0.0f
                                  ? m_DragKeys[left].CurrentTime > m_DragKeys[right].CurrentTime
                                  : m_DragKeys[left].CurrentTime < m_DragKeys[right].CurrentTime;
                          });
                for (size_t orderIndex : order)
                {
                    DragKey& dragKey = m_DragKeys[orderIndex];
                    const float targetTime = std::max(0.0f, std::min(dragKey.CurrentTime + deltaTime, m_Clip->GetDuration() * 1.1f));
                    if (!m_OnKeyframeTimeChanged(dragKey.Channel, dragKey.CurrentTime, targetTime))
                        continue;
                    dragKey.CurrentTime = targetTime;
                }

                m_SelectedKeys.clear();
                m_SelectedKeys.reserve(m_DragKeys.size());
                for (const DragKey& dragKey : m_DragKeys)
                    m_SelectedKeys.push_back({dragKey.Channel, dragKey.CurrentTime});
                m_DragCurrentTime = newTime;
                m_SelectedKeyTime = newTime;
            }
            MarkDirty(VisualDirty);
            e.Stop();
        }
        else if (m_ScalingSelection && m_Clip && m_OnKeyframeTimeChanged && m_ScaleInitialDuration > 0.0f)
        {
            const float maxTime = m_Clip->GetDuration() * 1.1f;
            const float rawScale = (timeAtMouse - m_ScaleAnchorTime) / m_ScaleInitialDuration;
            const float scale = std::clamp(rawScale, 0.05f, 20.0f);
            std::vector<size_t> order(m_DragKeys.size());
            for (size_t index = 0; index < order.size(); ++index)
                order[index] = index;
            std::sort(order.begin(), order.end(),
                      [this, scale](size_t left, size_t right)
                      {
                          return scale >= 1.0f
                              ? m_DragKeys[left].InitialTime > m_DragKeys[right].InitialTime
                              : m_DragKeys[left].InitialTime < m_DragKeys[right].InitialTime;
                      });
            for (size_t orderIndex : order)
            {
                DragKey& dragKey = m_DragKeys[orderIndex];
                const float targetTime =
                    std::clamp(m_ScaleAnchorTime + (dragKey.InitialTime - m_ScaleAnchorTime) * scale, 0.0f, maxTime);
                if (!m_OnKeyframeTimeChanged(dragKey.Channel, dragKey.CurrentTime, targetTime))
                    continue;
                dragKey.CurrentTime = targetTime;
            }
            m_SelectedKeys.clear();
            m_SelectedKeys.reserve(m_DragKeys.size());
            for (const DragKey& dragKey : m_DragKeys)
                m_SelectedKeys.push_back({dragKey.Channel, dragKey.CurrentTime});
            if (!m_DragKeys.empty())
            {
                m_SelectedChannel = m_DragKeys.front().Channel;
                m_SelectedKeyTime = m_DragKeys.front().CurrentTime;
                m_SelectedKeyIndex = -1;
            }
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
        else if (m_Seeking && m_OnSeekToTime && W > 0.0f)
        {
            float t = std::clamp(timeAtMouse, m_RangeStart, m_RangeEnd);
            m_OnSeekToTime(t);
            e.Stop();
        }
        return;
    }

    if (e.Id == kEventMouseDown && e.Button == 2 && m_OnPan && insideGrid)
    {
        m_Panning = true;
        m_PanLastGlobalX = e.X;
        e.Capture(this);
        e.Stop();
        return;
    }
    if (e.Id == kEventMouseDown && e.Button == 1 && m_OnSeekToTime && insideGrid)
    {
        m_Seeking = true;
        if (m_OnSeekBegin) m_OnSeekBegin();
        float t = std::clamp(timeAtMouse, m_RangeStart, m_RangeEnd);
        m_OnSeekToTime(t);
        e.Capture(this);
        e.Stop();
        return;
    }
    if (e.Id != kEventMouseDown || e.Button != 0)
        return;
    if (W <= 0.0f || H <= 0.0f)
        return;
    if ((e.Mods & Input::kModAlt) != 0 && m_OnPan && insideGrid)
    {
        m_Panning = true;
        m_PanLastGlobalX = e.X;
        e.Capture(this);
        e.Stop();
        return;
    }

    int hitChannel = -1;
    int hitKeyIndex = -1;
    float hitKeyTime = 0.0f;
    if (m_Clip)
        HitTestKeyframes(m_Clip, localX, localY, 0.0f, 0.0f, W, H,
                        m_RangeStart, m_RangeEnd, hitChannel, hitKeyIndex, hitKeyTime,
                        m_RowHeight, m_TrackRows, m_RowTopOffset, m_ScrollOffset);

    if (selectionBounds.Valid &&
        localX >= selectionBounds.HandleLeft && localX <= selectionBounds.HandleRight &&
        localY >= selectionBounds.HandleTop && localY <= selectionBounds.HandleBottom &&
        m_OnKeyframeTimeChanged && m_SelectedKeys.size() >= 2u)
    {
        m_ScalingSelection = true;
        m_ScaleAnchorTime = selectionBounds.MinTime;
        m_ScaleInitialDuration = std::max(0.001f, selectionBounds.MaxTime - selectionBounds.MinTime);
        m_DragKeys.clear();
        m_DragKeys.reserve(m_SelectedKeys.size());
        for (const SelectedKey& selectedKey : m_SelectedKeys)
            m_DragKeys.push_back({selectedKey.Channel, selectedKey.KeyTime, selectedKey.KeyTime});
        if (m_OnEditStarted)
            m_OnEditStarted();
        e.Capture(this);
        e.Stop();
        return;
    }

    const bool inScaleHandleZone =
        selectionBounds.Valid && m_SelectedKeys.size() >= 2u &&
        localX >= selectionBounds.HandleLeft && localX <= selectionBounds.HandleRight &&
        localY >= selectionBounds.HandleTop && localY <= selectionBounds.HandleBottom;

    if (hitChannel >= 0 && hitKeyIndex >= 0 && m_OnKeyframeTimeChanged)
    {
        const bool wasSelected = ContainsSelection(m_SelectedKeys, hitChannel, hitKeyTime);
        if (!shiftHeld)
        {
            if (!wasSelected)
                m_SelectedKeys = {{hitChannel, hitKeyTime}};
        }
        else if (!wasSelected)
            m_SelectedKeys.push_back({hitChannel, hitKeyTime});

        m_SelectedChannel = hitChannel;
        m_SelectedKeyIndex = hitKeyIndex;
        m_SelectedKeyTime = hitKeyTime;
        m_DraggingKey = true;
        m_DragChannel = hitChannel;
        m_DragCurrentTime = hitKeyTime;
        m_DragOffsetTime = timeAtMouse - hitKeyTime;
        m_DragKeys.clear();
        m_DragKeys.reserve(m_SelectedKeys.size());
        for (const SelectedKey& selectedKey : m_SelectedKeys)
            m_DragKeys.push_back({selectedKey.Channel, selectedKey.KeyTime, selectedKey.KeyTime});
        if (m_OnEditStarted)
            m_OnEditStarted();
        e.Capture(this);
    }
    else if (selectionBounds.Valid && m_Clip && m_OnKeyframeTimeChanged && !m_SelectedKeys.empty() &&
             hitChannel < 0 && !inScaleHandleZone &&
             localX >= selectionBounds.Left && localX <= selectionBounds.Right &&
             localY >= selectionBounds.Top && localY <= selectionBounds.Bottom)
    {
        // Drag from inside the selection box (not on a key block): move the whole selection.
        m_DraggingKey = true;
        m_DragChannel = m_SelectedKeys.front().Channel;
        m_DragCurrentTime = selectionBounds.MinTime;
        m_DragOffsetTime = timeAtMouse - selectionBounds.MinTime;
        m_DragKeys.clear();
        m_DragKeys.reserve(m_SelectedKeys.size());
        for (const SelectedKey& selectedKey : m_SelectedKeys)
            m_DragKeys.push_back({selectedKey.Channel, selectedKey.KeyTime, selectedKey.KeyTime});
        if (m_OnEditStarted)
            m_OnEditStarted();
        e.Capture(this);
    }
    else if (insideGrid)
    {
        m_PendingEmptyAction = true;
        m_BoxSelectModifierDown = controlHeld;
        m_EmptyActionStartX = localX;
        m_EmptyActionStartY = localY;
        m_BoxStartX = localX;
        m_BoxStartY = localY;
        m_BoxEndX = localX;
        m_BoxEndY = localY;
        m_BoxSelectionSeed = shiftHeld ? m_SelectedKeys : std::vector<SelectedKey>{};
        e.Capture(this);
        if (hitChannel < 0 && !shiftHeld)
            ClearSelection();
    }

    if (hitChannel >= 0 && hitKeyIndex >= 0)
    {
        MarkDirty(VisualDirty);
        if (m_OnKeyframeSelected)
            m_OnKeyframeSelected(hitChannel, hitKeyIndex, hitKeyTime);
    }
    e.Stop();
}

void DopeSheetView::SetTimeRange(float rangeStart, float rangeEnd)
{
    if (m_RangeStart != rangeStart || m_RangeEnd != rangeEnd)
    {
        m_RangeStart = rangeStart;
        m_RangeEnd = rangeEnd;
        MarkDirty(VisualDirty);
    }
}

void DopeSheetView::SetCurrentTime(float t)
{
    if (m_CurrentTime != t)
    {
        m_CurrentTime = t;
        MarkDirty(VisualDirty);
    }
}

float DopeSheetView::GetSelectedKeyframeTime() const
{
    return m_SelectedKeyTime;
}

void DopeSheetView::SetTrackRows(const std::vector<TrackRow>& rows)
{
    if (m_TrackRows == rows)
        return;

    m_TrackRows = rows;
    MarkDirty(VisualDirty);

    // Prune selected keys to the new selectable scope: a key is reachable if any row
    // for its channel is selectable. When no rows exist (fallback per-channel layout)
    // or no row is non-selectable, leave the selection alone.
    if (m_TrackRows.empty() || m_SelectedKeys.empty())
        return;

    bool anyNonSelectable = false;
    for (const TrackRow& row : m_TrackRows)
    {
        if (!row.Selectable) { anyNonSelectable = true; break; }
    }
    if (!anyNonSelectable)
        return;

    std::unordered_set<int> selectableChannels;
    selectableChannels.reserve(m_TrackRows.size());
    for (const TrackRow& row : m_TrackRows)
    {
        if (row.Selectable && row.ChannelIndex >= 0)
            selectableChannels.insert(row.ChannelIndex);
    }

    const size_t before = m_SelectedKeys.size();
    m_SelectedKeys.erase(std::remove_if(m_SelectedKeys.begin(), m_SelectedKeys.end(),
                                        [&](const SelectedKey& k)
                                        {
                                            return selectableChannels.find(k.Channel) == selectableChannels.end();
                                        }),
                        m_SelectedKeys.end());
    if (m_SelectedKeys.size() == before)
        return;

    m_DragKeys.clear();
    m_ScalingSelection = false;
    m_DraggingKey = false;
    if (m_SelectedKeys.empty())
    {
        m_SelectedChannel = -1;
        m_SelectedKeyIndex = -1;
        m_SelectedKeyTime = -1.0f;
    }
    else if (selectableChannels.find(m_SelectedChannel) == selectableChannels.end())
    {
        m_SelectedChannel = m_SelectedKeys.front().Channel;
        m_SelectedKeyIndex = -1;
        m_SelectedKeyTime = m_SelectedKeys.front().KeyTime;
    }
    if (m_OnSelectionChanged)
        m_OnSelectionChanged();
}

void DopeSheetView::SetScrollOffset(float y)
{
    if (m_ScrollOffset != y)
    {
        m_ScrollOffset = y;
        MarkDirty(VisualDirty);
    }
}

void DopeSheetView::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
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
    const int numChannels = m_Clip ? static_cast<int>(m_Clip->GetChannels().size()) : 4;
    const bool useTrackRows = !m_TrackRows.empty() && m_RowHeight > 0.0f;
    const int numRows = useTrackRows ? static_cast<int>(m_TrackRows.size())
                                     : std::max(1, numChannels);
    const float rowH = useTrackRows ? m_RowHeight
                                    : (m_RowHeight > 0.0f ? m_RowHeight : H / static_cast<float>(numRows));
    // m_RowHeight, m_RowTopOffset, m_ScrollOffset are logical CSS pixels;
    // scale to physical for all drawing positions.
    const float drawRowH      = rowH * cs;
    const float drawTopOffset = (useTrackRows ? m_RowTopOffset : 0.0f) * cs;
    const float drawScrollOff = (useTrackRows ? m_ScrollOffset : 0.0f) * cs;
    const uint32_t lineColor = PackColor(0.35f, 0.35f, 0.35f, 0.10f);

    for (int row = 0; row <= numRows; ++row)
    {
        float ry = y + drawTopOffset + static_cast<float>(row) * drawRowH - drawScrollOff;
        if (ry < y - 1.0f || ry > y + H + 1.0f) continue;
        ctx.Emit(MakeRect(x, ry - 0.5f * cs, W, 1.0f * cs, lineColor));
    }

    constexpr int kNumTimeTicks = 20;
    for (int i = 0; i <= kNumTimeTicks; ++i)
    {
        float t = m_RangeStart + (m_RangeEnd - m_RangeStart) * static_cast<float>(i) / static_cast<float>(kNumTimeTicks);
        float px = x + (t - m_RangeStart) / rangeDuration * W;
        if (px < x || px > x + W)
            continue;
        ctx.Emit(MakeRect(px - 0.5f * cs, y, 1.0f * cs, H, lineColor));
    }

    if (m_Clip)
    {
        const std::vector<AnimChannel>& channels = m_Clip->GetChannels();
        const uint32_t keyColor = PackColor(0.2f, 0.6f, 1.0f, 0.9f);
        const uint32_t selectedColor = PackColor(1.0f, 0.6f, 0.2f, 0.95f);
        const float blockH = std::min(drawRowH * 0.7f, kMaxKeyBlockHeightPx * cs);
        const float blockW = 6.0f * cs;

        // Summary row: occupies the area above the first channel row (m_RowTopOffset).
        if (useTrackRows && m_RowTopOffset > 1.0f)
        {
            const float summaryH = m_RowTopOffset * cs;
            ctx.Emit(MakeRect(x, y, W, summaryH, PackColor(0.10f, 0.10f, 0.10f, 0.35f)));
            ctx.Emit(MakeRect(x, y + summaryH - 1.0f * cs, W, 1.0f * cs, PackColor(0.5f, 0.5f, 0.5f, 0.25f)));

            const float kSummaryTickW = 3.0f * cs;
            const float summaryTickH = std::max(4.0f * cs, summaryH * 0.55f);
            const float summaryTickY = y + (summaryH - summaryTickH) * 0.5f;
            const uint32_t summaryKeyColor = PackColor(0.75f, 0.85f, 1.0f, 0.80f);
            const uint32_t summarySelColor = PackColor(1.0f, 0.65f, 0.20f, 0.95f);

            // Gather unique times (merge times within 0.5px of each other).
            std::vector<std::pair<float, bool>> summaryTimes; // (time, anySelected)
            for (const AnimChannel& ch : channels)
            {
                for (const AnimKeyframe& kf : ch.keys)
                {
                    const float px = (kf.time - m_RangeStart) / rangeDuration * W;
                    if (px < -kSummaryTickW || px > W + kSummaryTickW) continue;
                    bool found = false;
                    for (auto& entry : summaryTimes)
                    {
                        if (std::abs(entry.first - kf.time) < 0.001f)
                        {
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        const bool sel = std::any_of(m_SelectedKeys.begin(), m_SelectedKeys.end(),
                            [&kf](const SelectedKey& k) { return std::abs(k.KeyTime - kf.time) <= kKeySelectionEpsilon; });
                        summaryTimes.push_back({kf.time, sel});
                    }
                }
            }
            for (const auto& [t, sel] : summaryTimes)
            {
                const float kx = x + (t - m_RangeStart) / rangeDuration * W;
                const float bLeft = std::max(x, kx - kSummaryTickW * 0.5f);
                const float bRight = std::min(x + W, kx + kSummaryTickW * 0.5f);
                if (bRight <= bLeft) continue;
                ctx.Emit(MakeRect(bLeft, summaryTickY, bRight - bLeft, summaryTickH,
                                  sel ? summarySelColor : summaryKeyColor));
            }
        }

        if (useTrackRows)
        {
            const uint32_t dimmedKeyColor = PackColor(0.45f, 0.45f, 0.5f, 0.30f);
            for (size_t rowIdx = 0; rowIdx < m_TrackRows.size(); ++rowIdx)
            {
                const int ch = m_TrackRows[rowIdx].ChannelIndex;
                if (ch < 0 || static_cast<size_t>(ch) >= channels.size()) continue;

                if (!m_VisibleChannels.empty() &&
                    std::find(m_VisibleChannels.begin(), m_VisibleChannels.end(), ch) == m_VisibleChannels.end())
                    continue;

                const float rowBaseY = y + drawTopOffset + static_cast<float>(rowIdx) * drawRowH - drawScrollOff;
                if (rowBaseY + drawRowH < y || rowBaseY > y + H) continue;

                const bool rowSelectable = m_TrackRows[rowIdx].Selectable;
                const bool rowSelected = rowSelectable &&
                    ((ch == m_SelectedChannel) || (ch == m_ActiveChannel) ||
                     std::any_of(m_SelectedKeys.begin(), m_SelectedKeys.end(),
                                 [ch](const SelectedKey& k) { return k.Channel == ch; }));
                if (rowSelected)
                    ctx.Emit(MakeRect(x, rowBaseY, W, drawRowH, PackColor(1.0f, 0.55f, 0.10f, 0.10f)));

                const float rowY = rowBaseY + drawRowH * 0.15f;
                const AnimChannel& channel = channels[static_cast<size_t>(ch)];
                for (size_t ki = 0; ki < channel.keys.size(); ++ki)
                {
                    const AnimKeyframe& kf = channel.keys[ki];
                    float kx = x + (kf.time - m_RangeStart) / rangeDuration * W;
                    float startPx = kx - blockW * 0.5f;
                    float blockLeft = std::max(x, startPx);
                    float blockRight = std::min(x + W, startPx + blockW);
                    if (blockRight <= blockLeft) continue;
                    uint32_t color;
                    if (!rowSelectable)
                        color = dimmedKeyColor;
                    else if (ContainsSelection(m_SelectedKeys, ch, kf.time))
                        color = selectedColor;
                    else
                        color = keyColor;
                    ctx.Emit(MakeRect(blockLeft, rowY, blockRight - blockLeft, blockH, color));
                }
            }
        }
        else
        {
            const bool hasFilter = !m_VisibleChannels.empty();
            const size_t filterCount = hasFilter ? m_VisibleChannels.size() : 0;

            for (size_t ch = 0; ch < channels.size(); ++ch)
            {
                if (hasFilter)
                {
                    const auto it = std::find(m_VisibleChannels.begin(), m_VisibleChannels.end(), static_cast<int>(ch));
                    if (it == m_VisibleChannels.end()) continue;
                }
                else if (m_ActiveChannel >= 0 && static_cast<size_t>(m_ActiveChannel) != ch)
                    continue;

                const AnimChannel& channel = channels[ch];

                float rowY;
                if (hasFilter && filterCount > 1)
                {
                    const size_t slot = static_cast<size_t>(std::distance(m_VisibleChannels.begin(),
                        std::find(m_VisibleChannels.begin(), m_VisibleChannels.end(), static_cast<int>(ch))));
                    rowY = y + (static_cast<float>(slot) + 0.15f) * drawRowH;
                    if (static_cast<size_t>(m_ActiveChannel) == ch)
                        ctx.Emit(MakeRect(x, y + static_cast<float>(slot) * drawRowH, W, drawRowH, PackColor(1.0f, 1.0f, 1.0f, 0.04f)));
                }
                else
                {
                    rowY = y + drawRowH * 0.15f;
                    ctx.Emit(MakeRect(x, y, W, drawRowH, PackColor(1.0f, 1.0f, 1.0f, 0.04f)));
                }

                for (size_t ki = 0; ki < channel.keys.size(); ++ki)
                {
                    const AnimKeyframe& kf = channel.keys[ki];
                    float kx = x + (kf.time - m_RangeStart) / rangeDuration * W;
                    float startPx = kx - blockW * 0.5f;
                    float blockLeft = std::max(x, startPx);
                    float blockRight = std::min(x + W, startPx + blockW);
                    if (blockRight <= blockLeft) continue;
                    const bool selected = ContainsSelection(m_SelectedKeys, static_cast<int>(ch), kf.time);
                    ctx.Emit(MakeRect(blockLeft, rowY, blockRight - blockLeft, blockH,
                                      selected ? selectedColor : keyColor));
                }
            }
        }
    }

    const SelectionBounds drawBounds =
        ComputeSelectionBounds(m_Clip, m_SelectedKeys, x, y, W, H, m_RangeStart, m_RangeEnd,
                               m_RowHeight * cs, m_TrackRows, m_RowTopOffset * cs, m_ScrollOffset * cs);
    if (drawBounds.Valid)
    {
        const uint32_t outlineColor = PackColor(1.0f, 0.72f, 0.25f, 0.95f);
        const uint32_t fillColor = PackColor(1.0f, 0.72f, 0.25f, 0.10f);
        ctx.Emit(MakeRect(drawBounds.Left, drawBounds.Top, drawBounds.Right - drawBounds.Left,
                          drawBounds.Bottom - drawBounds.Top, fillColor));
        ctx.Emit(MakeRect(drawBounds.Left, drawBounds.Top, drawBounds.Right - drawBounds.Left, 1.0f * cs, outlineColor));
        ctx.Emit(MakeRect(drawBounds.Left, drawBounds.Bottom - 1.0f * cs, drawBounds.Right - drawBounds.Left, 1.0f * cs, outlineColor));
        ctx.Emit(MakeRect(drawBounds.Left, drawBounds.Top, 1.0f * cs, drawBounds.Bottom - drawBounds.Top, outlineColor));
        ctx.Emit(MakeRect(drawBounds.Right - 1.0f * cs, drawBounds.Top, 1.0f * cs, drawBounds.Bottom - drawBounds.Top, outlineColor));
        if (m_SelectedKeys.size() >= 2u)
        {
            ctx.Emit(MakeRect(drawBounds.HandleLeft,
                              drawBounds.HandleTop,
                              drawBounds.HandleRight - drawBounds.HandleLeft,
                              drawBounds.HandleBottom - drawBounds.HandleTop,
                              outlineColor));
        }
    }

    if (m_BoxSelecting)
    {
        const float boxLeft = x + std::min(m_BoxStartX, m_BoxEndX) * cs;
        const float boxTop = y + std::min(m_BoxStartY, m_BoxEndY) * cs;
        const float boxWidth = std::abs(m_BoxEndX - m_BoxStartX) * cs;
        const float boxHeight = std::abs(m_BoxEndY - m_BoxStartY) * cs;
        const uint32_t boxOutline = PackColor(0.85f, 0.90f, 1.0f, 0.95f);
        const uint32_t boxFill = PackColor(0.35f, 0.55f, 1.0f, 0.14f);
        ctx.Emit(MakeRect(boxLeft, boxTop, boxWidth, boxHeight, boxFill));
        ctx.Emit(MakeRect(boxLeft, boxTop, boxWidth, 1.0f * cs, boxOutline));
        ctx.Emit(MakeRect(boxLeft, boxTop + boxHeight - 1.0f * cs, boxWidth, 1.0f * cs, boxOutline));
        ctx.Emit(MakeRect(boxLeft, boxTop, 1.0f * cs, boxHeight, boxOutline));
        ctx.Emit(MakeRect(boxLeft + boxWidth - 1.0f * cs, boxTop, 1.0f * cs, boxHeight, boxOutline));
    }

    if (W > 0.0f && m_CurrentTime >= m_RangeStart && m_CurrentTime <= m_RangeEnd)
    {
        float playheadPx = x + (m_CurrentTime - m_RangeStart) / rangeDuration * W;
        playheadPx = std::max(x, std::min(x + W, playheadPx));
        ctx.Emit(MakeRect(std::max(x, playheadPx - 1.0f * cs), y, 2.0f * cs, H, PackColor(1.0f, 0.4f, 0.0f, 0.95f)));
    }
}

} // namespace GameEngine

namespace RegisterAnimationWindow
{
static auto s_reg_dopeSheet =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::DopeSheetView>(
        "DopeSheetView",
        []() { return std::make_unique<GameEngine::DopeSheetView>(); })
        .TagAlias("dopesheetview");
}
