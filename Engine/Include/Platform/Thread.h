#pragma once

#if defined(__EMSCRIPTEN__)
#  include <emscripten/threading.h>
#elif defined(_WIN32)
#  include <windows.h>
#elif defined(__APPLE__)
#  include <pthread.h>
#elif defined(__linux__)
#  include <pthread.h>
#  include <cstring>
#endif

namespace GameEngine::Platform {

// Whether the calling thread can park while workers complete work. The
// browser main thread must service worker requests; native threads may block.
inline bool CanBlockCurrentThread()
{
#if defined(__EMSCRIPTEN__)
    return !emscripten_is_main_browser_thread();
#else
    return true;
#endif
}

// Set a descriptive name for the calling thread, visible in debuggers and profilers.
// Names should be short ASCII strings (Linux truncates to 15 characters).
inline void SetCurrentThreadName([[maybe_unused]] const char* name)
{
#if defined(_WIN32)
    if (!name) return;
    wchar_t wide[64];
    int i = 0;
    while (name[i] && i < 63)
    {
        wide[i] = static_cast<wchar_t>(name[i]);
        ++i;
    }
    wide[i] = L'\0';
    SetThreadDescription(GetCurrentThread(), wide);
#elif defined(__APPLE__)
    if (name) pthread_setname_np(name);
#elif defined(__linux__)
    if (name)
    {
        char truncated[16];
        std::strncpy(truncated, name, 15);
        truncated[15] = '\0';
        pthread_setname_np(pthread_self(), truncated);
    }
#endif
}

} // namespace GameEngine::Platform
