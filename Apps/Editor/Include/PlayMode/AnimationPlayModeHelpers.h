#pragma once

#include "AssetCore/GUID.h"
#include "ECS/Entity.h"
#include "Types/Types.h"

#include <filesystem>
#include <vector>

namespace GameEngine { class Asset; class AssetManager; }
namespace GameEngine::ECS { class World; }

namespace GameEngine::Editor
{

// Collect all entities in the hierarchy rooted at `root` (including root itself).
void CollectEntitiesInSubtree(ECS::World& world, ECS::EntityHandle root,
                              std::vector<ECS::EntityHandle>& out);

// Find the first model GUID from MeshRenderer components in the subtree.
GUID FindFirstModelGuidInSubtree(ECS::World& world,
                                 const std::vector<ECS::EntityHandle>& subtree);

// Set AnimatorRef to rest state (ClipIndex=0, Time=0, paused) on all animated
// entities in the subtree.
void ApplyAnimatorRestState(ECS::World& world,
                            const std::vector<ECS::EntityHandle>& subtree);

// Assign a clip index to all AnimatorRef entities in the subtree (looping, unpaused).
// If clipIndex is 0, delegates to ApplyAnimatorRestState.
void ApplyAnimatorPlayState(ECS::World& world,
                            const std::vector<ECS::EntityHandle>& subtree,
                            uint32_t clipIndex,
                            float32 timeSeconds = 0.0f,
                            float32 speedScale = 1.0f,
                            bool loop = true,
                            float32 blendSeconds = 0.0f,
                            bool section = false,
                            float32 sectionStartSeconds = 0.0f,
                            float32 sectionEndSeconds = 0.0f);

// Load a model asset by GUID with a bounded wait. Returns nullptr on failure.
SharedPtr<Asset> TryAcquireModelAsset(AssetManager& am, const GUID& modelGuid);

// Resolve a clip GUID to a ClipStore index. Embedded clips are eagerly
// registered by ModelAsset at load time; standalone .anim/.fbx clip assets
// are loaded on demand via the AssetManager. Returns 0 on failure.
uint32_t ResolveClipIndex(const GUID& clipGuid, AssetManager& am);

// First embedded clip of the model attached to this entity's subtree, after
// ensuring the model asset is loaded. Both GUIDs are null when unavailable.
struct DefaultEmbeddedClip
{
    GUID modelGuid;
    GUID clipGuid;
};
DefaultEmbeddedClip ResolveDefaultEmbeddedClip(ECS::World& world, ECS::EntityHandle rootEntity,
                                               AssetManager& am);

// Resolve clip GUID to ClipStore index, loading the entity's source model first
// so embedded derived GUIDs are registered into the ClipStore.
uint32_t ResolveClipIndexForEntity(ECS::World& world, ECS::EntityHandle rootEntity,
                                   const GUID& clipGuid, AssetManager& am);

} // namespace GameEngine::Editor
