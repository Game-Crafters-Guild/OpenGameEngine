#include "Engine/Rendering/DDGISolveBudget.h"

#include <algorithm>

namespace GameEngine::Engine::Renderer
{

void DDGISolveBudget::Shrink()
{
    m_RaysPerTick = std::max(kRaysPerTickMin,
                             static_cast<uint32>(static_cast<float>(m_RaysPerTick) * kShrinkFactor));
    m_DtEmaMs = 0.0f;  // re-measure only the controller at the new cap
    m_Cooldown = kShrinkCooldownTicks;
}

void DDGISolveBudget::Tick(float frameDtMs)
{
    if (frameDtMs >= kPauseDtMs)
    {
        // A debugger break or host stall says nothing about solve pressure.
        m_DtEmaMs = 0.0f;
        m_OverloadStreak = 0;
        return;
    }
    if (frameDtMs >= kOverloadDtMs)
    {
        // One long frame may be unrelated (shader compile, asset load);
        // repeated ones mean the machine is not keeping up and the bounded
        // GI workload must back off.
        m_DtEmaMs = 0.0f;
        m_OverloadStreak = std::min(kOverloadStrikes, m_OverloadStreak + 1);
        if (m_OverloadStreak >= kOverloadStrikes && m_RaysPerTick > kRaysPerTickMin)
            Shrink();
        return;
    }

    m_OverloadStreak = 0;
    m_DtEmaMs = m_DtEmaMs > 0.0f ? m_DtEmaMs * (1.0f - kEmaAlpha) + frameDtMs * kEmaAlpha
                                 : frameDtMs;
    if (m_Cooldown > 0)
        --m_Cooldown;
    if (m_DtEmaMs > kShrinkAboveEmaMs && m_RaysPerTick > kRaysPerTickMin)
        Shrink();
    else if (m_Cooldown == 0 && m_DtEmaMs < kGrowBelowEmaMs && m_RaysPerTick < kRaysPerTickMax)
        m_RaysPerTick = std::min(kRaysPerTickMax, m_RaysPerTick + kGrowStepRays);
}

void DDGISolveBudget::OnRestResume()
{
    m_RaysPerTick = std::min(m_RaysPerTick, kRaysPerTickRestResume);
    m_DtEmaMs = 0.0f;
    m_Cooldown = std::max(m_Cooldown, kRestResumeCooldownTicks);
}

}  // namespace GameEngine::Engine::Renderer
