#pragma once

// Test-only census of UIElement's handler table.
//
// The table is protected and the public API reports only whether a handler fires, so an
// entry that is unregistered but never swept — or one that was unregistered out from under
// a different owner — is invisible from outside. Friended by UIElement (see the
// `friend struct UIEventHandlerAccess` declaration there), and shared by the suites that
// assert on registration bookkeeping rather than on observable firing.

#include "UI/Controls/Button.h"
#include "UI/Controls/TreeView.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <cstddef>
#include <cstdint>

namespace GameEngine
{

struct UIEventHandlerAccess
{
    // Button's stamped member slots. Same invisibility problem as the table: a
    // revoked slot and a never-stamped one look identical from outside.
    static bool OnMouseDownHasCallable(const Button& b) { return static_cast<bool>(b.m_OnMouseDown); }
    static bool OnMouseDownIsStamped(const Button& b) { return b.m_OnMouseDown.IsStamped(); }

    // Two slots on ONE control, for the pins that need a revocation walk to visit a
    // second slot after releasing the first. Button has only one slot; TreeView's
    // fifteen are where that shape now lives.
    static bool SelectionChangedHasCallable(const TreeView& tv)
    {
        return static_cast<bool>(tv.m_OnSelectionChanged);
    }
    static bool ItemActivatedHasCallable(const TreeView& tv)
    {
        return static_cast<bool>(tv.m_OnItemActivated);
    }

    // Drives a NON-void slot directly. TreeView reaches m_OnIconStateCheck only
    // through a virtualized row rebuild, which needs a bound provider and a laid-out
    // tree; the slot's return path is what is under test, not the row machinery.
    static bool InvokeIconStateCheck(TreeView& tv, TreeId id) { return tv.m_OnIconStateCheck(id); }

    static std::size_t HandlerCount(const UIElement& el, EventId id)
    {
        const auto it = el.m_EventHandlers.find(id);
        return it == el.m_EventHandlers.end() ? 0u : it->second.size();
    }

    // Whether the entry still owns a callable. Releasing it is the whole point of
    // unregistering, and it is invisible from the public API: an inactive entry
    // never fires either way.
    static bool HandlerHasCallable(const UIElement& el, EventId id, std::uint64_t key)
    {
        const auto it = el.m_EventHandlers.find(id);
        if (it == el.m_EventHandlers.end())
            return false;
        for (const auto& he : it->second)
            if (he.key == key)
                return static_cast<bool>(he.handler);
        return false;
    }

    // Handler-presence bits. Invisible from outside by design: a set bit and a
    // clear one differ only in whether a dispatch takes the map probe, which no
    // observable behaviour distinguishes — until one is wrongly clear, which is
    // exactly the failure these expose.
    static std::uint32_t HandlerBits(const UIElement& el) { return el.m_HandlerBits; }

    // Drop a bit WITHOUT touching the handlers behind it, to prove the dispatch
    // paths actually consult it. Nothing in production may do this.
    static void ClearHandlerBitForTest(UIElement& el, EventId id)
    {
        el.m_HandlerBits &= ~EventHandlerBit(id);
    }

    // Module-ownership bookkeeping. The stamp is private and invisible from the public
    // API: a revoked entry and a never-stamped one look identical from outside.
    static std::uint32_t OwnedHandlerCount(const UIElement& el) { return el.m_OwnedHandlerCount; }

    static bool IsStamped(const UIElement& el, EventId id, std::uint64_t key)
    {
        const auto it = el.m_EventHandlers.find(id);
        if (it == el.m_EventHandlers.end())
            return false;
        for (const auto& he : it->second)
            if (he.key == key)
                return he.ownerCodeAddr != 0;
        return false;
    }
};

} // namespace GameEngine
