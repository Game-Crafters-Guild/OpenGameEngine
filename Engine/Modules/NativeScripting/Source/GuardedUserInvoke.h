#pragma once

// Per-user-system crash isolation. Wraps a single hot-reloaded user-system invocation so a hard
// fault (access violation, etc.) in user C++ does not take down the host. Windows uses SEH
// (__try/__except under the default /EHsc); other platforms pass through (a fault dies via the
// process signal handlers). Returns true if the call completed, false if it hard-faulted.
//
// Only USER systems are routed through this — engine systems are ticked by the engine scheduler
// and never touch it, so the engine hot path is unaffected.

#include <memory>
#include <type_traits>
#include <utility>

namespace GameEngine::NativeScripting
{

#ifdef _WIN32

// The __try scope must be free of C++ objects needing unwinding (C2712 under /EHsc), so the guard
// is a tiny .cpp function whose only statement is an indirect call through this thunk. The actual
// callable (with its destructors / try-catch) lives in the caller's frame, outside any __try.
using GuardedThunk = void (*)(void* ctx);
bool GuardedInvokeRaw(GuardedThunk thunk, void* ctx) noexcept;

// Erase any callable to (thunk, ctx) and run it guarded. The closure stays in THIS frame; only its
// address crosses into GuardedInvokeRaw.
template <class Fn>
bool GuardedUserInvoke(Fn&& fn) noexcept
{
    using Callable = std::remove_reference_t<Fn>;
    GuardedThunk thunk = [](void* ctx) { (*static_cast<Callable*>(ctx))(); };
    return GuardedInvokeRaw(thunk, std::addressof(fn));
}

#else

// No SEH off Windows: run directly. A hard fault dies cleanly via the process signal handlers.
template <class Fn>
bool GuardedUserInvoke(Fn&& fn) noexcept
{
    std::forward<Fn>(fn)();
    return true;
}

#endif

} // namespace GameEngine::NativeScripting
