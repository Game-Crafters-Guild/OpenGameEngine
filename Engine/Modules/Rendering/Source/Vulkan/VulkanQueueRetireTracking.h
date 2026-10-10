#pragma once

#include "VulkanQueueSubmitContext.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace GameEngine
{
namespace Rendering
{

/// Graphics, compute, transfer. Also bounds the number of distinct submit contexts: a physical
/// queue only gets a context because some role resolved onto it.
inline constexpr size_t kQueueRoleCount = 3;

/// Completed values of the queue timelines, sampled once per retire sweep so a sweep over many
/// deferred entries costs one driver call per queue rather than one per entry.
///
/// Keyed by timeline HANDLE, never by role. Deferred work records the timeline its submit
/// actually signalled, which is the shared queue's timeline wherever two roles alias one
/// physical queue. Resolving completion by role instead reads a counter nothing signals — the
/// entry never retires — or, worse, another queue's counter, which runs ahead and frees the
/// resource while its own work is still executing.
class QueueTimelineCompletions
{
public:
    /// Records `timeline`'s completed value. The null handle (an unbound context) and handles
    /// already recorded are ignored, so contexts that alias one queue contribute one entry.
    void Add(SemaphoreHandle timeline, uint64_t completed)
    {
        if (!timeline.IsValid() || m_Count >= m_Entries.size())
            return;
        for (size_t i = 0; i < m_Count; ++i)
        {
            if (m_Entries[i].timeline.id == timeline.id)
                return;
        }
        m_Entries[m_Count] = {timeline, completed};
        ++m_Count;
    }

    /// Completed value for `timeline`. False when this sweep did not sample that timeline; the
    /// caller must then resolve the value itself or leave the entry outstanding, never
    /// substitute another timeline's progress.
    bool TryGet(SemaphoreHandle timeline, uint64_t& outCompleted) const
    {
        for (size_t i = 0; i < m_Count; ++i)
        {
            if (m_Entries[i].timeline.id == timeline.id)
            {
                outCompleted = m_Entries[i].completed;
                return true;
            }
        }
        return false;
    }

private:
    struct Entry
    {
        SemaphoreHandle timeline;
        uint64_t completed = 0;
    };

    std::array<Entry, kQueueRoleCount> m_Entries{};
    size_t m_Count = 0;
};

/// When a deferred destruction becomes safe, as one tag per submitting queue rather than a
/// single (timeline, value) pair as the staging-buffer and command-buffer retirement paths use:
/// those resources belong to exactly one submit on one queue, whereas an object reachable from
/// any queue must outlive all of them, and the queues' timelines are independent counters with
/// no common ordering — no single value can express "compute has passed X and graphics has
/// passed Y".
struct QueueRetireTags
{
    uint64_t graphics = 0;
    uint64_t compute = 0;
    uint64_t transfer = 0;
};

/// How far each role's queue has been submitted to, and how far its GPU timeline has reached.
struct QueueTimelineProgress
{
    uint64_t graphicsCompleted = 0;
    uint64_t computeCompleted = 0;
    uint64_t transferCompleted = 0;
    uint64_t graphicsSubmitted = 0;
    uint64_t computeSubmitted = 0;
    uint64_t transferSubmitted = 0;

    /// True once every queue has either reached the entry's tag or completed everything ever
    /// submitted to it.
    bool Retires(const QueueRetireTags& tags) const
    {
        // Per queue: either the GPU has reached the tag, or the queue has completed everything
        // ever submitted to it. The second case is what frees an entry tagged for a submit that
        // never came, so tagging forward cannot strand it.
        auto passed = [](uint64_t tag, uint64_t completed, uint64_t submitted)
        { return completed >= tag || completed >= submitted; };

        return passed(tags.graphics, graphicsCompleted, graphicsSubmitted) &&
               passed(tags.compute, computeCompleted, computeSubmitted) &&
               passed(tags.transfer, transferCompleted, transferSubmitted);
    }
};

/// Reads each role's pair out of the CONTEXT that role resolves to: the completed value from the
/// timeline that context signals, the submitted value from the counter that context allocates.
/// Taking the two halves from one object is the whole point — a role with no dedicated queue
/// resolves to another role's context, and the timeline named after the role is then signalled
/// by nothing, so a completed value read by role would sit at 0 against a submitted count that
/// climbs, and Retires would never fire again.
///
/// Aliased roles therefore yield identical clauses, which is redundant rather than wrong: the
/// tags they are tested against come from the same contexts.
///
/// COMPLETED is read before SUBMITTED within a role, so a submit landing between the two
/// over-states outstanding work rather than under-stating it — the safe direction.
inline QueueTimelineProgress SampleQueueProgress(
    const std::array<const QueueSubmitContext*, kQueueRoleCount>& byRole,
    const QueueTimelineCompletions& completions)
{
    auto sample = [&completions](const QueueSubmitContext* context, uint64_t& completed,
                                 uint64_t& submitted)
    {
        completed = 0;
        submitted = 0;
        if (!context)
            return;
        completions.TryGet(context->Timeline(), completed);
        submitted = context->LastSignalled();
    };

    QueueTimelineProgress progress{};
    sample(byRole[0], progress.graphicsCompleted, progress.graphicsSubmitted);
    sample(byRole[1], progress.computeCompleted, progress.computeSubmitted);
    sample(byRole[2], progress.transferCompleted, progress.transferSubmitted);
    return progress;
}

} // namespace Rendering
} // namespace GameEngine
