#include "Platform/Capabilities.h"

#include "JobSystem/Types.h"

#include <algorithm>
#include <thread>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

namespace GameEngine
{
namespace Platform
{

bool SupportsDynamicNativeModules()
{
#if defined(__EMSCRIPTEN__)
    return false;
#else
    return true;
#endif
}

bool SupportsMultipleWindows()
{
#if defined(__EMSCRIPTEN__)
    return false;
#else
    return true;
#endif
}

std::uint32_t RecommendedWorkerCount()
{
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    // The single-threaded ABI has no threads to give: the JobSystem runs
    // inline, and constructing a worker aborts.
    return 0;
#elif defined(__EMSCRIPTEN__)
    // Workers come from the pre-spawned pthread pool; keep the budget small
    // and leave headroom for the browser's own threads.
    const std::uint32_t cores = std::thread::hardware_concurrency();
    return cores > 2 ? std::min(cores - 2, 4u) : 1u;
#else
    return std::max(1u, std::thread::hardware_concurrency());
#endif
}

std::uint32_t BlockingThreadBudget()
{
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    return 0;
#elif defined(__EMSCRIPTEN__)
    return 1;
#else
    return static_cast<std::uint32_t>(JobSystem::kMaxBlockingThreadBudget);
#endif
}

bool SupportsAuxiliaryThreadPools()
{
#if defined(__EMSCRIPTEN__)
    return false;
#else
    return true;
#endif
}

bool SupportsInboundSockets()
{
#if defined(__EMSCRIPTEN__)
    return false;
#else
    return true;
#endif
}

bool SupportsSynchronousDirectoryWalk()
{
#if defined(__EMSCRIPTEN__)
    return false;
#else
    return true;
#endif
}

bool SupportsTransientThreads()
{
#if defined(__EMSCRIPTEN__)
    return false;
#else
    return true;
#endif
}

bool SupportsProcessCreation()
{
#if defined(__EMSCRIPTEN__)
    return false;
#else
    return true;
#endif
}

bool SupportsExternalFileChanges()
{
#if defined(__EMSCRIPTEN__)
    return false;
#else
    return true;
#endif
}

bool ProjectStorageIsSandboxed()
{
#if defined(__EMSCRIPTEN__)
    return true;
#else
    return false;
#endif
}

bool HostDrivesFrameLoop()
{
#if defined(__EMSCRIPTEN__)
    return true;
#else
    return false;
#endif
}

bool PrimaryShortcutModifierIsCommand()
{
#if defined(__EMSCRIPTEN__)
    // Resolved once and cached: the host OS cannot change under a running
    // page, and this is asked on every shortcut match. navigator.platform is
    // deprecated but universal; userAgentData.platform is its modern,
    // Chromium-only replacement and is preferred when present — and spells
    // its answer lower-case ("macOS"), unlike navigator.platform's "MacIntel",
    // so the match must be case-insensitive or the preferred source silently
    // loses on the one platform this whole check exists to catch. Everything
    // that reports as an Apple platform (macOS, iPad, iPhone — Safari on
    // iPadOS reports "MacIntel") uses Command; every other host uses Control.
    static const bool isCommand = EM_ASM_INT({
        var platform = (navigator.userAgentData && navigator.userAgentData.platform) ||
                       navigator.platform || '';
        return /Mac|iPhone|iPad|iPod/i.test(platform) ? 1 : 0;
    }) != 0;
    return isCommand;
#else
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
#endif
}

bool ClipboardReadCanPromptUser()
{
#if defined(__EMSCRIPTEN__)
    return true;
#else
    return false;
#endif
}

} // namespace Platform
} // namespace GameEngine
