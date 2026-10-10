#pragma once

#include "Logger/Types.h"
#include <cstdint>
#include <mutex>

namespace Logger
{

// Process-wide mutex serializing every DbgHelp user (dbghelp.dll is
// process-global and not thread-safe): CaptureBacktrace below, crash-dump
// paths, and the editor's hang watchdog — which acquires it BEFORE suspending
// the main thread so it can never freeze a thread mid-CaptureBacktrace and
// then walk against dbghelp's half-mutated internal state.
std::mutex& DbgHelpMutex();

struct BacktraceFrame
{
    std::uintptr_t Address = 0;
    String Module;
    String Symbol;  // demangled when possible
    String File;    // empty if resolution is unavailable on this platform
    int Line = 0;
};

// Captures a C++ callstack on the calling thread.
// maxFrames: upper bound on number of returned frames.
// skipFrames: number of top frames to drop (use to hide the logger/plumbing frames).
// Thread-safe. Returns empty on unsupported platforms.
Vector<BacktraceFrame> CaptureBacktrace(int maxFrames = 32, int skipFrames = 0);

} // namespace Logger
