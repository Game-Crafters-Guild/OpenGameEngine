#pragma once

// ModelRenderSetup: shared utility functions for preparing model assets
// for rendering. Used by ModelEntityFactory (drag-drop entity creation),
// MeshRendererInspector (inspector model assignment) and the runtime resolve
// (SceneResolveService).

#include "AssetCore/GUID.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/LocalBounds.h"
#include "ECS/ECS.h" // ECS::EntityHandle (by value in UnresolvedModelEntity)
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/ModelMaterialBridge.h"
#include "Types/Types.h"

#include <optional>
#include <unordered_map>
#include <vector>

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::MeshGPUHandle;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine
{
class Asset;
class ModelAsset;
class AssetManager;
namespace ECS { class World; struct ChangeGate; }

namespace Engine::Renderer
{
struct SkeletonResolveState;
class RenderServices;
class SceneResolveService;

// Result of registering a model's render resources (meshes + materials).
struct ModelRenderResources
{
    GUID modelGuid;
    const ModelAsset* modelAsset = nullptr;
    std::vector<Rendering::MeshGPUHandle> meshHandles;
    std::vector<ConvertedModelMaterial> materials;
};

// Load a model asset by GUID, register its GPU meshes and converted materials.
// Returns nullopt (with warning log) if the asset fails to load or isn't a ModelAsset.
// Waits for a model that is not resident: authoring tools only, never a frame path.
std::optional<ModelRenderResources> RegisterModelRenderResources(
    RenderServices& rs, const GUID& modelGuid);

// Register an already loaded model's GPU meshes and converted materials. Never
// loads and never waits; nullopt (with a warning) when the asset is not a loaded
// ModelAsset. The runtime resolve's form.
std::optional<ModelRenderResources> RegisterLoadedModelRenderResources(
    RenderServices& rs, const GUID& modelGuid, Asset& loadedAsset);

// Same as `RegisterModelRenderResources` but only registers (and uploads
// textures for) the material slot that `submeshIndex` references. Use when
// you know only one submesh of the model will be placed (e.g. drag-drop in
// the inspector). The returned `materials` vector still lists every model
// material slot so `PopulateMeshRenderer` can index correctly; the others
// are simply not pushed into MaterialRegistry / GPU texture storage.
std::optional<ModelRenderResources> RegisterModelRenderResourcesForSubmesh(
    RenderServices& rs, const GUID& modelGuid, uint32 submeshIndex);

// Resolve which submesh of `model` a MeshRenderer targets. Explicit positional
// `meshId` wins; otherwise a non-zero `MeshNameId` selects the submesh whose
// (case-folded) name matches — how a scene importer that cannot know the
// positional index (its source format addresses a mesh by file id, not by index) addresses one submesh of
// a multi-submesh model. A match interns the matched submesh name so a later
// save can write it. A name miss falls back to submesh 0 (still binds) and logs
// one diagnostic per (model, name), naming the mesh it wanted. The default
// scene-load case (meshId == 0, MeshNameId == 0) resolves to submesh 0 — never a skip.
uint32 ResolveSubmeshIndex(const GUID& modelGuid, const ModelAsset& model,
                           const Components::MeshRenderer& mr);

// Policy for how PopulateMeshRenderer sets MeshRenderer.materialAssetGuid.
enum class MeshMaterialFill
{
    // Overwrite the material with the submesh's embedded model material. Used by
    // authoring flows (drag-drop instantiation, inspector model assignment) where
    // assigning the model implies taking the model's own material.
    FromModel,
    // Keep an already-set explicit material; fill from the embedded model material
    // only when the MeshRenderer carries none. Used by the scene-load resolve pass
    // (a deserialized material must win over the model's per-submesh default) and
    // by the spline piece binder (a recipe's override material seeds the field).
    PreserveExplicit,
};

// Populate a MeshRenderer's GPU fields from registered resources for a given submesh.
// Always sets meshGpuHandleId and modelAssetGuid; the material follows `materialFill`.
void PopulateMeshRenderer(Components::MeshRenderer& mr,
                          const ModelRenderResources& resources,
                          uint32 submeshIndex,
                          MeshMaterialFill materialFill);

// Compute the enclosing bounding sphere from the model asset's aggregate bounding box.
// Returns a default LocalBounds if the asset has no meshes.
Components::LocalBounds ComputeModelBounds(const ModelAsset& modelAsset);

// After a scene load, iterate all MeshRenderer components that have a modelAssetGuid
// but no meshGpuHandleId and resolve them by loading the model asset and registering
// GPU meshes/materials. Waits for every load: the editor's synchronous additive
// open only. Every runtime path resolves through SceneResolveService.
void ResolveModelMeshRenderers(ECS::World& world, RenderServices& rs);

// Runtime bind: hands the MeshRenderers written since `gate` that still lack a
// GPU handle or a registered standalone material to `resolves`, which binds an
// entity of an already registered model at once and holds the rest for its
// later steps; nothing here waits for a load. Call from the main thread
// (RenderingLoop) before extraction waves. While the editor's scene-open build
// runs (IsSceneBuildPumpActive) this returns without consuming the changes.
void BindChangedMeshRenderers(ECS::World& world, RenderServices& rs, ECS::ChangeGate& gate,
                              SceneResolveService& resolves);

// True while the editor's scene-open build (SceneBuildPump) is mid-resolve. It is
// the only setter; the engine's SceneResolveService never sets it.
// BindChangedMeshRenderers and the skeleton binder hold their changes while it is set.
bool IsSceneBuildPumpActive();

// One entity to resolve: a MeshRenderer carrying a model GUID but no GPU handle.
struct UnresolvedModelEntity
{
    ECS::EntityHandle Entity;
    GUID ModelGuid;
};

// Per-model resolve cache: a model's registered resources are reused across every
// entity that references it, so a scene of N entities over K models loads K models.
using ModelResolveCache = std::unordered_map<GUID, std::optional<ModelRenderResources>>;

// Collect the entities ResolveModelMeshRenderers would resolve.
std::vector<UnresolvedModelEntity> CollectUnresolvedModelEntities(ECS::World& world);

// Resolve a single collected entity: register (once, cached; waiting for a model
// that is not resident) the model's GPU resources and populate the entity. The
// per-item body of ResolveModelMeshRenderers.
void ResolveOneModelEntity(ECS::World& world, RenderServices& rs, ECS::EntityHandle entity,
                           const GUID& modelGuid, ModelResolveCache& cache);

// Populate `entity`'s MeshRenderer and the components the model implies
// (SkinnedMeshRenderer for a skinned submesh, MorphTargetWeights, LocalBounds,
// WorldTransform) from registered resources. Never loads.
void ApplyModelRenderResources(ECS::World& world, RenderServices& rs, ECS::EntityHandle entity,
                               const ModelRenderResources& resources);

// Collect the standalone .material GUIDs referenced by MeshRenderers that are not
// yet in the runtime MaterialRegistry (model-embedded materials are already
// registered by the model resolve, so run this AFTER the model pass to skip them).
std::vector<GUID> CollectStandaloneMaterialGuids(ECS::World& world, RenderServices& rs);

// Load and register one standalone .material asset, waiting for it when it is not
// resident (authoring tools only). A GUID that resolves to a non-MaterialAsset (a
// model-derived material) is skipped — the model pass owns it.
void RegisterOneStandaloneMaterial(RenderServices& rs, const GUID& materialGuid);

// Register a standalone .material asset that has already loaded. Returns whether it
// was registered: false for a null asset (with a warning) and for a model-derived
// material, which its model registers.
bool RegisterLoadedStandaloneMaterial(RenderServices& rs, const GUID& materialGuid, Asset* asset);

// After a scene load, ensure every MaterialAsset GUID referenced by a MeshRenderer
// is registered in the runtime MaterialRegistry, waiting for each load (the editor's
// synchronous additive open only). Without this, standalone .material
// files (those not embedded in a model) would never reach the renderer until the
// user opened them in the inspector — RenderExtractionSystem skips meshes whose
// material isn't in the registry, leaving them invisible until something forced
// a registration (e.g. toggling a property).
void ResolveStandaloneMaterials(ECS::World& world, RenderServices& rs);

// Reconstruct model-backed skeleton caches, sharing a runtime across each model
// instance's meshes and animated helper nodes. Never waits: a model source that is
// not resident is requested and stays unresolved until it lands, as do missing
// instance owners; explicit procedural assignments without a model source remain
// supported in memory. Collects before mutation. Resolves the whole world once,
// looking models up in `assetManager`.
void ResolveSkinnedMeshAnimation(ECS::World& world, AssetManager& assetManager);

// Main-thread hydration before animation/extraction waves, every frame. Direct
// SceneIO loads and play-spawned components use the same path as editor document
// loads. Never waits for a model. An entity `resolves` still holds is skipped
// until the service completes it, and binds once on the first tick after; the
// editor's scene-open build holds every change until it finishes.
void BindChangedSkinnedMeshAnimation(ECS::World& world, SkeletonResolveState& state,
                                     const SceneResolveService& resolves);
void BindChangedSkinnedMeshAnimation(ECS::World& world, SkeletonResolveState& state,
                                     const SceneResolveService& resolves, AssetManager& assets);

} // namespace Engine::Renderer
} // namespace GameEngine
