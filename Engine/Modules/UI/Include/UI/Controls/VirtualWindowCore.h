#pragma once

#include "UI/VirtualizationCoordinator.h"

#include <cstdint>
#include <span>

namespace GameEngine::UI
{

// P5: the shared index-space engine behind the virtualized views (ListView,
// GridView, TreeView). Owns window math, the ring-slot mapping (previously
// nine hand-rolled mod lambdas across the three views), pool ensure/shrink
// orchestration, changeset application, safety-net validation, and
// aggregated impact reporting. Deliberately knows nothing about UIElement,
// Yoga, or provider id types — the owning view maps ids to slots and
// implements Host over its cell/row pool, which keeps this class fully
// testable headless.
class VirtualWindowCore final
{
  public:
    // Superset of the three views' per-run guard tuples. StructureGeneration
    // carries TreeView's flat-list generation (others bump it on provider
    // swap); any inequality forces a full rebind of the window.
    struct Guard
    {
        int First = -1;         // first visible line (row) index
        int Desired = -1;       // pooled window size in lines (visible + overscan)
        int ItemCount = -1;     // total items (lines * lanes may exceed this at the tail)
        int ContentHPx = -1;
        int ViewportWPx = -1;
        int ContentWPx = -1;
        std::uint64_t StructureGeneration = 0;
        bool operator==(const Guard&) const = default;
    };

    // Mirrors UIManager::VirtualizationImpact bit-for-bit so views can
    // forward the aggregate with a single cast (static_asserted in the .cpp
    // against the real enum via the owning views' translation units).
    enum class Impact : std::uint32_t
    {
        None = 0u,
        Rebind = 1u << 0,
        LayoutRects = 1u << 1,
        Topology = 1u << 2,
    };

    // The view's pool, seen as flat slots (lineSlot * lanes + lane).
    struct Host
    {
        virtual void EnsurePool(int slotCount) = 0; // grow-only
        // Actual flat slot count. The core queries this each Update instead
        // of caching a line count: GridView's lane count (columns) changes
        // independently of the line count, and a cached-lines model either
        // starves the pool on widen (blank cells) or misses stale tail
        // slots on narrow (ghost cells). Querying also keeps shrink honest
        // after a view-side Reset() with a retained pool.
        virtual int SlotCount() const = 0;
        virtual Impact Rebind(int slot, int itemIndex) = 0;
        virtual void UnbindSlot(int slot) = 0;
        virtual bool SlotBinds(int slot, int itemIndex) const = 0; // safety-net probe
        virtual void DestroyTailSlot() = 0;                        // shrink support

      protected:
        ~Host() = default;
    };

    // Normalized provider changeset: the view pulls its TYPED changeset and
    // maps Subset ids to the flat slots currently bound to them; the core
    // never sees id types.
    struct ChangeSetView
    {
        enum class Kind : std::uint8_t
        {
            None,
            Subset,
            All,
        };
        Kind ChangeKind = Kind::None;
        std::span<const int> AffectedSlots{};
    };

    // Pure window-math helpers (unit-tested directly).
    static int FirstFromUniform(float scrollY, float lineH, int lineCount);
    static int FirstFromPrefixSum(std::span<const int> cumulativeYPx, int scrollYPx, int count);

    // One call replacing each view's UpdateVirtualization midsection:
    // guard compare → changeset apply → subset fast-path / safety-net
    // validate / ring shift / full rebind → shrink hysteresis. Returns the
    // aggregated impact, which the VIEW forwards once to
    // UIManager::NotifyVirtualizationImpact (TreeView's aggregate-per-run
    // model — per-bind notifies armed late whole-tree relayouts).
    //
    // allowShrink: pass !UIElement::IsInEventDispatch() — UpdateVirtualization
    // runs from scroll callbacks mid-dispatch, and DestroyTailSlot destroys
    // live elements, which is unsafe there (this core is UI-agnostic, so the
    // dispatch state is injected). The stability counter still advances on
    // blocked updates; only the destruction is deferred.
    Impact Update(const Guard& next, VirtualizationCoordinator::Reason reason,
                  int lanes, const ChangeSetView& changes, Host& host,
                  bool allowShrink = true);

    // The single ring mapping: line slot for a window-local line index.
    int SlotFor(int localLine) const;

    int RingOffset() const { return m_RingOffset; }
    const Guard& LastGuard() const { return m_Last; }

    // Drop all window/ring/pool bookkeeping (view rebuilt its pool out of
    // band, e.g. a full provider reset that destroys cells).
    void Reset();

    // Shrink hysteresis (ListView's policy, now shared): the pool may keep
    // this many lines beyond Desired before shrinking, and only after this
    // many consecutive stable updates (guard fully equal, no changes).
    static constexpr int kShrinkSlackLines = 8;
    static constexpr int kShrinkStableUpdates = 45;

  private:
    Impact FullRebind(const Guard& g, int lanes, Host& host);

    Guard m_Last{};
    int m_RingOffset = 0;
    int m_StableUpdates = 0;
};

constexpr VirtualWindowCore::Impact operator|(VirtualWindowCore::Impact a, VirtualWindowCore::Impact b)
{
    return static_cast<VirtualWindowCore::Impact>(static_cast<std::uint32_t>(a) |
                                                  static_cast<std::uint32_t>(b));
}
constexpr VirtualWindowCore::Impact operator&(VirtualWindowCore::Impact a, VirtualWindowCore::Impact b)
{
    return static_cast<VirtualWindowCore::Impact>(static_cast<std::uint32_t>(a) &
                                                  static_cast<std::uint32_t>(b));
}
constexpr VirtualWindowCore::Impact& operator|=(VirtualWindowCore::Impact& a, VirtualWindowCore::Impact b)
{
    a = a | b;
    return a;
}
constexpr bool Any(VirtualWindowCore::Impact v)
{
    return v != VirtualWindowCore::Impact::None;
}

} // namespace GameEngine::UI
