#include "Assets/FileWatchingService.h"
#include "Logger/Logger.h"
#include "Platform/Capabilities.h"
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

namespace GameEngine {

namespace
{
// Subscription whose callback this thread is running, so Unsubscribe called
// from inside that callback does not wait for its own invocation. Only watcher
// threads dispatch, and a dispatch never nests, so one id per thread suffices.
thread_local FileWatchSubscription::SubscriptionId t_InvokingSubscription =
    FileWatchSubscription::kInvalidSubscriptionId;

static std::filesystem::path MakeAbsoluteLexical(const std::filesystem::path& p)
{
    if (p.empty())
        return {};
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(p, ec);
    if (ec)
        abs = p;
    return abs.lexically_normal();
}

/// Normalize a directory path into a stable map key.
/// On Windows, file systems are case-insensitive, so multiple callers may pass
/// the same physical directory with different casing (e.g. "C:\Dev\..." vs
/// "c:\dev\...").  Using a case-folded key avoids creating duplicate OS
/// watchers and the resulting event storms.
static std::string NormalizeWatcherKey(const std::filesystem::path& directory)
{
    std::filesystem::path abs = MakeAbsoluteLexical(directory);
    std::string key = abs.string();
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return key;
}

static std::string NormalizeExtensionLower(std::string ext)
{
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!ext.empty() && ext[0] != '.')
        ext.insert(ext.begin(), '.');
    return ext;
}

/// Absolute + lexically-normal form, case-folded on Windows. Used for
/// cached comparison paths (FilePattern::NormalizedDirectory, and the
/// one-shot normalization of event paths in OnFileChanged).
static std::filesystem::path NormalizeForMatching(const std::filesystem::path& p)
{
    if (p.empty())
        return {};
    std::filesystem::path abs = MakeAbsoluteLexical(p);
#ifdef _WIN32
    std::string s = abs.string();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::filesystem::path(std::move(s));
#else
    return abs;
#endif
}

/// Is `ancestor` a strict ancestor of `descendant` (purely lexical)?
/// Both paths must be pre-normalized to absolute-lexical form.
///
/// On Windows, paths are case-insensitive: `/Assets/` must be recognized as
/// an ancestor of `/assets/Sub/`. `lexically_relative` is case-sensitive, so
/// we case-fold both inputs on Windows to match NormalizeWatcherKey's
/// behavior — otherwise mixed-case subscriptions would bypass both reuse
/// and consolidation and silently create duplicate OS watchers.
static bool IsStrictAncestor(const std::filesystem::path& ancestor,
                             const std::filesystem::path& descendant)
{
#ifdef _WIN32
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::filesystem::path ancLower(toLower(ancestor.string()));
    const std::filesystem::path descLower(toLower(descendant.string()));
    const auto rel = descLower.lexically_relative(ancLower);
#else
    const auto rel = descendant.lexically_relative(ancestor);
#endif
    const auto s = rel.generic_string();
    // Empty or ".." prefix means descendant is outside/equal-to ancestor.
    // "." means equal — not a STRICT ancestor.
    if (s.empty() || s == ".")
        return false;
    return s.rfind("..", 0) != 0;
}
} // namespace

// FilePattern Implementation
FilePattern::FilePattern(const std::filesystem::path& dir,
                         const std::string& Pattern,
                         const std::unordered_set<std::string>& Exts,
                         bool Rec)
    : Directory(dir)
    , NormalizedDirectory(NormalizeForMatching(dir))
    , FilenamePattern(Pattern)
    , Extensions(Exts)
    , Recursive(Rec) {}

bool FilePattern::Matches(const std::filesystem::path& filePath) const {
    // Fallback slow path: caller handed us a raw path, so we pay the
    // normalization cost here. In FileWatchingService's hot event-dispatch
    // path the caller normalizes once and invokes MatchesNormalized below.
    return MatchesNormalized(NormalizeForMatching(filePath));
}

bool FilePattern::MatchesNormalized(const std::filesystem::path& absFile) const {
    // Purely-lexical containment test against the pre-normalized directory.
    // Never use std::filesystem::relative(): it may consult the filesystem
    // (canonicalization) and can fail/throw when either path does not exist
    // — common during atomic-save flows (temp file + rename).
    if (absFile.empty() || NormalizedDirectory.empty())
        return false;

    const std::filesystem::path relativePath = absFile.lexically_relative(NormalizedDirectory);
    const std::string relStr = relativePath.generic_string();
    if (relStr.empty() || relStr == "." || relStr.rfind("..", 0) == 0) {
        return false; // File is outside the directory (or cannot be expressed relative)
    }

    // Check recursion
    if (!Recursive && relativePath.has_parent_path()) {
        return false; // File is in subdirectory but recursion is disabled
    }

    // Check extension filter (uses original filePath casing would differ but
    // extensions are already case-folded in Extensions set).
    if (!Extensions.empty()) {
        std::string ext = NormalizeExtensionLower(absFile.extension().string());
        if (Extensions.find(ext) == Extensions.end() &&
            Extensions.find(ext.empty() ? ext : ext.substr(1)) == Extensions.end()) {
            return false;
        }
    }

    // Check filename pattern
    std::string filename = absFile.filename().string();
    if (!std::regex_match(filename, FilenamePattern)) {
        return false;
    }

    return true;
}

// FileWatchSubscription Implementation
FileWatchSubscription::FileWatchSubscription(SubscriptionId id, FileWatchingService* service)
    : m_Id(id), m_Service(service) {
}

FileWatchSubscription::~FileWatchSubscription() {
    if (m_Service) {
        m_Service->Unsubscribe(m_Id);
    }
}

FileWatchSubscription::FileWatchSubscription(FileWatchSubscription&& other) noexcept
    : m_Id(other.m_Id), m_Service(other.m_Service) {
    other.m_Service = nullptr;
}

FileWatchSubscription& FileWatchSubscription::operator=(FileWatchSubscription&& other) noexcept {
    if (this != &other) {
        if (m_Service) {
            m_Service->Unsubscribe(m_Id);
        }
        m_Id = other.m_Id;
        m_Service = other.m_Service;
        other.m_Service = nullptr;
    }
    return *this;
}

// FileWatchingService Implementation
FileWatchingService& FileWatchingService::GetInstance() {
    // NOTE: intentionally leaky singleton.
    //
    // FileWatchSubscription instances are owned by subsystems (AssetManager, ScriptManager, etc.)
    // and can be destroyed late during process shutdown. If the service were a normal function-local
    // static, its destructor could run before those subscriptions, leading to use-after-free when
    // a subscription destructor calls Unsubscribe().
    //
    // Keeping the service alive for the lifetime of the process avoids shutdown-order hazards and
    // makes subscription teardown deterministic.
    static FileWatchingService* instance = new FileWatchingService();
    return *instance;
}

FileWatchSubscription FileWatchingService::Subscribe(const FilePattern& pattern,
                                                    FileChangeCallback callback,
                                                    int Priority) {
    // An empty directory matches no file (MatchesNormalized rejects it), and a
    // watcher created for one can never start. Refuse it here so the caller's
    // mistake surfaces once, named, instead of as a permanently dead
    // subscription plus a "Directory does not exist" error from FileWatcher.
    if (pattern.Directory.empty()) {
        std::vector<std::string> extensions(pattern.Extensions.begin(), pattern.Extensions.end());
        std::sort(extensions.begin(), extensions.end());
        std::string extensionList;
        for (const auto& extension : extensions)
            extensionList += (extensionList.empty() ? "" : ", ") + extension;

        Logger::Log::Warning("FileWatchingService: refusing subscription with an empty directory "
                             "(extensions: [{}], recursive: {}); pass the directory to watch",
                             extensionList, pattern.Recursive);
        return FileWatchSubscription(FileWatchSubscription::kInvalidSubscriptionId, nullptr);
    }

    // Absorbed watchers from consolidation are stopped AFTER releasing the
    // mutex — StopWatching joins the watcher thread, which may be blocked
    // in OnFileChanged waiting for the same mutex we'd be holding.
    std::vector<std::unique_ptr<DirectoryWatcher>> deferredStops;
    SubscriptionId id = 0;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        id = m_NextSubscriptionId++;

        auto subscription = std::shared_ptr<Subscription>(new Subscription{
            .Id = id,
            .Pattern = pattern,
            .Callback = std::move(callback),
            .Priority = Priority,
            .Active = true
        });

        m_Subscriptions[id] = std::move(subscription);
        m_Stats.TotalSubscriptions++;

        // Get or create watcher for this directory. May reuse an ancestor's
        // recursive watcher or consolidate existing descendant watchers under
        // a new one — OwningWatcherKey records the actual owner for cleanup.
        std::string ownerKey;
        auto* watcher = GetOrCreateWatcher(pattern.Directory, pattern.Recursive,
                                           ownerKey, deferredStops);
        if (watcher) {
            watcher->Subscriptions.push_back(id);
            if (auto it = m_Subscriptions.find(id); it != m_Subscriptions.end()) {
                it->second->OwningWatcherKey = std::move(ownerKey);
            }
            // If the service is already watching, ensure newly created watchers are started immediately.
            if (m_Watching && watcher->Watcher && !watcher->Watcher->IsWatching())
            {
                if (watcher->Watcher->StartWatching(watcher->Directory, watcher->Recursive))
                {
                    m_Stats.ActiveWatchers = static_cast<uint32>(m_Watchers.size());
                }
            }
        }

        Logger::Log::Debug("FileWatchingService: Subscription {} created for directory: {}",
                    id, pattern.Directory.string());

        CollectWatchersToStop(deferredStops);
    }

    // Stop absorbed watchers outside the lock. Their threads may still deliver
    // events through OnFileChanged until the join — harmless, since their
    // subscriptions have already been rehomed under the lock.
    for (auto& w : deferredStops) {
        w->Watcher->StopWatching();
    }

    return FileWatchSubscription(id, this);
}

bool FileWatchingService::StartWatching() {
    if (!Platform::SupportsExternalFileChanges())
    {
        // The only writer is this process, so there is no external change to
        // discover; the poll this backend would run instead is pure cost.
        // This process's own writes report themselves: see
        // AssetManager::ExpectWrite.
        //
        // True, not false: the service is in the state this platform intends,
        // and a caller that treats false as a failure would warn about a
        // decision rather than a fault. Subscriptions stay registered and
        // inert, so IsWatching() keeps telling the truth about whether events
        // can arrive.
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true))
        {
            Logger::Log::Info("FileWatchingService: filesystem watching is disabled on this "
                              "platform — nothing outside the process can change these files. "
                              "The editor's own writes report themselves through "
                              "AssetManager::ExpectWrite.");
        }
        return true;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);

    if (m_Watching) {
        Logger::Log::Warning("FileWatchingService: Already watching");
        return true;
    }

    Logger::Log::Info("FileWatchingService: Starting centralized file watching for {} directories",
                m_Watchers.size());

    uint32 started = 0;
    uint32 failed = 0;
    for (auto& [path, watcher] : m_Watchers) {
        if (!watcher->Watcher->StartWatching(watcher->Directory, watcher->Recursive)) {
            Logger::Log::Error("FileWatchingService: Failed to start watching directory: {}", path);
            ++failed;
        } else {
            // Per-directory success; the summary below carries the count.
            Logger::Log::Debug("FileWatchingService: Started watching directory: {}", path);
            ++started;
        }
    }

    // Arm on partial success. One unwatchable directory — a source root that
    // could not be created, a disconnected drive — must not switch file
    // watching off for every other subscriber. Failures are named per
    // directory above and counted into the return value.
    m_Watching = true;
    m_Stats.ActiveWatchers = started;

    if (failed == 0) {
        Logger::Log::Info("FileWatchingService: Successfully started watching {} directories",
                    started);
    } else {
        Logger::Log::Warning("FileWatchingService: Watching {} directories; {} failed to start",
                    started, failed);
    }

    return failed == 0;
}

void FileWatchingService::StopWatching() {
    // Flip m_Watching under the lock so OnFileChanged stops dispatching to
    // subscribers, then join the watcher threads OUTSIDE m_Mutex: a watcher
    // thread may be blocked on m_Mutex inside OnFileChanged, and joining it
    // while holding the lock deadlocks (same hazard as the deferred-stop notes
    // on GetOrCreateWatcher/Unsubscribe). Watchers stay in m_Watchers so
    // StartWatching can re-arm them.
    std::vector<FileWatcher*> toJoin;
    std::vector<std::unique_ptr<DirectoryWatcher>> retired;
    bool wasWatching = false;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        CollectWatchersToStop(retired);

        wasWatching = m_Watching;
        if (wasWatching) {
            Logger::Log::Info("FileWatchingService: Stopping centralized file watching");
            m_Watching = false;
            m_Stats.ActiveWatchers = 0;

            toJoin.reserve(m_Watchers.size());
            for (auto& [path, watcher] : m_Watchers) {
                toJoin.push_back(watcher->Watcher.get());
            }
        }
    }

    // After these joins return, no subscription callback can be in flight or
    // start again — the watcher threads are the only dispatchers.
    for (auto& w : retired) {
        w->Watcher->StopWatching();
    }
    for (FileWatcher* watcher : toJoin) {
        watcher->StopWatching();
    }
    if (wasWatching) {
        Logger::Log::Info("FileWatchingService: File watching stopped");
    }
}

bool FileWatchingService::IsWatching() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Watching;
}

FileWatchingService::WatchingStats FileWatchingService::GetStats() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    WatchingStats stats = m_Stats;
    stats.TotalDirectories = static_cast<uint32>(m_Watchers.size());

    // Calculate average event processing time
    if (!m_EventProcessingTimes.empty()) {
        auto totalTime = std::chrono::milliseconds(0);
        for (const auto& time : m_EventProcessingTimes) {
            totalTime += time;
        }
        stats.AverageEventProcessingTime = totalTime / m_EventProcessingTimes.size();
    }
    
    return stats;
}

void FileWatchingService::Unsubscribe(SubscriptionId id) {
    // Same deferred-stop pattern as Subscribe: StopWatching joins the
    // watcher thread, which may be blocked in OnFileChanged on m_Mutex.
    std::vector<std::unique_ptr<DirectoryWatcher>> toStop;
    // Destroyed after the lock is released, on this thread.
    FileChangeCallback releasedCallback;
    {
        std::unique_lock<std::mutex> lock(m_Mutex);

        auto it = m_Subscriptions.find(id);
        if (it == m_Subscriptions.end()) {
            Logger::Log::Warning("FileWatchingService: Attempted to unsubscribe unknown subscription: {}", id);
            return;
        }

        const std::shared_ptr<Subscription> subscription = it->second;

        // Fall back to the pattern directory for robustness, though
        // OwningWatcherKey should always be populated.
        std::string ownerKey = subscription->OwningWatcherKey;
        if (ownerKey.empty())
            ownerKey = NormalizeWatcherKey(subscription->Pattern.Directory);

        m_Subscriptions.erase(it);
        m_Stats.TotalSubscriptions--;

        auto watcherIt = m_Watchers.find(ownerKey);
        if (watcherIt != m_Watchers.end()) {
            auto& subscriptions = watcherIt->second->Subscriptions;
            subscriptions.erase(std::remove(subscriptions.begin(), subscriptions.end(), id),
                                subscriptions.end());

            // Remove watcher if no subscriptions remain — hand the handle off
            // to be stopped after releasing the lock.
            if (subscriptions.empty()) {
                toStop.push_back(std::move(watcherIt->second));
                m_Watchers.erase(watcherIt);
                Logger::Log::Debug("FileWatchingService: Removed empty watcher: {}", ownerKey);
            }
        }

        // A dispatch that already holds this subscription checks Active before
        // each call, so no call starts from here on. Wait for the calls already
        // running on other watcher threads to return. Called from inside this
        // subscription's own callback, do not wait for that invocation: it is
        // further up this thread's stack and can only return after we do.
        subscription->Active = false;
        const uint32 ownCalls = t_InvokingSubscription == id ? 1u : 0u;
        m_CallbackFinished.wait(lock, [&subscription, ownCalls] {
            return subscription->ActiveCalls == ownCalls;
        });
        // A callback still running on this thread must outlive its own call;
        // the dispatch releases it with its snapshot instead.
        if (ownCalls == 0)
            releasedCallback = std::move(subscription->Callback);

        Logger::Log::Debug("FileWatchingService: Unsubscribed subscription: {}", id);

        CollectWatchersToStop(toStop);
    }

    for (auto& w : toStop) {
        w->Watcher->StopWatching();
    }
}

void FileWatchingService::CollectWatchersToStop(std::vector<std::unique_ptr<DirectoryWatcher>>& leaving) {
    // Subscribe and Unsubscribe run on a watcher thread only from inside a
    // callback, which is exactly when t_InvokingSubscription is set.
    if (t_InvokingSubscription != FileWatchSubscription::kInvalidSubscriptionId) {
        for (auto& w : leaving) {
            w->Watcher->RequestStop();
            m_RetiredWatchers.push_back(std::move(w));
        }
        leaving.clear();
        return;
    }

    for (auto& w : m_RetiredWatchers) {
        leaving.push_back(std::move(w));
    }
    m_RetiredWatchers.clear();
}

void FileWatchingService::OnFileChanged(const FileChangeEvent& event) {
    auto startTime = std::chrono::steady_clock::now();

    // Normalize the event path once instead of re-normalizing inside each
    // subscription's Matches call. Subscriptions match against their
    // cached NormalizedDirectory via MatchesNormalized.
    const std::filesystem::path normalizedEventPath = NormalizeForMatching(event.Path);

    // Snapshot the matching subscriptions under the lock and invoke them
    // outside it, so a callback can subscribe or unsubscribe without deadlock.
    // The snapshot keeps each Subscription alive, not callable: see Active.
    std::vector<std::shared_ptr<Subscription>> matchingSubscriptions;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        // If service is not watching, ignore incoming events
        if (!m_Watching) {
            return;
        }

        for (auto& [id, subscription] : m_Subscriptions) {
            if (subscription->Active && subscription->Pattern.MatchesNormalized(normalizedEventPath)) {
                matchingSubscriptions.push_back(subscription);
            }
        }

        if (matchingSubscriptions.empty()) {
            m_Stats.EventsFiltered++;
            return;
        }

        std::sort(matchingSubscriptions.begin(), matchingSubscriptions.end(),
                   [](const std::shared_ptr<Subscription>& a, const std::shared_ptr<Subscription>& b) {
                       return a->Priority > b->Priority;
                   });
    }
    const size_t matchedCount = matchingSubscriptions.size();

    for (const auto& subscription : matchingSubscriptions) {
        // An earlier callback in this loop, or another thread, may have
        // unsubscribed it since the snapshot. Only that subscription is
        // skipped; the rest of the loop still runs.
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (!subscription->Active) {
                continue;
            }
            ++subscription->ActiveCalls;
        }

        t_InvokingSubscription = subscription->Id;
        try {
            if (subscription->Callback) {
                subscription->Callback(event);
            }
        } catch (const std::exception& e) {
            Logger::Log::Error("FileWatchingService: Exception in subscription callback: {}", e.what());
        }
        t_InvokingSubscription = FileWatchSubscription::kInvalidSubscriptionId;

        bool unsubscribing = false;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            --subscription->ActiveCalls;
            unsubscribing = !subscription->Active;
        }
        // Unsubscribe clears Active before it waits, so a waiter can only
        // exist once Active is false.
        if (unsubscribing) {
            m_CallbackFinished.notify_all();
        }
    }

    // Update stats after processing
    auto endTime = std::chrono::steady_clock::now();
    auto processingTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Stats.EventsProcessed++;
        m_EventProcessingTimes.push_back(processingTime);
        if (m_EventProcessingTimes.size() > 100) {
            m_EventProcessingTimes.erase(m_EventProcessingTimes.begin());
        }
    }

    Logger::Log::Debug("FileWatchingService: Processed file change event for {} ({} subscriptions, {}ms)",
                       event.Path.string(), matchedCount, processingTime.count());
}

FileWatchingService::DirectoryWatcher* FileWatchingService::GetOrCreateWatcher(
    const std::filesystem::path& directory, bool recursive,
    std::string& outOwnerKey,
    std::vector<std::unique_ptr<DirectoryWatcher>>& outDeferredStops) {

    const std::string key = NormalizeWatcherKey(directory);

    // 1) Exact-match reuse — same directory, same OS watcher.
    if (auto it = m_Watchers.find(key); it != m_Watchers.end()) {
        outOwnerKey = it->first;
        return it->second.get();
    }

    // 2) Ancestor reuse — if a recursive watcher already covers this
    //    directory, reuse it. FilePattern::Matches per subscription filters
    //    events to the subscription's scope, so sharing one OS watcher
    //    across subscribers with overlapping subtrees is safe.
    const std::filesystem::path newAbs = MakeAbsoluteLexical(directory);
    for (auto& [existingKey, w] : m_Watchers) {
        if (!w->Recursive) continue;
        const std::filesystem::path existingAbs = MakeAbsoluteLexical(w->Directory);
        if (IsStrictAncestor(existingAbs, newAbs)) {
            Logger::Log::Debug("FileWatchingService: Reusing ancestor watcher '{}' for directory: {}",
                               existingKey, directory.string());
            outOwnerKey = existingKey;
            return w.get();
        }
    }

    // 3) Consolidation — if the new subscription is recursive, any existing
    //    watchers strictly under this directory become redundant: their
    //    subscriptions move to the new wider watcher and their OS handles
    //    are deferred for shutdown outside the lock (StopWatching joins the
    //    watcher thread, which may be blocked in OnFileChanged waiting for
    //    the same mutex we hold).
    std::vector<std::string> toAbsorb;
    if (recursive) {
        for (auto& [existingKey, w] : m_Watchers) {
            const std::filesystem::path existingAbs = MakeAbsoluteLexical(w->Directory);
            if (IsStrictAncestor(newAbs, existingAbs)) {
                toAbsorb.push_back(existingKey);
            }
        }
    }

    // 4) Create the new watcher.
    auto watcher = std::make_unique<DirectoryWatcher>();
    watcher->Watcher = std::make_unique<FileWatcher>();
    watcher->Directory = directory;
    watcher->Recursive = recursive;
    // The only write of the callback slot: here, before the thread exists.
    // It is never cleared; a watcher leaves the service by StopWatching.
    watcher->Watcher->SetCallback([this](const FileChangeEvent& event) {
        OnFileChanged(event);
    });

    // 5) Move subscriptions from absorbed watchers onto the new watcher, and
    //    hand the absorbed watchers' unique_ptrs off to the caller to stop
    //    outside the lock.
    std::vector<SubscriptionId> rehomed;
    for (const auto& k : toAbsorb) {
        auto it = m_Watchers.find(k);
        if (it == m_Watchers.end()) continue;
        for (auto id : it->second->Subscriptions) {
            watcher->Subscriptions.push_back(id);
            rehomed.push_back(id);
        }
        outDeferredStops.push_back(std::move(it->second));
        m_Watchers.erase(it);
        Logger::Log::Debug("FileWatchingService: Consolidated watcher '{}' into '{}'",
                           k, key);
    }

    DirectoryWatcher* result = watcher.get();
    m_Watchers[key] = std::move(watcher);

    // 6) Update rehomed subscriptions' OwningWatcherKey so future
    //    Unsubscribes locate them on the consolidated watcher.
    for (SubscriptionId id : rehomed) {
        if (auto sit = m_Subscriptions.find(id); sit != m_Subscriptions.end()) {
            sit->second->OwningWatcherKey = key;
        }
    }

    Logger::Log::Debug("FileWatchingService: Created watcher for directory: {}", directory.string());

    outOwnerKey = key;
    return result;
}


} // namespace GameEngine
