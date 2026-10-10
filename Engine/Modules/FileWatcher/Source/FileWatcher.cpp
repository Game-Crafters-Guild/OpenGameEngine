#include "FileWatcher/FileWatcher.h"
#include "Logger/Logger.h"
#include "Platform/Thread.h"

#include <algorithm>
#include <cctype>
#include <vector>

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#endif

#ifdef PLATFORM_MACOS
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#endif

namespace GameEngine
{

namespace
{
// Longest single wait in either watch loop. Debounced events fire at this
// granularity, and m_ShouldStop is rechecked at least this often.
constexpr uint32 kWatchLoopSliceMs = 50;
} // namespace

#ifdef PLATFORM_MACOS
namespace
{
// FSEvents coalescing window. Small enough that an editor save reaches the
// rescan in well under a frame's worth of human latency, large enough that a
// multi-file save wakes the loop once rather than per file.
constexpr CFAbsoluteTime kChangeSignalLatencySeconds = 0.05;
} // namespace

bool FileWatcher::StartChangeSignal()
{
    CFStringRef path = CFStringCreateWithCString(nullptr, m_WatchDirectory.c_str(),
                                                 kCFStringEncodingUTF8);
    if (path == nullptr)
        return false;
    CFArrayRef paths = CFArrayCreate(nullptr, reinterpret_cast<const void**>(&path), 1,
                                     &kCFTypeArrayCallBacks);
    CFRelease(path);
    if (paths == nullptr)
        return false;

    FSEventStreamContext context{};
    context.info = this;
    // The callback is a wake, not a source of events: it deliberately ignores
    // the reported paths and flags, because the rescan below it is what decides
    // what actually changed (and applies the extension/exclusion filters).
    auto onEvent = [](ConstFSEventStreamRef, void* info, size_t, void*,
                      const FSEventStreamEventFlags*, const FSEventStreamEventId*)
    {
        auto* self = static_cast<FileWatcher*>(info);
        {
            std::lock_guard<std::mutex> lock(self->m_WakeMutex);
            self->m_WakeRequested = true;
        }
        self->m_WakeCv.notify_one();
    };

    FSEventStreamRef stream = FSEventStreamCreate(
        nullptr, onEvent, &context, paths, kFSEventStreamEventIdSinceNow,
        kChangeSignalLatencySeconds,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
    CFRelease(paths);
    if (stream == nullptr)
        return false;

    dispatch_queue_t queue =
        dispatch_queue_create("com.gameengine.filewatcher.fsevents", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(stream, queue);
    if (!FSEventStreamStart(stream))
    {
        FSEventStreamSetDispatchQueue(stream, nullptr);
        FSEventStreamRelease(stream);
        dispatch_release(queue);
        return false;
    }

    m_ChangeSignalStream = stream;
    m_ChangeSignalQueue = queue;
    return true;
}

void FileWatcher::StopChangeSignal()
{
    if (m_ChangeSignalStream != nullptr)
    {
        auto stream = static_cast<FSEventStreamRef>(m_ChangeSignalStream);
        FSEventStreamStop(stream);
        // Stop + unschedule + invalidate prevent NEW callback invocations from
        // being enqueued, but FSEvents does not document that any of them waits
        // for a callback that is already executing — the drain below is what
        // guarantees that.
        FSEventStreamSetDispatchQueue(stream, nullptr);
        FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);
        m_ChangeSignalStream = nullptr;
    }
    if (m_ChangeSignalQueue != nullptr)
    {
        auto queue = static_cast<dispatch_queue_t>(m_ChangeSignalQueue);
        // Serial queue: an empty synchronous block cannot start until a callback
        // already on the queue has returned. After this line no callback can be
        // touching `this` (it captures the watcher), so releasing the queue and
        // tearing the watcher down is safe.
        dispatch_sync_f(queue, nullptr, [](void*) {});
        dispatch_release(queue);
        m_ChangeSignalQueue = nullptr;
    }
}
#endif // PLATFORM_MACOS


FileWatcher::FileWatcher()
    : m_Recursive(false), m_Watching(false), m_ShouldStop(false), m_PollingInterval(1000) // 1 second default
{
}

FileWatcher::~FileWatcher()
{
    StopWatching();
}

bool FileWatcher::StartWatching(const std::filesystem::path& directory, bool recursive)
{
    if (m_WatchThread.joinable())
    {
        Logger::Log::Warning("FileWatcher already has a running thread");
        return true;
    }

    if (!std::filesystem::exists(directory))
    {
        Logger::Log::Error("Directory does not exist: {}", directory.string());
        return false;
    }

    m_WatchDirectory = directory;
    m_Recursive = recursive;
    m_ShouldStop = false;

    // Pre-compute normalized absolute base path once (read-only after this point).
    std::error_code ec;
    m_NormalizedWatchBase = std::filesystem::absolute(m_WatchDirectory, ec);
    if (ec)
        m_NormalizedWatchBase = m_WatchDirectory;
    m_NormalizedWatchBase = m_NormalizedWatchBase.lexically_normal();

    // Start watching thread
    m_WatchThread = std::thread(&FileWatcher::WatchingThread, this);

    Logger::Log::Debug("Started watching directory: {} (recursive: {})",
                       directory.string(), recursive);
    return true;
}

void FileWatcher::StopWatching()
{
    if (!m_WatchThread.joinable())
    {
        m_PendingEvents.clear();
#ifdef PLATFORM_WINDOWS
        m_PendingRenameOldPath.clear();
        m_HasPendingRenameOldPath = false;
#endif
        return;
    }

    Logger::Log::Debug("Stopping file watcher");

    // join() has no timeout because no wait in the watch loop outlasts
    // kWatchLoopSliceMs: the polling loop waits on m_WakeCv, which RequestStop
    // ends at once, and the Windows loop rechecks m_ShouldStop after each
    // WaitForSingleObject slice. Stop returns within one slice plus any scan or
    // callback already running. A new blocking call in either loop must keep
    // that bound (a timed slice, or a wake from RequestStop), or stop hangs.
    RequestStop();
    m_WatchThread.join();

    m_Watching = false;

    // The watch thread has exited, so its pending state is safe to clear.
    m_PendingEvents.clear();
#ifdef PLATFORM_WINDOWS
    m_PendingRenameOldPath.clear();
    m_HasPendingRenameOldPath = false;
#endif

    Logger::Log::Debug("File watcher stopped");
}

void FileWatcher::RequestStop()
{
    m_ShouldStop = true;
    {
        std::lock_guard<std::mutex> lock(m_WakeMutex);
        m_WakeRequested = true;
    }
    m_WakeCv.notify_all();
}

void FileWatcher::AddExtensionFilter(const String& extension)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ExtensionFilters.push_back(extension);
}

void FileWatcher::ClearExtensionFilters()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ExtensionFilters.clear();
}

void FileWatcher::WatchingThread()
{
    Platform::SetCurrentThreadName("File Watcher");
    Logger::Log::Debug("FileWatcher thread started for directory: {}", m_WatchDirectory.string());
    m_Watching = true;

#ifdef PLATFORM_WINDOWS
    WatchWindows();
#else
#ifdef PLATFORM_MACOS
    // Owned by this thread so the stream's lifetime is strictly inside the
    // loop that its callback wakes.
    const bool signalled = StartChangeSignal();
    Logger::Log::Info("FileWatcher: polling every {} ms, FSEvents wake {}", m_PollingInterval,
                      signalled ? "active" : "unavailable (poll interval is the only latency)");
#endif
    WatchPolling();
#ifdef PLATFORM_MACOS
    StopChangeSignal();
#endif
#endif

    m_Watching = false;
}

bool FileWatcher::ShouldWatchFile(const std::filesystem::path& path) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    // Skip internal engine/editor persistence files (avoid feedback loops and noisy reloads).
    // Note: directory exclusions are handled by ShouldExcludeDirectory, but this covers
    // single files like the authoritative asset database.
    const String filename = path.filename().string();
    if (filename == "AssetDatabase.assetdb" || filename == "AssetDatabase.assetdb.tmp")
    {
        return false;
    }

    // Skip common build artifacts that can appear next to watched assets (typical
    // when the editor runs from its own exe output directory alongside .pdb/.ilk
    // and performs atomic saves via sibling .tmp files). Case-insensitive:
    // ReplaceFile-style saves (Photoshop, Office) produce "name~RFxxxx.TMP"
    // intermediates with an uppercase extension, and letting those through
    // drags the replaced asset's identity onto the intermediate path.
    String ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* kBuildExtensions[] = {
        ".tmp", ".pdb", ".ilk", ".exp", ".obj", ".lib",
        ".exe", ".dll",
    };
    for (const char* bx : kBuildExtensions)
    {
        if (ext == bx)
            return false;
    }

    // Skip hidden/special files (e.g., macOS .DS_Store, .gitkeep, etc.)
    // This matches the AssetRegistry/AsyncRegistryTasks behaviour which ignores
    // any file whose name starts with a dot.
    if (!filename.empty() && filename[0] == '.')
    {
        return false;
    }

    // Check if any parent directory should be excluded
    if (ShouldExcludeDirectory(path))
    {
        return false;
    }

    // If no filters, watch all files
    if (m_ExtensionFilters.empty())
    {
        return true;
    }

    for (const auto& filter : m_ExtensionFilters)
    {
        String loweredFilter = filter;
        std::transform(loweredFilter.begin(), loweredFilter.end(), loweredFilter.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == loweredFilter)
        {
            return true;
        }
    }
    return false;
}

bool FileWatcher::ShouldExcludeDirectory(const std::filesystem::path& path) const
{
    // Calculate a *lexical* relative path from the watch directory.
    //
    // IMPORTANT: do NOT use std::filesystem::relative() here.
    // It may consult the filesystem and can throw when either path doesn't exist,
    // which is common during atomic-save flows. Falling back to absolute paths
    // can also accidentally match excluded patterns like "/build/" in the *absolute*
    // path, filtering out legitimate changes (e.g. editor assets under build outputs).
    //
    // Use the pre-computed normalized base path to avoid repeated absolute() +
    // lexically_normal() calls (heavy allocation per invocation, called thousands
    // of times per polling cycle).
    const auto& base = m_NormalizedWatchBase;
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(path, ec);
    if (ec)
        abs = path;
    abs = abs.lexically_normal();

    std::filesystem::path relativePath = abs.lexically_relative(base);
    const std::string relStrCheck = relativePath.generic_string();
    if (relStrCheck.empty() || relStrCheck == "." || relStrCheck.rfind("..", 0) == 0)
    {
        // If we can't express this path relative to the watch root, don't exclude it here.
        // Higher layers can filter as needed.
        return false;
    }

    // Convert to string for easier checking
    String pathStr = relativePath.string();

    // Normalize path separators to forward slashes for consistent checking
    std::replace(pathStr.begin(), pathStr.end(), '\\', '/');

    // Ensure leading slash so patterns like "/obj/" match root-level directories (e.g. "obj/...")
    if (pathStr.empty() || pathStr[0] != '/')
    {
        pathStr = "/" + pathStr;
    }

    // Special exception: Allow watching demo resources for hot-reload
    if (pathStr.find("/Demo/Resources") != String::npos)
    {
        return false;
    }

    // Check if RELATIVE path contains excluded directories
    // These are build artifacts that should not trigger hot-reload
    static const Vector<String> excludedDirs = {
        "/obj/",          // .NET build intermediate files
        "/bin/",          // .NET build output files
        "/.vs/",          // Visual Studio files
        "/.vscode/",      // VS Code files
        "/node_modules/", // Node.js dependencies
        "/.git/",         // Git repository files
        "/.Cache/",       // Engine/Editor derived caches (AssetDbCache.sqlite, shader cache, etc.)
        "/.MyEngine/",    // Legacy derived cache root (backward compat with older layouts)
        "/.Editor/",      // Editor derived cache (thumbnails, layout, etc.)
        "/__pycache__/",  // Python bytecode cache (common inside scripts/venv)
        "/site-packages/", // Python venv packages (can be huge; never engine assets)
        "/.venv/",        // Python virtualenv (common naming)
        "/venv/",         // Python virtualenv (common naming)
        "/__MACOSX/",     // macOS archive metadata folder
        "/build/",        // CMake build directory (relative to watch dir)
        "/Debug/",        // Debug build artifacts
        "/Release/"       // Release build artifacts
    };

    for (const auto& excludedDir : excludedDirs)
    {
        if (pathStr.find(excludedDir) != String::npos)
        {
            return true;
        }

        // Also match the directory itself (no trailing slash) — Windows can
        // report a change on the directory entry rather than a file inside it.
        // e.g. pathStr "/submodule/.git" should match excludedDir "/.git/".
        const auto dirNoSlash = excludedDir.substr(0, excludedDir.size() - 1);
        if (pathStr.size() >= dirNoSlash.size() &&
            pathStr.compare(pathStr.size() - dirNoSlash.size(), dirNoSlash.size(), dirNoSlash) == 0)
        {
            return true;
        }
    }

    return false;
}

void FileWatcher::NotifyFileChange(const FileChangeEvent& event)
{
    if (m_Callback && ShouldWatchFile(event.Path))
    {
        // RACE CONDITION FIX: Use debounced notification to prevent multiple triggers
        NotifyFileChangeDebounced(event);
    }
}

#ifdef PLATFORM_WINDOWS
void FileWatcher::WatchWindows()
{
    // Open directory handle
    HANDLE hDir = CreateFileW(
        m_WatchDirectory.wstring().c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (hDir == INVALID_HANDLE_VALUE)
    {
        DWORD error = GetLastError();
        Logger::Log::Error("❌ Failed to open directory for watching: {} (error: {})", m_WatchDirectory.string(), error);
        return;
    }

    // Buffer for change notifications
    constexpr DWORD bufferSize = 64 * 1024; // 64KB buffer
    std::vector<uint8> buffer(bufferSize);
    OVERLAPPED overlapped = {};
    // Manual-reset event simplifies re-issuing overlapped operations safely.
    overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    if (!overlapped.hEvent)
    {
        Logger::Log::Error("Failed to create event for file watching: {}", GetLastError());
        CloseHandle(hDir);
        return;
    }

    Logger::Log::Debug("FileWatcher: ReadDirectoryChangesW armed for {}", m_WatchDirectory.string());

    auto issueRead = [&]() -> bool
    {
        // Reset overlapped state (except for the event handle) before re-issuing.
        HANDLE evt = overlapped.hEvent;
        overlapped = {};
        overlapped.hEvent = evt;
        ResetEvent(overlapped.hEvent);

        // Start async directory change monitoring. When using OVERLAPPED, lpBytesReturned is ignored.
        BOOL result = ReadDirectoryChangesW(
            hDir,
            buffer.data(),
            bufferSize,
            m_Recursive ? TRUE : FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME |
                FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_LAST_WRITE |
                FILE_NOTIFY_CHANGE_SIZE,
            nullptr,
            &overlapped,
            nullptr);

        if (result)
        {
            return true;
        }

        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING)
        {
            return true; // Expected for overlapped I/O
        }
        if (error == ERROR_OPERATION_ABORTED || m_ShouldStop.load())
        {
            Logger::Log::Debug("ReadDirectoryChangesW operation aborted (shutdown)");
            return false;
        }
        Logger::Log::Error("ReadDirectoryChangesW failed: {}", error);
        return false;
    };

    // Issue the first async read; then only re-issue after we have processed a completion.
    if (!issueRead())
    {
        // Cleanup below
        CancelIoEx(hDir, &overlapped);
        CloseHandle(overlapped.hEvent);
        CloseHandle(hDir);
        Logger::Log::Debug("Windows file watching stopped");
        return;
    }

    bool hasPendingRead = true;
    while (!m_ShouldStop.load())
    {
        // Wait one slice so debounced events fire on time and m_ShouldStop is
        // rechecked; nothing signals this event on stop.
        DWORD waitResult = WaitForSingleObject(overlapped.hEvent, kWatchLoopSliceMs);

        // Fire any debounced events whose deadline has elapsed
        ProcessReadyDebouncedEvents();

        if (waitResult == WAIT_TIMEOUT)
        {
            continue; // keep waiting on the same pending read
        }

        if (waitResult != WAIT_OBJECT_0)
        {
            DWORD error = GetLastError();
            if (error == ERROR_OPERATION_ABORTED || m_ShouldStop.load())
            {
                Logger::Log::Debug("WaitForSingleObject operation aborted (shutdown)");
                break;
            }
            Logger::Log::Error("WaitForSingleObject failed: {}", error);
            break;
        }

        DWORD bytesReturned = 0;
        if (!GetOverlappedResult(hDir, &overlapped, &bytesReturned, FALSE))
        {
            DWORD error = GetLastError();
            if (error == ERROR_OPERATION_ABORTED || m_ShouldStop.load())
            {
                Logger::Log::Debug("GetOverlappedResult operation aborted (shutdown)");
                break;
            }
            // NOTE: ERROR_NOTIFY_ENUM_DIR indicates overflow/lost events; we keep going.
            Logger::Log::Error("GetOverlappedResult failed: {}", error);
        }
        else
        {
            ProcessWindowsFileChanges(buffer.data(), bytesReturned);
        }

        hasPendingRead = false;

        if (m_ShouldStop.load())
        {
            break;
        }

        // Re-issue for the next set of changes
        if (!issueRead())
        {
            break;
        }
        hasPendingRead = true;
    }

    // Cancel any pending overlapped operations before cleanup
    if (hasPendingRead)
    {
        CancelIoEx(hDir, &overlapped);
    }

    // Cleanup
    CloseHandle(overlapped.hEvent);
    CloseHandle(hDir);
    Logger::Log::Debug("Windows file watching stopped");
}

void FileWatcher::ProcessWindowsFileChanges(uint8* buffer, uint32 bufferSize)
{
    if (!buffer || bufferSize == 0)
    {
        return;
    }

    const uint8* const begin = buffer;
    const uint8* const end = buffer + bufferSize;
    const uint8* ptr = begin;

    constexpr size_t kHeaderSize = offsetof(FILE_NOTIFY_INFORMATION, FileName);

    while (ptr + kHeaderSize <= end)
    {
        const FILE_NOTIFY_INFORMATION* info =
            reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(ptr);

        const uint32 fileNameBytes = info->FileNameLength;
        if ((fileNameBytes % sizeof(wchar_t)) != 0)
        {
            Logger::Log::Warning("Malformed FILE_NOTIFY_INFORMATION (odd FileNameLength={}): ignoring remaining data", fileNameBytes);
            break;
        }

        const size_t recordSize = kHeaderSize + static_cast<size_t>(fileNameBytes);
        if (ptr + recordSize > end)
        {
            Logger::Log::Warning("Truncated FILE_NOTIFY_INFORMATION (recordSize={} > remaining={}): ignoring remaining data",
                                 recordSize, static_cast<size_t>(end - ptr));
            break;
        }

        // Convert filename from wide string to regular string (not null-terminated)
        std::wstring wfilename(info->FileName, fileNameBytes / sizeof(wchar_t));
        std::filesystem::path filePath = m_WatchDirectory / std::filesystem::path(wfilename);

        // Skip if this is in an excluded directory OR the specific file is filtered
        // (build artifacts, persistence .tmp files, hidden files, etc.). ShouldWatchFile
        // catches file-level exclusions that ShouldExcludeDirectory doesn't (e.g.
        // AssetDatabase.assetdb.tmp in the watch root itself).
        if (ShouldExcludeDirectory(filePath) || !ShouldWatchFile(filePath))
        {
            // Avoid log spam: excluded paths (build output, .tmp files, etc.) can
            // change very frequently and are not interesting to the asset system.
        }
        else
        {
            auto emit = [this](FileChangeEvent& event)
            {
                Logger::Log::Debug("Windows file change detected: {} ({})",
                                   event.Path.string(), static_cast<int>(event.Type));
                NotifyFileChange(event);
            };

            const auto nowTs = std::filesystem::file_time_type::clock::now();

            switch (info->Action)
            {
            case FILE_ACTION_ADDED:
            {
                FileChangeEvent event;
                event.Path = filePath;
                event.Type = FileChangeType::Created;
                event.Timestamp = nowTs;
                emit(event);
                break;
            }
            case FILE_ACTION_REMOVED:
            {
                FileChangeEvent event;
                event.Path = filePath;
                event.Type = FileChangeType::Deleted;
                event.Timestamp = nowTs;
                emit(event);
                break;
            }
            case FILE_ACTION_MODIFIED:
            {
                FileChangeEvent event;
                event.Path = filePath;
                event.Type = FileChangeType::Modified;
                event.Timestamp = nowTs;
                emit(event);
                break;
            }
            case FILE_ACTION_RENAMED_OLD_NAME:
            {
                // Store for pairing with FILE_ACTION_RENAMED_NEW_NAME.
                // If an old name is already pending (unexpected), flush it as a delete
                // so the system does not get stuck with a stale pending path.
                if (m_HasPendingRenameOldPath && !m_PendingRenameOldPath.empty())
                {
                    FileChangeEvent flush;
                    flush.Path = m_PendingRenameOldPath;
                    flush.Type = FileChangeType::Deleted;
                    flush.Timestamp = nowTs;
                    emit(flush);
                }
                m_PendingRenameOldPath = filePath;
                m_HasPendingRenameOldPath = true;
                break;
            }
            case FILE_ACTION_RENAMED_NEW_NAME:
            {
                FileChangeEvent event;
                event.Path = filePath;
                event.Type = FileChangeType::Renamed;
                event.Timestamp = nowTs;
                if (m_HasPendingRenameOldPath)
                {
                    event.OldPath = m_PendingRenameOldPath;
                    m_PendingRenameOldPath.clear();
                    m_HasPendingRenameOldPath = false;
                }
                emit(event);
                break;
            }
            default:
            {
                // Treat unknown actions as modified.
                FileChangeEvent event;
                event.Path = filePath;
                event.Type = FileChangeType::Modified;
                event.Timestamp = nowTs;
                emit(event);
                break;
            }
            }
        }

        // Move to next notification
        const uint32 next = info->NextEntryOffset;
        if (next == 0)
        {
            break;
        }

        // Validate next offset stays within the returned buffer and makes forward progress.
        if ((next % 4) != 0)
        {
            Logger::Log::Warning("Malformed FILE_NOTIFY_INFORMATION (NextEntryOffset={} not 4-byte aligned): ignoring remaining data", next);
            break;
        }
        if (next < recordSize)
        {
            Logger::Log::Warning("Malformed FILE_NOTIFY_INFORMATION (NextEntryOffset={} < recordSize={}): ignoring remaining data", next, recordSize);
            break;
        }
        if (ptr + next > end)
        {
            Logger::Log::Warning("Malformed FILE_NOTIFY_INFORMATION (NextEntryOffset={} exceeds remaining={}): ignoring remaining data",
                                 next, static_cast<size_t>(end - ptr));
            break;
        }

        ptr += next;
    }
}
#endif

void FileWatcher::ForEachWatchedFile(const Function<void(const std::filesystem::directory_entry&)>& visit)
{
    if (m_Recursive)
    {
        std::filesystem::recursive_directory_iterator iter(m_WatchDirectory);
        const std::filesystem::recursive_directory_iterator end;
        for (; iter != end; ++iter)
        {
            const auto& entry = *iter;
            if (entry.is_directory() && ShouldExcludeDirectory(entry.path()))
            {
                iter.disable_recursion_pending();
                continue;
            }
            if (entry.is_regular_file() && ShouldWatchFile(entry.path()))
                visit(entry);
        }
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(m_WatchDirectory))
    {
        if (entry.is_regular_file() && ShouldWatchFile(entry.path()))
            visit(entry);
    }
}

void FileWatcher::WatchPolling()
{
    Logger::Log::Debug("Using polling-based file watching");

    try
    {
        ForEachWatchedFile([this](const std::filesystem::directory_entry& entry) {
            m_PolledFiles[entry.path()] = PolledFile{entry.last_write_time(), ReadFileIdentity(entry.path())};
        });
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        Logger::Log::Error("Error during initial file scan: {}", e.what());
        return;
    }

    while (!m_ShouldStop.load())
    {
        std::error_code ec;
        const bool present = std::filesystem::exists(m_WatchDirectory, ec) && !ec;
        if (!present)
        {
            // A missing root is a state, not an error: an asset mount can be
            // deleted and re-staged as a unit. Pause the poll so the walk does
            // not throw every tick, and keep m_PolledFiles so the tree is diffed
            // against it when the root returns rather than replayed as new.
            if (!m_WatchDirectoryMissing)
            {
                m_WatchDirectoryMissing = true;
                Logger::Log::Warning("FileWatcher: '{}' is gone; watching pauses until it returns",
                                     m_WatchDirectory.string());
            }
        }
        else
        {
            if (m_WatchDirectoryMissing)
            {
                m_WatchDirectoryMissing = false;
                Logger::Log::Info("FileWatcher: '{}' is back; watching resumes", m_WatchDirectory.string());
            }
            try
            {
                PollOnce();
            }
            catch (const std::filesystem::filesystem_error& e)
            {
                // The root can vanish between the check above and the walk;
                // report only a genuine error, not that narrow race.
                if (std::filesystem::exists(m_WatchDirectory))
                    Logger::Log::Error("Error during file watching: {}", e.what());
            }
        }

        WaitForNextPoll();
    }
}

void FileWatcher::WaitForNextPoll()
{
    // Wait in debounce-sized slices so pending debounced events still fire on
    // time, and cut the wait short as soon as a platform change signal arrives.
    // The poll interval stays the upper bound, so a platform with no signal —
    // or one that drops an event — behaves exactly as it did before.
    uint32 remainingMs = m_PollingInterval;
    while (remainingMs > 0 && !m_ShouldStop.load())
    {
        const uint32 sliceMs = std::min(remainingMs, kWatchLoopSliceMs);
        bool woken = false;
        {
            std::unique_lock<std::mutex> lock(m_WakeMutex);
            woken = m_WakeCv.wait_for(lock, std::chrono::milliseconds(sliceMs),
                                      [this] { return m_WakeRequested; });
            m_WakeRequested = false;
        }
        ProcessReadyDebouncedEvents();
        if (woken)
            return;
        remainingMs -= sliceMs;
    }
}

void FileWatcher::PollOnce()
{
    HashMap<std::filesystem::path, PolledFile> current;
    // Paths seen for the first time in this scan, keyed by identity. Their
    // Created events wait until the scan is complete: one that carries the
    // identity of a path that vanished in the same scan is that file's new
    // name, not a new file.
    HashMap<FileIdentity, std::filesystem::path> appearedByIdentity;
    std::vector<std::filesystem::path> appeared;

    ForEachWatchedFile([&](const std::filesystem::directory_entry& entry) {
        const auto lastWrite = entry.last_write_time();
        const auto previous = m_PolledFiles.find(entry.path());
        if (previous == m_PolledFiles.end())
        {
            PolledFile polled{lastWrite, ReadFileIdentity(entry.path())};
            if (polled.Identity.Valid)
                appearedByIdentity.emplace(polled.Identity, entry.path());
            appeared.push_back(entry.path());
            current[entry.path()] = std::move(polled);
            return;
        }

        if (previous->second.LastWrite == lastWrite)
        {
            current[entry.path()] = previous->second;
            return;
        }

        current[entry.path()] = PolledFile{lastWrite, ReadFileIdentity(entry.path())};
        FileChangeEvent event;
        event.Path = entry.path();
        event.Type = FileChangeType::Modified;
        event.Timestamp = lastWrite;
        NotifyFileChange(event);
    });

    // Every path the previous scan saw that is gone now. Its identity showing
    // up under a new path in this scan, with the timestamp it had, is a
    // rename; anything else was deleted. The timestamp is part of the match
    // because an inode is not unique over time: ext4 and xfs hand a deleted
    // file's inode to the next file created, so within one poll "delete A,
    // create B" can present B under A's identity. rename() preserves mtime
    // and a new file has a new one, so a mismatch falls through to
    // Deleted + Created; a rename followed by an edit inside one poll takes
    // that path too, which is what every poll reported before pairing.
    for (const auto& [path, polled] : m_PolledFiles)
    {
        if (current.find(path) != current.end())
            continue;

        FileChangeEvent event;
        auto renamedTo = polled.Identity.Valid ? appearedByIdentity.find(polled.Identity)
                                               : appearedByIdentity.end();
        if (renamedTo != appearedByIdentity.end() &&
            current[renamedTo->second].LastWrite != polled.LastWrite)
        {
            renamedTo = appearedByIdentity.end();
        }
        if (renamedTo != appearedByIdentity.end())
        {
            event.Path = renamedTo->second;
            event.OldPath = path;
            event.Type = FileChangeType::Renamed;
            event.Timestamp = current[renamedTo->second].LastWrite;
            appeared.erase(std::find(appeared.begin(), appeared.end(), renamedTo->second));
            appearedByIdentity.erase(renamedTo);
            Logger::Log::Debug("FileWatcher: '{}' -> '{}' paired as a rename by file identity",
                               event.OldPath.string(), event.Path.string());
        }
        else
        {
            event.Path = path;
            event.Type = FileChangeType::Deleted;
            event.Timestamp = std::filesystem::file_time_type::clock::now();
        }
        NotifyFileChange(event);
    }

    for (const auto& path : appeared)
    {
        FileChangeEvent event;
        event.Path = path;
        event.Type = FileChangeType::Created;
        event.Timestamp = current[path].LastWrite;
        NotifyFileChange(event);
    }

    m_PolledFiles = std::move(current);
}

void FileWatcher::NotifyFileChangeDebounced(const FileChangeEvent& event)
{
    if (!m_Callback)
        return;

    const auto now = std::chrono::steady_clock::now();

    // Hold Deleted events instead of firing them: a replace-rename (or a
    // delete-then-rewrite save) makes the path reappear within the hold
    // window, and tearing down the asset's registration in between would lose
    // its identity. ProcessReadyDebouncedEvents fires the survivors.
    if (event.Type == FileChangeType::Deleted)
    {
        m_PendingEvents.erase(event.Path);
        m_LastEventTimes[event.Path] = now;
        m_PendingDeletedEvents[event.Path] = {event, now + kDeletedEventHold};
        return;
    }

    // Preserve semantics for Created/Renamed and ensure oldPath isn't lost:
    // only debounce Modified events.
    if (event.Type != FileChangeType::Modified)
    {
        // The path reappeared — this was a replacement, not a deletion.
        if (m_PendingDeletedEvents.erase(event.Path) > 0)
        {
            Logger::Log::Debug("FileWatcher: '{}' reappeared before its Deleted fired "
                               "(replace-rename); suppressing the delete",
                               event.Path.string());
        }

        // Drop any pending debounced Modified event for this path (and oldPath for rename)
        m_PendingEvents.erase(event.Path);
        if (!event.OldPath.empty())
        {
            m_PendingEvents.erase(event.OldPath);
        }

        m_LastEventTimes[event.Path] = now;
        if (!event.OldPath.empty())
        {
            m_LastEventTimes[event.OldPath] = now;
        }

        m_Callback(event);
        return;
    }

    // A Modified event also proves the path exists again (Created records can
    // be coalesced away) — cancel any held Deleted for it.
    if (m_PendingDeletedEvents.erase(event.Path) > 0)
    {
        Logger::Log::Debug("FileWatcher: '{}' reappeared before its Deleted fired "
                           "(replace-rename); suppressing the delete",
                           event.Path.string());
    }

    // Debounce Modified events to prevent multiple triggers.
    bool shouldSchedule = false;
    bool shouldFireNow = false;
    auto& lastEventTime = m_LastEventTimes[event.Path];
    if (now - lastEventTime < DEBOUNCE_DELAY)
    {
        shouldSchedule = true;
    }
    else
    {
        lastEventTime = now;
        shouldFireNow = true;
    }

    if (shouldSchedule)
    {
        Logger::Log::Debug("Debouncing file change event for: {} (too soon)", event.Path.string());
        m_PendingEvents[event.Path] = now + DEBOUNCE_DELAY;
        return;
    }

    if (shouldFireNow)
    {
        Logger::Log::Debug("File change event passed debounce check: {}", event.Path.string());
        m_Callback(event);
    }
}

void FileWatcher::ProcessReadyDebouncedEvents()
{
    if (m_PendingEvents.empty() && m_PendingDeletedEvents.empty())
        return;

    auto now = std::chrono::steady_clock::now();
    std::vector<std::filesystem::path> readyEvents;

    for (auto it = m_PendingEvents.begin(); it != m_PendingEvents.end();)
    {
        if (now >= it->second)
        {
            readyEvents.push_back(it->first);
            it = m_PendingEvents.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (const auto& path : readyEvents)
    {
        if (m_Callback && ShouldWatchFile(path))
        {
            FileChangeEvent event;
            event.Path = path;
            event.Type = FileChangeType::Modified;
            event.Timestamp = std::filesystem::file_time_type::clock::now();

            Logger::Log::Debug("Firing debounced file change event: {}", path.string());

            m_LastEventTimes[path] = now;
            m_Callback(event);
        }
    }

    // Held Deleted events that survived the hold window are real deletions.
    std::vector<FileChangeEvent> readyDeleted;
    for (auto it = m_PendingDeletedEvents.begin(); it != m_PendingDeletedEvents.end();)
    {
        if (now >= it->second.Deadline)
        {
            readyDeleted.push_back(it->second.Event);
            it = m_PendingDeletedEvents.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (const auto& event : readyDeleted)
    {
        if (m_Callback && ShouldWatchFile(event.Path))
        {
            Logger::Log::Debug("Firing held Deleted event: {}", event.Path.string());
            m_LastEventTimes[event.Path] = now;
            m_Callback(event);
        }
    }
}

} // namespace GameEngine
