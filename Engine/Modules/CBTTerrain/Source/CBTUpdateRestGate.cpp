#include "CBTTerrain/CBTUpdateRestGate.h"

#include <algorithm>

namespace GameEngine::CBTTerrain
{

bool CBTUpdateRestGate::CanSkip(std::span<const uint8_t> inputs)
{
    if (!m_HasInputs || !std::equal(inputs.begin(), inputs.end(), m_Inputs.begin(), m_Inputs.end()))
    {
        m_Inputs.assign(inputs.begin(), inputs.end());
        m_HasInputs = true;
        Restart();
        return false;
    }
    return m_QuietReadings >= kQuietReadingsToRest;
}

uint64_t CBTUpdateRestGate::OnUpdateRecorded(bool forced)
{
    const uint64_t sequence = m_NextSequence++;
    if (forced)
        Restart();
    return sequence;
}

void CBTUpdateRestGate::OnActivityRead(uint64_t sequence, const CBTUpdateActivity& activity)
{
    if (sequence < m_FirstCountedSequence)
        return;
    const bool stepsUnchanged = m_HasSteps && activity.PressureStep == m_PressureStep &&
                                activity.OffFrustumKeepStep == m_OffFrustumKeepStep;
    const bool quiet = activity.SplitServed == 0 && activity.MergeServed == 0 && stepsUnchanged;
    m_QuietReadings = quiet ? m_QuietReadings + 1u : 0u;
    m_HasSteps = true;
    m_PressureStep = activity.PressureStep;
    m_OffFrustumKeepStep = activity.OffFrustumKeepStep;
}

void CBTUpdateRestGate::Reset()
{
    m_Inputs.clear();
    m_HasInputs = false;
    Restart();
}

void CBTUpdateRestGate::Restart()
{
    m_FirstCountedSequence = m_NextSequence;
    m_QuietReadings = 0;
    m_HasSteps = false;
}

} // namespace GameEngine::CBTTerrain
