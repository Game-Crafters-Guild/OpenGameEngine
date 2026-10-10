#include "Engine/Rendering/CompileConcurrencyGate.h"

#include <algorithm>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vector>
#endif

namespace GameEngine
{
namespace Engine::Renderer
{
namespace
{
// Reserve this many physical cores for the main thread and the extraction /
// streaming workers so a compile burst can never pin every core.
constexpr uint32_t kReservedCoresForFrame = 2;
constexpr uint32_t kMinCompileCap = 2;

uint32_t QueryPhysicalCoreCount()
{
#if defined(_WIN32)
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length == 0)
        return 0;
    std::vector<uint8_t> buffer(length);
    if (!GetLogicalProcessorInformationEx(
            RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length))
        return 0;

    uint32_t cores = 0;
    size_t offset = 0;
    while (offset < buffer.size())
    {
        auto* info =
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
        if (info->Relationship == RelationProcessorCore)
            ++cores;
        offset += info->Size;
    }
    return cores;
#else
    // No portable physical-core query. hardware_concurrency reports logical
    // processors; assume 2-way SMT (the common desktop case) and halve. The env
    // override exists for machines where this estimate is wrong.
    const uint32_t logical = std::thread::hardware_concurrency();
    return logical > 1 ? logical / 2 : logical;
#endif
}
} // namespace

uint32_t ComputeShaderCompileConcurrencyCap()
{
    if (const char* env = std::getenv("GE_SHADER_COMPILE_CAP"))
    {
        const int parsed = std::atoi(env);
        if (parsed > 0)
            return static_cast<uint32_t>(parsed);
    }

    uint32_t physical = QueryPhysicalCoreCount();
    if (physical == 0)
    {
        // Detection failed — fall back to a conservative logical-core estimate.
        const uint32_t logical = std::thread::hardware_concurrency();
        physical = logical > 1 ? logical / 2 : 1;
    }

    return std::max(kMinCompileCap,
                    physical > kReservedCoresForFrame ? physical - kReservedCoresForFrame
                                                      : kMinCompileCap);
}

void CompileConcurrencyGate::Submit(std::function<void()> work, JobSystem::JobPriority priority)
{
    {
        std::lock_guard lock(m_Mutex);
        // A slot is free only when running < cap; the pump keeps running == cap
        // while either lane is non-empty, so a free slot here implies both lanes
        // empty and admitting directly cannot reorder ahead of queued work.
        if (m_Running >= m_Cap)
        {
            if (priority == JobSystem::JobPriority::Background)
                m_PendingBackground.push_back(std::move(work));
            else
                m_PendingNormal.push_back(std::move(work));
            return;
        }
        ++m_Running;
        m_HighWater = std::max(m_HighWater, m_Running);
    }

    try
    {
        DispatchTracked(std::move(work), priority);
    }
    catch (...)
    {
        // The executor could not enqueue the task (task-envelope / queue
        // allocation failure — the pool's F13c contract self-drains inline on
        // shutdown and never throws there). The task never ran. Release the slot
        // AND pump: a concurrent Submit may have stranded a job in a lane between
        // our unlock above and this throw, and OnJobFinished decrements the slot
        // and promotes it. Then propagate the allocation failure to the caller.
        OnJobFinished();
        throw;
    }
}

void CompileConcurrencyGate::DispatchTracked(std::function<void()> work,
                                             JobSystem::JobPriority priority)
{
    m_Dispatch(
        [this, work = std::move(work)]() mutable
        {
            struct Finisher
            {
                CompileConcurrencyGate* Gate;
                ~Finisher() { Gate->OnJobFinished(); }
            } finisher{this};
            work();
        },
        priority);
}

void CompileConcurrencyGate::OnJobFinished() noexcept
{
    // Release the finished job's slot and promote the next pending job, draining
    // the Background lane (user-visible variant misses) before Normal (bulk
    // prewarm). Loop only to skip a job whose dispatch fails: drop it and try the
    // next so the slot is never leaked.
    for (;;)
    {
        std::function<void()> next;
        JobSystem::JobPriority priority = JobSystem::JobPriority::Normal;
        {
            std::lock_guard lock(m_Mutex);
            --m_Running;
            if (!m_PendingBackground.empty())
            {
                next = std::move(m_PendingBackground.front());
                m_PendingBackground.pop_front();
                priority = JobSystem::JobPriority::Background;
            }
            else if (!m_PendingNormal.empty())
            {
                next = std::move(m_PendingNormal.front());
                m_PendingNormal.pop_front();
                priority = JobSystem::JobPriority::Normal;
            }
            else
            {
                return;
            }
            ++m_Running;
            m_HighWater = std::max(m_HighWater, m_Running);
        }
        try
        {
            DispatchTracked(std::move(next), priority);
            return;
        }
        catch (...)
        {
            // Dispatch failed (task-envelope allocation failure — not shutdown;
            // the pool's F13c contract never throws on teardown). The promoted
            // job never ran; its captured accounting guard balances when its
            // closure is destroyed here. Loop to release its slot and try the
            // next so the slot is never leaked.
            continue;
        }
    }
}

} // namespace Engine::Renderer
} // namespace GameEngine
