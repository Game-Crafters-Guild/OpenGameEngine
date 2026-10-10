#pragma once

#include "Thumbnails/AssetThumbnailHandler.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine
{

// Generates downscaled PNG thumbnails for texture assets, so the asset browser
// and search results bind a small image instead of the full-resolution source.
//
// Threading: GetOrRequest runs on its caller's thread — the UI thread, both for
// direct panel requests and for the asset browser's deferred request pump — and
// does no more there than two filesystem timestamp reads. Decode, downscale and PNG
// encode run as JobSystem background jobs, at most kMaxDecodesInFlight at a
// time, and `onReady` is invoked from the job (same contract as
// VideoThumbnailHandler). The handler owns no threads.
class TextureThumbnailHandler : public AssetThumbnailHandler
{
public:
    explicit TextureThumbnailHandler(JobSystem::WorkStealingThreadPool& jobSystem);
    ~TextureThumbnailHandler() override;

    void SetCacheRoot(const std::filesystem::path& root) override;

    // Drops every cached decision and every queued job. The paths, and the
    // cache root they were keyed against, belong to the project being closed.
    void ResetForProjectSwitch() override;

    std::string GetOrRequest(const std::filesystem::path& assetPath,
                             int desiredSize,
                             std::function<void(const std::string& relPath)> onReady,
                             bool StaticModelListThumbnail = false) override;

    // Longest edge of a generated thumbnail, in pixels. One cached image serves
    // every request this handler accepts; `desiredSize` selects nothing on disk.
    // A request for MORE than this is declined outright rather than answered
    // with fewer pixels than it asked for — the preview surfaces
    // (AssetViewPanel, SceneViewPanel) size their request to the viewport and
    // must keep receiving the full-resolution source through the service's
    // fallback.
    static constexpr int kThumbLongEdge = 512;

    // Longest edge of the image GetOrRequestInlineImage caches: an image shown
    // in a panel at about its display size, in device pixels.
    static constexpr int kInlineImageLongEdge = 1024;

    // An image file shown inline (the AI Assistant's captures): its own cached
    // copy at kInlineImageLongEdge, apart from the tile image GetOrRequest
    // serves, which every other caller keeps. Returns the copy's path when it is
    // current (and calls `onReady` with it); otherwise answers once through
    // `onReady`, from a job or at once: the copy's path, the source itself when
    // it is no larger than kInlineImageLongEdge, or empty when the handler
    // cannot make one (undecodable, past the decode budget, no project).
    std::string GetOrRequestInlineImage(const std::filesystem::path& image,
                                        std::function<void(const std::string& path)> onReady);

    // Largest source this handler will decode, in pixels. Decoding is a whole-
    // image RGBA8 allocation (4 bytes/px, plus the compressed bytes until they
    // are released), and kMaxDecodesInFlight of them run at once while the
    // browser works through a folder's cells. 4096x4096 covers the texture sizes that
    // dominate real projects at a ~270 MB two-decode peak (the decoder holds
    // stb's buffer AND the Vector copy at the assign, ~135 MB each); beyond it the cost
    // grows without bound (a 16K HDRI would be ~1 GB) for an image nobody
    // inspects at thumbnail size. Larger sources keep the raw-source fallback,
    // which is what they resolve to today.
    static constexpr uint64_t kMaxSourcePixels = 4096ull * 4096ull;

    // Cache file name for a source path, keyed on the path alone. Validity is
    // decided by timestamp at request time, not by the name.
    static std::string MakeCacheFileName(const std::filesystem::path& assetPath);

    // True when the engine's CPU image decoder (DecodeImageToRGBA, stb_image)
    // can produce an RGBA8 payload for this extension AND the asset registry
    // routes that extension here. Container formats that decode to GPU blocks
    // (.ktx/.ktx2/.dds) and vector sources (.svg) are excluded: those keep
    // ThumbnailService's raw-source fallback.
    static bool IsDecodableSourceExtension(const std::filesystem::path& assetPath);

    // Number of full source decodes begun since process start. A cache hit, a
    // coalesced request, a declined extension, an oversized source and a
    // sub-thumbnail-size source all leave it unchanged; it is the cheapest
    // signal that this handler is not decoding work it already did.
    static uint64_t GetDecodeAttemptCount();

    // Whether ThumbnailService may hand this source's RAW path to a small tile
    // request when no cache PNG exists (yet). The raw path means the UI decodes
    // and GPU-uploads the source at FULL resolution for a <=128 px tile, and
    // that texture is retained — so it is only acceptable when the source is
    // known small: a decode declined for being thumbnail-sized already.
    // Unsupported formats and pending decodes answer
    // false; the tile keeps its type icon until the PNG callback fires.
    bool AllowsRawSourceFallback(const std::filesystem::path& assetPath) const;

private:
    using ReadyCallback = std::function<void(const std::string& relPath)>;
    using FileTime = std::filesystem::file_time_type;

    // Why a source produced no cache file.
    enum class Outcome
    {
        Written,   // cache file is present and current
        Declined,  // a property of this source revision: undecodable, too many
                   // pixels, or already no larger than a thumbnail
        Retry      // transient: the read raced a writer, or the rename lost and
                   // left no file. Must not be remembered.
    };

    // Which property of the source produced a Declined outcome. SmallSource is
    // the one decline whose raw path is a safe tile stand-in.
    enum class DeclineReason
    {
        SmallSource,
        TooLarge,
        Undecodable,
    };

    struct DeclineEntry
    {
        FileTime Time{};
        DeclineReason Reason = DeclineReason::Undecodable;
    };

    struct Job
    {
        std::filesystem::path SourcePath;
        std::filesystem::path CachePath;
        FileTime SourceTime{};
        // The cached image's long edge: kThumbLongEdge or kInlineImageLongEdge.
        int LongEdge = kThumbLongEdge;
        std::vector<ReadyCallback> Callbacks;
    };

    // The cached copy of `assetPath` at `longEdge` (GetOrRequest's tile image or the inline
    // image): its path when current, else a decode job queued or joined (`*outPending`
    // true, `onReady` called when it ends) or nothing to do (`*outPending` false).
    std::string RequestCachedImage(const std::filesystem::path& assetPath, int longEdge,
                                   ReadyCallback onReady, bool* outPending);
    // The key of a source's job and decline verdict at `longEdge`.
    static std::string MakeJobKey(const std::filesystem::path& assetPath, int longEdge);
    // The source was declined at `longEdge` for being no larger than it.
    bool IsSmallSource(const std::filesystem::path& assetPath, int longEdge) const;

    // Decode `sourcePath`, downscale to `longEdge` and write `cachePath` as
    // PNG. On Declined, `outDeclineReason` (never null) reports why.
    static Outcome GenerateThumbnail(const std::filesystem::path& sourcePath,
                                     const std::filesystem::path& cachePath,
                                     int longEdge,
                                     DeclineReason* outDeclineReason);

    // Publishes one decode job into the pool's background lane. The job body is
    // DecodeNextQueuedThumbnail, which decodes the queue's head and then either chains
    // another job for the next source or releases its slot.
    void EnqueueDecodeJob();
    void DecodeNextQueuedThumbnail();
    void ReleaseDecodeSlotLocked();

    // Decode is CPU- and I/O-bound and shares the pool with everything else the
    // editor defers; two concurrent decodes keep a cold project draining
    // without claiming every worker for thumbnails.
    static constexpr size_t kMaxDecodesInFlight = 2;

    JobSystem::WorkStealingThreadPool& m_JobSystem;

    mutable std::mutex m_Mutex;
    std::filesystem::path m_CacheRoot;
    std::deque<std::string> m_Queue;            // keys awaiting a decode, FIFO
    std::unordered_map<std::string, Job> m_Jobs; // key -> queued or running job
    // Sources this handler has decided not to thumbnail, against the source
    // timestamp that produced the decision. Without it a corrupt or oversized
    // file would be re-read on every navigation, since a decline writes no cache
    // file to short-circuit the next request; with the timestamp, editing the
    // source reopens the decision instead of freezing it for the session.
    std::unordered_map<std::string, DeclineEntry> m_Declined;
    // Decode jobs published and not yet returned, bounded by
    // kMaxDecodesInFlight. Counted under m_Mutex together with the queue, so a
    // source pushed while a job is deciding whether to exit is never stranded.
    size_t m_DecodesInFlight = 0;
    std::condition_variable m_Idle; // signalled when m_DecodesInFlight reaches 0
    bool m_Stopping = false;
};

} // namespace GameEngine
