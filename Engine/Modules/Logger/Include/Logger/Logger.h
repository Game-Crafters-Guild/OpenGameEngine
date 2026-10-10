#pragma once

#include "Logger/Backtrace.h"
#include "Logger/LogLevel.h"
#include "Logger/LogSink.h"
#include "Logger/Types.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <future>
#include <iterator>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

namespace Logger
{

class Log
{
  public:
    /** Crash/terminate handlers call this first: symbolizing a backtrace
        walks dyld state that may be mid-teardown there, turning the crash
        report into a second crash. Errors logged afterwards carry no
        backtrace; the OS crash report has the real one. Irreversible. */
    static void SuppressBacktraceCapture();

    struct Config
    {
        LogLevel GlobalMinLevel = LogLevel::Info;
        bool EnableSourceLocation = false;
        // Route Warning and above to stderr on the default console sink. Correct for
        // a process whose stdout is consumed as data; false for one whose console is
        // read by a human, where splitting the severities across two independently
        // buffered streams costs ordering and loses half of any single-stream capture.
        bool ConsoleUseStderr = true;
    };

    static void Initialize(const Config& config);
    static void Shutdown();
    static void AddSink(UniquePtr<LogSink> sink);
    static void ClearSinks();
    static void SetLogLevel(LogLevel level);
    static LogLevel GetLogLevel();

    /**
     * @brief Block until every queued entry has reached the sinks and the sinks
     *        have been flushed.
     *
     * Unbounded by design: the wait ends only when the drain thread acknowledges,
     * and the drain thread cannot acknowledge while another thread holds the sink
     * lock. A caller that must make progress even then — anything on a path that
     * ends in abort(), raise() or a debug break — wants FlushForCrash instead.
     */
    static void Flush();

    /**
     * @brief Ask the drain thread for a flush, wait at most @p timeout, return
     *        either way.
     *
     * For death paths — anything that ends in abort(), raise() or a debug break.
     * It never takes the sink mutex and never waits on one without a deadline,
     * so a thread stuck or dead inside a sink cannot turn a crash into a hang.
     *
     * @return true if the logger acknowledged the flush (or had nothing to
     *         flush); false if the deadline passed first or no drain thread was
     *         left to answer, meaning the tail of the log may be missing.
     */
    static bool FlushForCrash(std::chrono::milliseconds timeout);

    static size_t GetSinkCount();

    // Cross-module unification. Logger is a STATIC library, so a host exe and
    // a shared engine DLL each get their own copy of the GetState() singleton
    // and their logs silently fork (DLL warnings never reach the host's ring/
    // file/panel sinks). The host adopts the engine's state at startup —
    //   Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
    // — after which every operation in the host's Logger copy (AddSink,
    // levels, the queue) acts on the shared state. Call once, on the main
    // thread, before Initialize/AddSink and before any worker threads log.
    struct LoggerState;
    static LoggerState* GetStateForSharing();
    static void RedirectToSharedState(LoggerState* state);

    // ========================================
    // MAIN LOGGING INTERFACE
    // ========================================

    static void Trace(std::string_view message) { if (ShouldLog(LogLevel::Trace)) EnqueueString(LogLevel::Trace, message); }

    template <typename... Args>
    static void Trace(std::format_string<Args...> fmt, Args&&... args)
    {
        if (!ShouldLog(LogLevel::Trace)) return;
        EnqueueFormatted(LogLevel::Trace, fmt.get(), std::make_format_args(args...));
    }

    static void Debug(std::string_view message) { if (ShouldLog(LogLevel::Debug)) EnqueueString(LogLevel::Debug, message); }

    template <typename... Args>
    static void Debug(std::format_string<Args...> fmt, Args&&... args)
    {
        if (!ShouldLog(LogLevel::Debug)) return;
        EnqueueFormatted(LogLevel::Debug, fmt.get(), std::make_format_args(args...));
    }

    static void Info(std::string_view message) { if (ShouldLog(LogLevel::Info)) EnqueueString(LogLevel::Info, message); }

    template <typename... Args>
    static void Info(std::format_string<Args...> fmt, Args&&... args)
    {
        if (!ShouldLog(LogLevel::Info)) return;
        EnqueueFormatted(LogLevel::Info, fmt.get(), std::make_format_args(args...));
    }

    static void Warning(std::string_view message) { if (ShouldLog(LogLevel::Warning)) EnqueueString(LogLevel::Warning, message); }

    template <typename... Args>
    static void Warning(std::format_string<Args...> fmt, Args&&... args)
    {
        if (!ShouldLog(LogLevel::Warning)) return;
        EnqueueFormatted(LogLevel::Warning, fmt.get(), std::make_format_args(args...));
    }

    static void Error(std::string_view message) { if (ShouldLog(LogLevel::Error)) EnqueueString(LogLevel::Error, message); }

    template <typename... Args>
    static void Error(std::format_string<Args...> fmt, Args&&... args)
    {
        if (!ShouldLog(LogLevel::Error)) return;
        EnqueueFormatted(LogLevel::Error, fmt.get(), std::make_format_args(args...));
    }

    // Critical formats eagerly and dispatches synchronously for crash safety.
    static void Critical(std::string_view message) { if (ShouldLog(LogLevel::Critical)) CriticalImpl(message); }

    template <typename... Args>
    static void Critical(std::format_string<Args...> fmt, Args&&... args)
    {
        if (!ShouldLog(LogLevel::Critical)) return;
        auto& buf = GetFormatBuffer();
        buf.clear();
        std::format_to(std::back_inserter(buf), fmt, std::forward<Args>(args)...);
        CriticalImpl(std::string_view(buf));
    }

    // ========================================
    // LOGGING WITH SOURCE LOCATION
    // ========================================


    static void LogWithSource(LogLevel level, const OwnedSourceLocation& source, int line,
                              std::string_view message)
    {
        if (!ShouldLog(level)) return;
        if (level >= LogLevel::Critical)
            CriticalImplWithSource(message, source.File(), line, source.Function());
        else
            EnqueueStringWithSource(level, message, source.File(), line, source.Function());
    }

    template <typename... Args>
    static void LogWithSource(LogLevel level, const OwnedSourceLocation& source, int line,
                              std::format_string<Args...> fmt, Args&&... args)
    {
        if (!ShouldLog(level)) return;
        if (level >= LogLevel::Critical)
        {
            auto& buf = GetFormatBuffer();
            buf.clear();
            std::format_to(std::back_inserter(buf), fmt, std::forward<Args>(args)...);
            CriticalImplWithSource(std::string_view(buf), source.File(), line, source.Function());
            return;
        }
        EnqueueFormattedWithSource(level, fmt.get(), std::make_format_args(args...),
                                   source.File(), line, source.Function());
    }

    static void LogWithSource(LogLevel level, const char* file, int line, const char* function,
                              std::string_view message)
    {
        if (!ShouldLog(level)) return;
        if (level >= LogLevel::Critical)
        {
            CriticalImplWithSource(message, CopySourceCString(file), line, CopySourceCString(function));
            return;
        }
        EnqueueString(level, message, file, line, function);
    }

    template <typename... Args>
    static void LogWithSource(LogLevel level, const char* file, int line, const char* function,
                              std::format_string<Args...> fmt, Args&&... args)
    {
        if (!ShouldLog(level)) return;
        if (level >= LogLevel::Critical)
        {
            auto& buf = GetFormatBuffer();
            buf.clear();
            std::format_to(std::back_inserter(buf), fmt, std::forward<Args>(args)...);
            CriticalImplWithSource(std::string_view(buf), CopySourceCString(file), line, CopySourceCString(function));
            return;
        }
        EnqueueFormatted(level, fmt.get(), std::make_format_args(args...), file, line, function);
    }

    // The gate every entry point and the source macros share: below the
    // effective level nothing is formatted, copied or queued.
    static bool ShouldLog(LogLevel level)
    {
        return level >= GetState().m_EffectiveMinLevel.load(std::memory_order_relaxed);
    }

  private:

    // Backtrace capture must run on the calling thread (the drain thread's stack
    // is meaningless for diagnostics). Only Error+ severities pay the cost.
    static void MaybeAttachBacktrace(LogMessage& msg);

    // The single construction point for a queued record: everything but the
    // message text, owned by the record before it leaves the producer. A null
    // file/function means the call site supplied no source location.
    static LogMessage MakeOwnedRecord(LogLevel level, const char* file, int line,
                                      const char* function);

    // Consume borrowed arguments and any module-defined formatter before the
    // producer returns. Only owned text/metadata crosses the asynchronous queue;
    // moving, dispatching or destroying a record never calls into its producer.
    static void EnqueueFormatted(LogLevel level, std::string_view format, std::format_args args,
                                 const char* file = nullptr, int line = 0,
                                 const char* function = nullptr);

    // String enqueue copies the supplied text into the owned record.
    static void EnqueueString(LogLevel level, std::string_view message,
                              const char* file = nullptr, int line = 0,
                              const char* function = nullptr);

    // Owned metadata bypasses pointer probing, but is copied into the record.
    static void EnqueueFormattedWithSource(LogLevel level, std::string_view format,
                                           std::format_args args, std::string_view file,
                                           int line, std::string_view function);
    static void EnqueueStringWithSource(LogLevel level, std::string_view message,
                                        std::string_view file, int line, std::string_view function);

    // Critical synchronous dispatch (formats eagerly, bypasses queue).
    static void CriticalImpl(std::string_view message);
    static void CriticalImplWithSource(std::string_view message,
                                       std::string_view file, int line, std::string_view function);

    static void PushEntry(LogMessage&& entry);

    static const char* GetCachedThreadId();
    static std::string& GetFormatBuffer();

    // Async drain thread
    static void DrainThreadFunc();
    static void DispatchToSinks(const LogMessage& message);

    // Public ONLY so the opaque pointer can round-trip through
    // GetStateForSharing/RedirectToSharedState across module copies; treat as
    // an opaque handle outside Logger.cpp.
  public:
    struct LoggerState
    {
        Config m_Config;
        Vector<UniquePtr<LogSink>> Sinks;
        std::mutex Mutex;
        std::atomic<bool> Initialized{false};
        std::atomic<LogLevel> m_EffectiveMinLevel{LogLevel::Off};

        // Async drain infrastructure
        std::vector<LogMessage> Queue;
        std::mutex QueueMutex;
        std::condition_variable QueueCV;
        std::thread DrainThread;
        std::atomic<bool> DrainRunning{false};
        std::atomic<bool> DrainThreadWaiting{false};
        std::thread::id DrainThreadId{};
        std::vector<std::promise<void>> FlushPromises;

        ~LoggerState()
        {
            if (DrainThread.joinable())
            {
                DrainRunning.store(false, std::memory_order_release);
                QueueCV.notify_one();
                DrainThread.join();
            }
        }
    };

  private:
    static LoggerState& GetState();

    DISALLOW_COPY_AND_ASSIGN(Log);
};

} // namespace Logger

// ========================================
// CONVENIENCE MACROS
// ========================================

#if LOGGER_ENABLE_SOURCE_LOCATION && defined(_DEBUG)
#define LOGGER_LOG_OWNED_SOURCE(level, format, ...) \
    do { \
        if (Logger::Log::ShouldLog(level)) { \
            const auto& loggerSource = [](const char* file, const char* function) -> const Logger::OwnedSourceLocation& { \
                static const Logger::OwnedSourceLocation source(file, function); \
                return source; \
            }(__FILE__, __FUNCTION__); \
            Logger::Log::LogWithSource(level, loggerSource, __LINE__, format __VA_OPT__(,) __VA_ARGS__); \
        } \
    } while (false)
#define LOG_TRACE(format, ...) \
    LOGGER_LOG_OWNED_SOURCE(Logger::LogLevel::Trace, format __VA_OPT__(,) __VA_ARGS__)
#define LOG_DEBUG(format, ...) \
    LOGGER_LOG_OWNED_SOURCE(Logger::LogLevel::Debug, format __VA_OPT__(,) __VA_ARGS__)
#define LOG_INFO(format, ...) \
    LOGGER_LOG_OWNED_SOURCE(Logger::LogLevel::Info, format __VA_OPT__(,) __VA_ARGS__)
#define LOG_WARNING(format, ...) \
    LOGGER_LOG_OWNED_SOURCE(Logger::LogLevel::Warning, format __VA_OPT__(,) __VA_ARGS__)
#define LOG_ERROR(format, ...) \
    LOGGER_LOG_OWNED_SOURCE(Logger::LogLevel::Error, format __VA_OPT__(,) __VA_ARGS__)
#define LOG_CRITICAL(format, ...) \
    LOGGER_LOG_OWNED_SOURCE(Logger::LogLevel::Critical, format __VA_OPT__(,) __VA_ARGS__)
#else
#ifdef NDEBUG
#define LOG_TRACE(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#else
#define LOG_TRACE(format, ...) Logger::Log::Trace(format, ##__VA_ARGS__)
#define LOG_DEBUG(format, ...) Logger::Log::Debug(format, ##__VA_ARGS__)
#endif
#define LOG_INFO(format, ...) Logger::Log::Info(format, ##__VA_ARGS__)
#define LOG_WARNING(format, ...) Logger::Log::Warning(format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) Logger::Log::Error(format, ##__VA_ARGS__)
#define LOG_CRITICAL(format, ...) Logger::Log::Critical(format, ##__VA_ARGS__)
#endif
