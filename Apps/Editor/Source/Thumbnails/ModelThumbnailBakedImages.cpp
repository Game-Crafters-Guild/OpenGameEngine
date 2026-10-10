// ModelThumbnailHandler, part: Baked tiles: a grid tile whose render is in the PNG cache shows the
// decoded PNG instead of its render target, and gives its slot back once the browser shows the PNG
// itself.

#include "Thumbnails/ModelThumbnailHandler.h"

#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "Engine/Rendering/RenderServices.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Utils/TextureUploadHelpers.h"

#include <system_error>
#include <vector>

namespace GameEngine
{

namespace
{
// Frames before a baked tile asks again for a PNG that was not written yet (the
// write lands a few frames after its readback).
constexpr uint64_t kBakedImageRetryFrames = 15u;
} // namespace

bool ModelThumbnailHandler::IsTileBakeFinal(const WindowState& ws, const Slot& slot) const
{
    if (!slot.occupied || slot.isMaterial || slot.isLensFlare || slot.listStaticThumb)
        return false;
    if (!slot.ready || slot.inFlight)
        return false;
    // The Asset View preview renders the focused model live.
    if (!s_OrbitFocusGuid.IsNull() && slot.guid == s_OrbitFocusGuid)
        return false;
    if (!m_DiskCacheRequested.count(slot.guid) || ws.IsModelSettling(slot.guid))
        return false;
    if (m_PendingDiskCache && m_PendingDiskCache->guid == slot.guid)
        return false;
    return !ws.pendingSet.count({slot.guid, false, false, false, true});
}

bool ModelThumbnailHandler::WantsBakedImage(const WindowState& ws, const Slot& slot) const
{
    if (slot.bakeOnly)
        return false;
    if (slot.showsBakedImage)
        return false;
    if (slot.restoresBakedImage)
        return slot.occupied && !slot.inFlight && m_DiskCacheRequested.count(slot.guid) &&
               (s_OrbitFocusGuid.IsNull() || slot.guid != s_OrbitFocusGuid);
    return IsTileBakeFinal(ws, slot);
}

void ModelThumbnailHandler::RequestBakedImages(WindowState& ws)
{
    if (!m_AssetManager)
        return;
    auto& registry = m_AssetManager->GetRegistry();
    for (Slot& slot : ws.slots)
    {
        if (slot.bakedImagePending || slot.bakedImageRetryFrame > ws.frameCounter ||
            !WantsBakedImage(ws, slot))
            continue;
        AssetMetadata meta;
        if (!registry.TryGetAssetMetadata(slot.guid, meta) || meta.Path.empty())
            continue;
        slot.bakedImagePending = true;
        EngineCore::GetInstance().GetJobSystem().EnqueueWork(
            [inbox = m_BakedImages, guid = slot.guid, pngPath = ComputeModelCachePath(slot.guid),
             sourcePath = meta.Path]()
            {
                BakedImage image{guid};
                // A PNG older than its source is about to be replaced.
                std::error_code sourceError, pngError;
                const auto sourceTime = std::filesystem::last_write_time(sourcePath, sourceError);
                const auto pngTime = std::filesystem::last_write_time(pngPath, pngError);
                Vector<uint8> bytes;
                if (!sourceError && !pngError && pngTime >= sourceTime &&
                    ReadFileBytesShared(pngPath, bytes))
                {
                    DecodedImage decoded = DecodeImageToRGBA(bytes.data(), bytes.size());
                    if (decoded.valid)
                    {
                        image.width = decoded.width;
                        image.height = decoded.height;
                        image.pixels = std::move(decoded.pixels);
                    }
                }
                std::lock_guard lock(inbox->mutex);
                inbox->images.push_back(std::move(image));
            },
            JobSystem::JobPriority::Background);
    }
}

bool ModelThumbnailHandler::ShowBakedImages()
{
    std::vector<BakedImage> images;
    {
        std::lock_guard lock(m_BakedImages->mutex);
        images.swap(m_BakedImages->images);
    }
    if (images.empty())
        return false;
    Rendering::IDevice* device = m_RenderServices ? m_RenderServices->GetDevice() : nullptr;
    for (BakedImage& image : images)
    {
        for (auto& [windowId, ws] : m_Windows)
        {
            const auto it = ws.guidToSlot.find(image.guid);
            if (it == ws.guidToSlot.end() || it->second >= ws.slots.size())
                continue;
            const size_t slotIdx = it->second;
            Slot& slot = ws.slots[slotIdx];
            if (!slot.bakedImagePending)
                continue;
            slot.bakedImagePending = false;
            if (image.pixels.empty())
            {
                // A restoring tile whose PNG went stale renders again.
                if (slot.restoresBakedImage)
                {
                    slot.restoresBakedImage = false;
                    AcquireSlotModel(slot, slot.guid);
                    EnqueuePending(ws, slot.guid, false);
                }
                else
                {
                    slot.bakedImageRetryFrame = ws.frameCounter + kBakedImageRetryFrames;
                }
                continue;
            }
            // Focus or a reload can reach the tile while its PNG decodes.
            if (!device || !WantsBakedImage(ws, slot))
                continue;

            Rendering::TextureDesc td{};
            td.width = image.width;
            td.height = image.height;
            td.depth = 1;
            td.mipLevels = 1;
            td.arrayLayers = 1;
            td.sampleCount = 1;
            td.format = static_cast<uint32_t>(Rendering::TextureFormat::RGBA8_SRGB);
            td.usage = static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource) |
                       static_cast<uint32_t>(Rendering::TextureUsage::TransferDst);
            const std::string debugName = "Editor.ModelThumb.Baked" + std::to_string(slotIdx);
            td.debugName = debugName.c_str();
            const Rendering::TextureHandle baked = device->CreateTexture(td);
            if (!baked.IsValid())
                continue;
            Rendering::UploadTexture2D(device, baked, image.pixels.data(), image.width,
                                       image.height, static_cast<size_t>(image.width) * 4u,
                                       "Editor.ModelThumb.BakedUpload");
            // Timeline-deferred: frames in flight may still sample the target.
            if (slot.deviceTex.IsValid())
                device->DestroyTexture(slot.deviceTex);
            slot.deviceTex = baked;
            slot.deviceTexInitialized = true;
            slot.texWidth = image.width;
            slot.texHeight = image.height;
            slot.showsBakedImage = true;
            slot.restoresBakedImage = false;
            slot.ready = true;
        }
    }
    return true;
}

// GE_THUMBNAIL_CACHE_RELEASE_BEGIN
void ModelThumbnailHandler::ReleaseGridSlotServedByPng(const GUID& guid)
{
    std::lock_guard lock(m_ServedByPngMutex);
    m_ServedByPng.push_back(guid);
}

size_t ModelThumbnailHandler::PendingGridSlotReleaseCount() const
{
    std::lock_guard lock(m_ServedByPngMutex);
    return m_ServedByPng.size();
}

void ModelThumbnailHandler::ReleaseGridSlotsServedByPng()
{
    std::vector<GUID> served;
    {
        std::lock_guard lock(m_ServedByPngMutex);
        if (m_ServedByPng.empty())
            return;
        served.swap(m_ServedByPng);
    }
    for (const GUID& guid : served)
    {
        // Its PNG is current: a folder bake skips it without asking the disk.
        m_DiskCacheRequested.insert(guid);
        // The Asset View preview of this model orbits in its grid slot.
        if (guid == s_OrbitFocusGuid)
            continue;
        for (auto& [windowId, ws] : m_Windows)
        {
            (void)windowId;
            const auto it = ws.guidToSlot.find(guid);
            if (it == ws.guidToSlot.end())
                continue;
            const size_t slotIdx = it->second;
            Slot& slot = ws.slots[slotIdx];
            if (slot.inFlight || slot.bakeOnly)
                continue;
            DropBakedImage(ws, slot);
            ReleaseSlotAssignment(ws, slotIdx);
            slot.occupied = false;
        }
    }
}
void ModelThumbnailHandler::RequestModelTile(WindowState& ws, const GUID& guid, bool listStatic)
{
    const size_t slotIdx = AcquireSlotForGuid(ws, guid, listStatic);
    Slot& slot = ws.slots[slotIdx];
    if (!slot.ready && !slot.inFlight && !slot.restoresBakedImage)
    {
        // A tile whose bake is already in the PNG cache comes back as the PNG.
        const bool focused = !s_OrbitFocusGuid.IsNull() && guid == s_OrbitFocusGuid;
        if (!listStatic && !focused && m_DiskCacheRequested.count(guid))
        {
            slot.restoresBakedImage = true;
            return;
        }
        AcquireSlotModel(slot, guid);
        EnqueuePending(ws, guid, listStatic);
    }
}
// GE_THUMBNAIL_CACHE_RELEASE_END

void ModelThumbnailHandler::DropBakedImage(WindowState& ws, Slot& slot)
{
    if (!slot.showsBakedImage)
        return;
    if (Rendering::IDevice* device = m_RenderServices ? m_RenderServices->GetDevice() : nullptr)
        device->DestroyTexture(slot.deviceTex);
    slot.deviceTex = {};
    slot.deviceTexInitialized = false;
    slot.showsBakedImage = false;
    slot.texWidth = 0;
    slot.texHeight = 0;
    if (slot.uiBound)
    {
        ws.evictedUiKeys.push_back(slot.uiKey);
        slot.uiBound = false;
    }
    slot.ready = false;
}

} // namespace GameEngine
