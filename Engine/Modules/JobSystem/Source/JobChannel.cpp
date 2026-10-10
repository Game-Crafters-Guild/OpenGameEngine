#include "JobSystem/JobChannel.h"

#include "JobChannelState.h"

#include <utility>

namespace JobSystem
{

JobChannel::JobChannel(WorkStealingThreadPool& pool, const JobChannelDesc& desc)
    : m_Pool(pool), m_State(MakeShared<Detail::JobChannelState>(pool, desc.Name, desc.MaxRunning))
{
    m_Pool.RegisterChannel(m_State, desc.MaxRunning);
}

JobChannel::~JobChannel()
{
    m_Pool.UnregisterChannel(m_State.get(), m_State->MaxRunning());
    Vector<Detail::ChannelJob> queued;
    m_State->TakeQueued(queued);
    // In FIFO order: a Submit job is cancelled, an Enqueue job goes on to the
    // blocking threads (never dropped).
    for (Detail::ChannelJob& job : queued)
    {
        if (job.Envelope->NeedsTaskData())
        {
            m_Pool.CancelUnrunEnvelope(std::move(job.Envelope));
        }
        else
        {
            m_Pool.HandOnToBlockingThreads(std::move(job.Envelope), job.Channel);
        }
    }
}

void JobChannel::SetBeforeDispatchHookForTests(std::function<void()> hook)
{
    m_State->SetBeforeDispatchHook(std::move(hook));
}

bool JobChannel::Admit(UniquePtr<TaskBase> envelope, Detail::ChannelSlot& slot)
{
    switch (m_State->Admit(envelope, slot))
    {
    case Detail::JobChannelState::Admission::Queued:
        return true;
    case Detail::JobChannelState::Admission::Refused:
        // The pool's shutdown gate is set (for a Submit, it was set between
        // its first check and the channel's lock): a Submit job never runs and
        // its caller gets what a Submit after the gate gets; an Enqueue job
        // runs here, on the caller.
        m_Pool.DisposeEnvelopeAfterGate(std::move(envelope),
                                        WorkStealingThreadPool::ChannelContext(m_State->Name()));
        return false;
    case Detail::JobChannelState::Admission::Dispatch:
        break;
    }
    m_State->RunBeforeDispatchHook();
    m_Pool.DispatchToBlockingThreads(std::move(envelope), m_State->Name());
    return true;
}

} // namespace JobSystem
