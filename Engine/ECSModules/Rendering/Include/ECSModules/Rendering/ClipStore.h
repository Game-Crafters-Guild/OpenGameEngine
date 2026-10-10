#pragma once

#include "Assets/AnimationClip.h"   // shim that exposes ::GameEngine::AnimationClip via using-alias
#include "Types/Types.h"
#include "AssetCore/GUID.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace GameEngine {
class AssetManager;
}

namespace GameEngine { namespace Engine { namespace Renderer {

// Store for AnimationClips with numeric indices, for ECS-friendly AnimatorRef usage.
class ClipStore {
public:
    static ClipStore& Instance();

    // Returns a numeric clip index for the GUID, loading it via AssetManager if needed.
    uint32 GetOrLoadClipIndex(const GUID& guid, AssetManager& am);

    // Register a clip that was created at runtime (e.g., extracted from a container).
    // If the GUID already exists, replaces the stored clip and returns that index.
    uint32 RegisterRuntimeClip(const GUID& guid, SharedPtr<AnimationClip> clip);

    // Insert only when the GUID is absent. Returns the new 1-based index, or 0
    // if the GUID was already published (`clip` is not consumed). One lock so a
    // worker cannot first-insert then clobber a concurrent first-insert.
    uint32 TryInsertRuntimeClip(const GUID& guid, SharedPtr<AnimationClip>& clip);

    // Direct access by index (1-based IDs; 0 invalid)
    SharedPtr<AnimationClip> Get(uint32 clipIndex);

    // Retrieve index if already registered (returns 0 if absent)
    uint32 GetIndexIfPresent(const GUID& guid) const;

    // Testing utility: clears all registered clips (runtime cache only)
    void ClearForTest();

private:
    ClipStore() = default;

    // Guards RegisterRuntimeClip replace of a published slot (model bake
    // reload) against concurrent Get() copies of the same shared_ptr.
    mutable std::mutex m_Mutex;
    std::deque<SharedPtr<AnimationClip>> m_Clips;  // index = id-1; deque for pointer stability
    std::atomic<size_t> m_ClipCount{0};            // size published after emplace_back; Get() also locks
    std::unordered_map<GUID, uint32> m_GuidToIndex;
};

}}} // namespace GameEngine::Engine::Renderer

