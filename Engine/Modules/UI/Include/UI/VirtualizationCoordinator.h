#pragma once

#include <cstdint>
#include <vector>

namespace GameEngine
{
class UIManager;
class UIElement;

// Small helper to keep UIManager_Update phases readable:
// - Virtualized controls enqueue work items (deduped by ctx+fn)
// - UIManager drains the queue at a safe point (e.g., before Yoga solve)
//
// NOTE: This is only the *scheduling* mechanism; controls keep their own virtualization logic.
class VirtualizationCoordinator final
{
  public:
    enum class Reason : std::uint8_t
    {
        ScrollChanged = 0,
        ViewportChanged = 1,
        DataChanged = 2,
        Prewarm = 3,
    };

    using RunFn = void (*)(UIElement* ctx, UIManager& ui, Reason reason);
    using ResolveFn = UIElement* (*)(void* userCtx, UIManager& ui, std::uint64_t instanceId);

    // Enqueue work for an element instance. Caller provides instanceId so this header does not need
    // the full UIElement definition.
    void Enqueue(std::uint64_t instanceId, RunFn fn, Reason reason)
    {
        if (instanceId == 0 || !fn)
            return;

        // Dedup by (instanceId,fn). Merge reasons conservatively by keeping the "strongest" one.
        // (We currently treat larger enum values as "stronger"/more general.)
        for (auto& it : m_Items)
        {
            if (it.InstanceId == instanceId && it.Fn == fn)
            {
                if ((std::uint8_t)reason > (std::uint8_t)it.Reason)
                    it.Reason = reason;
                return;
            }
        }

        m_Items.push_back(WorkItem{instanceId, fn, reason});
    }

    bool HasPending() const { return !m_Items.empty(); }

    // Drain the current queue once, running each queued item at most once.
    // Returns true if any work ran.
    bool DrainOnce(UIManager& ui, void* resolveCtx, ResolveFn resolve)
    {
        if (m_Items.empty() || !resolve)
            return false;

        // Move out to allow callbacks to enqueue more work.
        m_Draining.swap(m_Items);
        const size_t n = m_Draining.size();
        for (size_t i = 0; i < n; ++i)
        {
            const WorkItem& it = m_Draining[i];
            if (!it.Fn || it.InstanceId == 0)
                continue;

            UIElement* el = resolve(resolveCtx, ui, it.InstanceId);
            if (!el)
                continue; // element no longer live/attached; drop work item silently

            it.Fn(el, ui, it.Reason);
        }
        m_Draining.clear();
        return n > 0;
    }

    void Clear()
    {
        m_Items.clear();
        m_Draining.clear();
    }

  private:
    struct WorkItem
    {
        std::uint64_t InstanceId = 0;
        RunFn Fn = nullptr;
        // Type qualified via the enclosing class: the member name shadows the
        // enum type inside this struct's scope.
        Reason Reason = VirtualizationCoordinator::Reason::ScrollChanged;
    };

    std::vector<WorkItem> m_Items;
    std::vector<WorkItem> m_Draining; // temp buffer to avoid re-entrancy issues
};

} // namespace GameEngine

