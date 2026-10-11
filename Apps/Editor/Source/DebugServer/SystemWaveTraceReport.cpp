#include "DebugServer/SystemWaveTraceReport.h"

#include <algorithm>
#include <map>
#include <vector>

namespace GameEngine::Editor
{

namespace
{

using json = nlohmann::json;
using Frame = ECS::SystemWaveTrace::Frame;
using WaveSample = ECS::SystemWaveTrace::WaveSample;
using SystemSample = ECS::SystemWaveTrace::SystemSample;

constexpr double kNsPerMs = 1e6;

double Ms(std::uint64_t ns)
{
    return static_cast<double>(ns) / kNsPerMs;
}

// Nearest-rank percentile of an unsorted sample; `values` is sorted in place.
double Percentile(std::vector<double>& values, double fraction)
{
    std::sort(values.begin(), values.end());
    const std::size_t rank = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1) + 0.5);
    return values[std::min(rank, values.size() - 1)];
}

json Distribution(std::vector<double> values)
{
    if (values.empty())
        return nullptr;
    const double median = Percentile(values, 0.5);
    return json{{"median", median}, {"p95", Percentile(values, 0.95)}, {"max", values.back()}};
}

std::string SystemName(const SystemWaveTraceContext& context, std::uint32_t slot)
{
    if (slot < context.SystemNames.size())
        return context.SystemNames[slot];
    return "<slot " + std::to_string(slot) + ">";
}

bool IsForked(const SystemSample& sample)
{
    return sample.PublishNs != 0;
}

std::uint64_t BodyNs(const SystemSample& sample)
{
    return sample.EndNs - sample.BeginNs;
}

// The gap between this wave's start and the end of whatever ran before it in the frame.
std::uint64_t GapBeforeNs(const Frame& frame, std::size_t wave)
{
    const std::uint64_t previousEnd = wave == 0 ? frame.BeginNs : frame.Waves[wave - 1].EndNs;
    return frame.Waves[wave].BeginNs - previousEnd;
}

struct FrameTotals
{
    std::uint64_t JoinWaitNs = 0;
    std::uint64_t InlineNs = 0;
    std::uint64_t HelpNs = 0;
    std::uint64_t WaveNs = 0;
    std::uint32_t ForkedWaves = 0;
    std::uint32_t Forks = 0;
    std::uint32_t Joins = 0;
    std::uint32_t SystemPublishes = 0;
};

FrameTotals Totals(const Frame& frame)
{
    FrameTotals totals;
    for (std::size_t w = 0; w < frame.WaveCount; ++w)
    {
        const WaveSample& wave = frame.Waves[w];
        totals.JoinWaitNs += wave.JoinWaitNs;
        totals.WaveNs += wave.EndNs - wave.BeginNs;
        totals.Forks += wave.Forks;
        totals.Joins += wave.Joins;
        totals.ForkedWaves += wave.Forks > 0 ? 1u : 0u;
    }
    for (std::size_t s = 0; s < frame.SystemCount; ++s)
    {
        const SystemSample& sample = frame.Systems[s];
        totals.SystemPublishes += sample.Publishes;
        if (!IsForked(sample))
            totals.InlineNs += BodyNs(sample);
        else if (sample.Worker == ECS::SystemWaveTrace::kCallerThread)
            totals.HelpNs += BodyNs(sample);
    }
    return totals;
}

json DescribeSummary(std::span<const Frame> frames)
{
    std::vector<double> ecs, period, joinWait, inlineWork, help, gaps, forkedWaves, forks, joins;
    std::vector<double> waveForkPublishes, systemPublishes, duringUpdate, perFrame, outside;
    std::uint64_t droppedWaves = 0;
    std::uint64_t droppedSystems = 0;
    for (std::size_t f = 0; f < frames.size(); ++f)
    {
        const Frame& frame = frames[f];
        const FrameTotals totals = Totals(frame);
        const std::uint64_t ecsNs = frame.EndNs - frame.BeginNs;
        ecs.push_back(Ms(ecsNs));
        joinWait.push_back(Ms(totals.JoinWaitNs));
        inlineWork.push_back(Ms(totals.InlineNs));
        help.push_back(Ms(totals.HelpNs));
        gaps.push_back(Ms(ecsNs - std::min(ecsNs, totals.WaveNs)));
        forkedWaves.push_back(totals.ForkedWaves);
        forks.push_back(totals.Forks);
        joins.push_back(totals.Joins);
        waveForkPublishes.push_back(frame.WaveForkPublishes);
        systemPublishes.push_back(totals.SystemPublishes);
        duringUpdate.push_back(static_cast<double>(frame.PublishedAtEnd - frame.PublishedAtBegin));
        droppedWaves += frame.DroppedWaves;
        droppedSystems += frame.DroppedSystems;
        // Consecutive frames only: a gap in Index means frames were overwritten or dropped.
        if (f + 1 < frames.size() && frames[f + 1].Index == frame.Index + 1)
        {
            const Frame& next = frames[f + 1];
            period.push_back(Ms(next.BeginNs - frame.BeginNs));
            const std::uint64_t published = next.PublishedAtBegin - frame.PublishedAtBegin;
            perFrame.push_back(static_cast<double>(published));
            const std::uint64_t attributed = frame.WaveForkPublishes + totals.SystemPublishes;
            outside.push_back(static_cast<double>(published - std::min(published, attributed)));
        }
    }
    return json{{"frames", frames.size()},
                {"ecsMs", Distribution(ecs)},
                {"framePeriodMs", Distribution(period)},
                {"joinWaitMs", Distribution(joinWait)},
                {"inlineMs", Distribution(inlineWork)},
                {"helpMs", Distribution(help)},
                {"gapsMs", Distribution(gaps)},
                {"forkedWaves", Distribution(forkedWaves)},
                {"forks", Distribution(forks)},
                {"joins", Distribution(joins)},
                {"publishes",
                 {{"waveForks", Distribution(waveForkPublishes)},
                  {"inSystems", Distribution(systemPublishes)},
                  {"duringUpdate", Distribution(duringUpdate)},
                  {"perFrame", Distribution(perFrame)},
                  {"outsideSystems", Distribution(outside)}}},
                {"dropped", {{"waves", droppedWaves}, {"systems", droppedSystems}}}};
}

struct WaveRow
{
    std::vector<std::uint32_t> Slots;
    std::vector<double> Makespan, GapBefore, JoinWait, CriticalPath, Dispatch, Forks;
};

json DescribeWaves(std::span<const Frame> frames, const SystemWaveTraceContext& context)
{
    std::map<std::uint32_t, WaveRow> rows;
    for (const Frame& frame : frames)
    {
        for (std::size_t w = 0; w < frame.WaveCount; ++w)
        {
            const WaveSample& wave = frame.Waves[w];
            WaveRow& row = rows[wave.PlanWave];
            row.Makespan.push_back(Ms(wave.EndNs - wave.BeginNs));
            row.GapBefore.push_back(Ms(GapBeforeNs(frame, w)));
            row.Forks.push_back(wave.Forks);
            std::uint64_t slowestFork = 0;
            for (std::size_t s = wave.FirstSystem; s < static_cast<std::size_t>(wave.FirstSystem) + wave.SystemCount; ++s)
            {
                const SystemSample& sample = frame.Systems[s];
                if (std::find(row.Slots.begin(), row.Slots.end(), sample.Slot) == row.Slots.end())
                    row.Slots.push_back(sample.Slot);
                if (!IsForked(sample))
                    continue;
                slowestFork = std::max(slowestFork, BodyNs(sample));
                row.Dispatch.push_back(Ms(sample.BeginNs - sample.PublishNs));
            }
            if (wave.Joins > 0)
            {
                row.JoinWait.push_back(Ms(wave.JoinWaitNs));
                row.CriticalPath.push_back(Ms(slowestFork));
            }
        }
    }
    json waves = json::array();
    for (auto& [planWave, row] : rows)
    {
        json systems = json::array();
        for (std::uint32_t slot : row.Slots)
            systems.push_back(SystemName(context, slot));
        waves.push_back({{"planWave", planWave},
                         {"systems", std::move(systems)},
                         {"frames", row.Makespan.size()},
                         {"forks", Distribution(row.Forks)},
                         {"makespanMs", Distribution(row.Makespan)},
                         {"gapBeforeMs", Distribution(row.GapBefore)},
                         {"joinWaitMs", Distribution(row.JoinWait)},
                         {"criticalPathMs", Distribution(row.CriticalPath)},
                         {"dispatchMs", Distribution(row.Dispatch)}});
    }
    return waves;
}

struct SystemRow
{
    std::uint32_t PlanWave = 0;
    std::vector<double> Body;
    std::uint64_t OnWorker = 0;
    std::uint64_t OnCaller = 0;
    std::uint64_t Forked = 0;
    std::uint64_t Publishes = 0;
};

json DescribeSystems(std::span<const Frame> frames, const SystemWaveTraceContext& context)
{
    std::map<std::uint32_t, SystemRow> rows;
    for (const Frame& frame : frames)
    {
        for (std::size_t s = 0; s < frame.SystemCount; ++s)
        {
            const SystemSample& sample = frame.Systems[s];
            SystemRow& row = rows[sample.Slot];
            row.PlanWave = frame.Waves[sample.Wave].PlanWave;
            row.Body.push_back(Ms(BodyNs(sample)));
            (sample.Worker == ECS::SystemWaveTrace::kCallerThread ? row.OnCaller : row.OnWorker) += 1;
            row.Forked += IsForked(sample) ? 1u : 0u;
            row.Publishes += sample.Publishes;
        }
    }
    std::vector<json> systems;
    for (auto& [slot, row] : rows)
    {
        const std::size_t updates = row.Body.size();
        systems.push_back({{"name", SystemName(context, slot)},
                           {"slot", slot},
                           {"planWave", row.PlanWave},
                           {"updates", updates},
                           {"forked", row.Forked},
                           {"onWorker", row.OnWorker},
                           {"onCaller", row.OnCaller},
                           {"publishesPerUpdate", static_cast<double>(row.Publishes) / static_cast<double>(updates)},
                           {"ms", Distribution(row.Body)}});
    }
    std::sort(systems.begin(), systems.end(), [](const json& a, const json& b)
              { return a["ms"]["median"].get<double>() > b["ms"]["median"].get<double>(); });
    return json(std::move(systems));
}

json DescribeFrame(const Frame& frame, const SystemWaveTraceContext& context)
{
    json waves = json::array();
    for (std::size_t w = 0; w < frame.WaveCount; ++w)
    {
        const WaveSample& wave = frame.Waves[w];
        json systems = json::array();
        for (std::size_t s = wave.FirstSystem; s < static_cast<std::size_t>(wave.FirstSystem) + wave.SystemCount; ++s)
        {
            const SystemSample& sample = frame.Systems[s];
            json entry{{"name", SystemName(context, sample.Slot)},
                       {"thread", sample.Worker == ECS::SystemWaveTrace::kCallerThread
                                      ? json("caller")
                                      : json("Job Worker #" + std::to_string(sample.Worker))},
                       {"beginMs", Ms(sample.BeginNs - frame.BeginNs)},
                       {"ms", Ms(BodyNs(sample))},
                       {"publishes", sample.Publishes}};
            if (IsForked(sample))
                entry["dispatchMs"] = Ms(sample.BeginNs - sample.PublishNs);
            systems.push_back(std::move(entry));
        }
        waves.push_back({{"planWave", wave.PlanWave},
                         {"beginMs", Ms(wave.BeginNs - frame.BeginNs)},
                         {"makespanMs", Ms(wave.EndNs - wave.BeginNs)},
                         {"gapBeforeMs", Ms(GapBeforeNs(frame, w))},
                         {"forks", wave.Forks},
                         {"joins", wave.Joins},
                         {"joinWaitMs", Ms(wave.JoinWaitNs)},
                         {"systems", std::move(systems)}});
    }
    return json{{"index", frame.Index},
                {"ecsMs", Ms(frame.EndNs - frame.BeginNs)},
                {"publishedDuringUpdate", frame.PublishedAtEnd - frame.PublishedAtBegin},
                {"waveForkPublishes", frame.WaveForkPublishes},
                {"waves", std::move(waves)}};
}

} // namespace

json BuildSystemWaveTraceReport(std::span<const Frame> frames, const SystemWaveTraceContext& context)
{
    json detail = json::array();
    const std::size_t detailCount = std::min(context.DetailFrames, frames.size());
    for (std::size_t f = frames.size() - detailCount; f < frames.size(); ++f)
        detail.push_back(DescribeFrame(frames[f], context));

    return json{{"enabled", context.Enabled},
                {"framesRecorded", context.FramesRecorded},
                {"frameCapacity", ECS::SystemWaveTrace::kFrameCapacity},
                {"summary", DescribeSummary(frames)},
                {"waves", DescribeWaves(frames, context)},
                {"systems", DescribeSystems(frames, context)},
                {"frames", std::move(detail)}};
}

} // namespace GameEngine::Editor
