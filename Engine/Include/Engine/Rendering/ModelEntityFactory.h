#pragma once

// ModelEntityFactory: creates ECS entities from a loaded ModelAsset, wiring up
// all rendering components (MeshRenderer, SkinnedMeshRenderer, AnimatorRef,
// SkeletonRef) and registering GPU resources (mesh buffers, materials) through
// the appropriate registries.
//
// This is the primary entry point for instantiating models in the world.
// It handles:
//   - Single-submesh models: one entity with MeshRenderer
//   - Multi-submesh models: parent entity + child entities per submesh
//   - Skinned models: SkinnedMeshRenderer + SkeletonRef + optional AnimatorRef
//   - Material registration: ImportedMaterialData -> MaterialDocument -> MaterialRegistry
//   - GPU mesh registration: ModelAsset meshes -> MeshGPURegistry
//
// Ownership:
//   - Stateless utility. Does NOT own the created entities, GPU resources, or materials.
//   - GPU resources are owned by MeshGPURegistry and MaterialRegistry via RenderServices.
//   - Created entities are owned by the ECS World.
//
// Thread safety:
//   - Must be called from the main thread (ECS entity creation is not thread-safe).

#include "AssetCore/GUID.h"
#include "ECS/Entity.h"
#include "Types/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine
{
class ModelAsset;
namespace ECS { class World; }

namespace Engine::Renderer
{
class RenderServices;

// Result of creating entities from a model.
struct ModelEntityResult
{
    // The root entity; its Transform starts at identity, places the model and
    // is never written by the model's animation. For a single rigid submesh
    // drawn once, on an unanimated node at identity, this IS the renderable
    // entity; otherwise it is the parent with Transform only.
    ECS::EntityHandle rootEntity;

    // All renderable entities (one per node that draws a submesh). When the
    // root is the renderable entity, this contains only the root entity.
    std::vector<ECS::EntityHandle> submeshEntities;

    // Whether the model was skinned (SkinnedMeshRenderer used instead of MeshRenderer).
    bool skinned = false;

    bool IsValid() const { return rootEntity.IsValid(); }
};

struct ModelEntityFactoryOptions
{
    bool SpawnImportedCameras = true;
    bool SpawnImportedLights = true;
    bool SpawnImportedHelperNodes = true;
};

class ModelEntityFactory
{
  public:
    // Create entities in the given World from a loaded ModelAsset.
    //
    // `rs` is used to register GPU meshes and materials.
    // `world` receives the created entities.
    // `modelAsset` must be loaded (IsLoaded() == true).
    // `modelGuid` is the asset identity (used for GPU resource deduplication).
    // `rootName` is the display name for the root entity (optional).
    //
    // Returns a ModelEntityResult describing all created entities and resources.
    static ModelEntityResult CreateFromModel(RenderServices& rs,
                                              ECS::World& world,
                                              const ModelAsset& modelAsset,
                                              const GUID& modelGuid,
                                              const std::string& rootName = "",
                                              const ModelEntityFactoryOptions& options = ModelEntityFactoryOptions{});
};

} // namespace Engine::Renderer
} // namespace GameEngine
