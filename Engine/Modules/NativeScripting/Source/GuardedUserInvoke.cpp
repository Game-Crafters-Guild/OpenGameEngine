#ifdef _WIN32

#include "GuardedUserInvoke.h"

#include <windows.h>

namespace GameEngine::NativeScripting
{

// Deliberately tiny and destructor-free: the only statement in the __try is an indirect call, so
// there are no C++ objects to unwind here (avoids C2712 under /EHsc). A hard fault in the thunk
// (access violation, etc.) unwinds to __except; the caller logs and disables the faulting system.
// C++ exceptions are caught inside the thunk's callable before they can reach here, so __except
// only ever fires on a genuine hardware fault.
bool GuardedInvokeRaw(GuardedThunk thunk, void* ctx) noexcept
{
    __try
    {
        thunk(ctx);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

} // namespace GameEngine::NativeScripting

#endif
