#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "UI/ModuleOwnedHandlers.h"
#include "UI/UIElement.h"

namespace GameEngine
{
namespace UI
{

// Where a revocation pass parks the callables it takes off a control's slots.
//
// Destroying a released callable runs the module's code, and the pass that
// released it is walking that control's slots — so the callable must not die
// until the walk is over and the control is no longer being touched. Slots have
// different signatures, so the sink erases them behind a destructor-only handle:
// nothing in here is ever called again, it is only kept alive and then dropped.
using RevokedCallableSink = std::vector<std::shared_ptr<void>>;

// A control callback member that records which hot-swappable image its callable
// came from, so a module unload can revoke it while that image is still mapped.
//
// Why the member's TYPE carries this rather than each setter: a native user
// module's std::function stored in a control member outlives the module's image
// exactly as a handler-table entry does, and the surface is 44 members across 11
// headers. A per-setter hand-off would be 44 forgettable call sites, and every
// new control would have to remember one. Changing the member's type leaves the
// setter bodies (`m_OnFoo = std::move(cb);`) and every read site
// (`if (m_OnFoo) m_OnFoo(id);`) correct unchanged.
//
// The stamp comes from the CALLABLE, never from the caller: target_type()'s
// type_info is emitted by the translation unit that instantiated the erasure, so
// it lives in the image whose code the callable is. Engine.dll and Editor.exe are
// never in the hot-swappable set, so their callables resolve to 0 and are
// structurally unstampable — see ModuleOwnedHandlers.h.
//
// A control declares its own slot bits, lists its slots once, and implements the
// four UIElement revocation virtuals in its .cpp. The release path is therefore a
// virtual defined in the control's own translation unit — never a function
// pointer stored in this object, which would point into whichever image last
// assigned the slot and reintroduce the double-link hazard.
template <typename Sig>
class ModuleOwnedCallback;

template <typename R, typename... Args>
class ModuleOwnedCallback<R(Args...)>
{
public:
    using FunctionType = std::function<R(Args...)>;

    // `owner` is the element this slot is a member of, and `slotBit` its bit in
    // that control's mask. The back-pointer is safe for the slot's whole life:
    // UIElement is non-copyable and non-movable, so the element can never
    // relocate out from under it.
    ModuleOwnedCallback(UIElement* owner, std::uint32_t slotBit)
        : m_Owner(owner), m_SlotBit(slotBit)
    {
    }

    // Neither copyable nor movable: a stamped slot is counted in the owning
    // element's m_OwnedHandlerCount, so a copy would double-count it and a move
    // would leave the count attributed to a slot that no longer holds the
    // callable. The element itself is non-copyable and non-movable too, so
    // nothing legitimate needs either.
    ModuleOwnedCallback(const ModuleOwnedCallback&) = delete;
    ModuleOwnedCallback& operator=(const ModuleOwnedCallback&) = delete;
    ModuleOwnedCallback(ModuleOwnedCallback&&) = delete;
    ModuleOwnedCallback& operator=(ModuleOwnedCallback&&) = delete;

    // No destructor work — and NOT because of destruction order, which runs the
    // other way: a control's slots are members of the derived class, so they are
    // destroyed BEFORE ~UIElement's body runs, not after.
    //
    // The reason is that the count and the index are per-ELEMENT, not per-slot.
    // ~UIElement clears m_OwnedHandlerCount and leaves the index in one step
    // whatever the slots did, so a per-slot decrement here would be redundant
    // work that the element-level sweep still has to do anyway — a branch per
    // slot on every teardown, fifteen of them per TreeView, buying nothing.
    ~ModuleOwnedCallback() = default;

    // Replaces whatever the slot held, restamping from the new callable.
    //
    // The old callable leaves in the parameter and dies with it, after every write
    // to this slot and to the owning element. Destroying it runs the module's
    // code, which can clear this slot or destroy the control; an assignment would
    // run that code with the stamp cleared and the new one not yet taken, and then
    // write m_OwnerCodeAddr and dereference m_Owner on top of whatever it did.
    //
    // Not airtight, and the gap is MSVC's: a callable held INLINE (sizeof <= 56
    // and nothrow-move-constructible) whose captures do not null on move has its
    // moved-from copy destroyed IN PLACE by the swap below, so that shape still
    // runs module code before the writes. Closing that class needs the callable's
    // storage to outlive its destruction — see the release rule on
    // UIElement::UnregisterEventHandler.
    ModuleOwnedCallback& operator=(FunctionType fn)
    {
        const std::uint64_t owner = fn ? UI::AttributeCallableOwner(fn.target_type()) : 0;
        m_Owner->ClearMemberSlotStamp(m_OwnerCodeAddr);
        fn.swap(m_Fn);
        m_OwnerCodeAddr = owner;
        m_Owner->NoteMemberSlotStamped(m_OwnerCodeAddr);
        return *this;
    }

    explicit operator bool() const { return static_cast<bool>(m_Fn); }

    // Invoked under the same executing-frame rule the handler table follows, so
    // revocation cannot free this callable while it is running: an executing slot
    // keeps its callable AND its stamp, so the quiesce ledger counts it, refuses
    // the unmap, and the frame returns into a still-mapped image.
    //
    // CONTRACT: a slot callback must not synchronously destroy the control that
    // owns the slot. Closing the scope touches *m_Owner after the callable
    // returns, so a self-destroying callback is a use-after-free. This is the
    // same rule DispatchEvent already imposes on every handler-table callable,
    // and what the editor's own callbacks already obey — the destructive ones
    // defer (SettingsPanel::RebuildLayout goes through PostAction) rather than
    // tearing down their control from inside its own callback.
    //
    // THE SCOPE CLOSE DOES MORE THAN RESTORE A POINTER, so the contract is wider
    // than "do not delete the control here". If this is the LAST executing frame
    // on the element, closing it runs UIElement::DrainInactiveHandlers, which
    // erases every entry deactivated during the call and destroys their callables
    // — module code, arbitrary user destructors — before this operator() returns.
    // A callback that unsubscribes anything on this element therefore hands
    // control back to module destructors at scope close, and those must not
    // destroy the control either. Same answer as above: defer.
    R operator()(Args... args) const
    {
        UIElement::ExecutingHandlerScope executing(*m_Owner,
                                                   UIElement::MemberSlotKey(m_SlotBit));
        return m_Fn(std::forward<Args>(args)...);
    }

    // ---- Revocation support, used by the owning control's four virtuals ----

    // Whether the slot's callable was attributed to a hot-swappable image. A
    // revoked slot and a never-stamped one are indistinguishable from outside,
    // which is why the test census reads this directly.
    bool IsStamped() const { return m_OwnerCodeAddr != 0; }

    bool OwnedByImage(std::uint64_t base, std::uint64_t size) const
    {
        return m_OwnerCodeAddr != 0 && m_OwnerCodeAddr >= base && m_OwnerCodeAddr < base + size;
    }

    bool IsExecuting() const
    {
        return m_Owner->IsHandlerExecuting(UIElement::MemberSlotKey(m_SlotBit));
    }

    std::uint32_t SlotBit() const { return m_SlotBit; }

    // Releases the callable and its stamp; true if a stamped callable was
    // actually released. An unstamped slot keeps its callable — an engine-owned
    // callback is not the module's to revoke.
    //
    // The callable goes to the caller's sink rather than dying here. Its captures'
    // destructors are module code, and that code can re-enter and clear another
    // slot — or destroy this whole control — while the caller is still walking the
    // slots. The sink outlives the walk, so the walk is over before any of it
    // runs. Callers must still re-check each slot: the sink defers the code, it
    // does not prove none ran.
    //
    // The MSVC gap on operator= applies here too: an inline-held callable whose
    // captures do not null on move is destroyed in place by the move into the
    // sink.
    bool Release(RevokedCallableSink& sink)
    {
        if (m_OwnerCodeAddr == 0)
            return false;
        m_Owner->ClearMemberSlotStamp(m_OwnerCodeAddr);
        // Swapped into the sink's own storage rather than moved and then cleared:
        // swap leaves the slot empty by contract, and nothing writes to the slot
        // afterwards.
        auto taken = std::make_shared<FunctionType>();
        taken->swap(m_Fn);
        sink.push_back(std::move(taken));
        return true;
    }

    // Drops a provisional stamp that resolved outside every mapped image, the way
    // CloseImageAttribution does for handler-table entries. Keeps the callable:
    // only the attribution was wrong.
    bool DropStampIfUnmapped()
    {
        if (m_OwnerCodeAddr == 0 || UI::AddressIsInsideMappedImage(m_OwnerCodeAddr))
            return false;
        m_Owner->ClearMemberSlotStamp(m_OwnerCodeAddr);
        return true;
    }

private:
    FunctionType m_Fn;
    std::uint64_t m_OwnerCodeAddr = 0;
    UIElement* m_Owner = nullptr;
    std::uint32_t m_SlotBit = 0;
};

} // namespace UI
} // namespace GameEngine
