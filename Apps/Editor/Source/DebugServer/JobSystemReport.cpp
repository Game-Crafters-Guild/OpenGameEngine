#include "DebugServer/JobSystemReport.h"

#include "JobSystem/JobSystemStatistics.h"

#include <string>

namespace GameEngine::Editor
{

namespace
{

using json = nlohmann::json;
using JobSystem::JobSystemStatistics;

constexpr double kNsPerMs = 1e6;

json DescribeLane(const JobSystemStatistics::Lane& lane)
{
    return json{{"queued", lane.Queued}, {"running", lane.Running}};
}

json DescribeChannel(const JobSystemStatistics::Channel& channel)
{
    return json{{"name", channel.Name ? channel.Name : ""},
                {"queued", channel.Queued},
                {"running", channel.Running},
                {"cap", channel.Cap},
                {"jobs", channel.Jobs},
                {"queueWaitMs", static_cast<double>(channel.QueueWaitNs) / kNsPerMs}};
}

json DescribeThread(const JobSystemStatistics::Occupancy& thread, std::uint64_t nowNs)
{
    const bool worker = thread.Kind == JobSystemStatistics::ThreadKind::Worker;
    json entry{{"thread", std::string(worker ? "Job Worker #" : "Job Blocking #") + std::to_string(thread.Index)},
               {"kind", worker ? "worker" : "blocking"},
               {"index", thread.Index},
               {"running", thread.Running ? json(thread.Running) : json(nullptr)}};
    if (thread.Running != nullptr && thread.SinceNs != 0 && nowNs >= thread.SinceNs)
        entry["runningForMs"] = static_cast<double>(nowNs - thread.SinceNs) / kNsPerMs;
    return entry;
}

} // namespace

json BuildJobSystemReport(const JobSystemStatistics& stats, std::uint64_t nowNs)
{
    json channels = json::array();
    for (const JobSystemStatistics::Channel& channel : stats.Channels)
        channels.push_back(DescribeChannel(channel));

    json threads = json::array();
    for (const JobSystemStatistics::Occupancy& thread : stats.Threads)
        threads.push_back(DescribeThread(thread, nowNs));

    return json{{"computeWorkers", stats.ComputeWorkers},
                {"blockingThreads", stats.BlockingThreads},
                {"lanes", {{"normal", DescribeLane(stats.Normal)}, {"background", DescribeLane(stats.Background)}}},
                {"channels", std::move(channels)},
                {"threads", std::move(threads)},
                {"tasksExecuted", stats.TasksExecuted},
                {"census",
                 {{"globalPushes", stats.GlobalPushes},
                  {"localPushes", stats.LocalPushes},
                  {"backgroundPushes", stats.BackgroundPushes},
                  {"globalPops", stats.GlobalPops},
                  {"localPops", stats.LocalPops},
                  {"stealPops", stats.StealPops},
                  {"backgroundPops", stats.BackgroundPops},
                  {"stealMisses", stats.StealMisses},
                  {"emptyLoopsWithBacklog", stats.EmptyLoopsWithBacklog},
                  {"backstopTimeouts", stats.BackstopTimeouts}}}};
}

} // namespace GameEngine::Editor
