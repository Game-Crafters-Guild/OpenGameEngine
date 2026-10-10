#include "EditorContextMenu/ContextMenuBackendPolicy.h"

#include <atomic>

namespace GameEngine
{
namespace
{

std::atomic<ContextMenuBackend> s_Backend{DefaultContextMenuBackend()};
std::atomic<unsigned> s_Generation{0};
std::atomic<bool> s_KeepOpenOnToggle{true};

} // namespace

bool NativeContextMenuAvailable()
{
#if defined(_WIN32) || defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

ContextMenuBackend DefaultContextMenuBackend()
{
    // Built-in everywhere: identical menus on every platform, and the macOS
    // NSMenu path can wedge the engine loop in its tracking session (its
    // dismiss handling is an open issue). Native stays selectable in
    // Settings > Interface for users who prefer the OS menu.
    return ContextMenuBackend::BuiltIn;
}

ContextMenuBackend GetContextMenuBackend()
{
    return s_Backend.load(std::memory_order_relaxed);
}

void SetContextMenuBackend(ContextMenuBackend backend)
{
    if (s_Backend.exchange(backend, std::memory_order_relaxed) != backend)
        s_Generation.fetch_add(1, std::memory_order_relaxed);
}

unsigned GetContextMenuBackendGeneration()
{
    return s_Generation.load(std::memory_order_relaxed);
}

bool GetContextMenuKeepOpenOnToggle()
{
    return s_KeepOpenOnToggle.load(std::memory_order_relaxed);
}

void SetContextMenuKeepOpenOnToggle(bool keepOpen)
{
    s_KeepOpenOnToggle.store(keepOpen, std::memory_order_relaxed);
}

} // namespace GameEngine
