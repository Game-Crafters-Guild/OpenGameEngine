#pragma once

#include <functional>
#include <vector>
#include <mutex>
#include <memory>
#include <thread>
#include <cassert>

namespace GameEngine {
namespace UI {

// IUiDispatcher
// - Post(fn): enqueue work to run later (typically next frame or right after event dispatch)
// - Drain(): execute all currently pending work now; work posted during Drain is deferred
//            to a subsequent Drain (no re-entrancy into the same Drain pass)
// - PendingCount(): diagnostic count of enqueued actions
class IUiDispatcher {
public:
    virtual ~IUiDispatcher() = default;

    // Enqueue work to be executed on the UI thread/scheduler at the next Drain.
    // Returns false when the dispatcher no longer accepts work; `fn` is then dropped, and
    // destroyed on the calling thread.
    virtual bool Post(std::function<void()> fn) = 0;

    // Execute all currently enqueued work. Call from the UI thread.
    // Re-entrant posts inside handlers are processed on a subsequent Drain.
    virtual void Drain() = 0;

    // Number of enqueued actions (diagnostics only).
    virtual size_t PendingCount() const = 0;
};

// Default implementation: thread-safe queue, drained on the UI thread.
//
// Post is callable from any thread; Drain and Close from the UI thread only. The owning
// UIManager shares this object with its elements' post routes (UI::UiPostTarget), so it can
// outlive the manager: the manager closes it as it dies, and every later Post is refused.
class UiDispatcher final : public IUiDispatcher {
public:
    bool Post(std::function<void()> fn) override {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Closed)
            return false;
        m_Queue.emplace_back(std::move(fn));
        return true;
    }
    // Refuse every later Post. Work already queued still runs at the next Drain.
    void Close() {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Closed = true;
    }
    void Drain() override {
        // Debug-only: capture the owner thread on first Drain and enforce thereafter
        #ifdef _DEBUG
        if (!m_OwnerCaptured) {
            m_OwnerThread = std::this_thread::get_id();
            m_OwnerCaptured = true;
        } else {
            assert(std::this_thread::get_id() == m_OwnerThread && "UiDispatcher::Drain must be called from the same thread that first drained (UI thread)");
        }
        #endif

        // Enforce the documented no-reentry contract. Drain is UI-thread-only, so a simple
        // bool guard suffices. A nested Drain (e.g. a posted lambda re-enters UIManager
        // which calls Drain again) becomes a no-op; any work posted during this pass is
        // picked up by the next Drain, which is exactly what the UIManager::Update
        // convergence loop expects.
        if (m_Draining)
            return;
        m_Draining = true;

        // Swap the queue into a persistent drain buffer so both m_Queue and m_DrainBuf
        // retain their capacity across calls (avoids per-drain heap allocation).
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_DrainBuf.swap(m_Queue);
        }
        for (auto& fn : m_DrainBuf) {
            if (fn) fn();
        }
        m_DrainBuf.clear();

        m_Draining = false;
    }
    size_t PendingCount() const override {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Queue.size();
    }
private:
    mutable std::mutex m_Mutex;
    std::vector<std::function<void()>> m_Queue; // guarded by m_Mutex
    bool m_Closed = false;                      // guarded by m_Mutex
    std::vector<std::function<void()>> m_DrainBuf; // persistent buffer for Drain(); UI thread only
    bool m_Draining = false; // re-entry guard (UI thread only)

    // Debug-only owner thread enforcement for Drain
    #ifdef _DEBUG
    std::thread::id m_OwnerThread{};
    bool m_OwnerCaptured = false;
    #endif
};



} // namespace UI
} // namespace GameEngine

