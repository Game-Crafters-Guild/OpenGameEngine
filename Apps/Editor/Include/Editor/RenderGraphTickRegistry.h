#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace GameEngine {

class UIManager;

namespace Rendering::RenderGraph {
class RGFrame;
}

namespace Editor {

/**
 * Per-frame render-graph participants owned outside EditorApplication.
 *
 * A panel (or any editor-side object) that needs to declare passes into a
 * window's frame registers a callback here instead of being named by the
 * application's frame loop. EditorApplication owns one registry, publishes it
 * on EditorContext, and drains it once per window per frame — it never learns
 * what the participants are.
 *
 * Callbacks run on the frame thread, after the backbuffer import and inside the
 * gate that guarantees the render-service frame spine has stamped this frame.
 * A participant that spans windows is called once per window and decides for
 * itself which window it draws for.
 *
 * Participants outlive the renderer: they hang off panels, and panels are
 * destroyed after the per-window RenderDeviceContext has already deleted
 * RenderServices and the device. Releasing renderer-backed handles is therefore
 * part of the registration contract rather than the registrant's private
 * business — see Register's release callback and ReleaseAll.
 */
class RenderGraphTickRegistry {
public:
    /// 0 is never handed out, so a default-constructed handle is "not registered".
    using Handle = uint64_t;
    static constexpr Handle kInvalidHandle = 0;

    using TickFn = std::function<void(uint64_t windowId, UIManager* ui,
                                      Rendering::RenderGraph::RGFrame& frame)>;
    using ReleaseFn = std::function<void()>;

    /// Adds a participant. The returned handle is the only way to remove it, so
    /// the registrant must keep it for its lifetime.
    ///
    /// @param release Drops every renderer-backed resource the participant owns
    ///        (views, cameras, textures). ReleaseAll runs it while the device
    ///        and RenderServices are still alive; the registrant's own teardown
    ///        runs later and must therefore be idempotent. Null only when the
    ///        participant owns no renderer resource at all.
    Handle Register(TickFn tick, ReleaseFn release);

    /// Removes a participant. Safe to call from inside a tick — the entry is
    /// tombstoned and compacted once the drain unwinds, so the vector never
    /// moves under the iteration.
    void Unregister(Handle handle);

    void TickAll(uint64_t windowId, UIManager* ui, Rendering::RenderGraph::RGFrame& frame);

    /// Releases every participant's renderer resources and empties the registry.
    /// The application calls this once, ahead of renderer teardown; after it no
    /// participant is ticked again.
    void ReleaseAll();

private:
    struct Entry {
        Handle Id = kInvalidHandle;
        TickFn Tick;
        ReleaseFn Release;
    };

    void Compact();

    std::vector<Entry> m_Entries;
    /// Registrations made while a drain is walking m_Entries; merged on unwind.
    std::vector<Entry> m_Pending;
    Handle m_NextHandle = 1;
    bool m_Ticking = false;
    bool m_HasTombstones = false;
};

} // namespace Editor
} // namespace GameEngine
