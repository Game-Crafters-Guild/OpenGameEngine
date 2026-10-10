// GUID-keyed GPU texture residency map owned by TextureService.

#pragma once

#include "AssetCore/GUID.h"
#include "Rendering/Core/Handle.h"

#include <shared_mutex>
#include <unordered_map>

namespace GameEngine
{
namespace Engine::Renderer
{

// The GUID -> GPU TextureHandle map behind TextureService::GetOrUpload.
//
// Threading: the ECS extraction wave resolves textures on JobSystem workers
// (TerrainExtraction, OceanExtraction, LensFlareExtraction and EZTreeExtraction
// all reach GetOrUpload) while the render thread uploads, evicts and hot-swaps
// through the same map. Read-mostly by construction: an entry is written once on
// first touch and then read every frame, so lookups take a shared lock and only
// first-touch publishes, evictions and hot-reload swaps take the exclusive one.
//
// The map is private and every operation locks, so a caller cannot reach the
// storage unsynchronized — which is the property that a bare std::unordered_map
// could not hold.
class TextureGPUCache
{
  public:
    bool TryGet(const GUID& guid, Rendering::TextureHandle& out) const
    {
        std::shared_lock lock(m_Mutex);
        const auto it = m_Map.find(guid);
        if (it == m_Map.end())
            return false;
        out = it->second;
        return true;
    }

    bool Contains(const GUID& guid) const
    {
        std::shared_lock lock(m_Mutex);
        return m_Map.find(guid) != m_Map.end();
    }

    // First-touch publish. Returns the handle now cached for `guid`: `candidate`
    // when this call installed it, otherwise the handle another thread published
    // while this one was still loading and uploading.
    //
    // Uploading outside the lock is what makes the losing case reachable, and it
    // is deliberate — holding the exclusive lock across a multi-millisecond
    // upload would stall every cache hit in the extraction wave behind it. A
    // caller whose candidate was not adopted therefore owns a duplicate GPU
    // texture and must retire it; overwriting instead would orphan the handle
    // other callers are already using.
    Rendering::TextureHandle PublishOrAdopt(const GUID& guid, Rendering::TextureHandle candidate)
    {
        std::unique_lock lock(m_Mutex);
        return m_Map.emplace(guid, candidate).first->second;
    }

    // Unconditional overwrite, for the render-thread paths that already own the
    // entry (the upload drain, and the hot-swap restore whose failed re-upload
    // may have left a negative entry behind).
    void Set(const GUID& guid, Rendering::TextureHandle handle)
    {
        std::unique_lock lock(m_Mutex);
        m_Map[guid] = handle;
    }

    bool Remove(const GUID& guid, Rendering::TextureHandle& out)
    {
        std::unique_lock lock(m_Mutex);
        const auto it = m_Map.find(guid);
        if (it == m_Map.end())
            return false;
        out = it->second;
        m_Map.erase(it);
        return true;
    }

    // Move the entries out and leave the cache empty, so shutdown can destroy
    // the textures without holding the lock across IDevice calls.
    std::unordered_map<GUID, Rendering::TextureHandle> Take()
    {
        std::unique_lock lock(m_Mutex);
        std::unordered_map<GUID, Rendering::TextureHandle> taken;
        taken.swap(m_Map);
        return taken;
    }

    // Drop every entry without destroying anything (the handles are already dead
    // after a device rebuild).
    void Clear()
    {
        std::unique_lock lock(m_Mutex);
        m_Map.clear();
    }

  private:
    mutable std::shared_mutex m_Mutex;
    std::unordered_map<GUID, Rendering::TextureHandle> m_Map;
};

} // namespace Engine::Renderer
} // namespace GameEngine
