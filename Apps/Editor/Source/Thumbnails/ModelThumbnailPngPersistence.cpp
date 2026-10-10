// ModelThumbnailHandler, part: Persisting finished renders: the readback of a settled slot and the
// PNG write.

#include "Thumbnails/ModelThumbnailHandler.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "Engine/Rendering/RenderServices.h"
#include "FileSystem/FileSystem.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <stb_image_resize2.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <system_error>
#include <vector>

namespace GameEngine
{

namespace
{
// The PNG cache only ever serves tile requests (<= 128 logical px, see
// ThumbnailService's kDiskCacheMaxTileRequestPx); preview sizes take the live
// path. Twice the tile size covers HiDPI grids without storing the 1024 px
// slot render, which is 16x the bytes to decode and upload per scroll.
constexpr uint32_t kDiskCachePngLongEdge = 256u;
} // namespace

bool ModelThumbnailHandler::RetryEmptyCapture(const GUID& guid)
{
    if (!m_EmptyCaptureRetried.insert(guid).second)
        return false;
    m_DiskCacheRequested.erase(guid);
    AssetMetadata meta;
    if (m_AssetManager && m_AssetManager->GetRegistry().TryGetAssetMetadata(guid, meta))
        Logger::Log::Info("Thumbnails: '{}' rendered empty; rendering it again before caching it",
                          meta.Path.string());
    for (auto& [windowId, ws] : m_Windows)
    {
        (void)windowId;
        for (RenderLane& lane : ws.lanes)
        {
            if (lane.SpawnedGuid() == guid)
                lane.spawnedSettleFrames = 0;
        }
        for (Slot& slot : ws.slots)
        {
            if (!slot.occupied || slot.guid != guid || slot.isLensFlare || slot.inFlight)
                continue;
            slot.ready = false;
            slot.materialRenderSettled = false;
            EnqueuePending(ws, guid, slot.listStaticThumb, slot.isMaterial, slot.previewIblEnabled);
        }
    }
    return true;
}

// GE_THUMBNAIL_CACHE_SCHEDULE_BEGIN
void ModelThumbnailHandler::PromotePendingMaterialFocus()
{
    // How long the Asset View waits for a tile that never settles before it orbits anyway.
    constexpr std::chrono::seconds kPendingFocusLimit{3};
    const GUID guid = s_PendingMaterialFocusGuid;
    if (guid.IsNull())
        return;
    std::error_code error;
    const std::filesystem::path cachePath =
        m_DiskCacheRoot.empty() ? std::filesystem::path() : ComputeMaterialCachePath(guid, s_PendingMaterialFocusIblEnabled);
    // The PNG of the focused variant (with or without the preview's IBL), not the GUID's
    // readback request, which either variant sets.
    const bool cached = cachePath.empty() || std::filesystem::exists(cachePath, error);
    if (!cached && std::chrono::steady_clock::now() - s_PendingMaterialFocusSince < kPendingFocusLimit)
        return;
    s_MaterialOrbitFocusGuid = guid;
    s_MaterialOrbitFocusIblEnabled = s_PendingMaterialFocusIblEnabled;
    s_PendingMaterialFocusGuid = GUID{};
}

void ModelThumbnailHandler::MaybeStartDiskCacheReadbackRG(WindowState& ws,
                                                          Rendering::RenderGraph::RGFrame& frame)
{
    PromotePendingMaterialFocus();
    if (m_DiskCacheRoot.empty() || !m_RenderServices || !m_AssetManager)
        return;
    if (m_PendingDiskCache)
        return; // single in-flight readback

    Rendering::IDevice* device = m_RenderServices->GetDevice();
    if (!device)
        return;

    auto& registry = m_AssetManager->GetRegistry();
    for (Slot& slot : ws.slots)
    {
        if (!slot.occupied || slot.isLensFlare)
            continue;
        // A baked tile's texture is the PNG itself.
        if (!slot.ready || slot.inFlight || !slot.deviceTexInitialized || slot.showsBakedImage)
            continue;
        if (slot.isMaterial)
        {
            // An unsettled render can miss the material's pass variant or its
            // texture bindings.
            if (!slot.materialRenderSettled)
                continue;
            if (!s_MaterialOrbitFocusGuid.IsNull() && slot.guid == s_MaterialOrbitFocusGuid &&
                slot.previewIblEnabled == s_MaterialOrbitFocusIblEnabled)
                continue;
        }
        else
        {
            // Only this model's old ready flag can hide an unsettled re-render.
            // A live orbit preview must not block other completed grid slots.
            if (ws.IsModelSettling(slot.guid))
                continue;
            // A model that gave up waiting for its GPU data rendered blank;
            // caching that frame would freeze the blank tile across sessions.
            if (slot.renderGaveUp)
                continue;
            if (!s_OrbitFocusGuid.IsNull() && slot.guid == s_OrbitFocusGuid)
                continue;
        }
        if (m_DiskCacheRequested.count(slot.guid))
            continue;
        // Only the canonical square grid render is cacheable. The focused
        // Asset View slot holds an orbiting, panel-shaped frame.
        const auto canonicalPx = static_cast<uint32_t>(s_ThumbnailResolutionPx);
        if (slot.texWidth != canonicalPx || slot.texHeight != canonicalPx)
            continue;

        AssetMetadata meta;
        if (!registry.TryGetAssetMetadata(slot.guid, meta) || meta.Path.empty())
            continue;
        if (slot.isMaterial && IsEphemeralPreviewMaterialPath(meta.Path))
            continue;

        const std::filesystem::path cachePath = slot.isMaterial
                                                    ? ComputeMaterialCachePath(slot.guid, slot.previewIblEnabled)
                                                    : ComputeModelCachePath(slot.guid);
        if (cachePath.empty())
            continue;

        std::error_code ec;
        if (std::filesystem::exists(cachePath, ec))
        {
            // Stale check mirrors the read side: an edited source re-bakes.
            std::error_code ecSrc, ecCache;
            const auto srcTime = std::filesystem::last_write_time(meta.Path, ecSrc);
            const auto cacheTime = std::filesystem::last_write_time(cachePath, ecCache);
            if (!ecSrc && !ecCache && cacheTime >= srcTime)
            {
                m_DiskCacheRequested.insert(slot.guid);
                continue;
            }
        }

        // The persistent slot texture already holds the tonemapped render: read it back, no
        // re-render needed. The capture puts the slot back in its sampled state after the copy,
        // which the next frame's import of the slot (the grid's sample, a re-render) claims.
        auto ticket = Rendering::RequestDeviceTextureReadbackRG(device, frame, slot.deviceTex,
                                                                Rendering::ResourceState::ShaderResource,
                                                                "Editor.ModelThumb.DiskCacheReadback");
        if (!ticket)
            return;

        m_PendingDiskCache.emplace(PendingDiskCache{slot.guid, cachePath, meta.Path,
                                                    std::move(ticket),
                                                    UI::UITextureSpace::DisplayLinearSdr(),
                                                    kDiskCachePngLongEdge});
        m_DiskCacheRequested.insert(slot.guid);
        return; // one per frame
    }
}

// GE_THUMBNAIL_CACHE_SCHEDULE_END

// GE_THUMBNAIL_CACHE_PERSIST_BEGIN
bool ModelThumbnailHandler::IsFullyTransparent(const std::vector<uint8_t>& pixels,
                                               Rendering::TextureFormat format)
{
    if (format == Rendering::TextureFormat::R16G16B16A16_FLOAT)
    {
        // Alpha is the fourth little-endian half; a zero exponent (zero or a
        // subnormal) converts to 0 coverage.
        for (size_t i = 7; i < pixels.size(); i += 8)
        {
            if (((pixels[i] >> 2) & 0x1Fu) != 0)
                return false;
        }
        return true;
    }
    for (size_t i = 3; i < pixels.size(); i += 4)
    {
        if (pixels[i] != 0)
            return false;
    }
    return true;
}

void ModelThumbnailHandler::PollDiskCacheReadback()
{
    if (!m_PendingDiskCache)
        return;

    if (!m_PendingDiskCache->ticket)
    {
        m_DiskCacheRequested.erase(m_PendingDiskCache->guid);
        m_PendingDiskCache.reset();
        return;
    }

    Rendering::ViewReadbackResult result;
    // A cancelled ticket (declaring frame abandoned before submit) can never
    // resolve — drop the pending and let the next scan retry instead of
    // wedging the single-in-flight pipeline.
    if (m_PendingDiskCache->ticket->IsConsumed())
    {
        m_DiskCacheRequested.erase(m_PendingDiskCache->guid);
        m_PendingDiskCache.reset();
        return;
    }
    if (!m_PendingDiskCache->ticket->TryGet(result))
        return;

    const GUID capturedGuid = m_PendingDiskCache->guid;
    std::filesystem::path outputPath = std::move(m_PendingDiskCache->outputPath);
    const UI::UITextureSpace srcSpace = m_PendingDiskCache->srcSpace;
    const uint32_t maxPngLongEdge = m_PendingDiskCache->maxPngLongEdge;
    m_PendingDiskCache.reset();

    if (result.pixels.empty() || result.width == 0 || result.height == 0)
        return;

    std::error_code ec;
    std::filesystem::create_directories(outputPath.parent_path(), ec);
    if (ec)
        return;

    const std::string cachePathStr = outputPath.generic_string();
    const Rendering::TextureFormat fmt = result.format;
    const uint32_t w = result.width;
    const uint32_t h = result.height;
    std::vector<uint8_t> pixels = std::move(result.pixels);
    // An empty frame is persisted only when the asset renders empty again.
    if (IsFullyTransparent(pixels, fmt) && RetryEmptyCapture(capturedGuid))
        return;

    // The declaration's stamp decides the transfer curve; `fmt` only decides how the
    // bytes unpack. This path keeps the source alpha (thumbnails composite over the
    // asset grid), which is why it does not share ReadbackToRgba8Srgb's opaque-alpha
    // conversion.
    const bool encode = !UI::IsEncodedAtRest(srcSpace);

    EngineCore::GetInstance().GetJobSystem().EnqueueWork([pixels = std::move(pixels), cachePathStr, fmt, w, h, encode, maxPngLongEdge]() mutable
    {
        const uint8_t* pngSrc = nullptr;
        std::vector<uint8_t> rgba8;

        auto linearToSrgb = [](float c) -> float
        {
            c = std::max(0.0f, std::min(1.0f, c));
            return c <= 0.0031308f ? c * 12.92f
                                   : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        };

        if (fmt == Rendering::TextureFormat::RGBA8_UNORM ||
            fmt == Rendering::TextureFormat::RGBA8_SRGB)
        {
            if (encode)
            {
                for (size_t i = 0; i + 3 < pixels.size(); i += 4)
                    for (size_t c = 0; c < 3; ++c)
                        pixels[i + c] = static_cast<uint8_t>(
                            linearToSrgb(static_cast<float>(pixels[i + c]) / 255.0f) * 255.0f + 0.5f);
            }
            pngSrc = pixels.data();
        }
        else if (fmt == Rendering::TextureFormat::R16G16B16A16_FLOAT)
        {
            const size_t pixelCount = static_cast<size_t>(w) * h;
            rgba8.resize(pixelCount * 4);
            const uint16_t* src = reinterpret_cast<const uint16_t*>(pixels.data());

            auto halfToFloat = [](uint16_t v) -> float
            {
                const uint32_t sign     = (v >> 15) & 0x1;
                const uint32_t exp      = (v >> 10) & 0x1F;
                const uint32_t mantissa = v & 0x3FF;
                if (exp == 0)  return 0.0f;
                if (exp == 31) return sign ? -1.0f : 1.0f;
                float f = std::ldexp(static_cast<float>(mantissa) / 1024.0f + 1.0f,
                                     static_cast<int>(exp) - 15);
                return sign ? -f : f;
            };
            auto toChannel = [&](uint16_t half) -> uint8_t
            {
                const float v = halfToFloat(half);
                const float out = encode ? linearToSrgb(v) : std::clamp(v, 0.0f, 1.0f);
                return static_cast<uint8_t>(out * 255.0f + 0.5f);
            };
            for (size_t i = 0; i < pixelCount; ++i)
            {
                rgba8[i * 4 + 0] = toChannel(src[i * 4 + 0]);
                rgba8[i * 4 + 1] = toChannel(src[i * 4 + 1]);
                rgba8[i * 4 + 2] = toChannel(src[i * 4 + 2]);
                // Alpha is coverage, never curve-mapped.
                float a = std::clamp(halfToFloat(src[i * 4 + 3]), 0.0f, 1.0f);
                rgba8[i * 4 + 3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
            }
            pngSrc = rgba8.data();
        }
        else
        {
            Logger::Log::Warning("ModelThumbnailHandler: unhandled material thumb format {}",
                                 static_cast<int>(fmt));
            return;
        }

        // Downscale before encoding when the declaration asked for it (model
        // slots render far larger than any tile samples them; see
        // kDiskCachePngLongEdge). pngSrc holds sRGB-encoded bytes here.
        uint32_t outW = w;
        uint32_t outH = h;
        std::vector<uint8_t> downscaled;
        if (maxPngLongEdge > 0 && std::max(w, h) > maxPngLongEdge)
        {
            const float scale = static_cast<float>(maxPngLongEdge) / static_cast<float>(std::max(w, h));
            outW = std::max(1u, static_cast<uint32_t>(std::lround(w * scale)));
            outH = std::max(1u, static_cast<uint32_t>(std::lround(h * scale)));
            downscaled.resize(static_cast<size_t>(outW) * outH * 4);
            if (stbir_resize_uint8_srgb(pngSrc, static_cast<int>(w), static_cast<int>(h), 0,
                                        downscaled.data(), static_cast<int>(outW),
                                        static_cast<int>(outH), 0, STBIR_RGBA) == nullptr)
            {
                Logger::Log::Warning("ModelThumbnailHandler: thumb downscale failed for '{}'",
                                     cachePathStr);
                return;
            }
            pngSrc = downscaled.data();
        }

        // Write beside the final path and publish: scroll-time readers probe
        // exists()+mtime every frame, so they must never observe a partial PNG.
        const std::filesystem::path cachePath = std::filesystem::path(cachePathStr);
        const std::filesystem::path tempPath = FileSystem::MakeTemporarySiblingPath(cachePath);
        const std::string tempPathStr = tempPath.string();

        if (!stbi_write_png(tempPathStr.c_str(),
                            static_cast<int>(outW),
                            static_cast<int>(outH),
                            4,
                            pngSrc,
                            static_cast<int>(outW * 4)))
        {
            Logger::Log::Warning("ModelThumbnailHandler: failed to write thumbnail '{}'",
                                 tempPathStr);
            std::error_code removeEc;
            std::filesystem::remove(tempPath, removeEc);
            return;
        }

        if (!FileSystem::PublishFile(tempPath, cachePath))
            return;

        Logger::Log::Info("Editor: wrote thumbnail '{}'", cachePathStr);
    }, JobSystem::JobPriority::Background);
}

// GE_THUMBNAIL_CACHE_PERSIST_END

} // namespace GameEngine
