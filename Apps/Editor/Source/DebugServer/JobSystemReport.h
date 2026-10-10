#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>

namespace JobSystem
{
struct JobSystemStatistics;
}

namespace GameEngine::Editor
{

// The get_job_system reply for one read of the pool's statistics: thread counts, each lane's
// queued and running jobs, one row per channel, one row per pool thread named as the thread is
// ("Job Worker #3", "Job Blocking #0") with what it runs and, for Background and channel jobs,
// for how long (`nowNs` is the steady_clock reading the durations are measured against), the
// derived tasksExecuted and the queue-topology census.
nlohmann::json BuildJobSystemReport(const JobSystem::JobSystemStatistics& stats, std::uint64_t nowNs);

} // namespace GameEngine::Editor
