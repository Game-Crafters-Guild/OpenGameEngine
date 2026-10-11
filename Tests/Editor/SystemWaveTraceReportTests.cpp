// get_ecs_wave_trace's reply (BuildSystemWaveTraceReport) over hand-built frames whose every
// derived number is known: makespans, gaps, join waits, inline and helped work, and where the
// frame's publishes came from.

#include <gtest/gtest.h>

#include "DebugServer/SystemWaveTraceReport.h"

#include <string>
#include <vector>

namespace
{

using GameEngine::ECS::SystemWaveTrace;
using GameEngine::Editor::BuildSystemWaveTraceReport;
using GameEngine::Editor::SystemWaveTraceContext;

constexpr std::uint64_t kUs = 1000;
constexpr std::uint64_t kMs = 1000 * kUs;

// One frame starting at `start`: wave 0 forks "Fast" and "Slow" (published at +10 us, running
// 20 us and 100 us on workers 0 and 1, joined at +115 us after a 100 us wait), then after a
// 5 us gap wave 1 runs "Inline" on the caller for 50 us; 2 us after it the frame ends.
// Slow publishes 3 jobs of its own; the publish loop published 2; the pool saw 7 during the
// update and 9 from this frame's start to the next.
SystemWaveTrace::Frame MakeFrame(std::uint64_t index, std::uint64_t start)
{
    SystemWaveTrace::Frame frame;
    frame.Index = index;
    frame.BeginNs = start;
    frame.PublishedAtBegin = 100 + 9 * index;
    frame.PublishedAtEnd = frame.PublishedAtBegin + 7;
    frame.WaveForkPublishes = 2;
    frame.WaveCount = 2;
    frame.SystemCount = 3;

    SystemWaveTrace::WaveSample& fork = frame.Waves[0];
    fork.PlanWave = 0;
    fork.FirstSystem = 0;
    fork.SystemCount = 2;
    fork.Forks = 2;
    fork.Joins = 1;
    fork.BeginNs = start + 5 * kUs;
    fork.EndNs = start + 120 * kUs;
    fork.JoinWaitNs = 100 * kUs;

    SystemWaveTrace::SystemSample& fast = frame.Systems[0];
    fast.Slot = 0;
    fast.Wave = 0;
    fast.Worker = 0;
    fast.PublishNs = start + 10 * kUs;
    fast.BeginNs = start + 14 * kUs;
    fast.EndNs = start + 34 * kUs;

    SystemWaveTrace::SystemSample& slow = frame.Systems[1];
    slow.Slot = 1;
    slow.Wave = 0;
    slow.Worker = 1;
    slow.PublishNs = start + 10 * kUs;
    slow.BeginNs = start + 16 * kUs;
    slow.EndNs = start + 116 * kUs;
    slow.Publishes = 3;

    SystemWaveTrace::WaveSample& single = frame.Waves[1];
    single.PlanWave = 1;
    single.FirstSystem = 2;
    single.SystemCount = 1;
    single.BeginNs = start + 125 * kUs;
    single.EndNs = start + 175 * kUs;

    SystemWaveTrace::SystemSample& inlineSystem = frame.Systems[2];
    inlineSystem.Slot = 2;
    inlineSystem.Wave = 1;
    inlineSystem.BeginNs = single.BeginNs;
    inlineSystem.EndNs = single.EndNs;

    frame.EndNs = start + 177 * kUs;
    return frame;
}

const std::vector<std::string> kNames = {"Fast", "Slow", "Inline"};

std::vector<SystemWaveTrace::Frame> ThreeFrames()
{
    return {MakeFrame(0, 1 * kMs), MakeFrame(1, 17 * kMs), MakeFrame(2, 33 * kMs)};
}

SystemWaveTraceContext Context(std::size_t detail = 0)
{
    SystemWaveTraceContext context;
    context.Enabled = true;
    context.FramesRecorded = 3;
    context.SystemNames = kNames;
    context.DetailFrames = detail;
    return context;
}

} // namespace

TEST(SystemWaveTraceReportTest, SummarisesTheFramesWithTheirDerivedTimes)
{
    // The newest frame's ECS time runs 3 us longer: 177, 177 and 180 us, whose
    // nearest-rank p95 is the third value (rank 0.95 * 2 = 1.9 rounds to 2).
    std::vector<SystemWaveTrace::Frame> frames = ThreeFrames();
    frames[2].EndNs += 3 * kUs;
    const nlohmann::json report = BuildSystemWaveTraceReport(frames, Context());

    EXPECT_EQ(report["enabled"], true);
    EXPECT_EQ(report["framesRecorded"], 3);
    EXPECT_EQ(report["frameCapacity"], SystemWaveTrace::kFrameCapacity);
    const nlohmann::json& summary = report["summary"];
    EXPECT_EQ(summary["frames"], 3);
    EXPECT_DOUBLE_EQ(summary["ecsMs"]["median"].get<double>(), 0.177);
    EXPECT_DOUBLE_EQ(summary["ecsMs"]["p95"].get<double>(), 0.180);
    EXPECT_DOUBLE_EQ(summary["ecsMs"]["max"].get<double>(), 0.180);
    EXPECT_DOUBLE_EQ(summary["framePeriodMs"]["median"].get<double>(), 16.0);
    EXPECT_DOUBLE_EQ(summary["joinWaitMs"]["median"].get<double>(), 0.1);
    EXPECT_DOUBLE_EQ(summary["inlineMs"]["median"].get<double>(), 0.05);
    EXPECT_DOUBLE_EQ(summary["helpMs"]["median"].get<double>(), 0.0);
    // 177 us of ECS time minus 115 + 50 us inside waves.
    EXPECT_DOUBLE_EQ(summary["gapsMs"]["median"].get<double>(), 0.012);
    EXPECT_EQ(summary["forkedWaves"]["median"], 1.0);
    EXPECT_EQ(summary["forks"]["median"], 2.0);
    EXPECT_EQ(summary["joins"]["median"], 1.0);
    const nlohmann::json& publishes = summary["publishes"];
    EXPECT_EQ(publishes["waveForks"]["median"], 2.0);
    EXPECT_EQ(publishes["inSystems"]["median"], 3.0);
    EXPECT_EQ(publishes["duringUpdate"]["median"], 7.0);
    EXPECT_EQ(publishes["perFrame"]["median"], 9.0);
    // 9 per frame, 2 + 3 of them attributed.
    EXPECT_EQ(publishes["outsideSystems"]["median"], 4.0);
    EXPECT_EQ(summary["dropped"]["waves"], 0);
}

TEST(SystemWaveTraceReportTest, DescribesEachPlanWave)
{
    const std::vector<SystemWaveTrace::Frame> frames = ThreeFrames();
    const nlohmann::json waves = BuildSystemWaveTraceReport(frames, Context())["waves"];
    ASSERT_EQ(waves.size(), 2u);

    const nlohmann::json& fork = waves[0];
    EXPECT_EQ(fork["planWave"], 0);
    EXPECT_EQ(fork["systems"], (nlohmann::json{"Fast", "Slow"}));
    EXPECT_EQ(fork["frames"], 3);
    EXPECT_DOUBLE_EQ(fork["makespanMs"]["median"].get<double>(), 0.115);
    EXPECT_DOUBLE_EQ(fork["gapBeforeMs"]["median"].get<double>(), 0.005);
    EXPECT_DOUBLE_EQ(fork["joinWaitMs"]["median"].get<double>(), 0.1);
    EXPECT_DOUBLE_EQ(fork["criticalPathMs"]["median"].get<double>(), 0.1);
    // Dispatch delays are 4 us (Fast) and 6 us (Slow) in every frame.
    EXPECT_DOUBLE_EQ(fork["dispatchMs"]["max"].get<double>(), 0.006);

    const nlohmann::json& single = waves[1];
    EXPECT_EQ(single["systems"], (nlohmann::json{"Inline"}));
    EXPECT_DOUBLE_EQ(single["gapBeforeMs"]["median"].get<double>(), 0.005);
    EXPECT_TRUE(single["joinWaitMs"].is_null());
    EXPECT_TRUE(single["dispatchMs"].is_null());
}

TEST(SystemWaveTraceReportTest, DescribesEachSystemSlowestFirst)
{
    const std::vector<SystemWaveTrace::Frame> frames = ThreeFrames();
    const nlohmann::json systems = BuildSystemWaveTraceReport(frames, Context())["systems"];
    ASSERT_EQ(systems.size(), 3u);
    EXPECT_EQ(systems[0]["name"], "Slow");
    EXPECT_EQ(systems[1]["name"], "Inline");
    EXPECT_EQ(systems[2]["name"], "Fast");

    EXPECT_EQ(systems[0]["updates"], 3);
    EXPECT_EQ(systems[0]["forked"], 3);
    EXPECT_EQ(systems[0]["onWorker"], 3);
    EXPECT_EQ(systems[0]["onCaller"], 0);
    EXPECT_DOUBLE_EQ(systems[0]["publishesPerUpdate"].get<double>(), 3.0);
    EXPECT_DOUBLE_EQ(systems[0]["ms"]["median"].get<double>(), 0.1);

    EXPECT_EQ(systems[1]["planWave"], 1);
    EXPECT_EQ(systems[1]["forked"], 0);
    EXPECT_EQ(systems[1]["onCaller"], 3);
}

TEST(SystemWaveTraceReportTest, AForkedBodyRunOnTheCallerCountsAsHelp)
{
    std::vector<SystemWaveTrace::Frame> frames = ThreeFrames();
    for (SystemWaveTrace::Frame& frame : frames)
        frame.Systems[0].Worker = SystemWaveTrace::kCallerThread;
    const nlohmann::json report = BuildSystemWaveTraceReport(frames, Context());
    EXPECT_DOUBLE_EQ(report["summary"]["helpMs"]["median"].get<double>(), 0.02);
    EXPECT_DOUBLE_EQ(report["summary"]["inlineMs"]["median"].get<double>(), 0.05);
}

TEST(SystemWaveTraceReportTest, DescribesTheNewestFramesOneByOne)
{
    const std::vector<SystemWaveTrace::Frame> frames = ThreeFrames();
    const nlohmann::json detail = BuildSystemWaveTraceReport(frames, Context(1))["frames"];
    ASSERT_EQ(detail.size(), 1u);
    EXPECT_EQ(detail[0]["index"], 2);
    EXPECT_EQ(detail[0]["publishedDuringUpdate"], 7);
    const nlohmann::json& fork = detail[0]["waves"][0];
    EXPECT_DOUBLE_EQ(fork["beginMs"].get<double>(), 0.005);
    ASSERT_EQ(fork["systems"].size(), 2u);
    EXPECT_EQ(fork["systems"][1]["name"], "Slow");
    EXPECT_EQ(fork["systems"][1]["thread"], "Job Worker #1");
    EXPECT_DOUBLE_EQ(fork["systems"][1]["dispatchMs"].get<double>(), 0.006);
    const nlohmann::json& inlineSystem = detail[0]["waves"][1]["systems"][0];
    EXPECT_EQ(inlineSystem["thread"], "caller");
    EXPECT_FALSE(inlineSystem.contains("dispatchMs"));
}

TEST(SystemWaveTraceReportTest, NonConsecutiveFramesGiveNoPeriod)
{
    std::vector<SystemWaveTrace::Frame> frames = {MakeFrame(0, 1 * kMs), MakeFrame(5, 81 * kMs)};
    const nlohmann::json summary = BuildSystemWaveTraceReport(frames, Context())["summary"];
    EXPECT_TRUE(summary["framePeriodMs"].is_null());
    EXPECT_TRUE(summary["publishes"]["perFrame"].is_null());
    EXPECT_EQ(summary["frames"], 2);
}

TEST(SystemWaveTraceReportTest, AnEmptyTraceReportsNoDistributions)
{
    SystemWaveTraceContext context = Context();
    context.Enabled = false;
    context.FramesRecorded = 0;
    const nlohmann::json report = BuildSystemWaveTraceReport({}, context);
    EXPECT_EQ(report["enabled"], false);
    EXPECT_EQ(report["summary"]["frames"], 0);
    EXPECT_TRUE(report["summary"]["ecsMs"].is_null());
    EXPECT_TRUE(report["waves"].empty());
    EXPECT_TRUE(report["systems"].empty());
}
