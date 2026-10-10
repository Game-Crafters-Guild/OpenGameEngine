#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "Logger/Logger.h"
#include "Logger/LogSink.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#elif defined(__linux__)
#include <sys/uio.h>
#endif
#endif

namespace Logger
{

namespace
{
// Set once at startup by RedirectToSharedState; nullptr = this module owns its
// own state (the standalone / single-module case).
Log::LoggerState* s_RedirectedState = nullptr;

// Upper bound on how long dispatched-but-unflushed output may sit in sink
// buffers (FileSink defaults to autoFlush=false, so a burst's trailing lines —
// e.g. a build-completion message — would otherwise stay invisible until later
// traffic happens to fill the stream buffer). The drain thread only arms this
// timeout while sinks are dirty; a flushed, idle logger blocks indefinitely.
constexpr std::chrono::milliseconds kMaxUnflushedSinkLatency{200};

// Poll interval for the deadline-bounded lock acquisition below.
constexpr std::chrono::milliseconds kCrashLockRetryInterval{1};

// std::mutex has no try_lock_until, and a blocking lock() is exactly what a
// death path cannot afford: the thread that holds it may be the one that died.
// Poll to the deadline instead. On success the caller owns the mutex and must
// adopt it.
bool TryLockUntil(std::mutex& mutex, std::chrono::steady_clock::time_point deadline)
{
    for (;;)
    {
        if (mutex.try_lock())
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(kCrashLockRetryInterval);
    }
}
} // namespace

Log::LoggerState& Log::GetState()
{
    if (s_RedirectedState)
        return *s_RedirectedState;
    static LoggerState state;
    return state;
}

Log::LoggerState* Log::GetStateForSharing()
{
    return &GetState();
}

void Log::RedirectToSharedState(LoggerState* state)
{
    s_RedirectedState = state;
}

std::string& Log::GetFormatBuffer()
{
    thread_local std::string buf;
    return buf;
}

void Log::Initialize(const Config& config)
{
    auto& state = GetState();

    // Configure sinks under Mutex (released before touching QueueMutex to
    // preserve lock ordering: QueueMutex -> Mutex, never the reverse).
    {
        std::lock_guard<std::mutex> lock(state.Mutex);
        state.m_Config = config;

        if (state.Sinks.empty())
        {
            ConsoleSink::Config consoleConfig;
            consoleConfig.MinLevel = config.GlobalMinLevel;
            consoleConfig.IncludeSource = config.EnableSourceLocation;
            consoleConfig.UseStderr = config.ConsoleUseStderr;
            state.Sinks.push_back(MakeUnique<ConsoleSink>(consoleConfig));
        }

        state.m_EffectiveMinLevel.store(state.m_Config.GlobalMinLevel, std::memory_order_relaxed);
    }

    // Clear stale async state from previous session (handles re-init after Shutdown)
    {
        std::lock_guard<std::mutex> qLock(state.QueueMutex);
        state.Queue.clear();
        for (auto& p : state.FlushPromises)
        {
            p.set_value();
        }
        state.FlushPromises.clear();
    }

    // Start drain thread. Threadless wasm cannot: std::thread construction
    // throws, and under -fno-exceptions the throw is an abort the catch below
    // never sees — so don't attempt the spawn. DrainRunning stays false, the
    // state the catch establishes, and every log call routes synchronously.
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    state.Initialized.store(true, std::memory_order_release);
#else
    if (!state.DrainRunning.load(std::memory_order_relaxed))
    {
        state.DrainRunning.store(true, std::memory_order_release);
        state.Initialized.store(true, std::memory_order_release);
        try
        {
            state.DrainThread = std::thread(&Log::DrainThreadFunc);
            // DrainThreadId is set by DrainThreadFunc itself as its first action,
            // eliminating the race where the thread starts before the ID is stored.
        }
        catch (const std::exception&)
        {
            state.DrainRunning.store(false, std::memory_order_relaxed);
            state.Initialized.store(true, std::memory_order_release);
        }
    }
    else
    {
        state.Initialized.store(true, std::memory_order_release);
    }
#endif
}

void Log::Shutdown()
{
    auto& state = GetState();

    bool wasDraining = false;
    {
        std::lock_guard<std::mutex> qLock(state.QueueMutex);
        wasDraining = state.DrainRunning.exchange(false, std::memory_order_release);
    }
    if (wasDraining)
    {
        state.QueueCV.notify_one();
    }

    if (state.DrainThread.joinable())
    {
        state.DrainThread.join();
    }
    // The drain thread is gone; clear its id so a later thread that recycles
    // the OS thread id doesn't take the drain-thread re-entrancy paths.
    state.DrainThreadId = {};

    // Dispatch any remaining owned records synchronously
    {
        std::lock_guard<std::mutex> qLock(state.QueueMutex);
        if (!state.Queue.empty())
        {
            std::lock_guard<std::mutex> sLock(state.Mutex);
            for (auto& entry : state.Queue)
            {
                const LogMessage& msg = entry;
                for (auto& sink : state.Sinks)
                {
                    if (sink && sink->IsAvailable() && sink->ShouldLog(msg.Level))
                    {
#if defined(_CPPUNWIND) || defined(__EXCEPTIONS)
                        try { sink->Write(msg); } catch (...) {}
#else
                        sink->Write(msg);
#endif
                    }
                }
            }
            state.Queue.clear();
        }

        for (auto& p : state.FlushPromises)
        {
            p.set_value();
        }
        state.FlushPromises.clear();
    }

    // Flush and clear sinks
    {
        std::lock_guard<std::mutex> sLock(state.Mutex);
        for (auto& sink : state.Sinks)
        {
            if (sink)
            {
#if defined(_CPPUNWIND) || defined(__EXCEPTIONS)
                try { sink->Flush(); } catch (...) {}
#else
                sink->Flush();
#endif
            }
        }
        state.Sinks.clear();
    }

    state.Initialized.store(false, std::memory_order_release);
    state.m_EffectiveMinLevel.store(LogLevel::Off, std::memory_order_relaxed);
}

void Log::AddSink(UniquePtr<LogSink> sink)
{
    if (!sink)
        return;

    auto& state = GetState();
    std::lock_guard<std::mutex> lock(state.Mutex);

    state.Sinks.push_back(std::move(sink));

    LogLevel effective = state.m_Config.GlobalMinLevel;
    state.m_EffectiveMinLevel.store(effective, std::memory_order_relaxed);
}

void Log::ClearSinks()
{
    auto& state = GetState();
    std::lock_guard<std::mutex> lock(state.Mutex);

    for (auto& sink : state.Sinks)
    {
        if (sink)
        {
#if defined(_CPPUNWIND) || defined(__EXCEPTIONS)
            try { sink->Flush(); } catch (...) {}
#else
            sink->Flush();
#endif
        }
    }

    state.Sinks.clear();
    state.m_EffectiveMinLevel.store(LogLevel::Off, std::memory_order_relaxed);
}

void Log::SetLogLevel(LogLevel level)
{
    auto& state = GetState();
    std::lock_guard<std::mutex> lock(state.Mutex);

    state.m_Config.GlobalMinLevel = level;
    state.m_EffectiveMinLevel.store(level, std::memory_order_relaxed);
}

LogLevel Log::GetLogLevel()
{
    return GetState().m_EffectiveMinLevel.load(std::memory_order_relaxed);
}

// ========================================
// OWNED RECORD CAPTURE
// ========================================

LogMessage Log::MakeOwnedRecord(LogLevel level, const char* file, int line, const char* function)
{
    LogMessage record;
    record.Level = level;
    record.Timestamp = std::chrono::system_clock::now();
    record.SetThreadId(GetCachedThreadId());
    if (file || function)
        record.SetSourceLocation(file, line, function);
    MaybeAttachBacktrace(record);
    return record;
}

namespace
{
// A bad format string or a throwing formatter must still produce a record: the
// failure text replaces the message rather than escaping to the caller.
void FormatRecordMessage(LogMessage& record, std::string_view format, std::format_args args)
{
#if defined(_CPPUNWIND) || defined(__EXCEPTIONS)
    try { record.Message = std::vformat(format, args); }
    catch (const std::exception& e) { record.Message = std::string("[FORMAT ERROR: ") + e.what() + "]"; }
    catch (...) { record.Message = "[FORMAT ERROR]"; }
#else
    record.Message = std::vformat(format, args);
#endif
}
} // namespace

void Log::EnqueueFormatted(LogLevel level, std::string_view format, std::format_args args,
                          const char* file, int line, const char* function)
{
    LogMessage record = MakeOwnedRecord(level, file, line, function);
    FormatRecordMessage(record, format, args);
    PushEntry(std::move(record));
}

// Maximum stack frames captured for Error/Critical logs. Trade-off:
//   - Capture cost is per-frame Sym{From,LineFromAddr64} PDB lookups on the
//     caller's thread (~us each on Windows / dbghelp).
//   - Memory cost is ~300-1000 B per frame in the LogMessage backtrace
//     shared_ptr, held by LogView for its retention window.
// 12 frames is enough to identify the root cause in nearly all real bugs
// (caller -> 2-3 framework layers -> main loop is fully visible) while
// halving memory + capture cost vs the original 24.
static constexpr int kBacktraceMaxFrames = 12;

/* Armed by crash/terminate handlers. backtrace_symbols walks dyld's image
   tables, and during process teardown or from a terminate handler those
   tables can be mid-destruction — symbolizing there converts the report of
   a crash into a second crash (observed: SIGILL in dyld findClosestSymbol
   under Logger::CaptureBacktrace from TerminateHandler). */
static std::atomic<bool> s_SuppressBacktraces{false};

void Log::SuppressBacktraceCapture()
{
    s_SuppressBacktraces.store(true, std::memory_order_relaxed);
}

void Log::MaybeAttachBacktrace(LogMessage& msg)
{
    if (msg.Level < LogLevel::Error)
        return;
    if (s_SuppressBacktraces.load(std::memory_order_relaxed))
        return;
    auto frames = MakeShared<const Vector<BacktraceFrame>>(CaptureBacktrace(kBacktraceMaxFrames, 2));
    if (!frames->empty())
        msg.Backtrace = std::move(frames);
}

void Log::EnqueueString(LogLevel level, std::string_view message,
                        const char* file, int line, const char* function)
{
    LogMessage record = MakeOwnedRecord(level, file, line, function);
    record.Message.assign(message.data(), message.size());
    PushEntry(std::move(record));
}

void Log::EnqueueFormattedWithSource(LogLevel level, std::string_view format,
                                      std::format_args args, std::string_view file,
                                      int line, std::string_view function)
{
    LogMessage record = MakeOwnedRecord(level, nullptr, 0, nullptr);
    record.SourceFile = file;
    record.SourceLine = line;
    record.Function = function;
    FormatRecordMessage(record, format, args);
    PushEntry(std::move(record));
}

void Log::EnqueueStringWithSource(LogLevel level, std::string_view message,
                                 std::string_view file, int line, std::string_view function)
{
    LogMessage record = MakeOwnedRecord(level, nullptr, 0, nullptr);
    record.SourceFile = file;
    record.SourceLine = line;
    record.Function = function;
    record.Message.assign(message.data(), message.size());
    PushEntry(std::move(record));
}

void Log::CriticalImpl(std::string_view message)
{
    auto& state = GetState();

    if (!state.Initialized.load(std::memory_order_acquire))
    {
        std::cout << "[CRITICAL] " << message << '\n';
        return;
    }

    LogMessage msg(LogLevel::Critical, String(message.data(), message.size()));
    msg.SetThreadId(GetCachedThreadId());
    MaybeAttachBacktrace(msg);

    // Re-entrancy guard: drain thread already holds state.Mutex during dispatch.
    // Acquiring it again would deadlock (non-recursive mutex).
    if (std::this_thread::get_id() == state.DrainThreadId)
    {
        DispatchToSinks(msg);
        return;
    }

    std::lock_guard<std::mutex> lock(state.Mutex);
    DispatchToSinks(msg);
}

void Log::CriticalImplWithSource(std::string_view message,
                                  std::string_view file, int line, std::string_view function)
{
    auto& state = GetState();

    if (!state.Initialized.load(std::memory_order_acquire))
    {
        std::cout << "[CRITICAL] " << message << '\n';
        return;
    }

    LogMessage msg(LogLevel::Critical, String(message.data(), message.size()));
    msg.SetThreadId(GetCachedThreadId());
    msg.SourceFile = file;
    msg.SourceLine = line;
    msg.Function = function;
    MaybeAttachBacktrace(msg);

    if (std::this_thread::get_id() == state.DrainThreadId)
    {
        DispatchToSinks(msg);
        return;
    }

    std::lock_guard<std::mutex> lock(state.Mutex);
    DispatchToSinks(msg);
}

// ========================================
// ASYNC QUEUE
// ========================================

void Log::PushEntry(LogMessage&& entry)
{
    auto& state = GetState();

    // Pre-initialization fallback
    if (!state.Initialized.load(std::memory_order_acquire))
    {
        std::cout << "[" << LogLevelToString(entry.Level) << "] " << entry.Message << '\n';
        return;
    }

    // Async path
    if (state.DrainRunning.load(std::memory_order_acquire))
    {
        {
            std::lock_guard<std::mutex> lock(state.QueueMutex);
            state.Queue.push_back(std::move(entry));
        }
        if (state.DrainThreadWaiting.load(std::memory_order_relaxed))
        {
            state.QueueCV.notify_one();
        }
        return;
    }

    // Synchronous fallback (drain thread not running). If we ARE the drain
    // thread, a sink callback logged during the final shutdown drain: Mutex is
    // already held by this thread (non-recursive), and dispatching inline
    // would recurse through any sink that logs on every message. Drop the
    // entry — the logger is past the point of accepting work.
    if (std::this_thread::get_id() == state.DrainThreadId)
    {
        return;
    }

    const LogMessage& msg = entry;
    std::lock_guard<std::mutex> lock(state.Mutex);
    for (auto& sink : state.Sinks)
    {
        if (sink && sink->IsAvailable() && sink->ShouldLog(msg.Level))
        {
#if defined(_CPPUNWIND) || defined(__EXCEPTIONS)
            try { sink->Write(msg); } catch (...) {}
#else
            sink->Write(msg);
#endif
        }
    }
}

void Log::DispatchToSinks(const LogMessage& message)
{
    auto& state = GetState();
    for (auto& sink : state.Sinks)
    {
        if (sink && sink->IsAvailable() && sink->ShouldLog(message.Level))
        {
#if defined(_CPPUNWIND) || defined(__EXCEPTIONS)
            try { sink->Write(message); } catch (...) {}
#else
            sink->Write(message);
#endif
        }
    }
}

void Log::DrainThreadFunc()
{
#if defined(_WIN32) || defined(_WIN64)
    SetThreadDescription(GetCurrentThread(), L"Logger Drain");
#elif defined(__APPLE__)
    pthread_setname_np("Logger Drain");
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), "Logger Drain");
#endif

    auto& state = GetState();
    state.DrainThreadId = std::this_thread::get_id();

    std::vector<LogMessage> localBatch;
    std::vector<std::promise<void>> localPromises;
    // True while dispatched output may still sit unflushed in sink buffers; the
    // deadline is armed when sinks FIRST become dirty (not per batch), so a
    // sustained trickle of entries cannot postpone the flush indefinitely.
    bool sinksDirty = false;
    std::chrono::steady_clock::time_point flushDeadline{};

    while (true)
    {
        bool flushDue = false;
        {
            std::unique_lock<std::mutex> lock(state.QueueMutex);
            state.DrainThreadWaiting.store(true, std::memory_order_relaxed);
            const auto wakePredicate = [&state]()
            {
                return !state.Queue.empty()
                    || !state.FlushPromises.empty()
                    || !state.DrainRunning.load(std::memory_order_relaxed);
            };
            if (sinksDirty)
            {
                // Bounded-latency wait: dispatched-but-unflushed output must
                // become visible by the deadline even if no new work arrives
                // (trailing lines of a burst would otherwise sit in the sinks'
                // stream buffers until later traffic happens to fill them).
                if (!state.QueueCV.wait_until(lock, flushDeadline, wakePredicate))
                    flushDue = true;
            }
            else
            {
                state.QueueCV.wait(lock, wakePredicate);
            }
            state.DrainThreadWaiting.store(false, std::memory_order_relaxed);

            localBatch.swap(state.Queue);
            localPromises.swap(state.FlushPromises);
        }

        // Dispatch owned records to sinks (single lock for the entire batch).
        if (!localBatch.empty())
        {
            std::lock_guard<std::mutex> sLock(state.Mutex);
            for (auto& entry : localBatch)
            {
                DispatchToSinks(entry);
            }
            localBatch.clear();
            if (!sinksDirty)
            {
                sinksDirty = true;
                flushDeadline = std::chrono::steady_clock::now() + kMaxUnflushedSinkLatency;
            }
        }
        if (sinksDirty && std::chrono::steady_clock::now() >= flushDeadline)
        {
            flushDue = true;
        }

        if (!localPromises.empty() || flushDue)
        {
            {
                std::lock_guard<std::mutex> sLock(state.Mutex);
                for (auto& sink : state.Sinks)
                {
                    if (sink && sink->IsAvailable())
                    {
#if defined(_CPPUNWIND) || defined(__EXCEPTIONS)
                        try { sink->Flush(); } catch (...) {}
#else
                        sink->Flush();
#endif
                    }
                }
            }
            for (auto& p : localPromises)
            {
                p.set_value();
            }
            localPromises.clear();
            sinksDirty = false;
        }

        if (!state.DrainRunning.load(std::memory_order_acquire))
        {
            {
                std::lock_guard<std::mutex> lock(state.QueueMutex);
                localBatch.swap(state.Queue);
                localPromises.swap(state.FlushPromises);
            }
            if (!localBatch.empty())
            {
                std::lock_guard<std::mutex> sLock(state.Mutex);
                for (auto& entry : localBatch)
                {
                    DispatchToSinks(entry);
                }
                localBatch.clear();
            }

            if (!localPromises.empty())
            {
                {
                    std::lock_guard<std::mutex> sLock(state.Mutex);
                    for (auto& sink : state.Sinks)
                    {
                        if (sink && sink->IsAvailable())
                        {
#if defined(_CPPUNWIND) || defined(__EXCEPTIONS)
                            try { sink->Flush(); } catch (...) {}
#else
                            sink->Flush();
#endif
                        }
                    }
                }
                for (auto& p : localPromises)
                {
                    p.set_value();
                }
                localPromises.clear();
            }
            break;
        }
    }
}

void Log::Flush()
{
    auto& state = GetState();

    if (!state.Initialized.load(std::memory_order_acquire))
    {
        return;
    }

    if (std::this_thread::get_id() == state.DrainThreadId)
    {
        return;
    }

    if (!state.DrainRunning.load(std::memory_order_acquire))
    {
        std::lock_guard<std::mutex> lock(state.Mutex);
        for (auto& sink : state.Sinks)
        {
            if (sink && sink->IsAvailable())
            {
#if defined(_CPPUNWIND) || defined(__EXCEPTIONS)
                try { sink->Flush(); } catch (...) {}
#else
                sink->Flush();
#endif
            }
        }
        return;
    }

    std::future<void> fut;
    {
        std::lock_guard<std::mutex> lock(state.QueueMutex);
        if (!state.DrainRunning.load(std::memory_order_acquire))
        {
            return;
        }
        state.FlushPromises.emplace_back();
        fut = state.FlushPromises.back().get_future();
    }
    state.QueueCV.notify_one();
    fut.wait();
}

bool Log::FlushForCrash(std::chrono::milliseconds timeout)
{
    auto& state = GetState();

    // Shutdown flushes and clears the sinks before clearing this, so an
    // uninitialized logger has nothing buffered to lose.
    if (!state.Initialized.load(std::memory_order_acquire))
    {
        return true;
    }

    // The drain thread flushes the sinks itself; waiting on its own
    // acknowledgement would never return. Report false: nothing was flushed, and the
    // caller reaching here died inside a sink on the drain thread — precisely the case
    // where the tail IS at risk, so it must not be told otherwise.
    if (std::this_thread::get_id() == state.DrainThreadId)
    {
        return false;
    }

    // Flushing the sinks directly would mean taking state.Mutex, and that is the
    // one thing a death path must not do: the crashing thread may be the thread
    // holding it (it is non-recursive, so even try_lock is undefined from the
    // owner). So the entire operation is "ask the drain thread, bounded" — with
    // no drain thread there is nobody left to ask.
    if (!state.DrainRunning.load(std::memory_order_acquire))
    {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;

    std::future<void> fut;
    if (TryLockUntil(state.QueueMutex, deadline))
    {
        std::unique_lock<std::mutex> qLock(state.QueueMutex, std::adopt_lock);
        if (!state.DrainRunning.load(std::memory_order_acquire))
        {
            return false;
        }
        state.FlushPromises.emplace_back();
        fut = state.FlushPromises.back().get_future();
    }

    if (!fut.valid())
    {
        return false;
    }

    state.QueueCV.notify_one();
    return fut.wait_until(deadline) == std::future_status::ready;
}

const char* Log::GetCachedThreadId()
{
    thread_local char buffer[16] = {};
    thread_local bool initialized = false;
    if (!initialized)
    {
#if defined(_WIN32) || defined(_WIN64)
        std::snprintf(buffer, sizeof(buffer), "%lu", static_cast<unsigned long>(GetCurrentThreadId()));
#else
        std::snprintf(buffer, sizeof(buffer), "%lu", static_cast<unsigned long>(reinterpret_cast<uintptr_t>(pthread_self())));
#endif
        initialized = true;
    }
    return buffer;
}

size_t Log::GetSinkCount()
{
    auto& state = GetState();
    std::lock_guard<std::mutex> lock(state.Mutex);
    return state.Sinks.size();
}

// ========================================
// SOURCE LOCATION — owned strings
// ========================================

namespace
{
std::atomic<int> s_SourceProbeUnavailable{0};

#if defined(_WIN32) || defined(_WIN64)
#if defined(_MSC_VER)
__declspec(noinline)
#endif
bool TryCopyCString(const char* src, char* dest, size_t destSize, size_t& outLen) noexcept
{
    outLen = 0;
    if (!src || !dest || destSize == 0)
        return false;
    __try
    {
        for (size_t i = 0; i < destSize; ++i)
        {
            const char c = src[i];
            dest[i] = c;
            if (c == '\0')
            {
                outLen = i;
                return true;
            }
        }
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
#else
// Kernel reads reject inaccessible pages without changing process-wide fault
// handlers. Read one page at a time so a NUL at a page boundary remains valid.
bool TryCopyCString(const char* src, char* dest, size_t destSize, size_t& outLen) noexcept
{
    outLen = 0;
    if (!src || !dest || destSize == 0)
        return false;
    static const size_t pageSize = [] {
        const long size = sysconf(_SC_PAGESIZE);
        return size > 0 ? static_cast<size_t>(size) : size_t{4096};
    }();
    uintptr_t address = reinterpret_cast<uintptr_t>(src);
    size_t offset = 0;
    while (offset < destSize)
    {
        const size_t chunk = (std::min)({destSize - offset, pageSize - address % pageSize, size_t{128}});
        size_t copied = 0;
#if defined(__APPLE__)
        mach_vm_size_t bytes = 0;
        if (mach_vm_read_overwrite(mach_task_self(), static_cast<mach_vm_address_t>(address),
                static_cast<mach_vm_size_t>(chunk),
                reinterpret_cast<mach_vm_address_t>(dest + offset), &bytes) != KERN_SUCCESS)
            return false;
        copied = static_cast<size_t>(bytes);
#elif defined(__linux__)
        iovec local{dest + offset, chunk};
        iovec remote{reinterpret_cast<void*>(address), chunk};
        const auto bytes = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
        if (bytes <= 0)
        {
            if (bytes < 0 && (errno == EPERM || errno == EACCES || errno == ENOSYS))
            {
                int expected = 0;
                const int reason = errno;
                if (s_SourceProbeUnavailable.compare_exchange_strong(expected, reason))
                    std::fprintf(stderr, "Logger: source-location probing unavailable (error %d); source metadata omitted\n", reason);
            }
            return false;
        }
        copied = static_cast<size_t>(bytes);
#elif defined(__EMSCRIPTEN__)
        const size_t memoryBytes = __builtin_wasm_memory_size(0) * size_t{65536};
        if (address >= memoryBytes || chunk > memoryBytes - address)
            return false;
        std::memcpy(dest + offset, reinterpret_cast<const void*>(address), chunk);
        copied = chunk;
#else
#error "Logger source-pointer probing requires Windows, macOS, Linux or Emscripten"
#endif
        const auto* terminator = static_cast<const char*>(std::memchr(dest + offset, '\0', copied));
        if (terminator)
        {
            outLen = static_cast<size_t>(terminator - dest);
            return true;
        }
        if (copied != chunk)
            return false;
        offset += copied;
        address += copied;
    }
    return false;
}
#endif

void ReportUnreadableSourcePointer(const char* src)
{
    static std::atomic<int> s_Reports{0};
    if (s_Reports.fetch_add(1, std::memory_order_relaxed) >= 8)
        return;

    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "Logger: dropped unreadable source-location pointer %p",
                  static_cast<const void*>(src));

    auto frames = CaptureBacktrace(8, 2);
    std::string msg = buf;
    if (!frames.empty())
    {
        msg += " from";
        const size_t n = (std::min)(frames.size(), size_t(4));
        for (size_t i = 0; i < n; ++i)
        {
            msg += ' ';
            if (!frames[i].Symbol.empty())
                msg += frames[i].Symbol;
            else
                msg += frames[i].Module;
        }
    }
    Log::Warning(std::string_view(msg));
}
} // namespace

String CopySourceCString(const char* src)
{
    if (!src)
        return {};
    if (s_SourceProbeUnavailable.load(std::memory_order_relaxed) != 0)
        return {};
    char buf[kMaxSourceCString];
    size_t n = 0;
    if (!TryCopyCString(src, buf, kMaxSourceCString, n))
    {
        if (s_SourceProbeUnavailable.load(std::memory_order_relaxed) == 0)
            ReportUnreadableSourcePointer(src);
        return {};
    }
    return String(buf, n);
}

void LogMessage::SetSourceLocation(const char* file, int32 line, const char* function)
{
    SourceFile = CopySourceCString(file);
    SourceLine = line;
    Function = CopySourceCString(function);
}

} // namespace Logger
