#include "Rendering/Core/RecomputeElision.h"

#include <cstdlib>

namespace GameEngine
{
namespace Rendering
{

const char* ElisionCauseName(ElisionCause cause)
{
    switch (cause)
    {
    case ElisionCause::Skipped:       return "Skipped";
    case ElisionCause::FirstEvaluate: return "FirstEvaluate";
    case ElisionCause::Forced:        return "Forced";
    case ElisionCause::EvaluationGap: return "EvaluationGap";
    case ElisionCause::InputsChanged: return "InputsChanged";
    case ElisionCause::NotSettled:    return "NotSettled";
    default:                          return "Unknown";
    }
}

const char* ElisionDisengageReason(ElisionCause cause, bool skip)
{
    if (!skip && cause == ElisionCause::Skipped)
        return "Disabled (inputs settled)";
    return ElisionCauseName(cause);
}

bool IdleElisionLoggingEnabled()
{
    static const bool enabled =
        ParseIdleElisionLoggingEnabled(std::getenv("GE_IDLE_ELISION_LOG"));
    return enabled;
}

bool RecomputeElisionGate::ShouldReportWindow(uint64_t period)
{
    if (period == 0 || m_Stats.Evaluated == 0 || m_Stats.Evaluated % period != 0)
        return false;
    const uint64_t evaluated = m_Stats.Evaluated - m_WindowBaseEvaluated;
    const uint64_t skipped = m_Stats.Skipped - m_WindowBaseSkipped;
    m_WindowBaseEvaluated = m_Stats.Evaluated;
    m_WindowBaseSkipped = m_Stats.Skipped;
    return skipped != evaluated;
}

RecomputeElisionGate::Decision RecomputeElisionGate::Evaluate(uint64_t frameStamp,
                                                              ElisionInputBlob&& inputs,
                                                              bool enabled, uint32_t settleFrames)
{
    // Cause resolution in priority order — exact equality throughout. The
    // cause is resolved independent of `enabled` so a disabled run still
    // measures the achievable skip rate (CascadeShadowCache convention).
    ElisionCause cause;
    if (!m_HasLast)
    {
        cause = ElisionCause::FirstEvaluate;
    }
    else if (m_Forced)
    {
        cause = ElisionCause::Forced;
    }
    else if (frameStamp != m_LastStamp + 1)
    {
        // The callsite skipped one or more frames: interleaved callsites may
        // have claimed (and rewritten) the arena regions its retained ranges
        // point into, and one-frame-stale feedback consumers were reduced
        // under layouts this gate never saw. Never trust equality across a gap.
        cause = ElisionCause::EvaluationGap;
    }
    else if (inputs != m_Last)
    {
        cause = ElisionCause::InputsChanged;
    }
    else
    {
        // Saturating: only the threshold comparison matters past settleFrames.
        if (m_ConsecutiveMatches < UINT32_MAX)
            ++m_ConsecutiveMatches;
        cause = m_ConsecutiveMatches >= settleFrames ? ElisionCause::Skipped
                                                     : ElisionCause::NotSettled;
    }

    if (cause != ElisionCause::Skipped && cause != ElisionCause::NotSettled)
    {
        m_Last = std::move(inputs);
        m_ConsecutiveMatches = 0;
    }
    m_HasLast = true;
    m_Forced = false;
    m_LastStamp = frameStamp;
    m_LastCause = cause;

    ++m_Stats.Evaluated;
    ++m_Stats.CauseCounts[static_cast<size_t>(cause)];

    Decision d{};
    d.Cause = cause;
    d.Skip = enabled && cause == ElisionCause::Skipped;
    if (d.Skip)
        ++m_Stats.Skipped;
    return d;
}

} // namespace Rendering
} // namespace GameEngine
