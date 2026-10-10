#pragma once

#include <functional>
#include <vector>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <map>
#include <chrono>
#include <atomic>
#include <cstdint>

namespace GameEngine {
namespace Scheduler {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Millis = std::chrono::milliseconds;
using Seconds = std::chrono::duration<double>;

using TaskId = std::uint64_t;

struct TaskToken {
    TaskId Id{0};
    bool IsValid() const noexcept { return Id != 0; }
};

    // IScheduler
    // Tick-driven scheduler interface for deferring work to the next tick or a future
    // time point.
    //
    // Semantics:
    // - Single-threaded execution model: work runs when the owner calls ProcessDue().
    // - Thread-safety: ScheduleNext/ScheduleAfter/ScheduleAt/Cancel are safe to call
    //   from any thread. ProcessDue() is expected to be called from the owning thread
    //   and must not be called concurrently.
    // - Re-entrancy: Work scheduled from within ProcessDue() does not run in the same
    //   ProcessDue() call; it is deferred until a subsequent call. This avoids unbounded
    //   recursion and preserves determinism.
    // - Ordering: ScheduleNext tasks execute FIFO. Timed tasks execute by ascending
    //   due-time. Tasks with identical due-times execute in submission order.
    // - Cancellation: Cancel() prevents tasks that have not yet run from executing.
    //   Cancelling the currently running task is a no-op (it will complete). Cancel on
    //   an already-executed or unknown token returns false.
    // - Time base: Uses std::chrono::steady_clock.

class IScheduler {
public:
    virtual ~IScheduler() = default;

    // Schedule to run on the next ProcessDue() call (never in the current call).
    virtual TaskToken ScheduleNext(std::function<void()> fn) = 0;

    // Schedule to run after a relative delay (wall time).
    virtual TaskToken ScheduleAfter(Millis delay, std::function<void()> fn) = 0;

    // Schedule to run at an absolute time point.
    virtual TaskToken ScheduleAt(TimePoint when, std::function<void()> fn) = 0;

    // Cancel a scheduled task if it has not run yet.
    virtual bool Cancel(TaskToken token) = 0;

    // Execute all tasks due as of 'now'. Returns number of tasks run.
    virtual std::size_t ProcessDue(TimePoint now = Clock::now()) = 0;

    // Diagnostic count of pending tasks.
    virtual std::size_t PendingCount() const = 0;
};

// Convenience overload for generic durations
template<class Rep, class Period>
inline TaskToken ScheduleAfter(IScheduler& sch,
                               std::chrono::duration<Rep, Period> d,
                               std::function<void()> fn) {
    return sch.ScheduleAfter(std::chrono::duration_cast<Millis>(d), std::move(fn));
}

// Chain API -------------------------------------------------------------
class ChainHandle {
public:
    ChainHandle() = default;
    explicit ChainHandle(std::shared_ptr<std::atomic<bool>> cancelled,
                         IScheduler* owner,
                         std::shared_ptr<std::atomic<TaskId>> currentId)
        : m_Cancelled(std::move(cancelled)), m_Owner(owner), m_CurrentId(std::move(currentId)) {}

    // ChainHandle
    // Represents a running chain started via ChainBuilder::Commit().
    // - Cancel() prevents further steps from being scheduled and attempts to cancel
    //   the currently scheduled step if it has not started yet.

    bool Cancel() {
        if (m_Cancelled) {
            m_Cancelled->store(true, std::memory_order_relaxed);
        }
        if (m_Owner && m_CurrentId) {
            TaskId id = m_CurrentId->load(std::memory_order_relaxed);
            if (id != 0) {
                return m_Owner->Cancel(TaskToken{id});
            }
        }
        return true;
    }

private:
    std::shared_ptr<std::atomic<bool>> m_Cancelled;
    IScheduler* m_Owner = nullptr;
    std::shared_ptr<std::atomic<TaskId>> m_CurrentId;
};

    // ChainBuilder
    // Fluent chain construction API. Each Then/After/At produces a step that is scheduled
    // when the previous step completes. Steps are scheduled using the underlying scheduler
    // semantics (Next/After/At) and therefore never execute re-entrantly in the same
    // ProcessDue() call.

class ChainBuilder {
public:
    explicit ChainBuilder(IScheduler& sch) : m_Sch(&sch) {}

    // Schedule the next step to run on the next ProcessDue after the previous step completes.
    ChainBuilder& Then(std::function<void()> fn) {
        m_Steps.push_back(Step{Kind::Next, Millis{0}, TimePoint{}, std::move(fn)});
        return *this;
    }

    // Schedule the next step to run after a delay following the previous step.
    ChainBuilder& After(Millis delay, std::function<void()> fn) {
        m_Steps.push_back(Step{Kind::After, delay, TimePoint{}, std::move(fn)});
        return *this;
    }

    // Schedule a step to run at an absolute time (rare for chains, but supported).
    ChainBuilder& At(TimePoint when, std::function<void()> fn) {
        m_Steps.push_back(Step{Kind::At, Millis{0}, when, std::move(fn)});
        return *this;
    }

    // Commit schedules the first step now (as Next) and wires continuations for the rest.
    ChainHandle Commit();

private:
    enum class Kind { Next, After, At };
    struct Step {
        Kind kind;
        Millis delay;
        TimePoint when;
        std::function<void()> fn;
    };

    IScheduler* m_Sch;
    std::vector<Step> m_Steps;
};

// Default implementation ------------------------------------------------
class DefaultScheduler final : public IScheduler {
public:
    DefaultScheduler();
    ~DefaultScheduler() override = default;

    TaskToken ScheduleNext(std::function<void()> fn) override;
    TaskToken ScheduleAfter(Millis delay, std::function<void()> fn) override;
// DefaultScheduler
// Reference implementation of IScheduler.
// - Thread-safe scheduling/cancel; single-threaded execution via ProcessDue().
// - Next-tick tasks kept in FIFO queue, timed tasks stored in std::map keyed by
//   (due-time, submission-id) ensuring deterministic ordering for equal times.
// - Cancel() tombstones next-tick entries and erases timed entries.
// - PendingCount() is an approximation (excludes tombstoned next-tick entries).

    TaskToken ScheduleAt(TimePoint when, std::function<void()> fn) override;
    bool Cancel(TaskToken token) override;
    std::size_t ProcessDue(TimePoint now = Clock::now()) override;
    std::size_t PendingCount() const override;

private:
    struct TaskEntry {
        TaskId id{0};
        TimePoint due{};
        std::function<void()> fn;
        bool Timed{false};
        bool Cancelled{false};
    };

    TaskId NextId();

    mutable std::mutex m_Mutex;
    std::unordered_map<TaskId, TaskEntry> m_Entries;
    std::vector<TaskId> m_NextTick;
    // Keyed by (due time, id) to guarantee FIFO among equal due times
    std::map<std::pair<TimePoint, TaskId>, TaskId> m_Timed;
    std::atomic<TaskId> m_IdGen{1};
};

} // namespace Scheduler
} // namespace GameEngine

