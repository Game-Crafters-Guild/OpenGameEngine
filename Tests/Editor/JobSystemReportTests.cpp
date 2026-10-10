// get_job_system's reply (BuildJobSystemReport): the shape an agent reads.

#include <gtest/gtest.h>

#include "DebugServer/JobSystemReport.h"
#include "JobSystem/JobSystemStatistics.h"

namespace
{

using GameEngine::Editor::BuildJobSystemReport;
using JobSystem::JobSystemStatistics;

JobSystemStatistics SampleStatistics()
{
    JobSystemStatistics stats;
    stats.ComputeWorkers = 2;
    stats.BlockingThreads = 1;
    stats.Normal = {3, 1};
    stats.Background = {7, 1};
    stats.Channels.push_back({"Script compiles", 2, 1, 1, 1'500'000, 9});
    stats.Threads.push_back({JobSystemStatistics::ThreadKind::Worker, 0, "Normal", 0});
    stats.Threads.push_back({JobSystemStatistics::ThreadKind::Worker, 1, "Background", 1'000'000'000});
    stats.Threads.push_back({JobSystemStatistics::ThreadKind::Blocking, 0, nullptr, 0});
    stats.TasksExecuted = 42;
    stats.GlobalPushes = 30;
    stats.BackgroundPops = 11;
    stats.BackstopTimeouts = 5;
    return stats;
}

} // namespace

TEST(JobSystemReportTest, ReportsEveryStatisticUnderItsName)
{
    const nlohmann::json report = BuildJobSystemReport(SampleStatistics(), 3'500'000'000);

    EXPECT_EQ(report["computeWorkers"], 2);
    EXPECT_EQ(report["blockingThreads"], 1);
    EXPECT_EQ(report["lanes"]["normal"]["queued"], 3);
    EXPECT_EQ(report["lanes"]["normal"]["running"], 1);
    EXPECT_EQ(report["lanes"]["background"]["queued"], 7);
    EXPECT_EQ(report["tasksExecuted"], 42);
    EXPECT_EQ(report["census"]["globalPushes"], 30);
    EXPECT_EQ(report["census"]["backgroundPops"], 11);
    EXPECT_EQ(report["census"]["backstopTimeouts"], 5);
    EXPECT_EQ(report["census"].size(), 10u);

    ASSERT_EQ(report["channels"].size(), 1u);
    const nlohmann::json& channel = report["channels"][0];
    EXPECT_EQ(channel["name"], "Script compiles");
    EXPECT_EQ(channel["queued"], 2);
    EXPECT_EQ(channel["running"], 1);
    EXPECT_EQ(channel["cap"], 1);
    EXPECT_EQ(channel["jobs"], 9);
    EXPECT_DOUBLE_EQ(channel["queueWaitMs"].get<double>(), 1.5);
}

// Threads carry the names the threads have; only a job with a start time gets
// a duration, and an idle thread reads null.
TEST(JobSystemReportTest, NamesEachThreadAndTimesOnlyTimedJobs)
{
    const nlohmann::json threads = BuildJobSystemReport(SampleStatistics(), 3'500'000'000)["threads"];
    ASSERT_EQ(threads.size(), 3u);

    EXPECT_EQ(threads[0]["thread"], "Job Worker #0");
    EXPECT_EQ(threads[0]["kind"], "worker");
    EXPECT_EQ(threads[0]["running"], "Normal");
    EXPECT_FALSE(threads[0].contains("runningForMs"));

    EXPECT_EQ(threads[1]["thread"], "Job Worker #1");
    EXPECT_EQ(threads[1]["running"], "Background");
    EXPECT_DOUBLE_EQ(threads[1]["runningForMs"].get<double>(), 2500.0);

    EXPECT_EQ(threads[2]["thread"], "Job Blocking #0");
    EXPECT_EQ(threads[2]["kind"], "blocking");
    EXPECT_TRUE(threads[2]["running"].is_null());
    EXPECT_FALSE(threads[2].contains("runningForMs"));
}
