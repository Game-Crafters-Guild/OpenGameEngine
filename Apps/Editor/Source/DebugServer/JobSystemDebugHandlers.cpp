#include "DebugServer/JobSystemDebugHandlers.h"

#include "Core/Engine.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "DebugServer/JobSystemReport.h"
#include "DebugServer/SystemWaveTraceReport.h"
#include "ECS/SystemWaveTrace.h"
#include "ECS/Systems.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine
{

using json = nlohmann::json;

namespace
{

// get_ecs_wave_trace: arm or disarm the rendering loop's wave trace, then describe what it holds.
json HandleEcsWaveTrace(const EditorDebugServer::RequestContext& ctx)
{
    auto* loop = EngineCore::GetInstance().GetRenderingLoop();
    auto* systems = loop ? loop->GetSystemManager() : nullptr;
    if (!systems)
        return Editor::RefuseRequest("No ECS system manager is running (the rendering loop has not started)");

    ECS::SystemWaveTrace& trace = systems->GetWaveTrace();
    if (ctx.params.contains("enabled"))
        trace.SetEnabled(ctx.params.value("enabled", false));

    const std::size_t window = static_cast<std::size_t>(std::clamp<std::int64_t>(
        ctx.params.value("window", static_cast<std::int64_t>(ECS::SystemWaveTrace::kFrameCapacity)), 1,
        static_cast<std::int64_t>(ECS::SystemWaveTrace::kFrameCapacity)));
    std::vector<ECS::SystemWaveTrace::Frame> frames;
    trace.CopyFrames(frames, window);

    std::vector<std::string> names(systems->GetSequentialSystemCount());
    for (std::size_t slot = 0; slot < names.size(); ++slot)
    {
        const char* name = systems->GetSequentialSystemName(slot);
        names[slot] = name ? name : "<retired>";
    }

    Editor::SystemWaveTraceContext context;
    context.Enabled = trace.IsEnabled();
    context.FramesRecorded = trace.GetFramesRecorded();
    context.SystemNames = names;
    context.DetailFrames = static_cast<std::size_t>(std::max<std::int64_t>(0, ctx.params.value("frames", 0)));
    return Editor::BuildSystemWaveTraceReport(frames, context);
}

} // namespace

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

    // get_ecs_wave_trace — how the ECS waves ran, frame by frame, read after the fact from the
    // rendering loop's SystemWaveTrace ring (the newest 256 SystemManager::Update calls), so it
    // sees forks a live get_job_system poll, which runs between frames, cannot. Params:
    //   enabled (bool, optional): arm or disarm the trace (arming clears the ring); omitted, it
    //     stays as it is. Frames are recorded only while it is armed.
    //   window (int, optional): summarise the newest N frames (default and cap 256).
    //   frames (int, optional): also describe the newest N frames one by one (default 0).
    // Reply: enabled, framesRecorded, frameCapacity, summary (ecsMs, framePeriodMs, joinWaitMs,
    //   inlineMs, helpMs, gapsMs, forkedWaves, forks, joins, publishes {waveForks, inSystems,
    //   duringUpdate, perFrame, outsideSystems}, dropped), waves[] per plan wave, systems[] per
    //   system, frames[] (see SystemWaveTraceReport.h).
    server.RegisterHandler("get_ecs_wave_trace", HandleEcsWaveTrace);
}

} // namespace GameEngine
