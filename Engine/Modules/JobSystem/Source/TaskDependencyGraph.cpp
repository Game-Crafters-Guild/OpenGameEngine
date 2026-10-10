#include "JobSystem/TaskDependencyGraph.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>

namespace JobSystem {

TaskDependencyGraph::~TaskDependencyGraph() = default;

UniquePtr<Task> TaskDependencyGraph::Register(UniquePtr<Task> task) {
    const TaskId taskId = task->GetTaskId();
    const Vector<TaskId>& declaredDependencies = task->GetDependencies();

    std::lock_guard<std::mutex> lock(m_Mutex);

    auto [it, inserted] = m_Tasks.try_emplace(taskId);
    if (!inserted) {
        // Unreachable via the public API: ids are pool-generated and
        // monotonic, and a UniquePtr payload can only be submitted once.
        // Returning "ready" avoids silently dropping the payload in Release,
        // but it double-registers an id whose completion bookkeeping will
        // then misfire — be loud instead of silent.
        assert(false && "TaskDependencyGraph::Register: duplicate task id");
        return task;
    }

    TaskNode& node = it->second;
    for (TaskId dependencyId : declaredDependencies) {
        if (dependencyId == taskId || dependencyId == 0) {
            continue; // self-dependency / invalid id
        }

        auto depIt = m_Tasks.find(dependencyId);
        if (depIt == m_Tasks.end()) {
            // F8: an absent dependency is TERMINAL (completed, failed, or
            // cancelled — terminal nodes are erased in the same critical
            // section that flips them). The dependent runs; it observes the
            // dependency's outcome through its handle.
#if GE_DEBUG_INSTRUMENTATION
            // F7: a never-submitted dependency id is a submit-order
            // violation — the dependent would silently run early. Loud but
            // NON-FATAL by default: the tombstone ring is a bounded
            // "recently terminal" heuristic, so a dependency held long past
            // terminal (or evicted in an extreme retirement burst) is legal
            // F8 usage that must not crash a Debug editor. Opt into the hard
            // abort for protocol bring-up via GE_JOB_TOMBSTONE_STRICT=1.
            if (!IsRecentlyTerminalLocked(dependencyId)) {
                std::fprintf(stderr,
                             "TaskDependencyGraph: task %llu depends on task %llu which is not "
                             "recently terminal — either a submit-order violation (submit "
                             "dependencies before dependents), a plain Submit(F&&) lambda id "
                             "passed to AddDependency directly (plain lambda submits never "
                             "graph-register; route lambda dependencies through Submit(fn, "
                             "deps), which bridges them), or a handle held long past "
                             "terminal.\n",
                             static_cast<unsigned long long>(taskId),
                             static_cast<unsigned long long>(dependencyId));
                std::fflush(stderr);
                static const bool kStrict = [] {
                    const char* e = std::getenv("GE_JOB_TOMBSTONE_STRICT");
                    return e && e[0] == '1';
                }();
                if (kStrict) {
                    std::abort();
                }
            }
#endif
            continue;
        }

        if (node.Dependencies.insert(dependencyId).second) {
#if GE_DEBUG_INSTRUMENTATION
            AssertNoCycleLocked(taskId, dependencyId);
#endif
            depIt->second.Dependents.insert(taskId);
            ++node.RemainingDependencies;
        }
    }

    if (node.RemainingDependencies == 0) {
        return task; // ready — the caller enqueues after unlock
    }

    node.Parked = std::move(task);
    return nullptr;
}

TaskGraphActions TaskDependencyGraph::MarkCompleted(TaskId taskId, bool success) {
    TaskGraphActions actions;

    std::lock_guard<std::mutex> lock(m_Mutex);

    auto it = m_Tasks.find(taskId);
    if (it == m_Tasks.end()) {
        return actions;
    }

    const TaskStatus terminalStatus = success ? TaskStatus::Completed : TaskStatus::Failed;
    TaskNode& node = it->second;
    node.Status = terminalStatus;
    actions.Events.push_back({taskId, terminalStatus});

    if (success) {
        for (TaskId dependentId : node.Dependents) {
            auto depIt = m_Tasks.find(dependentId);
            if (depIt == m_Tasks.end()) {
                continue; // dependent already cancelled and erased
            }

            TaskNode& dependent = depIt->second;
            if (dependent.RemainingDependencies > 0) {
                --dependent.RemainingDependencies;
            }
            if (dependent.RemainingDependencies == 0 && dependent.Status == TaskStatus::Pending &&
                dependent.Parked) {
                actions.ReadyTasks.push_back(std::move(dependent.Parked));
            }
        }
    } else {
        PropagateFailureLocked(node, TaskStatus::Failed, actions);
    }

#if GE_DEBUG_INSTRUMENTATION
    RecordTombstoneLocked(taskId);
#endif
    m_Tasks.erase(it);

    return actions;
}

bool TaskDependencyGraph::CancelPending(TaskId taskId, TaskGraphActions& actions) {
    std::lock_guard<std::mutex> lock(m_Mutex);

    auto it = m_Tasks.find(taskId);
    if (it == m_Tasks.end() || it->second.Status != TaskStatus::Pending) {
        return false;
    }

    TaskNode& node = it->second;
    actions.Events.push_back({taskId, TaskStatus::Cancelled});
    if (node.Parked) {
        actions.RetiredTasks.push_back(std::move(node.Parked));
    }
    PropagateFailureLocked(node, TaskStatus::Cancelled, actions);

#if GE_DEBUG_INSTRUMENTATION
    RecordTombstoneLocked(taskId);
#endif
    m_Tasks.erase(it);

    return true;
}

void TaskDependencyGraph::CancelAllPending(TaskGraphActions& actions) {
    std::lock_guard<std::mutex> lock(m_Mutex);

    // Snapshot ids first: the per-node cancel cascades into (and erases)
    // other nodes, which would invalidate a live map iteration.
    Vector<TaskId> pendingIds;
    pendingIds.reserve(m_Tasks.size());
    for (const auto& [id, node] : m_Tasks) {
        if (node.Status == TaskStatus::Pending) {
            pendingIds.push_back(id);
        }
    }

    for (TaskId taskId : pendingIds) {
        auto it = m_Tasks.find(taskId);
        if (it == m_Tasks.end() || it->second.Status != TaskStatus::Pending) {
            continue; // already cancelled via an earlier id's cascade
        }

        TaskNode& node = it->second;
        actions.Events.push_back({taskId, TaskStatus::Cancelled});
        if (node.Parked) {
            actions.RetiredTasks.push_back(std::move(node.Parked));
        }
        PropagateFailureLocked(node, TaskStatus::Cancelled, actions);

#if GE_DEBUG_INSTRUMENTATION
        RecordTombstoneLocked(taskId);
#endif
        m_Tasks.erase(it);
    }
}

bool TaskDependencyGraph::EnsureTracked(TaskId taskId) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    // A fresh node's defaults ARE the mirror shape: Pending, no dependencies,
    // no parked payload. Nothing else to set up.
    auto [it, inserted] = m_Tasks.try_emplace(taskId);
    (void)it;
    return inserted;
}

bool TaskDependencyGraph::MarkRunning(TaskId taskId) {
    std::lock_guard<std::mutex> lock(m_Mutex);

    auto it = m_Tasks.find(taskId);
    if (it == m_Tasks.end()) {
        return false;
    }

    it->second.Status = TaskStatus::Running;
    return true;
}

bool TaskDependencyGraph::TaskExists(TaskId taskId) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Tasks.find(taskId) != m_Tasks.end();
}

size_t TaskDependencyGraph::GetTrackedTaskCountForTests() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Tasks.size();
}

void TaskDependencyGraph::PropagateFailureLocked(const TaskNode& fromNode, TaskStatus failureStatus,
                                                 TaskGraphActions& actions) {
    Vector<TaskId> worklist(fromNode.Dependents.begin(), fromNode.Dependents.end());

    while (!worklist.empty()) {
        const TaskId dependentId = worklist.back();
        worklist.pop_back();

        auto it = m_Tasks.find(dependentId);
        if (it == m_Tasks.end()) {
            continue; // already cancelled and erased
        }

        TaskNode& dependent = it->second;
        if (dependent.Status != TaskStatus::Pending) {
            continue;
        }

        actions.Events.push_back({dependentId, failureStatus});
        if (dependent.Parked) {
            actions.RetiredTasks.push_back(std::move(dependent.Parked));
        }
        worklist.insert(worklist.end(), dependent.Dependents.begin(), dependent.Dependents.end());

#if GE_DEBUG_INSTRUMENTATION
        RecordTombstoneLocked(dependentId);
#endif
        m_Tasks.erase(it);
    }
}

#if GE_DEBUG_INSTRUMENTATION

bool TaskDependencyGraph::IsRecentlyTerminalLocked(TaskId taskId) const {
    return std::find(m_Tombstones.begin(), m_Tombstones.end(), taskId) != m_Tombstones.end();
}

void TaskDependencyGraph::RecordTombstoneLocked(TaskId taskId) {
    m_Tombstones[m_TombstoneNext] = taskId;
    m_TombstoneNext = (m_TombstoneNext + 1) % kTombstoneRingSize;
}

void TaskDependencyGraph::AssertNoCycleLocked(TaskId taskId, TaskId dependencyId) const {
    // Edges are registered at submit time and only point at already-present
    // (older) nodes, so a cycle is structurally impossible today; this is a
    // tripwire against future API drift. Real dependency chains are 1–2
    // edges deep, so a small visit cap bounds the walk.
    constexpr size_t kMaxVisits = 64;

    Vector<TaskId> worklist{dependencyId};
    std::unordered_set<TaskId> visited;

    while (!worklist.empty() && visited.size() < kMaxVisits) {
        const TaskId current = worklist.back();
        worklist.pop_back();
        if (!visited.insert(current).second) {
            continue;
        }

        if (current == taskId) {
            std::fprintf(stderr,
                         "TaskDependencyGraph: adding dependency %llu -> %llu closes a cycle\n",
                         static_cast<unsigned long long>(taskId),
                         static_cast<unsigned long long>(dependencyId));
            std::fflush(stderr);
            std::abort();
        }

        auto it = m_Tasks.find(current);
        if (it == m_Tasks.end()) {
            continue;
        }
        worklist.insert(worklist.end(), it->second.Dependencies.begin(), it->second.Dependencies.end());
    }
}

#endif // GE_DEBUG_INSTRUMENTATION

} // namespace JobSystem
