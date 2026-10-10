#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>

namespace GameEngine::UI::Detail
{

// The dispatch-window copy behind string value-event payloads.
//
// THE INVARIANT THIS CLASS OWNS: UIEvent::Text stays valid and byte-identical for the whole
// dispatch it was built for, whatever any handler does — including writing the very field
// being announced. A nested write is legal (Field<T>::NotifyValue downgrades it to
// without-notify semantics) and it reallocates the field's own buffer, so an event pointing
// at the field's string would hand later subscribers a dangling view. Copying at the dispatch
// site is what makes the managed ref-struct view's guarantee total instead of conditional:
// subscribers keep the payload they were given, while the field holds the adjusted value.
//
// A per-thread STACK of reusable buffers, not one buffer: a handler for one field's event may
// write ANOTHER string field, whose notification dispatches while this window is still open.
// Each nesting level owns its own buffer, and the destructor pops on scope exit — by unwind
// too, so a throwing handler cannot leak a level. Buffers keep their capacity across reuse,
// so the steady state is a memcpy and no allocation.
//
// SCOPE OF THE WINDOW: value events are dispatched on the control itself and do not bubble
// (UIEvents.h, kEventValueChanged), so one DispatchEvent call is the whole propagation
// window. If a string-carrying event ever bubbles, the window must be pushed once for the
// whole routing walk, not per element — a per-element window would free the bytes between
// ancestors.
//
// std::deque, NOT std::vector: a nested window may grow the container while an outer
// window's view is live, and vector growth MOVES the outer std::string — with SSO that
// relocates the very bytes the outer view points at. deque never moves existing elements
// when it grows at the back.
//
// Fully header-inline on purpose: the caller is Field<T>::NotifyValue, itself header-inline,
// and keeping the copy at the instantiation site lets an allocation probe in a test binary
// see it (UIDispatchAllocationTests' probe is blind to code compiled into Engine.dll).
class ScopedValueText
{
public:
    explicit ScopedValueText(std::string_view value)
    {
        Stack& s = TlsStack();
        if (s.Depth == s.Buffers.size())
            s.Buffers.emplace_back();
        std::string& buf = s.Buffers[s.Depth];
        // Claim the level only once the copy has succeeded: a constructor that throws gets no
        // destructor, so incrementing first would strand the level for the thread's lifetime.
        buf.assign(value);
        ++s.Depth;
        m_View = buf;
    }

    ~ScopedValueText() { --TlsStack().Depth; }

    ScopedValueText(const ScopedValueText&) = delete;
    ScopedValueText& operator=(const ScopedValueText&) = delete;

    // The dispatch-stable bytes. Valid until this scope closes; never longer.
    std::string_view View() const { return m_View; }

private:
    struct Stack
    {
        std::deque<std::string> Buffers;
        std::size_t Depth = 0;
    };

    // Function-local so the header needs no .cpp. One instance per thread PER MODULE
    // (header-inline across DLLs): harmless, because a window's push and pop compile
    // together at one call site and always hit the same instance, and nesting across
    // modules just uses one level of each stack.
    static Stack& TlsStack()
    {
        static thread_local Stack s;
        return s;
    }

    std::string_view m_View;
};

} // namespace GameEngine::UI::Detail
