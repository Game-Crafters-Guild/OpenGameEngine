#pragma once

#include "Types/Types.h"
#include "FileWatcher/FileWatcher.h"
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <regex>

namespace GameEngine {

/**
 * @brief File pattern matching for subscriptions.
 *
 * NormalizedDirectory is cached at construction so the per-event Matches path
 * doesn't re-run `absolute() + lexically_normal()` for each subscription on
 * every OS event. Treat Directory as write-once — if it ever needs to change
 * after construction, rebuild the FilePattern.
 */
struct FilePattern {
    std::filesystem::path Directory;
    std::filesystem::path NormalizedDirectory; // absolute + lexically_normal (case-folded on Windows)
    std::regex FilenamePattern;
    std::unordered_set<std::string> Extensions;
    bool Recursive = true;

    FilePattern(const std::filesystem::path& dir,
               const std::string& Pattern = ".*",
               const std::unordered_set<std::string>& Exts = {},
               bool Rec = true);

    /// Test against a raw file path. Normalizes internally — prefer
    /// MatchesNormalized in hot dispatch paths when the caller has already
    /// normalized the event path once across many subscriptions.
    bool Matches(const std::filesystem::path& filePath) const;

    /// Test against an already-normalized file path (absolute + lexically_normal,
    /// case-folded on Windows). Used by FileWatchingService::OnFileChanged after
    /// it normalizes the event path once per event.
    bool MatchesNormalized(const std::filesystem::path& normalizedFilePath) const;
};

/**
 * @brief Subscription handle for file watching
 *
 * Destroying or overwriting a valid handle unsubscribes. Once that returns,
 * the callback is not running and is never called again, so the callback may
 * use anything that outlives the handle. To give that guarantee, unsubscribing
 * waits for a call already running on another watcher thread. Therefore a
 * callback must not block on a thread that can destroy its handle, and two
 * callbacks must not unsubscribe each other. A callback may destroy its own
 * handle; that does not wait for its own call.
 */
class FileWatchSubscription {
public:
    using SubscriptionId = uint64;

    /// Id carried by a handle that was never registered with the service.
    /// Ids issued by FileWatchingService::Subscribe start at 1.
    static constexpr SubscriptionId kInvalidSubscriptionId = 0;

    FileWatchSubscription(SubscriptionId id, class FileWatchingService* service);
    ~FileWatchSubscription();
    
    // Non-copyable, movable
    FileWatchSubscription(const FileWatchSubscription&) = delete;
    FileWatchSubscription& operator=(const FileWatchSubscription&) = delete;
    FileWatchSubscription(FileWatchSubscription&& other) noexcept;
    FileWatchSubscription& operator=(FileWatchSubscription&& other) noexcept;
    
    SubscriptionId GetId() const { return m_Id; }

    /// True while this handle owns a registered subscription. False for a
    /// moved-from handle as well as for one Subscribe refused, so it answers
    /// "is there anything here to unsubscribe", not "was the pattern accepted".
    bool IsValid() const { return m_Service != nullptr; }
    
private:
    SubscriptionId m_Id;
    FileWatchingService* m_Service;
};

/**
 * @brief Centralized file watching service with subscription/observer pattern
 * 
 * This service consolidates multiple file watchers into a single system that
 * multiple components (Asset Manager, Script Manager, etc.) can subscribe to.
 * 
 * Benefits:
 * - Single file watcher per directory (no resource conflicts)
 * - Centralized debouncing and event coordination
 * - Pattern-based subscriptions for flexible filtering
 * - Reduced memory overhead and thread count
 * - Independent file access without coordination overhead
 */
class FileWatchingService {
public:
    using SubscriptionId = FileWatchSubscription::SubscriptionId;
    using FileChangeCallback = std::function<void(const FileChangeEvent&)>;
    
    /**
     * @brief Get the singleton instance
     */
    static FileWatchingService& GetInstance();
    
    /**
     * @brief Subscribe to file changes matching a pattern
     * @param pattern File pattern to match. An empty Directory is refused: it
     *        matches no file, so the call is a programming error rather than a
     *        watch. The refusal is logged once and nothing is registered.
     * @param callback Callback function for file changes
     * @param priority Priority level (higher priority callbacks are called first)
     * @return RAII subscription handle, inert when the pattern was refused.
     *         An inert handle carries kInvalidSubscriptionId; note IsValid()
     *         does not distinguish it from a moved-from handle.
     */
    FileWatchSubscription Subscribe(const FilePattern& pattern,
                                   FileChangeCallback callback,
                                   int Priority = 0);
    
    /**
     * @brief Start watching all subscribed directories.
     *
     * Arms the service even when some directories fail to start, so one
     * unwatchable root cannot disable watching for every other subscriber.
     * Each failure is logged by directory.
     *
     * @return true when every directory started; false when any failed. The
     *         service is watching either way — check IsWatching() for that.
     */
    bool StartWatching();
    
    /**
     * @brief Stop watching all directories.
     * Call StartWatching and StopWatching from one thread (the main thread):
     * StopWatching joins after releasing the lock, so they must not overlap.
     */
    void StopWatching();
    
    /**
     * @brief Check if service is currently watching
     */
    bool IsWatching() const;
    
    /**
     * @brief Get statistics about file watching
     */
    struct WatchingStats {
        uint32 TotalSubscriptions = 0;
        uint32 ActiveWatchers = 0;
        uint32 TotalDirectories = 0;
        uint32 EventsProcessed = 0;
        uint32 EventsFiltered = 0;
        std::chrono::milliseconds AverageEventProcessingTime{0};
    };
    WatchingStats GetStats() const;
    
    // FileAccessCoordinator removed - file watchers now work independently
    
private:
    FileWatchingService() = default;
    ~FileWatchingService() = default;
    
    /**
     * @brief Internal subscription data
     */
    struct Subscription {
        SubscriptionId Id;
        FilePattern Pattern;
        // Written by Subscribe; released by Unsubscribe once no call runs.
        FileChangeCallback Callback;
        int Priority;
        // Guarded by m_Mutex. Unsubscribe clears Active, and a dispatch checks
        // it before each call, so no call starts after that. ActiveCalls
        // counts calls in progress on all watcher threads; Unsubscribe waits
        // on m_CallbackFinished until it drops to zero, or to one when it
        // runs inside the subscription's own callback.
        bool Active = true;
        uint32 ActiveCalls = 0;
        // Normalized key of the DirectoryWatcher that owns this subscription.
        // May differ from Pattern.Directory after subset-based watcher
        // consolidation (e.g. subscription for /Assets/Nav/ may end up on a
        // recursive watcher rooted at /Assets/). Unsubscribe uses this key
        // to locate the owning watcher directly.
        std::string OwningWatcherKey;
    };
    
    /**
     * @brief Directory watcher data
     */
    struct DirectoryWatcher {
        std::unique_ptr<FileWatcher> Watcher;
        std::filesystem::path Directory;
        std::vector<SubscriptionId> Subscriptions;
        bool Recursive;
    };
    
    /**
     * @brief Unsubscribe from file watching (called by subscription destructor).
     * Returns only when the callback is not running on another thread and can
     * never be called again (see FileWatchSubscription).
     */
    void Unsubscribe(SubscriptionId id);
    
    /**
     * @brief Handle file change event from underlying FileWatcher
     */
    void OnFileChanged(const FileChangeEvent& event);
    
    /**
     * @brief Get or create directory watcher.
     *
     * @param outOwnerKey Populated with the normalized map key of the chosen
     *        watcher. Caller stores this on the subscription so Unsubscribe
     *        can find the owning watcher after consolidation (which may move
     *        a subscription onto a different directory's watcher).
     * @param outDeferredStops Absorbed watchers from descendant-consolidation
     *        are moved here. The caller passes them through
     *        CollectWatchersToStop and stops what it returns after releasing
     *        m_Mutex — stopping a watcher joins its thread,
     *        which can block inside OnFileChanged waiting for m_Mutex and
     *        deadlock if we stop under the lock.
     */
    DirectoryWatcher* GetOrCreateWatcher(const std::filesystem::path& directory,
                                         bool recursive,
                                         std::string& outOwnerKey,
                                         std::vector<std::unique_ptr<DirectoryWatcher>>& outDeferredStops);

    /**
     * @brief Decide who stops the watchers leaving the service. Call under m_Mutex.
     *
     * On return, @p leaving holds the watchers the caller must StopWatching
     * after releasing m_Mutex. Inside a callback, on a watcher thread, none:
     * that thread cannot join itself, and joining another watcher could wait
     * on a thread that waits on this one. Those watchers are told to stop and
     * parked in m_RetiredWatchers, and the next caller that is not on a
     * watcher thread takes them.
     */
    void CollectWatchersToStop(std::vector<std::unique_ptr<DirectoryWatcher>>& leaving);

    mutable std::mutex m_Mutex;
    // Shared so a dispatch in progress keeps an unsubscribed entry alive until
    // its loop has passed it.
    std::unordered_map<SubscriptionId, std::shared_ptr<Subscription>> m_Subscriptions;
    // Notified, when a call of an unsubscribing subscription returns, for
    // Unsubscribe's wait. Used with m_Mutex.
    std::condition_variable m_CallbackFinished;
    std::unordered_map<std::string, std::unique_ptr<DirectoryWatcher>> m_Watchers;
    // Removed from inside a callback and asked to stop, not yet joined. Guarded
    // by m_Mutex; see CollectWatchersToStop.
    std::vector<std::unique_ptr<DirectoryWatcher>> m_RetiredWatchers;

    SubscriptionId m_NextSubscriptionId = 1;
    bool m_Watching = false;
    
    // Statistics
    mutable WatchingStats m_Stats;
    std::vector<std::chrono::milliseconds> m_EventProcessingTimes;
    
    friend class FileWatchSubscription;
    
    DISALLOW_COPY_AND_ASSIGN(FileWatchingService);
};

} // namespace GameEngine
