#include "Panels/TimeCompositeView.h"

#include "Editor/DragDropPayloads.h"
#include "Input/InputSystem.h"
#include "Platform/SystemMetrics.h"
#include "UI/Interaction/Payload.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/ResolvedStyle.h"
#include "UI/UiContext.h"
#include "UI/UIManager.h"
#include "UI/UIEvents.h"
#include "UI/UIPrimitive.h"
#include "Rendering/Geometry/ShapeBuilder.h"
#include "Rendering/Text/FontAtlas.h"
#include <AssetCore/AssetTypes.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace GameEngine
{

namespace
{
const float kMaxClipBlockHeightPx = 32.0f;
const float kTrackRowHeightPx = 28.0f;
const float kTrackRowGapPx = 2.0f;
constexpr float kBoxSelectThresholdSq = 16.0f;
constexpr float kDragStartThresholdSq = 16.0f;
constexpr float kClipHorizontalDragStartThreshold = 2.0f;
constexpr float kClipVerticalDragStartThreshold = 10.0f;
constexpr float kResizeHandleW = 10.0f;
constexpr float kFadeHandleHotHalfW = 5.0f; // x-axis tolerance around fade tip for hit-test
constexpr float kFadeHandleHotTopH = 8.0f;  // top-of-block band reserved for fade-handle hit-test
constexpr float kFadeHandleVisualSize = 7.0f; // edge length of the small triangle marker drawn at the fade tip
constexpr float kMarkerHitHalfW = 5.0f;
constexpr float kTrackKeyHitHalfLogicalPx = 7.0f;
constexpr float kMarkerLineW = 2.0f;
constexpr float kMarkerCapH = 5.0f;
constexpr float kMarkerCapW = 8.0f;
constexpr float kMarkerDragDownRemoveThreshold = 20.0f;
constexpr float kTrackKeyEdgeInsetPx = 8.0f;
const uint32_t kClipBlockColor = UI::PackColor(0.3f, 0.5f, 0.8f, 0.9f);
const uint32_t kSelectedClipBlockColor = UI::PackColor(0.9f, 0.65f, 0.25f, 0.95f);
const uint32_t kValueKeyColor = UI::PackColor(0.35f, 0.85f, 0.60f, 0.95f);
const uint32_t kMethodKeyColor = UI::PackColor(0.95f, 0.55f, 0.35f, 0.95f);
const uint32_t kAudioKeyColor = UI::PackColor(0.25f, 0.72f, 1.0f, 0.95f);
const uint32_t kAnimationKeyColor = UI::PackColor(0.78f, 0.58f, 1.0f, 0.95f);

constexpr uint32_t kClipLabelFallbackArgb = 0xFFF0F0F0u;
const uint32_t kTrackKeyLabelBrightColor = UI::PackColor(1.0f, 1.0f, 1.0f, 1.0f);
const uint32_t kTrackKeyLabelDimColor = UI::PackColor(1.0f, 1.0f, 1.0f, 0.45f);

void EmitTrackKeyLabel(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                       float blockLeft, float blockTop, float blockW, float blockH,
                       const std::string& name, float cs, bool emphasized)
{
    if (!ctx.FontAtlas || blockW < 14.0f || blockH < 12.0f)
        return;
    std::string display = name;
    const VisualStyle& vs = style.Visual;
    const float baseLogicalFontSize = vs.FontSize > 0.0f ? vs.FontSize : 10.0f;
    const float fontSize =
        std::max(8.0f * cs, std::min(baseLogicalFontSize * cs, blockH - 4.0f * cs));
    const float pixelSize = std::max(1.0f, fontSize);
    const float emitLogicalFontSize = (cs > 0.0f) ? (fontSize / cs) : fontSize;
    const float kPadX = 4.0f * cs;
    const float maxTextW = std::max(4.0f, blockW - kPadX * 2.0f);
    while (display.size() > 1u && ctx.FontAtlas->MeasureUtf8(display, pixelSize).width > maxTextW)
        display.pop_back();
    if (display.empty() || ctx.FontAtlas->MeasureUtf8(display, pixelSize).width > maxTextW)
        return;
    const float textX = blockLeft + kPadX;
    const auto lm = ctx.FontAtlas->GetFontLineMetrics(pixelSize);
    const float lineH = std::max(1.0f, lm.height);
    const float textY = blockTop + std::max(0.0f, (blockH - lineH) * 0.5f);
    const uint32_t color = emphasized ? kTrackKeyLabelBrightColor : kTrackKeyLabelDimColor;
    ctx.EmitText(display, textX, textY, emitLogicalFontSize, color, ctx.FontAtlas);
}

void EmitClipNameLeftInsideBlock(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                                 float blockLeft, float blockTop, float blockW, float blockH,
                                 const std::string& name, float cs, bool hovered = false)
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
    if (hovered)
        ctx.Emit(UI::MakeRect(blockLeft, blockTop + 1.0f * cs, blockW, std::max(1.0f, blockH - 2.0f * cs),
                              UI::PackColor(1.0f, 1.0f, 1.0f, 0.11f)));
    uint32_t color = vs.Color;
    if ((color & 0xFF000000u) == 0)
        color = kClipLabelFallbackArgb;
    if (hovered)
        color = UI::PackColor(1.0f, 1.0f, 1.0f, 1.0f);
    ctx.EmitText(display, textX, textY, emitLogicalFontSize, color, ctx.FontAtlas);
}

void EmitTrackKeyDiamond(UI::PrimitiveEmitContext& ctx, float kx, float cy, float s,
                         uint32_t fill, bool hovered)
{
    if (hovered)
    {
        const float haloS = s + 3.0f;
        ctx.Emit(UI::MakeTriangle(kx, cy - haloS, kx + haloS, cy, kx, cy + haloS,
                                  UI::PackColor(1.0f, 1.0f, 1.0f, 0.22f)));
        ctx.Emit(UI::MakeTriangle(kx, cy - haloS, kx - haloS, cy, kx, cy + haloS,
                                  UI::PackColor(1.0f, 1.0f, 1.0f, 0.22f)));
    }
    ctx.Emit(UI::MakeTriangle(kx, cy - s, kx + s, cy, kx, cy + s, fill));
    ctx.Emit(UI::MakeTriangle(kx, cy - s, kx - s, cy, kx, cy + s, fill));
    if (hovered)
    {
        const float outlineS = s + 1.0f;
        ctx.Emit(UI::MakeTriangle(kx, cy - outlineS, kx + outlineS, cy, kx, cy + outlineS,
                                  UI::PackColor(1.0f, 1.0f, 1.0f, 0.18f)));
        ctx.Emit(UI::MakeTriangle(kx, cy - outlineS, kx - outlineS, cy, kx, cy + outlineS,
                                  UI::PackColor(1.0f, 1.0f, 1.0f, 0.18f)));
        ctx.Emit(UI::MakeTriangle(kx, cy - s, kx + s, cy, kx, cy + s, fill));
        ctx.Emit(UI::MakeTriangle(kx, cy - s, kx - s, cy, kx, cy + s, fill));
    }
}

void EmitTrackKeyConnectorSegment(UI::PrimitiveEmitContext& ctx,
                                  float fromTime, float toTime,
                                  float rangeStart, float rangeDuration,
                                  float x, float w, float centerY, float cs,
                                  uint32_t color)
{
    if (rangeDuration <= 0.0f || w <= 0.0f)
        return;
    const float thickness = std::max(1.0f, 2.0f * cs);
    const float lineY = centerY - thickness * 0.5f;
    const float viewRight = x + w;
    float left = x + (fromTime - rangeStart) / rangeDuration * w;
    float right = x + (toTime - rangeStart) / rangeDuration * w;
    if (right < left)
        std::swap(left, right);
    if (right <= x || left >= viewRight || right <= left)
        return;
    left = std::max(left + 1.0f * cs, x);
    right = std::min(right, viewRight);
    if (right > left)
        ctx.Emit(UI::MakeRect(left, lineY, right - left, thickness, color));
}

bool NearlySame(float a, float b)
{
    return std::fabs(a - b) <= 0.0001f;
}

bool SameCompositeValueKey(const CompositeValueKey& a, const CompositeValueKey& b)
{
    if (a.componentCount != b.componentCount)
        return false;
    const uint8_t count = std::min<uint8_t>(a.componentCount, 4u);
    for (uint8_t i = 0; i < count; ++i)
    {
        if (!NearlySame(a.value[i], b.value[i]))
            return false;
    }
    return true;
}

bool SameCompositeAudioKey(const CompositeAudioKey& a, const CompositeAudioKey& b)
{
    return a.audioGuid == b.audioGuid &&
           a.sourcePath == b.sourcePath &&
           a.name == b.name &&
           NearlySame(a.startOffset, b.startOffset) &&
           NearlySame(a.endOffset, b.endOffset) &&
           NearlySame(a.volumeDb, b.volumeDb) &&
           NearlySame(a.pitchScale, b.pitchScale);
}

bool SameCompositeAnimationKey(const CompositeAnimationKey& a, const CompositeAnimationKey& b)
{
    return a.animationGuid == b.animationGuid &&
           a.sourcePath == b.sourcePath &&
           a.animationName == b.animationName &&
           NearlySame(a.speedScale, b.speedScale);
}

float ComputeInsetTimelineX(float time, float rangeStart, float rangeDuration, float x, float w, float inset)
{
    if (w <= 0.0f)
        return x;
    const float usableW = std::max(1.0f, w - inset * 2.0f);
    return x + inset + (time - rangeStart) / rangeDuration * usableW;
}

template <typename Key, typename EqualFn>
void EmitGodotStyleKeyLinks(UI::PrimitiveEmitContext& ctx,
                            const std::vector<Key>& keys,
                            EqualFn equal,
                            float rangeStart, float rangeDuration,
                            float x, float w, float centerY, float cs,
                            uint32_t color)
{
    if (keys.size() < 2u)
        return;
    std::vector<const Key*> sorted;
    sorted.reserve(keys.size());
    for (const Key& key : keys)
        sorted.push_back(&key);
    std::sort(sorted.begin(), sorted.end(),
              [](const Key* a, const Key* b) { return a->time < b->time; });
    for (size_t i = 1; i < sorted.size(); ++i)
    {
        if (equal(*sorted[i - 1], *sorted[i]))
            EmitTrackKeyConnectorSegment(ctx, sorted[i - 1]->time, sorted[i]->time,
                                         rangeStart, rangeDuration, x, w, centerY, cs, color);
    }
}
}

static bool HitTestClips(const TimeCompositeModel* model,
                         float localX, float localY,
                         float drawX, float drawY, float drawW, float drawH,
                         float rangeStart, float rangeEnd, float rowH,
                         size_t& outTrackIdx, size_t& outClipIdx, float& outOffset)
{
    outTrackIdx = 0;
    outClipIdx = 0;
    outOffset = 0.0f;
    if (!model || model->tracks.empty() || drawW <= 0.0f || drawH <= 0.0f)
        return false;
    const float relX = localX - drawX;
    const float relY = localY - drawY;
    if (relX < 0.0f || relX > drawW || relY < 0.0f || relY > drawH)
        return false;
    const float rangeDuration = std::max(0.001f, rangeEnd - rangeStart);
    rowH = std::max(1.0f, rowH);
    for (size_t tr = 0; tr < model->tracks.size(); ++tr)
    {
        const CompositeTrack& track = model->tracks[tr];
        float rowY = (static_cast<float>(tr) + 0.1f) * rowH;
        float blockH = std::min(rowH * 0.8f, kMaxClipBlockHeightPx);
        if (relY < rowY || relY > rowY + blockH)
            continue;
        for (size_t ci = 0; ci < track.clips.size(); ++ci)
        {
            const CompositeClip& clip = track.clips[ci];
            float startPx = (clip.offsetOnTimeline - rangeStart) / rangeDuration * drawW;
            float duration = clip.outTime - clip.inTime;
            float blockW = std::max(4.0f, duration / rangeDuration * drawW);
            if (relX >= startPx && relX <= startPx + blockW)
            {
                outTrackIdx = tr;
                outClipIdx = ci;
                outOffset = clip.offsetOnTimeline;
                return true;
            }
        }
    }
    return false;
}

static bool HitTestClipLeftEdge(const TimeCompositeModel* model,
                                 float localX, float localY, float drawW, float drawH,
                                 float rangeStart, float rangeEnd, float rowH,
                                 size_t& outTrackIdx, size_t& outClipIdx, float& outStartOnTimeline)
{
    if (!model || model->tracks.empty() || drawW <= 0.0f || drawH <= 0.0f) return false;
    if (localX < 0.0f || localX > drawW || localY < 0.0f || localY > drawH) return false;
    const float rangeDuration = std::max(0.001f, rangeEnd - rangeStart);
    rowH = std::max(1.0f, rowH);
    for (size_t tr = 0; tr < model->tracks.size(); ++tr)
    {
        const float rowY   = (static_cast<float>(tr) + 0.1f) * rowH;
        const float blockH = std::min(rowH * 0.8f, kMaxClipBlockHeightPx);
        if (localY < rowY || localY > rowY + blockH) continue;
        for (size_t ci = 0; ci < model->tracks[tr].clips.size(); ++ci)
        {
            const CompositeClip& clip = model->tracks[tr].clips[ci];
            const float startPx = (clip.offsetOnTimeline - rangeStart) / rangeDuration * drawW;
            const float blockW  = std::max(4.0f, (clip.outTime - clip.inTime) / rangeDuration * drawW);
            if (std::abs(localX - startPx) <= kResizeHandleW && localX <= startPx + blockW)
            {
                outTrackIdx       = tr;
                outClipIdx        = ci;
                outStartOnTimeline = clip.offsetOnTimeline;
                return true;
            }
        }
    }
    return false;
}

static bool HitTestClipRightEdge(const TimeCompositeModel* model,
                                  float localX, float localY, float drawW, float drawH,
                                  float rangeStart, float rangeEnd, float rowH,
                                  size_t& outTrackIdx, size_t& outClipIdx, float& outEndOnTimeline)
{
    if (!model || model->tracks.empty() || drawW <= 0.0f || drawH <= 0.0f) return false;
    if (localX < 0.0f || localX > drawW || localY < 0.0f || localY > drawH) return false;
    const float rangeDuration = std::max(0.001f, rangeEnd - rangeStart);
    rowH = std::max(1.0f, rowH);
    for (size_t tr = 0; tr < model->tracks.size(); ++tr)
    {
        const float rowY  = (static_cast<float>(tr) + 0.1f) * rowH;
        const float blockH = std::min(rowH * 0.8f, kMaxClipBlockHeightPx);
        if (localY < rowY || localY > rowY + blockH) continue;
        for (size_t ci = 0; ci < model->tracks[tr].clips.size(); ++ci)
        {
            const CompositeClip& clip = model->tracks[tr].clips[ci];
            const float duration = clip.outTime - clip.inTime;
            const float startPx  = (clip.offsetOnTimeline - rangeStart) / rangeDuration * drawW;
            const float blockW   = std::max(4.0f, duration / rangeDuration * drawW);
            const float rightPx  = startPx + blockW;
            if (std::abs(localX - rightPx) <= kResizeHandleW && localX >= startPx)
            {
                outTrackIdx      = tr;
                outClipIdx       = ci;
                outEndOnTimeline = clip.offsetOnTimeline + duration;
                return true;
            }
        }
    }
    return false;
}

bool TimeCompositeView::HitTestClipFadeHandle(float localX, float localY, float W,
                                              size_t& trackIdx, size_t& clipIdx, bool& isLeft) const
{
    if (!m_Model || m_Model->tracks.empty() || W <= 0.0f) return false;
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const float rowH = std::max(1.0f, m_RowHeight > 0.0f ? m_RowHeight : kTrackRowHeightPx);
    for (size_t tr = 0; tr < m_Model->tracks.size(); ++tr)
    {
        const float rowY = (static_cast<float>(tr) + 0.1f) * rowH;
        // Fade handles live in the top band of the clip block.
        if (localY < rowY || localY > rowY + kFadeHandleHotTopH) continue;
        const auto& clips = m_Model->tracks[tr].clips;
        for (size_t ci = 0; ci < clips.size(); ++ci)
        {
            const CompositeClip& clip = clips[ci];
            const float duration = std::max(0.001f, clip.outTime - clip.inTime);
            const float startPx  = (clip.offsetOnTimeline - m_RangeStart) / rangeDuration * W;
            const float blockW   = std::max(4.0f, duration / rangeDuration * W);
            const float fadeInPx  = std::clamp(clip.fadeInDuration  / rangeDuration * W, 0.0f, blockW);
            const float fadeOutPx = std::clamp(clip.fadeOutDuration / rangeDuration * W, 0.0f, blockW);
            const float leftTipX  = startPx + fadeInPx;
            const float rightTipX = startPx + blockW - fadeOutPx;
            if (std::abs(localX - leftTipX) <= kFadeHandleHotHalfW && localX >= startPx - kFadeHandleHotHalfW)
            {
                trackIdx = tr;
                clipIdx  = ci;
                isLeft   = true;
                return true;
            }
            if (std::abs(localX - rightTipX) <= kFadeHandleHotHalfW && localX <= startPx + blockW + kFadeHandleHotHalfW)
            {
                trackIdx = tr;
                clipIdx  = ci;
                isLeft   = false;
                return true;
            }
        }
    }
    return false;
}

TimeCompositeView::TimeCompositeView()
{
    AddClass("animationwindow-timecomposite");
}

void TimeCompositeView::SetModel(const TimeCompositeModel* model)
{
    if (m_Model != model)
    {
        m_Model = model;
        MarkDirty(VisualDirty);
    }
}

void TimeCompositeView::SetTimeRange(float rangeStart, float rangeEnd)
{
    if (m_RangeStart != rangeStart || m_RangeEnd != rangeEnd)
    {
        m_RangeStart = rangeStart;
        m_RangeEnd = rangeEnd;
        MarkDirty(VisualDirty);
    }
}

void TimeCompositeView::SetCurrentTime(float t)
{
    if (m_CurrentTime != t)
    {
        m_CurrentTime = t;
        MarkDirty(VisualDirty);
    }
}

float TimeCompositeView::GetTimelineContentScale() const
{
    if (UIManager* ui = GetOwnerManager())
        return std::max(0.01f, ui->GetContentScale());
    return 1.0f;
}

float TimeCompositeView::GetTrackKeyEdgeInsetPx() const
{
    return kTrackKeyEdgeInsetPx * GetTimelineContentScale();
}

float TimeCompositeView::GetTrackKeyHitHalfPx() const
{
    return kTrackKeyHitHalfLogicalPx * GetTimelineContentScale();
}

void TimeCompositeView::SetSelectedClip(size_t trackIndex, size_t clipIndex)
{
    m_SelectedTrackIdx = trackIndex;
    m_SelectedClipIdx = clipIndex;
    m_SelectedClips = {{trackIndex, clipIndex}};
    m_SelectedTrackKeys.clear();
    MarkDirty(VisualDirty);
}

void TimeCompositeView::SetSelectedTrackKey(size_t trackIndex, CompositeTrackType keyType, size_t keyIndex)
{
    if (!m_Model || trackIndex >= m_Model->tracks.size())
        return;

    const CompositeTrack& track = m_Model->tracks[trackIndex];
    bool valid = false;
    if (keyType == CompositeTrackType::Property)
        valid = keyIndex < track.valueKeys.size();
    else if (keyType == CompositeTrackType::Method)
        valid = keyIndex < track.methodKeys.size();
    else if (keyType == CompositeTrackType::Audio)
        valid = keyIndex < track.audioKeys.size();
    else if (keyType == CompositeTrackType::Animation)
        valid = keyIndex < track.animationKeys.size();
    if (!valid)
        return;

    // Key-only selection: do not set m_SelectedTrackIdx — that drives the full-row tint.
    m_SelectedTrackIdx = static_cast<size_t>(-1);
    m_SelectedClipIdx = static_cast<size_t>(-1);
    m_SelectedClips.clear();
    m_SelectedTrackKeys = {TrackKeyRef{trackIndex, keyType, keyIndex}};
    m_ArmedTrackKeyDrag = TrackKeyRef{trackIndex, keyType, keyIndex};
    m_HasArmedTrackKeyDrag = true;

    m_HoveringMarker = true;
    m_HoverMarkerIsClip = false;
    m_HoverMarkerKind = MarkerDragKind::ValueKey;
    if (keyType == CompositeTrackType::Method)
        m_HoverMarkerKind = MarkerDragKind::MethodKey;
    else if (keyType == CompositeTrackType::Audio)
        m_HoverMarkerKind = MarkerDragKind::AudioKey;
    else if (keyType == CompositeTrackType::Animation)
        m_HoverMarkerKind = MarkerDragKind::AnimationKey;
    m_HoverMarkerTrackIdx = trackIndex;
    m_HoverMarkerClipIdx = 0;
    m_HoverMarkerIdx = keyIndex;

    MarkDirty(VisualDirty);
}

void TimeCompositeView::ArmTrackKeyForImmediateDrag(size_t trackIndex, CompositeTrackType keyType, size_t keyIndex)
{
    m_ArmedTrackKeyDrag = TrackKeyRef{trackIndex, keyType, keyIndex};
    m_HasArmedTrackKeyDrag = true;
}

void TimeCompositeView::ClearSelection()
{
    if (m_SelectedTrackIdx != static_cast<size_t>(-1) || !m_SelectedClips.empty() || !m_SelectedTrackKeys.empty())
    {
        m_SelectedTrackIdx = static_cast<size_t>(-1);
        m_SelectedClipIdx = static_cast<size_t>(-1);
        m_SelectedClips.clear();
        m_SelectedTrackKeys.clear();
        MarkDirty(VisualDirty);
    }
}

void TimeCompositeView::ClearMarkerInteractionState()
{
    bool dirty = false;
    if (m_SelectedTrackIdx != static_cast<size_t>(-1) || !m_SelectedClips.empty() || !m_SelectedTrackKeys.empty())
    {
        m_SelectedTrackIdx = static_cast<size_t>(-1);
        m_SelectedClipIdx = static_cast<size_t>(-1);
        m_SelectedClips.clear();
        m_SelectedTrackKeys.clear();
        dirty = true;
    }
    if (m_PendingDragKind == PendingDragKind::Marker)
    {
        m_PendingDragKind = PendingDragKind::None;
        m_PendingMarkerDragKind = MarkerDragKind::None;
        dirty = true;
    }
    if (m_MarkerDragKind != MarkerDragKind::None)
    {
        m_MarkerDragKind = MarkerDragKind::None;
        dirty = true;
    }
    if (m_HasArmedTrackKeyDrag)
    {
        m_HasArmedTrackKeyDrag = false;
        dirty = true;
    }
    if (m_HoveringMarker)
    {
        m_HoveringMarker = false;
        m_HoverMarkerKind = MarkerDragKind::None;
        m_HoverMarkerIsClip = false;
        dirty = true;
    }
    if (dirty)
        MarkDirty(VisualDirty);
}

bool TimeCompositeView::ResolveTrackKey(const TrackKeyRef& keyRef, MarkerDragKind& outDragKind, float& outKeyTime) const
{
    if (!m_Model || keyRef.trackIdx >= m_Model->tracks.size())
        return false;

    const CompositeTrack& track = m_Model->tracks[keyRef.trackIdx];
    if (keyRef.type == CompositeTrackType::Property && keyRef.keyIdx < track.valueKeys.size())
    {
        outDragKind = MarkerDragKind::ValueKey;
        outKeyTime = track.valueKeys[keyRef.keyIdx].time;
        return true;
    }
    if (keyRef.type == CompositeTrackType::Method && keyRef.keyIdx < track.methodKeys.size())
    {
        outDragKind = MarkerDragKind::MethodKey;
        outKeyTime = track.methodKeys[keyRef.keyIdx].time;
        return true;
    }
    if (keyRef.type == CompositeTrackType::Audio && keyRef.keyIdx < track.audioKeys.size())
    {
        outDragKind = MarkerDragKind::AudioKey;
        outKeyTime = track.audioKeys[keyRef.keyIdx].time;
        return true;
    }
    if (keyRef.type == CompositeTrackType::Animation && keyRef.keyIdx < track.animationKeys.size())
    {
        outDragKind = MarkerDragKind::AnimationKey;
        outKeyTime = track.animationKeys[keyRef.keyIdx].time;
        return true;
    }
    return false;
}

void TimeCompositeView::BeginTrackKeyDrag(size_t trackIndex, CompositeTrackType keyType, MarkerDragKind dragKind,
                                          size_t keyIndex, float keyTime, float localY, float timeAtMouse, UIEvent& e,
                                          bool selectKey)
{
    if (!m_Model || trackIndex >= m_Model->tracks.size() || keyIndex == static_cast<size_t>(-1))
        return;

    m_SelectedClips.clear();
    if (selectKey)
    {
        m_SelectedTrackIdx = static_cast<size_t>(-1);
        m_SelectedClipIdx = static_cast<size_t>(-1);
        m_SelectedTrackKeys = {TrackKeyRef{trackIndex, keyType, keyIndex}};
        m_ArmedTrackKeyDrag = TrackKeyRef{trackIndex, keyType, keyIndex};
        m_HasArmedTrackKeyDrag = true;
    }

    m_HoveringMarker = true;
    m_HoverMarkerIsClip = false;
    m_HoverMarkerKind = dragKind;
    m_HoverMarkerTrackIdx = trackIndex;
    m_HoverMarkerClipIdx = 0;
    m_HoverMarkerIdx = keyIndex;

    if (selectKey && m_OnTrackKeySelected)
        m_OnTrackKeySelected(trackIndex, keyType, keyIndex);

    m_PendingDragKind = PendingDragKind::None;
    m_PendingMarkerDragKind = MarkerDragKind::None;
    m_MarkerDragKind = dragKind;
    m_MarkerDragTrackIdx = trackIndex;
    m_MarkerDragIdx = keyIndex;
    m_MarkerDragStartY = localY;
    m_MarkerDragTimeOffset = timeAtMouse - keyTime;

    e.Capture(this);
    MarkDirty(VisualDirty);
}

static std::vector<std::pair<size_t,size_t>> BuildBoxSelection(
    const TimeCompositeModel* model, float W,
    float rangeStart, float rangeEnd, float rowH,
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
    rowH = std::max(1.0f, rowH);
    for (size_t tr = 0; tr < model->tracks.size(); ++tr)
    {
        const float rowTop    = (static_cast<float>(tr) + 0.1f) * rowH;
        const float blockH    = std::min(rowH * 0.8f, kMaxClipBlockHeightPx);
        const float rowBottom = rowTop + blockH;
        if (by1 < rowTop || by0 > rowBottom) continue;
        for (size_t ci = 0; ci < model->tracks[tr].clips.size(); ++ci)
        {
            const CompositeClip& clip = model->tracks[tr].clips[ci];
            const float duration = clip.outTime - clip.inTime;
            const float clipX0 = (clip.offsetOnTimeline - rangeStart) / rangeDuration * W;
            const float clipX1 = clipX0 + std::max(4.0f, duration / rangeDuration * W);
            if (bx1 < clipX0 || bx0 > clipX1) continue;
            const std::pair<size_t,size_t> key{tr, ci};
            if (std::find(result.begin(), result.end(), key) == result.end())
                result.push_back(key);
        }
    }
    return result;
}

static std::vector<TimeCompositeView::TrackKeyRef> BuildBoxKeySelection(
    const TimeCompositeModel* model, float W,
    float rangeStart, float rangeEnd, float rowH,
    float boxX0, float boxY0, float boxX1, float boxY1,
    const std::vector<TimeCompositeView::TrackKeyRef>& seed,
    float keyEdgeInsetPx)
{
    std::vector<TimeCompositeView::TrackKeyRef> result = seed;
    if (!model || W <= 0.0f) return result;
    const float rangeDuration = std::max(0.001f, rangeEnd - rangeStart);
    const float bx0 = std::min(boxX0, boxX1);
    const float bx1 = std::max(boxX0, boxX1);
    const float by0 = std::min(boxY0, boxY1);
    const float by1 = std::max(boxY0, boxY1);
    rowH = std::max(1.0f, rowH);
    auto addIfInside = [&](size_t tr, CompositeTrackType type, size_t keyIdx, float keyTime)
    {
        const float kx = ComputeInsetTimelineX(
            keyTime, rangeStart, rangeDuration, 0.0f, W, keyEdgeInsetPx);
        if (kx < bx0 || kx > bx1)
            return;
        TimeCompositeView::TrackKeyRef ref{tr, type, keyIdx};
        if (std::find(result.begin(), result.end(), ref) == result.end())
            result.push_back(ref);
    };
    for (size_t tr = 0; tr < model->tracks.size(); ++tr)
    {
        const CompositeTrack& track = model->tracks[tr];
        const float rowTop = static_cast<float>(tr) * rowH;
        const float rowBottom = rowTop + rowH;
        if (by1 < rowTop || by0 > rowBottom)
            continue;
        for (size_t ki = 0; ki < track.valueKeys.size(); ++ki)
            addIfInside(tr, CompositeTrackType::Property, ki, track.valueKeys[ki].time);
        for (size_t ki = 0; ki < track.methodKeys.size(); ++ki)
            addIfInside(tr, CompositeTrackType::Method, ki, track.methodKeys[ki].time);
        for (size_t ki = 0; ki < track.audioKeys.size(); ++ki)
            addIfInside(tr, CompositeTrackType::Audio, ki, track.audioKeys[ki].time);
        for (size_t ki = 0; ki < track.animationKeys.size(); ++ki)
            addIfInside(tr, CompositeTrackType::Animation, ki, track.animationKeys[ki].time);
    }
    return result;
}

bool TimeCompositeView::HitTestTrackMarker(float localX, float localY, float W,
                                            size_t& trackIdx, size_t& markerIdx) const
{
    if (!m_Model || W <= 0.0f) return false;
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const float rowH = std::max(1.0f, m_RowHeight > 0.0f ? m_RowHeight : kTrackRowHeightPx);
    for (size_t tr = 0; tr < m_Model->tracks.size(); ++tr)
    {
        const CompositeTrack& track = m_Model->tracks[tr];
        const float rowTop = static_cast<float>(tr) * rowH;
        const float rowBot = rowTop + rowH;
        if (localY < rowTop || localY > rowBot) continue;
        for (size_t mi = 0; mi < track.markers.size(); ++mi)
        {
            const float mx = (track.markers[mi].time - m_RangeStart) / rangeDuration * W;
            if (std::abs(localX - mx) <= kMarkerHitHalfW)
            {
                trackIdx = tr;
                markerIdx = mi;
                return true;
            }
        }
    }
    return false;
}

bool TimeCompositeView::HitTestTrackValueKey(float localX, float localY, float W,
                                             size_t& trackIdx, size_t& keyIdx) const
{
    if (!m_Model || W <= 0.0f)
        return false;
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const float rowH = std::max(1.0f, m_RowHeight > 0.0f ? m_RowHeight : kTrackRowHeightPx);
    for (size_t tr = 0; tr < m_Model->tracks.size(); ++tr)
    {
        const CompositeTrack& track = m_Model->tracks[tr];
        if (!CompositeTrackTypeUsesValueKeys(track.type))
            continue;
        const float rowTop = static_cast<float>(tr) * rowH;
        const float rowBot = rowTop + rowH;
        if (localY < rowTop || localY > rowBot)
            continue;
        for (size_t ki = 0; ki < track.valueKeys.size(); ++ki)
        {
            const float kx = ComputeInsetTimelineX(
                track.valueKeys[ki].time, m_RangeStart, rangeDuration, 0.0f, W, GetTrackKeyEdgeInsetPx());
            if (std::abs(localX - kx) <= GetTrackKeyHitHalfPx())
            {
                trackIdx = tr;
                keyIdx = ki;
                return true;
            }
        }
    }
    return false;
}

bool TimeCompositeView::HitTestTrackMethodKey(float localX, float localY, float W,
                                              size_t& trackIdx, size_t& keyIdx) const
{
    if (!m_Model || W <= 0.0f)
        return false;
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const float rowH = std::max(1.0f, m_RowHeight > 0.0f ? m_RowHeight : kTrackRowHeightPx);
    for (size_t tr = 0; tr < m_Model->tracks.size(); ++tr)
    {
        const CompositeTrack& track = m_Model->tracks[tr];
        if (!CompositeTrackTypeUsesMethodKeys(track.type))
            continue;
        const float rowTop = static_cast<float>(tr) * rowH;
        const float rowBot = rowTop + rowH;
        if (localY < rowTop || localY > rowBot)
            continue;
        for (size_t ki = 0; ki < track.methodKeys.size(); ++ki)
        {
            const float kx = ComputeInsetTimelineX(
                track.methodKeys[ki].time, m_RangeStart, rangeDuration, 0.0f, W, GetTrackKeyEdgeInsetPx());
            if (std::abs(localX - kx) <= GetTrackKeyHitHalfPx())
            {
                trackIdx = tr;
                keyIdx = ki;
                return true;
            }
        }
    }
    return false;
}

bool TimeCompositeView::HitTestTrackAudioKey(float localX, float localY, float W,
                                             size_t& trackIdx, size_t& keyIdx) const
{
    if (!m_Model || W <= 0.0f)
        return false;
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const float rowH = std::max(1.0f, m_RowHeight > 0.0f ? m_RowHeight : kTrackRowHeightPx);
    for (size_t tr = 0; tr < m_Model->tracks.size(); ++tr)
    {
        const CompositeTrack& track = m_Model->tracks[tr];
        if (!CompositeTrackTypeUsesAudioKeys(track.type))
            continue;
        const float rowTop = static_cast<float>(tr) * rowH;
        const float rowBot = rowTop + rowH;
        if (localY < rowTop || localY > rowBot)
            continue;
        for (size_t ki = 0; ki < track.audioKeys.size(); ++ki)
        {
            const float kx = ComputeInsetTimelineX(
                track.audioKeys[ki].time, m_RangeStart, rangeDuration, 0.0f, W, GetTrackKeyEdgeInsetPx());
            if (std::abs(localX - kx) <= GetTrackKeyHitHalfPx())
            {
                trackIdx = tr;
                keyIdx = ki;
                return true;
            }
        }
    }
    return false;
}

bool TimeCompositeView::HitTestTrackAnimationKey(float localX, float localY, float W,
                                                 size_t& trackIdx, size_t& keyIdx) const
{
    if (!m_Model || W <= 0.0f)
        return false;
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const float rowH = std::max(1.0f, m_RowHeight > 0.0f ? m_RowHeight : kTrackRowHeightPx);
    for (size_t tr = 0; tr < m_Model->tracks.size(); ++tr)
    {
        const CompositeTrack& track = m_Model->tracks[tr];
        if (!CompositeTrackTypeUsesAnimationKeys(track.type))
            continue;
        const float rowTop = static_cast<float>(tr) * rowH;
        const float rowBot = rowTop + rowH;
        if (localY < rowTop || localY > rowBot)
            continue;
        for (size_t ki = 0; ki < track.animationKeys.size(); ++ki)
        {
            const float kx = ComputeInsetTimelineX(
                track.animationKeys[ki].time, m_RangeStart, rangeDuration, 0.0f, W, GetTrackKeyEdgeInsetPx());
            if (std::abs(localX - kx) <= GetTrackKeyHitHalfPx())
            {
                trackIdx = tr;
                keyIdx = ki;
                return true;
            }
        }
    }
    return false;
}

bool TimeCompositeView::HitTestClipMarker(float localX, float localY, float W,
                                           size_t& trackIdx, size_t& clipIdx, size_t& markerIdx) const
{
    if (!m_Model || W <= 0.0f) return false;
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const float rowH = std::max(1.0f, m_RowHeight > 0.0f ? m_RowHeight : kTrackRowHeightPx);
    const float blockH = std::min(rowH * 0.8f, kMaxClipBlockHeightPx);
    for (size_t tr = 0; tr < m_Model->tracks.size(); ++tr)
    {
        const CompositeTrack& track = m_Model->tracks[tr];
        const float rowTop = static_cast<float>(tr) * rowH + rowH * 0.1f;
        if (localY < rowTop || localY > rowTop + blockH) continue;
        for (size_t ci = 0; ci < track.clips.size(); ++ci)
        {
            const CompositeClip& clip = track.clips[ci];
            for (size_t mi = 0; mi < clip.markers.size(); ++mi)
            {
                const float absTime = clip.offsetOnTimeline + clip.markers[mi].time;
                const float mx = (absTime - m_RangeStart) / rangeDuration * W;
                if (std::abs(localX - mx) <= kMarkerHitHalfW)
                {
                    trackIdx = tr;
                    clipIdx = ci;
                    markerIdx = mi;
                    return true;
                }
            }
        }
    }
    return false;
}

void TimeCompositeView::OnEvent(UIEvent& e)
{
    const float W = GetLayoutWidth();
    const float H = GetLayoutHeight();
    const float localX = e.X - GetLayoutX();
    const float clampedLocalX = std::clamp(localX, 0.0f, W);
    const float localY = e.Y - GetLayoutY();
    const float trackY = localY - m_HeaderOffset;
    const float rowH = std::max(1.0f, m_RowHeight > 0.0f ? m_RowHeight : kTrackRowHeightPx);
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const bool insideGrid = (W > 0.0f && H > 0.0f &&
                             localX >= 0.0f && localX <= W &&
                             localY >= 0.0f && localY <= H);
    const bool shiftHeld = (e.Mods & Input::kModShift) != 0;
    const bool ctrlHeld = (e.Mods & (Input::kModControl | Input::kModSuper)) != 0;
    const bool altHeld = (e.Mods & Input::kModAlt) != 0;
    const float timeAtMouse = (W > 0.0f && H > 0.0f)
                                  ? m_RangeStart + (clampedLocalX / W) * rangeDuration
                                  : m_RangeStart;

    auto finishActivePointerGesture = [this]() -> bool
    {
        bool hadGesture = false;
        if (m_Panning)
        {
            m_Panning = false;
            hadGesture = true;
        }
        if (m_PendingDragKind != PendingDragKind::None)
        {
            m_PendingDragKind = PendingDragKind::None;
            m_PendingMarkerDragKind = MarkerDragKind::None;
            hadGesture = true;
        }
        if (m_ReorderingTrack)
        {
            m_ReorderingTrack = false;
            hadGesture = true;
        }
        if (m_MovingTrackInTime)
        {
            m_MovingTrackInTime = false;
            if (m_OnTrackTimeDragEnded && m_Model && m_TimeDragTrackIdx < m_Model->tracks.size())
                m_OnTrackTimeDragEnded(m_TimeDragTrackIdx);
            hadGesture = true;
        }
        if (m_MarkerDragKind != MarkerDragKind::None)
        {
            m_MarkerDragKind = MarkerDragKind::None;
            hadGesture = true;
        }
        if (m_DraggingFade)
        {
            m_DraggingFade = false;
            if (m_OnClipDragEnded)
                m_OnClipDragEnded(m_FadeDragTrackIdx, m_FadeDragClipIdx);
            hadGesture = true;
        }
        if (m_ResizingClip)
        {
            m_ResizingClip = false;
            if (m_OnClipDragEnded)
                m_OnClipDragEnded(m_ResizeTrackIdx, m_ResizeClipIdx);
            hadGesture = true;
        }
        if (m_DraggingClip)
        {
            m_DraggingClip = false;
            if (m_OnClipDragEnded)
                m_OnClipDragEnded(m_DragTrackIdx, m_DragClipIdx);
            hadGesture = true;
        }
        if (m_Seeking)
        {
            m_Seeking = false;
            hadGesture = true;
        }
        if (m_BoxSelecting || m_PendingEmptyAction)
        {
            m_BoxSelecting = false;
            m_PendingEmptyAction = false;
            hadGesture = true;
        }
        if (hadGesture)
            MarkDirty(VisualDirty);
        return hadGesture;
    };

    if (e.Id == kEventMouseMove && !e.ButtonDown && finishActivePointerGesture())
    {
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseUp)
    {
        if (m_Panning) { m_Panning = false; e.Stop(); return; }
        if (m_PendingDragKind != PendingDragKind::None)
        {
            m_PendingDragKind = PendingDragKind::None;
            m_PendingMarkerDragKind = MarkerDragKind::None;
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
        if (m_ReorderingTrack)
        {
            m_ReorderingTrack = false;
            if (m_OnTrackReordered && m_Model &&
                m_ReorderDragTrackIdx < m_Model->tracks.size() &&
                m_ReorderTargetIdx   < m_Model->tracks.size() &&
                m_ReorderDragTrackIdx != m_ReorderTargetIdx)
            {
                m_OnTrackReordered(m_ReorderDragTrackIdx, m_ReorderTargetIdx);
            }
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
        if (m_MovingTrackInTime)
        {
            m_MovingTrackInTime = false;
            if (m_OnTrackTimeDragEnded && m_Model && m_TimeDragTrackIdx < m_Model->tracks.size())
                m_OnTrackTimeDragEnded(m_TimeDragTrackIdx);
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
        if (m_MarkerDragKind != MarkerDragKind::None)
        {
            const float draggedDown = localY - m_MarkerDragStartY;
            if (draggedDown >= kMarkerDragDownRemoveThreshold)
            {
                if (m_MarkerDragKind == MarkerDragKind::Track && m_OnTrackMarkerRemoved)
                    m_OnTrackMarkerRemoved(m_MarkerDragTrackIdx, m_MarkerDragIdx);
                else if (m_MarkerDragKind == MarkerDragKind::Clip && m_OnClipMarkerRemoved)
                    m_OnClipMarkerRemoved(m_MarkerDragTrackIdx, m_MarkerDragClipIdx, m_MarkerDragIdx);
                else if (m_MarkerDragKind == MarkerDragKind::ValueKey && m_OnTrackValueKeyRemoved)
                    m_OnTrackValueKeyRemoved(m_MarkerDragTrackIdx, m_MarkerDragIdx);
                else if (m_MarkerDragKind == MarkerDragKind::MethodKey && m_OnTrackMethodKeyRemoved)
                    m_OnTrackMethodKeyRemoved(m_MarkerDragTrackIdx, m_MarkerDragIdx);
                else if (m_MarkerDragKind == MarkerDragKind::AudioKey && m_OnTrackAudioKeyRemoved)
                    m_OnTrackAudioKeyRemoved(m_MarkerDragTrackIdx, m_MarkerDragIdx);
                else if (m_MarkerDragKind == MarkerDragKind::AnimationKey && m_OnTrackAnimationKeyRemoved)
                    m_OnTrackAnimationKeyRemoved(m_MarkerDragTrackIdx, m_MarkerDragIdx);
            }
            m_MarkerDragKind = MarkerDragKind::None;
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
        if (m_DraggingFade)
        {
            m_DraggingFade = false;
            if (m_OnClipDragEnded)
                m_OnClipDragEnded(m_FadeDragTrackIdx, m_FadeDragClipIdx);
            e.Stop();
            return;
        }
        if (m_ResizingClip)
        {
            m_ResizingClip = false;
            if (m_OnClipDragEnded)
                m_OnClipDragEnded(m_ResizeTrackIdx, m_ResizeClipIdx);
            e.Stop();
            return;
        }
        if (m_DraggingClip)
        {
            m_DraggingClip = false;
            const bool dropToNewRow = m_Model && m_DragTargetTrack == m_Model->tracks.size();
            const bool dropToExistingRow = m_Model && m_DragTargetTrack < m_Model->tracks.size() &&
                                           CompositeTrackTypeUsesClips(m_Model->tracks[m_DragTargetTrack].type);
            if (m_DragTargetTrack != m_DragTrackIdx && m_OnClipMovedToTrack && m_DragClipRelOffsets.size() == 1 &&
                m_Model && m_DragTrackIdx < m_Model->tracks.size() &&
                m_DragClipIdx < m_Model->tracks[m_DragTrackIdx].clips.size() &&
                (dropToNewRow || dropToExistingRow))
            {
                const float newOffset = m_Model->tracks[m_DragTrackIdx].clips[m_DragClipIdx].offsetOnTimeline;
                m_OnClipMovedToTrack(m_DragTrackIdx, m_DragClipIdx, m_DragTargetTrack, newOffset);
            }
            if (m_OnClipDragEnded)
                m_OnClipDragEnded(m_DragTrackIdx, m_DragClipIdx);
            MarkDirty(VisualDirty);
        }
        if (m_Seeking) m_Seeking = false;
        const bool hadBoxSelecting = m_BoxSelecting;
        m_BoxSelecting = false;
        m_PendingEmptyAction = false;
        if (hadBoxSelecting)
            MarkDirty(VisualDirty);
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseMove)
    {
        if (m_Panning && m_OnPan && W > 0.0f)
        {
            const float deltaPx = e.X - m_PanLastGlobalX;
            m_OnPan((deltaPx / W) * rangeDuration);
            m_PanLastGlobalX = e.X;
            e.Stop();
            return;
        }
        if (m_PendingDragKind != PendingDragKind::None)
        {
            const float dx = localX - m_PendingDragStartX;
            const float dy = localY - m_PendingDragStartY;
            const bool clipDragStarted =
                m_PendingDragKind == PendingDragKind::Clip &&
                (std::abs(dx) >= kClipHorizontalDragStartThreshold ||
                 std::abs(dy) >= kClipVerticalDragStartThreshold);
            const bool standardDragStarted =
                m_PendingDragKind != PendingDragKind::Clip &&
                (dx * dx + dy * dy) >= kDragStartThresholdSq;
            if (!clipDragStarted && !standardDragStarted)
            {
                e.Stop();
                return;
            }

            if (m_PendingDragKind == PendingDragKind::Clip)
            {
                m_DraggingClip = true;
                if (m_OnClipDragStarted)
                    m_OnClipDragStarted(m_DragTrackIdx, m_DragClipIdx);
            }
            else if (m_PendingDragKind == PendingDragKind::Resize)
            {
                m_ResizingClip = true;
                if (m_OnClipDragStarted)
                    m_OnClipDragStarted(m_ResizeTrackIdx, m_ResizeClipIdx);
            }
            else if (m_PendingDragKind == PendingDragKind::Fade)
            {
                m_DraggingFade = true;
                if (m_OnClipDragStarted)
                    m_OnClipDragStarted(m_FadeDragTrackIdx, m_FadeDragClipIdx);
            }
            else if (m_PendingDragKind == PendingDragKind::Marker)
                m_MarkerDragKind = m_PendingMarkerDragKind;

            m_PendingDragKind = PendingDragKind::None;
            m_PendingMarkerDragKind = MarkerDragKind::None;
        }
        if (m_MarkerDragKind != MarkerDragKind::None && W > 0.0f)
        {
            const float draggedDown = localY - m_MarkerDragStartY;
            if (draggedDown < kMarkerDragDownRemoveThreshold)
            {
                const float newTime = timeAtMouse - m_MarkerDragTimeOffset;
                if (m_MarkerDragKind == MarkerDragKind::Track && m_OnTrackMarkerMoved)
                    m_OnTrackMarkerMoved(m_MarkerDragTrackIdx, m_MarkerDragIdx, newTime);
                else if (m_MarkerDragKind == MarkerDragKind::ValueKey && m_OnTrackValueKeyMoved)
                    m_OnTrackValueKeyMoved(m_MarkerDragTrackIdx, m_MarkerDragIdx, newTime);
                else if (m_MarkerDragKind == MarkerDragKind::MethodKey && m_OnTrackMethodKeyMoved)
                    m_OnTrackMethodKeyMoved(m_MarkerDragTrackIdx, m_MarkerDragIdx, newTime);
                else if (m_MarkerDragKind == MarkerDragKind::AudioKey && m_OnTrackAudioKeyMoved)
                    m_OnTrackAudioKeyMoved(m_MarkerDragTrackIdx, m_MarkerDragIdx, newTime);
                else if (m_MarkerDragKind == MarkerDragKind::AnimationKey && m_OnTrackAnimationKeyMoved)
                    m_OnTrackAnimationKeyMoved(m_MarkerDragTrackIdx, m_MarkerDragIdx, newTime);
                else if (m_MarkerDragKind == MarkerDragKind::Clip && m_OnClipMarkerMoved)
                {
                    if (m_Model && m_MarkerDragTrackIdx < m_Model->tracks.size() &&
                        m_MarkerDragClipIdx < m_Model->tracks[m_MarkerDragTrackIdx].clips.size())
                    {
                        const float clipStart = m_Model->tracks[m_MarkerDragTrackIdx].clips[m_MarkerDragClipIdx].offsetOnTimeline;
                        m_OnClipMarkerMoved(m_MarkerDragTrackIdx, m_MarkerDragClipIdx, m_MarkerDragIdx,
                                            std::max(0.0f, newTime - clipStart));
                    }
                }
                MarkDirty(VisualDirty);
            }
            e.Stop();
            return;
        }
        if (m_DraggingFade && W > 0.0f)
        {
            if (m_Model && m_FadeDragTrackIdx < m_Model->tracks.size() &&
                m_FadeDragClipIdx < m_Model->tracks[m_FadeDragTrackIdx].clips.size())
            {
                const CompositeClip& clip = m_Model->tracks[m_FadeDragTrackIdx].clips[m_FadeDragClipIdx];
                const float duration = std::max(0.0f, clip.outTime - clip.inTime);
                if (m_FadeDragLeft && m_OnClipFadeInChanged)
                {
                    const float maxFade = std::max(0.0f, duration - clip.fadeOutDuration);
                    const float newFade = std::clamp(timeAtMouse - clip.offsetOnTimeline, 0.0f, maxFade);
                    m_OnClipFadeInChanged(m_FadeDragTrackIdx, m_FadeDragClipIdx, newFade);
                }
                else if (!m_FadeDragLeft && m_OnClipFadeOutChanged)
                {
                    const float clipEnd = clip.offsetOnTimeline + duration;
                    const float maxFade = std::max(0.0f, duration - clip.fadeInDuration);
                    const float newFade = std::clamp(clipEnd - timeAtMouse, 0.0f, maxFade);
                    m_OnClipFadeOutChanged(m_FadeDragTrackIdx, m_FadeDragClipIdx, newFade);
                }
            }
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
        if (m_ResizingClip && W > 0.0f)
        {
            if (m_Model && m_ResizeTrackIdx < m_Model->tracks.size() &&
                m_ResizeClipIdx < m_Model->tracks[m_ResizeTrackIdx].clips.size())
            {
                const CompositeClip& clip = m_Model->tracks[m_ResizeTrackIdx].clips[m_ResizeClipIdx];
                if (m_ResizeLeft && m_OnClipStartChanged)
                {
                    const float rightEdge = clip.offsetOnTimeline + (clip.outTime - clip.inTime);
                    const float newOffset = std::clamp(timeAtMouse - m_ResizeDragOffset, 0.0f, rightEdge - 1.0f / 30.0f);
                    const float newInTime = std::clamp(clip.inTime + (newOffset - clip.offsetOnTimeline), 0.0f, clip.outTime - 1.0f / 30.0f);
                    m_OnClipStartChanged(m_ResizeTrackIdx, m_ResizeClipIdx, newOffset, newInTime);
                }
                else if (!m_ResizeLeft && m_OnClipEndChanged)
                {
                    const float newDuration = std::max(1.0f / 30.0f, (timeAtMouse - m_ResizeDragOffset) - clip.offsetOnTimeline);
                    m_OnClipEndChanged(m_ResizeTrackIdx, m_ResizeClipIdx, clip.inTime + newDuration);
                }
            }
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
        if (m_DraggingClip && m_OnClipOffsetChanged && W > 0.0f)
        {
            const float newAnchorOffset = std::max(0.0f, timeAtMouse - m_DragOffsetTime);
            for (auto& [key, relOffset] : m_DragClipRelOffsets)
                m_OnClipOffsetChanged(key.first, key.second, std::max(0.0f, newAnchorOffset + relOffset));
            // Track cross-row target for single-clip cross-track movement.
            if (m_OnClipMovedToTrack && m_Model && !m_Model->tracks.empty() && m_DragClipRelOffsets.size() == 1 &&
                std::abs(localY - m_PendingDragStartY) >= kClipVerticalDragStartThreshold)
            {
                const size_t numTracks = m_Model->tracks.size();
                const float yForTarget = std::max(0.0f, trackY);
                const float rowF = yForTarget / rowH;
                const size_t baseRow = static_cast<size_t>(std::floor(rowF));
                const float rowFrac = rowF - static_cast<float>(baseRow);
                const size_t tgt = std::min(baseRow + (rowFrac >= 0.5f ? 1u : 0u), numTracks);
                const bool validDropTarget = (tgt == numTracks) ||
                                             (tgt < numTracks && CompositeTrackTypeUsesClips(m_Model->tracks[tgt].type));
                if (validDropTarget)
                {
                    if (tgt != m_DragTargetTrack)
                    {
                        m_DragTargetTrack = tgt;
                        MarkDirty(VisualDirty);
                    }
                }
            }
            MarkDirty(VisualDirty);
            e.Stop();
        }
        else if (m_Seeking && m_OnSeekToTime && W > 0.0f)
        {
            m_OnSeekToTime(std::clamp(timeAtMouse, m_RangeStart, m_RangeEnd));
            e.Stop();
        }
        else if ((m_BoxSelecting || m_ReorderingTrack || m_MovingTrackInTime || m_PendingEmptyAction) && W > 0.0f && H > 0.0f)
        {
            const float dx = localX - m_EmptyActionStartX;
            const float dy = trackY - m_EmptyActionStartY;
            const bool thresholdCrossed = (dx * dx + dy * dy) >= kBoxSelectThresholdSq;

            // Vertical drag on empty space with the reorder callback set → enter track reorder.
            if (!m_BoxSelecting && !m_ReorderingTrack && thresholdCrossed &&
                m_OnTrackReordered && m_Model && !m_Model->tracks.empty() &&
                std::abs(dy) > std::abs(dx) + 5.0f)
            {
                m_PendingEmptyAction = false;
                m_ReorderingTrack = true;
                const size_t numTracks = m_Model->tracks.size();
                const float startRow = std::max(0.0f, m_EmptyActionStartY) / rowH;
                m_ReorderDragTrackIdx = std::min(static_cast<size_t>(startRow), numTracks - 1);
            }
            else if (!m_BoxSelecting && !m_ReorderingTrack && !m_MovingTrackInTime && thresholdCrossed &&
                     !m_EmptyActionShiftHeld &&
                     m_OnTrackTimeOffsetChanged && m_Model && !m_Model->tracks.empty() &&
                     std::abs(dx) >= std::abs(dy) - 5.0f)
            {
                const size_t numTracks = m_Model->tracks.size();
                const float startRow = std::max(0.0f, m_EmptyActionStartY) / rowH;
                m_TimeDragTrackIdx = std::min(static_cast<size_t>(startRow), numTracks - 1);
                m_TimeDragLastX = m_EmptyActionStartX;
                m_PendingEmptyAction = false;
                m_MovingTrackInTime = true;
                if (m_OnTrackTimeDragStarted)
                    m_OnTrackTimeDragStarted(m_TimeDragTrackIdx);
            }

            if (m_ReorderingTrack && m_Model && !m_Model->tracks.empty())
            {
                const size_t numTracks = m_Model->tracks.size();
                const float clampedY = std::clamp(trackY, 0.0f, H - 1.0f);
                m_ReorderTargetIdx = std::min(static_cast<size_t>(clampedY / rowH), numTracks - 1);
                MarkDirty(VisualDirty);
                e.Stop();
            }
            else if (m_MovingTrackInTime)
            {
                const float deltaPx = localX - m_TimeDragLastX;
                if (std::abs(deltaPx) > 0.0f && m_OnTrackTimeOffsetChanged)
                    m_OnTrackTimeOffsetChanged(m_TimeDragTrackIdx, (deltaPx / W) * rangeDuration);
                m_TimeDragLastX = localX;
                MarkDirty(VisualDirty);
                e.Stop();
            }
            else if (m_BoxSelecting ||
                     (thresholdCrossed &&
                      (m_EmptyActionShiftHeld || !m_OnTrackTimeOffsetChanged) &&
                      (!m_OnTrackReordered || std::abs(dx) >= std::abs(dy) - 5.0f)))
            {
                m_BoxSelecting = true;
                m_BoxEndX = localX;
                m_BoxEndY = trackY;
                const auto boxClipHits = BuildBoxSelection(m_Model, W, m_RangeStart, m_RangeEnd, rowH,
                                                           m_BoxStartX, m_BoxStartY, m_BoxEndX, m_BoxEndY,
                                                           std::vector<std::pair<size_t,size_t>>{});
                const auto boxKeyHits = BuildBoxKeySelection(m_Model, W, m_RangeStart, m_RangeEnd, rowH,
                                                             m_BoxStartX, m_BoxStartY, m_BoxEndX, m_BoxEndY,
                                                             std::vector<TrackKeyRef>{}, GetTrackKeyEdgeInsetPx());
                if (ctrlHeld)
                {
                    m_SelectedClips = m_BoxSelectionSeed;
                    for (const auto& hit : boxClipHits)
                    {
                        const auto it = std::find(m_SelectedClips.begin(), m_SelectedClips.end(), hit);
                        if (it == m_SelectedClips.end())
                            m_SelectedClips.push_back(hit);
                        else
                            m_SelectedClips.erase(it);
                    }
                    m_SelectedTrackKeys = m_BoxKeySelectionSeed;
                    for (const auto& hit : boxKeyHits)
                    {
                        const auto it = std::find(m_SelectedTrackKeys.begin(), m_SelectedTrackKeys.end(), hit);
                        if (it == m_SelectedTrackKeys.end())
                            m_SelectedTrackKeys.push_back(hit);
                        else
                            m_SelectedTrackKeys.erase(it);
                    }
                }
                else
                {
                    m_SelectedClips = shiftHeld ? BuildBoxSelection(m_Model, W, m_RangeStart, m_RangeEnd, rowH,
                                                                    m_BoxStartX, m_BoxStartY, m_BoxEndX, m_BoxEndY,
                                                                    m_BoxSelectionSeed)
                                                : boxClipHits;
                    m_SelectedTrackKeys = shiftHeld ? BuildBoxKeySelection(m_Model, W, m_RangeStart, m_RangeEnd, rowH,
                                                                            m_BoxStartX, m_BoxStartY, m_BoxEndX, m_BoxEndY,
                                                                            m_BoxKeySelectionSeed, GetTrackKeyEdgeInsetPx())
                                                    : boxKeyHits;
                }
                if (!m_SelectedClips.empty())
                {
                    m_SelectedTrackIdx = m_SelectedClips.front().first;
                    m_SelectedClipIdx  = m_SelectedClips.front().second;
                }
                else if (!m_SelectedTrackKeys.empty())
                {
                    m_SelectedTrackIdx = static_cast<size_t>(-1);
                    m_SelectedClipIdx = static_cast<size_t>(-1);
                    const TrackKeyRef& selectedKey = m_SelectedTrackKeys.front();
                    if (m_OnTrackKeySelected)
                        m_OnTrackKeySelected(selectedKey.trackIdx, selectedKey.type, selectedKey.keyIdx);
                }
                else
                {
                    m_SelectedTrackIdx = static_cast<size_t>(-1);
                    m_SelectedClipIdx  = static_cast<size_t>(-1);
                }
                MarkDirty(VisualDirty);
                e.Stop();
            }
        }
        // Idle hover: update fade-handle highlight first (top band), then resize handle.
        if (!m_DraggingClip && !m_ResizingClip && !m_DraggingFade && !m_Panning && !m_Seeking && !m_BoxSelecting)
        {
            size_t fhTr = 0, fhCi = 0;
            bool fhLeft = false;
            const bool overFade = insideGrid &&
                HitTestClipFadeHandle(localX, trackY, W, fhTr, fhCi, fhLeft);
            if (overFade != m_HoveringFade ||
                (overFade && (fhTr != m_HoverFadeTrackIdx || fhCi != m_HoverFadeClipIdx || fhLeft != m_HoverFadeLeft)))
            {
                m_HoveringFade        = overFade;
                m_HoverFadeLeft       = fhLeft;
                m_HoverFadeTrackIdx   = fhTr;
                m_HoverFadeClipIdx    = fhCi;
                MarkDirty(VisualDirty);
            }
            size_t htr = 0, hci = 0;
            float dummy = 0.0f;
            const bool overLeft = !overFade && insideGrid &&
                HitTestClipLeftEdge(m_Model, localX, trackY, W, H, m_RangeStart, m_RangeEnd, rowH, htr, hci, dummy);
            const bool overRight = !overFade && !overLeft && insideGrid &&
                HitTestClipRightEdge(m_Model, localX, trackY, W, H, m_RangeStart, m_RangeEnd, rowH, htr, hci, dummy);
            const bool over = overLeft || overRight;
            if (over != m_HoveringResize ||
                (over && (htr != m_HoverResizeTrackIdx || hci != m_HoverResizeClipIdx || overLeft != m_HoverResizeLeft)))
            {
                m_HoveringResize      = over;
                m_HoverResizeLeft     = overLeft;
                m_HoverResizeTrackIdx = htr;
                m_HoverResizeClipIdx  = hci;
                MarkDirty(VisualDirty);
            }
            // Marker hover: highlight
            if (insideGrid && m_Model)
            {
                size_t hvTrack = 0, hvClip = 0, hvIdx = 0;
                bool found = false;
                bool foundIsClip = false;
                MarkerDragKind foundKind = MarkerDragKind::None;
                if (HitTestTrackValueKey(localX, trackY, W, hvTrack, hvIdx) &&
                    hvTrack < m_Model->tracks.size() &&
                    hvIdx < m_Model->tracks[hvTrack].valueKeys.size())
                {
                    found = true; foundIsClip = false; foundKind = MarkerDragKind::ValueKey;
                }
                else if (HitTestTrackMethodKey(localX, trackY, W, hvTrack, hvIdx) &&
                         hvTrack < m_Model->tracks.size() &&
                         hvIdx < m_Model->tracks[hvTrack].methodKeys.size())
                {
                    found = true; foundIsClip = false; foundKind = MarkerDragKind::MethodKey;
                }
                else if (HitTestTrackAudioKey(localX, trackY, W, hvTrack, hvIdx) &&
                         hvTrack < m_Model->tracks.size() &&
                         hvIdx < m_Model->tracks[hvTrack].audioKeys.size())
                {
                    found = true; foundIsClip = false; foundKind = MarkerDragKind::AudioKey;
                }
                else if (HitTestTrackAnimationKey(localX, trackY, W, hvTrack, hvIdx) &&
                         hvTrack < m_Model->tracks.size() &&
                         hvIdx < m_Model->tracks[hvTrack].animationKeys.size())
                {
                    found = true; foundIsClip = false; foundKind = MarkerDragKind::AnimationKey;
                }
                else if (HitTestTrackMarker(localX, trackY, W, hvTrack, hvIdx) &&
                    hvTrack < m_Model->tracks.size() &&
                    hvIdx < m_Model->tracks[hvTrack].markers.size())
                {
                    found = true; foundIsClip = false; foundKind = MarkerDragKind::Track;
                }
                else if (HitTestClipMarker(localX, trackY, W, hvTrack, hvClip, hvIdx) &&
                         hvTrack < m_Model->tracks.size() &&
                         hvClip < m_Model->tracks[hvTrack].clips.size() &&
                         hvIdx < m_Model->tracks[hvTrack].clips[hvClip].markers.size())
                {
                    found = true; foundIsClip = true; foundKind = MarkerDragKind::Clip;
                }
                const bool hadMarker = m_HoveringMarker;
                const bool stateChanged = found != hadMarker ||
                    (found && (foundKind != m_HoverMarkerKind ||
                               foundIsClip != m_HoverMarkerIsClip ||
                               hvTrack != m_HoverMarkerTrackIdx ||
                               hvClip  != m_HoverMarkerClipIdx  ||
                               hvIdx   != m_HoverMarkerIdx));
                if (stateChanged)
                {
                    m_HoveringMarker      = found;
                    m_HoverMarkerIsClip   = foundIsClip;
                    m_HoverMarkerKind     = foundKind;
                    m_HoverMarkerTrackIdx = hvTrack;
                    m_HoverMarkerClipIdx  = hvClip;
                    m_HoverMarkerIdx      = hvIdx;
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
        if (m_ReorderingTrack){ m_ReorderingTrack = false; dirty = true; }
        if (m_PendingDragKind != PendingDragKind::None)
        {
            m_PendingDragKind = PendingDragKind::None;
            m_PendingMarkerDragKind = MarkerDragKind::None;
            dirty = true;
        }
        if (dirty) MarkDirty(VisualDirty);
        if (m_HoveringMarker)
        {
            m_HoveringMarker = false;
            m_HoverMarkerKind = MarkerDragKind::None;
            MarkDirty(VisualDirty);
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
    if (e.Id == kEventMouseDown && e.Button == 1 && m_OnTrackContextMenu && insideGrid)
    {
        if (m_Model && trackY >= 0.0f)
        {
            const size_t clickedTrack = static_cast<size_t>(trackY / rowH);
            if (clickedTrack < m_Model->tracks.size())
            {
                m_OnTrackContextMenu(clickedTrack, std::clamp(timeAtMouse, m_RangeStart, m_RangeEnd), e.X, e.Y);
                e.Stop();
                return;
            }
        }
    }
    if (e.Id == kEventMouseDown && e.Button == 1 && m_OnSeekToTime && insideGrid)
    {
        m_Seeking = true;
        m_OnSeekToTime(std::clamp(timeAtMouse, m_RangeStart, m_RangeEnd));
        e.Capture(this);
        e.Stop();
        return;
    }
    if (e.Id != kEventMouseDown || e.Button != 0)
        return;
    if (W <= 0.0f || H <= 0.0f)
        return;
    size_t preHitTrack = 0;
    size_t preHitClip = 0;
    float preHitOffset = 0.0f;
    const bool preHitClipBlock = m_Model && !m_Model->tracks.empty() &&
                                 HitTestClips(m_Model, localX, trackY, 0.0f, 0.0f, W, H,
                                              m_RangeStart, m_RangeEnd, rowH, preHitTrack, preHitClip, preHitOffset);
    if ((e.Mods & Input::kModAlt) != 0 && m_OnPan && insideGrid && !preHitClipBlock)
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

    if (isDouble && insideGrid)
    {
        const size_t clickedTrack = static_cast<size_t>(trackY / rowH);
        if (m_Model && clickedTrack < m_Model->tracks.size())
        {
            const CompositeTrack& clicked = m_Model->tracks[clickedTrack];
            if (CompositeTrackTypeUsesValueKeys(clicked.type))
            {
                size_t keyIdx = static_cast<size_t>(-1);
                if (m_OnTrackValueKeyAdded)
                    keyIdx = m_OnTrackValueKeyAdded(clickedTrack, timeAtMouse);
                const float keyTime = std::max(0.0f, timeAtMouse);
                if (keyIdx != static_cast<size_t>(-1))
                {
                    BeginTrackKeyDrag(clickedTrack, CompositeTrackType::Property, MarkerDragKind::ValueKey,
                                      keyIdx, keyTime, localY, timeAtMouse, e, false);
                }
            }
            else if (CompositeTrackTypeUsesMethodKeys(clicked.type))
            {
                size_t keyIdx = static_cast<size_t>(-1);
                if (m_OnTrackMethodKeyAdded)
                    keyIdx = m_OnTrackMethodKeyAdded(clickedTrack, timeAtMouse);
                const float keyTime = std::max(0.0f, timeAtMouse);
                if (keyIdx != static_cast<size_t>(-1))
                {
                    BeginTrackKeyDrag(clickedTrack, CompositeTrackType::Method, MarkerDragKind::MethodKey,
                                      keyIdx, keyTime, localY, timeAtMouse, e, false);
                }
            }
            else if (CompositeTrackTypeUsesAudioKeys(clicked.type))
            {
                size_t keyIdx = static_cast<size_t>(-1);
                if (m_OnTrackAudioKeyAdded)
                    keyIdx = m_OnTrackAudioKeyAdded(clickedTrack, timeAtMouse);
                const float keyTime = std::max(0.0f, timeAtMouse);
                if (keyIdx != static_cast<size_t>(-1))
                {
                    BeginTrackKeyDrag(clickedTrack, CompositeTrackType::Audio, MarkerDragKind::AudioKey,
                                      keyIdx, keyTime, localY, timeAtMouse, e, false);
                }
            }
            else if (CompositeTrackTypeUsesAnimationKeys(clicked.type))
            {
                size_t keyIdx = static_cast<size_t>(-1);
                if (m_OnTrackAnimationKeyAdded)
                    keyIdx = m_OnTrackAnimationKeyAdded(clickedTrack, timeAtMouse);
                const float keyTime = std::max(0.0f, timeAtMouse);
                if (keyIdx != static_cast<size_t>(-1))
                {
                    BeginTrackKeyDrag(clickedTrack, CompositeTrackType::Animation, MarkerDragKind::AnimationKey,
                                      keyIdx, keyTime, localY, timeAtMouse, e, false);
                }
            }
            else if (preHitClipBlock)
            {
                if (m_OnClipMarkerAdded)
                {
                    const float clipStart = m_Model->tracks[preHitTrack].clips[preHitClip].offsetOnTimeline;
                    m_OnClipMarkerAdded(preHitTrack, preHitClip, std::max(0.0f, timeAtMouse - clipStart));
                }
            }
            else if (m_OnTrackMarkerAdded)
            {
                m_OnTrackMarkerAdded(clickedTrack, timeAtMouse);
            }
        }
        e.Stop();
        return;
    }

    // Marker hit-test takes priority over clip hit-test.
    {
        size_t mTrack = 0, mClip = 0, mIdx = 0;
        const auto startKeyDrag = [&](MarkerDragKind dragKind,
                                      size_t trackIdx,
                                      size_t keyIdx,
                                      float keyTime,
                                      bool immediate)
        {
            if (immediate)
                m_MarkerDragKind = dragKind;
            else
            {
                m_PendingDragKind = PendingDragKind::Marker;
                m_PendingMarkerDragKind = dragKind;
            }
            m_PendingDragStartX = localX;
            m_PendingDragStartY = localY;
            m_MarkerDragTrackIdx = trackIdx;
            m_MarkerDragIdx = keyIdx;
            m_MarkerDragStartY = localY;
            m_MarkerDragTimeOffset = timeAtMouse - keyTime;
        };
        const auto tryStartTrackKeyRefDrag = [&](const TrackKeyRef& keyRef, float hitHalfW, bool requireHorizontalHit) -> bool
        {
            if (!m_Model || keyRef.trackIdx >= m_Model->tracks.size())
                return false;
            const float rowTop = static_cast<float>(keyRef.trackIdx) * rowH;
            if (trackY < rowTop || trackY > rowTop + rowH)
                return false;

            MarkerDragKind dragKind = MarkerDragKind::None;
            float keyTime = 0.0f;
            if (!ResolveTrackKey(keyRef, dragKind, keyTime))
                return false;

            if (requireHorizontalHit)
            {
                const float keyX = ComputeInsetTimelineX(
                    keyTime, m_RangeStart, rangeDuration, 0.0f, W, GetTrackKeyEdgeInsetPx());
                if (std::abs(localX - keyX) > hitHalfW)
                    return false;
            }

            BeginTrackKeyDrag(keyRef.trackIdx, keyRef.type, dragKind, keyRef.keyIdx, keyTime, localY, timeAtMouse, e);
            m_HasArmedTrackKeyDrag = false;
            e.Stop();
            return true;
        };
        if (!ctrlHeld && !shiftHeld && m_Model)
        {
            // A key just inserted via menu/context keeps an armed ref until the next press.
            // Accept any click on that track row so layout/header offset lag cannot block drag.
            const float keyHitHalfW = GetTrackKeyHitHalfPx();
            if (m_HasArmedTrackKeyDrag && tryStartTrackKeyRefDrag(m_ArmedTrackKeyDrag, keyHitHalfW, false))
                return;
            for (const TrackKeyRef& keyRef : m_SelectedTrackKeys)
            {
                if (tryStartTrackKeyRefDrag(keyRef, keyHitHalfW, true))
                    return;
            }
        }
        if (HitTestTrackValueKey(localX, trackY, W, mTrack, mIdx))
        {
            const TrackKeyRef keyRef{mTrack, CompositeTrackType::Property, mIdx};
            if (ctrlHeld)
            {
                const auto it = std::find(m_SelectedTrackKeys.begin(), m_SelectedTrackKeys.end(), keyRef);
                if (it == m_SelectedTrackKeys.end())
                    m_SelectedTrackKeys.push_back(keyRef);
                else
                    m_SelectedTrackKeys.erase(it);
                MarkDirty(VisualDirty);
                e.Stop();
                return;
            }
            if (shiftHeld)
            {
                if (std::find(m_SelectedTrackKeys.begin(), m_SelectedTrackKeys.end(), keyRef) == m_SelectedTrackKeys.end())
                    m_SelectedTrackKeys.push_back(keyRef);
                MarkDirty(VisualDirty);
                e.Stop();
                return;
            }
            if (m_Model && mTrack < m_Model->tracks.size() && mIdx < m_Model->tracks[mTrack].valueKeys.size())
            {
                const float keyTime = m_Model->tracks[mTrack].valueKeys[mIdx].time;
                BeginTrackKeyDrag(mTrack, CompositeTrackType::Property, MarkerDragKind::ValueKey,
                                  mIdx, keyTime, localY, timeAtMouse, e);
            }
            e.Stop();
            return;
        }
        if (HitTestTrackMethodKey(localX, trackY, W, mTrack, mIdx))
        {
            const TrackKeyRef keyRef{mTrack, CompositeTrackType::Method, mIdx};
            if (ctrlHeld)
            {
                const auto it = std::find(m_SelectedTrackKeys.begin(), m_SelectedTrackKeys.end(), keyRef);
                if (it == m_SelectedTrackKeys.end())
                    m_SelectedTrackKeys.push_back(keyRef);
                else
                    m_SelectedTrackKeys.erase(it);
                MarkDirty(VisualDirty);
                e.Stop();
                return;
            }
            if (shiftHeld)
            {
                if (std::find(m_SelectedTrackKeys.begin(), m_SelectedTrackKeys.end(), keyRef) == m_SelectedTrackKeys.end())
                    m_SelectedTrackKeys.push_back(keyRef);
                MarkDirty(VisualDirty);
                e.Stop();
                return;
            }
            if (m_Model && mTrack < m_Model->tracks.size() && mIdx < m_Model->tracks[mTrack].methodKeys.size())
            {
                const float keyTime = m_Model->tracks[mTrack].methodKeys[mIdx].time;
                BeginTrackKeyDrag(mTrack, CompositeTrackType::Method, MarkerDragKind::MethodKey,
                                  mIdx, keyTime, localY, timeAtMouse, e);
            }
            e.Stop();
            return;
        }
        if (HitTestTrackAudioKey(localX, trackY, W, mTrack, mIdx))
        {
            const TrackKeyRef keyRef{mTrack, CompositeTrackType::Audio, mIdx};
            if (ctrlHeld)
            {
                const auto it = std::find(m_SelectedTrackKeys.begin(), m_SelectedTrackKeys.end(), keyRef);
                if (it == m_SelectedTrackKeys.end())
                    m_SelectedTrackKeys.push_back(keyRef);
                else
                    m_SelectedTrackKeys.erase(it);
                MarkDirty(VisualDirty);
                e.Stop();
                return;
            }
            if (shiftHeld)
            {
                if (std::find(m_SelectedTrackKeys.begin(), m_SelectedTrackKeys.end(), keyRef) == m_SelectedTrackKeys.end())
                    m_SelectedTrackKeys.push_back(keyRef);
                MarkDirty(VisualDirty);
                e.Stop();
                return;
            }
            if (m_Model && mTrack < m_Model->tracks.size() && mIdx < m_Model->tracks[mTrack].audioKeys.size())
            {
                const float keyTime = m_Model->tracks[mTrack].audioKeys[mIdx].time;
                BeginTrackKeyDrag(mTrack, CompositeTrackType::Audio, MarkerDragKind::AudioKey,
                                  mIdx, keyTime, localY, timeAtMouse, e);
            }
            e.Stop();
            return;
        }
        if (HitTestTrackAnimationKey(localX, trackY, W, mTrack, mIdx))
        {
            const TrackKeyRef keyRef{mTrack, CompositeTrackType::Animation, mIdx};
            if (ctrlHeld)
            {
                const auto it = std::find(m_SelectedTrackKeys.begin(), m_SelectedTrackKeys.end(), keyRef);
                if (it == m_SelectedTrackKeys.end())
                    m_SelectedTrackKeys.push_back(keyRef);
                else
                    m_SelectedTrackKeys.erase(it);
                MarkDirty(VisualDirty);
                e.Stop();
                return;
            }
            if (shiftHeld)
            {
                if (std::find(m_SelectedTrackKeys.begin(), m_SelectedTrackKeys.end(), keyRef) == m_SelectedTrackKeys.end())
                    m_SelectedTrackKeys.push_back(keyRef);
                MarkDirty(VisualDirty);
                e.Stop();
                return;
            }
            if (m_Model && mTrack < m_Model->tracks.size() && mIdx < m_Model->tracks[mTrack].animationKeys.size())
            {
                const float keyTime = m_Model->tracks[mTrack].animationKeys[mIdx].time;
                BeginTrackKeyDrag(mTrack, CompositeTrackType::Animation, MarkerDragKind::AnimationKey,
                                  mIdx, keyTime, localY, timeAtMouse, e);
            }
            e.Stop();
            return;
        }
        if (HitTestTrackMarker(localX, trackY, W, mTrack, mIdx))
        {
            m_PendingDragKind = PendingDragKind::Marker;
            m_PendingMarkerDragKind = MarkerDragKind::Track;
            m_PendingDragStartX = localX;
            m_PendingDragStartY = localY;
            m_MarkerDragTrackIdx = mTrack;
            m_MarkerDragIdx = mIdx;
            m_MarkerDragStartY = localY;
            if (m_Model && mTrack < m_Model->tracks.size() && mIdx < m_Model->tracks[mTrack].markers.size())
                m_MarkerDragTimeOffset = timeAtMouse - m_Model->tracks[mTrack].markers[mIdx].time;
            e.Capture(this);
            e.Stop();
            return;
        }
        if (HitTestClipMarker(localX, trackY, W, mTrack, mClip, mIdx))
        {
            m_PendingDragKind = PendingDragKind::Marker;
            m_PendingMarkerDragKind = MarkerDragKind::Clip;
            m_PendingDragStartX = localX;
            m_PendingDragStartY = localY;
            m_MarkerDragTrackIdx = mTrack;
            m_MarkerDragClipIdx = mClip;
            m_MarkerDragIdx = mIdx;
            m_MarkerDragStartY = localY;
            if (m_Model && mTrack < m_Model->tracks.size() && mClip < m_Model->tracks[mTrack].clips.size())
            {
                const float absTime = m_Model->tracks[mTrack].clips[mClip].offsetOnTimeline +
                                      m_Model->tracks[mTrack].clips[mClip].markers[mIdx].time;
                m_MarkerDragTimeOffset = timeAtMouse - absTime;
            }
            e.Capture(this);
            e.Stop();
            return;
        }
    }

    size_t hitTrack = preHitTrack;
    size_t hitClip = preHitClip;
    float hitOffset = preHitOffset;
    const bool hitClipBlock = preHitClipBlock;

    // Fade-handle hit-test: takes priority over edge resize; lives in the top band of the clip block.
    {
        size_t fhTr = 0, fhCi = 0;
        bool fhLeft = false;
        if ((m_OnClipFadeInChanged || m_OnClipFadeOutChanged) &&
            HitTestClipFadeHandle(localX, trackY, W, fhTr, fhCi, fhLeft) &&
            ((fhLeft && m_OnClipFadeInChanged) || (!fhLeft && m_OnClipFadeOutChanged)))
        {
            const std::pair<size_t,size_t> key{fhTr, fhCi};
            if (std::find(m_SelectedClips.begin(), m_SelectedClips.end(), key) == m_SelectedClips.end())
                SetSelectedClip(fhTr, fhCi);
            m_PendingDragKind = PendingDragKind::Fade;
            m_PendingDragStartX = localX;
            m_PendingDragStartY = localY;
            m_FadeDragLeft    = fhLeft;
            m_FadeDragTrackIdx = fhTr;
            m_FadeDragClipIdx  = fhCi;
            e.Capture(this);
            e.Stop();
            return;
        }
    }

    // Edge resize takes priority over body drag; right edge checked first.
    {
        size_t resTrack = 0, resClip = 0;
        float resTime = 0.0f;
        bool hitRight = m_OnClipEndChanged &&
            HitTestClipRightEdge(m_Model, localX, trackY, W, H, m_RangeStart, m_RangeEnd, rowH, resTrack, resClip, resTime);
        bool hitLeft = !hitRight && m_OnClipStartChanged &&
            HitTestClipLeftEdge(m_Model, localX, trackY, W, H, m_RangeStart, m_RangeEnd, rowH, resTrack, resClip, resTime);
        if (hitRight || hitLeft)
        {
            const std::pair<size_t,size_t> key{resTrack, resClip};
            if (std::find(m_SelectedClips.begin(), m_SelectedClips.end(), key) == m_SelectedClips.end())
                SetSelectedClip(resTrack, resClip);
            m_PendingDragKind = PendingDragKind::Resize;
            m_PendingDragStartX = localX;
            m_PendingDragStartY = localY;
            m_ResizeLeft       = hitLeft;
            m_ResizeTrackIdx   = resTrack;
            m_ResizeClipIdx    = resClip;
            m_ResizeDragOffset = timeAtMouse - resTime;
            e.Capture(this);
            e.Stop();
            return;
        }
    }

    if (hitClipBlock && m_OnClipOffsetChanged)
    {
        // Alt+drag duplicates the source clip and drags the duplicate.
        if (altHeld && !shiftHeld && !ctrlHeld && m_OnClipDuplicated)
        {
            size_t duplicatedClipIdx = hitClip;
            if (m_OnClipDuplicated(hitTrack, hitClip, duplicatedClipIdx))
                hitClip = duplicatedClipIdx;
        }
        const std::pair<size_t,size_t> clickKey{hitTrack, hitClip};
        const bool alreadySelected = std::find(m_SelectedClips.begin(), m_SelectedClips.end(), clickKey) != m_SelectedClips.end();
        if (ctrlHeld)
        {
            if (alreadySelected)
                m_SelectedClips.erase(std::remove(m_SelectedClips.begin(), m_SelectedClips.end(), clickKey), m_SelectedClips.end());
            else
                m_SelectedClips.push_back(clickKey);
            if (!m_SelectedClips.empty())
            {
                m_SelectedTrackIdx = m_SelectedClips.front().first;
                m_SelectedClipIdx = m_SelectedClips.front().second;
            }
            else
            {
                m_SelectedTrackIdx = static_cast<size_t>(-1);
                m_SelectedClipIdx = static_cast<size_t>(-1);
            }
            m_SelectedTrackKeys.clear();
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
        if (shiftHeld)
        {
            if (!alreadySelected)
                m_SelectedClips.push_back(clickKey);
            m_SelectedTrackIdx = hitTrack;
            m_SelectedClipIdx = hitClip;
            m_SelectedTrackKeys.clear();
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
        // Plain click/drag uses a single anchor clip selection so cross-track move
        // and single-clip drag semantics remain stable after multi-selection edits.
        if (!alreadySelected || m_SelectedClips.size() != 1)
        {
            SetSelectedClip(hitTrack, hitClip);
            if (m_OnClipSelected)
                m_OnClipSelected(hitTrack, hitClip);
        }
        m_PendingDragKind = PendingDragKind::Clip;
        m_PendingDragStartX = localX;
        m_PendingDragStartY = localY;
        m_DragTrackIdx = hitTrack;
        m_DragClipIdx = hitClip;
        m_DragTargetTrack = hitTrack;
        m_DragOffsetTime = timeAtMouse - hitOffset;
        m_DragClipRelOffsets.clear();
        for (auto& [tr, ci] : m_SelectedClips)
        {
            if (m_Model && tr < m_Model->tracks.size() && ci < m_Model->tracks[tr].clips.size())
                m_DragClipRelOffsets.push_back({{tr, ci}, m_Model->tracks[tr].clips[ci].offsetOnTimeline - hitOffset});
        }
        if (m_DragClipRelOffsets.empty() && m_Model &&
            m_DragTrackIdx < m_Model->tracks.size() &&
            m_DragClipIdx < m_Model->tracks[m_DragTrackIdx].clips.size())
        {
            // Ensure a single-clip drag always has an anchor payload.
            m_DragClipRelOffsets.push_back({{m_DragTrackIdx, m_DragClipIdx}, 0.0f});
        }
        e.Capture(this);
    }
    else if (insideGrid)
    {
        m_PendingEmptyAction = true;
        m_EmptyActionShiftHeld = shiftHeld;
        m_EmptyActionStartX = localX;
        m_EmptyActionStartY = trackY;
        m_BoxStartX = localX;
        m_BoxStartY = trackY;
        m_BoxEndX   = localX;
        m_BoxEndY   = trackY;
        const bool additiveSeed = shiftHeld || ctrlHeld;
        m_BoxSelectionSeed = additiveSeed ? m_SelectedClips : std::vector<std::pair<size_t,size_t>>{};
        m_BoxKeySelectionSeed = additiveSeed ? m_SelectedTrackKeys : std::vector<TrackKeyRef>{};
        e.Capture(this);
        if (!additiveSeed)
            ClearSelection();
    }
    e.Stop();
}

void TimeCompositeView::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
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
    const int numTracks = m_Model && !m_Model->tracks.empty()
                              ? static_cast<int>(m_Model->tracks.size())
                              : 0;
    const float rowH = std::max(1.0f, m_RowHeight > 0.0f ? m_RowHeight : kTrackRowHeightPx) * cs;
    const uint32_t lineColor = PackColor(0.35f, 0.35f, 0.35f, 0.10f);

    if (numTracks > 0)
    {
        for (int row = 0; row <= numTracks; ++row)
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

    if (m_Model && !m_Model->tracks.empty())
    {
        const float rowGap = std::min(rowH - 1.0f * cs, std::max(0.0f, kTrackRowGapPx * cs));
        const float rowGapHalf = rowGap * 0.5f;
        const float rowBodyH = std::max(1.0f, rowH - rowGap);
        for (size_t tr = 0; tr < m_Model->tracks.size(); ++tr)
        {
            const CompositeTrack& track = m_Model->tracks[tr];
            const float trackTop = ty + static_cast<float>(tr) * rowH + rowGapHalf;
            // User-picked track tint, drawn under any selection overlay so the active row
            // still reads as selected. track.color is engine-ARGB (0xAARRGGBB);
            // primitives use RGBA layout so we route through PackFromARGB.
            if (track.color != 0u)
                ctx.Emit(MakeRect(x, trackTop, W, rowBodyH, PackFromARGB(track.color)));
            const bool trackRowSelected =
                m_SelectedTrackIdx == tr && m_SelectedTrackKeys.empty();
            if (trackRowSelected)
                ctx.Emit(MakeRect(x, trackTop, W, rowBodyH, PackColor(1.0f, 0.55f, 0.10f, 0.10f)));
            float blockH = std::min(rowBodyH * 0.8f, kMaxClipBlockHeightPx * cs);

            for (size_t clipIndex = 0; clipIndex < track.clips.size(); ++clipIndex)
            {
                const CompositeClip& clip = track.clips[clipIndex];
                const std::pair<size_t,size_t> clipKey{tr, clipIndex};
                const bool selected = std::find(m_SelectedClips.begin(), m_SelectedClips.end(), clipKey) != m_SelectedClips.end();
                const bool movingToTrack = m_DraggingClip &&
                                           m_DragTargetTrack != m_DragTrackIdx &&
                                           m_DragClipRelOffsets.size() == 1 &&
                                           m_OnClipMovedToTrack &&
                                           selected &&
                                           tr == m_DragTrackIdx &&
                                           clipIndex == m_DragClipIdx &&
                                           (m_DragTargetTrack == m_Model->tracks.size() ||
                                            (m_DragTargetTrack < m_Model->tracks.size() &&
                                             CompositeTrackTypeUsesClips(m_Model->tracks[m_DragTargetTrack].type)));
                const size_t drawTrack = movingToTrack ? m_DragTargetTrack : tr;
                const float drawTrackTop = ty + static_cast<float>(drawTrack) * rowH + rowGapHalf;
                const float drawRowY = drawTrackTop + (rowBodyH - blockH) * 0.5f;
                float startPx = x + (clip.offsetOnTimeline - m_RangeStart) / rangeDuration * W;
                float duration = clip.outTime - clip.inTime;
                float blockW = std::max(4.0f, duration / rangeDuration * W);
                float blockLeft = std::max(x, startPx);
                float blockRight = std::min(x + W, startPx + blockW);
                if (blockRight > blockLeft)
                {
                    const float bl = blockLeft;
                    const float bw = blockRight - blockLeft;
                    ctx.Emit(MakeRect(bl, drawRowY, bw, blockH,
                                      selected ? kSelectedClipBlockColor : (clip.muted ? PackColor(0.3f, 0.3f, 0.35f, 0.7f) : kClipBlockColor)));
                    EmitClipNameLeftInsideBlock(ctx, style, bl, drawRowY, bw, blockH, clip.name, cs);
                    // Resize handle stripes at left and right edges.
                    const float handleW    = std::min(5.0f * cs, bw * 0.35f);
                    const bool rightHovered = m_HoveringResize && !m_HoverResizeLeft && m_HoverResizeTrackIdx == tr && m_HoverResizeClipIdx == clipIndex;
                    const bool leftHovered  = m_HoveringResize &&  m_HoverResizeLeft && m_HoverResizeTrackIdx == tr && m_HoverResizeClipIdx == clipIndex;
                    ctx.Emit(MakeRect(bl,                drawRowY, handleW, blockH,
                                      leftHovered  ? PackColor(1.0f, 1.0f, 1.0f, 0.65f) : PackColor(0.0f, 0.0f, 0.0f, 0.28f)));
                    ctx.Emit(MakeRect(bl + bw - handleW, drawRowY, handleW, blockH,
                                      rightHovered ? PackColor(1.0f, 1.0f, 1.0f, 0.65f) : PackColor(0.0f, 0.0f, 0.0f, 0.28f)));

                    // Fade ramps + handle markers. Fade widths are clamped to the clip block extents
                    // (which are themselves clipped to the visible time range).
                    {
                        const float fullBlockLeft  = startPx;
                        const float fullBlockRight = startPx + blockW;
                        const float fullBlockW     = std::max(0.0f, fullBlockRight - fullBlockLeft);
                        const float fadeInPx  = std::clamp(clip.fadeInDuration  / rangeDuration * W, 0.0f, fullBlockW);
                        const float fadeOutPx = std::clamp(clip.fadeOutDuration / rangeDuration * W, 0.0f, fullBlockW);
                        const uint32_t fadeFill   = PackColor(0.0f, 0.0f, 0.0f, 0.45f);
                        const uint32_t handleIdle = PackColor(1.0f, 0.85f, 0.30f, 0.95f);
                        const uint32_t handleHot  = PackColor(1.0f, 1.0f, 1.0f, 1.0f);
                        // Fade-in: darkened wedge in the upper-left, gain ramps 0→1 from clip start to (start + fadeIn).
                        if (fadeInPx > 0.5f)
                        {
                            const float x0 = fullBlockLeft;
                            const float x1 = fullBlockLeft + fadeInPx;
                            const float yTop = drawRowY;
                            const float yBot = drawRowY + blockH;
                            // Triangle vertices clipped to the visible block strip via std::max/std::min on x.
                            const float vx0 = std::max(x0, blockLeft);
                            const float vx1 = std::min(x1, blockRight);
                            if (vx1 > vx0)
                                ctx.Emit(MakeTriangle(std::max(x0, blockLeft), yTop,
                                                       std::min(x1, blockRight), yTop,
                                                       std::max(x0, blockLeft), yBot,
                                                       fadeFill));
                        }
                        // Fade-out: darkened wedge in the upper-right.
                        if (fadeOutPx > 0.5f)
                        {
                            const float x0 = fullBlockRight - fadeOutPx;
                            const float x1 = fullBlockRight;
                            const float yTop = drawRowY;
                            const float yBot = drawRowY + blockH;
                            const float vx0 = std::max(x0, blockLeft);
                            const float vx1 = std::min(x1, blockRight);
                            if (vx1 > vx0)
                                ctx.Emit(MakeTriangle(std::max(x0, blockLeft), yTop,
                                                       std::min(x1, blockRight), yTop,
                                                       std::min(x1, blockRight), yBot,
                                                       fadeFill));
                        }
                        // Fade tip handle markers (small downward triangles). Always shown so a fade
                        // can be started from a 0-duration corner; sit in the top band of the block.
                        const float hs = kFadeHandleVisualSize * cs;
                        const float leftTipX  = fullBlockLeft + fadeInPx;
                        const float rightTipX = fullBlockRight - fadeOutPx;
                        const bool leftFadeHover  = m_HoveringFade &&  m_HoverFadeLeft && m_HoverFadeTrackIdx == tr && m_HoverFadeClipIdx == clipIndex;
                        const bool rightFadeHover = m_HoveringFade && !m_HoverFadeLeft && m_HoverFadeTrackIdx == tr && m_HoverFadeClipIdx == clipIndex;
                        if (leftTipX >= blockLeft - hs && leftTipX <= blockRight + hs)
                        {
                            ctx.Emit(MakeTriangle(leftTipX - hs * 0.5f, drawRowY,
                                                   leftTipX + hs * 0.5f, drawRowY,
                                                   leftTipX,            drawRowY + hs,
                                                   leftFadeHover ? handleHot : handleIdle));
                        }
                        if (rightTipX >= blockLeft - hs && rightTipX <= blockRight + hs)
                        {
                            ctx.Emit(MakeTriangle(rightTipX - hs * 0.5f, drawRowY,
                                                   rightTipX + hs * 0.5f, drawRowY,
                                                   rightTipX,            drawRowY + hs,
                                                   rightFadeHover ? handleHot : handleIdle));
                        }
                    }

                    // Clip markers (relative to clip start).
                    for (size_t mi = 0; mi < clip.markers.size(); ++mi)
                    {
                        const TimelineMarker& m = clip.markers[mi];
                        const float absT = clip.offsetOnTimeline + m.time;
                        const float mx = x + (absT - m_RangeStart) / rangeDuration * W;
                        if (mx < blockLeft || mx > blockRight) continue;
                        const bool hov = m_HoveringMarker && m_HoverMarkerIsClip &&
                                         m_HoverMarkerTrackIdx == tr && m_HoverMarkerClipIdx == clipIndex && m_HoverMarkerIdx == mi;
                        ctx.Emit(MakeRect(mx - kMarkerLineW * cs * 0.5f, drawRowY, kMarkerLineW * cs, blockH,
                                          hov ? PackColor(1.0f, 1.0f, 1.0f, 1.0f) : PackColor(1.0f, 1.0f, 0.6f, 0.90f)));
                        ctx.Emit(MakeRect(mx - kMarkerCapW * cs * 0.5f, drawRowY, kMarkerCapW * cs, kMarkerCapH * cs,
                                          hov ? PackColor(1.0f, 1.0f, 1.0f, 1.0f) : PackColor(1.0f, 0.90f, 0.30f, 1.0f)));
                        if (!m.name.empty())
                            EmitClipNameLeftInsideBlock(ctx, style, mx + kMarkerCapW * cs * 0.6f, drawRowY, 60.0f * cs, kMarkerCapH * cs, m.name, cs);
                    }
                }
            }

            // Track markers (absolute time, drawn above clips).
            const float trackCenterY = trackTop + rowBodyH * 0.5f;
            const uint32_t keyLinkColor = PackColor(0.82f, 0.82f, 0.82f, 0.42f);
            const float keyInset = kTrackKeyEdgeInsetPx * cs;
            const float keyX = x + keyInset;
            const float keyW = std::max(1.0f, W - keyInset * 2.0f);
            if (CompositeTrackTypeUsesValueKeys(track.type))
                EmitGodotStyleKeyLinks(ctx, track.valueKeys, SameCompositeValueKey,
                                       m_RangeStart, rangeDuration, keyX, keyW, trackCenterY, cs, keyLinkColor);
            if (CompositeTrackTypeUsesAudioKeys(track.type))
                EmitGodotStyleKeyLinks(ctx, track.audioKeys, SameCompositeAudioKey,
                                       m_RangeStart, rangeDuration, keyX, keyW, trackCenterY, cs, keyLinkColor);
            if (CompositeTrackTypeUsesAnimationKeys(track.type))
                EmitGodotStyleKeyLinks(ctx, track.animationKeys, SameCompositeAnimationKey,
                                       m_RangeStart, rangeDuration, keyX, keyW, trackCenterY, cs, keyLinkColor);
            for (size_t ki = 0; ki < track.valueKeys.size(); ++ki)
            {
                const CompositeValueKey& key = track.valueKeys[ki];
                const float kx = ComputeInsetTimelineX(
                    key.time, m_RangeStart, rangeDuration, x, W, keyInset);
                if (kx < x || kx > x + W) continue;
                const bool selectedKey = std::find(m_SelectedTrackKeys.begin(), m_SelectedTrackKeys.end(),
                                                   TrackKeyRef{tr, CompositeTrackType::Property, ki}) != m_SelectedTrackKeys.end();
                const bool dragHov = m_MarkerDragKind == MarkerDragKind::ValueKey &&
                                     m_MarkerDragTrackIdx == tr && m_MarkerDragIdx == ki;
                const bool pointerHov = m_HoveringMarker && m_HoverMarkerKind == MarkerDragKind::ValueKey &&
                                        m_HoverMarkerTrackIdx == tr && m_HoverMarkerIdx == ki;
                const bool hov = dragHov || pointerHov || selectedKey;
                const float s = (hov ? 7.0f : 6.0f) * cs;
                const uint32_t fill = hov ? PackColor(1.0f, 1.0f, 1.0f, 1.0f) : kValueKeyColor;
                EmitTrackKeyDiamond(ctx, kx, trackCenterY, s, fill, hov);
                const std::string label = !key.label.empty() ? key.label : std::string{};
                if (!label.empty())
                    EmitTrackKeyLabel(ctx, style, kx + 8.0f * cs, trackTop + 2.0f * cs,
                                      80.0f * cs, rowBodyH - 4.0f * cs, label, cs, hov);
            }
            for (size_t ki = 0; ki < track.methodKeys.size(); ++ki)
            {
                const CompositeMethodKey& key = track.methodKeys[ki];
                const float kx = ComputeInsetTimelineX(
                    key.time, m_RangeStart, rangeDuration, x, W, keyInset);
                if (kx < x || kx > x + W) continue;
                const bool selectedKey = std::find(m_SelectedTrackKeys.begin(), m_SelectedTrackKeys.end(),
                                                   TrackKeyRef{tr, CompositeTrackType::Method, ki}) != m_SelectedTrackKeys.end();
                const bool dragHov = m_MarkerDragKind == MarkerDragKind::MethodKey &&
                                     m_MarkerDragTrackIdx == tr && m_MarkerDragIdx == ki;
                const bool pointerHov = m_HoveringMarker && m_HoverMarkerKind == MarkerDragKind::MethodKey &&
                                        m_HoverMarkerTrackIdx == tr && m_HoverMarkerIdx == ki;
                const bool hov = dragHov || pointerHov || selectedKey;
                const float s = (hov ? 7.0f : 6.0f) * cs;
                const uint32_t fill = hov ? PackColor(1.0f, 1.0f, 1.0f, 1.0f) : kMethodKeyColor;
                EmitTrackKeyDiamond(ctx, kx, trackCenterY, s, fill, hov);
                const std::string label = key.methodName.empty() ? std::string("Method") : key.methodName;
                EmitTrackKeyLabel(ctx, style, kx + 8.0f * cs, trackTop + 2.0f * cs,
                                  80.0f * cs, rowBodyH - 4.0f * cs, label, cs, hov);
            }
            for (size_t ki = 0; ki < track.audioKeys.size(); ++ki)
            {
                const CompositeAudioKey& key = track.audioKeys[ki];
                const float kx = ComputeInsetTimelineX(
                    key.time, m_RangeStart, rangeDuration, x, W, keyInset);
                if (kx < x || kx > x + W) continue;
                const bool selectedKey = std::find(m_SelectedTrackKeys.begin(), m_SelectedTrackKeys.end(),
                                                   TrackKeyRef{tr, CompositeTrackType::Audio, ki}) != m_SelectedTrackKeys.end();
                const bool dragHov = m_MarkerDragKind == MarkerDragKind::AudioKey &&
                                     m_MarkerDragTrackIdx == tr && m_MarkerDragIdx == ki;
                const bool pointerHov = m_HoveringMarker && m_HoverMarkerKind == MarkerDragKind::AudioKey &&
                                        m_HoverMarkerTrackIdx == tr && m_HoverMarkerIdx == ki;
                const bool hov = dragHov || pointerHov || selectedKey;
                const float s = (hov ? 7.0f : 6.0f) * cs;
                const uint32_t fill = hov ? PackColor(1.0f, 1.0f, 1.0f, 1.0f) : kAudioKeyColor;
                EmitTrackKeyDiamond(ctx, kx, trackCenterY, s, fill, hov);
                const std::string label = key.name.empty() ? std::string("Audio") : key.name;
                EmitTrackKeyLabel(ctx, style, kx + 8.0f * cs, trackTop + 2.0f * cs,
                                  90.0f * cs, rowBodyH - 4.0f * cs, label, cs, hov);
            }
            for (size_t ki = 0; ki < track.animationKeys.size(); ++ki)
            {
                const CompositeAnimationKey& key = track.animationKeys[ki];
                const float kx = ComputeInsetTimelineX(
                    key.time, m_RangeStart, rangeDuration, x, W, keyInset);
                if (kx < x || kx > x + W) continue;
                const bool selectedKey = std::find(m_SelectedTrackKeys.begin(), m_SelectedTrackKeys.end(),
                                                   TrackKeyRef{tr, CompositeTrackType::Animation, ki}) != m_SelectedTrackKeys.end();
                const bool dragHov = m_MarkerDragKind == MarkerDragKind::AnimationKey &&
                                     m_MarkerDragTrackIdx == tr && m_MarkerDragIdx == ki;
                const bool pointerHov = m_HoveringMarker && m_HoverMarkerKind == MarkerDragKind::AnimationKey &&
                                        m_HoverMarkerTrackIdx == tr && m_HoverMarkerIdx == ki;
                const bool hov = dragHov || pointerHov || selectedKey;
                const float s = (hov ? 7.0f : 6.0f) * cs;
                const uint32_t fill = hov ? PackColor(1.0f, 1.0f, 1.0f, 1.0f) : kAnimationKeyColor;
                EmitTrackKeyDiamond(ctx, kx, trackCenterY, s, fill, hov);
                const std::string label = key.animationName.empty() ? std::string("Animation") : key.animationName;
                EmitTrackKeyLabel(ctx, style, kx + 8.0f * cs, trackTop + 2.0f * cs,
                                  100.0f * cs, rowBodyH - 4.0f * cs, label, cs, hov);
            }
            for (size_t mi = 0; mi < track.markers.size(); ++mi)
            {
                const TimelineMarker& m = track.markers[mi];
                const float mx = x + (m.time - m_RangeStart) / rangeDuration * W;
                if (mx < x || mx > x + W) continue;
                const bool hov = m_HoveringMarker && !m_HoverMarkerIsClip &&
                                 m_HoverMarkerTrackIdx == tr && m_HoverMarkerIdx == mi;
                ctx.Emit(MakeRect(mx - kMarkerLineW * cs * 0.5f, trackTop, kMarkerLineW * cs, rowBodyH,
                                  hov ? PackColor(1.0f, 1.0f, 0.4f, 1.0f) : PackColor(0.95f, 0.75f, 0.15f, 0.85f)));
                ctx.Emit(MakeRect(mx - kMarkerCapW * cs * 0.5f, trackTop, kMarkerCapW * cs, kMarkerCapH * cs,
                                  hov ? PackColor(1.0f, 1.0f, 0.5f, 1.0f) : PackColor(1.0f, 0.85f, 0.20f, 1.0f)));
                if (!m.name.empty())
                    EmitClipNameLeftInsideBlock(ctx, style, mx + kMarkerCapW * cs * 0.6f, trackTop, 60.0f * cs, kMarkerCapH * cs, m.name, cs);
            }
        }
    }
    // No fallback clip rendered when there are no tracks — show an empty canvas.

    if (W > 0.0f && m_CurrentTime >= m_RangeStart && m_CurrentTime <= m_RangeEnd)
    {
        float playheadPx = x + (m_CurrentTime - m_RangeStart) / rangeDuration * W;
        playheadPx = std::max(x, std::min(x + W, playheadPx));
        ctx.Emit(MakeRect(std::max(x, playheadPx - 1.0f * cs), y, 2.0f * cs, H, PackColor(1.0f, 0.4f, 0.0f, 0.95f)));
    }

    if (m_DropPreviewVisible)
    {
        const uint32_t dropColor = m_DropPreviewAllowed
                                       ? PackColor(0.3f, 0.9f, 0.4f, 0.25f)
                                       : PackColor(0.9f, 0.3f, 0.3f, 0.20f);
        const uint32_t dropLineColor = m_DropPreviewAllowed
                                          ? PackColor(0.3f, 0.9f, 0.4f, 0.85f)
                                          : PackColor(0.9f, 0.3f, 0.3f, 0.70f);
        const float dropX = std::clamp(m_DropLastHoverX, x, x + W);
        const float rowH2 = (m_RowHeight > 0.0f ? m_RowHeight : kTrackRowHeightPx) * cs;
        const float rowY = ty + static_cast<float>(m_DropPreviewTrack) * rowH2;
        const float blockH = std::min(rowH2 * 0.8f, kMaxClipBlockHeightPx * cs);
        ctx.Emit(MakeRect(x, rowY, W, blockH, dropColor));
        ctx.Emit(MakeRect(dropX - 1.0f * cs, y, 2.0f * cs, H, dropLineColor));
    }

    if (m_BoxSelecting)
    {
        const float bx0 = x + std::min(m_BoxStartX, m_BoxEndX) * cs;
        const float bx1 = x + std::max(m_BoxStartX, m_BoxEndX) * cs;
        const float by0 = ty + std::min(m_BoxStartY, m_BoxEndY) * cs;
        const float by1 = ty + std::max(m_BoxStartY, m_BoxEndY) * cs;
        const float bw  = bx1 - bx0;
        const float bh  = by1 - by0;
        ctx.Emit(MakeRect(bx0, by0, bw, bh, PackColor(0.4f, 0.6f, 1.0f, 0.15f)));
        ctx.Emit(MakeRect(bx0,              by0,              bw,          1.0f * cs, PackColor(0.5f, 0.7f, 1.0f, 0.8f)));
        ctx.Emit(MakeRect(bx0,              by1 - 1.0f * cs, bw,          1.0f * cs, PackColor(0.5f, 0.7f, 1.0f, 0.8f)));
        ctx.Emit(MakeRect(bx0,              by0,              1.0f * cs,   bh,        PackColor(0.5f, 0.7f, 1.0f, 0.8f)));
        ctx.Emit(MakeRect(bx1 - 1.0f * cs, by0,              1.0f * cs,   bh,        PackColor(0.5f, 0.7f, 1.0f, 0.8f)));
    }
}

bool TimeCompositeView::AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const
{
    return typeId == UI::Interaction::GetPayloadTypeId<Editor::AssetPathsDragPayload>();
}

bool TimeCompositeView::HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const
{
    const float lx = GetLayoutX();
    const float ly = GetLayoutY();
    const float lw = GetLayoutWidth();
    const float lh = GetLayoutHeight();
    if (lw <= 0.0f || lh <= 0.0f)
        return false;
    if (x < lx || x > lx + lw || y < ly || y > ly + lh)
        return false;
    m_DropLastHoverX = x;
    const float rowH = m_RowHeight > 0.0f ? m_RowHeight : kTrackRowHeightPx;
    const float relY = y - ly - m_HeaderOffset;
    if (relY < 0.0f)
        return false;
    const float rowF = relY / std::max(1.0f, rowH);
    const size_t baseRow = static_cast<size_t>(std::floor(rowF));
    const float rowFrac = rowF - static_cast<float>(baseRow);
    // Top half targets the existing row; bottom half targets insertion after it.
    const size_t trackRow = baseRow + (rowFrac >= 0.5f ? 1u : 0u);
    out.TargetId = static_cast<UI::Interaction::ItemId>(trackRow);
    out.Location = UI::Interaction::DropLocation::OnEmptySpace;
    out.IndentDepth = 0;
    return true;
}

static bool HasTimelineClipFile(const std::vector<std::filesystem::path>& paths)
{
    for (const auto& p : paths)
    {
        const std::string ext = p.extension().string();
        const AssetType type = GetAssetTypeFromExtension(ext);
        if (type == AssetType::Audio || type == AssetType::Video || type == AssetType::Animation)
            return true;
    }
    return false;
}

static bool TrackTypeAcceptsAsset(CompositeTrackType trackType, AssetType assetType)
{
    if (trackType == CompositeTrackType::Audio)
        return assetType == AssetType::Audio;
    if (trackType == CompositeTrackType::Animation)
        return assetType == AssetType::Animation;
    if (trackType == CompositeTrackType::Video)
        return assetType == AssetType::Video;
    return false;
}

UI::Interaction::DropFeedback TimeCompositeView::CanDrop(const UI::Interaction::DropRequest& request) const
{
    const auto* payload = request.payload.TryGet<Editor::AssetPathsDragPayload>();
    if (!payload)
        return {false, "Not an asset drag"};
    if (!HasTimelineClipFile(payload->paths))
        return {false, "No audio, animation, or video files"};
    if (m_Model && request.hit.TargetId < m_Model->tracks.size())
    {
        const CompositeTrackType trackType = m_Model->tracks[static_cast<size_t>(request.hit.TargetId)].type;
        for (const auto& p : payload->paths)
        {
            const AssetType type = GetAssetTypeFromExtension(p.extension().string());
            if (!TrackTypeAcceptsAsset(trackType, type))
                return {false, "Asset type does not match this track"};
        }
    }
    return {true, {}};
}

void TimeCompositeView::PerformDrop(const UI::Interaction::DropRequest& request)
{
    const auto* payload = request.payload.TryGet<Editor::AssetPathsDragPayload>();
    if (!payload || !m_OnMediaDropped)
        return;

    std::vector<std::filesystem::path> mediaPaths;
    for (const auto& p : payload->paths)
    {
        const AssetType type = GetAssetTypeFromExtension(p.extension().string());
        if (type == AssetType::Audio || type == AssetType::Video || type == AssetType::Animation)
            mediaPaths.push_back(p);
    }
    if (mediaPaths.empty())
        return;

    const float lx = GetLayoutX();
    const float lw = GetLayoutWidth();
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    float timeOffset = m_RangeStart;
    if (lw > 0.0f)
        timeOffset = m_RangeStart + std::clamp((m_DropLastHoverX - lx) / lw, 0.0f, 1.0f) * rangeDuration;

    const size_t trackHint = static_cast<size_t>(request.hit.TargetId);
    m_OnMediaDropped(trackHint, timeOffset, mediaPaths);
}

void TimeCompositeView::SetDropPreview(const UI::Interaction::DropPreviewState& state)
{
    m_DropPreviewVisible = state.Visible;
    m_DropPreviewAllowed = state.Allowed;
    if (state.Visible)
        m_DropPreviewTrack = static_cast<size_t>(state.Hit.TargetId);
    MarkDirty(VisualDirty);
}

} // namespace GameEngine

namespace RegisterAnimationWindow
{
static auto s_reg_timeComposite =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::TimeCompositeView>(
        "TimeCompositeView",
        []() { return std::make_unique<GameEngine::TimeCompositeView>(); })
        .TagAlias("timecompositeview");
}
