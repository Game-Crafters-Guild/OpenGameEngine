#include "Scheduler/Scheduler.h"
#include <cassert>

namespace GameEngine {
namespace Scheduler {

// ---------------- DefaultScheduler ----------------
DefaultScheduler::DefaultScheduler() = default;

TaskId DefaultScheduler::NextId() {
    return m_IdGen.fetch_add(1, std::memory_order_relaxed);
}

TaskToken DefaultScheduler::ScheduleNext(std::function<void()> fn) {
    if (!fn) return {};
    std::lock_guard<std::mutex> lock(m_Mutex);
    TaskId id = NextId();
    TaskEntry e; e.id = id; e.fn = std::move(fn); e.Timed = false; e.Cancelled = false;
    m_Entries.emplace(id, std::move(e));
    m_NextTick.push_back(id);
    return TaskToken{ id };
}

TaskToken DefaultScheduler::ScheduleAfter(Millis delay, std::function<void()> fn) {
    return ScheduleAt(Clock::now() + delay, std::move(fn));
}

TaskToken DefaultScheduler::ScheduleAt(TimePoint when, std::function<void()> fn) {
    if (!fn) return {};
    std::lock_guard<std::mutex> lock(m_Mutex);
    TaskId id = NextId();
    m_Timed.emplace(std::make_pair(when, id), id);
    TaskEntry e; e.id = id; e.due = when; e.fn = std::move(fn); e.Timed = true; e.Cancelled = false;
    m_Entries.emplace(id, std::move(e));
    return TaskToken{ id };
}

bool DefaultScheduler::Cancel(TaskToken token) {
    if (!token.IsValid()) return false;
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Entries.find(token.Id);
    if (it == m_Entries.end()) return false;
    it->second.Cancelled = true;
    if (it->second.Timed) {
        // Erase from timed map (if still present) and remove entry
        m_Timed.erase(std::make_pair(it->second.due, it->second.id));
        m_Entries.erase(it);
    }
    // For next-tick queue we keep a tombstone in entries; it will be skipped on run
    return true;
}

std::size_t DefaultScheduler::ProcessDue(TimePoint now) {
    std::vector<TaskId> localNext;
    std::vector<TaskId> localTimed;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_NextTick.empty()) {
            localNext.swap(m_NextTick);
        }
        // Pop due timed tasks
        auto it = m_Timed.begin();
        while (it != m_Timed.end() && it->first.first <= now) {
            localTimed.push_back(it->second);
            it = m_Timed.erase(it);
        }
    }

    auto execOne = [&](TaskId id) {
        std::function<void()> fn;
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            auto eit = m_Entries.find(id);
            if (eit == m_Entries.end()) return false; // already cancelled and erased
            cancelled = eit->second.Cancelled;
            fn = std::move(eit->second.fn);
            m_Entries.erase(eit);
        }
        if (!cancelled && fn) { fn(); return true; }
        return false;
    };

    std::size_t ran = 0;
    for (TaskId id : localNext) { if (execOne(id)) ++ran; }
    for (TaskId id : localTimed) { if (execOne(id)) ++ran; }
    return ran;
}

std::size_t DefaultScheduler::PendingCount() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_NextTick.size() + m_Timed.size();
}

// ---------------- ChainBuilder ----------------
ChainHandle ChainBuilder::Commit() {
    if (!m_Sch || m_Steps.empty()) return ChainHandle{};

    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    auto currentId = std::make_shared<std::atomic<TaskId>>(0);
    auto steps = std::make_shared<std::vector<Step>>(std::move(m_Steps));

    // Recursive scheduling helper (kept alive via shared_ptr to avoid dangling refs)
    IScheduler* sch = m_Sch;
    auto scheduleIndex = std::make_shared<std::function<void(std::size_t)>>();
    *scheduleIndex = [sch, cancelled, currentId, steps, scheduleIndex](std::size_t i) {
        auto wrapper = [sch, cancelled, currentId, steps, scheduleIndex, i]() {
            if (cancelled->load(std::memory_order_relaxed)) return;
            auto& s = (*steps)[i];
            if (s.fn) s.fn();
            if (cancelled->load(std::memory_order_relaxed)) return;
            if (i + 1 < steps->size()) {
                (*scheduleIndex)(i + 1);
            }
        };

        TaskToken t{};
        auto& step = (*steps)[i];
        switch (step.kind) {
            case Kind::Next: t = sch->ScheduleNext(wrapper); break;
            case Kind::After: t = sch->ScheduleAfter(step.delay, wrapper); break;
            case Kind::At: t = sch->ScheduleAt(step.when, wrapper); break;
        }
        currentId->store(t.Id, std::memory_order_relaxed);
    };

    // Start chain
    (*scheduleIndex)(0);

    return ChainHandle{ cancelled, sch, currentId };
}

} // namespace Scheduler
} // namespace GameEngine

