#include "Panels/LaneClipEditorView.h"

#include "Input/InputSystem.h"
#include "Platform/SystemMetrics.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIEvents.h"
#include "UI/UiContext.h"
#include "UI/UIPrimitive.h"
#include "Rendering/Geometry/ShapeBuilder.h"
#include "Rendering/Text/FontAtlas.h"

#include <algorithm>
#include <chrono>
#include <string>

namespace GameEngine
{

namespace
{
constexpr float kLaneRowHeightPx = 28.0f;
constexpr float kMaxClipBlockHeightPx = 32.0f;
constexpr float kBoxSelectThresholdSq = 16.0f;
constexpr float kResizeHandleW = 6.0f;
constexpr float kFadeHandleHotHalfW = 5.0f;
constexpr float kFadeHandleHotTopH = 8.0f;
constexpr float kFadeHandleVisualSize = 7.0f;
constexpr float kMarkerHitHalfW = 5.0f;
constexpr float kMarkerLineW = 2.0f;
constexpr float kMarkerCapH = 5.0f;
constexpr float kMarkerCapW = 8.0f;
constexpr float kMarkerDragDownRemoveThreshold = 20.0f;

constexpr uint32_t kClipLabelFallbackArgb = 0xFFF0F0F0u;

void EmitClipNameLeftInsideBlock(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                                 float blockLeft, float blockTop, float blockW, float blockH,
                                 const std::string& name, float cs)
{
    if (!ctx.FontAtlas || blockW < 14.0f || blockH < 12.0f)
        return;
    std::string display = name.empty() ? std::string("Clip") : name;
    const VisualStyle& vs = style.Visual;
    // FontAtlas APIs take physical pixelSize; PrimitiveEmitContext::EmitText
    // takes LOGICAL fontSize and multiplies by contentScale internally.
    const float baseLogicalFontSize = vs.FontSize > 0.0f ? vs.FontSize : 10.0f;
    const float fontSize =
        std::max(8.0f * cs, std::min(baseLogicalFontSize * cs, blockH - 4.0f * cs));
    const float pixelSize = std::max(1.0f, fontSize);
    const float emitLogicalFontSize = (cs > 0.0f) ? (fontSize / cs) : fontSize;
    const float kPadX = 4.0f * cs;
    const float maxTextW = std::max(4.0f, blockW - kPadX * 2.0f);
    while (display.size() > 1u && ctx.FontAtlas->MeasureUtf8(display, pixelSize).width > maxTextW)
        display.pop_back();
    if (ctx.FontAtlas->MeasureUtf8(display, pixelSize).width > maxTextW)
        return;
    const float textX = blockLeft + kPadX;
    const auto lm = ctx.FontAtlas->GetFontLineMetrics(pixelSize);
    const float lineH = std::max(1.0f, lm.height);
    const float textY = blockTop + std::max(0.0f, (blockH - lineH) * 0.5f);
    uint32_t color = vs.Color;
    if ((color & 0xFF000000u) == 0)
        color = kClipLabelFallbackArgb;
    ctx.EmitText(display, textX, textY, emitLogicalFontSize, color, ctx.FontAtlas);
}

bool HitTestLaneClips(const LaneClipModel* model,
                      float localX, float localY,
                      float drawW, float drawH,
                      float rangeStart, float rangeEnd,
                      size_t& outLaneIdx, size_t& outClipIdx, float& outStartTime)
{
    outLaneIdx = 0;
    outClipIdx = 0;
    outStartTime = 0.0f;
    if (!model || model->lanes.empty() || drawW <= 0.0f || drawH <= 0.0f)
        return false;

    if (localX < 0.0f || localX > drawW || localY < 0.0f || localY > drawH)
        return false;

    const float rangeDuration = std::max(0.001f, rangeEnd - rangeStart);
    const float rowH = kLaneRowHeightPx;
    for (size_t laneIndex = 0; laneIndex < model->lanes.size(); ++laneIndex)
    {
        const LaneClipLane& lane = model->lanes[laneIndex];
        const float rowY = (static_cast<float>(laneIndex) + 0.1f) * rowH;
        const float blockH = rowH * 0.8f;
        if (localY < rowY || localY > rowY + blockH)
            continue;

        for (size_t clipIndex = 0; clipIndex < lane.clips.size(); ++clipIndex)
        {
            const LaneClipInstance& instance = lane.clips[clipIndex];
            const float startPx = (instance.startTimeOnLane - rangeStart) / rangeDuration * drawW;
            const float duration = std::max(0.1f, instance.sourceDuration * instance.scale * static_cast<float>(std::max(instance.loopCount, 1)));
            const float blockW = std::max(30.0f, duration / rangeDuration * drawW);
            if (localX >= startPx && localX <= startPx + blockW)
            {
                outLaneIdx = laneIndex;
                outClipIdx = clipIndex;
                outStartTime = instance.startTimeOnLane;
                return true;
            }
        }
    }

    return false;
}
} // namespace

static bool HitTestClipLeftEdgeLane(const LaneClipModel* model,
                                     float localX, float localY, float drawW, float drawH,
                                     float rangeStart, float rangeEnd,
                                     size_t& outLaneIdx, size_t& outClipIdx, float& outStartOnLane)
{
    if (!model || model->lanes.empty() || drawW <= 0.0f || drawH <= 0.0f) return false;
    if (localX < 0.0f || localX > drawW || localY < 0.0f || localY > drawH) return false;
    const float rangeDuration = std::max(0.001f, rangeEnd - rangeStart);
    for (size_t li = 0; li < model->lanes.size(); ++li)
    {
        const float rowY   = (static_cast<float>(li) + 0.1f) * kLaneRowHeightPx;
        const float blockH = kLaneRowHeightPx * 0.8f;
        if (localY < rowY || localY > rowY + blockH) continue;
        for (size_t ci = 0; ci < model->lanes[li].clips.size(); ++ci)
        {
            const LaneClipInstance& inst = model->lanes[li].clips[ci];
            const float duration = std::max(0.1f, inst.sourceDuration * inst.scale * static_cast<float>(std::max(inst.loopCount, 1)));
            const float startPx  = (inst.startTimeOnLane - rangeStart) / rangeDuration * drawW;
            const float blockW   = std::max(30.0f, duration / rangeDuration * drawW);
            if (std::abs(localX - startPx) <= kResizeHandleW && localX <= startPx + blockW)
            {
                outLaneIdx    = li;
                outClipIdx    = ci;
                outStartOnLane = inst.startTimeOnLane;
                return true;
            }
        }
    }
    return false;
}

static bool HitTestClipRightEdgeLane(const LaneClipModel* model,
                                      float localX, float localY, float drawW, float drawH,
                                      float rangeStart, float rangeEnd,
                                      size_t& outLaneIdx, size_t& outClipIdx, float& outEndOnLane)
{
    if (!model || model->lanes.empty() || drawW <= 0.0f || drawH <= 0.0f) return false;
    if (localX < 0.0f || localX > drawW || localY < 0.0f || localY > drawH) return false;
    const float rangeDuration = std::max(0.001f, rangeEnd - rangeStart);
    for (size_t li = 0; li < model->lanes.size(); ++li)
    {
        const float rowY   = (static_cast<float>(li) + 0.1f) * kLaneRowHeightPx;
        const float blockH = kLaneRowHeightPx * 0.8f;
        if (localY < rowY || localY > rowY + blockH) continue;
        for (size_t ci = 0; ci < model->lanes[li].clips.size(); ++ci)
        {
            const LaneClipInstance& inst = model->lanes[li].clips[ci];
            const float duration = std::max(0.1f, inst.sourceDuration * inst.scale * static_cast<float>(std::max(inst.loopCount, 1)));
            const float startPx  = (inst.startTimeOnLane - rangeStart) / rangeDuration * drawW;
            const float blockW   = std::max(30.0f, duration / rangeDuration * drawW);
            const float rightPx  = startPx + blockW;
            if (std::abs(localX - rightPx) <= kResizeHandleW && localX >= startPx)
            {
                outLaneIdx   = li;
                outClipIdx   = ci;
                outEndOnLane = inst.startTimeOnLane + duration;
                return true;
            }
        }
    }
    return false;
}

bool LaneClipEditorView::HitTestClipFadeHandle(float localX, float localY, float W,
                                                size_t& laneIdx, size_t& clipIdx, bool& isLeft) const
{
    if (!m_Model || m_Model->lanes.empty() || W <= 0.0f) return false;
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const float rowH = kLaneRowHeightPx;
    for (size_t li = 0; li < m_Model->lanes.size(); ++li)
    {
        const float rowY = (static_cast<float>(li) + 0.1f) * rowH;
        if (localY < rowY || localY > rowY + kFadeHandleHotTopH) continue;
        const auto& clips = m_Model->lanes[li].clips;
        for (size_t ci = 0; ci < clips.size(); ++ci)
        {
            const LaneClipInstance& inst = clips[ci];
            const float duration = std::max(0.1f, inst.sourceDuration * inst.scale * static_cast<float>(std::max(inst.loopCount, 1)));
            const float startPx  = (inst.startTimeOnLane - m_RangeStart) / rangeDuration * W;
            const float blockW   = std::max(30.0f, duration / rangeDuration * W);
            const float fadeInPx  = std::clamp(inst.fadeInDuration  / rangeDuration * W, 0.0f, blockW);
            const float fadeOutPx = std::clamp(inst.fadeOutDuration / rangeDuration * W, 0.0f, blockW);
            const float leftTipX  = startPx + fadeInPx;
            const float rightTipX = startPx + blockW - fadeOutPx;
            if (std::abs(localX - leftTipX) <= kFadeHandleHotHalfW && localX >= startPx - kFadeHandleHotHalfW)
            {
                laneIdx = li;
                clipIdx = ci;
                isLeft  = true;
                return true;
            }
            if (std::abs(localX - rightTipX) <= kFadeHandleHotHalfW && localX <= startPx + blockW + kFadeHandleHotHalfW)
            {
                laneIdx = li;
                clipIdx = ci;
                isLeft  = false;
                return true;
            }
        }
    }
    return false;
}

LaneClipEditorView::LaneClipEditorView()
{
    AddClass("animationwindow-laneclip");
}

void LaneClipEditorView::SetModel(const LaneClipModel* model)
{
    if (m_Model != model)
    {
        m_Model = model;
        MarkDirty(VisualDirty);
    }
}

void LaneClipEditorView::SetTimeRange(float rangeStart, float rangeEnd)
{
    if (m_RangeStart != rangeStart || m_RangeEnd != rangeEnd)
    {
        m_RangeStart = rangeStart;
        m_RangeEnd = rangeEnd;
        MarkDirty(VisualDirty);
    }
}

void LaneClipEditorView::SetCurrentTime(float t)
{
    if (m_CurrentTime != t)
    {
        m_CurrentTime = t;
        MarkDirty(VisualDirty);
    }
}

void LaneClipEditorView::SetSelectedClip(size_t laneIndex, size_t clipIndex)
{
    m_SelectedLaneIdx = laneIndex;
    m_SelectedClipIdx = clipIndex;
    m_SelectedClips = {{laneIndex, clipIndex}};
    MarkDirty(VisualDirty);
}

void LaneClipEditorView::ClearSelection()
{
    if (m_SelectedLaneIdx != static_cast<size_t>(-1) || !m_SelectedClips.empty())
    {
        m_SelectedLaneIdx = static_cast<size_t>(-1);
        m_SelectedClipIdx = static_cast<size_t>(-1);
        m_SelectedClips.clear();
        MarkDirty(VisualDirty);
    }
}

static std::vector<std::pair<size_t,size_t>> BuildBoxSelectionLane(
    const LaneClipModel* model, float W,
    float rangeStart, float rangeEnd,
    float boxX0, float boxY0, float boxX1, float boxY1,
    const std::vector<std::pair<size_t,size_t>>& seed)
{
    std::vector<std::pair<size_t,size_t>> result = seed;
    if (!model || W <= 0.0f) return result;
    const float rangeDuration = std::max(0.001f, rangeEnd - rangeStart);
    const float bx0 = std::min(boxX0, boxX1);
    const float bx1 = std::max(boxX0, boxX1);
    const float by0 = std::min(boxY0, boxY1);
    const float by1 = std::max(boxY0, boxY1);
    for (size_t li = 0; li < model->lanes.size(); ++li)
    {
        const float rowTop    = (static_cast<float>(li) + 0.1f) * kLaneRowHeightPx;
        const float blockH    = std::min(kLaneRowHeightPx * 0.8f, kMaxClipBlockHeightPx);
        const float rowBottom = rowTop + blockH;
        if (by1 < rowTop || by0 > rowBottom) continue;
        for (size_t ci = 0; ci < model->lanes[li].clips.size(); ++ci)
        {
            const LaneClipInstance& clip = model->lanes[li].clips[ci];
            const float clipDuration = std::max(0.1f, clip.sourceDuration * clip.scale * static_cast<float>(std::max(clip.loopCount, 1)));
            const float clipX0 = (clip.startTimeOnLane - rangeStart) / rangeDuration * W;
            const float clipX1 = clipX0 + std::max(4.0f, clipDuration / rangeDuration * W);
            if (bx1 < clipX0 || bx0 > clipX1) continue;
            const std::pair<size_t,size_t> key{li, ci};
            if (std::find(result.begin(), result.end(), key) == result.end())
                result.push_back(key);
        }
    }
    return result;
}

bool LaneClipEditorView::HitTestLaneMarker(float localX, float localY, float W,
                                            size_t& laneIdx, size_t& markerIdx) const
{
    if (!m_Model || W <= 0.0f) return false;
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const float rowH = kLaneRowHeightPx;
    for (size_t ln = 0; ln < m_Model->lanes.size(); ++ln)
    {
        const float rowTop = static_cast<float>(ln) * rowH;
        if (localY < rowTop || localY > rowTop + rowH) continue;
        for (size_t mi = 0; mi < m_Model->lanes[ln].markers.size(); ++mi)
        {
            const float mx = (m_Model->lanes[ln].markers[mi].time - m_RangeStart) / rangeDuration * W;
            if (std::abs(localX - mx) <= kMarkerHitHalfW)
            {
                laneIdx = ln;
                markerIdx = mi;
                return true;
            }
        }
    }
    return false;
}

bool LaneClipEditorView::HitTestClipMarker(float localX, float localY, float W,
                                            size_t& laneIdx, size_t& clipIdx, size_t& markerIdx) const
{
    if (!m_Model || W <= 0.0f) return false;
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const float rowH = kLaneRowHeightPx;
    const float blockH = rowH * 0.8f;
    for (size_t ln = 0; ln < m_Model->lanes.size(); ++ln)
    {
        const float rowTop = static_cast<float>(ln) * rowH + rowH * 0.1f;
        if (localY < rowTop || localY > rowTop + blockH) continue;
        for (size_t ci = 0; ci < m_Model->lanes[ln].clips.size(); ++ci)
        {
            const LaneClipInstance& inst = m_Model->lanes[ln].clips[ci];
            for (size_t mi = 0; mi < inst.markers.size(); ++mi)
            {
                const float absTime = inst.startTimeOnLane + inst.markers[mi].time;
                const float mx = (absTime - m_RangeStart) / rangeDuration * W;
                if (std::abs(localX - mx) <= kMarkerHitHalfW)
                {
                    laneIdx = ln;
                    clipIdx = ci;
                    markerIdx = mi;
                    return true;
                }
            }
        }
    }
    return false;
}

void LaneClipEditorView::OnEvent(UIEvent& e)
{
    const float W = GetLayoutWidth();
    const float H = GetLayoutHeight();
    const float localX = e.X - GetLayoutX();
    const float clampedLocalX = std::clamp(localX, 0.0f, W);
    const float localY = e.Y - GetLayoutY();
    const float trackY = localY - m_HeaderOffset;
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const bool insideGrid = (W > 0.0f && H > 0.0f &&
                             localX >= 0.0f && localX <= W &&
                             localY >= 0.0f && localY <= H);
    const float timeAtMouse = W > 0.0f ? m_RangeStart + (clampedLocalX / W) * rangeDuration : m_RangeStart;

    if (e.Id == kEventMouseUp)
    {
        if (m_Panning) { m_Panning = false; e.Stop(); return; }
        if (m_MarkerDragKind != MarkerDragKind::None)
        {
            const float draggedDown = localY - m_MarkerDragStartY;
            if (draggedDown >= kMarkerDragDownRemoveThreshold)
            {
                if (m_MarkerDragKind == MarkerDragKind::Lane && m_OnLaneMarkerRemoved)
                    m_OnLaneMarkerRemoved(m_MarkerDragLaneIdx, m_MarkerDragIdx);
                else if (m_MarkerDragKind == MarkerDragKind::Clip && m_OnClipMarkerRemoved)
                    m_OnClipMarkerRemoved(m_MarkerDragLaneIdx, m_MarkerDragClipIdx, m_MarkerDragIdx);
            }
            m_MarkerDragKind = MarkerDragKind::None;
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
        if (m_DraggingFade) { m_DraggingFade = false; e.Stop(); return; }
        if (m_ResizingClip) { m_ResizingClip = false; e.Stop(); return; }
        if (m_DraggingClip) { m_DraggingClip = false; e.Stop(); }
        if (m_Seeking) { m_Seeking = false; e.Stop(); }
        const bool hadBoxSelecting = m_BoxSelecting;
        m_BoxSelecting = false;
        m_PendingEmptyAction = false;
        if (hadBoxSelecting) { MarkDirty(VisualDirty); e.Stop(); }
        return;
    }

    if (e.Id == kEventMouseMove)
    {
        if (m_Panning && m_OnPan && W > 0.0f)
        {
            m_OnPan(((e.X - m_PanLastGlobalX) / W) * rangeDuration);
            m_PanLastGlobalX = e.X;
            e.Stop();
            return;
        }
        if (m_MarkerDragKind != MarkerDragKind::None && W > 0.0f)
        {
            if (localY - m_MarkerDragStartY < kMarkerDragDownRemoveThreshold)
            {
                const float newTime = timeAtMouse - m_MarkerDragTimeOffset;
                if (m_MarkerDragKind == MarkerDragKind::Lane && m_OnLaneMarkerMoved)
                    m_OnLaneMarkerMoved(m_MarkerDragLaneIdx, m_MarkerDragIdx, newTime);
                else if (m_MarkerDragKind == MarkerDragKind::Clip && m_OnClipMarkerMoved)
                {
                    if (m_Model && m_MarkerDragLaneIdx < m_Model->lanes.size() &&
                        m_MarkerDragClipIdx < m_Model->lanes[m_MarkerDragLaneIdx].clips.size())
                    {
                        const float instStart = m_Model->lanes[m_MarkerDragLaneIdx].clips[m_MarkerDragClipIdx].startTimeOnLane;
                        m_OnClipMarkerMoved(m_MarkerDragLaneIdx, m_MarkerDragClipIdx, m_MarkerDragIdx,
                                            std::max(0.0f, newTime - instStart));
                    }
                }
                MarkDirty(VisualDirty);
            }
            e.Stop();
            return;
        }
        if (m_DraggingFade && W > 0.0f)
        {
            if (m_Model && m_FadeDragLaneIdx < m_Model->lanes.size() &&
                m_FadeDragClipIdx < m_Model->lanes[m_FadeDragLaneIdx].clips.size())
            {
                const LaneClipInstance& inst = m_Model->lanes[m_FadeDragLaneIdx].clips[m_FadeDragClipIdx];
                const float duration = std::max(0.0f, inst.sourceDuration * inst.scale * static_cast<float>(std::max(inst.loopCount, 1)));
                if (m_FadeDragLeft && m_OnClipFadeInChanged)
                {
                    const float maxFade = std::max(0.0f, duration - inst.fadeOutDuration);
                    const float newFade = std::clamp(timeAtMouse - inst.startTimeOnLane, 0.0f, maxFade);
                    m_OnClipFadeInChanged(m_FadeDragLaneIdx, m_FadeDragClipIdx, newFade);
                }
                else if (!m_FadeDragLeft && m_OnClipFadeOutChanged)
                {
                    const float clipEnd = inst.startTimeOnLane + duration;
                    const float maxFade = std::max(0.0f, duration - inst.fadeInDuration);
                    const float newFade = std::clamp(clipEnd - timeAtMouse, 0.0f, maxFade);
                    m_OnClipFadeOutChanged(m_FadeDragLaneIdx, m_FadeDragClipIdx, newFade);
                }
            }
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
        if (m_ResizingClip && W > 0.0f)
        {
            if (m_Model && m_ResizeLaneIdx < m_Model->lanes.size() &&
                m_ResizeClipIdx < m_Model->lanes[m_ResizeLaneIdx].clips.size())
            {
                const LaneClipInstance& inst = m_Model->lanes[m_ResizeLaneIdx].clips[m_ResizeClipIdx];
                if (m_ResizeLeft && m_OnClipStartResized)
                {
                    const float oldEnd = inst.startTimeOnLane + std::max(0.1f, inst.sourceDuration * inst.scale * static_cast<float>(std::max(inst.loopCount, 1)));
                    const float newStart = std::clamp(timeAtMouse - m_ResizeDragOffset, 0.0f, oldEnd - 1.0f / 30.0f);
                    const float newDur   = std::max(1.0f / 30.0f, oldEnd - newStart);
                    m_OnClipStartResized(m_ResizeLaneIdx, m_ResizeClipIdx, newStart, newDur);
                }
                else if (!m_ResizeLeft && m_OnClipResized)
                {
                    const float newEndOnLane = timeAtMouse - m_ResizeDragOffset;
                    const float newDuration  = std::max(1.0f / 30.0f, newEndOnLane - inst.startTimeOnLane);
                    m_OnClipResized(m_ResizeLaneIdx, m_ResizeClipIdx, newDuration);
                }
            }
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
        if (m_DraggingClip && m_OnClipMoved && W > 0.0f)
        {
            const float newAnchorStart = std::clamp(timeAtMouse - m_DragOffsetTime, 0.0f, m_RangeEnd);
            for (auto& [key, relOffset] : m_DragClipRelOffsets)
                m_OnClipMoved(key.first, key.second, std::max(0.0f, newAnchorStart + relOffset));
            e.Stop();
            return;
        }
        if (m_Seeking && m_OnSeekToTime && W > 0.0f)
        {
            m_OnSeekToTime(std::clamp(timeAtMouse, m_RangeStart, m_RangeEnd));
            e.Stop();
        }
        else if ((m_BoxSelecting || m_PendingEmptyAction) && W > 0.0f && H > 0.0f)
        {
            const float dx = localX - m_EmptyActionStartX;
            const float dy = localY - m_EmptyActionStartY;
            if (m_BoxSelecting || (dx * dx + dy * dy) >= kBoxSelectThresholdSq)
            {
                m_BoxSelecting = true;
                m_BoxEndX = localX;
                m_BoxEndY = trackY;
                const bool shiftHeld = (e.Mods & Input::kModShift) != 0;
                m_SelectedClips = BuildBoxSelectionLane(m_Model, W, m_RangeStart, m_RangeEnd,
                                                        m_BoxStartX, m_BoxStartY, m_BoxEndX, m_BoxEndY,
                                                        shiftHeld ? m_BoxSelectionSeed : std::vector<std::pair<size_t,size_t>>{});
                if (!m_SelectedClips.empty())
                {
                    m_SelectedLaneIdx = m_SelectedClips.front().first;
                    m_SelectedClipIdx = m_SelectedClips.front().second;
                }
                else
                {
                    m_SelectedLaneIdx = static_cast<size_t>(-1);
                    m_SelectedClipIdx = static_cast<size_t>(-1);
                }
                MarkDirty(VisualDirty);
                e.Stop();
            }
        }
        // Idle hover: fade-handle highlight first (top band), then edge resize.
        if (!m_DraggingClip && !m_ResizingClip && !m_DraggingFade && !m_Panning && !m_Seeking && !m_BoxSelecting)
        {
            size_t fhLi = 0, fhCi = 0;
            bool fhLeft = false;
            const bool overFade = insideGrid &&
                HitTestClipFadeHandle(localX, trackY, W, fhLi, fhCi, fhLeft);
            if (overFade != m_HoveringFade ||
                (overFade && (fhLi != m_HoverFadeLaneIdx || fhCi != m_HoverFadeClipIdx || fhLeft != m_HoverFadeLeft)))
            {
                m_HoveringFade      = overFade;
                m_HoverFadeLeft     = fhLeft;
                m_HoverFadeLaneIdx  = fhLi;
                m_HoverFadeClipIdx  = fhCi;
                MarkDirty(VisualDirty);
            }
            size_t hli = 0, hci = 0;
            float dummy = 0.0f;
            const bool overLeft = !overFade && insideGrid &&
                HitTestClipLeftEdgeLane(m_Model, localX, trackY, W, H, m_RangeStart, m_RangeEnd, hli, hci, dummy);
            const bool overRight = !overFade && !overLeft && insideGrid &&
                HitTestClipRightEdgeLane(m_Model, localX, trackY, W, H, m_RangeStart, m_RangeEnd, hli, hci, dummy);
            const bool over = overLeft || overRight;
            if (over != m_HoveringResize ||
                (over && (hli != m_HoverResizeLaneIdx || hci != m_HoverResizeClipIdx || overLeft != m_HoverResizeLeft)))
            {
                m_HoveringResize     = over;
                m_HoverResizeLeft    = overLeft;
                m_HoverResizeLaneIdx = hli;
                m_HoverResizeClipIdx = hci;
                MarkDirty(VisualDirty);
            }
            // Marker hover: highlight
            if (insideGrid && m_Model)
            {
                size_t hvLane = 0, hvClip = 0, hvIdx = 0;
                bool found = false, foundIsClip = false;
                if (HitTestLaneMarker(localX, trackY, W, hvLane, hvIdx) &&
                    hvLane < m_Model->lanes.size() &&
                    hvIdx < m_Model->lanes[hvLane].markers.size())
                {
                    found = true; foundIsClip = false;
                }
                else if (HitTestClipMarker(localX, trackY, W, hvLane, hvClip, hvIdx) &&
                         hvLane < m_Model->lanes.size() &&
                         hvClip < m_Model->lanes[hvLane].clips.size() &&
                         hvIdx < m_Model->lanes[hvLane].clips[hvClip].markers.size())
                {
                    found = true; foundIsClip = true;
                }
                const bool hadMarker = m_HoveringMarker2;
                const bool stateChanged = found != hadMarker ||
                    (found && (foundIsClip != m_HoverMarker2IsClip ||
                               hvLane != m_HoverMarker2LaneIdx ||
                               hvClip != m_HoverMarker2ClipIdx  ||
                               hvIdx  != m_HoverMarker2Idx));
                if (stateChanged)
                {
                    m_HoveringMarker2      = found;
                    m_HoverMarker2IsClip   = foundIsClip;
                    m_HoverMarker2LaneIdx  = hvLane;
                    m_HoverMarker2ClipIdx  = hvClip;
                    m_HoverMarker2Idx      = hvIdx;
                    MarkDirty(VisualDirty);
                }
            }
        }
        return;
    }

    if (e.Id == kEventMouseLeave)
    {
        bool dirty = false;
        if (m_HoveringResize) { m_HoveringResize = false; dirty = true; }
        if (m_HoveringFade)   { m_HoveringFade   = false; dirty = true; }
        if (dirty) MarkDirty(VisualDirty);
        if (m_HoveringMarker2) { m_HoveringMarker2 = false; MarkDirty(VisualDirty); }
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
        m_OnSeekToTime(std::clamp(timeAtMouse, m_RangeStart, m_RangeEnd));
        e.Capture(this);
        e.Stop();
        return;
    }
    if (e.Id != kEventMouseDown || e.Button != 0 || W <= 0.0f || H <= 0.0f)
        return;

    if ((e.Mods & Input::kModAlt) != 0 && m_OnPan && insideGrid)
    {
        m_Panning = true;
        m_PanLastGlobalX = e.X;
        e.Capture(this);
        e.Stop();
        return;
    }

    // Double-click detection for adding markers.
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    const bool isDouble = (now - m_LastClickTime) < GameEngine::Platform::GetDoubleClickInterval() &&
                          std::abs(localX - m_LastClickX) < 10.0f &&
                          std::abs(localY - m_LastClickY) < 10.0f;
    m_LastClickTime = now;
    m_LastClickX = localX;
    m_LastClickY = localY;

    // Marker hit-test takes priority.
    {
        size_t mLane = 0, mClip = 0, mIdx = 0;
        if (HitTestLaneMarker(localX, trackY, W, mLane, mIdx))
        {
            m_MarkerDragKind = MarkerDragKind::Lane;
            m_MarkerDragLaneIdx = mLane;
            m_MarkerDragIdx = mIdx;
            m_MarkerDragStartY = localY;
            if (m_Model && mLane < m_Model->lanes.size() && mIdx < m_Model->lanes[mLane].markers.size())
                m_MarkerDragTimeOffset = timeAtMouse - m_Model->lanes[mLane].markers[mIdx].time;
            e.Capture(this);
            e.Stop();
            return;
        }
        if (HitTestClipMarker(localX, trackY, W, mLane, mClip, mIdx))
        {
            m_MarkerDragKind = MarkerDragKind::Clip;
            m_MarkerDragLaneIdx = mLane;
            m_MarkerDragClipIdx = mClip;
            m_MarkerDragIdx = mIdx;
            m_MarkerDragStartY = localY;
            if (m_Model && mLane < m_Model->lanes.size() && mClip < m_Model->lanes[mLane].clips.size())
            {
                const float instStart = m_Model->lanes[mLane].clips[mClip].startTimeOnLane;
                const float absTime   = instStart + m_Model->lanes[mLane].clips[mClip].markers[mIdx].time;
                m_MarkerDragTimeOffset = timeAtMouse - absTime;
            }
            e.Capture(this);
            e.Stop();
            return;
        }
    }

    // Fade-handle hit-test: takes priority over edge resize; lives in the top band of the clip block.
    {
        size_t fhLi = 0, fhCi = 0;
        bool fhLeft = false;
        if ((m_OnClipFadeInChanged || m_OnClipFadeOutChanged) &&
            HitTestClipFadeHandle(localX, trackY, W, fhLi, fhCi, fhLeft) &&
            ((fhLeft && m_OnClipFadeInChanged) || (!fhLeft && m_OnClipFadeOutChanged)))
        {
            const std::pair<size_t,size_t> key{fhLi, fhCi};
            if (std::find(m_SelectedClips.begin(), m_SelectedClips.end(), key) == m_SelectedClips.end())
                SetSelectedClip(fhLi, fhCi);
            m_DraggingFade   = true;
            m_FadeDragLeft   = fhLeft;
            m_FadeDragLaneIdx = fhLi;
            m_FadeDragClipIdx = fhCi;
            e.Capture(this);
            e.Stop();
            return;
        }
    }

    // Edge resize takes priority over body drag; right edge checked first.
    {
        size_t resLane = 0, resClip = 0;
        float resTime = 0.0f;
        bool hitRight = m_OnClipResized &&
            HitTestClipRightEdgeLane(m_Model, localX, trackY, W, H, m_RangeStart, m_RangeEnd, resLane, resClip, resTime);
        bool hitLeft = !hitRight && m_OnClipStartResized &&
            HitTestClipLeftEdgeLane(m_Model, localX, trackY, W, H, m_RangeStart, m_RangeEnd, resLane, resClip, resTime);
        if (hitRight || hitLeft)
        {
            const std::pair<size_t,size_t> key{resLane, resClip};
            if (std::find(m_SelectedClips.begin(), m_SelectedClips.end(), key) == m_SelectedClips.end())
                SetSelectedClip(resLane, resClip);
            m_ResizingClip     = true;
            m_ResizeLeft       = hitLeft;
            m_ResizeLaneIdx    = resLane;
            m_ResizeClipIdx    = resClip;
            m_ResizeDragOffset = timeAtMouse - resTime;
            e.Capture(this);
            e.Stop();
            return;
        }
    }

    size_t hitLane = 0, hitClip = 0;
    float hitStart = 0.0f;
    if (HitTestLaneClips(m_Model, localX, trackY, W, H, m_RangeStart, m_RangeEnd, hitLane, hitClip, hitStart))
    {
        if (isDouble && insideGrid)
        {
            if (m_OnClipMarkerAdded)
                m_OnClipMarkerAdded(hitLane, hitClip, std::max(0.0f, timeAtMouse - hitStart));
            e.Stop();
            return;
        }
        {
            const std::pair<size_t,size_t> clickKey{hitLane, hitClip};
            const bool alreadySelected = std::find(m_SelectedClips.begin(), m_SelectedClips.end(), clickKey) != m_SelectedClips.end();
            if (!alreadySelected)
            {
                SetSelectedClip(hitLane, hitClip);
                if (m_OnClipSelected)
                    m_OnClipSelected(hitLane, hitClip);
            }
        }
        if (m_OnClipMoved)
        {
            m_DraggingClip = true;
            m_DragLaneIdx = hitLane;
            m_DragClipIdx = hitClip;
            m_DragOffsetTime = timeAtMouse - hitStart;
            m_DragClipRelOffsets.clear();
            for (auto& [li, ci] : m_SelectedClips)
            {
                if (m_Model && li < m_Model->lanes.size() && ci < m_Model->lanes[li].clips.size())
                    m_DragClipRelOffsets.push_back({{li, ci}, m_Model->lanes[li].clips[ci].startTimeOnLane - hitStart});
            }
            e.Capture(this);
            e.Stop();
            return;
        }
    }
    else if (isDouble && insideGrid)
    {
        // Double-click on empty lane area → add lane marker.
        const size_t clickedLane = static_cast<size_t>(trackY / kLaneRowHeightPx);
        if (m_Model && clickedLane < m_Model->lanes.size() && m_OnLaneMarkerAdded)
            m_OnLaneMarkerAdded(clickedLane, timeAtMouse);
        e.Stop();
        return;
    }
    else if (insideGrid)
    {
        const bool shiftHeld = (e.Mods & Input::kModShift) != 0;
        m_PendingEmptyAction = true;
        m_EmptyActionStartX = localX;
        m_EmptyActionStartY = trackY;
        m_BoxStartX = localX;
        m_BoxStartY = trackY;
        m_BoxEndX   = localX;
        m_BoxEndY   = trackY;
        m_BoxSelectionSeed = shiftHeld ? m_SelectedClips : std::vector<std::pair<size_t,size_t>>{};
        e.Capture(this);
        if (!shiftHeld)
            ClearSelection();
    }
    e.Stop();
}

void LaneClipEditorView::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
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
    const float drawHeaderOffset = m_HeaderOffset * cs;
    const float ty = y + drawHeaderOffset;

    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const int numLanes = m_Model && !m_Model->lanes.empty()
                             ? static_cast<int>(m_Model->lanes.size())
                             : 0;
    const float rowH = kLaneRowHeightPx * cs;
    const uint32_t lineColor = PackColor(0.35f, 0.35f, 0.35f, 0.10f);

    if (numLanes > 0)
    {
        for (int row = 0; row <= numLanes; ++row)
        {
            float ry = ty + row * rowH;
            ctx.Emit(MakeRect(x, ry - 0.5f * cs, W, 1.0f * cs, lineColor));
        }

        for (int i = 0; i <= 20; ++i)
        {
            float t = m_RangeStart + (m_RangeEnd - m_RangeStart) * static_cast<float>(i) / 20.0f;
            float px = x + (t - m_RangeStart) / rangeDuration * W;
            if (px < x || px > x + W)
                continue;
            ctx.Emit(MakeRect(px - 0.5f * cs, y, 1.0f * cs, H, lineColor));
        }
    }

    const uint32_t blockColor = PackColor(0.5f, 0.35f, 0.7f, 0.9f);
    const uint32_t selectedBlockColor = PackColor(0.95f, 0.7f, 0.25f, 0.95f);
    if (m_Model && !m_Model->lanes.empty())
    {
        for (size_t ln = 0; ln < m_Model->lanes.size(); ++ln)
        {
            const LaneClipLane& lane = m_Model->lanes[ln];
            // lane.color is engine-ARGB; primitives use RGBA layout so route through PackFromARGB.
            if (lane.color != 0u)
                ctx.Emit(MakeRect(x, ty + static_cast<float>(ln) * rowH, W, rowH, PackFromARGB(lane.color)));
            if (m_SelectedLaneIdx == ln)
                ctx.Emit(MakeRect(x, ty + static_cast<float>(ln) * rowH, W, rowH, PackColor(1.0f, 0.55f, 0.10f, 0.10f)));
            float rowY = ty + (static_cast<float>(ln) + 0.1f) * rowH;
            float blockH = std::min(rowH * 0.8f, kMaxClipBlockHeightPx * cs);

            for (size_t clipIndex = 0; clipIndex < lane.clips.size(); ++clipIndex)
            {
                const LaneClipInstance& inst = lane.clips[clipIndex];
                float startPx = x + (inst.startTimeOnLane - m_RangeStart) / rangeDuration * W;
                float duration = std::max(0.1f, inst.sourceDuration * inst.scale * static_cast<float>(std::max(inst.loopCount, 1)));
                float blockW = std::max(30.0f, duration / rangeDuration * W);
                float blockLeft = std::max(x, startPx);
                float blockRight = std::min(x + W, startPx + blockW);
                if (blockRight > blockLeft)
                {
                    const float bl = blockLeft;
                    const float bw = blockRight - blockLeft;
                    const std::pair<size_t,size_t> clipKey{ln, clipIndex};
                    const bool selected = std::find(m_SelectedClips.begin(), m_SelectedClips.end(), clipKey) != m_SelectedClips.end();
                    ctx.Emit(MakeRect(bl, rowY, bw, blockH,
                                      selected
                                          ? selectedBlockColor
                                          : (inst.muted ? PackColor(0.35f, 0.3f, 0.35f, 0.7f) : blockColor)));
                    EmitClipNameLeftInsideBlock(ctx, style, bl, rowY, bw, blockH, inst.name, cs);
                    // Resize handle stripes at left and right edges.
                    const float handleW    = std::min(5.0f * cs, bw * 0.35f);
                    const bool rightHovered = m_HoveringResize && !m_HoverResizeLeft && m_HoverResizeLaneIdx == ln && m_HoverResizeClipIdx == clipIndex;
                    const bool leftHovered  = m_HoveringResize &&  m_HoverResizeLeft && m_HoverResizeLaneIdx == ln && m_HoverResizeClipIdx == clipIndex;
                    ctx.Emit(MakeRect(bl,                rowY, handleW, blockH,
                                      leftHovered  ? PackColor(1.0f, 1.0f, 1.0f, 0.65f) : PackColor(0.0f, 0.0f, 0.0f, 0.28f)));
                    ctx.Emit(MakeRect(bl + bw - handleW, rowY, handleW, blockH,
                                      rightHovered ? PackColor(1.0f, 1.0f, 1.0f, 0.65f) : PackColor(0.0f, 0.0f, 0.0f, 0.28f)));

                    // Fade ramps + handle markers.
                    {
                        const float fullBlockLeft  = startPx;
                        const float fullBlockRight = startPx + blockW;
                        const float fullBlockW     = std::max(0.0f, fullBlockRight - fullBlockLeft);
                        const float fadeInPx  = std::clamp(inst.fadeInDuration  / rangeDuration * W, 0.0f, fullBlockW);
                        const float fadeOutPx = std::clamp(inst.fadeOutDuration / rangeDuration * W, 0.0f, fullBlockW);
                        const uint32_t fadeFill   = PackColor(0.0f, 0.0f, 0.0f, 0.45f);
                        const uint32_t handleIdle = PackColor(1.0f, 0.85f, 0.30f, 0.95f);
                        const uint32_t handleHot  = PackColor(1.0f, 1.0f, 1.0f, 1.0f);
                        if (fadeInPx > 0.5f)
                        {
                            const float x0 = fullBlockLeft;
                            const float x1 = fullBlockLeft + fadeInPx;
                            const float yTop = rowY;
                            const float yBot = rowY + blockH;
                            if (std::min(x1, blockRight) > std::max(x0, blockLeft))
                                ctx.Emit(MakeTriangle(std::max(x0, blockLeft), yTop,
                                                       std::min(x1, blockRight), yTop,
                                                       std::max(x0, blockLeft), yBot,
                                                       fadeFill));
                        }
                        if (fadeOutPx > 0.5f)
                        {
                            const float x0 = fullBlockRight - fadeOutPx;
                            const float x1 = fullBlockRight;
                            const float yTop = rowY;
                            const float yBot = rowY + blockH;
                            if (std::min(x1, blockRight) > std::max(x0, blockLeft))
                                ctx.Emit(MakeTriangle(std::max(x0, blockLeft), yTop,
                                                       std::min(x1, blockRight), yTop,
                                                       std::min(x1, blockRight), yBot,
                                                       fadeFill));
                        }
                        const float hs = kFadeHandleVisualSize * cs;
                        const float leftTipX  = fullBlockLeft + fadeInPx;
                        const float rightTipX = fullBlockRight - fadeOutPx;
                        const bool leftFadeHover  = m_HoveringFade &&  m_HoverFadeLeft && m_HoverFadeLaneIdx == ln && m_HoverFadeClipIdx == clipIndex;
                        const bool rightFadeHover = m_HoveringFade && !m_HoverFadeLeft && m_HoverFadeLaneIdx == ln && m_HoverFadeClipIdx == clipIndex;
                        if (leftTipX >= blockLeft - hs && leftTipX <= blockRight + hs)
                        {
                            ctx.Emit(MakeTriangle(leftTipX - hs * 0.5f, rowY,
                                                   leftTipX + hs * 0.5f, rowY,
                                                   leftTipX,            rowY + hs,
                                                   leftFadeHover ? handleHot : handleIdle));
                        }
                        if (rightTipX >= blockLeft - hs && rightTipX <= blockRight + hs)
                        {
                            ctx.Emit(MakeTriangle(rightTipX - hs * 0.5f, rowY,
                                                   rightTipX + hs * 0.5f, rowY,
                                                   rightTipX,            rowY + hs,
                                                   rightFadeHover ? handleHot : handleIdle));
                        }
                    }

                    // Clip instance markers (relative to instance start).
                    for (size_t mi = 0; mi < inst.markers.size(); ++mi)
                    {
                        const LaneMarker& m = inst.markers[mi];
                        const float absT = inst.startTimeOnLane + m.time;
                        const float mx = x + (absT - m_RangeStart) / rangeDuration * W;
                        if (mx < blockLeft || mx > blockRight) continue;
                        const bool hov = m_HoveringMarker2 && m_HoverMarker2IsClip &&
                                         m_HoverMarker2LaneIdx == ln && m_HoverMarker2ClipIdx == clipIndex && m_HoverMarker2Idx == mi;
                        ctx.Emit(MakeRect(mx - kMarkerLineW * cs * 0.5f, rowY, kMarkerLineW * cs, blockH,
                                          hov ? PackColor(1.0f, 1.0f, 1.0f, 1.0f) : PackColor(1.0f, 1.0f, 0.6f, 0.90f)));
                        ctx.Emit(MakeRect(mx - kMarkerCapW * cs * 0.5f, rowY, kMarkerCapW * cs, kMarkerCapH * cs,
                                          hov ? PackColor(1.0f, 1.0f, 1.0f, 1.0f) : PackColor(1.0f, 0.90f, 0.30f, 1.0f)));
                        if (!m.name.empty())
                            EmitClipNameLeftInsideBlock(ctx, style, mx + kMarkerCapW * cs * 0.6f, rowY, 60.0f * cs, kMarkerCapH * cs, m.name, cs);
                    }
                }
            }

            // Lane markers (absolute time, drawn full row height).
            const float laneTop = ty + static_cast<float>(ln) * rowH;
            for (size_t mi = 0; mi < lane.markers.size(); ++mi)
            {
                const LaneMarker& m = lane.markers[mi];
                const float mx = x + (m.time - m_RangeStart) / rangeDuration * W;
                if (mx < x || mx > x + W) continue;
                const bool hov = m_HoveringMarker2 && !m_HoverMarker2IsClip &&
                                 m_HoverMarker2LaneIdx == ln && m_HoverMarker2Idx == mi;
                ctx.Emit(MakeRect(mx - kMarkerLineW * cs * 0.5f, laneTop, kMarkerLineW * cs, rowH,
                                  hov ? PackColor(1.0f, 1.0f, 0.4f, 1.0f) : PackColor(0.95f, 0.75f, 0.15f, 0.85f)));
                ctx.Emit(MakeRect(mx - kMarkerCapW * cs * 0.5f, laneTop, kMarkerCapW * cs, kMarkerCapH * cs,
                                  hov ? PackColor(1.0f, 1.0f, 0.4f, 1.0f) : PackColor(1.0f, 0.85f, 0.20f, 1.0f)));
                if (!m.name.empty())
                    EmitClipNameLeftInsideBlock(ctx, style, mx + kMarkerCapW * cs * 0.6f, laneTop, 60.0f * cs, kMarkerCapH * cs, m.name, cs);
            }
        }
    }

    if (W > 0.0f && m_CurrentTime >= m_RangeStart && m_CurrentTime <= m_RangeEnd)
    {
        float playheadPx = x + (m_CurrentTime - m_RangeStart) / rangeDuration * W;
        playheadPx = std::max(x, std::min(x + W, playheadPx));
        ctx.Emit(MakeRect(std::max(x, playheadPx - 1.0f * cs), y, 2.0f * cs, H, PackColor(1.0f, 0.4f, 0.0f, 0.95f)));
    }

    if (m_BoxSelecting)
    {
        const float bx0 = x + std::min(m_BoxStartX, m_BoxEndX) * cs;
        const float bx1 = x + std::max(m_BoxStartX, m_BoxEndX) * cs;
        const float by0 = ty + std::min(m_BoxStartY, m_BoxEndY) * cs;
        const float by1 = ty + std::max(m_BoxStartY, m_BoxEndY) * cs;
        const float bw  = bx1 - bx0;
        const float bh  = by1 - by0;
        ctx.Emit(MakeRect(bx0, by0, bw, bh, PackColor(0.25f, 0.55f, 1.0f, 0.12f)));
        ctx.Emit(MakeRect(bx0,              by0,              bw,          1.0f * cs, PackColor(0.4f, 0.7f, 1.0f, 0.8f)));
        ctx.Emit(MakeRect(bx0,              by1 - 1.0f * cs, bw,          1.0f * cs, PackColor(0.4f, 0.7f, 1.0f, 0.8f)));
        ctx.Emit(MakeRect(bx0,              by0,              1.0f * cs,   bh,        PackColor(0.4f, 0.7f, 1.0f, 0.8f)));
        ctx.Emit(MakeRect(bx1 - 1.0f * cs, by0,              1.0f * cs,   bh,        PackColor(0.4f, 0.7f, 1.0f, 0.8f)));
    }
}

} // namespace GameEngine

namespace RegisterAnimationWindow
{
static auto s_reg_laneClipEditor =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::LaneClipEditorView>(
        "LaneClipEditorView",
        []() { return std::make_unique<GameEngine::LaneClipEditorView>(); })
        .TagAlias("laneclipeditorview");
}
