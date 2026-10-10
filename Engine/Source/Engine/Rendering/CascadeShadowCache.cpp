#include "Engine/Rendering/CascadeShadowCache.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>

namespace GameEngine
{
namespace Engine::Renderer
{

namespace
{
bool MatEqual(const float a[16], const float b[16])
{
    return std::memcmp(a, b, 16 * sizeof(float)) == 0;
}

bool FitEqual(const CascadeRenderInputs& a, const CascadeRenderInputs& b)
{
    return MatEqual(a.LightVP, b.LightVP) && MatEqual(a.LightVPRel, b.LightVPRel) &&
           std::memcmp(a.CasterFootprint, b.CasterFootprint, sizeof(a.CasterFootprint)) == 0 &&
           std::memcmp(a.RenderOriginSector, b.RenderOriginSector,
                       sizeof(a.RenderOriginSector)) == 0;
}

bool ConfigEqual(const CascadeRenderInputs& a, const CascadeRenderInputs& b)
{
    return a.Resolution == b.Resolution && a.NumCascades == b.NumCascades &&
           a.ShadowLODBias == b.ShadowLODBias && a.LODForceLevel == b.LODForceLevel &&
           a.SseThresholdToCoverage == b.SseThresholdToCoverage &&
           a.SseThresholdToCoverageTight == b.SseThresholdToCoverageTight &&
           a.SelectionMode == b.SelectionMode && a.RenderLayerMask == b.RenderLayerMask && a.CasterReduction == b.CasterReduction;
}

bool AnyCasterOverlapsCascade(std::span<const ShadowCasterChangeSphere> spheres,
                              const float lightVP[16], uint32_t resolution)
{
    for (uint32_t i = 0; i < 16; ++i)
        if (!std::isfinite(lightVP[i]))
            return true;
    // The exclusion below is for orthographic cascades only.
    if (lightVP[3] != 0.0f || lightVP[7] != 0.0f || lightVP[11] != 0.0f || lightVP[15] <= 0.0f)
        return true;

    for (const auto& sphere : spheres)
    {
        if (!std::isfinite(sphere.X) || !std::isfinite(sphere.Y) || !std::isfinite(sphere.Z) ||
            !std::isfinite(sphere.Radius) || sphere.Radius < 0.0f)
            return true;
        bool outside = false;
        for (uint32_t axis = 0; axis < 2; ++axis)
        {
            const double x = static_cast<double>(lightVP[axis]) * sphere.X;
            const double y = static_cast<double>(lightVP[4 + axis]) * sphere.Y;
            const double z = static_cast<double>(lightVP[8 + axis]) * sphere.Z;
            const double translation = lightVP[12 + axis];
            const double rowLength = std::hypot(static_cast<double>(lightVP[axis]),
                                                lightVP[4 + axis], lightVP[8 + axis]);
            if (rowLength == 0.0)
                return true;
            const double radius = sphere.Radius * rowLength;
            // Outward tolerance covers float matrix/position cancellation at
            // large coordinates and one raster texel at the viewport boundary.
            const double rounding = 16.0 * std::numeric_limits<float>::epsilon() *
                (std::abs(x) + std::abs(y) + std::abs(z) + std::abs(translation) + radius + 1.0);
            const double border = 2.0 * lightVP[15] / std::max(1u, resolution);
            if (std::abs(x + y + z + translation) > lightVP[15] + radius + border + rounding)
            {
                outside = true;
                break;
            }
        }
        if (!outside)
            return true;
    }
    return false;
}
} // namespace

uint64_t CascadeShadowCache::TrackCasterChanges(uint32_t viewId, uint32_t slot, uint64_t worldId,
                                                const ShadowCasterChangeSet& changes,
                                                const float lightVP[16], uint32_t resolution)
{
    assert(slot < kSlotsPerView);
    SlotState& state = m_Views[viewId].Slots[slot];
    if (!state.HasObservedCasters || state.ObservedWorldId != worldId ||
        state.ObservedCasterEpoch != changes.Version)
    {
        const bool attributed = state.HasObservedCasters && state.ObservedWorldId == worldId &&
            changes.Version > state.ObservedCasterEpoch &&
            changes.Version - state.ObservedCasterEpoch == 1u && !changes.Unattributed;
        // Motion scheduling can retain the OLD fit for another frame. A mover
        // touching that footprint must force an immediate refresh even when it
        // misses the newly requested fit, or deferral would serve a stale shadow.
        if (!attributed || AnyCasterOverlapsCascade(changes.Spheres, lightVP, resolution) ||
            (state.HasRendered && AnyCasterOverlapsCascade(changes.Spheres,
                state.Rendered.LightVP, state.Rendered.Resolution)))
            state.RelevantCasterEpoch = changes.Version;
        state.ObservedWorldId = worldId;
        state.ObservedCasterEpoch = changes.Version;
        state.HasObservedCasters = true;
    }
    return state.RelevantCasterEpoch;
}

CascadeShadowCache::Decision CascadeShadowCache::Evaluate(uint32_t viewId, uint32_t slot,
                                                          uint64_t frameStamp,
                                                          const CascadeRenderInputs& in,
                                                          bool enabled,
                                                          bool includeCameraInSettle)
{
    assert(slot < kSlotsPerView);
    ViewState& view = m_Views[viewId];
    SlotState& s = view.Slots[slot];

    // Settle tracking first: does this frame's fit equal the CONSECUTIVE
    // previous frame's? (The fit the cull scheduler consumed for THIS frame is
    // the previous declare's — the header's one-frame-lag rule.) An Evaluate
    // gap (frame stamp not exactly one ahead: cascade-count shrink, hidden
    // view, frame-stream migration) invalidates the tracking — a gapped slot's
    // cull slice was built from a frame that never scheduled it, so a render
    // committed across a gap must never count as settled. Under the fit
    // freeze the frozen camera is part of what the cull consumed (the
    // header's frozen-camera qualification), so it joins the equality there.
    s.PrevFitEqual = s.HasPrevFrame && frameStamp == s.PrevFrameStamp + 1 &&
                     FitEqual(in, s.PrevFrame) &&
                     (!includeCameraInSettle ||
                      MatEqual(in.CameraViewProj, s.PrevFrame.CameraViewProj));
    s.PrevFrame = in;
    s.HasPrevFrame = true;
    s.PrevFrameStamp = frameStamp;

    const CascadeCacheDirtyCause cause = ResolveCause(s, in, s.UnderDrew);

    Stats& st = view.ViewStats;
    ++st.Evaluated;
    ++st.CauseCounts[static_cast<size_t>(cause)];

    Decision d{};
    d.Cause = cause;
    d.Skip = enabled && cause == CascadeCacheDirtyCause::Cached;
    if (d.Skip)
        ++st.Skipped;
    return d;
}

CascadeCacheDirtyCause CascadeShadowCache::ResolveCause(const SlotState& s,
                                                        const CascadeRenderInputs& in,
                                                        bool underDrew)
{
    // Priority order — exact equality throughout.
    if (in.HasContributorDrawCommands)
        return CascadeCacheDirtyCause::ContributorPresent;
    if (!s.HasRendered)
        return CascadeCacheDirtyCause::FirstRender;
    if (underDrew)
        return CascadeCacheDirtyCause::ExecUnderDraw;
    if (in.HasContributorDrawCommands != s.Rendered.HasContributorDrawCommands)
        return CascadeCacheDirtyCause::ContributorChanged;
    if (in.PhysicalId != s.Rendered.PhysicalId)
        return CascadeCacheDirtyCause::PhysicalChanged;
    if (in.WorldId != s.Rendered.WorldId || in.CasterEpoch != s.Rendered.CasterEpoch)
        return CascadeCacheDirtyCause::CasterContentChanged;
    if (!MatEqual(in.CameraViewProj, s.Rendered.CameraViewProj))
        return CascadeCacheDirtyCause::CameraChanged;
    if (!FitEqual(in, s.Rendered))
        return CascadeCacheDirtyCause::CascadeFitChanged;
    if (!ConfigEqual(in, s.Rendered))
        return CascadeCacheDirtyCause::ConfigChanged;
    if (!s.CullSettled)
        return CascadeCacheDirtyCause::CullNotSettled;
    return CascadeCacheDirtyCause::Cached;
}

CascadeCacheDirtyCause CascadeShadowCache::PeekCause(uint32_t viewId, uint32_t slot,
                                                     const CascadeRenderInputs& in,
                                                     bool pendingUnderDraw) const
{
    assert(slot < kSlotsPerView);
    const auto it = m_Views.find(viewId);
    if (it == m_Views.end())
        return ResolveCause(SlotState{}, in, pendingUnderDraw);
    const SlotState& s = it->second.Slots[slot];
    return ResolveCause(s, in, s.UnderDrew || pendingUnderDraw);
}

void CascadeShadowCache::OnRendered(uint32_t viewId, uint32_t slot, const CascadeRenderInputs& in)
{
    assert(slot < kSlotsPerView);
    ViewState& view = m_Views[viewId];
    SlotState& s = view.Slots[slot];
    s.Rendered = in;
    s.HasRendered = true;
    // The cull that fed this render consumed the PREVIOUS frame's fit; the
    // render is a skippable baseline only if that fit equalled this one.
    s.CullSettled = s.PrevFitEqual;
    // A fresh render attempt clears the exec feedback — if it under-draws too,
    // the recorder re-marks and the next declare re-dirties.
    s.UnderDrew = false;
}

void CascadeShadowCache::MarkUnderDrew(uint32_t viewId, uint32_t slot)
{
    assert(slot < kSlotsPerView);
    m_Views[viewId].Slots[slot].UnderDrew = true;
}

void CascadeShadowCache::InvalidateView(uint32_t viewId)
{
    const auto it = m_Views.find(viewId);
    if (it == m_Views.end())
        return;
    // Keep the stats and their window bookkeeping (instrumentation survives a
    // physical realloc); drop every slot's render record AND settle tracking —
    // a fresh physical means the next render must re-settle from scratch.
    for (SlotState& s : it->second.Slots)
        s = SlotState{};
}

void CascadeShadowCache::Reset()
{
    m_Views.clear();
}

const CascadeShadowCache::Stats& CascadeShadowCache::GetStats(uint32_t viewId) const
{
    static const Stats kZero{};
    const auto it = m_Views.find(viewId);
    return it != m_Views.end() ? it->second.ViewStats : kZero;
}

CascadeShadowCache::Stats CascadeShadowCache::ConsumeStatsWindow(uint32_t viewId)
{
    const auto it = m_Views.find(viewId);
    if (it == m_Views.end())
        return Stats{};
    const Stats& total = it->second.ViewStats;
    Stats& snapshot = it->second.LoggedSnapshot;
    Stats window{};
    window.Evaluated = total.Evaluated - snapshot.Evaluated;
    window.Skipped = total.Skipped - snapshot.Skipped;
    for (size_t i = 0; i < window.CauseCounts.size(); ++i)
        window.CauseCounts[i] = total.CauseCounts[i] - snapshot.CauseCounts[i];
    snapshot = total;
    return window;
}

bool CascadeShadowCache::ExchangeStormActive(uint32_t viewId, bool active)
{
    bool& flag = m_Views[viewId].StormActive;
    const bool previous = flag;
    flag = active;
    return previous;
}

} // namespace Engine::Renderer
} // namespace GameEngine
