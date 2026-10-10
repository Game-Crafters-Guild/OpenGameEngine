#pragma once

#include "AssetCore/AssetRegistry.h" // AssetReference
#include "AssetCore/GUID.h"
#include "ECS/Entity.h" // ECS::EntityHandle, ECS::EntityId
#include "Scene/SceneAssetResolver.h"
#include "Scene/SceneEmbedMaterializer.h"

#include <filesystem>
#include <string>
#include <unordered_map>

namespace GameEngine::Scene
{
// Per-scene-load state threaded through schema dispatch (ApplyProperty) and the
// SceneIO load helpers. Replaces the previous thread-local active-context
// pattern: schemas now receive ctx by const reference, so the helpers they
// call (TryResolveResourceIdToAssetReference, TryGetEmbedDefinition,
// TryResolveAssetReference) have no hidden TLS coupling.
struct SceneLoadContext
{
    const std::filesystem::path* SceneFile = nullptr;
    std::filesystem::path AssetRoot;
    ISceneAssetResolver* Resolver = nullptr;             // not owned
    ISceneEmbedMaterializer* EmbedMaterializer = nullptr; // not owned

    // Per-load tables — populated at load start, consumed by ResolveResourceId helper.
    std::unordered_map<std::string, std::filesystem::path> ResourceAbsPathById;
    std::unordered_map<std::string, std::string> ResourceGuidById;

    struct EmbedDef
    {
        std::string Type;
        // Persisted embed identity (`guid=` on the [embed] header). Empty for files
        // authored before the attribute existed; those fall back to deriving the
        // identity from the containing scene's GUID.
        std::string Guid;
        std::unordered_map<std::string, std::string> Properties;
    };
    std::unordered_map<std::string, EmbedDef> EmbedsById;

    // Per-load embed cache (embedGuid -> resolved AssetReference). mutable so
    // const helpers can update the cache while keeping ctx const-by-ref.
    mutable std::unordered_map<GUID, AssetReference> EmbedCache;

    // Entity-reference resolution for EntityHandle fields. EntityIdMap maps a scene
    // entity-id string -> the live handle created this load; it is fully populated
    // before any component is applied, so forward references resolve directly.
    // EntityIdPrefix namespaces ids for instanced blueprints/subscenes.
    const std::unordered_map<std::string, ECS::EntityHandle>* EntityIdMap = nullptr;
    std::string EntityIdPrefix;

    // The world being loaded into, where TryResolveAssetReference records a reference whose GUID
    // names no asset, with its authored path (UnresolvedComponentStore), so a save writes the path
    // back. Null for an apply that writes no scene text: such a reference keeps its GUID alone.
    ECS::World* TargetWorld = nullptr; // not owned

    // Component slots a retired-schema fold owns for this load, keyed by the live
    // component's schema name. A retired block that folds into a live component
    // (TerrainCorridorEffect -> TerrainFlattenEffect) claims the slot; the live
    // component's schema drops later blocks for a claimed slot — warning once, via
    // Warned — so the fold's outcome does not depend on which block the file lists
    // first. mutable for the same reason as EmbedCache: schemas receive ctx by
    // const reference.
    struct RetiredFoldClaim
    {
        bool Warned = false;
    };
    mutable std::unordered_map<std::string, std::unordered_map<ECS::EntityId, RetiredFoldClaim>>
        RetiredFoldClaims;
};

// Per-scene-save state threaded through schema dispatch (Serialize) and
// FormatAssetReferenceForSave. Mirrors SceneLoadContext on the save side.
struct SceneSaveContext
{
    const std::filesystem::path* SceneFile = nullptr;
    std::filesystem::path AssetRoot;
    ISceneAssetResolver* Resolver = nullptr; // not owned

    // Maps a live entity id -> its saved entity-id string (tag or "e_<id>"), so an
    // EntityHandle field serializes a stable reference instead of a runtime id.
    const std::unordered_map<ECS::EntityId, std::string>* EntityTagById = nullptr;

    // The saved world's unresolved scene data, if it has any: FormatAssetReferenceForSave reads the
    // authored path of a reference whose GUID still names no asset from it.
    const ECS::UnresolvedComponentStore* Unresolved = nullptr; // not owned
};

} // namespace GameEngine::Scene
