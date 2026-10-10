#include "DebugServer/JobSystemDebugHandlers.h"

#include "Core/Engine.h"
#include "DebugServer/EditorDebugServer.h"
#include "DebugServer/JobSystemReport.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>

namespace GameEngine
{

using json = nlohmann::json;

void RegisterJobSystemDebugHandlers(EditorDebugServer& server)
{
    // get_job_system — the engine job system right now (WorkStealingThreadPool::GetStatistics).
    // No parameters. Reply:
    //   computeWorkers, blockingThreads: thread counts (blocking threads are created on a channel's
    //     first dispatch and never retired).
    //   lanes.normal / lanes.background: {queued, running} — jobs waiting in the lane and compute
    //     workers running a job of that class.
    //   channels[]: {name, queued, running, cap, jobs, queueWaitMs} per registered JobChannel —
    //     jobs past the cap, jobs holding a slot, the cap, cumulative jobs that took a slot and the
    //     cumulative time jobs waited in the channel for one.
    //   threads[]: {thread, kind, index, running, runningForMs?} per pool thread: running is
    //     "Normal", "Background" or the channel's name, null when idle; runningForMs is set for
    //     Background and channel jobs only (the pool reads no clock for Normal jobs).
    //   tasksExecuted: jobs dequeued from the compute lanes plus channel jobs the blocking threads
    //     ran; census: the cumulative queue-topology counters it is derived from.
    // Read twice and subtract for a rate.
    server.RegisterHandler("get_job_system", [](const EditorDebugServer::RequestContext&) -> json
    {
        const JobSystem::JobSystemStatistics stats = EngineCore::GetInstance().GetJobSystem().GetStatistics();
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return Editor::BuildJobSystemReport(
            stats, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()));
    });
}

} // namespace GameEngine
