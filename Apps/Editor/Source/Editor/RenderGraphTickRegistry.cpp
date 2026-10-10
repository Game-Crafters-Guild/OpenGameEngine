#include "Editor/RenderGraphTickRegistry.h"

#include <algorithm>
#include <iterator>

namespace GameEngine::Editor {

RenderGraphTickRegistry::Handle RenderGraphTickRegistry::Register(TickFn tick, ReleaseFn release)
{
    if (!tick)
        return kInvalidHandle;
    const Handle handle = m_NextHandle++;
    // Appending mid-drain would reallocate the vector the drain is walking, and
    // the callback being executed lives in it. Park it until the drain unwinds.
    if (m_Ticking)
        m_Pending.push_back(Entry{handle, std::move(tick), std::move(release)});
    else
        m_Entries.push_back(Entry{handle, std::move(tick), std::move(release)});
    return handle;
}

void RenderGraphTickRegistry::Unregister(Handle handle)
{
    if (handle == kInvalidHandle)
        return;

    // Only the id is cleared. A self-unregistering participant is calling this
    // from inside its own Tick, so assigning to that std::function would destroy
    // the closure whose body is on the stack. Compact() drops the functions once
    // the drain has unwound; until then the cleared id is what marks the entry
    // dead, and every walk skips on it.
    const auto clear = [handle](std::vector<Entry>& entries) {
        for (Entry& entry : entries) {
            if (entry.Id != handle)
                continue;
            entry.Id = kInvalidHandle;
            return true;
        }
        return false;
    };

    if (!clear(m_Entries))
        clear(m_Pending);
    // Either way a tombstone now exists in a vector that Compact walks after the
    // pending merge, so the flag is unconditional.
    m_HasTombstones = true;

    if (!m_Ticking)
        Compact();
}

void RenderGraphTickRegistry::TickAll(uint64_t windowId, UIManager* ui,
                                      Rendering::RenderGraph::RGFrame& frame)
{
    m_Ticking = true;
    // Index-based rather than iterator-based: a tick may unregister itself,
    // which tombstones its entry in place instead of erasing it.
    for (size_t i = 0; i < m_Entries.size(); ++i) {
        if (m_Entries[i].Id == kInvalidHandle)
            continue;
        if (m_Entries[i].Tick)
            m_Entries[i].Tick(windowId, ui, frame);
    }
    m_Ticking = false;

    if (!m_Pending.empty()) {
        m_Entries.insert(m_Entries.end(), std::make_move_iterator(m_Pending.begin()),
                         std::make_move_iterator(m_Pending.end()));
        m_Pending.clear();
    }
    Compact();
}

void RenderGraphTickRegistry::ReleaseAll()
{
    // Drained into locals first: a release callback typically runs the
    // registrant's own teardown, which unregisters. Emptying the members up
    // front makes that Unregister a no-op instead of a mutation of the vector
    // being walked.
    std::vector<Entry> entries = std::move(m_Entries);
    std::vector<Entry> pending = std::move(m_Pending);
    m_Entries.clear();
    m_Pending.clear();
    m_HasTombstones = false;

    // A cleared id is an unregistered participant whose function objects Compact
    // has not dropped yet; it owns nothing to release.
    for (Entry& entry : entries) {
        if (entry.Id != kInvalidHandle && entry.Release)
            entry.Release();
    }
    for (Entry& entry : pending) {
        if (entry.Id != kInvalidHandle && entry.Release)
            entry.Release();
    }
}

void RenderGraphTickRegistry::Compact()
{
    if (!m_HasTombstones)
        return;
    m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
                                   [](const Entry& e) { return e.Id == kInvalidHandle; }),
                    m_Entries.end());
    m_HasTombstones = false;
}

} // namespace GameEngine::Editor
