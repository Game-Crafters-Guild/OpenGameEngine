// ModelThumbnailHandler, part: The transient model imports the slots render from: acquiring them
// and releasing them once no slot or lane needs them.

#include "Thumbnails/ModelThumbnailHandler.h"

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"

#include <chrono>
#include <memory>

namespace GameEngine
{

std::shared_ptr<ModelAsset> ModelThumbnailHandler::AcquireSlotModel(Slot& slot, const GUID& guid)
{
    if (!slot.modelImport.valid())
    {
        if (m_AssetManager->IsLoadSuppressed(guid))
            return nullptr;
        AssetLoadRequest request(guid, AssetLoadResultCallback{}, AssetLoadPriority::High);
        request.Residency = AssetResidency::Transient;
        AssetLoadHandle handle = m_AssetManager->LoadAsset(request);
        if (!handle.Future)
            return nullptr;
        slot.modelImport = *handle.Future;
    }
    return ImportedSlotModel(slot);
}

std::shared_ptr<ModelAsset> ModelThumbnailHandler::ImportedSlotModel(const Slot& slot)
{
    if (!slot.modelImport.valid() || IsSlotModelImporting(slot))
        return nullptr;
    return std::dynamic_pointer_cast<ModelAsset>(slot.modelImport.get());
}

bool ModelThumbnailHandler::IsSlotModelImporting(const Slot& slot)
{
    return slot.modelImport.valid() &&
           slot.modelImport.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

// GE_THUMBNAIL_CACHE_IMPORTS_BEGIN
bool ModelThumbnailHandler::ReleaseSlotModel(Slot& slot)
{
    return ReleaseModelImport(slot.guid, slot.modelImport);
}

bool ModelThumbnailHandler::ReleaseModelImport(const GUID& guid,
                                               std::shared_future<std::shared_ptr<Asset>>& modelImport)
{
    if (!modelImport.valid())
        return true;
    for (const auto& [windowId, ws] : m_Windows)
    {
        for (const RenderLane& lane : ws.lanes)
        {
            if (lane.spawnedModelGuid == guid && lane.inFlightSlot != kMaxResidentSlots)
                return false;
        }
    }
    modelImport = {};
    // The handler is the only transient requester: a slot that still holds the
    // same import (a list thumbnail, another window) releases the model later.
    if (IsModelImportHeld(guid))
        return true;
    // The unload drops the model's GPU meshes, so no lane may keep them in
    // its GPU scene.
    for (auto& [windowId, ws] : m_Windows)
    {
        for (RenderLane& lane : ws.lanes)
        {
            if (lane.spawnedModelGuid == guid)
                TeardownSpawnedModel(lane);
        }
    }
    m_AssetManager->ReleaseTransientAsset(guid);
    return true;
}

bool ModelThumbnailHandler::IsModelImportHeld(const GUID& guid) const
{
    for (const auto& [windowId, ws] : m_Windows)
    {
        for (const Slot& other : ws.slots)
        {
            if (other.modelImport.valid() && other.guid == guid)
                return true;
        }
    }
    for (const DeferredModelRelease& deferred : m_DeferredModelReleases)
    {
        if (deferred.modelImport.valid() && deferred.guid == guid)
            return true;
    }
    return false;
}

void ModelThumbnailHandler::DeferModelRelease(Slot& slot)
{
    if (slot.modelImport.valid())
        m_DeferredModelReleases.push_back({slot.guid, std::move(slot.modelImport)});
    slot.modelImport = {};
}

void ModelThumbnailHandler::ReleaseDeferredModelImports()
{
    for (size_t i = 0; i < m_DeferredModelReleases.size();)
    {
        DeferredModelRelease& deferred = m_DeferredModelReleases[i];
        if (ReleaseModelImport(deferred.guid, deferred.modelImport))
        {
            m_DeferredModelReleases[i] = std::move(m_DeferredModelReleases.back());
            m_DeferredModelReleases.pop_back();
        }
        else
        {
            ++i;
        }
    }
}

void ModelThumbnailHandler::ReleaseAllModelImports()
{
    for (auto& [windowId, ws] : m_Windows)
    {
        for (Slot& slot : ws.slots)
            DeferModelRelease(slot);
    }
    ReleaseDeferredModelImports();
}
// GE_THUMBNAIL_CACHE_IMPORTS_END

void ModelThumbnailHandler::ReleaseBakedModelImports(WindowState& ws)
{
    // A tile needs its model only until the bake is in the PNG cache: it then
    // shows the PNG, and so does every later session.
    for (Slot& slot : ws.slots)
    {
        if (slot.modelImport.valid() && IsTileBakeFinal(ws, slot))
            ReleaseSlotModel(slot);
    }
}

} // namespace GameEngine
