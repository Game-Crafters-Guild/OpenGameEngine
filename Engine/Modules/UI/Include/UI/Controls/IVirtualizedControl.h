#pragma once

#include <cstdint>

namespace GameEngine
{
class UIManager;

// A virtualized data view (ListView / GridView / TreeView) that participates in
// the UIManager per-frame provider change pump (C-8).
//
// MarkChanged only bumps a provider-internal counter; nothing pulls the
// changeset until a scroll/viewport event drives UpdateVirtualization. That
// leaves an idle-gated panel repainting nothing on a pure data mutation. The
// pump closes the gap: while a control is attached it registers itself, and once
// per frame UIManager compares ProviderChangeVersion() against the last value it
// saw. On a bump it calls EnqueuePumpWork, which schedules the SAME coordinator
// work item the control's scroll path uses (Reason::DataChanged). The enqueued
// work makes VirtualizationCoordinator::HasPending() true, which the idle/pointer
// gates already consult — so the mutation is serviced the same frame without the
// view raising any dirty mark.
class IVirtualizedControl
{
  public:
    virtual ~IVirtualizedControl() = default;

    // Current provider change-tracking version, or 0 when no provider is bound.
    // Must be cheap (a single counter read, no allocation): it runs once per
    // registered control every frame, including fully idle frames.
    //
    // Providers are borrowed, never owned: the implementation must return 0
    // (not dereference a cached pointer) after the provider is detached. The
    // pump runs between frames, so a provider destroyed mid-frame is only safe
    // if the control clears its reference in the same mutation.
    virtual std::uint64_t ProviderChangeVersion() const = 0;

    // Enqueue this control's data-changed virtualization work on the manager.
    virtual void EnqueuePumpWork(UIManager& ui) = 0;
};

} // namespace GameEngine
