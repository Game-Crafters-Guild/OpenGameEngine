#include "Thumbnails/TextureThumbnailHandler.h"

#include "AssetCore/SharedFileRead.h"
#include "Engine/Rendering/EmbeddedImageDecoder.h"
#include "FileSystem/FileSystem.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "Types/StringUtils.h"

#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <string_view>
#include <utility>

namespace GameEngine
{

namespace
{

std::atomic<uint64_t> g_DecodeAttempts{0};

} // namespace

TextureThumbnailHandler::TextureThumbnailHandler(JobSystem::WorkStealingThreadPool& jobSystem)
    : m_JobSystem(jobSystem)
{
}

TextureThumbnailHandler::~TextureThumbnailHandler()
{
    std::unique_lock<std::mutex> lock(m_Mutex);
    m_Stopping = true;
    m_Queue.clear();
    // Dropping the jobs drops their callbacks: the panels and grids that
    // registered them are torn down alongside the service, so a late
    // invocation would run against freed state.
    m_Jobs.clear();
    // A decode already past its dequeue holds copies of everything it needs and
    // finishes on its own; waiting for it here is what guarantees no callback
    // runs after this destructor returns. Jobs still queued in the pool return
    // as soon as they see m_Stopping, and EnqueueWork never drops one.
    m_Idle.wait(lock, [this] { return m_DecodesInFlight == 0; });
}

void TextureThumbnailHandler::SetCacheRoot(const std::filesystem::path& root)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_CacheRoot = root;
}

void TextureThumbnailHandler::ResetForProjectSwitch()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Queue.clear();
    // A running job keeps its own copy of the paths it was given and finishes
    // against the old project's cache root; erasing the map drops its callbacks,
    // so nothing from the closed project reaches the new project's UI.
    m_Jobs.clear();
    m_Declined.clear();
}

uint64_t TextureThumbnailHandler::GetDecodeAttemptCount()
{
    return g_DecodeAttempts.load(std::memory_order_relaxed);
}

std::string TextureThumbnailHandler::MakeCacheFileName(const std::filesystem::path& assetPath)
{
    std::string key = assetPath.generic_string();
    std::replace(key.begin(), key.end(), '\\', '/');
    const std::size_t h = std::hash<std::string>{}(key);
    return "texthumb_" + std::to_string(h) + ".png";
}

bool TextureThumbnailHandler::IsDecodableSourceExtension(const std::filesystem::path& assetPath)
{
    const auto ext = ToLowerAscii(assetPath.has_extension() ? assetPath.extension().string()
                                                            : std::string());
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
           ext == ".tga" || ext == ".hdr";
}

std::string TextureThumbnailHandler::GetOrRequest(
    const std::filesystem::path& assetPath,
    int desiredSize,
    std::function<void(const std::string& relPath)> onReady,
    bool /*StaticModelListThumbnail*/)
{
    if (!IsDecodableSourceExtension(assetPath))
        return {};

    // A caller asking for more pixels than the cache holds gets the source. The
    // asset preview panes size their request to the viewport (up to 4096), and
    // answering those from a 512-pixel cache would quietly downgrade the surface
    // people inspect textures on.
    if (desiredSize > kThumbLongEdge)
        return {};

    bool pending = false;
    return RequestCachedImage(assetPath, kThumbLongEdge, std::move(onReady), &pending);
}

std::string TextureThumbnailHandler::GetOrRequestInlineImage(const std::filesystem::path& image,
                                                             std::function<void(const std::string& path)> onReady)
{
    // An empty answer from the cache is the source itself when it is no larger than the copy
    // would be, and no image otherwise.
    auto answer = [this, image, onReady = std::move(onReady)](const std::string& cached) {
        if (!cached.empty())
            onReady(cached);
        else
            onReady(IsSmallSource(image, kInlineImageLongEdge) ? image.generic_string() : std::string());
    };
    if (!IsDecodableSourceExtension(image))
    {
        answer({});
        return {};
    }
    bool pending = false;
    std::string cached = RequestCachedImage(image, kInlineImageLongEdge, answer, &pending);
    if (cached.empty() && !pending)
        answer({});
    return cached;
}

std::string TextureThumbnailHandler::MakeJobKey(const std::filesystem::path& assetPath, int longEdge)
{
    // The tile image keeps the bare path as its key.
    return longEdge == kThumbLongEdge ? assetPath.generic_string()
                                      : assetPath.generic_string() + "|" + std::to_string(longEdge);
}

bool TextureThumbnailHandler::IsSmallSource(const std::filesystem::path& assetPath, int longEdge) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto it = m_Declined.find(MakeJobKey(assetPath, longEdge));
    return it != m_Declined.end() && it->second.Reason == DeclineReason::SmallSource;
}

std::string TextureThumbnailHandler::RequestCachedImage(const std::filesystem::path& assetPath, int longEdge,
                                                        ReadyCallback onReady, bool* outPending)
{
    *outPending = false;
    std::filesystem::path cacheRoot;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Stopping)
            return {};
        cacheRoot = m_CacheRoot;
    }

    // No project open yet: keep the raw source rather than writing a cache file
    // somewhere the project doesn't own.
    if (cacheRoot.empty())
        return {};

    // The source stamp decides both whether a cached image is current and
    // whether a previous decline still applies.
    std::error_code ec;
    const FileTime sourceTime = std::filesystem::last_write_time(assetPath, ec);
    if (ec)
        return {};

    const std::string key = MakeJobKey(assetPath, longEdge);
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto declined = m_Declined.find(key);
        if (declined != m_Declined.end())
        {
            if (declined->second.Time == sourceTime)
                return {};
            m_Declined.erase(declined); // the source changed; decide again
        }
    }

    std::string cacheName = MakeCacheFileName(assetPath);
    if (longEdge != kThumbLongEdge)
        cacheName.insert(cacheName.size() - std::string_view(".png").size(), "_" + std::to_string(longEdge));
    const std::filesystem::path cachePath = cacheRoot / cacheName;

    const FileTime cacheTime = std::filesystem::last_write_time(cachePath, ec);
    if (!ec && cacheTime >= sourceTime)
    {
        // Absolute, as the scene and material cache hits in ThumbnailService
        // return — the UI layer accepts both absolute and Assets/-relative paths.
        const std::string abs = cachePath.generic_string();
        if (onReady)
            onReady(abs);
        return abs;
    }

    bool dispatch = false;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Stopping)
            return {};

        auto it = m_Jobs.find(key);
        if (it != m_Jobs.end())
        {
            // Ride the queued or running job instead of decoding the same
            // source twice; the decode collects callbacks when it finishes.
            if (onReady)
                it->second.Callbacks.push_back(std::move(onReady));
            *outPending = true;
            return {};
        }

        Job job;
        job.SourcePath = assetPath;
        job.CachePath = cachePath;
        job.SourceTime = sourceTime;
        job.LongEdge = longEdge;
        if (onReady)
            job.Callbacks.push_back(std::move(onReady));
        m_Jobs.emplace(key, std::move(job));
        m_Queue.push_back(key);

        if (m_DecodesInFlight < kMaxDecodesInFlight)
        {
            ++m_DecodesInFlight;
            dispatch = true;
        }
    }

    // Published outside m_Mutex: in the pool's inline mode the job body runs
    // right here, and it takes that lock.
    *outPending = true;
    if (dispatch)
        EnqueueDecodeJob();

    // Nothing yet — ThumbnailService falls back to the raw source path for this
    // request and swaps in the thumbnail when the callback fires.
    return {};
}

bool TextureThumbnailHandler::AllowsRawSourceFallback(const std::filesystem::path& assetPath) const
{
    // A compressed file's byte size does not bound its decoded dimensions.
    // Keep the type icon for formats we cannot probe; full previews are not
    // subject to this small-tile guard.
    if (!IsDecodableSourceExtension(assetPath))
        return false;

    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto it = m_Declined.find(assetPath.generic_string());
    if (it == m_Declined.end())
        return false; // no verdict yet (decode pending): the PNG callback is coming
    return it->second.Reason == DeclineReason::SmallSource;
}

void TextureThumbnailHandler::EnqueueDecodeJob()
{
    m_JobSystem.EnqueueWork([this] { DecodeNextQueuedThumbnail(); },
                            JobSystem::JobPriority::Background);
}

void TextureThumbnailHandler::ReleaseDecodeSlotLocked()
{
    if (--m_DecodesInFlight == 0)
        m_Idle.notify_all();
}

void TextureThumbnailHandler::DecodeNextQueuedThumbnail()
{
    std::string key;
    std::filesystem::path sourcePath;
    std::filesystem::path cachePath;
    FileTime sourceTime{};
    int longEdge = kThumbLongEdge;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        bool found = false;
        while (!m_Stopping && !m_Queue.empty())
        {
            key = std::move(m_Queue.front());
            m_Queue.pop_front();

            auto it = m_Jobs.find(key);
            if (it == m_Jobs.end())
                continue;
            sourcePath = it->second.SourcePath;
            cachePath = it->second.CachePath;
            sourceTime = it->second.SourceTime;
            longEdge = it->second.LongEdge;
            found = true;
            break;
        }
        if (!found)
        {
            ReleaseDecodeSlotLocked();
            return;
        }
    }

    DeclineReason declineReason = DeclineReason::Undecodable;
    Outcome outcome = Outcome::Retry;
    try
    {
        outcome = GenerateThumbnail(sourcePath, cachePath, longEdge, &declineReason);
    }
    catch (const std::exception& error)
    {
        Logger::Log::Warning("TextureThumbnailHandler: decoding '{}' failed: {}",
                             sourcePath.string(), error.what());
    }
    catch (...)
    {
        Logger::Log::Warning("TextureThumbnailHandler: decoding '{}' failed", sourcePath.string());
    }

    std::vector<ReadyCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_Jobs.find(key);
        if (it != m_Jobs.end())
        {
            callbacks = std::move(it->second.Callbacks);
            m_Jobs.erase(it);
        }
        if (outcome == Outcome::Declined)
            m_Declined.emplace(key, DeclineEntry{sourceTime, declineReason});
        if (m_Stopping)
            callbacks.clear();
    }

    const std::string result =
        (outcome == Outcome::Written) ? cachePath.generic_string() : std::string();
    for (auto& callback : callbacks)
    {
        // A subscriber failure must not skip another subscriber or strand the
        // decode slot that shutdown waits for.
        try
        {
            callback(result);
        }
        catch (const std::exception& error)
        {
            Logger::Log::Warning("TextureThumbnailHandler: callback failed: {}", error.what());
        }
        catch (...)
        {
            Logger::Log::Warning("TextureThumbnailHandler: callback failed");
        }
    }

    // The slot stays claimed while the callbacks run, so the destructor's wait
    // covers them too. Chain rather than loop: each decode is its own pool task,
    // so Normal-priority work is scheduled between thumbnails instead of behind
    // a worker held for the whole queue.
    bool chain = false;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Stopping && !m_Queue.empty())
            chain = true;
        else
            ReleaseDecodeSlotLocked();
    }
    if (chain)
        EnqueueDecodeJob();
}

TextureThumbnailHandler::Outcome TextureThumbnailHandler::GenerateThumbnail(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& cachePath,
    int longEdge,
    DeclineReason* outDeclineReason)
{
    Vector<uint8> bytes;
    if (!ReadFileBytesShared(sourcePath, bytes) || bytes.empty())
    {
        // A failed or empty read says nothing about the source (a locked file,
        // a transient share violation). Retrying costs one read; remembering it
        // would cost the thumbnail for the rest of the session. (A race with an
        // in-place writer surfaces as truncated BYTES, not a failed read — that
        // path lands on Declined below, keyed to the pre-write timestamp, and
        // the writer's mtime bump reopens the decision.)
        Logger::Log::Debug("TextureThumbnailHandler: cannot read '{}'", sourcePath.string());
        return Outcome::Retry;
    }

    // Header first: dimensions decide whether a full decode is worth its
    // allocation, and both of the answers below cost nothing beyond this read.
    int infoW = 0;
    int infoH = 0;
    int infoChannels = 0;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                               &infoW, &infoH, &infoChannels) ||
        infoW <= 0 || infoH <= 0)
    {
        Logger::Log::Debug("TextureThumbnailHandler: not a decodable image '{}'",
                           sourcePath.string());
        *outDeclineReason = DeclineReason::Undecodable;
        return Outcome::Declined;
    }

    const uint64_t sourcePixels =
        static_cast<uint64_t>(infoW) * static_cast<uint64_t>(infoH);
    if (sourcePixels > kMaxSourcePixels)
    {
        Logger::Log::Debug(
            "TextureThumbnailHandler: '{}' is {}x{}, past the {} pixel decode budget",
            sourcePath.string(), infoW, infoH, kMaxSourcePixels);
        *outDeclineReason = DeclineReason::TooLarge;
        return Outcome::Declined;
    }

    // Already thumbnail-sized: re-encoding it at its own resolution would spend
    // a decode and a second copy on disk to produce a file no smaller than the
    // source. The service's fallback serves the source itself, for free.
    if (std::max(infoW, infoH) <= longEdge)
    {
        *outDeclineReason = DeclineReason::SmallSource;
        return Outcome::Declined;
    }

    g_DecodeAttempts.fetch_add(1, std::memory_order_relaxed);
    const DecodedImage decoded = DecodeImageToRGBA(bytes.data(), bytes.size());
    if (!decoded.valid || decoded.width == 0 || decoded.height == 0)
    {
        Logger::Log::Debug("TextureThumbnailHandler: cannot decode '{}'", sourcePath.string());
        *outDeclineReason = DeclineReason::Undecodable;
        return Outcome::Declined;
    }

    // The compressed bytes are dead once the pixels exist, and two decodes
    // holding both at once is the peak this handler is budgeted against.
    bytes.clear();
    bytes.shrink_to_fit();

    const int srcW = static_cast<int>(decoded.width);
    const int srcH = static_cast<int>(decoded.height);

    const double scale = static_cast<double>(longEdge) / static_cast<double>(std::max(srcW, srcH));
    const int dstW = std::max(1, static_cast<int>(std::lround(srcW * scale)));
    const int dstH = std::max(1, static_cast<int>(std::lround(srcH * scale)));

    std::vector<uint8_t> resized(static_cast<size_t>(dstW) * static_cast<size_t>(dstH) * 4);
    // sRGB-aware filtering: browsing is dominated by colour textures, and
    // averaging those in gamma space darkens every thumbnail. Alpha rides
    // STBIR_RGBA, which keeps coverage off the transfer curve.
    if (stbir_resize_uint8_srgb(decoded.pixels.data(), srcW, srcH, 0,
                                resized.data(), dstW, dstH, 0,
                                STBIR_RGBA) == nullptr)
    {
        Logger::Log::Warning("TextureThumbnailHandler: downscale failed for '{}'",
                             sourcePath.string());
        return Outcome::Retry;
    }

    std::error_code ec;
    std::filesystem::create_directories(cachePath.parent_path(), ec);

    // Stage beside the destination; FileSystem owns unique naming and the
    // platform's publication semantics (atomic replacement where supported).
    const std::filesystem::path tempPath = FileSystem::MakeTemporarySiblingPath(cachePath);

    if (!stbi_write_png(tempPath.string().c_str(), dstW, dstH, 4, resized.data(), dstW * 4))
    {
        Logger::Log::Warning("TextureThumbnailHandler: failed to write '{}'", tempPath.string());
        std::filesystem::remove(tempPath, ec);
        return Outcome::Retry;
    }

    if (!GameEngine::FileSystem::PublishFile(tempPath, cachePath))
    {
        std::error_code existsEc;
        // Another writer won the race; its file is as good as ours. If there is
        // no file at all the publish simply failed, which says nothing about the
        // source — the next request must be free to try again.
        return std::filesystem::exists(cachePath, existsEc) ? Outcome::Written : Outcome::Retry;
    }

    return Outcome::Written;
}

} // namespace GameEngine
