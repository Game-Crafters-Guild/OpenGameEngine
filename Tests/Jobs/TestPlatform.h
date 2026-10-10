#pragma once

// Cross-platform shims for the pure JobSystem suite. The suite was born on
// Windows; its latency diagnostics (timer-resolution logging, ambient-CPU
// sampling) and spin hint are WinAPI. Windows keeps the original behavior;
// other hosts get honest fallbacks so the assertions — which are all
// platform-neutral — run everywhere.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define GE_TEST_HAS_WIN_TIMERS 1
#else
#include <cstdlib>
#include <thread>
#define GE_TEST_HAS_WIN_TIMERS 0
// WinAPI spin hint; a scheduler yield is the closest portable equivalent for
// the test-side busy waits (the pool under test has its own CpuRelax).
#define YieldProcessor() std::this_thread::yield()
#endif

namespace GameEngine::Tests
{
// Death tests: keep an expected abort from raising the Windows error-report
// UI. POSIX aborts are already silent.
inline void DisableAbortDialogs()
{
#if defined(_WIN32)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

// Process-environment write, usable inside death-test bodies (preprocessor
// directives cannot appear inside macro arguments).
inline void SetEnvVar(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
} // namespace GameEngine::Tests
