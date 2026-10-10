#include "Panels/CurvesGraphView.h"

#include "Panels/Animation/KeyTimeMatch.h"

#include "Assets/AnimationClip.h"
#include <cstdio>
#include <cmath>
#include <limits>
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIEvents.h"
#include "UI/UiContext.h"
#include "UI/UIPrimitive.h"
#include "Rendering/Geometry/ShapeBuilder.h"

namespace GameEngine
{

namespace
{
constexpr float kSelectionHandleHalfSizePx = 7.5f;
constexpr float kBoxSelectThresholdSq = 16.0f;

float GetChannelComponentValue(const AnimChannel& channel, const AnimKeyframe& keyframe, uint32 component)
{
    switch (channel.path)
    {
    case AnimPath::Translation:
        return keyframe.translation[std::min<uint32>(component, 2u)];
    case AnimPath::Scale:
        return keyframe.scale[std::min<uint32>(component, 2u)];
    case AnimPath::Rotation:
        return keyframe.rotation[std::min<uint32>(component, 3u)];
    case AnimPath::MorphWeight:
        return keyframe.translation[0];
    }

    return 0.0f;
}

uint32 GetComponentCount(const AnimChannel& channel)
{
    if (channel.path == AnimPath::MorphWeight)
        return 1u;
    return channel.path == AnimPath::Rotation ? 4u : 3u;
}

uint32 GetComponentColor(uint32 component, bool emphasized)
{
    switch (component)
    {
    case 0:
        return emphasized ? UI::PackColor(0.95f, 0.35f, 0.35f, 0.95f) : UI::PackColor(0.95f, 0.35f, 0.35f, 0.70f);
    case 1:
        return emphasized ? UI::PackColor(0.35f, 0.85f, 0.40f, 0.95f) : UI::PackColor(0.35f, 0.85f, 0.40f, 0.70f);
    case 2:
        return emphasized ? UI::PackColor(0.35f, 0.60f, 0.95f, 0.95f) : UI::PackColor(0.35f, 0.60f, 0.95f, 0.70f);
    case 3:
        return emphasized ? UI::PackColor(0.95f, 0.80f, 0.35f, 0.95f) : UI::PackColor(0.95f, 0.80f, 0.35f, 0.70f);
    default:
        return emphasized ? UI::PackColor(0.85f, 0.85f, 0.85f, 0.95f) : UI::PackColor(0.85f, 0.85f, 0.85f, 0.70f);
    }
}

// Pinned reference curves are drawn dim and neutral so they read as context
// rather than as one of the editable component colors.
const uint32 kPinnedReferenceCurveColor = UI::PackColor(0.55f, 0.55f, 0.55f, 0.65f);

constexpr float kMinHandleWeight = 0.05f;

float ClampHandleWeight(float weight)
{
    return std::max(kMinHandleWeight, weight);
}

struct TangentHandlePoint
{
    bool Valid = false;
    float Time = 0.0f;
    float Value = 0.0f;
};

bool ApplyHandlePointToCurve(int channelIndex,
                             const AnimChannel& channel,
                             uint32 component,
                             size_t keyIndex,
                             bool incoming,
                             float targetTime,
                             float targetValue,
                             const CurvesGraphView::TangentEditedFn& onTangentEdited)
{
    if (!onTangentEdited || keyIndex >= channel.keys.size())
        return false;

    const size_t neighborIndex = incoming ? (keyIndex - 1u) : (keyIndex + 1u);
    if ((incoming && keyIndex == 0u) || neighborIndex >= channel.keys.size())
        return false;

    const AnimKeyframe& keyframe = channel.keys[keyIndex];
    const float keyValue = GetChannelComponentValue(channel, keyframe, component);
    const float segmentDt = incoming
        ? std::max(0.0001f, keyframe.time - channel.keys[neighborIndex].time)
        : std::max(0.0001f, channel.keys[neighborIndex].time - keyframe.time);
    const float rawHandleDt = incoming ? (keyframe.time - targetTime) : (targetTime - keyframe.time);
    const float handleDt = std::clamp(rawHandleDt, segmentDt * kMinHandleWeight, segmentDt);
    const float tangent = incoming
        ? (keyValue - targetValue) / handleDt
        : (targetValue - keyValue) / handleDt;
    onTangentEdited(channelIndex,
                    static_cast<int>(component),
                    keyframe.time,
                    incoming,
                    tangent,
                    handleDt / segmentDt);
    return true;
}

TangentHandlePoint GetTangentHandlePoint(const AnimChannel& channel, uint32 component, size_t keyIndex, bool incoming)
{
    if (keyIndex >= channel.keys.size())
        return {};

    const AnimKeyframe& keyframe = channel.keys[keyIndex];
    const float keyValue = GetChannelComponentValue(channel, keyframe, component);
    if (incoming)
    {
        if (keyIndex == 0)
            return {};

        const float segmentDt = std::max(0.0001f, keyframe.time - channel.keys[keyIndex - 1u].time);
        const float handleDt = segmentDt * ClampHandleWeight(keyframe.inWeight[component]);
        return {true, keyframe.time - handleDt, keyValue - keyframe.inTangent[component] * handleDt};
    }

    if (keyIndex + 1u >= channel.keys.size())
        return {};

    const float segmentDt = std::max(0.0001f, channel.keys[keyIndex + 1u].time - keyframe.time);
    const float handleDt = segmentDt * ClampHandleWeight(keyframe.outWeight[component]);
    return {true, keyframe.time + handleDt, keyValue + keyframe.outTangent[component] * handleDt};
}

/// Linear sampler at arbitrary time across a channel's keys (clamped at the
/// ends). Used to seed lattice control-point values along the actual curve so
/// the rest lattice rides on the curve and zero-displacement = no change.
float SampleChannelLinear(const AnimChannel& ch, uint32 comp, float t)
{
    if (ch.keys.empty()) return 0.0f;
    if (t <= ch.keys.front().time) return GetChannelComponentValue(ch, ch.keys.front(), comp);
    if (t >= ch.keys.back().time)  return GetChannelComponentValue(ch, ch.keys.back(),  comp);
    for (size_t i = 0; i + 1 < ch.keys.size(); ++i)
    {
        const float t0 = ch.keys[i].time;
        const float t1 = ch.keys[i + 1].time;
        if (t >= t0 && t <= t1)
        {
            const float u = (t - t0) / std::max(1e-6f, t1 - t0);
            const float v0 = GetChannelComponentValue(ch, ch.keys[i],     comp);
            const float v1 = GetChannelComponentValue(ch, ch.keys[i + 1], comp);
            return v0 + (v1 - v0) * u;
        }
    }
    return GetChannelComponentValue(ch, ch.keys.back(), comp);
}

/// Bezier basis evaluation (Bernstein polynomials, De Casteljau algorithm)
/// over an N-point control polyline. Returns the value at parameter `u` in
/// [0..1] given per-CP values (or per-CP deltas — caller chooses what's in
/// `cps`). The curve interpolates the first and last CPs but treats interior
/// CPs as control handles that bow the curve smoothly. For N==3 this is the
/// classic quadratic Bezier the lattice tool started with: dragging any CP
/// visibly bends the deformation curve at every key, not only at keys whose
/// times happen to coincide with the CP. Stable for the N<=16 range the
/// lattice clamps to. For N==2 this degenerates to a lerp.
float EvaluateBezierBasis(const float* cps, size_t n, float u)
{
    if (n == 0) return 0.0f;
    if (n == 1) return cps[0];
    if (u <= 0.0f) return cps[0];
    if (u >= 1.0f) return cps[n - 1];

    // De Casteljau iteratively reduces the control polygon. Stack-allocate
    // a small working buffer to avoid heap traffic on every move event.
    std::array<float, 16> work{};
    const size_t cap = std::min(n, work.size());
    for (size_t i = 0; i < cap; ++i) work[i] = cps[i];
    const float omu = 1.0f - u;
    for (size_t k = 1; k < cap; ++k)
        for (size_t i = 0; i + k < cap; ++i)
            work[i] = omu * work[i] + u * work[i + 1];
    return work[0];
}

/// Catmull-Rom basis evaluation across an N-point control polyline at an
/// arbitrary time `t`. Unlike the Bezier basis above, this one *interpolates*
/// every CP (including interior ones), so the curve passes through each
/// handle exactly. Trade-off: keys whose times match a CP's time take that
/// CP's value verbatim, so dragging a different CP leaves them un-moved.
/// Endpoints are mirror-extended so curvature is natural at the edges.
/// `cpTimes` and `cps` must have `n` entries each; for N==2 this is a lerp.
float EvaluateCatmullRomBasis(const float* cpTimes,
                               const float* cps,
                               size_t n,
                               float t)
{
    if (n == 0) return 0.0f;
    if (n == 1) return cps[0];
    if (t <= cpTimes[0])     return cps[0];
    if (t >= cpTimes[n - 1]) return cps[n - 1];

    size_t i = 0;
    while (i + 1 < n - 1 && t > cpTimes[i + 1]) ++i;
    const float t0 = cpTimes[i];
    const float t1 = cpTimes[i + 1];
    const float u  = (t - t0) / std::max(1e-6f, t1 - t0);

    if (n == 2)
        return cps[0] + (cps[1] - cps[0]) * u;

    const float dPrev = (i == 0)     ? (2.0f * cps[0]   - cps[1])     : cps[i - 1];
    const float dNext = (i + 2 >= n) ? (2.0f * cps[n-1] - cps[n - 2]) : cps[i + 2];
    const float dA    = cps[i];
    const float dB    = cps[i + 1];

    const float u2 = u * u;
    const float u3 = u2 * u;
    const float wPrev = 0.5f * (-u + 2.0f * u2 - u3);
    const float wA    = 0.5f * (2.0f - 5.0f * u2 + 3.0f * u3);
    const float wB    = 0.5f * (u + 4.0f * u2 - 3.0f * u3);
    const float wNext = 0.5f * (-u2 + u3);
    return wPrev * dPrev + wA * dA + wB * dB + wNext * dNext;
}

bool ContainsSelection(const std::vector<CurvesGraphView::SelectedKey>& selection, int channel, float keyTime)
{
    return std::any_of(selection.begin(), selection.end(),
                       [channel, keyTime](const CurvesGraphView::SelectedKey& key)
                       {
                           return key.Channel == channel && KeyTimesMatch(key.KeyTime, keyTime);
                       });
}

// Evaluate channel component value with pre/post infinity extrapolation.
float EvaluateChannelExtrapolated(const AnimChannel& ch, uint32 comp, float time)
{
    if (ch.keys.empty()) return 0.0f;

    const float firstTime = ch.keys.front().time;
    const float lastTime  = ch.keys.back().time;
    const float duration  = std::max(1e-6f, lastTime - firstTime);
    const float firstVal  = GetChannelComponentValue(ch, ch.keys.front(), comp);
    const float lastVal   = GetChannelComponentValue(ch, ch.keys.back(),  comp);

    auto mapTime = [&](float t, AnimExtrapolation mode) -> float {
        switch (mode)
        {
        case AnimExtrapolation::Constant:  return (t < firstTime) ? firstTime : lastTime;
        case AnimExtrapolation::Linear:    return t; // handled separately via slope
        case AnimExtrapolation::Cycle:
        {
            const float rel = t - firstTime;
            const float rem = rel - std::floor(rel / duration) * duration;
            return firstTime + rem;
        }
        case AnimExtrapolation::CycleWithOffset: // offset applied after; map same as Cycle
        {
            const float rel = t - firstTime;
            const float rem = rel - std::floor(rel / duration) * duration;
            return firstTime + rem;
        }
        case AnimExtrapolation::Oscillate:
        {
            const float rel = t - firstTime;
            const float period = 2.0f * duration;
            float rem = rel - std::floor(rel / period) * period;
            if (rem > duration) rem = period - rem;
            return firstTime + rem;
        }
        }
        return t;
    };

    if (time < firstTime)
    {
        if (ch.preInfinity == AnimExtrapolation::Constant)
            return firstVal;
        if (ch.preInfinity == AnimExtrapolation::Linear)
        {
            const float slope = ch.keys.front().outTangent[comp];
            return firstVal + slope * (time - firstTime);
        }
        const float mappedTime = mapTime(time, ch.preInfinity);
        // evaluate at mapped time (simplified: just lerp from channel data)
        if (ch.keys.size() >= 2)
        {
            for (size_t i = 0; i + 1 < ch.keys.size(); ++i)
            {
                if (mappedTime >= ch.keys[i].time && mappedTime <= ch.keys[i + 1].time)
                {
                    const float t0 = ch.keys[i].time, t1 = ch.keys[i + 1].time;
                    const float u = (mappedTime - t0) / std::max(1e-6f, t1 - t0);
                    const float v0 = GetChannelComponentValue(ch, ch.keys[i], comp);
                    const float v1 = GetChannelComponentValue(ch, ch.keys[i + 1], comp);
                    const float baseVal = v0 + (v1 - v0) * u;
                    if (ch.preInfinity == AnimExtrapolation::CycleWithOffset)
                    {
                        const float cycle = std::floor((time - firstTime) / duration);
                        return baseVal + cycle * (lastVal - firstVal);
                    }
                    return baseVal;
                }
            }
        }
        return firstVal;
    }

    if (time > lastTime)
    {
        if (ch.postInfinity == AnimExtrapolation::Constant)
            return lastVal;
        if (ch.postInfinity == AnimExtrapolation::Linear)
        {
            const float slope = ch.keys.back().inTangent[comp];
            return lastVal + slope * (time - lastTime);
        }
        const float mappedTime = mapTime(time, ch.postInfinity);
        if (ch.keys.size() >= 2)
        {
            for (size_t i = 0; i + 1 < ch.keys.size(); ++i)
            {
                if (mappedTime >= ch.keys[i].time && mappedTime <= ch.keys[i + 1].time)
                {
                    const float t0 = ch.keys[i].time, t1 = ch.keys[i + 1].time;
                    const float u = (mappedTime - t0) / std::max(1e-6f, t1 - t0);
                    const float v0 = GetChannelComponentValue(ch, ch.keys[i], comp);
                    const float v1 = GetChannelComponentValue(ch, ch.keys[i + 1], comp);
                    const float baseVal = v0 + (v1 - v0) * u;
                    if (ch.postInfinity == AnimExtrapolation::CycleWithOffset)
                    {
                        const float cycle = std::ceil((time - lastTime) / duration);
                        return baseVal + cycle * (lastVal - firstVal);
                    }
                    return baseVal;
                }
            }
        }
        return lastVal;
    }

    // Inside range — shouldn't be called, but handle gracefully
    return GetChannelComponentValue(ch, ch.keys.front(), comp);
}

int FindKeyIndexForTime(const AnimChannel& channel, float keyTime)
{
    for (size_t keyIndex = 0; keyIndex < channel.keys.size(); ++keyIndex)
    {
        if (KeyTimesMatch(channel.keys[keyIndex].time, keyTime))
            return static_cast<int>(keyIndex);
    }

    return -1;
}

struct SelectionBounds
{
    bool Valid = false;
    float Left = 0.0f;
    float Top = 0.0f;
    float Right = 0.0f;
    float Bottom = 0.0f;
    float TimeHandleLeft = 0.0f;
    float TimeHandleTop = 0.0f;
    float TimeHandleRight = 0.0f;
    float TimeHandleBottom = 0.0f;
    float LeftTimeHandleLeft = 0.0f;
    float LeftTimeHandleTop = 0.0f;
    float LeftTimeHandleRight = 0.0f;
    float LeftTimeHandleBottom = 0.0f;
    float ValueHandleLeft = 0.0f;
    float ValueHandleTop = 0.0f;
    float ValueHandleRight = 0.0f;
    float ValueHandleBottom = 0.0f;
    float BottomValueHandleLeft = 0.0f;
    float BottomValueHandleTop = 0.0f;
    float BottomValueHandleRight = 0.0f;
    float BottomValueHandleBottom = 0.0f;
    float MinTime = 0.0f;
    float MaxTime = 0.0f;
    float MinValue = 0.0f;
    float MaxValue = 0.0f;
};

SelectionBounds ComputeSelectionBounds(const AnimationClip* clip,
                                       int /*selectedChannel*/,
                                       uint32 selectedComponent,
                                       const std::vector<CurvesGraphView::SelectedKey>& selection,
                                       float drawX, float drawY, float drawW, float drawH,
                                       float rangeStart, float rangeEnd,
                                       float valueMin, float valueMax,
                                       bool allComponents = false,
                                       float pixelScale = 1.0f)
{
    SelectionBounds bounds;
    if (!clip || selection.empty() || drawW <= 0.0f || drawH <= 0.0f)
        return bounds;

    const std::vector<AnimChannel>& channels = clip->GetChannels();
    const float rangeDuration = std::max(0.001f, rangeEnd - rangeStart);
    const float valueRange = std::max(0.001f, valueMax - valueMin);

    bool initialized = false;
    for (const CurvesGraphView::SelectedKey& key : selection)
    {
        if (key.Channel < 0 || static_cast<size_t>(key.Channel) >= channels.size())
            continue;

        const AnimChannel& ch = channels[static_cast<size_t>(key.Channel)];
        const int keyIndex = FindKeyIndexForTime(ch, key.KeyTime);
        if (keyIndex < 0)
            continue;

        const AnimKeyframe& keyframe = ch.keys[static_cast<size_t>(keyIndex)];
        const float px = drawX + (keyframe.time - rangeStart) / rangeDuration * drawW;

        const uint32 compStart = allComponents ? 0u : selectedComponent;
        const uint32 compEnd   = allComponents ? GetComponentCount(ch) : selectedComponent + 1u;

        for (uint32 comp = compStart; comp < compEnd; ++comp)
        {
            const float keyValue = GetChannelComponentValue(ch, keyframe, comp);
            const float py = drawY + drawH - ((keyValue - valueMin) / valueRange) * drawH;
            const float kPad = 4.0f * pixelScale;
            const float left = px - kPad;
            const float right = px + kPad;
            const float top = py - kPad;
            const float bottom = py + kPad;

            if (!initialized)
            {
                bounds.Left = left;
                bounds.Top = top;
                bounds.Right = right;
                bounds.Bottom = bottom;
                bounds.MinTime = keyframe.time;
                bounds.MaxTime = keyframe.time;
                bounds.MinValue = keyValue;
                bounds.MaxValue = keyValue;
                initialized = true;
            }
            else
            {
                bounds.Left = std::min(bounds.Left, left);
                bounds.Top = std::min(bounds.Top, top);
                bounds.Right = std::max(bounds.Right, right);
                bounds.Bottom = std::max(bounds.Bottom, bottom);
                bounds.MinTime = std::min(bounds.MinTime, keyframe.time);
                bounds.MaxTime = std::max(bounds.MaxTime, keyframe.time);
                bounds.MinValue = std::min(bounds.MinValue, keyValue);
                bounds.MaxValue = std::max(bounds.MaxValue, keyValue);
            }
        }
    }

    if (!initialized)
        return bounds;

    const float handleHalf = kSelectionHandleHalfSizePx * pixelScale;
    const float expand = 4.0f * pixelScale;
    bounds.Valid = true;
    bounds.Left -= expand;
    bounds.Top -= expand;
    bounds.Right += expand;
    bounds.Bottom += expand;
    const float timeHandleCenterY = (bounds.Top + bounds.Bottom) * 0.5f;
    bounds.TimeHandleLeft = bounds.Right - handleHalf;
    bounds.TimeHandleRight = bounds.Right + handleHalf;
    bounds.TimeHandleTop = timeHandleCenterY - handleHalf;
    bounds.TimeHandleBottom = timeHandleCenterY + handleHalf;
    bounds.LeftTimeHandleLeft = bounds.Left - handleHalf;
    bounds.LeftTimeHandleRight = bounds.Left + handleHalf;
    bounds.LeftTimeHandleTop = timeHandleCenterY - handleHalf;
    bounds.LeftTimeHandleBottom = timeHandleCenterY + handleHalf;
    const float valueHandleCenterX = (bounds.Left + bounds.Right) * 0.5f;
    bounds.ValueHandleLeft = valueHandleCenterX - handleHalf;
    bounds.ValueHandleRight = valueHandleCenterX + handleHalf;
    bounds.ValueHandleTop = bounds.Top - handleHalf;
    bounds.ValueHandleBottom = bounds.Top + handleHalf;
    bounds.BottomValueHandleLeft = valueHandleCenterX - handleHalf;
    bounds.BottomValueHandleRight = valueHandleCenterX + handleHalf;
    bounds.BottomValueHandleTop = bounds.Bottom - handleHalf;
    bounds.BottomValueHandleBottom = bounds.Bottom + handleHalf;
    return bounds;
}

// toggleMode: Ctrl-box — keys inside the box that are already in the seed are removed,
// keys not in the seed are added. Keys outside the box are left unchanged from the seed.
std::vector<CurvesGraphView::SelectedKey> BuildBoxSelection(const AnimationClip* clip,
                                                            const std::vector<int>& visibleChannels,
                                                            uint32 selectedComponent,
                                                            bool allComponents,
                                                            float drawW, float drawH,
                                                            float rangeStart, float rangeEnd,
                                                            float valueMin, float valueMax,
                                                            float boxStartX, float boxStartY,
                                                            float boxEndX, float boxEndY,
                                                            const std::vector<CurvesGraphView::SelectedKey>& seed,
                                                            bool toggleMode = false)
{
    std::vector<CurvesGraphView::SelectedKey> selection = seed;
    if (!clip || visibleChannels.empty() || drawW <= 0.0f || drawH <= 0.0f)
        return selection;

    const std::vector<AnimChannel>& channels = clip->GetChannels();
    const float rangeDuration = std::max(0.001f, rangeEnd - rangeStart);
    const float valueRange = std::max(0.001f, valueMax - valueMin);
    const float boxLeft = std::min(boxStartX, boxEndX);
    const float boxRight = std::max(boxStartX, boxEndX);
    const float boxTop = std::min(boxStartY, boxEndY);
    const float boxBottom = std::max(boxStartY, boxEndY);

    for (int chIdx : visibleChannels)
    {
        if (chIdx < 0 || static_cast<size_t>(chIdx) >= channels.size())
            continue;
        const AnimChannel& ch = channels[static_cast<size_t>(chIdx)];
        const uint32 numComp = GetComponentCount(ch);
        const uint32 compStart = allComponents ? 0u : std::min(selectedComponent, numComp - 1u);
        const uint32 compEnd   = allComponents ? numComp : compStart + 1u;
        for (const AnimKeyframe& keyframe : ch.keys)
        {
            const float px = (keyframe.time - rangeStart) / rangeDuration * drawW;
            if (px < boxLeft || px > boxRight)
                continue;
            bool hit = false;
            for (uint32 c = compStart; c < compEnd && !hit; ++c)
            {
                const float keyValue = GetChannelComponentValue(ch, keyframe, c);
                const float py = drawH - ((keyValue - valueMin) / valueRange) * drawH;
                if (py >= boxTop && py <= boxBottom)
                    hit = true;
            }
            if (!hit)
                continue;
            if (toggleMode)
            {
                auto it = std::find_if(selection.begin(), selection.end(),
                    [chIdx, &keyframe](const CurvesGraphView::SelectedKey& k)
                    { return k.Channel == chIdx && KeyTimesMatch(k.KeyTime, keyframe.time); });
                if (it != selection.end())
                    selection.erase(it);
                else
                    selection.push_back({chIdx, keyframe.time});
            }
            else if (!ContainsSelection(selection, chIdx, keyframe.time))
            {
                selection.push_back({chIdx, keyframe.time});
            }
        }
    }

    return selection;
}
} // namespace

CurvesGraphView::CurvesGraphView()
{
    AddClass("animationwindow-curves");
}

void CurvesGraphView::SetClip(const AnimationClip* clip)
{
    if (m_Clip != clip)
    {
        m_Clip = clip;
        m_PinnedCurves.clear();
        m_SelectedKeyIndex = -1;
        m_SelectedKeyTime = -1.0f;
        m_SelectedKeys.clear();
        m_DragKeys.clear();
        m_ScalingSelectionTime = false;
        m_ScalingSelectionValue = false;
        m_ScaleTimeFromRight = false;
        m_ScaleValueFromBottom = false;
        m_BoxSelecting = false;
        m_PendingEmptyAction = false;
        m_BoxSelectModifierDown = false;
        m_DraggingLinkedHandles = false;
        m_RightClickScrubbing = false;
        m_DragThresholdMet = false;
        m_HandleDragThresholdMet = false;
        m_PrimaryHandleDrag = {};
        m_SecondaryHandleDrag = {};
        MarkDirty(VisualDirty);
    }
    else
    {
        // Clip hasn't changed - don't clear selection, just refresh visuals.
        MarkDirty(VisualDirty);
    }
}

void CurvesGraphView::SetTimeRange(float rangeStart, float rangeEnd)
{
    if (m_RangeStart != rangeStart || m_RangeEnd != rangeEnd)
    {
        m_RangeStart = rangeStart;
        m_RangeEnd = rangeEnd;
        MarkDirty(VisualDirty);
    }
}

void CurvesGraphView::SetValueRange(float valueMin, float valueMax)
{
    if (m_ValueMin != valueMin || m_ValueMax != valueMax)
    {
        m_ValueMin = valueMin;
        m_ValueMax = valueMax;
        MarkDirty(VisualDirty);
    }
}

void CurvesGraphView::ApplyValueZoom(float scrollY)
{
    const float center = 0.5f * (m_ValueMin + m_ValueMax);
    const float range = m_ValueMax - m_ValueMin;
    const float zoomFactor = (scrollY < 0.0f) ? 1.0f / 1.15f : 1.15f;
    float newRange = range * zoomFactor;
    newRange = std::clamp(newRange, 0.01f, 1000.0f);
    m_ValueMin = center - newRange * 0.5f;
    m_ValueMax = center + newRange * 0.5f;
    MarkDirty(VisualDirty);
}

void CurvesGraphView::SetCurrentTime(float t)
{
    if (m_CurrentTime != t)
    {
        m_CurrentTime = t;
        MarkDirty(VisualDirty);
    }
}

void CurvesGraphView::SetChannelColors(const std::unordered_map<uint32, uint32>& colors)
{
    m_ChannelColors = colors;
    MarkDirty(VisualDirty);
}

void CurvesGraphView::SetShowGrid(bool show)
{
    m_ShowGrid = show;
    MarkDirty(VisualDirty);
}

void CurvesGraphView::SetGridColor(uint32 argb)
{
    m_GridColor = m_GridHLineColor = m_GridVLineColor = argb;
    MarkDirty(VisualDirty);
}

void CurvesGraphView::SetGridHLineColor(uint32 argb)
{
    m_GridHLineColor = argb;
    MarkDirty(VisualDirty);
}

void CurvesGraphView::SetGridVLineColor(uint32 argb)
{
    m_GridVLineColor = argb;
    MarkDirty(VisualDirty);
}

void CurvesGraphView::SetGridLineThickness(float px)
{
    m_GridLineThickness = std::max(0.5f, px);
    MarkDirty(VisualDirty);
}

void CurvesGraphView::SetBaselineColor(uint32 argb)
{
    m_BaselineColor = argb;
    MarkDirty(VisualDirty);
}

void CurvesGraphView::SetBaselineThickness(float px)
{
    m_BaselineThickness = std::max(1.0f, px);
    MarkDirty(VisualDirty);
}

void CurvesGraphView::SetCurveLineWidth(float px)
{
    m_CurveLineWidth = std::max(1.0f, px);
    MarkDirty(VisualDirty);
}

void CurvesGraphView::SetCurveViewMode(CurveViewMode mode)
{
    m_CurveViewMode = mode;
    MarkDirty(VisualDirty);
}

bool CurvesGraphView::RebuildLatticeFromSelection()
{
    // Mid-drag rebuild would invalidate the captured drag entries — bail.
    if (m_LatticeDragIndex >= 0) return m_LatticeActive;
    if (!m_Clip || m_SelectedKeys.empty())
    {
        m_LatticeActive = false;
        m_LatticePoints.clear();
        return false;
    }

    float tMin = std::numeric_limits<float>::max();
    float tMax = -std::numeric_limits<float>::max();
    int   keyCount = 0;
    const AnimChannel* repCh = nullptr;
    for (const SelectedKey& sk : m_SelectedKeys)
    {
        if (sk.Channel < 0 || static_cast<size_t>(sk.Channel) >= m_Clip->GetChannels().size()) continue;
        const AnimChannel& ch = m_Clip->GetChannels()[static_cast<size_t>(sk.Channel)];
        if (!repCh) repCh = &ch;
        for (const AnimKeyframe& kf : ch.keys)
        {
            if (!KeyTimesMatch(kf.time, sk.KeyTime)) continue;
            tMin = std::min(tMin, kf.time);
            tMax = std::max(tMax, kf.time);
            ++keyCount;
        }
    }

    constexpr int kRequiredKeys = 3;
    if (keyCount < kRequiredKeys || !repCh || tMax <= tMin)
    {
        m_LatticeActive = false;
        m_LatticePoints.clear();
        MarkDirty(VisualDirty);
        return false;
    }

    const int n = std::clamp(m_LatticePointCount, 2, 16);
    m_LatticePoints.assign(static_cast<size_t>(n), LatticeCP{});
    const float span = tMax - tMin;
    for (int ci = 0; ci < n; ++ci)
    {
        const float u = static_cast<float>(ci) / static_cast<float>(n - 1);
        const float t = tMin + span * u;
        const float v = SampleChannelLinear(*repCh, m_SelectedComponent, t);
        m_LatticePoints[static_cast<size_t>(ci)] = {t, v, v};
    }
    // Baselines: per selected key, the value at gesture-start so deformation
    // is additive from the rest pose (multi-channel support — each entry
    // carries its own channel and baseline).
    m_LatticeBaselineKeyValues.clear();
    for (const SelectedKey& sk : m_SelectedKeys)
    {
        if (sk.Channel < 0 || static_cast<size_t>(sk.Channel) >= m_Clip->GetChannels().size()) continue;
        const AnimChannel& ch = m_Clip->GetChannels()[static_cast<size_t>(sk.Channel)];
        for (const AnimKeyframe& kf : ch.keys)
            if (KeyTimesMatch(kf.time, sk.KeyTime))
            {
                m_LatticeBaselineKeyValues.push_back({sk.Channel, kf.time, GetChannelComponentValue(ch, kf, m_SelectedComponent)});
                break;
            }
    }
    m_LatticeActive = true;
    MarkDirty(VisualDirty);
    return true;
}

void CurvesGraphView::BumpLatticePointCount(int delta)
{
    if (delta == 0) return;
    SetLatticePointCount(m_LatticePointCount + delta);
}

void CurvesGraphView::SetLatticePointCount(int count)
{
    const int newN = std::clamp(count, 2, 16);
    if (newN == m_LatticePointCount) return;
    m_LatticePointCount = newN;
    if (m_ActiveTool == CurveTool::Lattice && m_LatticeDragIndex < 0)
    {
        // Force a rebuild so the user immediately sees the new CP count.
        m_LatticeActive = false;
        RebuildLatticeFromSelection();
    }
    MarkDirty(VisualDirty);
}

void CurvesGraphView::ToggleLatticeBasis()
{
    SetLatticeBasis(m_LatticeBasis == LatticeBasis::Bezier
        ? LatticeBasis::CatmullRom
        : LatticeBasis::Bezier);
}

void CurvesGraphView::SetLatticeBasis(LatticeBasis basis)
{
    if (m_LatticeDragIndex >= 0) return; // don't switch mid-drag
    if (m_LatticeBasis == basis) return;
    m_LatticeBasis = basis;
    MarkDirty(VisualDirty);
}

void CurvesGraphView::SetActiveTool(CurveTool tool)
{
    m_ActiveTool = tool;
    m_RetimeDragging = false;
    m_LatticeActive = false;
    m_LatticeDragIndex = -1;
    m_LatticeHoverIndex = -1;
    m_LatticeDragKeys.clear();
    // Always reset region state on a tool change so leaving Retime fully
    // tears down its overlays/handles (otherwise a stale region could keep
    // rendering or get reused if the user toggles Retime back on).
    m_RetimeRegionActive = false;
    m_RetimePendingIn = false;
    m_RetimeHover = RetimeHover::None;
    m_RetimePreview.clear();
    m_DrawingCurve = false;
    m_DrawCurveSamples.clear();
    m_DrawCurveChannel = -1;

    // Activating Retime should clear any prior keyframe selection — the
    // tool's gestures operate on time ranges, not on the existing pick set,
    // and leftover highlights confuse the user (and the snapshot filter).
    // ClearSelection() resets both the multi-selection (m_SelectedKeys) and
    // the single-key state (m_SelectedKeyIndex / m_SelectedKeyTime); the
    // earlier "if (!m_SelectedKeys.empty())" guard missed the single-key
    // case, leaving the last-clicked key drawn as selected after toggling.
    if (tool == CurveTool::Retime)
    {
        const bool hadSelection =
            !m_SelectedKeys.empty() || m_SelectedKeyIndex >= 0 || m_SelectedKeyTime >= 0.0f;
        if (hadSelection)
        {
            ClearSelection();
            if (m_OnSelectionChanged) m_OnSelectionChanged();
        }
    }
    // Activating Lattice with keys already selected should produce the
    // lattice immediately — without this rebuild, m_LatticeActive stayed
    // false until the next pointer event landed on the curve view, so the
    // user only saw the existing selection markers ("I see only selected
    // points") and dragging produced no deformation because the CPs hadn't
    // been built yet.
    if (tool == CurveTool::Lattice)
        RebuildLatticeFromSelection();
    MarkDirty(VisualDirty);
}

void CurvesGraphView::TakeBufferSnapshot()
{
    if (!m_Clip) return;
    const auto& channels = m_Clip->GetChannels();
    m_BufferValues.clear();
    m_BufferValues.resize(channels.size());
    for (size_t ci = 0; ci < channels.size(); ++ci)
    {
        const AnimChannel& ch = channels[ci];
        const uint32 numComp = GetComponentCount(ch);
        m_BufferValues[ci].reserve(ch.keys.size() * numComp);
        for (const AnimKeyframe& key : ch.keys)
            for (uint32 c = 0; c < numComp; ++c)
                m_BufferValues[ci].push_back(GetChannelComponentValue(ch, key, c));
    }
    m_BufferSnapshotValid = !m_BufferValues.empty();
    MarkDirty(VisualDirty);
}

void CurvesGraphView::SwapBufferCurve()
{
    // Swap is a visual toggle — each call flips snapshot vs live data.
    // Since we can't mutate the clip from the view, just cycle the flag so the
    // buffer overlay draws on top and the user can see the difference visually.
    // Full swap (moving keys) is handled by AnimationWindowPanel.
    MarkDirty(VisualDirty);
}

void CurvesGraphView::RestoreSelection(std::vector<SelectedKey> keys, int selectedChannel, uint32 selectedComponent)
{
    m_SelectedKeys = std::move(keys);
    m_SelectedChannel = selectedChannel;
    m_SelectedComponent = selectedComponent;
    m_SelectedKeyIndex = -1;
    m_SelectedKeyTime = m_SelectedKeys.empty() ? -1.0f : m_SelectedKeys.front().KeyTime;
    MarkDirty(VisualDirty);
}

void CurvesGraphView::SetShowAllComponents(bool show)
{
    if (m_ShowAllComponents != show)
    {
        m_ShowAllComponents = show;
        RecomputeValueRange();
        MarkDirty(VisualDirty);
    }
}

void CurvesGraphView::SetVisibleChannels(const std::vector<int>& channels)
{
    std::vector<int> normalized;
    normalized.reserve(channels.size());
    for (int channel : channels)
    {
        if (channel < 0)
            continue;
        if (std::find(normalized.begin(), normalized.end(), channel) == normalized.end())
            normalized.push_back(channel);
    }

    if (m_VisibleChannels != normalized)
    {
        m_VisibleChannels = std::move(normalized);
        RecomputeValueRange();
        MarkDirty(VisualDirty);
    }
}

void CurvesGraphView::SetPinnedCurves(std::vector<PinnedCurve> curves)
{
    std::sort(curves.begin(), curves.end(), [](const auto& a, const auto& b) {
        return a.Channel < b.Channel || (a.Channel == b.Channel && a.Component < b.Component);
    });
    curves.erase(std::unique(curves.begin(), curves.end()), curves.end());
    if (m_PinnedCurves == curves) return;
    m_PinnedCurves = std::move(curves);
    RecomputeValueRange();
    MarkDirty(VisualDirty);
}

void CurvesGraphView::RecomputeValueRange()
{
    if (!m_Clip)
        return;

    const std::vector<AnimChannel>& channels = m_Clip->GetChannels();
    bool foundValue = false;
    float valueMin = 0.0f;
    float valueMax = 0.0f;

    auto expandComponent = [&](int channelIndex, uint32 componentIndex)
    {
        if (channelIndex < 0 || static_cast<size_t>(channelIndex) >= channels.size())
            return;

        const AnimChannel& channel = channels[static_cast<size_t>(channelIndex)];
        if (componentIndex >= GetComponentCount(channel))
            return;

        for (const AnimKeyframe& keyframe : channel.keys)
        {
            const float value = GetChannelComponentValue(channel, keyframe, componentIndex);
            if (!foundValue)
            {
                valueMin = value;
                valueMax = value;
                foundValue = true;
            }
            else
            {
                valueMin = std::min(valueMin, value);
                valueMax = std::max(valueMax, value);
            }
        }
    };

    auto expandRange = [&](int channelIndex, bool includeAllComponents)
    {
        if (channelIndex < 0 || static_cast<size_t>(channelIndex) >= channels.size())
            return;

        const AnimChannel& channel = channels[static_cast<size_t>(channelIndex)];
        if (channel.keys.empty())
            return;

        const uint32 componentStart = includeAllComponents ? 0u : std::min<uint32>(m_SelectedComponent, GetComponentCount(channel) - 1u);
        const uint32 componentEnd = includeAllComponents ? GetComponentCount(channel) : (componentStart + 1u);
        for (uint32 componentIndex = componentStart; componentIndex < componentEnd; ++componentIndex)
            expandComponent(channelIndex, componentIndex);
    };

    if (!m_VisibleChannels.empty())
    {
        const bool includeAllComponents = m_VisibleChannels.size() > 1u || m_ShowAllComponents;
        for (int channelIndex : m_VisibleChannels)
            expandRange(channelIndex, includeAllComponents);
    }
    else if (m_SelectedChannel >= 0)
    {
        expandRange(m_SelectedChannel, false);
    }

    // Pinned curves stay inside the visible value range even when nothing selects them.
    for (const auto& pin : m_PinnedCurves)
        expandComponent(pin.Channel, pin.Component);

    if (!foundValue)
        return;

    const float padding = std::max(0.25f, (valueMax - valueMin) * 0.15f);
    SetValueRange(valueMin - padding, valueMax + padding);
}

void CurvesGraphView::SetSelectedCurve(int channel, uint32 component)
{
    if (m_SelectedChannel != channel || m_SelectedComponent != component)
    {
        m_SelectedChannel = channel;
        m_SelectedComponent = component;
        // Don't reset selection state if we have selected keys - preserves selection during T/V edits
        if (m_SelectedKeys.empty())
        {
            m_SelectedKeyIndex = -1;
            m_SelectedKeyTime = -1.0f;
        }
        // Don't clear selected keys - preserve selection across curve changes
        // m_SelectedKeys.clear();
        // m_DragKeys.clear();
        m_DraggingLinkedHandles = false;
        m_PrimaryHandleDrag = {};
        m_SecondaryHandleDrag = {};
        m_BoxSelectModifierDown = false;
        RecomputeValueRange();
        MarkDirty(VisualDirty);
    }
}

void CurvesGraphView::ClearSelection()
{
    if (m_SelectedKeyIndex != -1 || m_SelectedKeyTime >= 0.0f || !m_SelectedKeys.empty())
    {
        m_SelectedKeyIndex = -1;
        m_SelectedKeyTime = -1.0f;
        m_SelectedKeys.clear();
        m_DragKeys.clear();
        m_ScalingSelectionTime = false;
        m_ScalingSelectionValue = false;
        m_ScaleTimeFromRight = false;
        m_ScaleValueFromBottom = false;
        m_BoxSelecting = false;
        m_PendingEmptyAction = false;
        m_BoxSelectModifierDown = false;
        MarkDirty(VisualDirty);
    }
}

void CurvesGraphView::SelectAllVisibleKeys()
{
    if (!m_Clip)
        return;

    const std::vector<AnimChannel>& channels = m_Clip->GetChannels();
    std::vector<int> sourceChannels = m_VisibleChannels;
    if (sourceChannels.empty() && m_SelectedChannel >= 0)
        sourceChannels.push_back(m_SelectedChannel);
    std::vector<SelectedKey> selection;
    for (int channelIndex : sourceChannels)
    {
        if (channelIndex < 0 || static_cast<size_t>(channelIndex) >= channels.size())
            continue;
        for (const AnimKeyframe& keyframe : channels[static_cast<size_t>(channelIndex)].keys)
        {
            if (!ContainsSelection(selection, channelIndex, keyframe.time))
                selection.push_back({channelIndex, keyframe.time});
        }
    }

    m_SelectedKeys = std::move(selection);
    m_DragKeys.clear();
    m_SelectedKeyIndex = -1;
    m_SelectedKeyTime = m_SelectedKeys.empty() ? -1.0f : m_SelectedKeys.front().KeyTime;
    if (!m_SelectedKeys.empty())
        m_SelectedChannel = m_SelectedKeys.front().Channel;
    MarkDirty(VisualDirty);
    if (m_OnSelectionChanged)
        m_OnSelectionChanged();
}

void CurvesGraphView::SelectKeyAtTime(int channel, float keyTime)
{
    if (!m_Clip || channel < 0 || static_cast<size_t>(channel) >= m_Clip->GetChannels().size()) return;
    const int keyIdx = FindKeyIndexForTime(m_Clip->GetChannels()[static_cast<size_t>(channel)], keyTime);
    if (keyIdx < 0) return;
    m_SelectedChannel = channel;
    m_SelectedKeyIndex = keyIdx;
    m_SelectedKeyTime = keyTime;
    m_SelectedKeys = {{channel, keyTime}};
    m_DragKeys.clear();
    MarkDirty(VisualDirty);
    if (m_OnKeyframeSelected)
        m_OnKeyframeSelected(channel, static_cast<int>(m_SelectedComponent), keyIdx, keyTime);
}

float CurvesGraphView::GetSelectedKeyframeTime() const
{
    return m_SelectedKeyTime;
}

void CurvesGraphView::OnEvent(UIEvent& e)
{
    // e.x/y and GetLayout*() are in logical (CSS) pixels; m_ContentX/W are
    // physical. Always derive local coords from the logical layout values so
    // mouse hit-testing is correct at any HiDPI scale.
    const float W = GetLayoutWidth();
    const float H = GetLayoutHeight();
    const float originX = GetLayoutX();
    const float originY = GetLayoutY();
    const float localX = e.X - originX;
    const float clampedLocalX = std::clamp(localX, 0.0f, W);
    const float localY = e.Y - originY;
    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const float valueRange = std::max(0.001f, m_ValueMax - m_ValueMin);
    const bool shiftHeld = (e.Mods & Input::kModShift) != 0;
    const bool controlHeld = (e.Mods & Input::kModControl) != 0;
    [[maybe_unused]] const bool altHeld = (e.Mods & Input::kModAlt) != 0;
    const SelectionBounds selectionBounds =
        ComputeSelectionBounds(m_Clip,
                               m_SelectedChannel,
                               m_SelectedComponent,
                               m_SelectedKeys,
                               0.0f,
                               0.0f,
                               W,
                               H,
                               m_RangeStart,
                               m_RangeEnd,
                               m_ValueMin,
                               m_ValueMax,
                               m_ShowAllComponents || m_VisibleChannels.size() > 1u);

    if (e.Id == kEventMouseUp && e.Button == 2 && m_Panning)
    {
        m_Panning = false;
        e.Stop();
        return;
    }
    // Retime, DrawCurve, and Lattice each own their own MouseUp dispatch
    // (commit batch, sample simplification, reset state). Skip the catch-all
    // MouseUp cleanup below when any of those drags is in flight; otherwise
    // this block consumes the up-event and the per-tool handler never runs —
    // for retime that left m_RetimeDragging stuck (every later click looked
    // like "still dragging"); for DrawCurve it dropped the entire stroke
    // commit so the live yellow preview vanished without producing any
    // keyframes; for Lattice it left m_LatticeDragIndex stuck so the CP
    // followed the cursor forever after a release.
    if (e.Id == kEventMouseUp && e.Button == 0 &&
        !m_RetimeDragging && !m_DrawingCurve && m_LatticeDragIndex < 0)
    {
        if (m_Panning)
        {
            m_Panning = false;
            e.Stop();
            return;
        }
        EditGesture endedGesture = EditGesture::Keyframe;
        bool hadEditGesture = false;
        if (m_DraggingInHandle)
        {
            m_DraggingInHandle = false;
            m_DraggingLinkedHandles = false;
            m_FreeTangentDrag = false;
            const bool wasHandleDragged = m_HandleDragThresholdMet;
            m_HandleDragThresholdMet = false;
            m_PrimaryHandleDrag = {};
            m_SecondaryHandleDrag = {};
            endedGesture = EditGesture::Tangent;
            hadEditGesture = wasHandleDragged;
            e.Stop();
        }
        if (m_DraggingOutHandle)
        {
            m_DraggingOutHandle = false;
            m_DraggingLinkedHandles = false;
            m_FreeTangentDrag = false;
            const bool wasHandleDragged = m_HandleDragThresholdMet;
            m_HandleDragThresholdMet = false;
            m_PrimaryHandleDrag = {};
            m_SecondaryHandleDrag = {};
            endedGesture = EditGesture::Tangent;
            hadEditGesture = wasHandleDragged;
            e.Stop();
        }
        if (m_DraggingKey)
        {
            m_DraggingKey = false;
            m_DragThresholdMet = false;
            m_DragConstrained = false;
            endedGesture = EditGesture::Keyframe;
            hadEditGesture = true;
            e.Stop();
        }
        if (m_ScalingSelectionTime || m_ScalingSelectionValue)
        {
            m_ScalingSelectionTime = false;
            m_ScalingSelectionValue = false;
            m_ScaleTimeFromRight = false;
            m_ScaleValueFromBottom = false;
            endedGesture = EditGesture::Keyframe;
            hadEditGesture = true;
            e.Stop();
        }
        if (hadEditGesture && m_OnEditFinished)
            m_OnEditFinished(endedGesture);
        m_PendingEmptyAction = false;
        m_BoxSelecting = false;
        m_BoxSelectModifierDown = false;
        m_DragKeys.clear();
        if (m_OnSelectionChanged) m_OnSelectionChanged();
        return;
    }
    if ((m_DraggingInHandle || m_DraggingOutHandle) && m_OnTangentEdited && m_Clip && m_SelectedChannel >= 0 &&
        static_cast<size_t>(m_SelectedChannel) < m_Clip->GetChannels().size() &&
        W > 0.0f && H > 0.0f)
    {
        if (!m_HandleDragThresholdMet)
        {
            constexpr float kHandleDragThresholdPx = 3.0f;
            const float dx = localX - m_HandleDragStartX;
            const float dy = localY - m_HandleDragStartY;
            if (dx * dx + dy * dy < kHandleDragThresholdPx * kHandleDragThresholdPx)
            {
                e.Stop();
                return;
            }
            m_HandleDragThresholdMet = true;
            if (m_OnEditStarted)
                m_OnEditStarted(EditGesture::Tangent);
        }
        const AnimChannel& channel = m_Clip->GetChannels()[static_cast<size_t>(m_SelectedChannel)];
        const int selectedKeyIndex = FindKeyIndexForTime(channel, m_SelectedKeyTime);
        if (selectedKeyIndex >= 0 && static_cast<size_t>(selectedKeyIndex) < channel.keys.size())
        {
            const size_t keyIndex = static_cast<size_t>(selectedKeyIndex);
            const float pointerTime = m_RangeStart + (clampedLocalX / W) * rangeDuration;
            const float pointerValue = m_ValueMin + (1.0f - std::clamp(localY / H, 0.0f, 1.0f)) * valueRange;
            bool editedHandle = false;

            {
                const bool incoming = m_DraggingInHandle;
                editedHandle = ApplyHandlePointToCurve(m_SelectedChannel,
                                                       channel,
                                                       m_SelectedComponent,
                                                       keyIndex,
                                                       incoming,
                                                       pointerTime,
                                                       pointerValue,
                                                       m_OnTangentEdited);

                // Locked mode (default): mirror tangent to the opposite handle so both
                // form a straight line through the keyframe (G1 continuity).
                // ALT or a persistent "broken" tangent flag disables mirroring.
                const bool isBroken = (m_Clip->GetChannels()[static_cast<size_t>(m_SelectedChannel)]
                                           .keys[keyIndex].tangentBroken & (1u << m_SelectedComponent)) != 0;
                if (editedHandle && !m_FreeTangentDrag && !isBroken)
                {
                    const AnimKeyframe& kf = m_Clip->GetChannels()[static_cast<size_t>(m_SelectedChannel)].keys[keyIndex];
                    const float mirroredTangent = incoming ? kf.inTangent[m_SelectedComponent]
                                                           : kf.outTangent[m_SelectedComponent];
                    const bool otherIncoming = !incoming;
                    const size_t otherNeighbor = otherIncoming ? (keyIndex - 1u) : (keyIndex + 1u);
                    const bool otherValid = otherIncoming ? (keyIndex > 0u)
                                                          : (otherNeighbor < m_Clip->GetChannels()[static_cast<size_t>(m_SelectedChannel)].keys.size());
                    if (otherValid)
                    {
                        const float otherWeight = otherIncoming ? kf.inWeight[m_SelectedComponent]
                                                                : kf.outWeight[m_SelectedComponent];
                        m_OnTangentEdited(m_SelectedChannel,
                                          static_cast<int>(m_SelectedComponent),
                                          kf.time,
                                          otherIncoming,
                                          mirroredTangent,
                                          otherWeight);
                    }
                }
            }

            if (editedHandle)
            {
                MarkDirty(VisualDirty);
                e.Stop();
                return;
            }
        }
    }
    if (e.Id == kEventMouseMove && (m_BoxSelecting || (m_PendingEmptyAction && !m_Seeking)) && m_Clip && !m_VisibleChannels.empty() && W > 0.0f && H > 0.0f)
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
                                               m_VisibleChannels,
                                               m_SelectedComponent,
                                               m_ShowAllComponents || m_VisibleChannels.size() > 1u,
                                               W,
                                               H,
                                               m_RangeStart,
                                               m_RangeEnd,
                                               m_ValueMin,
                                               m_ValueMax,
                                               m_BoxStartX,
                                               m_BoxStartY,
                                               m_BoxEndX,
                                               m_BoxEndY,
                                               (shiftHeld || m_BoxSelectModifierDown) ? m_BoxSelectionSeed : std::vector<SelectedKey>{},
                                               m_BoxSelectModifierDown);
            if (m_SelectedKeys.empty())
            {
                m_SelectedKeyIndex = -1;
                m_SelectedKeyTime = -1.0f;
            }
            else
            {
                m_SelectedKeyTime = m_SelectedKeys.front().KeyTime;
                m_SelectedKeyIndex = -1; // multi-channel box; primary index irrelevant
            }
            if (m_OnSelectionChanged) m_OnSelectionChanged();
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
    }
    if (e.Id == kEventMouseMove && m_DraggingKey && m_OnKeyframeEdited && W > 0.0f && H > 0.0f)
    {
        if (!m_DragThresholdMet)
        {
            constexpr float kDragThresholdPx = 3.0f;
            const float dx = localX - m_DragStartLocalX;
            const float dy = localY - m_DragStartLocalY;
            if (dx * dx + dy * dy < kDragThresholdPx * kDragThresholdPx)
            {
                MarkDirty(VisualDirty);
                e.Stop();
                return;
            }
            m_DragThresholdMet = true;
            // Determine constraint direction based on initial movement when shift is held
            if (m_DragConstrained)
            {
                m_DragConstrainHorizontal = std::abs(dx) >= std::abs(dy);
            }
            if (m_OnEditStarted)
                m_OnEditStarted(EditGesture::Keyframe);
        }
        float newTime = m_RangeStart + (clampedLocalX / W) * rangeDuration;
        newTime = std::clamp(newTime, m_RangeStart, m_RangeEnd);
        float valueAlpha = 1.0f - std::clamp(localY / H, 0.0f, 1.0f);
        float newValue = m_ValueMin + valueAlpha * valueRange;
        // Apply constraint: hold shift to constrain to time (horizontal) or value (vertical)
        if (m_DragConstrained)
        {
            if (m_DragConstrainHorizontal)
            {
                // Constrain to time only - keep original values
                newValue = m_DragCurrentValue;
            }
            else
            {
                // Constrain to value only - keep original times
                newTime = m_DragCurrentTime;
            }
        }
        // Apply snapping to drag position if enabled
        if (m_SnapTime)
            newTime = ComputeTimeSnap(newTime);
        if (m_SnapValue)
        {
            const float snapStep = ComputeValueSnapStep(valueRange);
            newValue = std::round(newValue / snapStep) * snapStep;
        }
        const float deltaTime = newTime - m_DragCurrentTime;
        const float deltaValue = newValue - m_DragCurrentValue;
        if (std::abs(deltaTime) > 0.0f || std::abs(deltaValue) > 0.0f)
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
                KeyDragSnapshot& dragKey = m_DragKeys[orderIndex];
                float targetTime = std::clamp(dragKey.CurrentTime + deltaTime, m_RangeStart, m_RangeEnd);
                float targetValue = dragKey.CurrentValue + deltaValue;
                // Apply snapping if enabled
                if (m_SnapTime)
                    targetTime = ComputeTimeSnap(targetTime);
                if (m_SnapValue)
                {
                    const float snapStep = ComputeValueSnapStep(valueRange);
                    targetValue = std::round(targetValue / snapStep) * snapStep;
                }
                m_OnKeyframeEdited(dragKey.Channel,
                                   static_cast<int>(m_SelectedComponent),
                                   dragKey.CurrentTime,
                                   targetTime,
                                   targetValue);
                dragKey.CurrentTime = targetTime;
                dragKey.CurrentValue = targetValue;
            }
            m_SelectedKeys.clear();
            m_SelectedKeys.reserve(m_DragKeys.size());
            for (const KeyDragSnapshot& dragKey : m_DragKeys)
                m_SelectedKeys.push_back({dragKey.Channel, dragKey.CurrentTime});
            m_DragCurrentTime = newTime;
            m_DragCurrentValue = newValue;
            if (!m_DragKeys.empty())
                m_SelectedKeyTime = m_DragKeys.front().CurrentTime;
        }
        MarkDirty(VisualDirty);
        e.Stop();
        return;
    }
    if (e.Id == kEventMouseMove && m_ScalingSelectionTime && m_OnKeyframeEdited && W > 0.0f && H > 0.0f && m_ScaleInitialDuration > 0.0f)
    {
        const float pointerTime = m_RangeStart + (clampedLocalX / W) * rangeDuration;
        const float rawScale = m_ScaleTimeFromRight
            ? (m_ScaleAnchorTime - pointerTime) / m_ScaleInitialDuration
            : (pointerTime - m_ScaleAnchorTime) / m_ScaleInitialDuration;
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
            KeyDragSnapshot& dragKey = m_DragKeys[orderIndex];
            const float targetTime =
                std::clamp(m_ScaleAnchorTime + (dragKey.InitialTime - m_ScaleAnchorTime) * scale, m_RangeStart, m_RangeEnd);
            m_OnKeyframeEdited(dragKey.Channel,
                               static_cast<int>(m_SelectedComponent),
                               dragKey.CurrentTime,
                               targetTime,
                               dragKey.InitialValue);
            dragKey.CurrentTime = targetTime;
            dragKey.CurrentValue = dragKey.InitialValue;
        }
        m_SelectedKeys.clear();
        m_SelectedKeys.reserve(m_DragKeys.size());
        for (const KeyDragSnapshot& dragKey : m_DragKeys)
            m_SelectedKeys.push_back({dragKey.Channel, dragKey.CurrentTime});
        if (!m_DragKeys.empty())
        {
            m_SelectedKeyTime = m_DragKeys.front().CurrentTime;
            m_SelectedKeyIndex = -1; // multi-channel; primary index irrelevant during scale
        }
        MarkDirty(VisualDirty);
        e.Stop();
        return;
    }
    if (e.Id == kEventMouseMove && m_ScalingSelectionValue && m_OnKeyframeEdited && W > 0.0f && H > 0.0f && m_ScaleInitialValueExtent > 0.0f)
    {
        const float pointerValue = m_ValueMin + (1.0f - std::clamp(localY / H, 0.0f, 1.0f)) * valueRange;
        const float rawScale = m_ScaleValueFromBottom
            ? (m_ScaleAnchorValue - pointerValue) / m_ScaleInitialValueExtent
            : (pointerValue - m_ScaleAnchorValue) / m_ScaleInitialValueExtent;
        const float scale = std::clamp(rawScale, 0.05f, 20.0f);
        for (KeyDragSnapshot& dragKey : m_DragKeys)
        {
            const float targetValue = m_ScaleAnchorValue + (dragKey.InitialValue - m_ScaleAnchorValue) * scale;
            m_OnKeyframeEdited(dragKey.Channel,
                               static_cast<int>(m_SelectedComponent),
                               dragKey.CurrentTime,
                               dragKey.CurrentTime,
                               targetValue);
            dragKey.CurrentValue = targetValue;
        }
        MarkDirty(VisualDirty);
        e.Stop();
        return;
    }
    if (e.Id == kEventMouseMove && m_Panning && m_OnPan && W > 0.0f && H > 0.0f)
    {
        const float deltaPx = e.X - m_PanLastGlobalX;
        const float deltaTime = (deltaPx / W) * rangeDuration;
        m_OnPan(deltaTime);
        m_PanLastGlobalX = e.X;
        if (!m_PanHorizontalOnly && !(e.Mods & Input::kModShift))
        {
            const float deltaY = e.Y - m_PanLastGlobalY;
            const float panValueRange = m_ValueMax - m_ValueMin;
            const float deltaValue = (deltaY / H) * panValueRange;
            m_ValueMin += deltaValue;
            m_ValueMax += deltaValue;
            MarkDirty(VisualDirty);
        }
        m_PanLastGlobalY = e.Y;
        e.Stop();
        return;
    }
    if (e.Id == kEventMouseMove && m_Seeking && m_OnSeekToTime && W > 0.0f)
    {
        float t = m_RangeStart + (clampedLocalX / W) * rangeDuration;
        t = std::clamp(t, m_RangeStart, m_RangeEnd);
        m_OnSeekToTime(t);
        e.Stop();
        return;
    }
    if (e.Id == kEventMouseDown && m_OnPan && W > 0.0f && H > 0.0f &&
        localX >= 0.0f && localX <= W && localY >= 0.0f && localY <= H &&
        (e.Button == 2 || (e.Button == 0 && (e.Mods & Input::kModAlt) != 0)))
    {
        m_Panning = true;
        m_PanHorizontalOnly = (e.Button == 0) && ((e.Mods & Input::kModShift) != 0);
        m_PanLastGlobalX = e.X;
        m_PanLastGlobalY = e.Y;
        e.Capture(this);
        e.Stop();
        return;
    }
    // Lattice tool: handle in-progress drag independent of selection state.
    // Runs before the init block so that EnsureEditableClip() in OnEditStarted
    // (which replaces m_Clip and clears m_SelectedKeys) cannot break a captured drag.
    if (m_ActiveTool == CurveTool::Lattice && m_LatticeDragIndex >= 0 &&
        !m_LatticePoints.empty() && m_Clip && W > 0.0f && H > 0.0f)
    {
        if (e.Id == kEventMouseMove)
        {
            const float vDelta = (m_LatticeAnchorY - localY) / H * (m_ValueMax - m_ValueMin);
            const size_t cp = static_cast<size_t>(m_LatticeDragIndex);
            m_LatticePoints[cp].Value += vDelta;
            m_LatticeAnchorY = localY;

            // Optional value-snapping mirrors the rest of the editor's gestures.
            if (m_SnapValue)
            {
                const float step = ComputeValueSnapStep(m_ValueMax - m_ValueMin);
                if (step > 0.0f)
                    m_LatticePoints[cp].Value =
                        std::round(m_LatticePoints[cp].Value / step) * step;
            }

            if (m_OnLatticeEdited && m_LatticePoints.size() >= 2u)
            {
                const size_t n = m_LatticePoints.size();
                std::vector<float> times(n);
                std::vector<float> deltas(n);
                for (size_t i = 0; i < n; ++i)
                {
                    times[i]  = m_LatticePoints[i].Time;
                    deltas[i] = m_LatticePoints[i].Value - m_LatticePoints[i].BaselineValue;
                }
                const float tFirst = times.front();
                const float tLast  = times.back();
                const float span   = std::max(1e-6f, tLast - tFirst);
                const bool useBezier = (m_LatticeBasis == LatticeBasis::Bezier);
                for (const auto& entry : m_LatticeDragKeys)
                {
                    float d;
                    if (useBezier)
                    {
                        const float u = std::clamp((entry.Time - tFirst) / span, 0.0f, 1.0f);
                        d = EvaluateBezierBasis(deltas.data(), n, u);
                    }
                    else
                    {
                        d = EvaluateCatmullRomBasis(times.data(), deltas.data(), n, entry.Time);
                    }
                    m_OnLatticeEdited(entry.Channel, entry.Time, entry.BaselineValue + d);
                }
            }
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
        if (e.Id == kEventMouseUp)
        {
            m_LatticeDragIndex = -1;
            if (m_OnEditFinished) m_OnEditFinished(EditGesture::Lattice);
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
    }

    // ---- Retime tool ------------------------------------------------------
    // Single pivot:
    //   plain drag  → scale keys past pivot (proportional)
    //   Shift+drag  → translate keys past pivot
    // Two-handle (Maya-style) region:
    //   Alt+drag    → draw a region rubber-band (no keys move during draw)
    //   while a region is active:
    //     drag near regionLeft edge  → scale keys in region around regionRight
    //     drag near regionRight edge → scale keys in region around regionLeft
    //     drag inside region middle  → translate region + keys
    //     click outside region       → clear the region (no other action)
    // Modifiers: Ctrl forces frame snap during the drag.
    // Skip the entire retime block when other gestures own the pointer —
    // panning and right-click scrubbing must keep working with the retime
    // tool active. Mouse-down for non-left buttons is also gated so right-
    // and middle-click never engage retime.
    const bool retimeMouseEligible =
        (e.Id != kEventMouseDown || e.Button == 0) && !m_Panning && !m_RightClickScrubbing;
    if (m_ActiveTool == CurveTool::Retime && m_Clip && W > 0.0f && retimeMouseEligible)
    {
        // Convert pixel x to time and back. These are reused below.
        auto pxToTime = [&](float px) {
            return m_RangeStart + (std::clamp(px, 0.0f, W) / W) * (m_RangeEnd - m_RangeStart);
        };
        auto timeToPx = [&](float t) {
            return std::clamp((t - m_RangeStart) / std::max(1e-6f, m_RangeEnd - m_RangeStart), 0.0f, 1.0f) * W;
        };
        constexpr float kHandleHitPx = 6.0f; // pixel distance for region edge grab

        if (e.Id == kEventMouseDown && e.Button == 0 && localX >= 0.0f && localX <= W)
        {
            const float clickTime = pxToTime(localX);

            // Decide which gesture to start. Note: when a pending in-point is
            // set we still start a tentative DefineRegion drag here — the
            // mouse-up handler decides whether to pair (no drag) or to use
            // this gesture's own bounds as a fresh region (real drag).
            //
            // Region-aware dispatch (when a region is visible):
            //   - hit a left/right handle  → scale that side
            //   - press inside the body    → tentative translate; if the
            //     cursor moves past the click-vs-drag threshold the body
            //     drags with the cursor, otherwise the release dismisses the
            //     region (the "click again to clear" UX)
            //   - press anywhere else      → clear region and start a fresh
            //     in/out drag at the click point
            RetimeMode chosen;
            if (m_RetimeRegionActive)
            {
                const float leftPx  = timeToPx(m_RetimeRegionLeft);
                const float rightPx = timeToPx(m_RetimeRegionRight);
                if (std::abs(localX - leftPx) <= kHandleHitPx)
                    chosen = RetimeMode::RegionScaleLeft;
                else if (std::abs(localX - rightPx) <= kHandleHitPx)
                    chosen = RetimeMode::RegionScaleRight;
                else if (clickTime >= m_RetimeRegionLeft && clickTime <= m_RetimeRegionRight)
                    chosen = RetimeMode::RegionTranslate;
                else
                {
                    m_RetimeRegionActive = false;
                    m_RetimePendingIn = false;
                    chosen = RetimeMode::DefineRegion;
                }
            }
            else
            {
                // No region yet: drag defines the in/out points; a click-only
                // (no movement) becomes the pending in-point (or pairs with
                // an existing one) on mouse-up.
                chosen = RetimeMode::DefineRegion;
            }
            m_RetimeDragConfirmed = false;

            m_RetimeMode         = chosen;
            m_RetimeDragging     = true;
            m_RetimeAnchorX      = localX;
            m_RetimeDeltaSeconds = 0.0f;
            m_RetimeScaleFactor  = 1.0f;
            m_RetimePreview.clear();

            // Save region's pre-drag state so Escape can revert it.
            m_RetimeRegionLeftOrig  = m_RetimeRegionLeft;
            m_RetimeRegionRightOrig = m_RetimeRegionRight;
            m_RetimeRegionWasActive = m_RetimeRegionActive;

            if (chosen == RetimeMode::DefineRegion)
            {
                // Replace any existing region; finalize on mouse-up.
                m_RetimeRegionLeft   = clickTime;
                m_RetimeRegionRight  = clickTime;
                m_RetimeRegionActive = false; // becomes true on mouse-up
                if (m_OnEditStarted) m_OnEditStarted(EditGesture::Retime);
                e.Capture(this);
                e.Stop();
                MarkDirty(VisualDirty);
                return;
            }

            // Single-pivot path: pivot = click time.
            // Region path: pivot is the *anchor* edge (the opposite of the
            // handle being dragged); for Translate we set pivot = regionLeft
            // for HUD bookkeeping only.
            switch (chosen)
            {
                case RetimeMode::Scale:
                case RetimeMode::Translate:
                    m_RetimePivotTime = clickTime;
                    break;
                case RetimeMode::RegionScaleRight:
                case RetimeMode::RegionTranslate:
                    m_RetimePivotTime = m_RetimeRegionLeft;
                    break;
                case RetimeMode::RegionScaleLeft:
                    m_RetimePivotTime = m_RetimeRegionRight;
                    break;
                default:
                    break;
            }

            // Snapshot eligible keys.
            std::vector<int> chans;
            if (!m_VisibleChannels.empty())
                chans = m_VisibleChannels;
            else
            {
                chans.reserve(m_Clip->GetChannels().size());
                for (size_t i = 0; i < m_Clip->GetChannels().size(); ++i)
                    chans.push_back(static_cast<int>(i));
            }
            const bool restrictToSelection =
                (chosen == RetimeMode::Scale || chosen == RetimeMode::Translate)
                && !m_SelectedKeys.empty();
            auto isKeySelected = [this](int ci, float t)
            {
                for (const SelectedKey& sk : m_SelectedKeys)
                    if (sk.Channel == ci && std::abs(sk.KeyTime - t) < 1e-5f)
                        return true;
                return false;
            };
            float farthest = 0.0f;
            for (int ci : chans)
            {
                if (ci < 0 || static_cast<size_t>(ci) >= m_Clip->GetChannels().size())
                    continue;
                if (m_IsChannelLocked && m_IsChannelLocked(ci))
                    continue;
                const AnimChannel& ch = m_Clip->GetChannels()[static_cast<size_t>(ci)];
                for (const AnimKeyframe& kf : ch.keys)
                {
                    bool eligible = false;
                    switch (chosen)
                    {
                        case RetimeMode::Scale:
                        case RetimeMode::Translate:
                            eligible = (kf.time > m_RetimePivotTime);
                            break;
                        case RetimeMode::RegionScaleRight:
                        case RetimeMode::RegionScaleLeft:
                        case RetimeMode::RegionTranslate:
                            eligible = (kf.time >= m_RetimeRegionLeft && kf.time <= m_RetimeRegionRight);
                            break;
                        default:
                            break;
                    }
                    if (!eligible) continue;
                    if (restrictToSelection && !isKeySelected(ci, kf.time))
                        continue;
                    m_RetimePreview.push_back(RetimePreviewEntry{ci, kf.time, kf.time, kf.time});
                    farthest = std::max(farthest, kf.time - m_RetimePivotTime);
                }
            }
            m_RetimeReferenceDist = (chosen == RetimeMode::Scale) ? farthest
                : (chosen == RetimeMode::RegionScaleRight ? (m_RetimeRegionRight - m_RetimeRegionLeft)
                : (chosen == RetimeMode::RegionScaleLeft  ? (m_RetimeRegionRight - m_RetimeRegionLeft) : 0.0f));

            if (m_OnEditStarted) m_OnEditStarted(EditGesture::Retime);
            e.Capture(this);
            e.Stop();
            MarkDirty(VisualDirty);
            return;
        }

        // Hover-update for the active region's handles/body when no drag is
        // in progress. Updates m_RetimeHover so the visual layer can brighten
        // the part the cursor is over.
        if (e.Id == kEventMouseMove && !m_RetimeDragging && m_RetimeRegionActive)
        {
            const float lpx = timeToPx(m_RetimeRegionLeft);
            const float rpx = timeToPx(m_RetimeRegionRight);
            RetimeHover h = RetimeHover::None;
            if (localY >= 0.0f && localY <= H)
            {
                if (std::abs(localX - lpx) <= kHandleHitPx)
                    h = RetimeHover::LeftHandle;
                else if (std::abs(localX - rpx) <= kHandleHitPx)
                    h = RetimeHover::RightHandle;
                else if (localX >= lpx && localX <= rpx)
                    h = RetimeHover::Body;
            }
            if (h != m_RetimeHover)
            {
                m_RetimeHover = h;
                MarkDirty(VisualDirty);
            }
            // Don't e.Stop() — let other hover-tracking code see the move too.
        }
        if (e.Id == kEventMouseMove && m_RetimeDragging)
        {
            const float newPx = std::clamp(localX, 0.0f, W);
            const float dragDelta = (newPx - m_RetimeAnchorX) / W * (m_RangeEnd - m_RangeStart);
            m_RetimeDeltaSeconds = dragDelta;
            const bool snapToFrame = m_SnapTime || controlHeld;
            // RegionTranslate is initiated by clicking inside the body — the
            // user might just want to dismiss. Hold off on touching keys
            // until the cursor actually leaves the click position.
            constexpr float kClickVsDragPx = 3.0f;
            if (m_RetimeMode == RetimeMode::RegionTranslate && !m_RetimeDragConfirmed)
            {
                if (std::abs(newPx - m_RetimeAnchorX) <= kClickVsDragPx)
                {
                    e.Stop();
                    return;
                }
                m_RetimeDragConfirmed = true;
            }

            if (m_RetimeMode == RetimeMode::DefineRegion)
            {
                // Live-extend the rubber band. Sorted bounds are computed from
                // anchorX/cursorX so the user can drag in either direction.
                const float anchorTime = pxToTime(m_RetimeAnchorX);
                const float cursorTime = pxToTime(newPx);
                m_RetimeRegionLeft  = std::min(anchorTime, cursorTime);
                m_RetimeRegionRight = std::max(anchorTime, cursorTime);
                MarkDirty(VisualDirty);
                e.Stop();
                return;
            }

            // For region-scale modes the reference is the original region
            // width; clamp the floor on the resulting scale so the timeline
            // doesn't invert (and so very narrow regions don't go to NaN).
            float scale = 1.0f;
            switch (m_RetimeMode)
            {
                case RetimeMode::Scale:
                    if (m_RetimeReferenceDist > 1e-4f)
                        scale = std::max(0.05f, 1.0f + dragDelta / m_RetimeReferenceDist);
                    break;
                case RetimeMode::RegionScaleRight:
                    if (m_RetimeReferenceDist > 1e-4f)
                        scale = std::max(0.05f, 1.0f + dragDelta / m_RetimeReferenceDist);
                    break;
                case RetimeMode::RegionScaleLeft:
                    if (m_RetimeReferenceDist > 1e-4f)
                        scale = std::max(0.05f, 1.0f - dragDelta / m_RetimeReferenceDist);
                    break;
                default:
                    break;
            }
            m_RetimeScaleFactor = scale;

            for (RetimePreviewEntry& rc : m_RetimePreview)
            {
                float nt = rc.originalTime;
                switch (m_RetimeMode)
                {
                    case RetimeMode::Scale:
                        nt = m_RetimePivotTime + (rc.originalTime - m_RetimePivotTime) * scale;
                        if (nt <= m_RetimePivotTime) nt = m_RetimePivotTime + 1e-4f;
                        break;
                    case RetimeMode::Translate:
                        nt = rc.originalTime + dragDelta;
                        if (nt <= m_RetimePivotTime) nt = m_RetimePivotTime + 1e-4f;
                        break;
                    case RetimeMode::RegionScaleRight:
                        nt = m_RetimeRegionLeftOrig + (rc.originalTime - m_RetimeRegionLeftOrig) * scale;
                        break;
                    case RetimeMode::RegionScaleLeft:
                        nt = m_RetimeRegionRightOrig - (m_RetimeRegionRightOrig - rc.originalTime) * scale;
                        break;
                    case RetimeMode::RegionTranslate:
                        nt = rc.originalTime + dragDelta;
                        break;
                    default:
                        break;
                }
                if (snapToFrame) nt = ComputeTimeSnap(nt);
                rc.newTime = nt;
            }

            // Update region bounds in real time so handles render where the
            // user is dragging, not where they started.
            switch (m_RetimeMode)
            {
                case RetimeMode::RegionScaleRight:
                    m_RetimeRegionRight = std::max(m_RetimeRegionLeftOrig + 1e-4f,
                                                   m_RetimeRegionRightOrig + dragDelta);
                    break;
                case RetimeMode::RegionScaleLeft:
                    m_RetimeRegionLeft  = std::min(m_RetimeRegionRightOrig - 1e-4f,
                                                   m_RetimeRegionLeftOrig + dragDelta);
                    break;
                case RetimeMode::RegionTranslate:
                    m_RetimeRegionLeft  = m_RetimeRegionLeftOrig  + dragDelta;
                    m_RetimeRegionRight = m_RetimeRegionRightOrig + dragDelta;
                    break;
                default:
                    break;
            }

            // Live commit: push the keys that actually changed since the
            // previous MouseMove out to the asset so the graph re-paints
            // with the new positions. Skipped for DefineRegion (no keys move
            // during draw) and when nothing actually changed this frame.
            if (m_OnRetimeApplied && m_RetimeMode != RetimeMode::DefineRegion)
            {
                std::vector<RetimeChange> live;
                live.reserve(m_RetimePreview.size());
                for (const RetimePreviewEntry& rc : m_RetimePreview)
                {
                    if (std::abs(rc.newTime - rc.liveTime) > 1e-5f)
                        live.push_back(RetimeChange{rc.channel, rc.liveTime, rc.newTime});
                }
                if (!live.empty())
                {
                    const bool shiftRight = (dragDelta >= 0.0f);
                    std::sort(live.begin(), live.end(),
                              [shiftRight](const RetimeChange& a, const RetimeChange& b) {
                                  if (a.channel != b.channel) return a.channel < b.channel;
                                  return shiftRight ? (a.originalTime > b.originalTime)
                                                    : (a.originalTime < b.originalTime);
                              });
                    m_OnRetimeApplied(live);
                    // Advance liveTime so the next frame's diff is against
                    // the keyframes' actual current positions.
                    for (RetimePreviewEntry& rc : m_RetimePreview)
                        rc.liveTime = rc.newTime;
                }
            }

            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }

        if (e.Id == kEventMouseUp && m_RetimeDragging)
        {
            m_RetimeDragging = false;

            if (m_RetimeMode == RetimeMode::DefineRegion)
            {
                // Click vs. drag is decided by *pixel* distance, not time —
                // a time-based threshold breaks on wide timelines where a
                // 1-pixel jitter crosses any small ε.
                constexpr float kClickVsDragPx = 3.0f;
                const float pxDelta = std::abs(localX - m_RetimeAnchorX);
                if (pxDelta > kClickVsDragPx)
                {
                    // Real click-and-drag → use this gesture's own bounds and
                    // discard any pending in-point.
                    m_RetimeRegionActive = true;
                    m_RetimePendingIn    = false;
                }
                else if (m_RetimePendingIn)
                {
                    // Click without drag while a pending in-point exists →
                    // pair them to form the region (click→click in/out).
                    const float t1 = m_RetimePendingInTime;
                    const float t2 = pxToTime(m_RetimeAnchorX);
                    m_RetimeRegionLeft   = std::min(t1, t2);
                    m_RetimeRegionRight  = std::max(t1, t2);
                    m_RetimeRegionActive = (m_RetimeRegionRight - m_RetimeRegionLeft) > 1e-4f;
                    m_RetimePendingIn    = false;
                }
                else
                {
                    // First click of a click→click sequence: drop the pending
                    // in-point marker and wait for the second click.
                    m_RetimeRegionLeft   = m_RetimeRegionLeftOrig;
                    m_RetimeRegionRight  = m_RetimeRegionRightOrig;
                    m_RetimeRegionActive = m_RetimeRegionWasActive;
                    m_RetimePendingIn    = true;
                    m_RetimePendingInTime = pxToTime(m_RetimeAnchorX);
                }
                if (m_OnEditFinished) m_OnEditFinished(EditGesture::Retime);
                MarkDirty(VisualDirty);
                e.Stop();
                return;
            }

            // RegionTranslate that never crossed the drag threshold = a
            // "click on body" → dismiss the region (the alternate gesture
            // for the same input is body-drag-to-translate, handled live
            // during MouseMove).
            if (m_RetimeMode == RetimeMode::RegionTranslate && !m_RetimeDragConfirmed)
            {
                m_RetimeRegionActive = false;
                m_RetimePendingIn    = false;
            }

            // Per-MouseMove already streamed every change to the asset, so
            // there's nothing left to commit here. Just clean up state.
            m_RetimePreview.clear();
            m_RetimeReferenceDist = 0.0f;
            m_RetimeDeltaSeconds = 0.0f;
            m_RetimeScaleFactor = 1.0f;
            m_RetimeDragConfirmed = false;

            if (m_OnEditFinished) m_OnEditFinished(EditGesture::Retime);
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }

        // Escape: cancel the active drag (no commit) or, if a region is
        // active and we're idle, clear the region.
        if (e.Id == kEventKeyDown && e.Key == Input::kKeyCode_Escape)
        {
            if (m_RetimeDragging)
            {
                m_RetimeDragging = false;
                m_RetimePreview.clear();
                // Restore region to whatever it was before the gesture started.
                m_RetimeRegionLeft  = m_RetimeRegionLeftOrig;
                m_RetimeRegionRight = m_RetimeRegionRightOrig;
                m_RetimeRegionActive = m_RetimeRegionWasActive;
                if (m_OnEditFinished) m_OnEditFinished(EditGesture::Retime);
                MarkDirty(VisualDirty);
                e.Stop();
                return;
            }
            if (m_RetimeRegionActive || m_RetimePendingIn)
            {
                m_RetimeRegionActive = false;
                m_RetimePendingIn = false;
                MarkDirty(VisualDirty);
                e.Stop();
                return;
            }
        }
    }

    // ---- DrawCurve tool ---------------------------------------------------
    // Press-drag-release to paint a freehand curve onto the selected
    // channel/component. Each MouseMove records a (time, value) sample;
    // MouseUp commits the stroke as a sorted, deduped batch via
    // m_OnDrawCurveCommitted (the panel does the actual key replacement +
    // undo registration). Requires a selected channel — there's no obvious
    // target otherwise.
    if (m_ActiveTool == CurveTool::DrawCurve && m_Clip && W > 0.0f && H > 0.0f &&
        (e.Id != kEventMouseDown || e.Button == 0) && !m_Panning && !m_RightClickScrubbing)
    {
        auto pxToTime = [&](float px) {
            return m_RangeStart + (std::clamp(px, 0.0f, W) / W) * (m_RangeEnd - m_RangeStart);
        };
        auto pyToValue = [&](float py) {
            const float t = std::clamp(py / std::max(1.0f, H), 0.0f, 1.0f);
            return m_ValueMax - t * (m_ValueMax - m_ValueMin);
        };

        if (e.Id == kEventMouseDown && e.Button == 0 &&
            localX >= 0.0f && localX <= W && localY >= 0.0f && localY <= H)
        {
            if (m_SelectedChannel < 0)
            {
                // No target — bail without consuming the click so the user's
                // intent (probably "select something first") isn't masked.
                return;
            }
            m_DrawingCurve = true;
            m_DrawCurveChannel = m_SelectedChannel;
            m_DrawCurveComponent = m_SelectedComponent;
            m_DrawCurveSamples.clear();
            m_DrawCurveSamples.push_back({pxToTime(localX), pyToValue(localY)});
            if (m_OnEditStarted) m_OnEditStarted(EditGesture::Keyframe);
            e.Capture(this);
            e.Stop();
            MarkDirty(VisualDirty);
            return;
        }
        if (e.Id == kEventMouseMove && m_DrawingCurve)
        {
            const float t = pxToTime(localX);
            const float v = pyToValue(std::clamp(localY, 0.0f, H));
            // Keep samples monotonic in time. If the cursor backtracks left
            // of the previous sample we still record it but the commit step
            // sorts and dedupes; this means a backtrack overwrites in time
            // order rather than producing crossed keys.
            m_DrawCurveSamples.push_back({t, v});
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
        if (e.Id == kEventMouseUp && m_DrawingCurve)
        {
            m_DrawingCurve = false;
            // Sort by time and dedupe samples that landed on the same frame
            // (keeping the latest value — matches "draw over" expectations).
            std::sort(m_DrawCurveSamples.begin(), m_DrawCurveSamples.end(),
                      [](const DrawCurveSample& a, const DrawCurveSample& b) {
                          return a.time < b.time;
                      });
            for (size_t i = 1; i < m_DrawCurveSamples.size();)
            {
                if (std::abs(m_DrawCurveSamples[i].time - m_DrawCurveSamples[i - 1].time) < 1e-5f)
                {
                    m_DrawCurveSamples[i - 1] = m_DrawCurveSamples[i];
                    m_DrawCurveSamples.erase(m_DrawCurveSamples.begin() + static_cast<ptrdiff_t>(i));
                }
                else
                    ++i;
            }

            // Douglas–Peucker decimation: drop samples that fall close enough
            // to the chord between their neighbors. Without this, a fast
            // stroke produces one key per MouseMove (hundreds of them),
            // making the resulting curve look jagged and breaking
            // auto-tangents on every sample.
            if (m_DrawCurveSamples.size() > 2)
            {
                const float vRange = std::max(1e-6f, m_ValueMax - m_ValueMin);
                const float tRange = std::max(1e-6f, m_RangeEnd - m_RangeStart);
                // Tolerance ~0.6% of each axis — gives ~10–20 keys for a
                // moderately complex stroke and is roughly invisible at
                // typical zoom levels.
                const float tolT = tRange * 0.006f;
                const float tolV = vRange * 0.006f;
                std::vector<bool> keep(m_DrawCurveSamples.size(), false);
                keep.front() = true;
                keep.back() = true;
                std::vector<std::pair<size_t, size_t>> stack;
                stack.emplace_back(0, m_DrawCurveSamples.size() - 1);
                while (!stack.empty())
                {
                    auto [lo, hi] = stack.back();
                    stack.pop_back();
                    if (hi <= lo + 1) continue;
                    const auto& a = m_DrawCurveSamples[lo];
                    const auto& b = m_DrawCurveSamples[hi];
                    const float dt = b.time - a.time;
                    const float dv = b.value - a.value;
                    // Normalize axes before measuring chord-distance so the
                    // tolerance is direction-independent.
                    const float ndt = (std::abs(dt) > 1e-6f) ? (dt / tolT) : 0.0f;
                    const float ndv = (dv / tolV);
                    const float lenSq = ndt * ndt + ndv * ndv;
                    size_t worstIdx = 0;
                    float worst = 0.0f;
                    for (size_t i = lo + 1; i < hi; ++i)
                    {
                        const auto& p = m_DrawCurveSamples[i];
                        const float pt = (p.time - a.time) / tolT;
                        const float pv = (p.value - a.value) / tolV;
                        float d2;
                        if (lenSq < 1e-12f)
                            d2 = pt * pt + pv * pv;
                        else
                        {
                            const float t = std::clamp((pt * ndt + pv * ndv) / lenSq, 0.0f, 1.0f);
                            const float qx = t * ndt - pt;
                            const float qy = t * ndv - pv;
                            d2 = qx * qx + qy * qy;
                        }
                        if (d2 > worst) { worst = d2; worstIdx = i; }
                    }
                    if (worst > 1.0f) // tolerance == 1 in normalized units
                    {
                        keep[worstIdx] = true;
                        stack.emplace_back(lo, worstIdx);
                        stack.emplace_back(worstIdx, hi);
                    }
                }
                std::vector<DrawCurveSample> simplified;
                simplified.reserve(m_DrawCurveSamples.size());
                for (size_t i = 0; i < m_DrawCurveSamples.size(); ++i)
                    if (keep[i]) simplified.push_back(m_DrawCurveSamples[i]);
                m_DrawCurveSamples = std::move(simplified);
            }

            if (m_OnDrawCurveCommitted && m_DrawCurveSamples.size() >= 2)
                m_OnDrawCurveCommitted(m_DrawCurveChannel, m_DrawCurveComponent, m_DrawCurveSamples);
            m_DrawCurveSamples.clear();
            if (m_OnEditFinished) m_OnEditFinished(EditGesture::Keyframe);
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
    }

    // Lattice deform: activate or rebuild the lattice when the user makes a
    // non-empty selection that differs from the currently captured baseline.
    // After activation the lattice persists across selection clears (the
    // baseline owns its own copy of channel + key time + value, so dragging
    // works even after the user clicks empty space and the marquee box is
    // gone). The only ways out are toggling the tool off or re-selecting a
    // different non-empty key set.
    if (m_ActiveTool == CurveTool::Lattice && m_Clip && W > 0.0f && H > 0.0f)
    {
        if (!m_SelectedKeys.empty())
        {
            bool needsRebuild = !m_LatticeActive;
            if (!needsRebuild && m_LatticeDragIndex < 0)
            {
                if (m_LatticeBaselineKeyValues.size() != m_SelectedKeys.size())
                    needsRebuild = true;
                else
                {
                    for (size_t i = 0; i < m_SelectedKeys.size(); ++i)
                    {
                        if (m_LatticeBaselineKeyValues[i].Channel != m_SelectedKeys[i].Channel ||
                            !KeyTimesMatch(m_LatticeBaselineKeyValues[i].Time, m_SelectedKeys[i].KeyTime))
                        {
                            needsRebuild = true;
                            break;
                        }
                    }
                }
            }
            if (needsRebuild)
                RebuildLatticeFromSelection();
        }

        // Square half-extent for both hover detection and mouse-down picking.
        // Wider than the 6-7px keyframe pick radius so the lattice always
        // claims clicks near its CPs even when a keyframe sits underneath.
        constexpr float kLatticeCPHit = 14.0f;

        auto pickCP = [&](float lx, float ly) -> int
        {
            for (size_t ci = 0; ci < m_LatticePoints.size(); ++ci)
            {
                const float valRange = std::max(0.001f, m_ValueMax - m_ValueMin);
                const float cpLocalX = (m_LatticePoints[ci].Time - m_RangeStart) / rangeDuration * W;
                const float cpNy     = (m_LatticePoints[ci].Value - m_ValueMin) / valRange;
                const float cpLocalY = H - cpNy * H;
                if (std::abs(lx - cpLocalX) < kLatticeCPHit && std::abs(ly - cpLocalY) < kLatticeCPHit)
                    return static_cast<int>(ci);
            }
            return -1;
        };

        if (m_LatticeActive && e.Id == kEventMouseMove && m_LatticeDragIndex < 0)
        {
            const int newHover = pickCP(localX, localY);
            if (newHover != m_LatticeHoverIndex)
            {
                m_LatticeHoverIndex = newHover;
                MarkDirty(VisualDirty);
            }
        }

        if (m_LatticeActive && e.Id == kEventMouseDown && e.Button == 0)
        {
            const int hit = pickCP(localX, localY);
            if (hit >= 0)
            {
                m_LatticeDragIndex = hit;
                m_LatticeHoverIndex = hit;
                m_LatticeAnchorY   = localY;
                // Snapshot channel+time+baseline before OnEditStarted can replace the clip
                // (which would clear m_SelectedKeys). The top-level drag handlers use this.
                // Drag-key snapshot is just a copy of the baseline (which
                // already carries channel + time + value), so the lattice
                // can keep deforming the originally captured keys even if
                // the user has since cleared the selection.
                m_LatticeDragKeys = m_LatticeBaselineKeyValues;
                if (m_OnEditStarted) m_OnEditStarted(EditGesture::Lattice);
                e.Capture(this);
                e.Stop();
                return;
            }
        }
    }
    else if (m_ActiveTool != CurveTool::Lattice)
    {
        m_LatticeActive    = false;
        m_LatticeDragIndex = -1;
        m_LatticeHoverIndex = -1;
        m_LatticePoints.clear();
    }

    if (e.Id == kEventMouseDown && e.Button == 0 && m_Clip && m_SelectedChannel >= 0 &&
        static_cast<size_t>(m_SelectedChannel) < m_Clip->GetChannels().size() && W > 0.0f && H > 0.0f)
    {
        const AnimChannel& channel = m_Clip->GetChannels()[static_cast<size_t>(m_SelectedChannel)];

        // Build m_DragKeys from each selected key's own channel (supports multi-curve selection).
        auto buildScaleDragKeys = [&]()
        {
            m_DragKeys.clear();
            m_DragKeys.reserve(m_SelectedKeys.size());
            for (const SelectedKey& selectedKey : m_SelectedKeys)
            {
                if (selectedKey.Channel < 0 || static_cast<size_t>(selectedKey.Channel) >= m_Clip->GetChannels().size())
                    continue;
                const AnimChannel& dragCh = m_Clip->GetChannels()[static_cast<size_t>(selectedKey.Channel)];
                const int selectedIndex = FindKeyIndexForTime(dragCh, selectedKey.KeyTime);
                if (selectedIndex < 0)
                    continue;
                const float t = dragCh.keys[static_cast<size_t>(selectedIndex)].time;
                const float v = GetChannelComponentValue(dragCh, dragCh.keys[static_cast<size_t>(selectedIndex)], m_SelectedComponent);
                m_DragKeys.push_back({selectedKey.Channel, t, v, t, v});
            }
        };

        if (selectionBounds.Valid &&
            localX >= selectionBounds.TimeHandleLeft && localX <= selectionBounds.TimeHandleRight &&
            localY >= selectionBounds.TimeHandleTop && localY <= selectionBounds.TimeHandleBottom &&
            m_OnKeyframeEdited && m_SelectedKeys.size() >= 2u)
        {
            m_ScalingSelectionTime = true;
            m_ScalingSelectionValue = false;
            m_ScaleAnchorTime = selectionBounds.MinTime;
            m_ScaleInitialDuration = std::max(0.001f, selectionBounds.MaxTime - selectionBounds.MinTime);
            buildScaleDragKeys();
            if (m_OnEditStarted)
                m_OnEditStarted(EditGesture::Keyframe);
            e.Capture(this);
            e.Stop();
            return;
        }
        if (selectionBounds.Valid &&
            localX >= selectionBounds.ValueHandleLeft && localX <= selectionBounds.ValueHandleRight &&
            localY >= selectionBounds.ValueHandleTop && localY <= selectionBounds.ValueHandleBottom &&
            m_OnKeyframeEdited && m_SelectedKeys.size() >= 2u)
        {
            m_ScalingSelectionTime = false;
            m_ScalingSelectionValue = true;
            m_ScaleValueFromBottom = false;
            m_ScaleAnchorValue = 0.5f * (selectionBounds.MinValue + selectionBounds.MaxValue);
            m_ScaleInitialValueExtent = std::max(0.001f, selectionBounds.MaxValue - m_ScaleAnchorValue);
            buildScaleDragKeys();
            if (m_OnEditStarted)
                m_OnEditStarted(EditGesture::Keyframe);
            e.Capture(this);
            e.Stop();
            return;
        }
        if (selectionBounds.Valid &&
            localX >= selectionBounds.LeftTimeHandleLeft && localX <= selectionBounds.LeftTimeHandleRight &&
            localY >= selectionBounds.LeftTimeHandleTop && localY <= selectionBounds.LeftTimeHandleBottom &&
            m_OnKeyframeEdited && m_SelectedKeys.size() >= 2u)
        {
            m_ScalingSelectionTime = true;
            m_ScalingSelectionValue = false;
            m_ScaleTimeFromRight = true;
            m_ScaleAnchorTime = selectionBounds.MaxTime;
            m_ScaleInitialDuration = std::max(0.001f, selectionBounds.MaxTime - selectionBounds.MinTime);
            buildScaleDragKeys();
            if (m_OnEditStarted)
                m_OnEditStarted(EditGesture::Keyframe);
            e.Capture(this);
            e.Stop();
            return;
        }
        if (selectionBounds.Valid &&
            localX >= selectionBounds.BottomValueHandleLeft && localX <= selectionBounds.BottomValueHandleRight &&
            localY >= selectionBounds.BottomValueHandleTop && localY <= selectionBounds.BottomValueHandleBottom &&
            m_OnKeyframeEdited && m_SelectedKeys.size() >= 2u)
        {
            m_ScalingSelectionTime = false;
            m_ScalingSelectionValue = true;
            m_ScaleValueFromBottom = true;
            m_ScaleAnchorValue = selectionBounds.MaxValue;
            m_ScaleInitialValueExtent = std::max(0.001f, selectionBounds.MaxValue - selectionBounds.MinValue);
            buildScaleDragKeys();
            if (m_OnEditStarted)
                m_OnEditStarted(EditGesture::Keyframe);
            e.Capture(this);
            e.Stop();
            return;
        }

        // Hit-test keys on all visible channels and all drawn components first.
        // Keyframes take precedence over handles (tested below) when both are near.
        const bool testAllComps = m_ShowAllComponents || m_VisibleChannels.size() > 1u;
        constexpr float kKeyHitDistanceSq = 225.0f; // 15px radius (increased from 12px)
        float bestDistanceSq = kKeyHitDistanceSq;
        int hitKeyIndex = -1;
        int hitKeyChannel = m_SelectedChannel;
        for (int chIdx : m_VisibleChannels)
        {
            if (chIdx < 0 || static_cast<size_t>(chIdx) >= m_Clip->GetChannels().size())
                continue;
            const AnimChannel& testCh = m_Clip->GetChannels()[static_cast<size_t>(chIdx)];
            const uint32 numComp = GetComponentCount(testCh);
            const uint32 compStart = testAllComps ? 0u : std::min(m_SelectedComponent, numComp - 1u);
            const uint32 compEnd   = testAllComps ? numComp : compStart + 1u;
            for (size_t keyIndex = 0; keyIndex < testCh.keys.size(); ++keyIndex)
            {
                const float px = (testCh.keys[keyIndex].time - m_RangeStart) / rangeDuration * W;
                for (uint32 c = compStart; c < compEnd; ++c)
                {
                    const float keyValue = GetChannelComponentValue(testCh, testCh.keys[keyIndex], c);
                    const float py = H - ((keyValue - m_ValueMin) / valueRange) * H;
                    const float dx = localX - px;
                    const float dy = localY - py;
                    const float distanceSq = dx * dx + dy * dy;
                    if (distanceSq <= bestDistanceSq)
                    {
                        bestDistanceSq = distanceSq;
                        hitKeyIndex = static_cast<int>(keyIndex);
                        hitKeyChannel = chIdx;
                    }
                }
            }
        }

        // Handle hit-test only if no keyframe was hit (keyframes take precedence).
        const int selectedKeyIndex = FindKeyIndexForTime(channel, m_SelectedKeyTime);
        if (hitKeyIndex < 0 && channel.interp == AnimInterp::CubicSpline && selectedKeyIndex >= 0 &&
            static_cast<size_t>(selectedKeyIndex) < channel.keys.size() && m_OnTangentEdited)
        {
            constexpr float kHandleHitDistanceSq = 144.0f;
            float bestHandleDistanceSq = kHandleHitDistanceSq;
            bool hitIncomingHandle = false;
            bool hitHandle = false;

            const TangentHandlePoint inHandle =
                GetTangentHandlePoint(channel, m_SelectedComponent, static_cast<size_t>(selectedKeyIndex), true);
            if (inHandle.Valid)
            {
                const float dx = localX - ((inHandle.Time - m_RangeStart) / rangeDuration * W);
                const float dy = localY - (H - ((inHandle.Value - m_ValueMin) / valueRange) * H);
                const float distanceSq = dx * dx + dy * dy;
                if (distanceSq <= bestHandleDistanceSq)
                {
                    bestHandleDistanceSq = distanceSq;
                    hitIncomingHandle = true;
                    hitHandle = true;
                }
            }

            const TangentHandlePoint outHandle =
                GetTangentHandlePoint(channel, m_SelectedComponent, static_cast<size_t>(selectedKeyIndex), false);
            if (outHandle.Valid)
            {
                const float dx = localX - ((outHandle.Time - m_RangeStart) / rangeDuration * W);
                const float dy = localY - (H - ((outHandle.Value - m_ValueMin) / valueRange) * H);
                const float distanceSq = dx * dx + dy * dy;
                if (distanceSq <= bestHandleDistanceSq)
                {
                    bestHandleDistanceSq = distanceSq;
                    hitIncomingHandle = false;
                    hitHandle = true;
                }
            }

            if (hitHandle)
            {
                m_DraggingInHandle = hitIncomingHandle;
                m_DraggingOutHandle = !hitIncomingHandle;
                m_DraggingLinkedHandles = false;
                // ALT breaks the tangent lock; default is mirrored (locked) tangents.
                m_FreeTangentDrag = (e.Mods & Input::kModAlt) != 0;
                m_PrimaryHandleDrag = {};
                m_SecondaryHandleDrag = {};
                m_HandleDragThresholdMet = false;
                m_HandleDragStartX = localX;
                m_HandleDragStartY = localY;
                e.Capture(this);
                e.Stop();
                return;
            }
        }

        const bool inTimeScaleZoneCurves =
            selectionBounds.Valid && m_SelectedKeys.size() >= 2u &&
            localX >= selectionBounds.TimeHandleLeft && localX <= selectionBounds.TimeHandleRight &&
            localY >= selectionBounds.TimeHandleTop && localY <= selectionBounds.TimeHandleBottom;
        const bool inValueScaleZoneCurves =
            selectionBounds.Valid && m_SelectedKeys.size() >= 2u &&
            localX >= selectionBounds.ValueHandleLeft && localX <= selectionBounds.ValueHandleRight &&
            localY >= selectionBounds.ValueHandleTop && localY <= selectionBounds.ValueHandleBottom;

        auto buildDragKeysFromSelection = [&]()
        {
            m_DragKeys.clear();
            m_DragKeys.reserve(m_SelectedKeys.size());
            for (const SelectedKey& selectedKey : m_SelectedKeys)
            {
                if (selectedKey.Channel < 0 || static_cast<size_t>(selectedKey.Channel) >= m_Clip->GetChannels().size())
                    continue;
                const AnimChannel& dragCh = m_Clip->GetChannels()[static_cast<size_t>(selectedKey.Channel)];
                const int selectedIndex = FindKeyIndexForTime(dragCh, selectedKey.KeyTime);
                if (selectedIndex < 0)
                    continue;
                const float t = dragCh.keys[static_cast<size_t>(selectedIndex)].time;
                const float v = GetChannelComponentValue(dragCh, dragCh.keys[static_cast<size_t>(selectedIndex)], m_SelectedComponent);
                m_DragKeys.push_back({selectedKey.Channel, t, v, t, v});
            }
        };

        if (hitKeyIndex < 0 && selectionBounds.Valid && m_OnKeyframeEdited && !m_SelectedKeys.empty() &&
            !inTimeScaleZoneCurves && !inValueScaleZoneCurves &&
            localX >= selectionBounds.Left && localX <= selectionBounds.Right &&
            localY >= selectionBounds.Top && localY <= selectionBounds.Bottom)
        {
            const float pointerTime =
                std::clamp(m_RangeStart + (clampedLocalX / W) * rangeDuration, m_RangeStart, m_RangeEnd);
            const float valueAlpha = 1.0f - std::clamp(localY / H, 0.0f, 1.0f);
            const float pointerValue = m_ValueMin + valueAlpha * valueRange;
            m_SelectedKeyTime = m_SelectedKeys.front().KeyTime;
            m_SelectedKeyIndex = -1;
            m_DragCurrentTime = pointerTime;
            m_DragCurrentValue = pointerValue;
            m_DraggingKey = true;
            m_DragThresholdMet = false;
            m_DragStartLocalX = localX;
            m_DragStartLocalY = localY;
            m_DragConstrained = (e.Mods & Input::kModShift) != 0;
            buildDragKeysFromSelection();
            e.Capture(this);
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }

        if (hitKeyIndex >= 0 && m_OnKeyframeEdited)
        {
            const AnimChannel& hitCh = m_Clip->GetChannels()[static_cast<size_t>(hitKeyChannel)];
            const float hitKeyTime = hitCh.keys[static_cast<size_t>(hitKeyIndex)].time;
            const bool wasSelected = ContainsSelection(m_SelectedKeys, hitKeyChannel, hitKeyTime);
            if (controlHeld)
            {
                // Ctrl+click: toggle this key without clearing the rest
                if (wasSelected)
                {
                    auto it = std::find_if(m_SelectedKeys.begin(), m_SelectedKeys.end(),
                        [hitKeyChannel, hitKeyTime](const SelectedKey& k)
                        { return k.Channel == hitKeyChannel && KeyTimesMatch(k.KeyTime, hitKeyTime); });
                    if (it != m_SelectedKeys.end()) m_SelectedKeys.erase(it);
                }
                else
                    m_SelectedKeys.push_back({hitKeyChannel, hitKeyTime});
                if (m_OnSelectionChanged) m_OnSelectionChanged();
                MarkDirty(VisualDirty);
                e.Stop();
                return;
            }
            if (!shiftHeld)
            {
                if (!wasSelected)
                    m_SelectedKeys = {{hitKeyChannel, hitKeyTime}};
            }
            else if (!wasSelected)
                m_SelectedKeys.push_back({hitKeyChannel, hitKeyTime});

            m_SelectedChannel = hitKeyChannel;
            m_SelectedKeyIndex = hitKeyIndex;
            m_SelectedKeyTime = hitKeyTime;
            m_DragCurrentTime = hitKeyTime;
            m_DragCurrentValue = GetChannelComponentValue(hitCh,
                                                         hitCh.keys[static_cast<size_t>(hitKeyIndex)],
                                                         m_SelectedComponent);
            m_DraggingKey = true;
            m_DragThresholdMet = false;
            m_DragStartLocalX = localX;
            m_DragStartLocalY = localY;
            m_DragConstrained = (e.Mods & Input::kModShift) != 0;
            buildDragKeysFromSelection();
            e.Capture(this);
            if (m_OnKeyframeSelected)
            {
                m_OnKeyframeSelected(m_SelectedChannel,
                                     static_cast<int>(m_SelectedComponent),
                                     hitKeyIndex,
                                     hitKeyTime);
            }
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
    }
    if (e.Id == kEventMouseDown && e.Button == 0 && W > 0.0f)
    {
        const float clickTime = m_RangeStart + (localX / W) * rangeDuration;

        // Key insertion mode: insert a key at the clicked time on the selected channel
        // and immediately start dragging it
        if (m_KeyInsertionEnabled && m_OnInsertKeyAt && m_OnKeyframeEdited && m_SelectedChannel >= 0 && m_Clip &&
            static_cast<size_t>(m_SelectedChannel) < m_Clip->GetChannels().size())
        {
            // Insert the key
            m_OnInsertKeyAt(m_SelectedChannel, clickTime);

            // Find the newly inserted key (it should be at clickTime)
            const AnimChannel& channel = m_Clip->GetChannels()[static_cast<size_t>(m_SelectedChannel)];
            const int keyIndex = FindKeyIndexForTime(channel, clickTime);
            if (keyIndex >= 0 && static_cast<size_t>(keyIndex) < channel.keys.size())
            {
                const float keyTime = channel.keys[static_cast<size_t>(keyIndex)].time;
                const float keyValue = GetChannelComponentValue(channel, channel.keys[static_cast<size_t>(keyIndex)], m_SelectedComponent);

                // Select only this key
                m_SelectedKeys.clear();
                m_SelectedKeys.push_back({m_SelectedChannel, keyTime});
                m_SelectedKeyTime = keyTime;
                m_SelectedKeyIndex = keyIndex;

                // Notify selection changed
                if (m_OnKeyframeSelected)
                    m_OnKeyframeSelected(m_SelectedChannel, m_SelectedComponent, keyIndex, keyTime);
                if (m_OnSelectionChanged)
                    m_OnSelectionChanged();

                // Set up drag state for immediate dragging
                m_DragKeys.clear();
                m_DragKeys.push_back({m_SelectedChannel, keyTime, keyValue, keyTime, keyValue});
                m_DragCurrentTime = keyTime;
                m_DragCurrentValue = keyValue;
                m_DraggingKey = true;
                m_DragThresholdMet = false;
                m_DragStartLocalX = localX;
                m_DragStartLocalY = localY;

                if (m_OnEditStarted)
                    m_OnEditStarted(EditGesture::Keyframe);

                e.Capture(this);
                MarkDirty(VisualDirty);
                e.Stop();
                return;
            }
            e.Stop();
            return;
        }

        if (!shiftHeld && !controlHeld)
        {
            m_SelectedKeys.clear();
            m_SelectedKeyIndex = -1;
            m_SelectedKeyTime = -1.0f;
        }
        m_PendingEmptyAction = true;
        m_BoxSelectModifierDown = controlHeld;
        m_EmptyActionStartX = localX;
        m_EmptyActionStartY = localY;
        m_BoxStartX = localX;
        m_BoxStartY = localY;
        m_BoxEndX = localX;
        m_BoxEndY = localY;
        // Shift: additive seed; Ctrl: toggle seed (keeps current selection, toggles what's in box)
        m_BoxSelectionSeed = (shiftHeld || controlHeld) ? m_SelectedKeys : std::vector<SelectedKey>{};
        e.Capture(this);
        e.Stop();
    }

    // Right-click on background: start scrubbing. Right-click on keyframe: context menu on release.
    if (e.Id == kEventMouseDown && e.Button == 1 && W > 0.0f && H > 0.0f)
    {
        bool onKey = false;
        if (m_Clip)
        {
            const auto& channels = m_Clip->GetChannels();
            constexpr float kHitRadiusPx = 12.0f;
            float bestDist = kHitRadiusPx * kHitRadiusPx;
            const float vRange = std::max(0.001f, m_ValueMax - m_ValueMin);
            for (int chIdx : m_VisibleChannels)
            {
                if (chIdx < 0 || static_cast<size_t>(chIdx) >= channels.size()) continue;
                const AnimChannel& ch = channels[static_cast<size_t>(chIdx)];
                for (const AnimKeyframe& key : ch.keys)
                {
                    const float kpx = (key.time - m_RangeStart) / rangeDuration * W;
                    const uint32 comp = m_SelectedComponent < 3u ? m_SelectedComponent : 0u;
                    const float val = (ch.path == AnimPath::Translation) ? key.translation[comp]
                                    : (ch.path == AnimPath::Rotation)    ? key.rotation[comp]
                                                                          : key.scale[comp];
                    const float kpy = H - (val - m_ValueMin) / vRange * H;
                    const float dx = localX - kpx, dy = localY - kpy;
                    if (dx * dx + dy * dy < bestDist) { bestDist = dx * dx + dy * dy; onKey = true; }
                }
            }
        }
        if (!onKey && m_OnSeekToTime)
        {
            m_RightClickScrubbing = true;
            m_Seeking = true;
            float t = m_RangeStart + (clampedLocalX / W) * rangeDuration;
            t = std::clamp(t, m_RangeStart, m_RangeEnd);
            m_OnSeekToTime(t);
            e.Capture(this);
            e.Stop();
        }
    }
    if (e.Id == kEventMouseMove && m_RightClickScrubbing && m_OnSeekToTime && W > 0.0f)
    {
        float t = m_RangeStart + (clampedLocalX / W) * rangeDuration;
        t = std::clamp(t, m_RangeStart, m_RangeEnd);
        m_OnSeekToTime(t);
        e.Stop();
        return;
    }

    // Right-click on keyframe: context menu on mouse-up (only when not scrubbing).
    if (e.Id == kEventMouseUp && e.Button == 1 && m_OnKeyframeContextMenu && W > 0.0f && H > 0.0f)
    {
        const bool wasScrubbing = m_RightClickScrubbing;
        m_RightClickScrubbing = false;
        m_Seeking = false;
        if (wasScrubbing)
        {
            e.Stop();
            return;
        }
        int hitChannel = -1;
        float hitKeyTime = -1.0f;
        if (m_Clip)
        {
            const auto& channels = m_Clip->GetChannels();
            constexpr float kHitRadiusPx = 12.0f;
            float bestDist = kHitRadiusPx * kHitRadiusPx;
            const float vRange = std::max(0.001f, m_ValueMax - m_ValueMin);
            const bool rcAllComps = m_ShowAllComponents || m_VisibleChannels.size() > 1u;
            for (int chIdx : m_VisibleChannels)
            {
                if (chIdx < 0 || static_cast<size_t>(chIdx) >= channels.size()) continue;
                const AnimChannel& ch = channels[static_cast<size_t>(chIdx)];
                const uint32 numComp = GetComponentCount(ch);
                const uint32 compStart = rcAllComps ? 0u : std::min(m_SelectedComponent, numComp - 1u);
                const uint32 compEnd   = rcAllComps ? numComp : compStart + 1u;
                for (const AnimKeyframe& key : ch.keys)
                {
                    const float kpx = (key.time - m_RangeStart) / rangeDuration * W;
                    for (uint32 c = compStart; c < compEnd; ++c)
                    {
                        const float val = GetChannelComponentValue(ch, key, c);
                        const float kpy = H - (val - m_ValueMin) / vRange * H;
                        const float dx = localX - kpx, dy = localY - kpy;
                        const float dist = dx * dx + dy * dy;
                        if (dist < bestDist) { bestDist = dist; hitChannel = chIdx; hitKeyTime = key.time; }
                    }
                }
            }
        }
        if (hitChannel >= 0)
        {
            m_OnKeyframeContextMenu(hitChannel, hitKeyTime, e.X, e.Y);
            e.Stop();
        }
    }

    // Hover detection — runs on any mouse move when no drag/pan/seek is active.
    if (e.Id == kEventMouseMove &&
        !m_DraggingKey && !m_DraggingInHandle && !m_DraggingOutHandle &&
        !m_ScalingSelectionTime && !m_ScalingSelectionValue &&
        !m_BoxSelecting && !m_Panning && !m_Seeking &&
        m_Clip && m_SelectedChannel >= 0 && W > 0.0f && H > 0.0f &&
        static_cast<size_t>(m_SelectedChannel) < m_Clip->GetChannels().size())
    {
        const AnimChannel& hoverChannel = m_Clip->GetChannels()[static_cast<size_t>(m_SelectedChannel)];
        int   newHoverCh  = -1;
        float newHoverKT  = -1.0f;
        bool  newHoverIn  = false;
        bool  newHoverOut = false;

        // Keyframe hover first — nearest key within hit radius across all visible channels/components.
        // Keyframes take precedence over handles (tested below) when both are near.
        constexpr float kKeyHoverDistSq = 225.0f; // 15px radius (matches click hit-test)
        float bestDist = kKeyHoverDistSq;
        const bool hoverAllComps = m_ShowAllComponents || m_VisibleChannels.size() > 1u;
        for (int hChIdx : m_VisibleChannels)
        {
            if (hChIdx < 0 || static_cast<size_t>(hChIdx) >= m_Clip->GetChannels().size()) continue;
            const AnimChannel& hCh = m_Clip->GetChannels()[static_cast<size_t>(hChIdx)];
            const uint32 numComp = GetComponentCount(hCh);
            const uint32 compStart = hoverAllComps ? 0u : std::min(m_SelectedComponent, numComp - 1u);
            const uint32 compEnd   = hoverAllComps ? numComp : compStart + 1u;
            for (size_t ki = 0; ki < hCh.keys.size(); ++ki)
            {
                const float px = (hCh.keys[ki].time - m_RangeStart) / rangeDuration * W;
                for (uint32 c = compStart; c < compEnd; ++c)
                {
                    const float kv = GetChannelComponentValue(hCh, hCh.keys[ki], c);
                    const float py = H - (kv - m_ValueMin) / valueRange * H;
                    const float dx = localX - px, dy = localY - py;
                    const float dist = dx * dx + dy * dy;
                    if (dist <= bestDist) { bestDist = dist; newHoverCh = hChIdx; newHoverKT = hCh.keys[ki].time; }
                }
            }
        }

        // Handle hover only if no keyframe is hovered (keyframes take precedence).
        const int selKeyIdx = FindKeyIndexForTime(hoverChannel, m_SelectedKeyTime);
        if (newHoverCh < 0 && hoverChannel.interp == AnimInterp::CubicSpline && selKeyIdx >= 0 &&
            static_cast<size_t>(selKeyIdx) < hoverChannel.keys.size())
        {
            constexpr float kHandleHoverDistSq = 144.0f; // 12px radius (matches click hit-test)
            const TangentHandlePoint inH  = GetTangentHandlePoint(hoverChannel, m_SelectedComponent,
                                                                   static_cast<size_t>(selKeyIdx), true);
            const TangentHandlePoint outH = GetTangentHandlePoint(hoverChannel, m_SelectedComponent,
                                                                   static_cast<size_t>(selKeyIdx), false);
            if (inH.Valid)
            {
                const float hx = (inH.Time - m_RangeStart) / rangeDuration * W;
                const float hy = H - (inH.Value - m_ValueMin) / valueRange * H;
                const float dx = localX - hx, dy = localY - hy;
                if (dx * dx + dy * dy <= kHandleHoverDistSq) newHoverIn = true;
            }
            if (!newHoverIn && outH.Valid)
            {
                const float hx = (outH.Time - m_RangeStart) / rangeDuration * W;
                const float hy = H - (outH.Value - m_ValueMin) / valueRange * H;
                const float dx = localX - hx, dy = localY - hy;
                if (dx * dx + dy * dy <= kHandleHoverDistSq) newHoverOut = true;
            }
        }

        HoveredSelHandle newHoveredSel = HoveredSelHandle::None;
        if (m_SelectedKeys.size() >= 2u)
        {
            if (selectionBounds.Valid)
            {
                if (localX >= selectionBounds.TimeHandleLeft && localX <= selectionBounds.TimeHandleRight &&
                    localY >= selectionBounds.TimeHandleTop  && localY <= selectionBounds.TimeHandleBottom)
                    newHoveredSel = HoveredSelHandle::Right;
                else if (localX >= selectionBounds.LeftTimeHandleLeft && localX <= selectionBounds.LeftTimeHandleRight &&
                         localY >= selectionBounds.LeftTimeHandleTop  && localY <= selectionBounds.LeftTimeHandleBottom)
                    newHoveredSel = HoveredSelHandle::Left;
                else if (localX >= selectionBounds.ValueHandleLeft && localX <= selectionBounds.ValueHandleRight &&
                         localY >= selectionBounds.ValueHandleTop  && localY <= selectionBounds.ValueHandleBottom)
                    newHoveredSel = HoveredSelHandle::Top;
                else if (localX >= selectionBounds.BottomValueHandleLeft && localX <= selectionBounds.BottomValueHandleRight &&
                         localY >= selectionBounds.BottomValueHandleTop  && localY <= selectionBounds.BottomValueHandleBottom)
                    newHoveredSel = HoveredSelHandle::Bottom;
            }
        }

        if (m_HoverKeyChannel != newHoverCh || !KeyTimesMatch(m_HoverKeyTime, newHoverKT) ||
            m_HoverInHandle != newHoverIn || m_HoverOutHandle != newHoverOut ||
            m_HoveredSelHandle != newHoveredSel)
        {
            m_HoverKeyChannel  = newHoverCh;
            m_HoverKeyTime     = newHoverKT;
            m_HoverInHandle    = newHoverIn;
            m_HoverOutHandle   = newHoverOut;
            m_HoveredSelHandle = newHoveredSel;
            MarkDirty(VisualDirty);
        }
    }

    if (e.Id == kEventMouseLeave)
    {
        if (m_HoverKeyChannel >= 0 || m_HoverInHandle || m_HoverOutHandle ||
            m_HoveredSelHandle != HoveredSelHandle::None)
        {
            m_HoverKeyChannel  = -1;
            m_HoverKeyTime     = -1.0f;
            m_HoverInHandle    = false;
            m_HoverOutHandle   = false;
            m_HoveredSelHandle = HoveredSelHandle::None;
            MarkDirty(VisualDirty);
        }
    }
}

void CurvesGraphView::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
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

    // x/y/W/H are in physical pixels (logical * contentScale). Cache both so
    // that OnEvent can use the logical layout values for hit testing while
    // drawing emits correct physical-pixel primitives.
    const float cs = ctx.ContentScale > 0.0f ? ctx.ContentScale : 1.0f;
    m_ContentScale = cs;
    m_ContentX = x;
    m_ContentY = y;
    m_ContentW = W;
    m_ContentH = H;

    const float rangeDuration = std::max(0.001f, m_RangeEnd - m_RangeStart);
    const float valueRange    = std::max(0.001f, m_ValueMax - m_ValueMin);
    auto toFloat3 = [](uint32_t argb, float& r, float& g, float& b) {
        r = ((argb >> 16) & 0xFF) / 255.0f;
        g = ((argb >> 8)  & 0xFF) / 255.0f;
        b = ((argb)        & 0xFF) / 255.0f;
    };
    float hR, hG, hB, vR, vG, vB, blR, blG, blB;
    toFloat3(m_GridHLineColor, hR, hG, hB);
    toFloat3(m_GridVLineColor, vR, vG, vB);
    toFloat3(m_BaselineColor, blR, blG, blB);
    const uint32_t hLineColor  = PackColor(hR, hG, hB, 0.35f);
    const uint32_t vLineColor  = PackColor(vR, vG, vB, 0.25f);
    const uint32_t zeroColor   = PackColor(blR, blG, blB, 0.75f);
    const uint32_t labelColor  = PackColor(0.65f, 0.65f, 0.65f, 0.80f);
    const float kFontSize = 10.0f * cs;
    const float kLabelPad = 6.0f * cs;

    // Choose nice value step: target ~5 horizontal grid lines.
    auto niceStep = [](float range, int targetDivs) -> float {
        const float rawStep = range / static_cast<float>(targetDivs);
        const float mag = std::pow(10.0f, std::floor(std::log10(rawStep)));
        const float norm = rawStep / mag;
        float nice = (norm <= 1.5f) ? 1.0f : (norm <= 3.5f) ? 2.0f : (norm <= 7.5f) ? 5.0f : 10.0f;
        return nice * mag;
    };
    if (m_ShowGrid && m_Clip)
    {
        const float vStep = niceStep(valueRange, 5);
        const float vFirst = std::ceil(m_ValueMin / vStep) * vStep;
        // Horizontal grid lines + value labels.
        for (float v = vFirst; v <= m_ValueMax + vStep * 0.5f; v += vStep)
        {
            const float py = y + H - (v - m_ValueMin) / valueRange * H;
            if (py < y - 1.0f || py > y + H + 1.0f) continue;
            const bool isZero = std::abs(v) < vStep * 0.01f;
            const float thickness = (isZero ? m_BaselineThickness : m_GridLineThickness) * cs;
            ctx.Emit(MakeRect(x, py - thickness * 0.5f, W, thickness, isZero ? zeroColor : hLineColor));

            // Value label on the left edge.
            char buf[32];
            if (std::abs(v) < 1e-5f) v = 0.0f;
            if (std::fmod(std::abs(v), 1.0f) < 1e-4f)
                std::snprintf(buf, sizeof(buf), "%g", v);
            else
                std::snprintf(buf, sizeof(buf), "%.2g", v);
            // EmitText takes LOGICAL fontSize and multiplies by contentScale internally.
            const float kLogicalFontSize = (cs > 0.0f) ? (kFontSize / cs) : kFontSize;
            ctx.EmitText(buf, x + kLabelPad, py - kFontSize - 4.0f * cs, kLogicalFontSize, labelColor, ctx.FontAtlas);
        }

        // Vertical grid lines (time axis).
        {
            // Snap to frame boundaries when FPS is set.
            if (m_Fps > 0.0f)
            {
                const float frameDuration = 1.0f / m_Fps;
                // Show a frame line every N frames, depending on zoom level.
                // Target roughly 8-12 major grid lines across the visible range.
                const float totalFrames = rangeDuration * m_Fps;
                int framesPerGrid = 1;
                if (totalFrames > 120.0f)        framesPerGrid = 10;
                else if (totalFrames > 60.0f)    framesPerGrid = 5;
                else if (totalFrames > 30.0f)    framesPerGrid = 2;
                
                const float tStep = frameDuration * static_cast<float>(framesPerGrid);
                const float tFirst = std::ceil(m_RangeStart / tStep) * tStep;
                for (float t = tFirst; t <= m_RangeEnd + tStep * 0.5f; t += tStep)
                {
                    const float px = x + (t - m_RangeStart) / rangeDuration * W;
                    if (px < x - 1.0f || px > x + W + 1.0f) continue;
                    ctx.Emit(MakeRect(px - m_GridLineThickness * cs * 0.5f, y, m_GridLineThickness * cs, H, vLineColor));
                }
            }
            else
            {
                // Fallback to nice time steps when FPS is not set.
                const float tStep = niceStep(rangeDuration, 8);
                const float tFirst = std::ceil(m_RangeStart / tStep) * tStep;
                for (float t = tFirst; t <= m_RangeEnd + tStep * 0.5f; t += tStep)
                {
                    const float px = x + (t - m_RangeStart) / rangeDuration * W;
                    if (px < x - 1.0f || px > x + W + 1.0f) continue;
                    ctx.Emit(MakeRect(px - m_GridLineThickness * cs * 0.5f, y, m_GridLineThickness * cs, H, vLineColor));
                }
            }
        }
    }

    // Draw playhead before early-returning so it's visible even with no clip loaded.
    if (W > 0.0f && m_CurrentTime >= m_RangeStart && m_CurrentTime <= m_RangeEnd)
    {
        float playheadPx = x + (m_CurrentTime - m_RangeStart) / rangeDuration * W;
        playheadPx = std::max(x, std::min(x + W, playheadPx));
        ctx.Emit(MakeRect(std::max(x, playheadPx - cs), y, 2.0f * cs, H, PackColor(1.0f, 0.4f, 0.0f, 0.95f)));
    }

    if (!m_Clip)
        return;

    const std::vector<AnimChannel>& channels = m_Clip->GetChannels();
    if (channels.empty())
        return;

    const float valMin = m_ValueMin;
    const float valMax = m_ValueMax;
    const float valRange = std::max(0.001f, valMax - valMin);

    constexpr uint32_t kCurveSegments = 64;
    const float kCurveThickness = m_CurveLineWidth * cs;

    auto timeToPx = [&](float t) -> float {
        return x + (t - m_RangeStart) / rangeDuration * W;
    };
    auto valueToPy = [&](float v) -> float {
        float ny = (v - valMin) / valRange;
        return y + H - ny * H;
    };

    const uint32_t pointColor         = m_KeyframeColor;
    const uint32_t pointHoverColor    = m_KeyframeColor;
    const uint32_t selectedPointColor = m_KeyframeSelectedColor;
    const uint32_t selectedHoverColor = m_KeyframeSelectedColor;

    auto& drawChannels = m_DrawChannels;
    drawChannels.clear();
    if (!m_VisibleChannels.empty())
    {
        drawChannels = m_VisibleChannels;
    }
    else if (m_SelectedChannel >= 0)
    {
        drawChannels.push_back(m_SelectedChannel);
    }
    else
    {
        drawChannels.reserve(channels.size());
    for (size_t ch = 0; ch < channels.size(); ++ch)
            drawChannels.push_back(static_cast<int>(ch));
    }

    const size_t editableChannelCount = drawChannels.size();
    AppendPinnedChannels(drawChannels, m_PinnedCurves);

    // Draw buffer snapshot curves (gray dashed overlay) before live curves.
    if (m_BufferSnapshotValid && !m_BufferValues.empty())
    {
        constexpr uint32 kBufColor = 0x55AAAAAA; // gray, ~33% alpha
        for (int ci : drawChannels)
        {
            if (ci < 0 || static_cast<size_t>(ci) >= channels.size()) continue;
            if (static_cast<size_t>(ci) >= m_BufferValues.size()) continue;
            const AnimChannel& ch = channels[static_cast<size_t>(ci)];
            const std::vector<float>& bufVals = m_BufferValues[static_cast<size_t>(ci)];
            const uint32 numComp = GetComponentCount(ch);
            if (ch.keys.size() * numComp != bufVals.size()) continue;
            for (uint32 c = 0; c < numComp; ++c)
            {
                float prevPx = -1.0f, prevPy = -1.0f;
                bool dashOn = true; float dashAccum = 0.0f;
                for (size_t ki = 0; ki < ch.keys.size(); ++ki)
                {
                    const float curPx = timeToPx(ch.keys[ki].time);
                    const float curPy = valueToPy(bufVals[ki * numComp + c]);
                    if (prevPx >= 0.0f)
                    {
                        const float segLen = std::sqrt((curPx - prevPx)*(curPx - prevPx) + (curPy - prevPy)*(curPy - prevPy));
                        dashAccum += segLen;
                        if (dashOn && dashAccum >= 5.0f * cs) { dashAccum -= 5.0f * cs; dashOn = false; }
                        else if (!dashOn && dashAccum >= 4.0f * cs) { dashAccum -= 4.0f * cs; dashOn = true; }
                        if (dashOn) ctx.Emit(MakeLine(prevPx, prevPy, curPx, curPy, 1.5f * cs, kBufColor));
                    }
                    prevPx = curPx; prevPy = curPy;
                }
            }
        }
    }

    // Precompute per-channel value ranges for Stacked/Normalized modes.
    struct ChannelValueRange { float vMin; float vMax; };
    std::vector<ChannelValueRange> channelRanges(channels.size(), {0.0f, 1.0f});
    for (int ci : drawChannels)
    {
        if (ci < 0 || static_cast<size_t>(ci) >= channels.size()) continue;
        const AnimChannel& ch = channels[static_cast<size_t>(ci)];
        float lo = std::numeric_limits<float>::max(), hi = -std::numeric_limits<float>::max();
        const uint32 nc = GetComponentCount(ch);
        for (const AnimKeyframe& k : ch.keys)
            for (uint32 c = 0; c < nc; ++c)
            {
                const float v = GetChannelComponentValue(ch, k, c);
                lo = std::min(lo, v); hi = std::max(hi, v);
            }
        if (lo > hi) { lo = -1.0f; hi = 1.0f; }
        if (std::abs(hi - lo) < 1e-4f) { const float mid = 0.5f * (lo + hi); lo = mid - 1.0f; hi = mid + 1.0f; }
        channelRanges[static_cast<size_t>(ci)] = {lo, hi};
    }
    const int validDrawCount = static_cast<int>(drawChannels.size());

    const bool multiChannelSelection = editableChannelCount > 1u || m_ShowAllComponents;
    int drawChannelSlot = 0; // lane index for Stacked mode
    for (int channelIndex : drawChannels)
    {
        const int currentSlot = drawChannelSlot++;
        if (channelIndex < 0 || static_cast<size_t>(channelIndex) >= channels.size())
            continue;

        const AnimChannel& channel = channels[static_cast<size_t>(channelIndex)];
        if (channel.keys.empty())
            continue;

        // Determine per-channel valueToPy for Stacked/Normalized modes.
        float laneY = y, laneH = H;
        std::function<float(float)> channelValueToPy;
        if (m_CurveViewMode == CurveViewMode::Stacked && validDrawCount > 1)
        {
            laneH = H / static_cast<float>(validDrawCount);
            laneY = y + currentSlot * laneH;
            const ChannelValueRange& r = channelRanges[static_cast<size_t>(channelIndex)];
            const float vr = std::max(0.001f, r.vMax - r.vMin);
            channelValueToPy = [laneY, laneH, r, vr](float v) {
                return laneY + laneH - ((v - r.vMin) / vr) * laneH;
            };
            // Divider between lanes.
            if (currentSlot > 0)
                ctx.Emit(MakeRect(x, laneY, W, 1.0f * cs, PackColor(0.4f, 0.4f, 0.4f, 0.4f)));
        }
        else if (m_CurveViewMode == CurveViewMode::Normalized)
        {
            const ChannelValueRange& r = channelRanges[static_cast<size_t>(channelIndex)];
            const float vr = std::max(0.001f, r.vMax - r.vMin);
            const float chMid = 0.5f * (r.vMin + r.vMax);
            channelValueToPy = [y, H, chMid, vr](float v) {
                const float norm = (v - chMid) / (0.5f * vr); // -1..1
                return y + H - ((norm + 1.0f) * 0.5f) * H;
            };
        }
        else
        {
            channelValueToPy = valueToPy;
        }

        // Find selected time range for this channel to highlight the curve between first/last selected key.
        float selFirstTime = std::numeric_limits<float>::max();
        float selLastTime  = -std::numeric_limits<float>::max();
        for (const SelectedKey& sk : m_SelectedKeys)
            if (sk.Channel == channelIndex)
            {
                selFirstTime = std::min(selFirstTime, sk.KeyTime);
                selLastTime  = std::max(selLastTime,  sk.KeyTime);
            }
        const bool channelHasSelRange = (selFirstTime <= selLastTime);

        const uint32 componentStart = multiChannelSelection ? 0u : std::min<uint32>(m_SelectedComponent, GetComponentCount(channel) - 1u);
        const uint32 componentEnd = multiChannelSelection ? GetComponentCount(channel) : (componentStart + 1u);
        // Pinned-only channels are appended behind the editable ones, so the slot decides.
        const bool channelEditable = static_cast<size_t>(currentSlot) < editableChannelCount;
        for (uint32 componentIndex = 0; componentIndex < GetComponentCount(channel); ++componentIndex)
        {
            const bool editable = channelEditable && componentIndex >= componentStart && componentIndex < componentEnd;
            if (!editable && !IsCurvePinned(m_PinnedCurves, channelIndex, componentIndex)) continue;
            const bool hasSelRange = editable && channelHasSelRange;
            struct KeyPt
            {
                float t;
                float v;
                size_t KeyIndex;
            };
            std::vector<KeyPt> pts;
            pts.reserve(channel.keys.size());
            for (size_t keyIndex = 0; keyIndex < channel.keys.size(); ++keyIndex)
            {
                const AnimKeyframe& keyframe = channel.keys[keyIndex];
                pts.push_back({keyframe.time, GetChannelComponentValue(channel, keyframe, componentIndex), keyIndex});
            }

            const bool emphasized =
                static_cast<int>(channelIndex) == m_SelectedChannel && componentIndex == m_SelectedComponent;
            uint32 curveColor = GetComponentColor(componentIndex, emphasized);
            {
                // Per-channel override takes priority; falls back to per-component.
                const uint32 perChannelKey = ((static_cast<uint32>(channelIndex) + 1u) << 8u) | componentIndex;
                auto overrideIt = m_ChannelColors.find(perChannelKey);
                if (overrideIt == m_ChannelColors.end())
                    overrideIt = m_ChannelColors.find(componentIndex);
                if (overrideIt != m_ChannelColors.end())
                {
                    // ColorPickerValueToArgb returns 0xAARRGGBB; PackColor expects 0xAABBGGRR.
                    const uint32 argb = overrideIt->second;
                    const uint32 r = (argb >> 16) & 0xFFu;
                    const uint32 g = (argb >> 8) & 0xFFu;
                    const uint32 b = argb & 0xFFu;
                    const uint32 a = static_cast<uint32>((emphasized ? 0.95f : 0.70f) * 255.0f);
                    curveColor = r | (g << 8) | (b << 16) | (a << 24);
                }
            }
            if (!editable) curveColor = kPinnedReferenceCurveColor;
        // Bright version of the curve color for the selected range.
        uint32 brightCurveColor = curveColor;
        if (hasSelRange)
        {
            const uint32 r = (curveColor)       & 0xFFu;
            const uint32 g = (curveColor >> 8)  & 0xFFu;
            const uint32 b = (curveColor >> 16) & 0xFFu;
            const uint32 a = (curveColor >> 24) & 0xFFu;
            constexpr float kBrightFactor = 0.55f;
            const uint32 nr = std::min(255u, r + static_cast<uint32>((255u - r) * kBrightFactor));
            const uint32 ng = std::min(255u, g + static_cast<uint32>((255u - g) * kBrightFactor));
            const uint32 nb = std::min(255u, b + static_cast<uint32>((255u - b) * kBrightFactor));
            const uint32 na = std::min(255u, a + 30u);
            brightCurveColor = nr | (ng << 8) | (nb << 16) | (na << 24);
        }
        const float kBrightThickness = kCurveThickness * 1.6f;

        for (size_t i = 0; i + 1 < pts.size(); ++i)
        {
            float t0 = pts[i].t, v0 = pts[i].v;
            float t1 = pts[i + 1].t, v1 = pts[i + 1].v;

            const bool segHighlighted = hasSelRange &&
                t0 >= selFirstTime - 1e-5f && t1 <= selLastTime + 1e-5f;
            const uint32 segColor     = segHighlighted ? brightCurveColor : curveColor;
            const float  segThickness = segHighlighted ? kBrightThickness  : kCurveThickness;

            // Check per-keyframe segment interpolation override for the starting key of this segment.
            const AnimKeyframe& keyframe0 = channel.keys[pts[i].KeyIndex];
            const AnimInterp segInterp = keyframe0.hasSegmentInterpOverride ? keyframe0.segmentInterp : channel.interp;

            if (segInterp == AnimInterp::Step)
            {
                const float startPx = timeToPx(t0);
                const float endPx = timeToPx(t1);
                const float startPy = channelValueToPy(v0);
                const float endPy = channelValueToPy(v1);
                ctx.Emit(MakeLine(startPx, startPy, endPx, startPy, segThickness, segColor));
                ctx.Emit(MakeLine(endPx, startPy, endPx, endPy, 1.2f, segColor));
                continue;
            }

            if (segInterp == AnimInterp::Linear || pts.size() < 3)
            {
                ctx.Emit(MakeLine(timeToPx(t0), channelValueToPy(v0), timeToPx(t1), channelValueToPy(v1), segThickness, segColor));
                continue;
            }

                const float dt = std::max(0.0001f, t1 - t0);
                const AnimKeyframe& keyframe1 = channel.keys[pts[i + 1u].KeyIndex];
                const float c0dt = dt * ClampHandleWeight(keyframe0.outWeight[componentIndex]);
                const float c1dt = dt * ClampHandleWeight(keyframe1.inWeight[componentIndex]);
                const float c0t = t0 + c0dt;
                const float c0v = v0 + keyframe0.outTangent[componentIndex] * c0dt;
                const float c1t = t1 - c1dt;
                const float c1v = v1 - keyframe1.inTangent[componentIndex] * c1dt;

            float prevPx = timeToPx(t0);
            float prevPy = channelValueToPy(v0);
            for (uint32_t seg = 1; seg <= kCurveSegments; ++seg)
            {
                    const float u = static_cast<float>(seg) / static_cast<float>(kCurveSegments);
                    const float u2 = u * u;
                    const float u3 = u2 * u;
                    const float inv = 1.0f - u;
                    const float inv2 = inv * inv;
                    const float inv3 = inv2 * inv;
                    const float bt = inv3 * t0 + 3.0f * inv2 * u * c0t + 3.0f * inv * u2 * c1t + u3 * t1;
                    const float bv = inv3 * v0 + 3.0f * inv2 * u * c0v + 3.0f * inv * u2 * c1v + u3 * v1;
                    const float curPx = timeToPx(bt);
                    const float curPy = channelValueToPy(bv);
                ctx.Emit(MakeLine(prevPx, prevPy, curPx, curPy, segThickness, segColor));
                prevPx = curPx;
                prevPy = curPy;
            }
        }

        // Draw pre/post infinity extrapolation tails as dashed lines.
        {
            constexpr uint32 kExtrapSegments = 48;
            constexpr float kDashOnPx  = 5.0f;
            constexpr float kDashOffPx = 4.0f;

            auto drawExtrapTail = [&](float tStart, float tEnd)
            {
                if (tStart >= tEnd) return;
                const uint32 extrapColor = (curveColor & 0x00FFFFFFu) | 0x66000000u; // 40% alpha
                const float totalDt = tEnd - tStart;
                const float dtPerSeg = totalDt / static_cast<float>(kExtrapSegments);
                float prevPx = timeToPx(tStart);
                float prevPy = channelValueToPy(EvaluateChannelExtrapolated(channel, componentIndex, tStart));
                float dashAccum = 0.0f;
                bool dashOn = true;
                for (uint32 s = 1; s <= kExtrapSegments; ++s)
                {
                    const float t = tStart + dtPerSeg * static_cast<float>(s);
                    const float curPx = timeToPx(t);
                    const float curPy = channelValueToPy(EvaluateChannelExtrapolated(channel, componentIndex, t));
                    const float segLen = std::sqrt((curPx - prevPx) * (curPx - prevPx) + (curPy - prevPy) * (curPy - prevPy));
                    dashAccum += segLen;
                    if (dashOn && dashAccum >= kDashOnPx)
                    {
                        ctx.Emit(MakeLine(prevPx, prevPy, curPx, curPy, kCurveThickness, extrapColor));
                        dashAccum -= kDashOnPx;
                        dashOn = false;
                    }
                    else if (!dashOn && dashAccum >= kDashOffPx)
                    {
                        dashAccum -= kDashOffPx;
                        dashOn = true;
                    }
                    else if (dashOn)
                    {
                        ctx.Emit(MakeLine(prevPx, prevPy, curPx, curPy, kCurveThickness, extrapColor));
                    }
                    prevPx = curPx;
                    prevPy = curPy;
                }
            };

            if (!pts.empty())
            {
                const float firstKeyTime = pts.front().t;
                const float lastKeyTime  = pts.back().t;
                if (channel.preInfinity != AnimExtrapolation::Constant && m_RangeStart < firstKeyTime)
                    drawExtrapTail(m_RangeStart, firstKeyTime);
                if (channel.postInfinity != AnimExtrapolation::Constant && m_RangeEnd > lastKeyTime)
                    drawExtrapTail(lastKeyTime, m_RangeEnd);
            }
        }

        if (!editable) continue; // A pinned reference curve has no interactive keys.

        for (const KeyPt& pt : pts)
        {
            const float px = timeToPx(pt.t);
            const float py = channelValueToPy(pt.v);
            const bool selected = ContainsSelection(m_SelectedKeys, static_cast<int>(channelIndex), pt.t);
            const bool hovered  = m_HoverKeyChannel == static_cast<int>(channelIndex) &&
                                  KeyTimesMatch(m_HoverKeyTime, pt.t);
            const float half = (hovered ? 4.5f : 3.0f) * cs;

            const uint32 keyColor = selected ? (hovered ? selectedHoverColor : selectedPointColor)
                                             : (hovered ? pointHoverColor    : pointColor);

            // Glow ring on hover.
            if (hovered)
            {
                const float glowR = 7.0f * cs;
                const uint32 glowColor = selected
                    ? PackColor(1.0f, 0.7f, 0.2f, 0.22f)
                    : PackColor(0.3f, 0.9f, 0.6f, 0.22f);
                const float gl = std::max(x,           px - glowR);
                const float gr = std::min(x + W,       px + glowR);
                const float gt = std::max(laneY,        py - glowR);
                const float gb = std::min(laneY + laneH, py + glowR);
                if (gr > gl && gb > gt)
                    ctx.Emit(MakeRect(gl, gt, gr - gl, gb - gt, glowColor));
            }

            // Determine key shape from tangent type.
            const AnimKeyframe& kf = channel.keys[pt.KeyIndex];
            const AnimTangentType outType = kf.outTangentType[componentIndex];
            const bool isAutoFamily = (outType == AnimTangentType::Auto   ||
                                       outType == AnimTangentType::Plateau ||
                                       outType == AnimTangentType::Clamped);
            const bool isRotation   = (channel.path == AnimPath::Rotation);

            if (isRotation)
            {
                // Circle: approximate with 8 triangles from center.
                constexpr int kSides = 8;
                for (int s = 0; s < kSides; ++s)
                {
                    const float a0 = (static_cast<float>(s)     / kSides) * 6.2831853f;
                    const float a1 = (static_cast<float>(s + 1) / kSides) * 6.2831853f;
                    const float ax = px + std::cos(a0) * half;
                    const float ay = py + std::sin(a0) * half;
                    const float bx = px + std::cos(a1) * half;
                    const float by = py + std::sin(a1) * half;
                    if (ax >= x && ax <= x + W && bx >= x && bx <= x + W)
                        ctx.Emit(MakeTriangle(px, py, ax, ay, bx, by, keyColor));
                }
            }
            else if (isAutoFamily)
            {
                // Diamond (rotated square): two triangles.
                const float L = std::max(x,           px - half);
                const float R = std::min(x + W,       px + half);
                const float T = std::max(laneY,        py - half);
                const float B = std::min(laneY + laneH, py + half);
                if (R > L && B > T)
                {
                    ctx.Emit(MakeTriangle(px, T, R, py, px, B, keyColor)); // right half
                    ctx.Emit(MakeTriangle(px, T, px, B, L, py, keyColor)); // left half
                }
            }
            else
            {
                // Square (default): axis-aligned rect.
                const float ptLeft   = std::max(x,           px - half);
                const float ptRight  = std::min(x + W,       px + half);
                const float ptTop    = std::max(laneY,        py - half);
                const float ptBottom = std::min(laneY + laneH, py + half);
                if (ptRight > ptLeft && ptBottom > ptTop)
                    ctx.Emit(MakeRect(ptLeft, ptTop, ptRight - ptLeft, ptBottom - ptTop, keyColor));
            }
        }

            const int selectedKeyIndex = FindKeyIndexForTime(channel, m_SelectedKeyTime);
            if (emphasized && channel.interp == AnimInterp::CubicSpline && selectedKeyIndex >= 0 &&
                static_cast<size_t>(selectedKeyIndex) < channel.keys.size())
            {
                const TangentHandlePoint inHandle =
                    GetTangentHandlePoint(channel, componentIndex, static_cast<size_t>(selectedKeyIndex), true);
                const TangentHandlePoint outHandle =
                    GetTangentHandlePoint(channel, componentIndex, static_cast<size_t>(selectedKeyIndex), false);
                const AnimKeyframe& selectedKey = channel.keys[static_cast<size_t>(selectedKeyIndex)];
                const float keyPx = timeToPx(selectedKey.time);
                const float keyPy = channelValueToPy(GetChannelComponentValue(channel, selectedKey, componentIndex));
                const bool isBroken = (selectedKey.tangentBroken & (1u << componentIndex)) != 0;
                const uint32 handleLineColor = isBroken
                    ? PackColor(0.95f, 0.80f, 0.20f, 0.45f)
                    : PackColor(0.9f,  0.9f,  0.9f,  0.45f);
                const uint32 handlePointColor = isBroken
                    ? PackColor(0.95f, 0.80f, 0.20f, 0.95f)
                    : PackColor(0.95f, 0.95f, 0.95f, 0.95f);

                auto drawHandle = [&](const TangentHandlePoint& handle, bool isIncoming)
                {
                    if (!handle.Valid)
                        return;

                    const float handlePx = timeToPx(handle.Time);
                    const float handlePy = channelValueToPy(handle.Value);
                    const bool hov = isIncoming ? m_HoverInHandle : m_HoverOutHandle;
                    const float lineW = (hov ? 1.5f : 1.0f) * cs;
                    const uint32 lineCol = hov
                        ? PackColor(1.0f, 1.0f, 1.0f, 0.75f)
                        : handleLineColor;
                    ctx.Emit(MakeLine(keyPx, keyPy, handlePx, handlePy, lineW, lineCol));

                    const float half  = (hov ? 4.5f : 3.0f) * cs;
                    const float left   = std::max(x,     handlePx - half);
                    const float right  = std::min(x + W, handlePx + half);
                    const float top    = std::max(y,     handlePy - half);
                    const float bottom = std::min(y + H, handlePy + half);
                    if (right > left && bottom > top)
                    {
                        if (hov)
                        {
                            const float handleGlowR = 7.0f * cs;
                            const float gl = std::max(x,     handlePx - handleGlowR);
                            const float gr = std::min(x + W, handlePx + handleGlowR);
                            const float gt = std::max(y,     handlePy - handleGlowR);
                            const float gb = std::min(y + H, handlePy + handleGlowR);
                            if (gr > gl && gb > gt)
                                ctx.Emit(MakeRect(gl, gt, gr - gl, gb - gt,
                                                  PackColor(1.0f, 1.0f, 1.0f, 0.15f)));
                        }
                        const uint32 ptCol = hov ? PackColor(1.0f, 1.0f, 1.0f, 1.0f) : handlePointColor;
                        ctx.Emit(MakeRect(left, top, right - left, bottom - top, ptCol));
                    }
                };

                drawHandle(inHandle,  true);
                drawHandle(outHandle, false);
            }
        }
    }

    const SelectionBounds drawBounds =
        ComputeSelectionBounds(m_Clip,
                               m_SelectedChannel,
                               m_SelectedComponent,
                               m_SelectedKeys,
                               x,
                               y,
                               W,
                               H,
                               m_RangeStart,
                               m_RangeEnd,
                               m_ValueMin,
                               m_ValueMax,
                               m_ShowAllComponents || m_VisibleChannels.size() > 1u,
                               cs);
    if (drawBounds.Valid)
    {
        const float bord = 1.0f * cs;
        const uint32 outlineColor = PackColor(1.0f, 0.72f, 0.25f, 0.95f);
        const uint32 fillColor = PackColor(1.0f, 0.72f, 0.25f, 0.10f);
        ctx.Emit(MakeRect(drawBounds.Left, drawBounds.Top, drawBounds.Right - drawBounds.Left,
                          drawBounds.Bottom - drawBounds.Top, fillColor));
        ctx.Emit(MakeRect(drawBounds.Left, drawBounds.Top, drawBounds.Right - drawBounds.Left, bord, outlineColor));
        ctx.Emit(MakeRect(drawBounds.Left, drawBounds.Bottom - bord, drawBounds.Right - drawBounds.Left, bord, outlineColor));
        ctx.Emit(MakeRect(drawBounds.Left, drawBounds.Top, bord, drawBounds.Bottom - drawBounds.Top, outlineColor));
        ctx.Emit(MakeRect(drawBounds.Right - bord, drawBounds.Top, bord, drawBounds.Bottom - drawBounds.Top, outlineColor));
        if (m_SelectedKeys.size() >= 2u)
        {
            const uint32 hoveredHandleColor = PackColor(1.0f, 1.0f, 1.0f, 1.0f);
            auto HandleColor = [&](HoveredSelHandle which) {
                return m_HoveredSelHandle == which ? hoveredHandleColor : outlineColor;
            };
            ctx.Emit(MakeRect(drawBounds.TimeHandleLeft,
                              drawBounds.TimeHandleTop,
                              drawBounds.TimeHandleRight - drawBounds.TimeHandleLeft,
                              drawBounds.TimeHandleBottom - drawBounds.TimeHandleTop,
                              HandleColor(HoveredSelHandle::Right)));
            ctx.Emit(MakeRect(drawBounds.LeftTimeHandleLeft,
                              drawBounds.LeftTimeHandleTop,
                              drawBounds.LeftTimeHandleRight - drawBounds.LeftTimeHandleLeft,
                              drawBounds.LeftTimeHandleBottom - drawBounds.LeftTimeHandleTop,
                              HandleColor(HoveredSelHandle::Left)));
            ctx.Emit(MakeRect(drawBounds.ValueHandleLeft,
                              drawBounds.ValueHandleTop,
                              drawBounds.ValueHandleRight - drawBounds.ValueHandleLeft,
                              drawBounds.ValueHandleBottom - drawBounds.ValueHandleTop,
                              HandleColor(HoveredSelHandle::Top)));
            ctx.Emit(MakeRect(drawBounds.BottomValueHandleLeft,
                              drawBounds.BottomValueHandleTop,
                              drawBounds.BottomValueHandleRight - drawBounds.BottomValueHandleLeft,
                              drawBounds.BottomValueHandleBottom - drawBounds.BottomValueHandleTop,
                              HandleColor(HoveredSelHandle::Bottom)));
        }
    }

    if (m_BoxSelecting)
    {
        // m_BoxStart/End are in logical (CSS) pixels from OnEvent; scale to physical here.
        const float bord = 1.0f * cs;
        const float boxLeft = x + std::min(m_BoxStartX, m_BoxEndX) * cs;
        const float boxTop = y + std::min(m_BoxStartY, m_BoxEndY) * cs;
        const float boxWidth = std::abs(m_BoxEndX - m_BoxStartX) * cs;
        const float boxHeight = std::abs(m_BoxEndY - m_BoxStartY) * cs;
        const uint32 boxOutline = PackColor(0.85f, 0.90f, 1.0f, 0.95f);
        const uint32 boxFill = PackColor(0.35f, 0.55f, 1.0f, 0.14f);
        ctx.Emit(MakeRect(boxLeft, boxTop, boxWidth, boxHeight, boxFill));
        ctx.Emit(MakeRect(boxLeft, boxTop, boxWidth, bord, boxOutline));
        ctx.Emit(MakeRect(boxLeft, boxTop + boxHeight - bord, boxWidth, bord, boxOutline));
        ctx.Emit(MakeRect(boxLeft, boxTop, bord, boxHeight, boxOutline));
        ctx.Emit(MakeRect(boxLeft + boxWidth - bord, boxTop, bord, boxHeight, boxOutline));
    }

    if (W > 0.0f && m_CurrentTime >= m_RangeStart && m_CurrentTime <= m_RangeEnd)
    {
        float playheadPx = x + (m_CurrentTime - m_RangeStart) / rangeDuration * W;
        playheadPx = std::max(x, std::min(x + W, playheadPx));
        ctx.Emit(MakeRect(std::max(x, playheadPx - cs), y, 2.0f * cs, H, PackColor(1.0f, 0.4f, 0.0f, 0.95f)));
    }

    // Lattice deform overlay: N control points + Catmull-Rom spline through
    // them, with a translucent band showing the influence range.
    if (m_ActiveTool == CurveTool::Lattice && m_LatticeActive && m_LatticePoints.size() >= 2u)
    {
        // Engine PackColor layout is RGBA-low-byte (R in bits 0-7, A in 24-31),
        // so a hex literal here reads "AABBGGRR" rather than the conventional
        // "AARRGGBB". Each constant is annotated with its #RRGGBB equivalent.
        constexpr uint32 kLatticeLineColor   = 0xFFFFBB66; // #66BBFF (solid blue, matches CP fill)
        constexpr uint32 kLatticeCurveColor  = 0xFFFFFFFF; // #FFFFFF (white)
        // Idle CP: solid blue square (border colour matches the fill so no
        // visible outline). Hovered/active CP brightens the fill and gains a
        // white outline so the affordance reads instantly against keyframes.
        constexpr uint32 kLatticeCPFill      = 0xFFFFBB66; // #66BBFF (saturated blue)
        constexpr uint32 kLatticeCPBorder    = 0xFFFFBB66; // matches fill (no visible outline)
        constexpr uint32 kLatticeCPHotFill   = 0xFFFFE5B3; // #B3E5FF (brighter cyan-blue on hover)
        constexpr uint32 kLatticeCPHotBorder = 0xFFFFFFFF; // #FFFFFF (white outline on hover)
        constexpr uint32 kLatticeBandColor   = 0x14FFBB66; // ~8% alpha of #66BBFF
        const float kCPHalf = 6.0f * cs;
        const float kCPHalfHot = 8.0f * cs;
        const float kCPBorder = 1.5f * cs;

        const size_t n = m_LatticePoints.size();
        const float tA = m_LatticePoints.front().Time;
        const float tB = m_LatticePoints.back().Time;

        // Translucent influence band across the affected time range.
        const float bandLpx = std::clamp(x + (tA - m_RangeStart) / rangeDuration * W, x, x + W);
        const float bandRpx = std::clamp(x + (tB - m_RangeStart) / rangeDuration * W, x, x + W);
        if (bandRpx > bandLpx)
            ctx.Emit(MakeRect(bandLpx, y, bandRpx - bandLpx, H, kLatticeBandColor));

        // Cache CP times + values so the spline-sample loop can call either
        // basis function with a contiguous buffer.
        std::vector<float> times(n);
        std::vector<float> values(n);
        for (size_t i = 0; i < n; ++i)
        {
            times[i]  = m_LatticePoints[i].Time;
            values[i] = m_LatticePoints[i].Value;
        }

        // Sample the deformed lattice spline at sub-pixel resolution and
        // emit a thin polyline. Visualization matches the active basis so
        // the user sees exactly the shape getting baked into the curve.
        const int kSamples = std::max(16, static_cast<int>((bandRpx - bandLpx) * 0.5f));
        const bool useBezier = (m_LatticeBasis == LatticeBasis::Bezier);
        float prevLX = -1.0f, prevLY = -1.0f;
        for (int s = 0; s <= kSamples; ++s)
        {
            const float u = static_cast<float>(s) / static_cast<float>(kSamples);
            const float t = tA + (tB - tA) * u;
            const float v = useBezier
                ? EvaluateBezierBasis(values.data(), n, u)
                : EvaluateCatmullRomBasis(times.data(), values.data(), n, t);
            const float sx = x + (t - m_RangeStart) / rangeDuration * W;
            const float sy = valueToPy(v);
            if (prevLX >= 0.0f)
                ctx.Emit(MakeLine(prevLX, prevLY, sx, sy, 1.5f * cs, kLatticeLineColor));
            prevLX = sx; prevLY = sy;
        }

        // Draw the CP handles on top of the spline. Each handle is a blue
        // square framed in white so it reads clearly against keyframe
        // markers (which are smaller, green/orange, and lack a border). The
        // hovered or actively-dragged CP grows and switches to an amber/white
        // scheme so the user has unambiguous feedback about what they're
        // about to grab.
        const int hotCP = (m_LatticeDragIndex >= 0) ? m_LatticeDragIndex : m_LatticeHoverIndex;
        for (size_t ci = 0; ci < n; ++ci)
        {
            const bool hot = (static_cast<int>(ci) == hotCP);
            const float half = hot ? kCPHalfHot : kCPHalf;
            const uint32_t border = hot ? kLatticeCPHotBorder : kLatticeCPBorder;
            const uint32_t fill   = hot ? kLatticeCPHotFill   : kLatticeCPFill;
            const float cpPx = x + (m_LatticePoints[ci].Time - m_RangeStart) / rangeDuration * W;
            const float cpPy = valueToPy(m_LatticePoints[ci].Value);
            const float hl = std::max(x,     cpPx - half);
            const float hr = std::min(x + W, cpPx + half);
            const float ht = std::max(y,     cpPy - half);
            const float hb = std::min(y + H, cpPy + half);
            if (hr > hl && hb > ht)
            {
                ctx.Emit(MakeRect(hl, ht, hr - hl, hb - ht, border));
                const float fl = hl + kCPBorder;
                const float fr = hr - kCPBorder;
                const float ft = ht + kCPBorder;
                const float fb = hb - kCPBorder;
                if (fr > fl && fb > ft)
                    ctx.Emit(MakeRect(fl, ft, fr - fl, fb - ft, fill));
            }
        }

        // (HUD intentionally omitted — N + basis controls live in the
        // Lattice options bar above the curve view.)
        (void)kLatticeCurveColor;
    }
    else if (m_ActiveTool == CurveTool::Lattice && !m_LatticeActive && ctx.FontAtlas)
    {
        // Empty-state hint: tool is active but we have no lattice yet
        // (selection too small or none). Tell the user what to do.
        const char* hint = "Select 3+ keyframes to activate the lattice";
        const float kFont = 12.0f * cs;
        const float padX = 8.0f * cs, padY = 5.0f * cs;
        const float wPx = kFont * 0.55f * static_cast<float>(std::strlen(hint)) + padX * 2.0f;
        const float hPx = kFont + padY * 2.0f;
        const float bx = x + (W - wPx) * 0.5f;
        const float by = y + 12.0f * cs;
        ctx.Emit(MakeRect(bx, by, wPx, hPx, PackColor(0.0f, 0.0f, 0.0f, 0.55f)));
        const float kLogicalFont = (cs > 0.0f) ? (kFont / cs) : kFont;
        ctx.EmitText(hint, bx + padX, by + padY, kLogicalFont,
                     PackColor(0.92f, 0.92f, 0.92f, 0.95f), ctx.FontAtlas);
    }

    // Retime tool overlay. Three layers, painted bottom-to-top:
    //   1) Persistent region (when active) with edge handles.
    //   2) The single-pivot "shade right of pivot" + pivot line, only when
    //      no region is active.
    //   3) Ghost markers for keys being moved + HUD chip.
    if (m_ActiveTool == CurveTool::Retime && W > 0.0f)
    {
        // Pending in-point marker (drawn before the region so the active
        // region's edges paint on top if they overlap).
        if (m_RetimePendingIn && !m_RetimeRegionActive)
        {
            const float px = std::clamp(timeToPx(m_RetimePendingInTime), x, x + W);
            ctx.Emit(MakeRect(px - cs, y, 2.0f * cs, H, PackColor(0.55f, 0.85f, 1.0f, 0.75f)));
            ctx.Emit(MakeRect(px - 4.0f * cs, y, 8.0f * cs, 4.0f * cs, PackColor(0.55f, 0.85f, 1.0f, 0.95f)));
        }
        // Draw the region rectangle either when a region is actually set,
        // or while the user is *currently* drawing one (DefineRegion drag in
        // progress). Without the m_RetimeDragging guard, m_RetimeMode lingers
        // at DefineRegion after a previous gesture and paints stale edges
        // on top of the pending-in marker → user sees three vertical lines.
        if (m_RetimeRegionActive || (m_RetimeMode == RetimeMode::DefineRegion && m_RetimeDragging))
        {
            const float lpx = std::clamp(timeToPx(m_RetimeRegionLeft),  x, x + W);
            const float rpx = std::clamp(timeToPx(m_RetimeRegionRight), x, x + W);
            const float rw = std::max(0.0f, rpx - lpx);
            const bool bodyHot  = (m_RetimeHover == RetimeHover::Body);
            const bool leftHot  = (m_RetimeHover == RetimeHover::LeftHandle);
            const bool rightHot = (m_RetimeHover == RetimeHover::RightHandle);
            if (rw > 0.0f)
                ctx.Emit(MakeRect(lpx, y, rw, H,
                                  bodyHot ? PackColor(0.25f, 0.65f, 1.0f, 0.18f)
                                          : PackColor(0.25f, 0.65f, 1.0f, 0.10f)));
            // Region edges. Hover brightens + thickens the relevant edge.
            const uint32_t edgeIdle = PackColor(0.45f, 0.80f, 1.0f, 0.85f);
            const uint32_t edgeHot  = PackColor(0.85f, 0.95f, 1.0f, 1.0f);
            const float lw = (leftHot  ? 3.0f : 2.0f) * cs;
            const float rwid = (rightHot ? 3.0f : 2.0f) * cs;
            ctx.Emit(MakeRect(lpx - lw * 0.5f,   y, lw,   H, leftHot  ? edgeHot : edgeIdle));
            ctx.Emit(MakeRect(rpx - rwid * 0.5f, y, rwid, H, rightHot ? edgeHot : edgeIdle));
            if (m_RetimeRegionActive)
            {
                const float kHandleWBase = 6.0f * cs;
                const float kHandleHBase = 22.0f * cs;
                const uint32_t handleIdle = PackColor(0.55f, 0.85f, 1.0f, 0.95f);
                const uint32_t handleHot  = PackColor(1.00f, 1.00f, 1.00f, 1.0f);
                const float lhw = leftHot  ? kHandleWBase + 2.0f * cs : kHandleWBase;
                const float rhw = rightHot ? kHandleWBase + 2.0f * cs : kHandleWBase;
                const float lhh = leftHot  ? kHandleHBase + 4.0f * cs : kHandleHBase;
                const float rhh = rightHot ? kHandleHBase + 4.0f * cs : kHandleHBase;
                const float lgy = y + (H - lhh) * 0.5f;
                const float rgy = y + (H - rhh) * 0.5f;
                ctx.Emit(MakeRect(lpx - lhw * 0.5f, lgy, lhw, lhh, leftHot  ? handleHot : handleIdle));
                ctx.Emit(MakeRect(rpx - rhw * 0.5f, rgy, rhw, rhh, rightHot ? handleHot : handleIdle));
            }
        }
        else if (m_RetimeDragging &&
                 (m_RetimeMode == RetimeMode::Scale || m_RetimeMode == RetimeMode::Translate))
        {
            // Single-pivot Maya-style preview: pivot line + tinted band from
            // pivot to the right edge, only while a Scale/Translate drag is
            // actually in progress. Without the m_RetimeDragging guard this
            // ran on tool enable too, where m_RetimePivotTime defaults to 0
            // and the entire timeline lit up as if it were the active region.
            const float pivotPx = timeToPx(m_RetimePivotTime);
            if (pivotPx >= x && pivotPx <= x + W)
            {
                const float regionW = (x + W) - pivotPx;
                if (regionW > 0.0f)
                    ctx.Emit(MakeRect(pivotPx, y, regionW, H, PackColor(0.2f, 0.5f, 1.0f, 0.08f)));
                ctx.Emit(MakeRect(pivotPx - cs, y, 2.0f * cs, H, PackColor(0.4f, 0.7f, 1.0f, 0.85f)));
            }
        }

        if (m_RetimeDragging && !m_RetimePreview.empty())
        {
            const uint32_t ghostCol = PackColor(0.4f, 0.85f, 1.0f, 0.55f);
            for (const RetimePreviewEntry& rc : m_RetimePreview)
            {
                const float gx = timeToPx(rc.newTime);
                if (gx >= x && gx <= x + W)
                    ctx.Emit(MakeRect(gx - 0.5f * cs, y, 1.0f * cs, H, ghostCol));
            }
        }

        if (m_RetimeDragging)
        {
            char hud[80];
            const char* modeLabel = "scale";
            switch (m_RetimeMode)
            {
                case RetimeMode::Scale:            modeLabel = "scale"; break;
                case RetimeMode::Translate:        modeLabel = "shift"; break;
                case RetimeMode::DefineRegion:     modeLabel = "region"; break;
                case RetimeMode::RegionScaleRight: modeLabel = "scale R"; break;
                case RetimeMode::RegionScaleLeft:  modeLabel = "scale L"; break;
                case RetimeMode::RegionTranslate:  modeLabel = "shift region"; break;
            }
            if (m_RetimeMode == RetimeMode::DefineRegion)
            {
                std::snprintf(hud, sizeof(hud), "%s  [%.3f → %.3f]  width %.3fs",
                              modeLabel, m_RetimeRegionLeft, m_RetimeRegionRight,
                              m_RetimeRegionRight - m_RetimeRegionLeft);
            }
            else
            {
                std::snprintf(hud, sizeof(hud), "%s  Δ %+.3fs  ×%.3f",
                              modeLabel, m_RetimeDeltaSeconds, m_RetimeScaleFactor);
            }
            const float kHudFont = 11.0f * cs;
            const uint32_t hudBg  = PackColor(0.0f, 0.0f, 0.0f, 0.55f);
            const uint32_t hudFg  = PackColor(0.95f, 0.95f, 0.95f, 0.95f);
            const float hudPadX   = 6.0f * cs;
            const float hudPadY   = 3.0f * cs;
            const float hudW = kHudFont * 0.65f * static_cast<float>(std::strlen(hud)) + hudPadX * 2.0f;
            const float hudH = kHudFont + hudPadY * 2.0f;
            const float hudX = x + 8.0f * cs;
            const float hudY = y + 8.0f * cs;
            ctx.Emit(MakeRect(hudX, hudY, hudW, hudH, hudBg));
            const float kLogicalHudFont = (cs > 0.0f) ? (kHudFont / cs) : kHudFont;
            ctx.EmitText(hud, hudX + hudPadX, hudY + hudPadY, kLogicalHudFont, hudFg, ctx.FontAtlas);
        }
    }

    // DrawCurve: render the in-progress stroke as a polyline of small
    // segments, plus a small dot at each sample so the user can read the
    // sampling density.
    if (m_ActiveTool == CurveTool::DrawCurve && m_DrawingCurve && m_DrawCurveSamples.size() >= 2)
    {
        const float vRange = std::max(1e-6f, m_ValueMax - m_ValueMin);
        auto sampleX = [&](float t) {
            return x + std::clamp((t - m_RangeStart) / std::max(1e-6f, m_RangeEnd - m_RangeStart), 0.0f, 1.0f) * W;
        };
        auto sampleY = [&](float v) {
            return y + (1.0f - std::clamp((v - m_ValueMin) / vRange, 0.0f, 1.0f)) * H;
        };
        const uint32_t strokeCol = PackColor(1.0f, 0.85f, 0.30f, 0.95f);
        const float thickness = 2.0f * cs;
        for (size_t i = 1; i < m_DrawCurveSamples.size(); ++i)
        {
            const float ax = sampleX(m_DrawCurveSamples[i - 1].time);
            const float ay = sampleY(m_DrawCurveSamples[i - 1].value);
            const float bx = sampleX(m_DrawCurveSamples[i].time);
            const float by = sampleY(m_DrawCurveSamples[i].value);
            ctx.Emit(MakeLine(ax, ay, bx, by, thickness, strokeCol));
        }
    }
}

float CurvesGraphView::ComputeTimeSnap(float t) const
{
    if (m_SnapTimeStep > 0.0f)
        return std::round(t / m_SnapTimeStep) * m_SnapTimeStep;
    if (m_Fps > 0.0f)
        return std::round(t * m_Fps) / m_Fps;
    return t;
}

float CurvesGraphView::ComputeValueSnapStep(float valueRange) const
{
    // If a custom step is set (>0), use it; otherwise auto-compute based on visual grid.
    if (m_SnapValueStep > 0.0f)
        return m_SnapValueStep;

    // Match the visual grid step calculation (target ~5 horizontal grid lines).
    const float rawStep = valueRange / 5.0f;
    const float mag = std::pow(10.0f, std::floor(std::log10(rawStep)));
    const float norm = rawStep / mag;
    float nice = (norm <= 1.5f) ? 1.0f : (norm <= 3.5f) ? 2.0f : (norm <= 7.5f) ? 5.0f : 10.0f;
    return nice * mag;
}

} // namespace GameEngine

namespace RegisterAnimationWindow
{
static auto s_reg_curvesGraph =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::CurvesGraphView>(
        "CurvesGraphView",
        []() { return std::make_unique<GameEngine::CurvesGraphView>(); })
        .TagAlias("curvesgraphview");
}
