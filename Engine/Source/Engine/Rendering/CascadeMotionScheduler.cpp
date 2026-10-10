#include "Engine/Rendering/CascadeMotionScheduler.h"

#include "Engine/Rendering/CascadeShadowCache.h"

#include <algorithm>
#include <cassert>

namespace GameEngine
{
namespace Engine::Renderer
{

CascadeUpdateClass ClassifyCascadeDirtyCause(CascadeCacheDirtyCause cause)
{
    switch (cause)
    {
    case CascadeCacheDirtyCause::Cached:
        return CascadeUpdateClass::Cached;
    case CascadeCacheDirtyCause::CameraChanged:
    case CascadeCacheDirtyCause::CascadeFitChanged:
        return CascadeUpdateClass::Motion;
    default:
        // FirstRender, ContributorPresent, ExecUnderDraw, ContributorChanged,
        // PhysicalChanged, CasterContentChanged, ConfigChanged, CullNotSettled:
        // the retained layer is (or may be) WRONG, not merely stale — render now.
        return CascadeUpdateClass::Correctness;
    }
}

CascadeUpdateClass ClassifyCascadePair(CascadeCacheDirtyCause depthCause,
                                       const CascadeCacheDirtyCause* tintCause,
                                       bool contentFitsLockstep)
{
    CascadeUpdateClass cls = ClassifyCascadeDirtyCause(depthCause);
    if (!tintCause)
        return cls;
    const CascadeUpdateClass tintCls = ClassifyCascadeDirtyCause(*tintCause);
    if (tintCls > cls)
        cls = tintCls;
    // Deferral keeps BOTH retained layers under the depth-content fit; a
    // Motion pair whose families hold content from DIFFERENT fits must render
    // instead (see the header contract).
    if (cls == CascadeUpdateClass::Motion && !contentFitsLockstep)
        cls = CascadeUpdateClass::Correctness;
    return cls;
}

CascadeMotionScheduler::FramePlan CascadeMotionScheduler::PlanFrame(
    uint32_t viewId, uint32_t numCascades,
    const std::array<CascadeUpdateClass, kMaxCascades>& classes,
    const std::array<uint64_t, kMaxCascades>& staleness, uint32_t motionCap, uint32_t maxAge)
{
    assert(numCascades <= kMaxCascades);
    numCascades = std::min(numCascades, kMaxCascades);

    FramePlan plan{};
    if (motionCap == 0)
        return plan;

    // Cached and Correctness slots resolve outside the budget: Cached content
    // equals the current fit (byte-equality is the cache's skip condition) and
    // a correctness render replaces the content this frame.
    uint32_t motionSlots[kMaxCascades];
    uint32_t motionCount = 0;
    for (uint32_t c = 0; c < numCascades; ++c)
    {
        if (classes[c] == CascadeUpdateClass::Motion)
            motionSlots[motionCount++] = c;
    }
    if (motionCount == 0)
        return plan;

    Stats& stats = m_Views[viewId];

    // Staleness-forced renders first (the bound beats the budget), most-stale
    // first so a burst drains in order; then nearest-first until the budget
    // runs out; the rest defer. A slot with unknown content staleness can
    // never defer — it always classifies as forced.
    const auto forced = [&](uint32_t c) { return staleness[c] > maxAge; };
    uint32_t budget = motionCap;
    std::sort(motionSlots, motionSlots + motionCount,
              [&](uint32_t a, uint32_t b)
              {
                  const bool fa = forced(a);
                  const bool fb = forced(b);
                  if (fa != fb)
                      return fa; // forced slots first
                  if (fa && staleness[a] != staleness[b])
                      return staleness[a] > staleness[b]; // most stale first
                  return a < b;                           // nearest-first otherwise
              });
    for (uint32_t i = 0; i < motionCount; ++i)
    {
        const uint32_t c = motionSlots[i];
        if (forced(c) || budget > 0)
        {
            if (budget > 0)
                --budget;
            else
                ++stats.ForcedBeyondCap;
            if (forced(c))
                ++stats.ForcedByAge;
            continue; // renders this frame
        }
        plan.DeferMask |= 1u << c;
        ++stats.Deferred;
    }
    return plan;
}

void CascadeMotionScheduler::Reset()
{
    m_Views.clear();
}

const CascadeMotionScheduler::Stats& CascadeMotionScheduler::GetStats(uint32_t viewId) const
{
    static const Stats kZero{};
    const auto it = m_Views.find(viewId);
    return it != m_Views.end() ? it->second : kZero;
}

} // namespace Engine::Renderer
} // namespace GameEngine
