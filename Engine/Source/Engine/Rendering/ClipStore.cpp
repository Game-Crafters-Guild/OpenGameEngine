#include "ECSModules/Rendering/ClipStore.h"
#include "Assets/AnimationClip.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Logger/Logger.h"

namespace GameEngine { namespace Engine { namespace Renderer {

ClipStore& ClipStore::Instance() {
    static ClipStore g{}; return g;
}

uint32 ClipStore::GetOrLoadClipIndex(const GUID& guid, AssetManager& am) {
    if (guid.IsNull()) return 0;

    {
        std::lock_guard lock(m_Mutex);
        auto it = m_GuidToIndex.find(guid);
        if (it != m_GuidToIndex.end()) return it->second;
    }

    // A clip asset of its own (.anim) is a small file with no import, so it is read here. An
    // embedded clip's derived GUID loads nothing on its own: its container registers it into this
    // store as it loads, and playback resolves that container first without waiting
    // (ResolvePlaybackClipIndex). m_Mutex is not held across the load, since a model loading on
    // another thread registers its clips here.
    auto future = am.LoadAssetAsync(guid);
    SharedPtr<Asset> base = future.get();

    {
        std::lock_guard lock(m_Mutex);
        auto it = m_GuidToIndex.find(guid);
        if (it != m_GuidToIndex.end()) return it->second;
    }

    SharedPtr<AnimationClip> clip = std::dynamic_pointer_cast<AnimationClip>(base);
    if (!clip) {
        Logger::Log::Error("ClipStore: GUID {} is not an AnimationClip", guid.ToCompactString());
        return 0;
    }

    std::lock_guard lock(m_Mutex);
    auto it = m_GuidToIndex.find(guid);
    if (it != m_GuidToIndex.end()) return it->second;

    m_Clips.emplace_back(clip);
    m_ClipCount.store(m_Clips.size(), std::memory_order_release);
    uint32 idx = static_cast<uint32>(m_Clips.size());
    m_GuidToIndex.emplace(guid, idx);
    return idx;
}

uint32 ClipStore::RegisterRuntimeClip(const GUID& guid, SharedPtr<AnimationClip> clip) {
    if (guid.IsNull() || !clip) return 0;

    std::lock_guard lock(m_Mutex);
    auto it = m_GuidToIndex.find(guid);
    if (it != m_GuidToIndex.end())
    {
        // Same derived GUID on model reload (bake/mirror kv). Swap the
        // pointer so AnimatorRef indices keep working against new keys.
        const uint32 idx = it->second;
        const size_t slot = static_cast<size_t>(idx - 1);
        if (slot < m_Clips.size())
            m_Clips[slot] = std::move(clip);
        return idx;
    }
    m_Clips.emplace_back(std::move(clip));
    m_ClipCount.store(m_Clips.size(), std::memory_order_release);
    uint32 idx = static_cast<uint32>(m_Clips.size());
    m_GuidToIndex.emplace(guid, idx);
    return idx;
}

uint32 ClipStore::TryInsertRuntimeClip(const GUID& guid, SharedPtr<AnimationClip>& clip)
{
    if (guid.IsNull() || !clip)
        return 0;

    std::lock_guard lock(m_Mutex);
    auto it = m_GuidToIndex.find(guid);
    if (it != m_GuidToIndex.end())
        return 0;
    m_Clips.emplace_back(std::move(clip));
    m_ClipCount.store(m_Clips.size(), std::memory_order_release);
    uint32 idx = static_cast<uint32>(m_Clips.size());
    m_GuidToIndex.emplace(guid, idx);
    return idx;
}

SharedPtr<AnimationClip> ClipStore::Get(uint32 clipIndex) {
    if (clipIndex == 0) return nullptr;
    std::lock_guard lock(m_Mutex);
    size_t i = static_cast<size_t>(clipIndex - 1);
    if (i >= m_Clips.size()) return nullptr;
    return m_Clips[i];
}

uint32 ClipStore::GetIndexIfPresent(const GUID& guid) const {
    std::lock_guard lock(m_Mutex);
    auto it = m_GuidToIndex.find(guid);
    if (it == m_GuidToIndex.end()) return 0;
    return it->second;
}

void ClipStore::ClearForTest() {
    std::lock_guard lock(m_Mutex);
    m_Clips.clear();
    m_ClipCount.store(0, std::memory_order_release);
    m_GuidToIndex.clear();
}

}}} // namespace GameEngine::Engine::Renderer
