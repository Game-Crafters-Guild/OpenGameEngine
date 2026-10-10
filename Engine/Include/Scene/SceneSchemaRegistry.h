#pragma once

#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "ECS/Entity.h"
#include "Scene/SceneIOContext.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Scene
{

// A component schema teaches the scene system how to:
// - serialize a component into "Component.property = value" lines
// - apply parsed properties to an entity during load
// - optionally remove the component via "-Component" directives (blueprint overrides)
class ISceneComponentSchema
{
  public:
    virtual ~ISceneComponentSchema() = default;

    // The canonical schema name used in files (e.g. "Transform", "Name").
    virtual std::string_view GetComponentName() const = 0;

    // The reflected component type this schema owns, when its file token is not that type's
    // reflected name (the "Spline" token serializes SplineComponent). The registry treats the
    // owned type as claimed: the save loop's reflection fallback never writes it a second time
    // under its reflected name, and that reflected name resolves to no schema on load. Schemas
    // whose token equals their component's reflected name claim it by the token alone and keep
    // the default.
    virtual ECS::ComponentTypeId GetOwnedComponentType() const { return 0; }

    // Append 0+ lines (without trailing '\n') for this component if present on entity.
    // The save context exposes asset resolver / asset root state needed by
    // helpers like FormatAssetReferenceForSave; pass an empty SceneSaveContext
    // when calling outside SaveSceneToFile (e.g. presence probes).
    virtual void Serialize(const ECS::World& world,
                           ECS::EntityHandle entity,
                           const SceneSaveContext& ctx,
                           std::vector<std::string>& outLines) const = 0;

    // Returns whether the component exists on the entity.
    // Default implementation infers presence from whether Serialize emits any lines.
    // Schemas for "tag" components (no properties) should override this to avoid false negatives.
    virtual bool IsPresent(const ECS::World& world, ECS::EntityHandle entity) const
    {
        SceneSaveContext stubCtx{};
        std::vector<std::string> tmp;
        Serialize(world, entity, stubCtx, tmp);
        return !tmp.empty();
    }

    // Apply a single property assignment to an entity.
    // Returns false if the value is invalid.
    // The load context exposes [resource]/[embed] tables and the asset
    // resolver needed by helpers like TryResolveAssetReference; pass an
    // empty SceneLoadContext when calling outside LoadSceneFromFile (e.g.
    // tools that clear an asset slot via the integer-zero sentinel).
    virtual bool ApplyProperty(ECS::World& world,
                               ECS::EntityHandle entity,
                               const SceneLoadContext& ctx,
                               std::string_view property,
                               std::string_view value,
                               std::string* outError) const = 0;

    // Apply a whole component's property assignments in one call. Lets a schema create the
    // component and capture/apply its bytes once for the group rather than per property.
    // Semantics match calling ApplyProperty for each pair in order: skips (unknown field,
    // transient, no serializer) keep going and the call still succeeds; a malformed value
    // returns false with the same error string ApplyProperty would set. On failure,
    // `*outFailedIndex` (when non-null) receives the index of the offending pair so the
    // caller can report its source line. The default impl loops ApplyProperty, so schemas
    // that do not override this behave identically.
    //
    // One sanctioned divergence: a schema may accept a value that is split across several
    // properties (Animator's embedded model/index pair) only here, as a group, and refuse
    // each half in ApplyProperty. SceneIO's per-property fallback after a failed group
    // then records the halves as degradation skips instead of installing half a value.
    virtual bool ApplyProperties(ECS::World& world,
                                 ECS::EntityHandle entity,
                                 const SceneLoadContext& ctx,
                                 std::span<const std::pair<std::string_view, std::string_view>> props,
                                 std::string* outError,
                                 std::size_t* outFailedIndex) const
    {
        for (std::size_t i = 0; i < props.size(); ++i)
        {
            if (!ApplyProperty(world, entity, ctx, props[i].first, props[i].second, outError))
            {
                if (outFailedIndex)
                    *outFailedIndex = i;
                return false;
            }
        }
        return true;
    }

    // Ensure the component exists on the entity using default values.
    // Used for blueprint override "ComponentName" / "+ComponentName" additions.
    virtual bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const
    {
        (void)world;
        (void)entity;
        if (outError)
            *outError = "AddDefault not supported for this component";
        return false;
    }

    // Remove this component from entity (used for "-Component" directives).
    // Default implementation returns false (not supported).
    virtual bool Remove(ECS::World& world, ECS::EntityHandle entity) const
    {
        (void)world;
        (void)entity;
        return false;
    }

    // Visit each external AssetReference held by this component. Used by tools
    // (missing-asset detection, dependency graph builders) that need to walk a
    // scene's asset references without round-tripping through Serialize text.
    // Default impl is empty — schemas that hold no asset references can ignore
    // this. Schemas that do should call `visitor` once per asset slot, passing
    // the GUID, the asset type, the authored path (may be empty), and the
    // property name (e.g. "material", "clip", "HDRI").
    using AssetRefVisitor = std::function<void(const GUID& guid,
                                               AssetType type,
                                               std::string_view authoredPath,
                                               std::string_view propertyName)>;
    virtual void EnumerateAssetReferences(const ECS::World& world,
                                          ECS::EntityHandle entity,
                                          const AssetRefVisitor& visitor) const
    {
        (void)world;
        (void)entity;
        (void)visitor;
    }
};

// Global registry of component schemas (module-extensible).
class SceneSchemaRegistry
{
  public:
    static void Register(std::unique_ptr<ISceneComponentSchema> schema);

    // Find a hand-written schema by name; if none exists, fall back to a synthesized
    // reflection schema for a component reflected under that name (ComponentFieldRegistry).
    // Returns null when the name matches neither a hand-written schema nor a reflected type,
    // and for the reflected name of a type a hand-written schema owns under another token
    // (see GetOwnedComponentType): that type has exactly one schema, reached by its token.
    static const ISceneComponentSchema* Find(std::string_view componentName);

    // True when a hand-written schema is registered under `componentName`; never synthesizes a
    // reflection schema. Registration guards use this so a reflected type's fallback can never
    // stand in for the built-in schema about to be registered.
    static bool HasRegistered(std::string_view componentName);

    // The hand-written schema that serializes `typeId`: one registered under the type's reflected
    // simple name, or one that owns the type through GetOwnedComponentType. Null when the type is
    // covered by reflection alone (or not reflected at all).
    static const ISceneComponentSchema* FindForComponentType(ECS::ComponentTypeId typeId);

    // The registered component type a schema's token carries: the type the schema owns, else
    // the reflected type of that simple name, else the registered unreflected type whose
    // canonical name ends in it. 0 when no registered type carries the token. A resolution is
    // cached per token; a token that resolved to nothing is looked up again next time, so a
    // type that registers later is found.
    static ECS::ComponentTypeId ComponentTypeForSchema(const ISceneComponentSchema& schema);

    // The token under which a registered component type appears in a file: its hand-written
    // schema's, else its reflection fallback's. Empty when nothing saves the type.
    static std::string_view SchemaTokenForComponentType(ECS::ComponentTypeId typeId);

    static std::vector<const ISceneComponentSchema*> GetAllSorted();

    // Save-side reflection fallback: returns a synthesized reflection schema for `typeId` ONLY when
    // the type is reflected AND no hand-written schema serializes it (FindForComponentType is null),
    // so the save loop serializes reflected components a built-in schema doesn't already cover.
    // Returns null otherwise. The scene save loop calls this per component on each entity, after
    // the hand-written schema pass.
    static const ISceneComponentSchema* ReflectionSchemaForUnhandledType(ECS::ComponentTypeId typeId);

    // C12 load-abort purge / unload quiesce ledger (see ECS/ModuleRegistration.h).
    // Hand-written schema objects live in the registering module's image: purge
    // drops every schema stamped with exactly {moduleId, generation} before that
    // image unmaps; a schema still stamped with an older generation blocks the
    // superseded image's unload.
    static std::size_t PurgeModuleSchemas(std::string_view moduleId, std::uint64_t generation);
    static std::size_t CountSupersededModuleSchemas(std::string_view moduleId,
                                                    std::uint64_t currentGeneration);
};

// Register the built-in scene component schemas (Transform, Camera, MeshRenderer, Physics, etc.).
// This is their only registration; EngineCore::Initialize calls it, so every lookup after engine
// initialization finds them. Safe to call multiple times (idempotent).
void RegisterBuiltInSceneSchemas();

// Register the built-in schemas and, once, the schemas of the engine plugins registered so far.
// Call before loading or saving a scene. Safe to call multiple times (idempotent).
void EnsureBuiltInSchemasRegistered();

// Convenience auto-registrar for static initialization in modules.
template <typename TSchema>
struct SceneSchemaAutoRegistrar
{
    SceneSchemaAutoRegistrar()
    {
        SceneSchemaRegistry::Register(std::make_unique<TSchema>());
    }
};

#define GE_REGISTER_SCENE_SCHEMA(SchemaType) \
    namespace { [[maybe_unused]] static ::GameEngine::Scene::SceneSchemaAutoRegistrar<SchemaType> SchemaType##_scene_schema_registrar; }

} // namespace GameEngine::Scene

