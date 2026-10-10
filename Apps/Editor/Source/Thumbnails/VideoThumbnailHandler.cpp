#include "Thumbnails/VideoThumbnailHandler.h"

#include "Logger/Logger.h"
#include "Core/Engine.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Video/VideoPlayer.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <vector>

namespace GameEngine
{

namespace
{

constexpr int kThumbLongEdge = 512;
constexpr float kMinBrightness = 15.0f; // average pixel value threshold to skip dark frames
// Longest this offline extractor blocks for one seek's frame to be decoded.
constexpr float kFrameWaitTimeoutSeconds = 1.0f;

float AveragePixelBrightness(const std::vector<uint8_t>& rgba, int w, int h)
{
    if (rgba.empty())
        return 0.0f;
    uint64_t sum = 0;
    const size_t pixelCount = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < pixelCount; ++i)
    {
        // Approximate luminance: (R + G + B) / 3
        sum += rgba[i * 4 + 0];
        sum += rgba[i * 4 + 1];
        sum += rgba[i * 4 + 2];
    }
    return static_cast<float>(sum) / static_cast<float>(pixelCount * 3);
}

// Simple box-filter downscale from (srcW x srcH) RGBA to (dstW x dstH).
std::vector<uint8_t> DownscaleRGBA(const uint8_t* src, int srcW, int srcH, int dstW, int dstH)
{
    std::vector<uint8_t> dst(static_cast<size_t>(dstW) * dstH * 4);
    const float scaleX = static_cast<float>(srcW) / dstW;
    const float scaleY = static_cast<float>(srcH) / dstH;

    for (int dy = 0; dy < dstH; ++dy)
    {
        for (int dx = 0; dx < dstW; ++dx)
        {
            const int sx = std::min(static_cast<int>(dx * scaleX), srcW - 1);
            const int sy = std::min(static_cast<int>(dy * scaleY), srcH - 1);
            const size_t srcIdx = (static_cast<size_t>(sy) * srcW + sx) * 4;
            const size_t dstIdx = (static_cast<size_t>(dy) * dstW + dx) * 4;
            dst[dstIdx + 0] = src[srcIdx + 0];
            dst[dstIdx + 1] = src[srcIdx + 1];
            dst[dstIdx + 2] = src[srcIdx + 2];
            dst[dstIdx + 3] = src[srcIdx + 3];
        }
    }
    return dst;
}

} // namespace

VideoThumbnailHandler::VideoThumbnailHandler() = default;

std::filesystem::path VideoThumbnailHandler::GetCachePath(const std::filesystem::path& assetPath) const
{
    // Hash the absolute path to create a unique cache filename.
    std::size_t h = std::hash<std::string>{}(assetPath.string());
    std::string filename = "vidthumb_" + std::to_string(h) + ".png";

    if (!m_CacheRoot.empty())
        return m_CacheRoot / filename;

    // Fallback: next to the video file
    return assetPath.parent_path() / (".thumbcache_" + filename);
}

std::string VideoThumbnailHandler::GetOrRequest(const std::filesystem::path& assetPath,
                                                 int /*desiredSize*/,
                                                 std::function<void(const std::string& relPath)> onReady,
                                                 bool /*StaticModelListThumbnail*/)
{
    const auto cachePath = GetCachePath(assetPath);

    // If cached thumbnail exists, return it immediately.
    std::error_code ec;
    if (std::filesystem::exists(cachePath, ec))
    {
        std::string rel;
        if (!m_AssetsRoot.empty())
        {
            auto r = std::filesystem::relative(cachePath, m_AssetsRoot, ec);
            if (!ec && !r.empty())
                rel = r.generic_string();
        }
        if (rel.empty())
            rel = cachePath.generic_string();

        if (onReady)
            onReady(rel);
        return rel;
    }

    // Check if already generating.
    const std::string key = assetPath.string();
    {
        std::lock_guard<std::mutex> lock(m_State->Mutex);
        if (m_State->InFlight.count(key))
            return {};
        m_State->InFlight.insert(key);
    }

    // Dispatch to a background job for async frame extraction.
    auto assetsRoot = m_AssetsRoot;

    EngineCore::GetInstance().GetJobSystem().EnqueueWork([state = m_State, assetPath, cachePath, assetsRoot, onReady, key]()
    {
        Video::VideoPlayer player;
        bool extracted = false;

        if (player.Load(assetPath.string()))
        {
            const float duration = player.GetDuration();
            const int srcW = player.GetWidth();
            const int srcH = player.GetHeight();

            if (duration > 0.0f && srcW > 0 && srcH > 0)
            {
                std::vector<uint8_t> frameRGBA(static_cast<size_t>(srcW) * srcH * 4);

                // Try a series of seek positions to find a non-black frame.
                // Start at 10% of duration, then 20%, 30%, etc.
                constexpr float kSeekSteps[] = {0.10f, 0.20f, 0.30f, 0.50f, 0.02f};

                for (float pct : kSeekSteps)
                {
                    const float seekTime = duration * pct;
                    player.Seek(seekTime);
                    // Pump one frame after seek.
                    player.Play();

                    if (player.WaitForFrame(kFrameWaitTimeoutSeconds))
                    {
                        player.GetFrameRGBA(frameRGBA.data());
                        if (AveragePixelBrightness(frameRGBA, srcW, srcH) >= kMinBrightness)
                        {
                            extracted = true;
                            break;
                        }
                    }
                    player.Stop();
                }

                // If all seeks were dark, use the last extracted frame anyway.
                if (!extracted && player.PollNewFrame())
                {
                    player.GetFrameRGBA(frameRGBA.data());
                    extracted = true;
                }

                if (extracted)
                {
                    // Downscale so the long edge is kThumbLongEdge, preserving aspect ratio.
                    int dstW, dstH;
                    if (srcW >= srcH)
                    {
                        dstW = kThumbLongEdge;
                        dstH = std::max(1, srcH * kThumbLongEdge / srcW);
                    }
                    else
                    {
                        dstH = kThumbLongEdge;
                        dstW = std::max(1, srcW * kThumbLongEdge / srcH);
                    }
                    auto thumb = DownscaleRGBA(frameRGBA.data(), srcW, srcH, dstW, dstH);

                    // Ensure cache directory exists.
                    std::error_code dirEc;
                    std::filesystem::create_directories(cachePath.parent_path(), dirEc);

                    stbi_write_png(cachePath.string().c_str(),
                                   dstW, dstH, 4,
                                   thumb.data(),
                                   dstW * 4);
                }
            }
            player.Unload();
        }

        // Remove from in-flight set.
        {
            std::lock_guard<std::mutex> lock(state->Mutex);
            state->InFlight.erase(key);
        }

        if (onReady)
        {
            std::string rel;
            if (extracted)
            {
                std::error_code ec;
                if (!assetsRoot.empty())
                {
                    auto r = std::filesystem::relative(cachePath, assetsRoot, ec);
                    if (!ec && !r.empty())
                        rel = r.generic_string();
                }
                if (rel.empty())
                    rel = cachePath.generic_string();
            }
            onReady(rel);
        }
    }, JobSystem::JobPriority::Background);

    return {};
}

} // namespace GameEngine
