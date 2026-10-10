#include "EditorApplication.h"
#include "Diagnostics/MainThreadHangWatchdog.h"
#include "Logger/Logger.h"
#include "Logger/FileSink.h"
#include "Logger/Backtrace.h"
#include "Core/Application.h" // PathUtils (default log file location)
#include "Core/Engine.h"
#include "Core/EngineLoggerBridge.h"
#include "Assets/AssetRegistry.h"
#include "AssetDatabase/AssetDbCache_Sqlite.h"
#include "Scripting/ScriptManager.h"
#include "Scripting/ScriptingABI.h"
#include "Scripting/ECSABI.h"
#include "ECS/ECS.h"
#include "ECS/ComponentRegistry.h"
#include "Core/CommandLine.h"
#include "Startup/EditorCommandLine.h"
#include "Startup/EditorStartup.h"
#include "Platform/DiscreteGpuPreference.h"
#include "Platform/Thread.h"
#include <chrono>
#include <atomic>
#include <exception>
#include <csignal>
#include <sstream>
#include <iomanip>
#include <string>
#include <string_view>
#include <filesystem>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#if defined(__APPLE__)
#include <system_error>
#include <execinfo.h>
#endif
#ifdef _WIN32
#  include <windows.h>
#  include <DbgHelp.h>
#  pragma comment(lib, "dbghelp.lib")
#else
#  include <unistd.h> // getpid (per-process log path)
#endif

#if defined(GE_ENABLE_MIMALLOC) && GE_ENABLE_MIMALLOC
	#include <mimalloc.h>
#endif

namespace {
#ifdef _WIN32
    // Crash evidence must exist in EVERY Windows config. The SEH filter below
    // swallows the exception before WER sees it, so there is no event-log entry
    // to fall back on: this dump, and the log trail the handlers flush before
    // dying, are the whole record. Without them a crash is indistinguishable
    // from an external kill.
    static std::atomic_bool gCrashDumpWritten{false};

    std::filesystem::path GetCrashDumpDirectory()
    {
        return (GameEngine::PathUtils::GetUserCacheDirectory() / "GameEngine" / "CrashDumps").lexically_normal();
    }

    std::filesystem::path BuildCrashDumpPath(std::string_view reason)
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);

        std::ostringstream name;
        name << "Editor-"
             << std::setfill('0')
             << std::setw(4) << now.wYear
             << std::setw(2) << now.wMonth
             << std::setw(2) << now.wDay
             << "-"
             << std::setw(2) << now.wHour
             << std::setw(2) << now.wMinute
             << std::setw(2) << now.wSecond
             << "-"
             << GetCurrentProcessId()
             << "-"
             << reason
             << ".dmp";

        return GetCrashDumpDirectory() / name.str();
    }

    void WriteEditorCrashDump(std::string_view reason, EXCEPTION_POINTERS* exceptionPointers)
    {
        bool expected = false;
        if (!gCrashDumpWritten.compare_exchange_strong(expected, true))
            return;

        namespace fs = std::filesystem;
        const fs::path dumpPath = BuildCrashDumpPath(reason);

        std::error_code ec;
        fs::create_directories(dumpPath.parent_path(), ec);
        if (ec)
        {
            Logger::Log::Error("Crash dump: failed to create directory '{}': {}", dumpPath.parent_path().string(), ec.message());
            std::fprintf(stderr, "[CRASH] dump directory creation failed: %s\n", dumpPath.parent_path().string().c_str());
            return;
        }

        HANDLE file = CreateFileW(
            dumpPath.wstring().c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (file == INVALID_HANDLE_VALUE)
        {
            Logger::Log::Error("Crash dump: failed to create '{}': GetLastError={}", dumpPath.string(), GetLastError());
            std::fprintf(stderr, "[CRASH] dump file creation failed: %s\n", dumpPath.string().c_str());
            return;
        }

        MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
        exceptionInfo.ThreadId = GetCurrentThreadId();
        exceptionInfo.ExceptionPointers = exceptionPointers;
        exceptionInfo.ClientPointers = FALSE;

#ifdef _DEBUG
        // Local Debug sessions: full memory, heap contents inspectable.
        MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithFullMemory |
            MiniDumpWithFullMemoryInfo |
            MiniDumpWithHandleData |
            MiniDumpWithThreadInfo |
            MiniDumpWithUnloadedModules);
#else
        // Always-on configs (DebugFast/Release) run unattended for hours with
        // multi-GB working sets; keep the dump walkable (all thread stacks,
        // referenced heap, globals) without full-RAM file sizes.
        MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithIndirectlyReferencedMemory |
            MiniDumpWithDataSegs |
            MiniDumpWithHandleData |
            MiniDumpWithThreadInfo |
            MiniDumpWithUnloadedModules);
#endif

        const BOOL ok = MiniDumpWriteDump(
            GetCurrentProcess(),
            GetCurrentProcessId(),
            file,
            dumpType,
            exceptionPointers ? &exceptionInfo : nullptr,
            nullptr,
            nullptr);

        const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        CloseHandle(file);

        if (ok)
        {
            Logger::Log::Error("Crash dump written: {}", dumpPath.string());
            std::fprintf(stderr, "[CRASH] dump written: %s\n", dumpPath.string().c_str());
            std::wstring debugMessage = L"GameEngine crash dump written: ";
            debugMessage += dumpPath.wstring();
            debugMessage += L"\n";
            OutputDebugStringW(debugMessage.c_str());
        }
        else
        {
            Logger::Log::Error("Crash dump: MiniDumpWriteDump failed for '{}': GetLastError={}", dumpPath.string(), error);
            std::fprintf(stderr, "[CRASH] MiniDumpWriteDump failed for %s: GetLastError=%lu\n", dumpPath.string().c_str(), error);
        }
    }
#endif

    // Process id of this editor, for paths that must not collide between the
    // several editors this machine routinely runs at once.
    unsigned long CurrentProcessId()
    {
#ifdef _WIN32
        return static_cast<unsigned long>(::GetCurrentProcessId());
#else
        return static_cast<unsigned long>(::getpid());
#endif
    }

#if LOGGER_ENABLE_FILE_LOGGING
    // Exactly one file sink per run — -logfile picks the path, otherwise a
    // per-process default one — so the sink's configuration and the line that
    // announces it live here rather than at either call site.
    void AttachEditorFileSink(const std::string& path, bool append)
    {
        const Logger::FileLogLevel fileLevel = Logger::ResolveFileLogLevel();

        Logger::FileSink::Config fileCfg;
        fileCfg.filename = path;
        fileCfg.append = append;
        fileCfg.minLevel = fileLevel.level;
        fileCfg.includeTimestamp = true;
#if LOGGER_ENABLE_SOURCE_LOCATION
        fileCfg.includeSource = true;
#endif
        Logger::Log::AddSink(Logger::MakeUnique<Logger::FileSink>(fileCfg));
        // Reported after the attach so both lines reach the file the run will be
        // read from, not only the console the run may not have.
        if (!fileLevel.unrecognizedValue.empty())
        {
            Logger::Log::Warning("{}='{}' names no log level; the log file falls back to {}",
                                 Logger::kFileLogLevelEnvVar, fileLevel.unrecognizedValue,
                                 Logger::LogLevelToString(fileCfg.minLevel));
        }
        Logger::Log::Info("Editor logfile: {} (records {} and above; override with {}=<debug|info|warn|error>)",
                          fileCfg.filename, Logger::LogLevelToString(fileCfg.minLevel),
                          Logger::kFileLogLevelEnvVar);
    }
#endif

    // A crash handler must not be able to block. Log::Flush waits for the drain
    // thread to acknowledge, and the drain thread cannot acknowledge while
    // another thread holds the sink mutex — which is exactly the state a thread
    // that faulted inside a sink leaves behind. Bound the wait well above the
    // drain's own 200ms flush latency and let the process die either way.
    constexpr std::chrono::milliseconds kCrashFlushTimeout{2000};

    void FlushLogsBeforeDying() noexcept
    {
        if (!Logger::Log::FlushForCrash(kCrashFlushTimeout))
        {
            std::fprintf(stderr,
                         "[CRASH] log flush did not complete within %llims; "
                         "the end of the log file may be missing\n",
                         static_cast<long long>(kCrashFlushTimeout.count()));
        }
    }

    void TerminateHandler() noexcept {
        // No symbolized backtraces from here on: dyld may be mid-teardown and
        // symbolization here has crashed the crash report (SIGILL in dyld).
        Logger::Log::SuppressBacktraceCapture();
        // Evidence goes to BOTH channels: the Logger (log file, console sink, live
        // log ring) and a synchronous stderr write. The mirror is not redundant —
        // the Editor's console sink routes every level to stdout, so a capture of
        // stderr alone would otherwise hold nothing at all.
        Logger::Log::Error("std::terminate called");
        std::fprintf(stderr, "[CRASH] std::terminate called\n");
        try {
            auto eptr = std::current_exception();
            if (eptr) std::rethrow_exception(eptr);
        } catch (const std::exception& e) {
            Logger::Log::Error("Unhandled exception at terminate: {}", e.what());
            std::fprintf(stderr, "[CRASH] unhandled exception at terminate: %s\n", e.what());
        } catch (...) {
            Logger::Log::Error("Unhandled non-std exception at terminate");
            std::fprintf(stderr, "[CRASH] unhandled non-std exception at terminate\n");
        }
#ifdef _WIN32
        WriteEditorCrashDump("terminate", nullptr);
#endif
        // Both channels are buffered and std::abort() flushes neither: the Logger
        // is asynchronous and its sinks are flushed on a timer, so the tail of the
        // trail is exactly what a crash would otherwise take with it.
        //
        // The log flush goes last of the three because it is the only one that
        // can come back empty-handed; the dump and the stderr mirror are already
        // on disk by then.
        std::fflush(stderr);
        FlushLogsBeforeDying();
        std::abort();
    }

#ifdef _WIN32
    LONG WINAPI UnhandledSEHFilter(EXCEPTION_POINTERS* info) {
        const unsigned code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
        const void* faultAddr = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
        std::ostringstream oss; oss << "Unhandled SEH exception: 0x" << std::uppercase
            << std::setfill('0') << std::setw(8) << std::hex << code
            << " at 0x" << reinterpret_cast<std::uintptr_t>(faultAddr);
        Logger::Log::Error("{}", oss.str());
        std::fprintf(stderr, "[CRASH] %s\n", oss.str().c_str());
        // Symbolized stack of the faulting thread (the filter runs on it). The
        // dump carries the exact context; this makes the log self-sufficient.
        for (const auto& f : Logger::CaptureBacktrace(48, 1))
        {
            Logger::Log::Error("  {}!{} ({}:{}) [0x{:x}]",
                               f.Module.empty() ? "?" : f.Module,
                               f.Symbol.empty() ? "?" : f.Symbol,
                               f.File, f.Line, f.Address);
            std::fprintf(stderr, "[CRASH]   %s!%s (%s:%d) [0x%llx]\n",
                         f.Module.empty() ? "?" : f.Module.c_str(),
                         f.Symbol.empty() ? "?" : f.Symbol.c_str(),
                         f.File.c_str(), f.Line,
                         static_cast<unsigned long long>(f.Address));
        }
        WriteEditorCrashDump("seh", info);
        std::fflush(stderr);
        FlushLogsBeforeDying();
        // Swallowing the exception keeps the quiet-exit behavior (no WER UI),
        // which is safe now that the dump + backtrace above exist in all configs.
        return EXCEPTION_EXECUTE_HANDLER;
    }
#endif

	    // Track how many times we've seen a fatal signal to avoid infinite
	    // re-entry loops when the faulting instruction keeps triggering SIGSEGV.
	    static std::sig_atomic_t gSignalCount = 0;

    void SignalHandler(int sig) {
        // Best-effort diagnostic only; this is not strictly async-signal-safe but
        // greatly improves crash visibility. Installed and active in ALL configs
        // including Release: it logs (Logger + synchronous stderr), writes the
        // minidump, flushes both channels, then restores SIG_DFL and re-raises so
        // the process terminates.
        if (gSignalCount < 3) {
            ++gSignalCount;
            Logger::Log::Error("Caught signal: {}", sig);
            std::fprintf(stderr, "[CRASH] caught signal: %d\n", sig);

#ifdef _WIN32
            WriteEditorCrashDump("signal", nullptr);
#endif

#if defined(__APPLE__)
            // Best-effort stack trace to help diagnose hard crashes (e.g. SIGSEGV=11).
            // backtrace_symbols_fd writes directly to an fd (stderr=2).
            void* frames[128];
            const int count = backtrace(frames, (int)(sizeof(frames) / sizeof(frames[0])));
            if (count > 0)
                backtrace_symbols_fd(frames, count, 2);
#elif defined(_WIN32)
            // Symbolize the faulting thread's stack (SymFromAddr) so hard crashes
            // are diagnosable from the log in every Windows config. The frames are
            // mirrored to stderr because the Editor's console sink writes to
            // stdout, leaving a stderr-only capture otherwise empty.
            for (const auto& f : Logger::CaptureBacktrace(48, 1))
            {
                Logger::Log::Error("  {}!{} ({}:{}) [0x{:x}]",
                                   f.Module.empty() ? "?" : f.Module,
                                   f.Symbol.empty() ? "?" : f.Symbol,
                                   f.File, f.Line, f.Address);
                std::fprintf(stderr, "[CRASH]   %s!%s (%s:%d) [0x%llx]\n",
                             f.Module.empty() ? "?" : f.Module.c_str(),
                             f.Symbol.empty() ? "?" : f.Symbol.c_str(),
                             f.File.c_str(), f.Line,
                             static_cast<unsigned long long>(f.Address));
            }
#endif
            std::fflush(stderr);
            FlushLogsBeforeDying();
        }

        // Restore default handler and re-raise so the process terminates
        // instead of spinning forever on the same faulting instruction.
        std::signal(sig, SIG_DFL);
        std::raise(sig);
    }

#if defined(__APPLE__)
    static bool EnvListHasExistingPath(const char* envValue)
    {
        if (!envValue || !envValue[0])
            return false;

        // Vulkan loader uses ':' on Unix-like platforms for multiple ICD jsons.
        const std::string v(envValue);
        size_t start = 0;
        while (start <= v.size())
        {
            size_t end = v.find(':', start);
            if (end == std::string::npos)
                end = v.size();

            const std::string token = v.substr(start, end - start);
            if (!token.empty())
            {
                std::error_code ec;
                if (std::filesystem::exists(std::filesystem::path(token), ec))
                    return true;
            }

            if (end == v.size())
                break;
            start = end + 1;
        }
        return false;
    }

    static void EnsureBundledMoltenVkIcdVisible()
    {
        // Make Finder/Dock launches self-contained:
        // ensure VK_ICD_FILENAMES points at the bundled MoltenVK_icd.json.
        // Prefer the app bundle even when the shell has a Vulkan SDK ICD pinned,
        // because local MoltenVK test builds are staged into the bundle.
        // This is a temporary bridge for local MoltenVK patches and can be
        // relaxed once official MoltenVK releases expose the required support.
        // Upstream PR: https://github.com/KhronosGroup/MoltenVK/pull/2740
        const char* existing = std::getenv("VK_ICD_FILENAMES");

        // Look for the staged ICD inside the .app bundle.
        const std::filesystem::path exeDir = GameEngine::PathUtils::GetExecutableDirectory();
        const std::filesystem::path icd =
            (exeDir / ".." / "Resources" / "vulkan" / "icd.d" / "MoltenVK_icd.json").lexically_normal();

        std::error_code ec;
        if (!icd.empty() && std::filesystem::exists(icd, ec))
        {
            const char* existingDriverFiles = std::getenv("VK_DRIVER_FILES");
            ::setenv("VK_ICD_FILENAMES", icd.string().c_str(), /*overwrite=*/1);
            ::setenv("VK_DRIVER_FILES", icd.string().c_str(), /*overwrite=*/1);
            const bool overrodeIcd =
                existing && existing[0] != '\0' && std::string(existing) != icd.string();
            const bool overrodeDriver =
                existingDriverFiles && existingDriverFiles[0] != '\0' &&
                std::string(existingDriverFiles) != icd.string();
            if (overrodeIcd)
                Logger::Log::Info("Overriding VK_ICD_FILENAMES '{}' with bundled MoltenVK ICD: {}", existing, icd.string());
            if (overrodeDriver)
                Logger::Log::Info("Overriding VK_DRIVER_FILES '{}' with bundled MoltenVK ICD: {}", existingDriverFiles, icd.string());
            if (!overrodeIcd && !overrodeDriver)
                Logger::Log::Info("Using bundled MoltenVK ICD: {}", icd.string());
        }
        else
        {
            if (EnvListHasExistingPath(existing))
            {
                Logger::Log::Info("VK_ICD_FILENAMES already set: {}", existing);
            }
            else if (existing && existing[0] != '\0')
            {
                Logger::Log::Warning(
                    "VK_ICD_FILENAMES was set but did not point to an existing ICD json; and no bundled MoltenVK_icd.json was found at '{}'. Vulkan may fail.",
                    icd.string());
            }
        }
    }
#endif
}

static int EditorMainImpl(int argc, char** argv) {
    GameEngine::Platform::SetCurrentThreadName("Main Thread");

    // Unify logging across modules FIRST: the exe statically links Logger and
    // would otherwise run its own singleton, silently forking Engine.dll's
    // logs away from every sink registered below (console/file, the debug
    // server's get_log ring, the Log panel).
    Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());

    // Inject the SQLite derived-cache implementation. Engine.dll links no SQL
    // engine; the editor opts back into the cache by registering this factory
    // before any asset source is mounted. The standalone Player never calls
    // this, so it runs cache-less (and SQLite-free).
    GameEngine::AssetRegistry::SetAssetDbCacheFactory(
        [] { return std::make_unique<GameEngine::AssetDatabase::AssetDbCache_Sqlite>(); });

    // Install basic crash handlers for diagnostics
    std::set_terminate(TerminateHandler);
#ifdef _WIN32
    SetUnhandledExceptionFilter(UnhandledSEHFilter);
#endif
    std::signal(SIGABRT, SignalHandler);
#ifdef SIGSEGV
    std::signal(SIGSEGV, SignalHandler);
#endif

    #if defined(GE_ENABLE_MIMALLOC) && GE_ENABLE_MIMALLOC
    const int mimallocVersion = mi_version();
    #endif

    // Parse Editor-specific command-line arguments (project selection, logging).
    const GameEngine::Editor::Startup::EditorCommandLineArgs editorArgs =
        GameEngine::Editor::Startup::ParseEditorCommandLine(argc, argv);

    // The logger is initialized unconditionally: the Editor UI reads its sinks, so a
    // build that reached here without any would show no logs at all.
    if (Logger::Log::GetSinkCount() == 0)
    {
        Logger::Log::Config logCfg;
        // Default policy:
        // - UI replay runs: keep log volume low (Info)
        // - Interactive Editor: Debug (so pipeline/hotreload issues are visible in RelWithDebInfo too)
        logCfg.GlobalMinLevel =
            editorArgs.uiReplayScenario.has_value() ? Logger::LogLevel::Info : Logger::LogLevel::Debug;
        // One console stream. Nothing consumes the Editor's stdout as data, so the
        // stderr split buys nothing here and costs the two failures it caused:
        // a capture of one stream looks complete while missing every Warning and
        // Error, and even `2>&1` can misorder, because the streams buffer
        // independently and the async drain writes them from the drain thread.
        logCfg.ConsoleUseStderr = false;
#if LOGGER_ENABLE_SOURCE_LOCATION
        logCfg.EnableSourceLocation = true;
#endif
        Logger::Log::Initialize(logCfg);
    }

    // Unrecognized flags are reported here rather than inside the parser: the
    // parser runs before the logger has sinks (it is what resolves -logfile), so
    // a warning raised there would go nowhere. An unknown flag must be loud —
    // a silently ignored one looks applied, and a mistyped --debug-port leaves
    // the editor on the default port, which may be another developer's.
    for (const std::string& unknown : editorArgs.unrecognizedArgs)
    {
        Logger::Log::Warning("Editor: unrecognized command-line argument '{}' (ignored)", unknown);
    }

    // UI replay/automation runs should never hang on CompileServer startup/connection.
    // Opt out preemptively so shutdown is deterministic even when pipes/process creation fails.
    if (editorArgs.uiReplayScenario.has_value())
    {
#ifdef _WIN32
        _putenv_s("GE_DISABLE_COMPILE_SERVER", "1");
#else
        setenv("GE_DISABLE_COMPILE_SERVER", "1", 1);
#endif
    }

    // Parse engine command line arguments (assets, CLR flags, etc.)
    GameEngine::EngineArgs engineArgs = GameEngine::ParseEngineArgs(argc, argv);

    // With no explicit -logfile, still write a per-user default one. The console is
    // not a reliable record of a session: a Finder/Dock or Explorer launch has no
    // console at all, and a redirected one captures a single stream, silently
    // dropping whichever severities the console sink routes to the other. The file
    // sink takes one ordered path with no severity split, so a readable artifact
    // always exists; Logger::ResolveFileLogLevel decides how much of the session
    // lands in it. Its location is logged below and exported as GE_LOGFILE.
    //
    // The path carries the process id because several editors routinely run at once
    // on one machine. Sharing one path would interleave their lines, and worse, wedge
    // rotation: the rename cannot take a file another process holds open. The id also
    // bounds the file set — the OS reuses process ids, so these names are revisited
    // rather than accumulating the way a timestamped name would, and each run opens
    // its own fresh file.
    //
    // This is intentionally best-effort; failures should not prevent startup.
#if LOGGER_ENABLE_FILE_LOGGING
    if (!editorArgs.logFile.has_value())
    {
        try
        {
            namespace fs = std::filesystem;
            std::error_code ec;

            const fs::path logDir = (GameEngine::PathUtils::GetUserCacheDirectory() / "GameEngine" / "Logs").lexically_normal();
            if (!logDir.empty())
            {
                fs::create_directories(logDir, ec);
                ec.clear();

                const fs::path logPath =
                    (logDir / ("Editor-" + std::to_string(CurrentProcessId()) + ".log")).lexically_normal();
                const std::string finalPath = logPath.string();

                // This process owns this file, so start it clean: a reused process id
                // must not concatenate an unrelated earlier session onto this one.
                AttachEditorFileSink(finalPath, /*append=*/false);

#ifdef _WIN32
                _putenv_s("GE_LOGFILE", finalPath.c_str());
#else
                setenv("GE_LOGFILE", finalPath.c_str(), 1);
#endif
            }
        }
        catch (...)
        {
            // best-effort only
        }
    }
#endif

#if defined(__APPLE__)
    // Ensure the Vulkan loader can find MoltenVK when launched from Finder/Dock (clean env).
    // Do this after logger setup so diagnostics are visible.
    EnsureBundledMoltenVkIcdVisible();
#endif

    if (editorArgs.logFile.has_value())
    {
        const std::string finalPath = editorArgs.logFile->string();

	// An explicit -logfile replaces the per-user default above rather than adding
	// to it: the two branches are mutually exclusive, so exactly one file sink
	// exists either way.

    #if LOGGER_ENABLE_FILE_LOGGING
	// Ensure parent directory exists. Without this the FileSink's
	// `ofstream::open` silently fails when -logfile points into a
	// not-yet-created Logs/ folder, and the run logs nothing to disk.
	{
	    namespace fs = std::filesystem;
	    const fs::path logPath(finalPath);
	    if (fs::is_directory(logPath))
	    {
		Logger::Log::Error("-logfile '{}' is a directory; pass a file path", finalPath);
	    }
	    else
	    {
		const fs::path parentDir = logPath.parent_path();
		if (!parentDir.empty() && !fs::exists(parentDir))
		{
		    std::error_code ec;
		    fs::create_directories(parentDir, ec);
		    if (ec)
			Logger::Log::Error("-logfile: failed to create directory '{}': {}",
					   parentDir.string(), ec.message());
		}
	    }
	}

	AttachEditorFileSink(finalPath, /*append=*/true);
    #endif

    #ifdef _WIN32
	_putenv_s("GE_LOGFILE", finalPath.c_str());
    #else
	setenv("GE_LOGFILE", finalPath.c_str(), 1);
    #endif
    }

    GameEngine::ApplicationConfig config =
        GameEngine::Editor::Startup::BuildEditorApplicationConfig(editorArgs, engineArgs);

    if (editorArgs.projectRoot.has_value())
    {
        Logger::Log::Info("Editor: ProjectRoot set via --project: {}", config.WorkspaceDirectory);
    }
    else
    {
        Logger::Log::Info("Editor: No --project specified; using default user project: {}",
                          config.WorkspaceDirectory);
    }

    GameEngine::ScriptsConfig scriptsConfig;
    if (editorArgs.uiReplayScenario.has_value())
    {
        // UI replay automation should be fast and deterministic:
        // - disable CLR + script hot-reload so Engine can skip ScriptManager entirely
        // - avoid file watchers and background build tasks that can prolong shutdown
        scriptsConfig.disableClr = true;
        scriptsConfig.enableHotReload = false;
        scriptsConfig.enableAsyncHotReload = false;
        scriptsConfig.enableAutoProjectGeneration = false;
        scriptsConfig.deferInitialLoad = false;
    }
    else
    {
        scriptsConfig.enableHotReload = true;
        scriptsConfig.enableAsyncHotReload = true;
        scriptsConfig.deferInitialLoad = true; // show visuals fast; compile/load scripts in background on clean builds
    }
    GameEngine::EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);

    // Unify the DLL-local logger with this module's state (same fix as the
    // exe<->Engine.dll split): without it, GameEngine.Native.dll logs vanish.
    GE_SetHostLoggerState(GameEngine::GetEngineLoggerState());

    // Share the host's ComponentRegistry with the DLL. Without this, the DLL's
    // copy of Engine.lib has separate handler maps, so blob-component metadata
    // registered by the host isn't visible to managed code in the DLL.
    // (ComponentTypeId is consteval post-Phase-1b — no allocator state to share.)
    GE_SetHostEcsState(
        GameEngine::ECS::ComponentRegistry::GetComponentsPtr(),
        GameEngine::ECS::ComponentRegistry::GetNameToTypeIdPtr(),
        GameEngine::ECS::ComponentRegistry::GetHandlersPtr());

    #if defined(GE_ENABLE_MIMALLOC) && GE_ENABLE_MIMALLOC
    Logger::Log::Info("mimalloc version {}", mimallocVersion);
    #endif

    auto tMainStart = std::chrono::high_resolution_clock::now();
    GameEngine::EditorApplication app(config);
    try {
        app.ConfigureFromCommandLine(editorArgs);
        {
            // Capture the original command line so an unrecoverable-device-loss
            // Save-and-Restart can relaunch with the same args (e.g. --project).
            std::vector<std::string> relaunchArgs;
            for (int i = 1; i < argc; ++i)
                relaunchArgs.emplace_back(argv[i]);
            app.SetRelaunchArgs(std::move(relaunchArgs));
        }
        if (!app.Initialize()) {
            Logger::Log::Error("Editor failed to initialize");
            return -1;
        }
        {
            auto tTotal = std::chrono::duration<double, std::milli>(
                std::chrono::high_resolution_clock::now() - tMainStart).count();
            Logger::Log::Info("[Startup] TOTAL main->Initialize: {:.1f}ms", tTotal);
        }
        // Hang watchdog: attributes main-thread stalls (frozen message pump)
        // to a call stack and the job system's occupancy in the log. Arms on
        // the first frame's heartbeat.
        GameEngine::Editor::MainThreadHangWatchdog::Start(&GameEngine::EngineCore::GetInstance().GetJobSystem());
        const int runResult = app.Run();
        // Shut down here rather than leaving it to ~EditorApplication below, so
        // teardown runs inside the watchdog window: a subsystem that wedges on
        // the way out otherwise just stops the log mid-line, leaving nothing to
        // say whether the process is slow or dead, or which phase it died in.
        // Shutdown() is idempotent, so the destructor's call stays correct.
        GameEngine::Editor::MainThreadHangWatchdog::BeginShutdownWatch();
        app.Shutdown();
        GameEngine::Editor::MainThreadHangWatchdog::Stop();
        return runResult;
    } catch (const std::exception& e) {
        Logger::Log::Error("Unhandled exception in main: {}", e.what());
        std::fprintf(stderr, "[CRASH] unhandled exception in main: %s\n", e.what());
#ifdef _WIN32
        WriteEditorCrashDump("unhandled-cpp-exception", nullptr);
#endif
        std::fflush(stderr);
        FlushLogsBeforeDying();
        // ~EditorApplication destroys the job system the watchdog reads.
        GameEngine::Editor::MainThreadHangWatchdog::Stop();
        return -2;
    } catch (...) {
        Logger::Log::Error("Unhandled non-std exception in main");
        std::fprintf(stderr, "[CRASH] unhandled non-std exception in main\n");
#ifdef _WIN32
        WriteEditorCrashDump("unhandled-unknown-exception", nullptr);
#endif
        std::fflush(stderr);
        FlushLogsBeforeDying();
        GameEngine::Editor::MainThreadHangWatchdog::Stop();
        return -3;
    }
}

#ifdef _WIN32
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow)
{
	(void)hInstance;
	(void)hPrevInstance;
	(void)lpCmdLine;
	(void)nCmdShow;

	// Use the CRT-provided argv so command-line handling stays consistent when
	// running as a Windows subsystem (no console) application.
	return EditorMainImpl(__argc, __argv);
}
#endif

int main(int argc, char** argv)
{
    return EditorMainImpl(argc, argv);
}
