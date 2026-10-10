#pragma once

#include "ECS/World.h"
#include "ECS/Systems.h"
#include "ECS/Query.h"
#include "ECS/QueryPolicy.h"
#include "JobSystem/JobCounter.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include <vector>
#include <functional>
#include <chrono>
#include <cstdio>
#include <type_traits>

namespace GameEngine {
namespace ECS {

// A minimal conflict-free scheduler prototype with helper and fluent SystemJob APIs
class Scheduler {
public:
    struct JobDesc {
        DenseSignature ReadSet;
        DenseSignature WriteSet;
        std::function<void()> Run;
        const char* Name = "UnnamedJob";
    };

    // Optional dependency edge (index-based); simple pair for now
    struct Dependency { size_t Before; size_t After; };

    // Fluent SystemJob wrapper
    class SystemJob {
    public:
        JobDesc Desc;
        std::vector<size_t> DependsOnList; // indices of prior jobs in the submission list

        SystemJob& Name(const char* n) { Desc.Name = n; return *this; }
        SystemJob& WithName(const char* n) { Desc.Name = n; return *this; }
        SystemJob& DependsOn(size_t idx) { DependsOnList.push_back(idx); return *this; }

        template<typename QueryT, typename Fn>
        static SystemJob Each(QueryT& q, Fn&& fn) {
            SystemJob sj; sj.Desc = Scheduler::MakeJobEach(q, std::forward<Fn>(fn)); return sj; }
        template<typename QueryT, typename Fn>
        static SystemJob Parallel(QueryT& q, Fn&& fn, std::size_t minBatchSize = 0) {
            SystemJob sj; sj.Desc = Scheduler::MakeJobParallel(q, std::forward<Fn>(fn), minBatchSize); return sj; }
        template<typename QueryT, typename Fn>
        static SystemJob ForEachChunk(QueryT& q, Fn&& fn) {
            SystemJob sj; sj.Desc = Scheduler::MakeJobForEachChunk(q, std::forward<Fn>(fn)); return sj; }
        template<typename QueryT, typename Fn>
        static SystemJob ParallelChunks(QueryT& q, Fn&& fn) {
            SystemJob sj; sj.Desc = Scheduler::MakeJobParallelChunks(q, std::forward<Fn>(fn)); return sj; }
    };



private:
    JobSystem::WorkStealingThreadPool* m_JobSystem = nullptr;

public:
    explicit Scheduler(JobSystem::WorkStealingThreadPool* js = nullptr) : m_JobSystem(js) {}

    void SetJobSystem(JobSystem::WorkStealingThreadPool* js) { m_JobSystem = js; }

    // Helpers to build JobDesc from a Query and a lambda for different iteration modes
    template<typename QueryT, typename Fn>
    static JobDesc MakeJobEach(QueryT& query, Fn&& fn, const char* name = "UnnamedJob") {
        JobDesc jd;
        jd.ReadSet = query.GetReadSet();
        jd.WriteSet = query.GetWriteSet();
        jd.Run = [&, f = std::forward<Fn>(fn)]() mutable { query.Each(std::move(f)); };
        jd.Name = name;
        return jd;
    }

    template<typename QueryT, typename Fn>
    static JobDesc MakeJobParallel(QueryT& query, Fn&& fn, std::size_t minBatchSize = 0, const char* name = "UnnamedJob") {
        JobDesc jd;
        jd.ReadSet = query.GetReadSet();
        jd.WriteSet = query.GetWriteSet();
        jd.Run = [&, f = std::forward<Fn>(fn), minBatchSize]() mutable {
            // Synchronous fork-join (slice 5): safe even though this runs on a
            // worker — the join inside Parallel participates on its own batches.
            query.Parallel(f, minBatchSize);
        };
        jd.Name = name;
        return jd;
    }

    // Chunk helpers support both pointer-only lambdas and pointer+count lambdas without relying on Query internals
    template<typename QueryT, typename Fn>
    static JobDesc MakeJobForEachChunk(QueryT& query, Fn&& fn, const char* name = "UnnamedJob") {
        JobDesc jd;
        jd.ReadSet = query.GetReadSet();
        jd.WriteSet = query.GetWriteSet();
        jd.Run = [&, f = std::forward<Fn>(fn)]() mutable {
            query.ForEachChunk(std::move(f));
        };
        jd.Name = name;
        return jd;
    }

    template<typename QueryT, typename Fn>
    static JobDesc MakeJobParallelChunks(QueryT& query, Fn&& fn, const char* name = "UnnamedJob") {
        JobDesc jd;
        jd.ReadSet = query.GetReadSet();
        jd.WriteSet = query.GetWriteSet();
        jd.Run = [&, f = std::forward<Fn>(fn)]() mutable {
            // Synchronous fork-join (slice 5, see MakeJobParallel).
            query.ParallelChunks(std::move(f));
        };
        jd.Name = name;
        return jd;
    }

    // Schedule SystemJobs; derive dependencies and run (with optional diagnostics)
    void Run(const std::vector<SystemJob>& systemJobs) {
        std::vector<JobDesc> jobs; jobs.reserve(systemJobs.size());
        std::vector<Dependency> deps;
        for (size_t i = 0; i < systemJobs.size(); ++i) {
            jobs.push_back(systemJobs[i].Desc);
            for (size_t d : systemJobs[i].DependsOnList) deps.push_back({ d, i });
        }
        Run(jobs, deps);
    }

    // Schedule jobs with optional dependencies in waves; jobs within a wave are run in parallel if conflict-free
    void Run(const std::vector<JobDesc>& jobs, const std::vector<Dependency>& deps = {}) {
    #if ECS_SCHEDULER_DIAGNOSTICS
        auto t0 = std::chrono::high_resolution_clock::now();
    #endif
        std::vector<bool> scheduled(jobs.size(), false);
        std::vector<size_t> indegree(jobs.size(), 0);
        std::vector<std::vector<size_t>> outgoing(jobs.size());

        // Build dependency graph
        for (auto& d : deps) {
            if (d.Before < jobs.size() && d.After < jobs.size()) {
                indegree[d.After]++;
                outgoing[d.Before].push_back(d.After);
            }
        }

        size_t remaining = jobs.size();
        [[maybe_unused]] size_t waveIndex = 0;

        while (remaining > 0) {
        #if ECS_SCHEDULER_DIAGNOSTICS
            auto w0 = std::chrono::high_resolution_clock::now();
        #endif
            DenseSignature waveRead;
            DenseSignature waveWrite;
            std::vector<size_t> wave;

            // Greedy pack among nodes with indegree == 0
            for (size_t i = 0; i < jobs.size(); ++i) {
                if (scheduled[i] || indegree[i] != 0) continue;
                const auto& j = jobs[i];

                bool conflict = false;
                if (waveWrite.Intersects(j.WriteSet) ||
                    waveWrite.Intersects(j.ReadSet) ||
                    waveRead.Intersects(j.WriteSet)) {
                    conflict = true;
                }
                if (conflict) continue;

                wave.push_back(i);
                waveRead.UnionInPlace(j.ReadSet);
                waveWrite.UnionInPlace(j.WriteSet);
            }

            if (wave.empty()) {
                // Fallback: pick first unscheduled zero-indegree job; if none, pick any to break cycles
                size_t pick = jobs.size();
                for (size_t i = 0; i < jobs.size(); ++i) if (!scheduled[i] && indegree[i] == 0) { pick = i; break; }
                if (pick == jobs.size()) { for (size_t i = 0; i < jobs.size(); ++i) if (!scheduled[i]) { pick = i; break; } }
                if (pick != jobs.size()) wave.push_back(pick);
            }

            // Wave join on a JobCounter (slice 5): no TaskHandles, no per-task
            // registry traffic; a worker-thread caller participates instead of
            // blocking a pool lane.
            JobSystem::JobCounter waveCounter;
            for (size_t idx : wave) {
                scheduled[idx] = true;
                --remaining;
                if (m_JobSystem) {
                    m_JobSystem->Run(jobs[idx].Run, waveCounter);
                } else {
                    jobs[idx].Run();
                }
            }

            if (m_JobSystem) m_JobSystem->Wait(waveCounter);

            // Reduce indegree for completed nodes
            for (size_t idx : wave) {
                for (size_t to : outgoing[idx]) { if (indegree[to] > 0) indegree[to]--; }
            }
        #if ECS_SCHEDULER_DIAGNOSTICS
            auto w1 = std::chrono::high_resolution_clock::now();
            auto waveMs = std::chrono::duration_cast<std::chrono::microseconds>(w1 - w0).count();
            std::printf("[Scheduler] Wave %zu: tasks=%zu, time=%lldus\n", waveIndex++, wave.size(), (long long)waveMs);
        #endif
        }
    #if ECS_SCHEDULER_DIAGNOSTICS
        auto t1 = std::chrono::high_resolution_clock::now();
        auto totalMs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        std::printf("[Scheduler] Total time=%lldus, jobs=%zu\n", (long long)totalMs, jobs.size());
    #endif
    }
};

} // namespace ECS
} // namespace GameEngine

