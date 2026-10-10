#include "Scene/SceneSchemaRegistry.h"
#include "Scene/SceneValue.h"
#include "Types/StringUtils.h"

#include "Components/Animation/Animator.h"
#include "Assets/ModelAsset.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "Components/Animation/ValueCurve.h"
#include "Components/Audio/AudioEmitter.h"
#include "Components/Audio/AudioListener.h"
#include "Components/GameLogicGraphRef.h"
#include "Components/Hierarchy.h"
#include "Components/Measure/MeasureComponent.h"
#include "Components/Name.h"
#include "Components/Rendering/Camera.h"
#include "Components/Rendering/LODGroup.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/MorphTargetWeights.h"
#include "Components/Rendering/PostProcessEffects/BloomEffect.h"
#include "Components/Rendering/PostProcessEffects/ExposureAdjustmentEffect.h"
#include "Components/Rendering/PostProcessEffects/AtmosphericCloudLayer.h"
#include "Components/Rendering/PostProcessEffects/VolumetricClouds.h"
#include "Components/Rendering/PostProcessEffects/CubeLutEffect.h"
#include "Components/Rendering/PostProcessEffects/HeatDistortionEffect.h"
#include "Components/Rendering/PostProcessEffects/HeightFogEffect.h"
#include "Components/Rendering/PostProcessEffects/VolumetricFogEffect.h"
#include "Components/Rendering/AmbientLight.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/RenderLayer.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/Skybox.h"
#include "Components/Rendering/WindVolume.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "Components/Video/VideoTextureComponent.h"
#include "Core/Engine.h"
#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/MeshNameRegistry.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "PluginAPI/EnginePlugin.h"
#include "Scene/SceneIO.h" // TryResolveResourceIdToAssetReference
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"

#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/CapsuleColliderShape.h"
#include "PhysicsECS/Components/CharacterController.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/PhysicsColliderOwner.h"
#include "PhysicsECS/Components/PhysicsWorldSettingsComponent.h"
#include "PhysicsECS/Components/PlaneColliderShape.h"
#include "PhysicsECS/Components/SphereColliderShape.h"
#include "PhysicsECS/PhysicsWorldService.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace GameEngine::Scene
{
namespace
{
class NameSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "Name"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override;

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override;

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const override;
    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override;
};

class TransformSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "Transform"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override;

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override;

    // Composes the matrix once from the block's position, rotation and scale. Applying the lines
    // one at a time re-decomposes the matrix per line, and each decomposition moves the values.
    bool ApplyProperties(ECS::World& world,
                         ECS::EntityHandle entity,
                         [[maybe_unused]] const SceneLoadContext& ctx,
                         std::span<const std::pair<std::string_view, std::string_view>> props,
                         std::string* outError,
                         std::size_t* outFailedIndex) const override;

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const override;
    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override;
};

class HierarchyOrderSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "HierarchyOrder"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* o = world.GetComponent<Components::HierarchyOrder>(entity);
        if (!o)
            return;
        outLines.push_back(std::string("HierarchyOrder.order = ") + std::to_string(o->order));
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        if (property != "order")
        {
            if (outError)
                *outError = "Unknown property";
            return false;
        }

        SceneValue v;
        std::string err;
        if (!ParseValue(value, v, &err) || v.Kind != SceneValueKind::Int)
        {
            if (outError)
                *outError = err.empty() ? "HierarchyOrder.order must be an int" : err;
            return false;
        }
        if (v.IntValue < std::numeric_limits<std::int32_t>::min() || v.IntValue > std::numeric_limits<std::int32_t>::max())
        {
            if (outError)
                *outError = "HierarchyOrder.order out of range";
            return false;
        }

        Components::HierarchyOrder o{};
        o.order = static_cast<std::int32_t>(v.IntValue);
        world.AddComponentImmediate(entity, o);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        Components::HierarchyOrder o{};
        world.AddComponentImmediate(entity, o);
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::HierarchyOrder>(entity);
        return true;
    }
};

class MeshRendererSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "MeshRenderer"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override;

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override;

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const override;
    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override;

    void EnumerateAssetReferences(const ECS::World& world,
                                  ECS::EntityHandle entity,
                                  const AssetRefVisitor& visitor) const override;
};

class MorphTargetWeightsSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "MorphTargetWeights"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override;

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override;

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const override;
    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override;
};

class PhysicsWorldSettingsSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "PhysicsWorldSettingsComponent"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override;

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override;

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const override;
    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override;
};

class PhysicsBodySchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "PhysicsBody"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override;

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override;

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const override;
    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override;
};

class CharacterControllerSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "CharacterController"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override;

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override;

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const override;
    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override;
};

class PhysicsColliderSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "PhysicsCollider"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override;

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override;

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const override;
    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override;
};

class PhysicsColliderOwnerSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "PhysicsColliderOwner"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override;

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override;

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const override;
    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override;
};

class BoxColliderShapeSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "BoxColliderShape"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override;

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override;

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const override;
    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override;
};

class SphereColliderShapeSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "SphereColliderShape"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override;

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override;

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const override;
    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override;
};

class CapsuleColliderShapeSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "CapsuleColliderShape"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override;

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override;

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const override;
    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override;
};

class PlaneColliderShapeSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "PlaneColliderShape"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override;

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override;

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const override;
    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override;
};

void NameSchema::Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const
{
    const auto* n = world.GetComponent<Components::Name>(entity);
    // Skip empty values: an empty Name is equivalent to no Name as far as the
    // editor hierarchy is concerned (the load path's auto-fallback fills it
    // from the scene id), and emitting `Name.value = ""` clutters scene
    // diffs. Default IsPresent treats this as "absent", which lets generic
    // schema-driven walks see the right state without a custom override.
    if (!n || n->value[0] == '\0')
        return;
    outLines.push_back(std::string("Name.value = ") + FormatQuoted(n->View()));
}

bool NameSchema::ApplyProperty(ECS::World& world,
                               ECS::EntityHandle entity,
                               [[maybe_unused]] const SceneLoadContext& ctx,
                               std::string_view property,
                               std::string_view value,
                               std::string* outError) const
{
    if (property != "value")
    {
        if (outError)
            *outError = "Unknown Name property";
        return false;
    }
    std::string s;
    if (!ParseQuotedString(value, s))
    {
        if (outError)
            *outError = "Name.value must be a quoted string";
        return false;
    }
    Components::Name nm{};
    std::memset(nm.value, 0, sizeof(nm.value));
    const size_t maxCopy = sizeof(nm.value) - 1;
    const size_t toCopy = std::min(maxCopy, s.size());
    std::memcpy(nm.value, s.data(), toCopy);
    nm.value[toCopy] = '\0';
    world.AddComponentImmediate(entity, nm);
    return true;
}

bool NameSchema::AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const
{
    (void)outError;
    if (world.GetComponent<Components::Name>(entity))
        return true;
    Components::Name nm{};
    std::memset(nm.value, 0, sizeof(nm.value));
    nm.value[0] = '\0';
    world.AddComponentImmediate(entity, nm);
    return true;
}

bool NameSchema::Remove(ECS::World& world, ECS::EntityHandle entity) const
{
    world.RemoveComponentImmediate<Components::Name>(entity);
    return true;
}

void TransformSchema::Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const
{
    const auto* t = world.GetComponent<Components::Transform>(entity);
    if (!t)
        return;
    const auto pos = t->GetPosition();
    const auto rot = t->GetRotation();
    const auto scl = t->GetScale();
    outLines.push_back(std::string("Transform.position = ") + FormatFloat3(pos.x, pos.y, pos.z));
    const auto& rq = rot.GetGLM();
    outLines.push_back(std::string("Transform.rotation = ") + FormatFloat4(rq.x, rq.y, rq.z, rq.w));
    outLines.push_back(std::string("Transform.scale = ") + FormatFloat3(scl.x, scl.y, scl.z));
}

// A Transform block's authored values; the component itself stores only the composed matrix.
struct TransformFields
{
    Float3 Position{0, 0, 0};
    Float4 Rotation{0, 0, 0, 1};
    Float3 Scale{1, 1, 1};
};

// The entity's current Transform decomposed, or the defaults when it has none.
static TransformFields ReadTransformFields(const ECS::World& world, ECS::EntityHandle entity)
{
    TransformFields fields;
    if (const auto* t = world.GetComponent<Components::Transform>(entity))
    {
        const auto p = t->GetPosition();
        const auto& r = t->GetRotation().GetGLM();
        const auto s = t->GetScale();
        fields.Position = {p.x, p.y, p.z};
        fields.Rotation = {r.x, r.y, r.z, r.w};
        fields.Scale = {s.x, s.y, s.z};
    }
    return fields;
}

static bool ParseTransformField(TransformFields& fields, std::string_view property, std::string_view value,
                                std::string* outError)
{
    if (property == "position")
    {
        if (ParseFloat3(value, fields.Position))
            return true;
        if (outError)
            *outError = "Transform.position must be (x, y, z)";
        return false;
    }
    if (property == "rotation")
    {
        if (ParseFloat4(value, fields.Rotation))
            return true;
        if (outError)
            *outError = "Transform.rotation must be (x, y, z, w)";
        return false;
    }
    if (property == "scale")
    {
        if (ParseFloat3(value, fields.Scale))
            return true;
        if (outError)
            *outError = "Transform.scale must be (x, y, z)";
        return false;
    }
    if (outError)
        *outError = "Unknown Transform property";
    return false;
}

static void WriteTransformFields(ECS::World& world, ECS::EntityHandle entity, const TransformFields& fields)
{
    const Components::Transform xf = Components::Transform::FromTRS(
        Mathematics::Vector3{fields.Position.X, fields.Position.Y, fields.Position.Z},
        Mathematics::Quaternion{fields.Rotation.W, fields.Rotation.X, fields.Rotation.Y, fields.Rotation.Z},
        Mathematics::Vector3{fields.Scale.X, fields.Scale.Y, fields.Scale.Z});
    world.AddComponentImmediate(entity, xf);
}

bool TransformSchema::ApplyProperty(ECS::World& world,
                                    ECS::EntityHandle entity,
                                    [[maybe_unused]] const SceneLoadContext& ctx,
                                    std::string_view property,
                                    std::string_view value,
                                    std::string* outError) const
{
    TransformFields fields = ReadTransformFields(world, entity);
    if (!ParseTransformField(fields, property, value, outError))
        return false;
    WriteTransformFields(world, entity, fields);
    return true;
}

bool TransformSchema::ApplyProperties(ECS::World& world,
                                      ECS::EntityHandle entity,
                                      [[maybe_unused]] const SceneLoadContext& ctx,
                                      std::span<const std::pair<std::string_view, std::string_view>> props,
                                      std::string* outError,
                                      std::size_t* outFailedIndex) const
{
    if (props.empty())
        return true;
    TransformFields fields = ReadTransformFields(world, entity);
    for (std::size_t i = 0; i < props.size(); ++i)
    {
        if (!ParseTransformField(fields, props[i].first, props[i].second, outError))
        {
            if (outFailedIndex)
                *outFailedIndex = i;
            return false;
        }
    }
    WriteTransformFields(world, entity, fields);
    return true;
}

bool TransformSchema::AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const
{
    (void)outError;
    if (world.GetComponent<Components::Transform>(entity))
        return true;
    const Components::Transform xf = Components::Transform::FromTRS(
        Mathematics::Vector3{0, 0, 0},
        Mathematics::Quaternion{},
        Mathematics::Vector3{1, 1, 1});
    world.AddComponentImmediate(entity, xf);
    return true;
}

bool TransformSchema::Remove(ECS::World& world, ECS::EntityHandle entity) const
{
    world.RemoveComponentImmediate<Components::Transform>(entity);
    return true;
}

void MeshRendererSchema::Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const
{
    using namespace Engine::Renderer;
    const auto* mr = world.GetComponent<Components::MeshRenderer>(entity);
    if (!mr)
        return;

    // Mesh: serialize as primitive name or model asset GUID (not runtime meshId).
    // Check meshGpuHandleId first — if it maps to a known primitive, serialize the name.
    // Otherwise check modelAssetGuid for asset-loaded meshes.
    // Fall back to legacy meshId for compatibility.
    bool meshSerialized = false;

    // Try to identify the mesh by checking if it's a registered primitive via its GPU handle.
    // The GPU handle encodes the GUID that was used during RegisterAll().
    // We also check the modelAssetGuid field which tracks the source model asset.
    {
        // Try modelAssetGuid first (tracks the model asset)
        const GUID meshGuid = mr->modelAssetGuid.ToGuid();
        if (!meshGuid.IsNull())
        {
            auto primName = PrimitiveGenerator::NameFromGuid(meshGuid);
            if (!primName.empty())
            {
                outLines.push_back(std::string("MeshRenderer.meshPrimitive = ") + std::string(primName));
                meshSerialized = true;
            }
            else
            {
                outLines.push_back(std::string("MeshRenderer.meshAsset = ") +
                                   FormatAssetReferenceForSave(ctx, meshGuid, ""));
                // Submesh index within the model (0 for single-mesh models).
                if (mr->meshId != 0)
                    outLines.push_back(std::string("MeshRenderer.meshId = ") + std::to_string(mr->meshId));
                meshSerialized = true;
            }
        }
    }

    if (!meshSerialized)
    {
        // Legacy fallback: serialize numeric meshId
        outLines.push_back(std::string("MeshRenderer.meshId = ") + std::to_string(mr->meshId));
    }

    // Submesh-by-name selector, written as the name the id was interned from.
    // The name is the durable identity: it is re-hashed on every load, so a
    // change to HashMeshName re-keys the binding instead of orphaning it, and
    // the line stays readable in a diff. The hash is written back only while
    // the name is unknown; a bind replaces it with the string on the next save.
    if (mr->MeshNameId != 0)
    {
        const std::string meshName = Engine::Renderer::FindMeshName(mr->MeshNameId);
        if (!meshName.empty())
        {
            outLines.push_back(std::string("MeshRenderer.meshName = ") + FormatQuoted(meshName));
        }
        else
        {
            // The id arrived as a bare `meshNameHash` and has not bound yet (the
            // scene is still resolving, or its model is missing), so no name is
            // known. Keep the number the loader can still read rather than
            // dropping the selector.
            Logger::Log::Warning(
                "MeshRenderer: submesh selector {} has no known name yet; saved as meshNameHash "
                "(it becomes meshName once the entity binds)",
                mr->MeshNameId);
            outLines.push_back(std::string("MeshRenderer.meshNameHash = ") + std::to_string(mr->MeshNameId));
        }
    }

    // Material: serialize the AssetRef path+guid pair via FormatAssetReferenceForSave,
    // which queries the active save context's resolver to heal stale data.
    {
        const GUID g = mr->materialAssetGuid.ToGuid();
        if (!g.IsNull())
        {
            outLines.push_back(std::string("MeshRenderer.material = ") +
                               FormatAssetReferenceForSave(ctx, g, ""));
        }
    }
    outLines.push_back(std::string("MeshRenderer.renderLayerMask = ") + std::to_string(mr->renderLayerMask));
    outLines.push_back(std::string("MeshRenderer.castShadows = ") + (mr->castShadows ? "true" : "false"));
    outLines.push_back(std::string("MeshRenderer.receiveShadows = ") + (mr->receiveShadows ? "true" : "false"));
    outLines.push_back(std::string("MeshRenderer.motionVectors = ") + (mr->motionVectors ? "true" : "false"));
}

void MeshRendererSchema::EnumerateAssetReferences(const ECS::World& world,
                                                  ECS::EntityHandle entity,
                                                  const AssetRefVisitor& visitor) const
{
    const auto* mr = world.GetComponent<Components::MeshRenderer>(entity);
    if (!mr || !visitor)
        return;

    if (const GUID g = mr->modelAssetGuid.ToGuid(); !g.IsNull())
        // Property name lowercased to match SceneIO's case-folded property
        // dispatch — schemas only see "meshasset" in ApplyProperty.
        visitor(g, AssetType::Model, std::string_view{}, "meshasset");
    if (const GUID g = mr->materialAssetGuid.ToGuid(); !g.IsNull())
        visitor(g, AssetType::Material, std::string_view{}, "material");
}

static bool ParseU32(std::string_view value, uint32_t& out, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;
    if (v.Kind == SceneValueKind::Int)
    {
        if (v.IntValue < 0 || v.IntValue > 0xFFFFFFFFll)
        {
            if (outError)
                *outError = "Value out of range for uint32";
            return false;
        }
        out = static_cast<uint32_t>(v.IntValue);
        return true;
    }
    if (v.Kind == SceneValueKind::Float)
    {
        if (v.FloatValue < 0.0 || v.FloatValue > 4294967295.0)
        {
            if (outError)
                *outError = "Value out of range for uint32";
            return false;
        }
        out = static_cast<uint32_t>(v.FloatValue);
        return true;
    }
    if (outError)
        *outError = "Expected an integer value";
    return false;
}

static bool ParseU64(std::string_view value, uint64_t& out, std::string* outError)
{
    // Parse in the destination's own domain: MeshNameId is an FNV hash that routinely sets the
    // high bit, which the int64-based SceneValue path cannot represent.
    switch (ParseIntegerToken(value, out))
    {
    case IntegerTokenResult::Ok:
        return true;
    case IntegerTokenResult::OutOfRange:
        if (outError)
            *outError = "Unsigned integer value out of range";
        return false;
    case IntegerTokenResult::Malformed:
        break;
    }
    if (outError)
        *outError = "Expected an unsigned integer value";
    return false;
}

static bool ParseBool(std::string_view value, bool& out, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;
    if (v.Kind != SceneValueKind::Bool)
    {
        if (outError)
            *outError = "Expected true/false";
        return false;
    }
    out = v.BoolValue;
    return true;
}

static bool ParseI32(std::string_view value, int32_t& out, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;
    if (v.Kind == SceneValueKind::Int)
    {
        constexpr int64_t kMin = (int64_t)std::numeric_limits<int32_t>::min();
        constexpr int64_t kMax = (int64_t)std::numeric_limits<int32_t>::max();
        if (v.IntValue < kMin || v.IntValue > kMax)
        {
            if (outError)
                *outError = "Value out of range for int32";
            return false;
        }
        out = static_cast<int32_t>(v.IntValue);
        return true;
    }
    if (v.Kind == SceneValueKind::Float)
    {
        constexpr double kMin = (double)std::numeric_limits<int32_t>::min();
        constexpr double kMax = (double)std::numeric_limits<int32_t>::max();
        if (v.FloatValue < kMin || v.FloatValue > kMax)
        {
            if (outError)
                *outError = "Value out of range for int32";
            return false;
        }
        out = static_cast<int32_t>(v.FloatValue);
        return true;
    }
    if (outError)
        *outError = "Expected an integer value";
    return false;
}

static bool ParseF32(std::string_view value, float32& out, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;
    if (v.Kind == SceneValueKind::Float)
    {
        out = (float32)v.FloatValue;
        return true;
    }
    if (v.Kind == SceneValueKind::Int)
    {
        out = (float32)v.IntValue;
        return true;
    }
    if (outError)
        *outError = "Expected a numeric value";
    return false;
}

static bool ParseSceneString(std::string_view value, std::string& out, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;
    if (v.Kind == SceneValueKind::String || v.Kind == SceneValueKind::Identifier)
    {
        out = v.StringValue;
        return true;
    }
    if (outError)
        *outError = "Expected a string value";
    return false;
}

static bool ParseU8(std::string_view value, uint8& out, std::string* outError)
{
    uint32_t tmp = 0;
    if (!ParseU32(value, tmp, outError))
        return false;
    if (tmp > 0xFFu)
    {
        if (outError)
            *outError = "Value out of range for uint8";
        return false;
    }
    out = (uint8)tmp;
    return true;
}

bool MeshRendererSchema::ApplyProperty(ECS::World& world,
                                       ECS::EntityHandle entity,
                                       [[maybe_unused]] const SceneLoadContext& ctx,
                                       std::string_view property,
                                       std::string_view value,
                                       std::string* outError) const
{
    Components::MeshRenderer mr{};
    if (auto* existing = world.GetComponent<Components::MeshRenderer>(entity))
        mr = *existing;

    // NOTE: SceneIO lowercases property names. Keep comparisons lowercase for robustness.
    if (property == "meshid")
    {
        uint32_t v = 0;
        if (!ParseU32(value, v, outError))
            return false;
        mr.meshId = v;
    }
    else if (property == "meshname")
    {
        // The selector form both importers and saves write: a quoted source-node
        // name. Interning derives the id and keeps the string, so the next save
        // writes the name back and model resolution matches submeshes by the id
        // (exact first, then an _LOD<N>-folded fallback in ResolveSubmeshIndex).
        std::string name;
        if (!ParseSceneString(value, name, outError))
            return false;
        mr.MeshNameId = Engine::Renderer::InternMeshName(name);
    }
    else if (property == "meshnamehash")
    {
        // The hash form: what scenes saved before the selector became a name
        // carry, and what Serialize writes back for a selector whose name is not
        // yet known. The hash binds only while HashMeshName still produces it,
        // and no name can be recovered from it — ResolveSubmeshIndex interns the
        // submesh it matches, which is what lets the next save write `meshName`.
        uint64_t v = 0;
        if (!ParseU64(value, v, outError))
            return false;
        mr.MeshNameId = v;
    }
    else if (property == "meshprimitive")
    {
        // Resolve primitive name ("Cube", "Sphere", etc.) to a GPU handle.
        // The value is already lowercased by the parser.
        GUID primGuid = Engine::Renderer::PrimitiveGenerator::GuidFromName(value);
        if (primGuid.IsNull())
        {
            if (outError)
                *outError = "Unknown mesh primitive: " + std::string(value);
            return false;
        }

        // Use the shared helper to set mesh + default material consistently.
        // Preserve properties already set from previous lines in the scene file.
        auto* rsPtr = EngineCore::GetInstance().GetRenderServices();
        GUID matGuid = Engine::Renderer::PrimitiveGenerator::DefaultMaterialGuid();
        auto primMr = Engine::Renderer::PrimitiveGenerator::MakePrimitiveMeshRenderer(rsPtr, primGuid, matGuid);
        // Copy mesh/material fields from the helper, keep other fields from existing mr.
        mr.meshGpuHandleId = primMr.meshGpuHandleId;
        mr.meshId = primMr.meshId;
        mr.modelAssetGuid = primMr.modelAssetGuid;
        // Only set material if not already set by a previous "material" property.
        if (mr.materialAssetGuid.IsNull())
        {
            mr.materialAssetGuid = primMr.materialAssetGuid;
        }

        // Populate LocalBounds for the primitive so spatial queries
        // (picking, culling, AI LOS, audio occlusion) see real bounds
        // instead of relying on per-system fallback rules. Only set if
        // the entity doesn't already have a LocalBounds (preserve any
        // earlier scene-file overrides).
        if (!world.GetComponent<Components::LocalBounds>(entity))
        {
            world.AddComponentImmediate(entity,
                                        Engine::Renderer::PrimitiveGenerator::MakePrimitiveLocalBounds(rsPtr, primGuid));
        }
    }
    else if (property == "meshasset")
    {
        // Accepts:
        //   `MeshRenderer.meshAsset = [path="..." guid="..."]`  (canonical)
        //   `MeshRenderer.meshAsset = "<guid>"`                  (legacy bare GUID)
        SceneValue sv{};
        if (!ParseValue(value, sv, outError))
            return false;
        AssetReference ref{};
        std::string err;
        if (!TryResolveAssetReference(ctx, sv, AssetType::Model, ref, &err))
        {
            if (outError)
                *outError = err.empty() ? "Invalid mesh asset reference" : err;
            return false;
        }
        if (ref.guid.IsNull())
        {
            if (outError)
                *outError = "MeshRenderer.meshAsset resolved to a null GUID";
            return false;
        }
        mr.modelAssetGuid.Set(ref.guid);
        // The GPU handle is resolved after the scene load: RuntimeHost::OpenScene
        // hands the world to SceneResolveService, which binds it over frames.
    }
    else if (property == "material")
    {
        auto clearMaterialAsset = [&]()
        {
            mr.materialAssetGuid.Clear();
        };
        auto setMaterialAsset = [&](const AssetReference& ref)
        {
            if (!ref.IsValid())
            {
                clearMaterialAsset();
                return;
            }
            mr.materialAssetGuid.Set(ref.guid);
        };

        // Supports:
        // - `MeshRenderer.material = [path="..." guid="..."]` (canonical)
        // - `MeshRenderer.material = #myMaterial`              (authored via [resource]/[embed])
        // - `MeshRenderer.material = "<guid>"`                  (legacy bare GUID)
        // - `MeshRenderer.material = 0`                         (clears)
        SceneValue sv{};
        if (!ParseValue(value, sv, outError))
            return false;

        if (sv.Kind == SceneValueKind::Int && sv.IntValue == 0)
        {
            clearMaterialAsset();
        }
        else
        {
            AssetReference ref{};
            std::string err;
            if (!TryResolveAssetReference(ctx, sv, AssetType::Material, ref, &err))
            {
                if (outError)
                    *outError = err.empty() ? "MeshRenderer.material: failed to resolve" : err;
                return false;
            }
            if (ref.type != AssetType::Unknown && ref.type != AssetType::Material)
            {
                if (outError)
                    *outError = "MeshRenderer.material must reference a Material asset";
                return false;
            }
            setMaterialAsset(ref);
        }
    }
    else if (property == "materialid")
    {
        // Legacy field — silently accepted on load for backwards compatibility
        // with old scene files; the value is dropped (no longer stored on
        // MeshRenderer). New scene saves omit this property.
        uint32_t v = 0;
        if (!ParseU32(value, v, outError))
            return false;
    }
    else if (property == "renderlayermask")
    {
        uint32_t v = 0;
        if (!ParseU32(value, v, outError))
            return false;
        mr.renderLayerMask = v;
    }
    else if (property == "castshadows")
    {
        bool b = true;
        if (!ParseBool(value, b, outError))
            return false;
        mr.castShadows = b;
    }
    else if (property == "receiveshadows")
    {
        bool b = true;
        if (!ParseBool(value, b, outError))
            return false;
        mr.receiveShadows = b;
    }
    else if (property == "motionvectors")
    {
        bool b = true;
        if (!ParseBool(value, b, outError))
            return false;
        mr.motionVectors = b;
    }
    else if (property == "lodgroupid")
    {
        // Legacy field (pre-2026-07 scenes): accepted and ignored. LOD grouping
        // never had a consumer; per-entity LOD control is LODGroup.Bias.
    }
    else
    {
        if (outError)
            *outError = "Unknown MeshRenderer property";
        return false;
    }

    world.AddComponentImmediate(entity, mr);
    return true;
}

bool MeshRendererSchema::AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const
{
    (void)outError;
    if (world.GetComponent<Components::MeshRenderer>(entity))
        return true;
    Components::MeshRenderer mr{};
    world.AddComponentImmediate(entity, mr);
    return true;
}

bool MeshRendererSchema::Remove(ECS::World& world, ECS::EntityHandle entity) const
{
    world.RemoveComponentImmediate<Components::MeshRenderer>(entity);
    return true;
}

void MorphTargetWeightsSchema::Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const
{
    const auto* morph = world.GetComponent<Components::MorphTargetWeights>(entity);
    if (!morph)
        return;

    const uint32 count = std::min(morph->weightCount, Components::MorphTargetWeights::kMaxWeights);
    outLines.push_back(std::string("MorphTargetWeights.weightCount = ") + std::to_string(count));
    for (uint32 i = 0; i < count; ++i)
    {
        outLines.push_back(std::string("MorphTargetWeights.weight") + std::to_string(i) +
                           " = " + FormatFloat(morph->weights[i]));
    }
}

bool MorphTargetWeightsSchema::ApplyProperty(ECS::World& world,
                                             ECS::EntityHandle entity,
                                             [[maybe_unused]] const SceneLoadContext& ctx,
                                             std::string_view property,
                                             std::string_view value,
                                             std::string* outError) const
{
    Components::MorphTargetWeights morph{};
    if (auto* existing = world.GetComponent<Components::MorphTargetWeights>(entity))
        morph = *existing;

    if (property == "weightcount")
    {
        uint32 v = 0;
        if (!ParseU32(value, v, outError))
            return false;
        morph.weightCount = std::min(v, Components::MorphTargetWeights::kMaxWeights);
    }
    else if (property.rfind("weight", 0) == 0 && property.size() > 6)
    {
        uint32 index = 0;
        if (!ParseU32(property.substr(6), index, outError))
            return false;
        if (index >= Components::MorphTargetWeights::kMaxWeights)
        {
            if (outError)
                *outError = "MorphTargetWeights weight index out of range";
            return false;
        }
        float32 weight = 0.0f;
        if (!ParseF32(value, weight, outError))
            return false;
        morph.weights[index] = weight;
        morph.weightCount = std::max(morph.weightCount, index + 1u);
        ++morph.version;
    }
    else
    {
        if (outError)
            *outError = "Unknown MorphTargetWeights property";
        return false;
    }

    world.AddComponentImmediate(entity, morph);
    return true;
}

bool MorphTargetWeightsSchema::AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const
{
    (void)outError;
    if (world.GetComponent<Components::MorphTargetWeights>(entity))
        return true;
    world.AddComponentImmediate(entity, Components::MorphTargetWeights{});
    return true;
}

bool MorphTargetWeightsSchema::Remove(ECS::World& world, ECS::EntityHandle entity) const
{
    world.RemoveComponentImmediate<Components::MorphTargetWeights>(entity);
    return true;
}

void PhysicsWorldSettingsSchema::Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const
{
    const auto* c = world.GetComponent<Components::PhysicsWorldSettingsComponent>(entity);
    if (!c)
        return;

    const auto& s = c->settings;
    outLines.push_back(std::string("PhysicsWorldSettingsComponent.gravity = ") + FormatFloat3(s.gravity.x, s.gravity.y, s.gravity.z));
    outLines.push_back(std::string("PhysicsWorldSettingsComponent.fixedTimeStep = ") + FormatFloat(s.fixedTimeStep));
    outLines.push_back(std::string("PhysicsWorldSettingsComponent.maxSubSteps = ") + std::to_string(s.maxSubSteps));
    outLines.push_back(std::string("PhysicsWorldSettingsComponent.collisionSteps = ") + std::to_string(s.collisionSteps));
    outLines.push_back(std::string("PhysicsWorldSettingsComponent.maxBodies = ") + std::to_string(s.maxBodies));
    outLines.push_back(std::string("PhysicsWorldSettingsComponent.maxBodyPairs = ") + std::to_string(s.maxBodyPairs));
    outLines.push_back(std::string("PhysicsWorldSettingsComponent.maxContactConstraints = ") + std::to_string(s.maxContactConstraints));
    outLines.push_back(std::string("PhysicsWorldSettingsComponent.tempAllocatorBytes = ") + std::to_string(s.tempAllocatorBytes));
    outLines.push_back(std::string("PhysicsWorldSettingsComponent.numThreads = ") + std::to_string(s.numThreads));
}

bool PhysicsWorldSettingsSchema::ApplyProperty(ECS::World& world,
                                               ECS::EntityHandle entity,
                                               [[maybe_unused]] const SceneLoadContext& ctx,
                                               std::string_view property,
                                               std::string_view value,
                                               std::string* outError) const
{
    Components::PhysicsWorldSettingsComponent c{};
    if (auto* existing = world.GetComponent<Components::PhysicsWorldSettingsComponent>(entity))
        c = *existing;

    if (property == "gravity")
    {
        Float3 g{0, -9.81f, 0};
        if (!ParseFloat3(value, g))
        {
            if (outError)
                *outError = "PhysicsWorldSettingsComponent.gravity must be (x, y, z)";
            return false;
        }
        c.settings.gravity = {g.X, g.Y, g.Z};
    }
    else if (property == "fixedtimestep")
    {
        float32 f = c.settings.fixedTimeStep;
        if (!ParseF32(value, f, outError))
            return false;
        c.settings.fixedTimeStep = f;
    }
    else if (property == "maxsubsteps")
    {
        int32_t i = c.settings.maxSubSteps;
        if (!ParseI32(value, i, outError))
            return false;
        c.settings.maxSubSteps = i;
    }
    else if (property == "collisionsteps")
    {
        int32_t i = c.settings.collisionSteps;
        if (!ParseI32(value, i, outError))
            return false;
        c.settings.collisionSteps = i;
    }
    else if (property == "maxbodies")
    {
        uint32_t v = c.settings.maxBodies;
        if (!ParseU32(value, v, outError))
            return false;
        c.settings.maxBodies = v;
    }
    else if (property == "maxbodypairs")
    {
        uint32_t v = c.settings.maxBodyPairs;
        if (!ParseU32(value, v, outError))
            return false;
        c.settings.maxBodyPairs = v;
    }
    else if (property == "maxcontactconstraints")
    {
        uint32_t v = c.settings.maxContactConstraints;
        if (!ParseU32(value, v, outError))
            return false;
        c.settings.maxContactConstraints = v;
    }
    else if (property == "tempallocatorbytes")
    {
        uint32_t v = c.settings.tempAllocatorBytes;
        if (!ParseU32(value, v, outError))
            return false;
        c.settings.tempAllocatorBytes = v;
    }
    else if (property == "numthreads")
    {
        int32_t i = c.settings.numThreads;
        if (!ParseI32(value, i, outError))
            return false;
        c.settings.numThreads = i;
    }
    else
    {
        if (outError)
            *outError = "Unknown PhysicsWorldSettingsComponent property";
        return false;
    }

    world.AddComponentImmediate(entity, c);
    return true;
}

bool PhysicsWorldSettingsSchema::AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const
{
    (void)outError;
    if (world.GetComponent<Components::PhysicsWorldSettingsComponent>(entity))
        return true;
    Components::PhysicsWorldSettingsComponent c{};
    world.AddComponentImmediate(entity, c);
    return true;
}

bool PhysicsWorldSettingsSchema::Remove(ECS::World& world, ECS::EntityHandle entity) const
{
    world.RemoveComponentImmediate<Components::PhysicsWorldSettingsComponent>(entity);
    return true;
}

void PhysicsBodySchema::Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const
{
    const auto* b = world.GetComponent<Components::PhysicsBody>(entity);
    if (!b)
        return;

    // Serialize authoring fields only (skip runtime handles/state).
    outLines.push_back(std::string("PhysicsBody.motionType = ") + std::to_string((uint32)b->motionType));
    outLines.push_back(std::string("PhysicsBody.mass = ") + FormatFloat(b->mass));
    outLines.push_back(std::string("PhysicsBody.linearDamping = ") + FormatFloat(b->linearDamping));
    outLines.push_back(std::string("PhysicsBody.angularDamping = ") + FormatFloat(b->angularDamping));
    outLines.push_back(std::string("PhysicsBody.gravityScale = ") + FormatFloat(b->gravityScale));
    outLines.push_back(std::string("PhysicsBody.centerOfMassOffset = ") + FormatFloat3(b->centerOfMassOffsetX, b->centerOfMassOffsetY, b->centerOfMassOffsetZ));
    outLines.push_back(std::string("PhysicsBody.materialFriction = ") + FormatFloat(b->material.friction));
    outLines.push_back(std::string("PhysicsBody.materialRestitution = ") + FormatFloat(b->material.restitution));
    outLines.push_back(std::string("PhysicsBody.linearVelocity = ") + FormatFloat3(b->linearVelocityX, b->linearVelocityY, b->linearVelocityZ));
    outLines.push_back(std::string("PhysicsBody.angularVelocity = ") + FormatFloat3(b->angularVelocityX, b->angularVelocityY, b->angularVelocityZ));
    outLines.push_back(std::string("PhysicsBody.allowSleep = ") + (b->allowSleep ? "true" : "false"));
    outLines.push_back(std::string("PhysicsBody.startAwake = ") + (b->startAwake ? "true" : "false"));
    outLines.push_back(std::string("PhysicsBody.continuousCollision = ") + (b->continuousCollision ? "true" : "false"));
}

bool PhysicsBodySchema::ApplyProperty(ECS::World& world,
                                      ECS::EntityHandle entity,
                                      [[maybe_unused]] const SceneLoadContext& ctx,
                                      std::string_view property,
                                      std::string_view value,
                                      std::string* outError) const
{
    Components::PhysicsBody b{};
    if (auto* existing = world.GetComponent<Components::PhysicsBody>(entity))
        b = *existing;

    // A scene stores authoring data only; the handles are the physics world's.
    Components::ClearPhysicsBodyRuntimeState(b);

    if (property == "motiontype")
    {
        uint32_t v = 0;
        if (!ParseU32(value, v, outError))
            return false;
        b.motionType = (Physics::MotionType)(uint8)v;
    }
    else if (property == "mass")
    {
        if (!ParseF32(value, b.mass, outError))
            return false;
    }
    else if (property == "lineardamping")
    {
        if (!ParseF32(value, b.linearDamping, outError))
            return false;
    }
    else if (property == "angulardamping")
    {
        if (!ParseF32(value, b.angularDamping, outError))
            return false;
    }
    else if (property == "gravityscale")
    {
        if (!ParseF32(value, b.gravityScale, outError))
            return false;
    }
    else if (property == "centerofmassoffset")
    {
        Float3 v{b.centerOfMassOffsetX, b.centerOfMassOffsetY, b.centerOfMassOffsetZ};
        if (!ParseFloat3(value, v))
        {
            if (outError)
                *outError = "PhysicsBody.centerOfMassOffset must be (x, y, z)";
            return false;
        }
        b.centerOfMassOffsetX = v.X;
        b.centerOfMassOffsetY = v.Y;
        b.centerOfMassOffsetZ = v.Z;
    }
    else if (property == "centerofmassoffsetx")
    {
        if (!ParseF32(value, b.centerOfMassOffsetX, outError))
            return false;
    }
    else if (property == "centerofmassoffsety")
    {
        if (!ParseF32(value, b.centerOfMassOffsetY, outError))
            return false;
    }
    else if (property == "centerofmassoffsetz")
    {
        if (!ParseF32(value, b.centerOfMassOffsetZ, outError))
            return false;
    }
    else if (property == "materialfriction")
    {
        if (!ParseF32(value, b.material.friction, outError))
            return false;
    }
    else if (property == "materialrestitution")
    {
        if (!ParseF32(value, b.material.restitution, outError))
            return false;
    }
    else if (property == "linearvelocity")
    {
        Float3 v{b.linearVelocityX, b.linearVelocityY, b.linearVelocityZ};
        if (!ParseFloat3(value, v))
        {
            if (outError)
                *outError = "PhysicsBody.linearVelocity must be (x, y, z)";
            return false;
        }
        b.linearVelocityX = v.X;
        b.linearVelocityY = v.Y;
        b.linearVelocityZ = v.Z;
    }
    else if (property == "angularvelocity")
    {
        Float3 v{b.angularVelocityX, b.angularVelocityY, b.angularVelocityZ};
        if (!ParseFloat3(value, v))
        {
            if (outError)
                *outError = "PhysicsBody.angularVelocity must be (x, y, z)";
            return false;
        }
        b.angularVelocityX = v.X;
        b.angularVelocityY = v.Y;
        b.angularVelocityZ = v.Z;
    }
    else if (property == "allowsleep")
    {
        if (!ParseBool(value, b.allowSleep, outError))
            return false;
    }
    else if (property == "startawake")
    {
        if (!ParseBool(value, b.startAwake, outError))
            return false;
    }
    else if (property == "continuouscollision")
    {
        if (!ParseBool(value, b.continuousCollision, outError))
            return false;
    }
    else
    {
        if (outError)
            *outError = "Unknown PhysicsBody property";
        return false;
    }

    world.AddComponentImmediate(entity, b);
    return true;
}

bool PhysicsBodySchema::AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const
{
    (void)outError;
    if (world.GetComponent<Components::PhysicsBody>(entity))
        return true;
    Components::PhysicsBody b{};
    world.AddComponentImmediate(entity, b);
    return true;
}

bool PhysicsBodySchema::Remove(ECS::World& world, ECS::EntityHandle entity) const
{
    world.RemoveComponentImmediate<Components::PhysicsBody>(entity);
    return true;
}

void CharacterControllerSchema::Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const
{
    const auto* c = world.GetComponent<Components::CharacterController>(entity);
    if (!c)
        return;

    outLines.push_back(std::string("CharacterController.motor = ") + std::to_string((uint32)c->motor));
    outLines.push_back(std::string("CharacterController.radius = ") + FormatFloat(c->radius));
    outLines.push_back(std::string("CharacterController.height = ") + FormatFloat(c->height));
    outLines.push_back(std::string("CharacterController.maxSlopeAngleDegrees = ") + FormatFloat(c->maxSlopeAngleDegrees));
    outLines.push_back(std::string("CharacterController.maxStepHeight = ") + FormatFloat(c->maxStepHeight));
    outLines.push_back(std::string("CharacterController.skinWidth = ") + FormatFloat(c->skinWidth));
    outLines.push_back(std::string("CharacterController.mass = ") + FormatFloat(c->mass));
    outLines.push_back(std::string("CharacterController.gravityScale = ") + FormatFloat(c->gravityScale));
}

bool CharacterControllerSchema::ApplyProperty(ECS::World& world,
                                              ECS::EntityHandle entity,
                                              [[maybe_unused]] const SceneLoadContext& ctx,
                                              std::string_view property,
                                              std::string_view value,
                                              std::string* outError) const
{
    Components::CharacterController c{};
    Physics::CharacterHandle previousCharacter{};
    if (auto* existing = world.GetComponent<Components::CharacterController>(entity))
    {
        c = *existing;
        previousCharacter = existing->character;
    }

    if (property == "motor")
    {
        uint32_t v = 0;
        if (!ParseU32(value, v, outError))
            return false;
        if (v > 1u)
        {
            if (outError)
                *outError = "CharacterController.motor must be 0 (Kinematic) or 1 (Dynamic)";
            return false;
        }
        c.motor = (v == 1u) ? Physics::CharacterMotor::Dynamic : Physics::CharacterMotor::Kinematic;
    }
    else if (property == "radius")
    {
        if (!ParseF32(value, c.radius, outError))
            return false;
    }
    else if (property == "height")
    {
        if (!ParseF32(value, c.height, outError))
            return false;
    }
    else if (property == "maxslopeangledegrees")
    {
        if (!ParseF32(value, c.maxSlopeAngleDegrees, outError))
            return false;
    }
    else if (property == "maxstepheight")
    {
        if (!ParseF32(value, c.maxStepHeight, outError))
            return false;
    }
    else if (property == "skinwidth")
    {
        if (!ParseF32(value, c.skinWidth, outError))
            return false;
    }
    else if (property == "mass")
    {
        if (!ParseF32(value, c.mass, outError))
            return false;
    }
    else if (property == "gravityscale")
    {
        if (!ParseF32(value, c.gravityScale, outError))
            return false;
    }
    else
    {
        if (outError)
            *outError = "Unknown CharacterController property";
        return false;
    }

    if (previousCharacter.IsValid())
    {
        if (auto* pw = PhysicsECS::PhysicsWorldService::TryGet())
        {
            if (pw->IsCharacterValid(previousCharacter))
                pw->DestroyCharacter(previousCharacter);
        }
    }
    Components::ClearCharacterControllerRuntimeState(c);

    world.AddComponentImmediate(entity, c);
    return true;
}

bool CharacterControllerSchema::AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const
{
    (void)outError;
    if (world.GetComponent<Components::CharacterController>(entity))
        return true;
    Components::CharacterController c{};
    world.AddComponentImmediate(entity, c);
    return true;
}

bool CharacterControllerSchema::Remove(ECS::World& world, ECS::EntityHandle entity) const
{
    world.RemoveComponentImmediate<Components::CharacterController>(entity);
    return true;
}

void PhysicsColliderSchema::Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const
{
    const auto* c = world.GetComponent<Components::PhysicsCollider>(entity);
    if (!c)
        return;

    outLines.push_back(std::string("PhysicsCollider.layer = ") + std::to_string((uint32)c->layer));
    outLines.push_back(std::string("PhysicsCollider.groupId = ") + std::to_string((uint32)c->collisionGroup.groupId));
    outLines.push_back(std::string("PhysicsCollider.subGroupId = ") + std::to_string((uint32)c->collisionGroup.subGroupId));
    outLines.push_back(std::string("PhysicsCollider.belongsToMask = ") + std::to_string(c->belongsToMask));
    outLines.push_back(std::string("PhysicsCollider.collidesWithMask = ") + std::to_string(c->collidesWithMask));
    outLines.push_back(std::string("PhysicsCollider.isTrigger = ") + (c->isTrigger ? "true" : "false"));
    outLines.push_back(std::string("PhysicsCollider.overrideMaterial = ") + (c->overrideMaterial ? "true" : "false"));
    outLines.push_back(std::string("PhysicsCollider.materialFriction = ") + FormatFloat(c->material.friction));
    outLines.push_back(std::string("PhysicsCollider.materialRestitution = ") + FormatFloat(c->material.restitution));
}

bool PhysicsColliderSchema::ApplyProperty(ECS::World& world,
                                          ECS::EntityHandle entity,
                                          [[maybe_unused]] const SceneLoadContext& ctx,
                                          std::string_view property,
                                          std::string_view value,
                                          std::string* outError) const
{
    Components::PhysicsCollider c{};
    if (auto* existing = world.GetComponent<Components::PhysicsCollider>(entity))
        c = *existing;

    if (property == "layer")
    {
        uint32_t v = 0;
        if (!ParseU32(value, v, outError))
            return false;
        c.layer = (Physics::CollisionLayer)v;
    }
    else if (property == "groupid")
    {
        uint32_t v = 0;
        if (!ParseU32(value, v, outError))
            return false;
        c.collisionGroup.groupId = (uint16)v;
    }
    else if (property == "subgroupid")
    {
        uint32_t v = 0;
        if (!ParseU32(value, v, outError))
            return false;
        c.collisionGroup.subGroupId = (uint16)v;
    }
    else if (property == "belongstomask")
    {
        uint32_t v = 0;
        if (!ParseU32(value, v, outError))
            return false;
        c.belongsToMask = v;
    }
    else if (property == "collideswithmask")
    {
        uint32_t v = 0;
        if (!ParseU32(value, v, outError))
            return false;
        c.collidesWithMask = v;
    }
    else if (property == "istrigger")
    {
        if (!ParseBool(value, c.isTrigger, outError))
            return false;
    }
    else if (property == "overridematerial")
    {
        if (!ParseBool(value, c.overrideMaterial, outError))
            return false;
    }
    else if (property == "materialfriction")
    {
        if (!ParseF32(value, c.material.friction, outError))
            return false;
    }
    else if (property == "materialrestitution")
    {
        if (!ParseF32(value, c.material.restitution, outError))
            return false;
    }
    else
    {
        if (outError)
            *outError = "Unknown PhysicsCollider property";
        return false;
    }

    world.AddComponentImmediate(entity, c);
    return true;
}

bool PhysicsColliderSchema::AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const
{
    (void)outError;
    if (world.GetComponent<Components::PhysicsCollider>(entity))
        return true;
    Components::PhysicsCollider c{};
    world.AddComponentImmediate(entity, c);
    return true;
}

bool PhysicsColliderSchema::Remove(ECS::World& world, ECS::EntityHandle entity) const
{
    world.RemoveComponentImmediate<Components::PhysicsCollider>(entity);
    return true;
}

void PhysicsColliderOwnerSchema::Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const
{
    const auto* c = world.GetComponent<Components::PhysicsColliderOwner>(entity);
    if (!c)
        return;
    // NOTE: EntityHandles are not stable across scene save/load. We intentionally do not serialize this yet.
    // Leaving it schema-backed avoids "unknown component" load failures when authoring systems emit it.
    (void)outLines;
}

bool PhysicsColliderOwnerSchema::ApplyProperty(ECS::World& world,
                                               ECS::EntityHandle entity,
                                               [[maybe_unused]] const SceneLoadContext& ctx,
                                               std::string_view property,
                                               std::string_view /*value*/,
                                               std::string* outError) const
{
    // EntityHandles are not stable across scene save/load yet. Accept properties to avoid failing loads,
    // but leave the owner handle invalid so the collider defaults to owning itself.
    (void)world;
    (void)entity;
    (void)property;
    (void)outError;
    return true;
}

bool PhysicsColliderOwnerSchema::AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const
{
    (void)outError;
    if (world.GetComponent<Components::PhysicsColliderOwner>(entity))
        return true;
    Components::PhysicsColliderOwner c{};
    world.AddComponentImmediate(entity, c);
    return true;
}

bool PhysicsColliderOwnerSchema::Remove(ECS::World& world, ECS::EntityHandle entity) const
{
    world.RemoveComponentImmediate<Components::PhysicsColliderOwner>(entity);
    return true;
}

void BoxColliderShapeSchema::Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const
{
    const auto* c = world.GetComponent<Components::BoxColliderShape>(entity);
    if (!c)
        return;
    outLines.push_back(std::string("BoxColliderShape.halfExtents = ") + FormatFloat3(c->halfExtentsX, c->halfExtentsY, c->halfExtentsZ));
}

bool BoxColliderShapeSchema::ApplyProperty(ECS::World& world,
                                           ECS::EntityHandle entity,
                                           [[maybe_unused]] const SceneLoadContext& ctx,
                                           std::string_view property,
                                           std::string_view value,
                                           std::string* outError) const
{
    Components::BoxColliderShape c{};
    if (auto* existing = world.GetComponent<Components::BoxColliderShape>(entity))
        c = *existing;

    if (property == "halfextents")
    {
        Float3 v{c.halfExtentsX, c.halfExtentsY, c.halfExtentsZ};
        if (!ParseFloat3(value, v))
        {
            if (outError)
                *outError = "BoxColliderShape.halfExtents must be (x, y, z)";
            return false;
        }
        c.halfExtentsX = v.X;
        c.halfExtentsY = v.Y;
        c.halfExtentsZ = v.Z;
    }
    else if (property == "halfextentsx")
    {
        if (!ParseF32(value, c.halfExtentsX, outError))
            return false;
    }
    else if (property == "halfextentsy")
    {
        if (!ParseF32(value, c.halfExtentsY, outError))
            return false;
    }
    else if (property == "halfextentsz")
    {
        if (!ParseF32(value, c.halfExtentsZ, outError))
            return false;
    }
    else
    {
        if (outError)
            *outError = "Unknown BoxColliderShape property";
        return false;
    }

    world.AddComponentImmediate(entity, c);
    return true;
}

bool BoxColliderShapeSchema::AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const
{
    (void)outError;
    if (world.GetComponent<Components::BoxColliderShape>(entity))
        return true;
    Components::BoxColliderShape c{};
    world.AddComponentImmediate(entity, c);
    return true;
}

bool BoxColliderShapeSchema::Remove(ECS::World& world, ECS::EntityHandle entity) const
{
    world.RemoveComponentImmediate<Components::BoxColliderShape>(entity);
    return true;
}

void SphereColliderShapeSchema::Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const
{
    const auto* c = world.GetComponent<Components::SphereColliderShape>(entity);
    if (!c)
        return;
    outLines.push_back(std::string("SphereColliderShape.radius = ") + FormatFloat(c->radius));
}

bool SphereColliderShapeSchema::ApplyProperty(ECS::World& world,
                                              ECS::EntityHandle entity,
                                              [[maybe_unused]] const SceneLoadContext& ctx,
                                              std::string_view property,
                                              std::string_view value,
                                              std::string* outError) const
{
    Components::SphereColliderShape c{};
    if (auto* existing = world.GetComponent<Components::SphereColliderShape>(entity))
        c = *existing;

    if (property == "radius")
    {
        if (!ParseF32(value, c.radius, outError))
            return false;
    }
    else
    {
        if (outError)
            *outError = "Unknown SphereColliderShape property";
        return false;
    }

    world.AddComponentImmediate(entity, c);
    return true;
}

bool SphereColliderShapeSchema::AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const
{
    (void)outError;
    if (world.GetComponent<Components::SphereColliderShape>(entity))
        return true;
    Components::SphereColliderShape c{};
    world.AddComponentImmediate(entity, c);
    return true;
}

bool SphereColliderShapeSchema::Remove(ECS::World& world, ECS::EntityHandle entity) const
{
    world.RemoveComponentImmediate<Components::SphereColliderShape>(entity);
    return true;
}

void CapsuleColliderShapeSchema::Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const
{
    const auto* c = world.GetComponent<Components::CapsuleColliderShape>(entity);
    if (!c)
        return;
    outLines.push_back(std::string("CapsuleColliderShape.radius = ") + FormatFloat(c->radius));
    outLines.push_back(std::string("CapsuleColliderShape.halfHeight = ") + FormatFloat(c->halfHeight));
    outLines.push_back(std::string("CapsuleColliderShape.axis = ") + std::to_string((uint32)c->axis));
}

bool CapsuleColliderShapeSchema::ApplyProperty(ECS::World& world,
                                               ECS::EntityHandle entity,
                                               [[maybe_unused]] const SceneLoadContext& ctx,
                                               std::string_view property,
                                               std::string_view value,
                                               std::string* outError) const
{
    Components::CapsuleColliderShape c{};
    if (auto* existing = world.GetComponent<Components::CapsuleColliderShape>(entity))
        c = *existing;

    if (property == "radius")
    {
        if (!ParseF32(value, c.radius, outError))
            return false;
    }
    else if (property == "halfheight")
    {
        if (!ParseF32(value, c.halfHeight, outError))
            return false;
    }
    else if (property == "axis")
    {
        uint8 a = c.axis;
        if (!ParseU8(value, a, outError))
            return false;
        c.axis = a;
    }
    else
    {
        if (outError)
            *outError = "Unknown CapsuleColliderShape property";
        return false;
    }

    world.AddComponentImmediate(entity, c);
    return true;
}

bool CapsuleColliderShapeSchema::AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const
{
    (void)outError;
    if (world.GetComponent<Components::CapsuleColliderShape>(entity))
        return true;
    Components::CapsuleColliderShape c{};
    world.AddComponentImmediate(entity, c);
    return true;
}

bool CapsuleColliderShapeSchema::Remove(ECS::World& world, ECS::EntityHandle entity) const
{
    world.RemoveComponentImmediate<Components::CapsuleColliderShape>(entity);
    return true;
}

void PlaneColliderShapeSchema::Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const
{
    const auto* c = world.GetComponent<Components::PlaneColliderShape>(entity);
    if (!c)
        return;
    outLines.push_back(std::string("PlaneColliderShape.normal = ") + FormatFloat3(c->normalX, c->normalY, c->normalZ));
    outLines.push_back(std::string("PlaneColliderShape.d = ") + FormatFloat(c->d));
    outLines.push_back(std::string("PlaneColliderShape.halfExtent = ") + FormatFloat(c->halfExtent));
}

bool PlaneColliderShapeSchema::ApplyProperty(ECS::World& world,
                                             ECS::EntityHandle entity,
                                             [[maybe_unused]] const SceneLoadContext& ctx,
                                             std::string_view property,
                                             std::string_view value,
                                             std::string* outError) const
{
    Components::PlaneColliderShape c{};
    if (auto* existing = world.GetComponent<Components::PlaneColliderShape>(entity))
        c = *existing;

    if (property == "normal")
    {
        Float3 v{c.normalX, c.normalY, c.normalZ};
        if (!ParseFloat3(value, v))
        {
            if (outError)
                *outError = "PlaneColliderShape.normal must be (x, y, z)";
            return false;
        }
        c.normalX = v.X;
        c.normalY = v.Y;
        c.normalZ = v.Z;
    }
    else if (property == "d")
    {
        if (!ParseF32(value, c.d, outError))
            return false;
    }
    else if (property == "halfextent")
    {
        if (!ParseF32(value, c.halfExtent, outError))
            return false;
    }
    else
    {
        if (outError)
            *outError = "Unknown PlaneColliderShape property";
        return false;
    }

    world.AddComponentImmediate(entity, c);
    return true;
}

bool PlaneColliderShapeSchema::AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const
{
    (void)outError;
    if (world.GetComponent<Components::PlaneColliderShape>(entity))
        return true;
    Components::PlaneColliderShape c{};
    world.AddComponentImmediate(entity, c);
    return true;
}

bool PlaneColliderShapeSchema::Remove(ECS::World& world, ECS::EntityHandle entity) const
{
    world.RemoveComponentImmediate<Components::PlaneColliderShape>(entity);
    return true;
}

// Marker: entity is disabled in editor/runtime (no `Disabled` in scene = enabled).
class DisabledSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "Disabled"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        if (!world.GetComponent<ECS::Disabled>(entity))
            return;
        outLines.push_back("Disabled.present = true");
    }

    bool IsPresent(const ECS::World& world, ECS::EntityHandle entity) const override
    {
        return world.GetComponent<ECS::Disabled>(entity) != nullptr;
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        if (property != "present")
        {
            if (outError)
                *outError = "Unknown Disabled property";
            return false;
        }
        bool present = false;
        if (!ParseBool(value, present, outError))
            return false;
        if (present)
            world.AddComponentImmediate(entity, ECS::Disabled{});
        else
            world.RemoveComponentImmediate<ECS::Disabled>(entity);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        world.AddComponentImmediate(entity, ECS::Disabled{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<ECS::Disabled>(entity);
        return true;
    }
};

// Sky day-curves serialize as a compact "time,value;time,value;..." list (keys are Linear).
static std::string FormatSkyCurve(const Math::Curve& curve)
{
    std::string s;
    for (uint8_t i = 0; i < curve.KeyCount; ++i)
    {
        if (i != 0)
            s += ';';
        s += FormatFloat(curve.Keys[i].Time);
        s += ',';
        s += FormatFloat(curve.Keys[i].Value);
    }
    return s;
}

static void ParseSkyCurve(std::string_view value, Math::Curve& out)
{
    out.KeyCount = 0;
    size_t pos = 0;
    while (pos < value.size() && out.KeyCount < Math::Curve::Capacity)
    {
        const size_t semi = value.find(';', pos);
        const std::string_view pair =
            (semi == std::string_view::npos) ? value.substr(pos) : value.substr(pos, semi - pos);
        const size_t comma = pair.find(',');
        if (comma != std::string_view::npos)
        {
            float t = 0.0f;
            float v = 0.0f;
            if (ParseF32(pair.substr(0, comma), t, nullptr) && ParseF32(pair.substr(comma + 1), v, nullptr))
            {
                Math::CurveKey key{};
                key.Time = t;
                key.Value = v;
                out.TryInsert(key);
            }
        }
        if (semi == std::string_view::npos)
            break;
        pos = semi + 1;
    }
}

static std::string_view ToSkyScalarCurveShapeModeName(Components::SkyScalarCurveShapeMode mode)
{
    return mode == Components::SkyScalarCurveShapeMode::CubicBezier ? "cubicBezier" : "keyCurve";
}

static bool TryParseSkyScalarCurveShapeMode(std::string_view value, Components::SkyScalarCurveShapeMode& out)
{
    std::string raw = std::string(value);
    std::string quoted;
    if (ParseQuotedString(value, quoted))
        raw = quoted;
    std::transform(raw.begin(), raw.end(), raw.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    if (raw == "keycurve" || raw == "keys" || raw == "curve")
    {
        out = Components::SkyScalarCurveShapeMode::KeyCurve;
        return true;
    }
    if (raw == "cubicbezier" || raw == "bezier" || raw == "cubic")
    {
        out = Components::SkyScalarCurveShapeMode::CubicBezier;
        return true;
    }
    return false;
}

static std::string FormatSkyScalarBezier(const Components::SkyScalarCubicBezier& bezier)
{
    return FormatFloat(bezier.Control1X) + "," +
           FormatFloat(bezier.Control1Y) + "," +
           FormatFloat(bezier.Control2X) + "," +
           FormatFloat(bezier.Control2Y) + "," +
           FormatFloat(bezier.AnchorStartY) + "," +
           FormatFloat(bezier.AnchorEndY);
}

static bool ParseSkyScalarBezier(std::string_view value, Components::SkyScalarCubicBezier& out)
{
    float32 fields[6] = {
        out.Control1X,
        out.Control1Y,
        out.Control2X,
        out.Control2Y,
        out.AnchorStartY,
        out.AnchorEndY,
    };

    size_t fieldStart = 0;
    int parsedCount = 0;
    for (int i = 0; i < 6; ++i)
    {
        const size_t fieldEnd = value.find(',', fieldStart);
        const std::string_view tok = value.substr(
            fieldStart,
            fieldEnd == std::string_view::npos ? value.size() - fieldStart : fieldEnd - fieldStart);
        float32 parsed = 0.0f;
        if (!ParseF32(tok, parsed, nullptr))
            return false;
        fields[i] = parsed;
        ++parsedCount;
        if (fieldEnd == std::string_view::npos)
            break;
        fieldStart = fieldEnd + 1;
    }

    if (parsedCount < 6)
        return false;

    out.Control1X = std::clamp(fields[0], 0.0f, 1.0f);
    out.Control1Y = fields[1];
    out.Control2X = std::clamp(fields[2], 0.0f, 1.0f);
    out.Control2Y = fields[3];
    out.AnchorStartY = fields[4];
    out.AnchorEndY = fields[5];
    return true;
}

// Retired SkyEnvironment properties, and where their values go now.
//
// The sun's colour is the atmosphere's: white above it, warmed toward the horizon by the extra air
// mass a lower sun's light crosses, and written back to the linked light so lit surfaces warm with
// the sky. The retired ramp multiplied that reddening in a second time — into the scattering source
// term AND into the light — which is why a 15-degree sun rendered a violet sky over salmon ground.
// The defect was WHERE the ramp landed, not that an artistic tint exists, so an authored ramp is
// carried onto SunTintKeys, which is applied once, at the source, ahead of the atmosphere.
//
// Only the DAY keys convert. Midnight and Dawn are dropped to white, and that is not laziness — the
// two halves of the old ramp meant different things:
//   * Midday/Sunset were effectively multipliers on a white daytime sun, so carrying them onto a
//     tint keeps their intent (over a physically warm sun now, so the look shifts toward physical —
//     the notice says so).
//   * Midnight/Dawn encoded absolute LEVELS. The old midnight default was 0.0048 — the night's
//     brightness lived in that number, and SkySystemConfig::moonColor carries it now. Reading such
//     a value as a multiplier inverts the author's intent: an authored midnight of (0.2, 0.2, 0.4),
//     42x BRIGHTER than the old default, would become a 0.2x multiplier — a night five times
//     DARKER than the one they asked for. The tint also no longer touches the moon at all, so there
//     is nothing for a night key to multiply. Dropped, with the value named in the notice so the
//     author can re-author it against the moon's colour.
//
// Midday/Sunset convert PER KEY against the retired default, not all-or-nothing: a scene that
// authored only an orange sunset keeps that key and gets white for the one it left alone. A key
// authored to exactly the retired default is indistinguishable from an untouched one in the file,
// and resolves as untouched.
struct RetiredSkyProperty
{
    // The key as ApplyProperty receives it: the loader lower-cases before dispatch.
    std::string_view Key;
    // The same key as a scene FILE spells it. Carried explicitly because the notice names it, and
    // a notice that says `suncolorsunset` sends the author grepping for a string their file does
    // not contain.
    std::string_view Authored;
    // The day key the value lands on; nullptr for the uniform setter (which writes the day keys)
    // and for the night keys, which are dropped rather than converted.
    float32 (Components::SkyVec3DayKeys::*Target)[3];
    // The value this key carried before it was retired. A match means the scene inherited the
    // default rather than authoring it, and the key converts to white.
    float32 RetiredDefault[3];
    // False for the night keys: their value was a level, not a multiplier, so it is reported and
    // dropped instead of being reinterpreted as a tint.
    bool ConvertsToTint;
};

constexpr std::string_view kRetiredSunColorReason =
    "the sun's colour comes from the atmosphere's transmittance now, and an authored tint belongs "
    "on SunTint, which is applied once at the source";

constexpr RetiredSkyProperty kRetiredSkyProperties[] = {
    {"suncolor", "SunColor", nullptr, {1.0f, 1.0f, 1.0f}, true},
    {"suncolormidnight", "SunColorMidnight", nullptr,
     {0.004776953f, 0.005181517f, 0.006048833f}, false},
    {"suncolordawn", "SunColorDawn", nullptr,
     {0.274677312f, 0.144128471f, 0.099898728f}, false},
    {"suncolormidday", "SunColorMidday", &Components::SkyVec3DayKeys::Midday,
     {1.0f, 1.0f, 1.0f}, true},
    {"suncolorsunset", "SunColorSunset", &Components::SkyVec3DayKeys::Sunset,
     {1.0f, 0.165132195f, 0.491020850f}, true},
    // The retired Light -> Sky Color toggle. It was the only path that handed the linked light a
    // colour WITHOUT the atmosphere's extinction, so the lit scene and the dome disagreed, and both
    // of its transitions trapped the author. SunTint replaces it: a light authored at 3200 K is a
    // SunTint key.
    {"usesunlightcolor", "UseSunLightColor", nullptr, {0.0f, 0.0f, 0.0f}, false},
};

const RetiredSkyProperty* FindRetiredSkyProperty(std::string_view property)
{
    for (const auto& entry : kRetiredSkyProperties)
        if (entry.Key == property)
            return &entry;
    return nullptr;
}

// Scene text carries the retired defaults at the serializer's precision, so the comparison is a
// tolerance rather than equality: `0.2746773` in a file against `0.274677312f` in the header.
bool MatchesRetiredDefault(const Float3& value, const float32 (&retiredDefault)[3])
{
    constexpr float32 kEpsilon = 1e-5f;
    return std::abs(value.X - retiredDefault[0]) <= kEpsilon &&
           std::abs(value.Y - retiredDefault[1]) <= kEpsilon &&
           std::abs(value.Z - retiredDefault[2]) <= kEpsilon;
}

// Warns once per process per retired property, naming what became of the authored value. Four
// outcomes, because "retired" alone leaves the author guessing whether their scene changed.
enum class RetiredSkyOutcome
{
    ConvertedToWhite, // held the old default: never art direction, so the physical sky stands
    CarriedToTint,    // an authored day key, carried onto the matching SunTint key
    DroppedNightLevel,// an authored night key: a LEVEL, not a multiplier, so it cannot convert
    ToggleRetired,    // UseSunLightColor was on: the sun's colour is authored through SunTint now
};

void WarnRetiredSkyProperty(const RetiredSkyProperty& retired, RetiredSkyOutcome outcome,
                            const Float3& authored)
{
    static std::mutex mutex;
    static std::unordered_set<std::string> warned;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!warned.emplace(std::string(retired.Key)).second)
            return;
    }

    switch (outcome)
    {
    case RetiredSkyOutcome::ConvertedToWhite:
        Logger::Log::Warning("Scene: SkyEnvironment.{} is retired ({}). It held the old default, "
                             "which was never art direction, so it converts to white (no tint) and "
                             "the sky is the physical one. Re-save the scene to drop the line.",
                             retired.Authored, kRetiredSunColorReason);
        break;
    case RetiredSkyOutcome::CarriedToTint:
        Logger::Log::Warning("Scene: SkyEnvironment.{} is retired ({}). Its authored value carries "
                             "over to SkyEnvironment.SunTint{}. The tint now multiplies a "
                             "physically derived sun instead of white, so the look shifts toward "
                             "physical — check it and re-key if you want the old strength. Re-save "
                             "the scene to store it under the new name.",
                             retired.Authored, kRetiredSunColorReason,
                             retired.Authored.substr(std::string_view("SunColor").size()));
        break;
    case RetiredSkyOutcome::DroppedNightLevel:
        Logger::Log::Warning("Scene: SkyEnvironment.{} is retired ({}), and its authored value "
                             "({}, {}, {}) is DROPPED rather than converted. That key set the "
                             "night's absolute level; SunTint is a multiplier and no longer touches "
                             "the moon at all, so reading it as one would invert your intent. The "
                             "night's level lives in the moon's colour now — re-author it there if "
                             "this scene needs a different night.",
                             retired.Authored, kRetiredSunColorReason, authored.X, authored.Y,
                             authored.Z);
        break;
    case RetiredSkyOutcome::ToggleRetired:
        Logger::Log::Warning("Scene: SkyEnvironment.{} is retired ({}) and was set to true — this "
                             "scene's sun colour came from its linked light and will now come from "
                             "the atmosphere. It was the only path that lit the scene with a colour "
                             "the sky dome did not have. Author the sun's colour with "
                             "SkyEnvironment.SunTint* instead; a light authored at 3200 K is a "
                             "SunTint key.",
                             retired.Authored, kRetiredSunColorReason);
        break;
    }
}

// A Math::Curve as one scene property line, every field of every key: "t,v,inTan,outTan,interp,mode"
// per key, keys separated by ';'. The one format for a curve whose keys may be Smooth, with tangents
// (ValueCurve's shape, the sky's sun illuminance curve): the compact sky format above keeps only time
// and value, and every key would come back Linear.
static std::string FormatCurveKeys(const Math::Curve& curve)
{
    std::string out;
    for (uint8 i = 0; i < curve.KeyCount; ++i)
    {
        const Math::CurveKey& k = curve.Keys[i];
        if (i > 0)
            out += ';';
        out += FormatFloat(k.Time);
        out += ',';
        out += FormatFloat(k.Value);
        out += ',';
        out += FormatFloat(k.InTangent);
        out += ',';
        out += FormatFloat(k.OutTangent);
        out += ',';
        out += std::to_string(static_cast<int>(k.Interp));
        out += ',';
        out += std::to_string(static_cast<int>(k.TangentMode));
    }
    return out;
}

// Parse FormatCurveKeys' line back into a curve. Missing trailing fields of a key read 0 / Linear /
// Auto, so the compact "time,value;..." form reads as Linear keys. An empty or garbage line gives an
// empty curve; the caller picks its own default for that.
static Math::Curve ParseCurveKeys(std::string_view value)
{
    Math::Curve curve{};
    size_t segStart = 0;
    while (segStart <= value.size() && curve.KeyCount < Math::Curve::Capacity)
    {
        const size_t segEnd = value.find(';', segStart);
        const std::string_view seg = value.substr(
            segStart, segEnd == std::string_view::npos ? value.size() - segStart : segEnd - segStart);
        if (!seg.empty())
        {
            float32 fields[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            size_t fieldStart = 0;
            for (int f = 0; f < 6; ++f)
            {
                const size_t fieldEnd = seg.find(',', fieldStart);
                const std::string_view tok = seg.substr(
                    fieldStart,
                    fieldEnd == std::string_view::npos ? seg.size() - fieldStart : fieldEnd - fieldStart);
                float32 parsed = 0.0f;
                if (ParseF32(tok, parsed, nullptr))
                    fields[f] = parsed;
                if (fieldEnd == std::string_view::npos)
                    break;
                fieldStart = fieldEnd + 1;
            }
            Math::CurveKey key;
            key.Time = fields[0];
            key.Value = fields[1];
            key.InTangent = fields[2];
            key.OutTangent = fields[3];
            key.Interp = static_cast<Math::CurveInterp>(std::clamp(static_cast<int>(fields[4]), 0, 2));
            key.TangentMode = static_cast<Math::CurveTangentMode>(std::clamp(static_cast<int>(fields[5]), 0, 4));
            curve.TryInsert(key);
        }
        if (segEnd == std::string_view::npos)
            break;
        segStart = segEnd + 1;
    }
    return curve;
}

// The sky's sun illuminance curve as the sky reads it: every key's time inside the day [0, 24) and its
// value a finite illuminance of 0 or more; a key with a non-finite time is dropped. An empty curve is
// the component's default.
static Math::Curve SanitisedSunIlluminanceCurve(const Math::Curve& parsed)
{
    constexpr float kDayHours = 24.0f;
    Math::Curve curve{};
    for (uint8 i = 0; i < parsed.KeyCount; ++i)
    {
        Math::CurveKey key = parsed.Keys[i];
        if (!std::isfinite(key.Time))
            continue;
        key.Time = std::clamp(key.Time, 0.0f, std::nextafter(kDayHours, 0.0f));
        key.Value = std::isfinite(key.Value) ? std::max(key.Value, 0.0f) : 0.0f;
        if (!std::isfinite(key.InTangent))
            key.InTangent = 0.0f;
        if (!std::isfinite(key.OutTangent))
            key.OutTangent = 0.0f;
        curve.TryInsert(key);
    }
    return curve.KeyCount > 0 ? curve : Components::SkySunDrive::DefaultSunIlluminanceCurve();
}

std::string_view SunIlluminanceSourceToken(Components::SkySunIlluminanceSource source)
{
    return source == Components::SkySunIlluminanceSource::Curve ? "Curve" : "Light";
}

// "Light" or "Curve", in any case, quoted or not. Anything else is the light, said out loud: the scene
// still loads with the sun the light holds.
Components::SkySunIlluminanceSource ParseSunIlluminanceSource(std::string_view value)
{
    std::string raw(value);
    std::string quoted;
    if (ParseQuotedString(value, quoted))
        raw = quoted;
    std::string lowered = raw;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lowered == "curve")
        return Components::SkySunIlluminanceSource::Curve;
    if (lowered != "light")
        Logger::Log::Warning("Scene: SkyEnvironment.SunIlluminanceSource = {} is not Light or Curve; the sky "
                             "takes the sun's illuminance from the light. Re-save the scene to store it.",
                             raw);
    return Components::SkySunIlluminanceSource::Light;
}

std::string_view SunPathToken(Components::SkySunPathKind kind)
{
    return kind == Components::SkySunPathKind::Custom ? "Custom" : "Earth";
}

// "Earth" or "Custom", in any case, quoted or not. Anything else is the Earth path, said out loud: a
// scene from a newer build, or a typo, still loads with a sun where a sun can be.
Components::SkySunPathKind ParseSunPath(std::string_view value)
{
    std::string raw(value);
    std::string quoted;
    if (ParseQuotedString(value, quoted))
        raw = quoted;
    std::string lowered = raw;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lowered == "custom")
        return Components::SkySunPathKind::Custom;
    if (lowered != "earth")
        Logger::Log::Warning("Scene: SkyEnvironment.SunPath = {} is not Earth or Custom; the sky uses the "
                             "Earth path. Re-save the scene to store it.",
                             raw);
    return Components::SkySunPathKind::Earth;
}

class SkyEnvironmentSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "SkyEnvironment"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::SkyEnvironment>(entity);
        if (!c)
            return;
        outLines.push_back(std::string("SkyEnvironment.TimeOfDayHours = ") + FormatFloat(c->TimeOfDayHours));
        outLines.push_back(std::string("SkyEnvironment.Latitude = ") + FormatFloat(c->Latitude));
        outLines.push_back(std::string("SkyEnvironment.DayOfYear = ") + std::to_string(c->DayOfYear));
        outLines.push_back(std::string("SkyEnvironment.NorthHeading = ") + FormatFloat(c->NorthHeading));
        outLines.push_back(std::string("SkyEnvironment.SunPath = ") + std::string(SunPathToken(c->SunPath)));
        outLines.push_back(std::string("SkyEnvironment.CustomAxisHeading = ") + FormatFloat(c->CustomAxisHeading));
        outLines.push_back(std::string("SkyEnvironment.CustomAxisAltitude = ") + FormatFloat(c->CustomAxisAltitude));
        outLines.push_back(std::string("SkyEnvironment.CustomNoonHeight = ") + FormatFloat(c->CustomNoonHeight));
        outLines.push_back(std::string("SkyEnvironment.DayKeyTimeMidnight = ") + FormatFloat(c->DayKeyTimesHours.Midnight));
        outLines.push_back(std::string("SkyEnvironment.DayKeyTimeDawn = ") + FormatFloat(c->DayKeyTimesHours.Dawn));
        outLines.push_back(std::string("SkyEnvironment.DayKeyTimeMidday = ") + FormatFloat(c->DayKeyTimesHours.Midday));
        outLines.push_back(std::string("SkyEnvironment.DayKeyTimeSunset = ") + FormatFloat(c->DayKeyTimesHours.Sunset));
        outLines.push_back(std::string("SkyEnvironment.AnimateTimeOfDay = ") + (c->AnimateTimeOfDay ? "true" : "false"));
        outLines.push_back(std::string("SkyEnvironment.TimeOfDayCycleSeconds = ") + FormatFloat(c->TimeOfDayCycleSeconds));
        outLines.push_back(std::string("SkyEnvironment.SkyExposureTrim = ") + FormatFloat(c->SkyExposureTrim));
        outLines.push_back(std::string("SkyEnvironment.IblIntensity = ") + FormatFloat(c->IblIntensity));
        outLines.push_back(std::string("SkyEnvironment.IblLowerHemisphereDarkness = ") + FormatFloat(c->IblLowerHemisphereDarkness));
        outLines.push_back(std::string("SkyEnvironment.AmbientTintSky = ") + FormatFloat3(c->AmbientTintSky[0], c->AmbientTintSky[1], c->AmbientTintSky[2]));
        outLines.push_back(std::string("SkyEnvironment.AmbientTintEquator = ") + FormatFloat3(c->AmbientTintEquator[0], c->AmbientTintEquator[1], c->AmbientTintEquator[2]));
        outLines.push_back(std::string("SkyEnvironment.AmbientTintGround = ") + FormatFloat3(c->AmbientTintGround[0], c->AmbientTintGround[1], c->AmbientTintGround[2]));
        outLines.push_back(std::string("SkyEnvironment.Mode = ") + std::to_string(static_cast<uint32_t>(c->Mode)));
        outLines.push_back(std::string("SkyEnvironment.GradientSkyTopColor = ") + FormatFloat3(c->GradientSkyTopColor[0], c->GradientSkyTopColor[1], c->GradientSkyTopColor[2]));
        outLines.push_back(std::string("SkyEnvironment.GradientSkyHorizonColor = ") + FormatFloat3(c->GradientSkyHorizonColor[0], c->GradientSkyHorizonColor[1], c->GradientSkyHorizonColor[2]));
        outLines.push_back(std::string("SkyEnvironment.GradientSkyBottomColor = ") + FormatFloat3(c->GradientSkyBottomColor[0], c->GradientSkyBottomColor[1], c->GradientSkyBottomColor[2]));
        outLines.push_back(std::string("SkyEnvironment.GradientSkyIntensity = ") + FormatFloat(c->GradientSkyIntensity));
        outLines.push_back(std::string("SkyEnvironment.BelowHorizonMode = ") + std::to_string(static_cast<uint32_t>(c->BelowHorizonMode)));
        outLines.push_back(std::string("SkyEnvironment.GroundHazeStrength = ") + FormatFloat(c->GroundHazeStrength));
        if (std::string sunLightRef = FormatEntityReferenceForSave(ctx, c->SunLight); !sunLightRef.empty())
            outLines.push_back(std::string("SkyEnvironment.SunLight = ") + sunLightRef);
        outLines.push_back(std::string("SkyEnvironment.TimeOfDayDrivesSunLight = ") + (c->TimeOfDayDrivesSunLight ? "true" : "false"));
        outLines.push_back(std::string("SkyEnvironment.DriveSunColor = ") + (c->DriveSunColor ? "true" : "false"));
        outLines.push_back(std::string("SkyEnvironment.SunIlluminanceSource = ") +
                           std::string(SunIlluminanceSourceToken(c->SunIlluminanceSource)));
        outLines.push_back(std::string("SkyEnvironment.SunIlluminanceCurve = ") + FormatCurveKeys(c->SunIlluminanceCurve));
        outLines.push_back(std::string("SkyEnvironment.MoonlightIlluminance = ") + FormatFloat(c->MoonlightIlluminance));
        outLines.push_back(std::string("SkyEnvironment.AutoSunMoon = ") + (c->AutoSunMoon ? "true" : "false"));
        outLines.push_back(std::string("SkyEnvironment.ShowSun = ") + (c->ShowSun ? "true" : "false"));
        outLines.push_back(std::string("SkyEnvironment.ShowMoon = ") + (c->ShowMoon ? "true" : "false"));
        outLines.push_back(std::string("SkyEnvironment.SunSize = ") + FormatFloat(c->SunSize));
        outLines.push_back(std::string("SkyEnvironment.SunSize2D = ") + FormatFloat(c->SunSize2D));
        outLines.push_back(std::string("SkyEnvironment.SkyPan2D = ") + FormatFloat(c->SkyPan2D));
        outLines.push_back(std::string("SkyEnvironment.MoonPhase01 = ") + FormatFloat(c->MoonPhase01));
        outLines.push_back(std::string("SkyEnvironment.MoonArcPosition = ") + FormatFloat(c->MoonArcPosition));
        outLines.push_back(std::string("SkyEnvironment.AutoMoonArc = ") + (c->AutoMoonArc ? "true" : "false"));
        outLines.push_back(std::string("SkyEnvironment.MoonCycleDays = ") + FormatFloat(c->MoonCycleDays));
        outLines.push_back(std::string("SkyEnvironment.MoonSize = ") + FormatFloat(c->MoonSize));
        outLines.push_back(std::string("SkyEnvironment.MoonSize2D = ") + FormatFloat(c->MoonSize2D));
        outLines.push_back(std::string("SkyEnvironment.MoonExposureEV = ") + FormatFloat(c->MoonExposureEV));
        outLines.push_back(std::string("SkyEnvironment.FallingStarsEnabled = ") + (c->FallingStarsEnabled ? "true" : "false"));
        outLines.push_back(std::string("SkyEnvironment.FallingStarAmount = ") + FormatFloat(c->FallingStarAmount));
        outLines.push_back(std::string("SkyEnvironment.FallingStarFrequency = ") + FormatFloat(c->FallingStarFrequency));
        outLines.push_back(std::string("SkyEnvironment.FallingStarSpeed = ") + FormatFloat(c->FallingStarSpeed));
        outLines.push_back(std::string("SkyEnvironment.FallingStarLength = ") + FormatFloat(c->FallingStarLength));
        outLines.push_back(std::string("SkyEnvironment.FallingStarThickness = ") + FormatFloat(c->FallingStarThickness));
        outLines.push_back(std::string("SkyEnvironment.FallingStarDotSize = ") + FormatFloat(c->FallingStarDotSize));
        outLines.push_back(std::string("SkyEnvironment.FallingStarDotSize2D = ") + FormatFloat(c->FallingStarDotSize2D));
        outLines.push_back(std::string("SkyEnvironment.SunDirOverride = ") + FormatFloat3(c->SunDirOverride[0], c->SunDirOverride[1], c->SunDirOverride[2]));
        outLines.push_back(std::string("SkyEnvironment.SunTintMidnight = ") + FormatFloat3(
                                                                                 c->SunTintKeys.Midnight[0], c->SunTintKeys.Midnight[1], c->SunTintKeys.Midnight[2]));
        outLines.push_back(std::string("SkyEnvironment.SunTintDawn = ") + FormatFloat3(
                                                                             c->SunTintKeys.Dawn[0], c->SunTintKeys.Dawn[1], c->SunTintKeys.Dawn[2]));
        outLines.push_back(std::string("SkyEnvironment.SunTintMidday = ") + FormatFloat3(
                                                                               c->SunTintKeys.Midday[0], c->SunTintKeys.Midday[1], c->SunTintKeys.Midday[2]));
        outLines.push_back(std::string("SkyEnvironment.SunTintSunset = ") + FormatFloat3(
                                                                               c->SunTintKeys.Sunset[0], c->SunTintKeys.Sunset[1], c->SunTintKeys.Sunset[2]));
        outLines.push_back(std::string("SkyEnvironment.GroundAlbedoMidnight = ") + FormatFloat3(
                                                                                       c->GroundAlbedoKeys.Midnight[0], c->GroundAlbedoKeys.Midnight[1], c->GroundAlbedoKeys.Midnight[2]));
        outLines.push_back(std::string("SkyEnvironment.GroundAlbedoDawn = ") + FormatFloat3(
                                                                                   c->GroundAlbedoKeys.Dawn[0], c->GroundAlbedoKeys.Dawn[1], c->GroundAlbedoKeys.Dawn[2]));
        outLines.push_back(std::string("SkyEnvironment.GroundAlbedoMidday = ") + FormatFloat3(
                                                                                     c->GroundAlbedoKeys.Midday[0], c->GroundAlbedoKeys.Midday[1], c->GroundAlbedoKeys.Midday[2]));
        outLines.push_back(std::string("SkyEnvironment.GroundAlbedoSunset = ") + FormatFloat3(
                                                                                     c->GroundAlbedoKeys.Sunset[0], c->GroundAlbedoKeys.Sunset[1], c->GroundAlbedoKeys.Sunset[2]));
        outLines.push_back(std::string("SkyEnvironment.GroundNightColorMidnight = ") + FormatFloat3(
                                                                                           c->GroundNightColorKeys.Midnight[0], c->GroundNightColorKeys.Midnight[1], c->GroundNightColorKeys.Midnight[2]));
        outLines.push_back(std::string("SkyEnvironment.GroundNightColorDawn = ") + FormatFloat3(
                                                                                       c->GroundNightColorKeys.Dawn[0], c->GroundNightColorKeys.Dawn[1], c->GroundNightColorKeys.Dawn[2]));
        outLines.push_back(std::string("SkyEnvironment.GroundNightColorMidday = ") + FormatFloat3(
                                                                                         c->GroundNightColorKeys.Midday[0], c->GroundNightColorKeys.Midday[1], c->GroundNightColorKeys.Midday[2]));
        outLines.push_back(std::string("SkyEnvironment.GroundNightColorSunset = ") + FormatFloat3(
                                                                                         c->GroundNightColorKeys.Sunset[0], c->GroundNightColorKeys.Sunset[1], c->GroundNightColorKeys.Sunset[2]));
        outLines.push_back(std::string("SkyEnvironment.GroundBrightnessCurve = ") + FormatSkyCurve(c->GroundBrightnessKeys));
        outLines.push_back(std::string("SkyEnvironment.GroundBrightnessCurveMode = ") + std::string(ToSkyScalarCurveShapeModeName(c->GroundBrightnessShapeMode)));
        outLines.push_back(std::string("SkyEnvironment.GroundBrightnessBezier = ") + FormatSkyScalarBezier(c->GroundBrightnessBezier));
        outLines.push_back(std::string("SkyEnvironment.BelowHorizonBlendSharpnessCurve = ") + FormatSkyCurve(c->BelowHorizonBlendSharpnessKeys));
        outLines.push_back(std::string("SkyEnvironment.BelowHorizonBlendSharpnessCurveMode = ") + std::string(ToSkyScalarCurveShapeModeName(c->BelowHorizonBlendSharpnessShapeMode)));
        outLines.push_back(std::string("SkyEnvironment.BelowHorizonBlendSharpnessBezier = ") + FormatSkyScalarBezier(c->BelowHorizonBlendSharpnessBezier));
        outLines.push_back(std::string("SkyEnvironment.BelowHorizonDarknessCurve = ") + FormatSkyCurve(c->BelowHorizonDarknessKeys));
        outLines.push_back(std::string("SkyEnvironment.BelowHorizonDarknessCurveMode = ") + std::string(ToSkyScalarCurveShapeModeName(c->BelowHorizonDarknessShapeMode)));
        outLines.push_back(std::string("SkyEnvironment.BelowHorizonDarknessBezier = ") + FormatSkyScalarBezier(c->BelowHorizonDarknessBezier));
        outLines.push_back(std::string("SkyEnvironment.BelowHorizonDarkColorMidnight = ") + FormatFloat3(
                                                                                                c->BelowHorizonDarkColorKeys.Midnight[0], c->BelowHorizonDarkColorKeys.Midnight[1], c->BelowHorizonDarkColorKeys.Midnight[2]));
        outLines.push_back(std::string("SkyEnvironment.BelowHorizonDarkColorDawn = ") + FormatFloat3(
                                                                                            c->BelowHorizonDarkColorKeys.Dawn[0], c->BelowHorizonDarkColorKeys.Dawn[1], c->BelowHorizonDarkColorKeys.Dawn[2]));
        outLines.push_back(std::string("SkyEnvironment.BelowHorizonDarkColorMidday = ") + FormatFloat3(
                                                                                              c->BelowHorizonDarkColorKeys.Midday[0], c->BelowHorizonDarkColorKeys.Midday[1], c->BelowHorizonDarkColorKeys.Midday[2]));
        outLines.push_back(std::string("SkyEnvironment.BelowHorizonDarkColorSunset = ") + FormatFloat3(
                                                                                              c->BelowHorizonDarkColorKeys.Sunset[0], c->BelowHorizonDarkColorKeys.Sunset[1], c->BelowHorizonDarkColorKeys.Sunset[2]));
        outLines.push_back(std::string("SkyEnvironment.GroundHorizonColorMidnight = ") + FormatFloat3(
                                                                                             c->GroundHorizonColorKeys.Midnight[0], c->GroundHorizonColorKeys.Midnight[1], c->GroundHorizonColorKeys.Midnight[2]));
        outLines.push_back(std::string("SkyEnvironment.GroundHorizonColorDawn = ") + FormatFloat3(
                                                                                         c->GroundHorizonColorKeys.Dawn[0], c->GroundHorizonColorKeys.Dawn[1], c->GroundHorizonColorKeys.Dawn[2]));
        outLines.push_back(std::string("SkyEnvironment.GroundHorizonColorMidday = ") + FormatFloat3(
                                                                                           c->GroundHorizonColorKeys.Midday[0], c->GroundHorizonColorKeys.Midday[1], c->GroundHorizonColorKeys.Midday[2]));
        outLines.push_back(std::string("SkyEnvironment.GroundHorizonColorSunset = ") + FormatFloat3(
                                                                                           c->GroundHorizonColorKeys.Sunset[0], c->GroundHorizonColorKeys.Sunset[1], c->GroundHorizonColorKeys.Sunset[2]));
        outLines.push_back(std::string("SkyEnvironment.GroundHorizonNightColorMidnight = ") + FormatFloat3(
                                                                                                  c->GroundHorizonNightColorKeys.Midnight[0], c->GroundHorizonNightColorKeys.Midnight[1], c->GroundHorizonNightColorKeys.Midnight[2]));
        outLines.push_back(std::string("SkyEnvironment.GroundHorizonNightColorDawn = ") + FormatFloat3(
                                                                                              c->GroundHorizonNightColorKeys.Dawn[0], c->GroundHorizonNightColorKeys.Dawn[1], c->GroundHorizonNightColorKeys.Dawn[2]));
        outLines.push_back(std::string("SkyEnvironment.GroundHorizonNightColorMidday = ") + FormatFloat3(
                                                                                                c->GroundHorizonNightColorKeys.Midday[0], c->GroundHorizonNightColorKeys.Midday[1], c->GroundHorizonNightColorKeys.Midday[2]));
        outLines.push_back(std::string("SkyEnvironment.GroundHorizonNightColorSunset = ") + FormatFloat3(
                                                                                                c->GroundHorizonNightColorKeys.Sunset[0], c->GroundHorizonNightColorKeys.Sunset[1], c->GroundHorizonNightColorKeys.Sunset[2]));
        outLines.push_back(std::string("SkyEnvironment.NightSkyHorizonColorMidnight = ") + FormatFloat3(
                                                                                               c->NightSkyHorizonColorKeys.Midnight[0], c->NightSkyHorizonColorKeys.Midnight[1], c->NightSkyHorizonColorKeys.Midnight[2]));
        outLines.push_back(std::string("SkyEnvironment.NightSkyHorizonColorDawn = ") + FormatFloat3(
                                                                                           c->NightSkyHorizonColorKeys.Dawn[0], c->NightSkyHorizonColorKeys.Dawn[1], c->NightSkyHorizonColorKeys.Dawn[2]));
        outLines.push_back(std::string("SkyEnvironment.NightSkyHorizonColorMidday = ") + FormatFloat3(
                                                                                             c->NightSkyHorizonColorKeys.Midday[0], c->NightSkyHorizonColorKeys.Midday[1], c->NightSkyHorizonColorKeys.Midday[2]));
        outLines.push_back(std::string("SkyEnvironment.NightSkyHorizonColorSunset = ") + FormatFloat3(
                                                                                             c->NightSkyHorizonColorKeys.Sunset[0], c->NightSkyHorizonColorKeys.Sunset[1], c->NightSkyHorizonColorKeys.Sunset[2]));
        outLines.push_back(std::string("SkyEnvironment.GroundHorizonCosWidthCurve = ") + FormatSkyCurve(c->GroundHorizonCosWidthKeys));
        outLines.push_back(std::string("SkyEnvironment.GroundHorizonCosWidthCurveMode = ") + std::string(ToSkyScalarCurveShapeModeName(c->GroundHorizonCosWidthShapeMode)));
        outLines.push_back(std::string("SkyEnvironment.GroundHorizonCosWidthBezier = ") + FormatSkyScalarBezier(c->GroundHorizonCosWidthBezier));
        outLines.push_back(std::string("SkyEnvironment.GroundHorizonNightCosWidthCurve = ") + FormatSkyCurve(c->GroundHorizonNightCosWidthKeys));
        outLines.push_back(std::string("SkyEnvironment.GroundHorizonNightCosWidthCurveMode = ") + std::string(ToSkyScalarCurveShapeModeName(c->GroundHorizonNightCosWidthShapeMode)));
        outLines.push_back(std::string("SkyEnvironment.GroundHorizonNightCosWidthBezier = ") + FormatSkyScalarBezier(c->GroundHorizonNightCosWidthBezier));
        outLines.push_back(std::string("SkyEnvironment.StarDensity = ") + FormatFloat(c->StarDensity));
        outLines.push_back(std::string("SkyEnvironment.StarBrightness = ") + FormatFloat(c->StarBrightness));
        outLines.push_back(std::string("SkyEnvironment.StarSize = ") + FormatFloat(c->StarSize));
        outLines.push_back(std::string("SkyEnvironment.StarDiamondShape = ") + FormatFloat(c->StarDiamondShape));
        outLines.push_back(std::string("SkyEnvironment.StarCoreSize = ") + FormatFloat(c->StarCoreSize));
        outLines.push_back(std::string("SkyEnvironment.StarGlowFalloff = ") + FormatFloat(c->StarGlowFalloff));
        outLines.push_back(std::string("SkyEnvironment.TwinkleSpeed = ") + FormatFloat(c->TwinkleSpeed));
        outLines.push_back(std::string("SkyEnvironment.TwinkleIntensity = ") + FormatFloat(c->TwinkleIntensity));
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        Components::SkyEnvironment c{};
        if (auto* existing = world.GetComponent<Components::SkyEnvironment>(entity))
            c = *existing;

        if (property == "timeofdayhours")
        {
            if (!ParseF32(value, c.TimeOfDayHours, outError))
                return false;
        }
        else if (property == "latitude")
        {
            if (!ParseF32(value, c.Latitude, outError))
                return false;
            c.Latitude = std::isfinite(c.Latitude)
                             ? std::clamp(c.Latitude, -Rendering::kMaximumLatitudeDegrees,
                                          Rendering::kMaximumLatitudeDegrees)
                             : 0.0f;
        }
        else if (property == "dayofyear")
        {
            if (!ParseI32(value, c.DayOfYear, outError))
                return false;
            c.DayOfYear = std::clamp(c.DayOfYear, int32_t{1}, Rendering::kDaysInCalendarYear);
        }
        else if (property == "northheading")
        {
            if (!ParseF32(value, c.NorthHeading, outError))
                return false;
            c.NorthHeading = Rendering::SanitisedHeadingDegrees(c.NorthHeading);
        }
        else if (property == "sunpath")
        {
            c.SunPath = ParseSunPath(value);
        }
        else if (property == "customaxisheading")
        {
            if (!ParseF32(value, c.CustomAxisHeading, outError))
                return false;
            c.CustomAxisHeading = Rendering::SanitisedHeadingDegrees(c.CustomAxisHeading);
        }
        else if (property == "customaxisaltitude")
        {
            if (!ParseF32(value, c.CustomAxisAltitude, outError))
                return false;
            c.CustomAxisAltitude = std::isfinite(c.CustomAxisAltitude)
                                       ? std::clamp(c.CustomAxisAltitude, -Rendering::kMaximumLatitudeDegrees,
                                                    Rendering::kMaximumLatitudeDegrees)
                                       : 0.0f;
        }
        else if (property == "customnoonheight")
        {
            if (!ParseF32(value, c.CustomNoonHeight, outError))
                return false;
            c.CustomNoonHeight = std::isfinite(c.CustomNoonHeight)
                                     ? std::clamp(c.CustomNoonHeight, Rendering::kLowestNoonHeightDegrees,
                                                  Rendering::kHighestNoonHeightDegrees)
                                     : Rendering::kOverheadNoonHeightDegrees;
        }
        else if (property == "daykeytimemidnight")
        {
            if (!ParseF32(value, c.DayKeyTimesHours.Midnight, outError))
                return false;
        }
        else if (property == "daykeytimedawn")
        {
            if (!ParseF32(value, c.DayKeyTimesHours.Dawn, outError))
                return false;
        }
        else if (property == "daykeytimemidday")
        {
            if (!ParseF32(value, c.DayKeyTimesHours.Midday, outError))
                return false;
        }
        else if (property == "daykeytimesunset")
        {
            if (!ParseF32(value, c.DayKeyTimesHours.Sunset, outError))
                return false;
        }
        else if (property == "animatetimeofday")
        {
            if (!ParseBool(value, c.AnimateTimeOfDay, outError))
                return false;
        }
        else if (property == "timeofdaycycleseconds")
        {
            if (!ParseF32(value, c.TimeOfDayCycleSeconds, outError))
                return false;
        }
        else if (property == "skyexposuretrim")
        {
            if (!ParseF32(value, c.SkyExposureTrim, outError))
                return false;
        }
        // Migration: the old per-time-of-day sky-EV curve collapsed to one flat trim. Adopt the
        // daytime key (or the legacy single value) as the trim; the night/dawn/sunset keys fall
        // through to the unknown-property tolerance below (the atmosphere + auto-exposure own the
        // day/night arc now).
        else if (property == "exposureevmidday" || property == "exposureev")
        {
            if (!ParseF32(value, c.SkyExposureTrim, outError))
                return false;
        }
        else if (property == "iblintensity")
        {
            if (!ParseF32(value, c.IblIntensity, outError))
                return false;
        }
        else if (property == "ibllowerhemispheredarkness")
        {
            if (!ParseF32(value, c.IblLowerHemisphereDarkness, outError))
                return false;
        }
        else if (property == "ambienttintsky")
        {
            Float3 v{c.AmbientTintSky[0], c.AmbientTintSky[1], c.AmbientTintSky[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.AmbientTintSky must be (x, y, z)";
                return false;
            }
            c.AmbientTintSky[0] = v.X;
            c.AmbientTintSky[1] = v.Y;
            c.AmbientTintSky[2] = v.Z;
        }
        else if (property == "ambienttintequator")
        {
            Float3 v{c.AmbientTintEquator[0], c.AmbientTintEquator[1], c.AmbientTintEquator[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.AmbientTintEquator must be (x, y, z)";
                return false;
            }
            c.AmbientTintEquator[0] = v.X;
            c.AmbientTintEquator[1] = v.Y;
            c.AmbientTintEquator[2] = v.Z;
        }
        else if (property == "ambienttintground")
        {
            Float3 v{c.AmbientTintGround[0], c.AmbientTintGround[1], c.AmbientTintGround[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.AmbientTintGround must be (x, y, z)";
                return false;
            }
            c.AmbientTintGround[0] = v.X;
            c.AmbientTintGround[1] = v.Y;
            c.AmbientTintGround[2] = v.Z;
        }
        else if (property == "mode")
        {
            uint32_t v = 0;
            if (!ParseU32(value, v, outError))
                return false;
            if (v > 1)
            {
                if (outError)
                    *outError = "SkyEnvironment.Mode must be 0-1";
                return false;
            }
            c.Mode = static_cast<Components::SkyMode>(v);
        }
        else if (property == "gradientskytopcolor")
        {
            Float3 v{c.GradientSkyTopColor[0], c.GradientSkyTopColor[1], c.GradientSkyTopColor[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GradientSkyTopColor must be (x, y, z)";
                return false;
            }
            c.GradientSkyTopColor[0] = v.X;
            c.GradientSkyTopColor[1] = v.Y;
            c.GradientSkyTopColor[2] = v.Z;
        }
        else if (property == "gradientskyhorizoncolor")
        {
            Float3 v{c.GradientSkyHorizonColor[0], c.GradientSkyHorizonColor[1], c.GradientSkyHorizonColor[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GradientSkyHorizonColor must be (x, y, z)";
                return false;
            }
            c.GradientSkyHorizonColor[0] = v.X;
            c.GradientSkyHorizonColor[1] = v.Y;
            c.GradientSkyHorizonColor[2] = v.Z;
        }
        else if (property == "gradientskybottomcolor")
        {
            Float3 v{c.GradientSkyBottomColor[0], c.GradientSkyBottomColor[1], c.GradientSkyBottomColor[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GradientSkyBottomColor must be (x, y, z)";
                return false;
            }
            c.GradientSkyBottomColor[0] = v.X;
            c.GradientSkyBottomColor[1] = v.Y;
            c.GradientSkyBottomColor[2] = v.Z;
        }
        else if (property == "gradientskyintensity")
        {
            if (!ParseF32(value, c.GradientSkyIntensity, outError))
                return false;
        }
        else if (property == "belowhorizonmode")
        {
            uint32_t v = 0;
            if (!ParseU32(value, v, outError))
                return false;
            if (v > 2)
            {
                if (outError)
                    *outError = "SkyEnvironment.BelowHorizonMode must be 0-2";
                return false;
            }
            c.BelowHorizonMode = static_cast<GameEngine::Rendering::SkyBelowHorizonMode>(v);
        }
        else if (property == "groundhazestrength")
        {
            if (!ParseF32(value, c.GroundHazeStrength, outError))
                return false;
        }
        else if (property == "turbidity" ||
                 property == "cloudamount2d" ||
                 property == "cloudwidth2d" ||
                 property == "cloudheight2d" ||
                 property == "cloudrandom2d" ||
                 property == "cloudspeed2d" ||
                 property == "clouddirection2d")
        {
            float legacyValue{};
            if (!ParseF32(value, legacyValue, outError))
                return false;
        }
        else if (property == "sunlight")
        {
            if (!TryResolveEntityReference(ctx, value, c.SunLight))
            {
                if (outError)
                    *outError = "SkyEnvironment.SunLight: expected quoted entity-id string";
                return false;
            }
        }
        else if (property == "timeofdaydrivessunlight" ||
                 property == "timeofdaydrivessunlightcolor" ||
                 property == "syncsunlightcolor")
        {
            if (!ParseBool(value, c.TimeOfDayDrivesSunLight, outError))
                return false;
        }
        else if (property == "drivesuncolor")
        {
            if (!ParseBool(value, c.DriveSunColor, outError))
                return false;
        }
        else if (property == "sunilluminancesource")
        {
            c.SunIlluminanceSource = ParseSunIlluminanceSource(value);
        }
        else if (property == "sunilluminancecurve")
        {
            c.SunIlluminanceCurve = SanitisedSunIlluminanceCurve(ParseCurveKeys(value));
        }
        else if (property == "moonlightilluminance")
        {
            if (!ParseF32(value, c.MoonlightIlluminance, outError))
                return false;
            c.MoonlightIlluminance = Components::SkySunDrive::SanitisedMoonlightLux(c.MoonlightIlluminance);
        }
        else if (property == "autosunmoon")
        {
            if (!ParseBool(value, c.AutoSunMoon, outError))
                return false;
        }
        else if (property == "showsun")
        {
            if (!ParseBool(value, c.ShowSun, outError))
                return false;
        }
        else if (property == "showmoon")
        {
            if (!ParseBool(value, c.ShowMoon, outError))
                return false;
        }
        else if (property == "sunsize")
        {
            if (!ParseF32(value, c.SunSize, outError))
                return false;
            // std::clamp passes a NaN through and turns an overflow into the limit, so a non-finite
            // size is the physical sun's, as the system reads it.
            c.SunSize = std::isfinite(c.SunSize)
                            ? std::clamp(c.SunSize, Components::kSunSizeMin, Components::kSunSizeMax)
                            : Components::SkyEnvironment{}.SunSize;
        }
        else if (property == "sunsize2d")
        {
            if (!ParseF32(value, c.SunSize2D, outError))
                return false;
            c.SunSize2D = std::clamp(c.SunSize2D, 0.1f, 6.0f);
        }
        else if (property == "skypan2d")
        {
            if (!ParseF32(value, c.SkyPan2D, outError))
                return false;
            c.SkyPan2D = std::clamp(c.SkyPan2D, -1.0f, 1.0f);
        }
        else if (property == "moonphase01")
        {
            if (!ParseF32(value, c.MoonPhase01, outError))
                return false;
            c.MoonPhase01 = std::clamp(c.MoonPhase01, 0.0f, 1.0f);
        }
        else if (property == "moonarcposition")
        {
            if (!ParseF32(value, c.MoonArcPosition, outError))
                return false;
            c.MoonArcPosition = std::clamp(c.MoonArcPosition, 0.0f, 1.0f);
        }
        else if (property == "mooncycle01")
        {
            if (!ParseF32(value, c.MoonArcPosition, outError))
                return false;
            c.MoonArcPosition = std::clamp(c.MoonArcPosition, 0.0f, 1.0f);
        }
        else if (property == "automoonarc")
        {
            if (!ParseBool(value, c.AutoMoonArc, outError))
                return false;
        }
        else if (property == "automoonphase")
        {
            if (!ParseBool(value, c.AutoMoonArc, outError))
                return false;
        }
        else if (property == "mooncycledays")
        {
            if (!ParseF32(value, c.MoonCycleDays, outError))
                return false;
            c.MoonCycleDays = std::clamp(c.MoonCycleDays, 1.0f, 365.0f);
        }
        else if (property == "moonsize")
        {
            if (!ParseF32(value, c.MoonSize, outError))
                return false;
            c.MoonSize = std::clamp(c.MoonSize, 0.1f, 6.0f);
        }
        else if (property == "moonsize2d")
        {
            if (!ParseF32(value, c.MoonSize2D, outError))
                return false;
            c.MoonSize2D = std::clamp(c.MoonSize2D, 0.1f, 6.0f);
        }
        else if (property == "moonexposureev")
        {
            if (!ParseF32(value, c.MoonExposureEV, outError))
                return false;
            c.MoonExposureEV = std::clamp(c.MoonExposureEV, -8.0f, 8.0f);
        }
        else if (property == "fallingstarsenabled")
        {
            if (!ParseBool(value, c.FallingStarsEnabled, outError))
                return false;
        }
        else if (property == "fallingstaramount")
        {
            if (!ParseF32(value, c.FallingStarAmount, outError))
                return false;
            c.FallingStarAmount = std::clamp(c.FallingStarAmount, 0.0f, 1.0f);
        }
        else if (property == "fallingstarfrequency")
        {
            if (!ParseF32(value, c.FallingStarFrequency, outError))
                return false;
            c.FallingStarFrequency = std::clamp(c.FallingStarFrequency, 0.0f, 4.0f);
        }
        else if (property == "fallingstarspeed")
        {
            if (!ParseF32(value, c.FallingStarSpeed, outError))
                return false;
            c.FallingStarSpeed = std::clamp(c.FallingStarSpeed, 0.2f, 50.0f);
        }
        else if (property == "fallingstarlength")
        {
            if (!ParseF32(value, c.FallingStarLength, outError))
                return false;
            c.FallingStarLength = std::clamp(c.FallingStarLength, 0.2f, 4.0f);
        }
        else if (property == "fallingstarthickness")
        {
            if (!ParseF32(value, c.FallingStarThickness, outError))
                return false;
            c.FallingStarThickness = std::clamp(c.FallingStarThickness, 0.1f, 4.0f);
        }
        else if (property == "fallingstardotsize")
        {
            if (!ParseF32(value, c.FallingStarDotSize, outError))
                return false;
            c.FallingStarDotSize = std::clamp(c.FallingStarDotSize, 0.1f, 4.0f);
        }
        else if (property == "fallingstardotsize2d")
        {
            if (!ParseF32(value, c.FallingStarDotSize2D, outError))
                return false;
            c.FallingStarDotSize2D = std::clamp(c.FallingStarDotSize2D, 0.1f, 4.0f);
        }
        else if (property == "sundiroverride")
        {
            Float3 v{c.SunDirOverride[0], c.SunDirOverride[1], c.SunDirOverride[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.SunDirOverride must be (x, y, z)";
                return false;
            }
            c.SunDirOverride[0] = v.X;
            c.SunDirOverride[1] = v.Y;
            c.SunDirOverride[2] = v.Z;
        }
        else if (property == "suntint")
        {
            Float3 v{c.SunTintKeys.Midnight[0], c.SunTintKeys.Midnight[1], c.SunTintKeys.Midnight[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.SunTint must be (x, y, z)";
                return false;
            }
            Components::SkyVec3DayKeysSetUniform(c.SunTintKeys, v.X, v.Y, v.Z);
        }
        else if (property == "suntintmidnight")
        {
            Float3 v{c.SunTintKeys.Midnight[0], c.SunTintKeys.Midnight[1], c.SunTintKeys.Midnight[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.SunTintMidnight must be (x, y, z)";
                return false;
            }
            c.SunTintKeys.Midnight[0] = v.X;
            c.SunTintKeys.Midnight[1] = v.Y;
            c.SunTintKeys.Midnight[2] = v.Z;
        }
        else if (property == "suntintdawn")
        {
            Float3 v{c.SunTintKeys.Dawn[0], c.SunTintKeys.Dawn[1], c.SunTintKeys.Dawn[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.SunTintDawn must be (x, y, z)";
                return false;
            }
            c.SunTintKeys.Dawn[0] = v.X;
            c.SunTintKeys.Dawn[1] = v.Y;
            c.SunTintKeys.Dawn[2] = v.Z;
        }
        else if (property == "suntintmidday")
        {
            Float3 v{c.SunTintKeys.Midday[0], c.SunTintKeys.Midday[1], c.SunTintKeys.Midday[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.SunTintMidday must be (x, y, z)";
                return false;
            }
            c.SunTintKeys.Midday[0] = v.X;
            c.SunTintKeys.Midday[1] = v.Y;
            c.SunTintKeys.Midday[2] = v.Z;
        }
        else if (property == "suntintsunset")
        {
            Float3 v{c.SunTintKeys.Sunset[0], c.SunTintKeys.Sunset[1], c.SunTintKeys.Sunset[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.SunTintSunset must be (x, y, z)";
                return false;
            }
            c.SunTintKeys.Sunset[0] = v.X;
            c.SunTintKeys.Sunset[1] = v.Y;
            c.SunTintKeys.Sunset[2] = v.Z;
        }
        else if (property == "groundalbedo")
        {
            Float3 v{c.GroundAlbedoKeys.Midnight[0], c.GroundAlbedoKeys.Midnight[1], c.GroundAlbedoKeys.Midnight[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundAlbedo must be (x, y, z)";
                return false;
            }
            Components::SkyVec3DayKeysSetUniform(c.GroundAlbedoKeys, v.X, v.Y, v.Z);
        }
        else if (property == "groundalbedomidnight")
        {
            Float3 v{c.GroundAlbedoKeys.Midnight[0], c.GroundAlbedoKeys.Midnight[1], c.GroundAlbedoKeys.Midnight[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundAlbedoMidnight must be (x, y, z)";
                return false;
            }
            c.GroundAlbedoKeys.Midnight[0] = v.X;
            c.GroundAlbedoKeys.Midnight[1] = v.Y;
            c.GroundAlbedoKeys.Midnight[2] = v.Z;
        }
        else if (property == "groundalbedodawn")
        {
            Float3 v{c.GroundAlbedoKeys.Dawn[0], c.GroundAlbedoKeys.Dawn[1], c.GroundAlbedoKeys.Dawn[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundAlbedoDawn must be (x, y, z)";
                return false;
            }
            c.GroundAlbedoKeys.Dawn[0] = v.X;
            c.GroundAlbedoKeys.Dawn[1] = v.Y;
            c.GroundAlbedoKeys.Dawn[2] = v.Z;
        }
        else if (property == "groundalbedomidday")
        {
            Float3 v{c.GroundAlbedoKeys.Midday[0], c.GroundAlbedoKeys.Midday[1], c.GroundAlbedoKeys.Midday[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundAlbedoMidday must be (x, y, z)";
                return false;
            }
            c.GroundAlbedoKeys.Midday[0] = v.X;
            c.GroundAlbedoKeys.Midday[1] = v.Y;
            c.GroundAlbedoKeys.Midday[2] = v.Z;
        }
        else if (property == "groundalbedosunset")
        {
            Float3 v{c.GroundAlbedoKeys.Sunset[0], c.GroundAlbedoKeys.Sunset[1], c.GroundAlbedoKeys.Sunset[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundAlbedoSunset must be (x, y, z)";
                return false;
            }
            c.GroundAlbedoKeys.Sunset[0] = v.X;
            c.GroundAlbedoKeys.Sunset[1] = v.Y;
            c.GroundAlbedoKeys.Sunset[2] = v.Z;
        }
        else if (property == "groundbrightnesscurve")
        {
            ParseSkyCurve(value, c.GroundBrightnessKeys);
        }
        else if (property == "groundbrightnesscurvemode")
        {
            if (!TryParseSkyScalarCurveShapeMode(value, c.GroundBrightnessShapeMode))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundBrightnessCurveMode must be keyCurve or cubicBezier";
                return false;
            }
        }
        else if (property == "groundbrightnessbezier")
        {
            if (!ParseSkyScalarBezier(value, c.GroundBrightnessBezier))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundBrightnessBezier must be c1x,c1y,c2x,c2y,startY,endY";
                return false;
            }
        }
        else if (property == "groundnightcolor")
        {
            Float3 v{c.GroundNightColorKeys.Midnight[0], c.GroundNightColorKeys.Midnight[1], c.GroundNightColorKeys.Midnight[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundNightColor must be (x, y, z)";
                return false;
            }
            Components::SkyVec3DayKeysSetUniform(c.GroundNightColorKeys, v.X, v.Y, v.Z);
        }
        else if (property == "groundnightcolormidnight")
        {
            Float3 v{c.GroundNightColorKeys.Midnight[0], c.GroundNightColorKeys.Midnight[1], c.GroundNightColorKeys.Midnight[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundNightColorMidnight must be (x, y, z)";
                return false;
            }
            c.GroundNightColorKeys.Midnight[0] = v.X;
            c.GroundNightColorKeys.Midnight[1] = v.Y;
            c.GroundNightColorKeys.Midnight[2] = v.Z;
        }
        else if (property == "groundnightcolordawn")
        {
            Float3 v{c.GroundNightColorKeys.Dawn[0], c.GroundNightColorKeys.Dawn[1], c.GroundNightColorKeys.Dawn[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundNightColorDawn must be (x, y, z)";
                return false;
            }
            c.GroundNightColorKeys.Dawn[0] = v.X;
            c.GroundNightColorKeys.Dawn[1] = v.Y;
            c.GroundNightColorKeys.Dawn[2] = v.Z;
        }
        else if (property == "groundnightcolormidday")
        {
            Float3 v{c.GroundNightColorKeys.Midday[0], c.GroundNightColorKeys.Midday[1], c.GroundNightColorKeys.Midday[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundNightColorMidday must be (x, y, z)";
                return false;
            }
            c.GroundNightColorKeys.Midday[0] = v.X;
            c.GroundNightColorKeys.Midday[1] = v.Y;
            c.GroundNightColorKeys.Midday[2] = v.Z;
        }
        else if (property == "groundnightcolorsunset")
        {
            Float3 v{c.GroundNightColorKeys.Sunset[0], c.GroundNightColorKeys.Sunset[1], c.GroundNightColorKeys.Sunset[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundNightColorSunset must be (x, y, z)";
                return false;
            }
            c.GroundNightColorKeys.Sunset[0] = v.X;
            c.GroundNightColorKeys.Sunset[1] = v.Y;
            c.GroundNightColorKeys.Sunset[2] = v.Z;
        }
        else if (property == "belowhorizonblendsharpnesscurve")
        {
            ParseSkyCurve(value, c.BelowHorizonBlendSharpnessKeys);
        }
        else if (property == "belowhorizonblendsharpnesscurvemode")
        {
            if (!TryParseSkyScalarCurveShapeMode(value, c.BelowHorizonBlendSharpnessShapeMode))
            {
                if (outError)
                    *outError = "SkyEnvironment.BelowHorizonBlendSharpnessCurveMode must be keyCurve or cubicBezier";
                return false;
            }
        }
        else if (property == "belowhorizonblendsharpnessbezier")
        {
            if (!ParseSkyScalarBezier(value, c.BelowHorizonBlendSharpnessBezier))
            {
                if (outError)
                    *outError = "SkyEnvironment.BelowHorizonBlendSharpnessBezier must be c1x,c1y,c2x,c2y,startY,endY";
                return false;
            }
        }
        else if (property == "belowhorizondarknesscurve")
        {
            ParseSkyCurve(value, c.BelowHorizonDarknessKeys);
        }
        else if (property == "belowhorizondarknesscurvemode")
        {
            if (!TryParseSkyScalarCurveShapeMode(value, c.BelowHorizonDarknessShapeMode))
            {
                if (outError)
                    *outError = "SkyEnvironment.BelowHorizonDarknessCurveMode must be keyCurve or cubicBezier";
                return false;
            }
        }
        else if (property == "belowhorizondarknessbezier")
        {
            if (!ParseSkyScalarBezier(value, c.BelowHorizonDarknessBezier))
            {
                if (outError)
                    *outError = "SkyEnvironment.BelowHorizonDarknessBezier must be c1x,c1y,c2x,c2y,startY,endY";
                return false;
            }
        }
        else if (property == "belowhorizondarkcolor")
        {
            Float3 v{c.BelowHorizonDarkColorKeys.Midnight[0], c.BelowHorizonDarkColorKeys.Midnight[1], c.BelowHorizonDarkColorKeys.Midnight[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.BelowHorizonDarkColor must be (x, y, z)";
                return false;
            }
            Components::SkyVec3DayKeysSetUniform(c.BelowHorizonDarkColorKeys, v.X, v.Y, v.Z);
        }
        else if (property == "belowhorizondarkcolormidnight")
        {
            Float3 v{c.BelowHorizonDarkColorKeys.Midnight[0], c.BelowHorizonDarkColorKeys.Midnight[1], c.BelowHorizonDarkColorKeys.Midnight[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.BelowHorizonDarkColorMidnight must be (x, y, z)";
                return false;
            }
            c.BelowHorizonDarkColorKeys.Midnight[0] = v.X;
            c.BelowHorizonDarkColorKeys.Midnight[1] = v.Y;
            c.BelowHorizonDarkColorKeys.Midnight[2] = v.Z;
        }
        else if (property == "belowhorizondarkcolordawn")
        {
            Float3 v{c.BelowHorizonDarkColorKeys.Dawn[0], c.BelowHorizonDarkColorKeys.Dawn[1], c.BelowHorizonDarkColorKeys.Dawn[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.BelowHorizonDarkColorDawn must be (x, y, z)";
                return false;
            }
            c.BelowHorizonDarkColorKeys.Dawn[0] = v.X;
            c.BelowHorizonDarkColorKeys.Dawn[1] = v.Y;
            c.BelowHorizonDarkColorKeys.Dawn[2] = v.Z;
        }
        else if (property == "belowhorizondarkcolormidday")
        {
            Float3 v{c.BelowHorizonDarkColorKeys.Midday[0], c.BelowHorizonDarkColorKeys.Midday[1], c.BelowHorizonDarkColorKeys.Midday[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.BelowHorizonDarkColorMidday must be (x, y, z)";
                return false;
            }
            c.BelowHorizonDarkColorKeys.Midday[0] = v.X;
            c.BelowHorizonDarkColorKeys.Midday[1] = v.Y;
            c.BelowHorizonDarkColorKeys.Midday[2] = v.Z;
        }
        else if (property == "belowhorizondarkcolorsunset")
        {
            Float3 v{c.BelowHorizonDarkColorKeys.Sunset[0], c.BelowHorizonDarkColorKeys.Sunset[1], c.BelowHorizonDarkColorKeys.Sunset[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.BelowHorizonDarkColorSunset must be (x, y, z)";
                return false;
            }
            c.BelowHorizonDarkColorKeys.Sunset[0] = v.X;
            c.BelowHorizonDarkColorKeys.Sunset[1] = v.Y;
            c.BelowHorizonDarkColorKeys.Sunset[2] = v.Z;
        }
        else if (property == "groundhorizoncolor")
        {
            Float3 v{c.GroundHorizonColorKeys.Midnight[0], c.GroundHorizonColorKeys.Midnight[1], c.GroundHorizonColorKeys.Midnight[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundHorizonColor must be (x, y, z)";
                return false;
            }
            Components::SkyVec3DayKeysSetUniform(c.GroundHorizonColorKeys, v.X, v.Y, v.Z);
        }
        else if (property == "groundhorizoncolormidnight")
        {
            Float3 v{c.GroundHorizonColorKeys.Midnight[0], c.GroundHorizonColorKeys.Midnight[1], c.GroundHorizonColorKeys.Midnight[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundHorizonColorMidnight must be (x, y, z)";
                return false;
            }
            c.GroundHorizonColorKeys.Midnight[0] = v.X;
            c.GroundHorizonColorKeys.Midnight[1] = v.Y;
            c.GroundHorizonColorKeys.Midnight[2] = v.Z;
        }
        else if (property == "groundhorizoncolordawn")
        {
            Float3 v{c.GroundHorizonColorKeys.Dawn[0], c.GroundHorizonColorKeys.Dawn[1], c.GroundHorizonColorKeys.Dawn[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundHorizonColorDawn must be (x, y, z)";
                return false;
            }
            c.GroundHorizonColorKeys.Dawn[0] = v.X;
            c.GroundHorizonColorKeys.Dawn[1] = v.Y;
            c.GroundHorizonColorKeys.Dawn[2] = v.Z;
        }
        else if (property == "groundhorizoncolormidday")
        {
            Float3 v{c.GroundHorizonColorKeys.Midday[0], c.GroundHorizonColorKeys.Midday[1], c.GroundHorizonColorKeys.Midday[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundHorizonColorMidday must be (x, y, z)";
                return false;
            }
            c.GroundHorizonColorKeys.Midday[0] = v.X;
            c.GroundHorizonColorKeys.Midday[1] = v.Y;
            c.GroundHorizonColorKeys.Midday[2] = v.Z;
        }
        else if (property == "groundhorizoncolorsunset")
        {
            Float3 v{c.GroundHorizonColorKeys.Sunset[0], c.GroundHorizonColorKeys.Sunset[1], c.GroundHorizonColorKeys.Sunset[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundHorizonColorSunset must be (x, y, z)";
                return false;
            }
            c.GroundHorizonColorKeys.Sunset[0] = v.X;
            c.GroundHorizonColorKeys.Sunset[1] = v.Y;
            c.GroundHorizonColorKeys.Sunset[2] = v.Z;
        }
        else if (property == "groundhorizonnightcolor")
        {
            Float3 v{c.GroundHorizonNightColorKeys.Midnight[0], c.GroundHorizonNightColorKeys.Midnight[1], c.GroundHorizonNightColorKeys.Midnight[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundHorizonNightColor must be (x, y, z)";
                return false;
            }
            Components::SkyVec3DayKeysSetUniform(c.GroundHorizonNightColorKeys, v.X, v.Y, v.Z);
        }
        else if (property == "groundhorizonnightcolormidnight")
        {
            Float3 v{c.GroundHorizonNightColorKeys.Midnight[0], c.GroundHorizonNightColorKeys.Midnight[1], c.GroundHorizonNightColorKeys.Midnight[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundHorizonNightColorMidnight must be (x, y, z)";
                return false;
            }
            c.GroundHorizonNightColorKeys.Midnight[0] = v.X;
            c.GroundHorizonNightColorKeys.Midnight[1] = v.Y;
            c.GroundHorizonNightColorKeys.Midnight[2] = v.Z;
        }
        else if (property == "groundhorizonnightcolordawn")
        {
            Float3 v{c.GroundHorizonNightColorKeys.Dawn[0], c.GroundHorizonNightColorKeys.Dawn[1], c.GroundHorizonNightColorKeys.Dawn[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundHorizonNightColorDawn must be (x, y, z)";
                return false;
            }
            c.GroundHorizonNightColorKeys.Dawn[0] = v.X;
            c.GroundHorizonNightColorKeys.Dawn[1] = v.Y;
            c.GroundHorizonNightColorKeys.Dawn[2] = v.Z;
        }
        else if (property == "groundhorizonnightcolormidday")
        {
            Float3 v{c.GroundHorizonNightColorKeys.Midday[0], c.GroundHorizonNightColorKeys.Midday[1], c.GroundHorizonNightColorKeys.Midday[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundHorizonNightColorMidday must be (x, y, z)";
                return false;
            }
            c.GroundHorizonNightColorKeys.Midday[0] = v.X;
            c.GroundHorizonNightColorKeys.Midday[1] = v.Y;
            c.GroundHorizonNightColorKeys.Midday[2] = v.Z;
        }
        else if (property == "groundhorizonnightcolorsunset")
        {
            Float3 v{c.GroundHorizonNightColorKeys.Sunset[0], c.GroundHorizonNightColorKeys.Sunset[1], c.GroundHorizonNightColorKeys.Sunset[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundHorizonNightColorSunset must be (x, y, z)";
                return false;
            }
            c.GroundHorizonNightColorKeys.Sunset[0] = v.X;
            c.GroundHorizonNightColorKeys.Sunset[1] = v.Y;
            c.GroundHorizonNightColorKeys.Sunset[2] = v.Z;
        }
        else if (property == "nightskyhorizoncolor")
        {
            Float3 v{c.NightSkyHorizonColorKeys.Midnight[0], c.NightSkyHorizonColorKeys.Midnight[1], c.NightSkyHorizonColorKeys.Midnight[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.NightSkyHorizonColor must be (x, y, z)";
                return false;
            }
            Components::SkyVec3DayKeysSetUniform(c.NightSkyHorizonColorKeys, v.X, v.Y, v.Z);
        }
        else if (property == "nightskyhorizoncolormidnight")
        {
            Float3 v{c.NightSkyHorizonColorKeys.Midnight[0], c.NightSkyHorizonColorKeys.Midnight[1], c.NightSkyHorizonColorKeys.Midnight[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.NightSkyHorizonColorMidnight must be (x, y, z)";
                return false;
            }
            c.NightSkyHorizonColorKeys.Midnight[0] = v.X;
            c.NightSkyHorizonColorKeys.Midnight[1] = v.Y;
            c.NightSkyHorizonColorKeys.Midnight[2] = v.Z;
        }
        else if (property == "nightskyhorizoncolordawn")
        {
            Float3 v{c.NightSkyHorizonColorKeys.Dawn[0], c.NightSkyHorizonColorKeys.Dawn[1], c.NightSkyHorizonColorKeys.Dawn[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.NightSkyHorizonColorDawn must be (x, y, z)";
                return false;
            }
            c.NightSkyHorizonColorKeys.Dawn[0] = v.X;
            c.NightSkyHorizonColorKeys.Dawn[1] = v.Y;
            c.NightSkyHorizonColorKeys.Dawn[2] = v.Z;
        }
        else if (property == "nightskyhorizoncolormidday")
        {
            Float3 v{c.NightSkyHorizonColorKeys.Midday[0], c.NightSkyHorizonColorKeys.Midday[1], c.NightSkyHorizonColorKeys.Midday[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.NightSkyHorizonColorMidday must be (x, y, z)";
                return false;
            }
            c.NightSkyHorizonColorKeys.Midday[0] = v.X;
            c.NightSkyHorizonColorKeys.Midday[1] = v.Y;
            c.NightSkyHorizonColorKeys.Midday[2] = v.Z;
        }
        else if (property == "nightskyhorizoncolorsunset")
        {
            Float3 v{c.NightSkyHorizonColorKeys.Sunset[0], c.NightSkyHorizonColorKeys.Sunset[1], c.NightSkyHorizonColorKeys.Sunset[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "SkyEnvironment.NightSkyHorizonColorSunset must be (x, y, z)";
                return false;
            }
            c.NightSkyHorizonColorKeys.Sunset[0] = v.X;
            c.NightSkyHorizonColorKeys.Sunset[1] = v.Y;
            c.NightSkyHorizonColorKeys.Sunset[2] = v.Z;
        }
        else if (property == "groundhorizoncoswidthcurve")
        {
            ParseSkyCurve(value, c.GroundHorizonCosWidthKeys);
        }
        else if (property == "groundhorizoncoswidthcurvemode")
        {
            if (!TryParseSkyScalarCurveShapeMode(value, c.GroundHorizonCosWidthShapeMode))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundHorizonCosWidthCurveMode must be keyCurve or cubicBezier";
                return false;
            }
        }
        else if (property == "groundhorizoncoswidthbezier")
        {
            if (!ParseSkyScalarBezier(value, c.GroundHorizonCosWidthBezier))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundHorizonCosWidthBezier must be c1x,c1y,c2x,c2y,startY,endY";
                return false;
            }
        }
        else if (property == "groundhorizonnightcoswidthcurve")
        {
            ParseSkyCurve(value, c.GroundHorizonNightCosWidthKeys);
        }
        else if (property == "groundhorizonnightcoswidthcurvemode")
        {
            if (!TryParseSkyScalarCurveShapeMode(value, c.GroundHorizonNightCosWidthShapeMode))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundHorizonNightCosWidthCurveMode must be keyCurve or cubicBezier";
                return false;
            }
        }
        else if (property == "groundhorizonnightcoswidthbezier")
        {
            if (!ParseSkyScalarBezier(value, c.GroundHorizonNightCosWidthBezier))
            {
                if (outError)
                    *outError = "SkyEnvironment.GroundHorizonNightCosWidthBezier must be c1x,c1y,c2x,c2y,startY,endY";
                return false;
            }
        }
        else if (property == "stardensity")
        {
            if (!ParseF32(value, c.StarDensity, outError))
                return false;
        }
        else if (property == "starbrightness")
        {
            if (!ParseF32(value, c.StarBrightness, outError))
                return false;
        }
        else if (property == "starsize")
        {
            if (!ParseF32(value, c.StarSize, outError))
                return false;
        }
        else if (property == "stardiamondshape")
        {
            if (!ParseF32(value, c.StarDiamondShape, outError))
                return false;
        }
        else if (property == "starcoresize")
        {
            if (!ParseF32(value, c.StarCoreSize, outError))
                return false;
        }
        else if (property == "starglowfalloff")
        {
            if (!ParseF32(value, c.StarGlowFalloff, outError))
                return false;
        }
        else if (property == "twinklespeed")
        {
            if (!ParseF32(value, c.TwinkleSpeed, outError))
                return false;
        }
        else if (property == "twinkleintensity")
        {
            if (!ParseF32(value, c.TwinkleIntensity, outError))
                return false;
        }
        else if (const RetiredSkyProperty* retired = FindRetiredSkyProperty(property))
        {
            // A retired knob, named and accounted for rather than left to the silent skip below.
            // A malformed value still fails like any other.
            if (property == "usesunlightcolor")
            {
                // The one retired property that is not a colour. Only `true` changed anything, so
                // only `true` is worth a notice; warning every project whose scene carries the
                // default `false` would be noise about a no-op.
                bool wasOn = false;
                if (!ParseBool(value, wasOn, outError))
                    return false;
                if (wasOn)
                    WarnRetiredSkyProperty(*retired, RetiredSkyOutcome::ToggleRetired, Float3{});
            }
            else
            {
                Float3 v{};
                if (!ParseFloat3(value, v))
                {
                    if (outError)
                        *outError = std::string("SkyEnvironment.") +
                                    std::string(retired->Authored) + " must be (x, y, z)";
                    return false;
                }

                const bool wasDefault = MatchesRetiredDefault(v, retired->RetiredDefault);
                if (!retired->ConvertsToTint)
                {
                    // A night key: its value was a level, not a multiplier. Drop it, and say so
                    // with the number in hand — but only if it was authored, since the default
                    // carries no intent to report.
                    if (!wasDefault)
                        WarnRetiredSkyProperty(*retired, RetiredSkyOutcome::DroppedNightLevel, v);
                    else
                        WarnRetiredSkyProperty(*retired, RetiredSkyOutcome::ConvertedToWhite, v);
                }
                else
                {
                    const Float3 tint = wasDefault ? Float3{1.0f, 1.0f, 1.0f} : v;
                    if (retired->Target != nullptr)
                    {
                        float32(&key)[3] = c.SunTintKeys.*(retired->Target);
                        key[0] = tint.X;
                        key[1] = tint.Y;
                        key[2] = tint.Z;
                    }
                    else
                    {
                        // The uniform setter wrote all four keys; only the day keys convert.
                        c.SunTintKeys.Midday[0] = tint.X;
                        c.SunTintKeys.Midday[1] = tint.Y;
                        c.SunTintKeys.Midday[2] = tint.Z;
                        c.SunTintKeys.Sunset[0] = tint.X;
                        c.SunTintKeys.Sunset[1] = tint.Y;
                        c.SunTintKeys.Sunset[2] = tint.Z;
                    }
                    WarnRetiredSkyProperty(*retired,
                                           wasDefault ? RetiredSkyOutcome::ConvertedToWhite
                                                      : RetiredSkyOutcome::CarriedToTint,
                                           v);
                }
            }
            // Deliberately NO early return: `c` is a local copy and the write-back that commits it
            // is at the end of this function. Returning here would warn, convert, and then discard.
        }
        else
        {
            // Tolerate an unrecognized property NAME (an older/renamed/removed field, or one a newer
            // editor added) per the ISceneComponentSchema contract: skip it and keep loading rather
            // than aborting the entire scene. A malformed VALUE still fails in the branch that parses
            // it; only an unknown name reaches here. This retires the per-key legacy-ignore blocks.
            return true;
        }

        // Keep day-key times ordered and inside a safe [0, 24) range.
        constexpr float kMinGapHours = 0.1f;
        c.DayKeyTimesHours.Midnight = std::clamp(c.DayKeyTimesHours.Midnight, 0.0f, 23.9f);
        c.DayKeyTimesHours.Dawn = std::clamp(c.DayKeyTimesHours.Dawn, c.DayKeyTimesHours.Midnight + kMinGapHours, 23.9f);
        c.DayKeyTimesHours.Midday = std::clamp(c.DayKeyTimesHours.Midday, c.DayKeyTimesHours.Dawn + kMinGapHours, 23.9f);
        c.DayKeyTimesHours.Sunset = std::clamp(c.DayKeyTimesHours.Sunset, c.DayKeyTimesHours.Midday + kMinGapHours, 23.999f);

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::SkyEnvironment>(entity))
            return true;
        Components::SkyEnvironment c{};
        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::SkyEnvironment>(entity);
        return true;
    }
};

// Opt-in additive ambient irradiance floor. Colors are authored linear RGB (0..1); Intensity is
// nits on the 203-nit reference-white anchor (see AmbientLight.h). Mirrors the SkyEnvironment
// ambient-tint (de)serialization patterns.
class AmbientLightSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "AmbientLight"; }

    void Serialize(const ECS::World& world,
                   ECS::EntityHandle entity,
                   const SceneSaveContext& /*ctx*/,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::AmbientLight>(entity);
        if (!c)
            return;
        outLines.push_back(std::string("AmbientLight.Mode = ") + std::to_string(static_cast<uint32_t>(c->Mode)));
        outLines.push_back(std::string("AmbientLight.Color = ") + FormatFloat3(c->Color[0], c->Color[1], c->Color[2]));
        outLines.push_back(std::string("AmbientLight.SkyColor = ") + FormatFloat3(c->SkyColor[0], c->SkyColor[1], c->SkyColor[2]));
        outLines.push_back(std::string("AmbientLight.EquatorColor = ") + FormatFloat3(c->EquatorColor[0], c->EquatorColor[1], c->EquatorColor[2]));
        outLines.push_back(std::string("AmbientLight.GroundColor = ") + FormatFloat3(c->GroundColor[0], c->GroundColor[1], c->GroundColor[2]));
        outLines.push_back(std::string("AmbientLight.Intensity = ") + FormatFloat(c->Intensity));
        outLines.push_back(std::string("AmbientLight.AffectSpecular = ") + (c->AffectSpecular ? "true" : "false"));
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        Components::AmbientLight c{};
        if (auto* existing = world.GetComponent<Components::AmbientLight>(entity))
            c = *existing;

        if (property == "mode")
        {
            uint32_t v = 0;
            if (!ParseU32(value, v, outError))
                return false;
            if (v > 1)
            {
                if (outError)
                    *outError = "AmbientLight.Mode must be 0-1";
                return false;
            }
            c.Mode = static_cast<Components::AmbientLightMode>(v);
        }
        else if (property == "color")
        {
            Float3 v{c.Color[0], c.Color[1], c.Color[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "AmbientLight.Color must be (x, y, z)";
                return false;
            }
            c.Color[0] = v.X;
            c.Color[1] = v.Y;
            c.Color[2] = v.Z;
        }
        else if (property == "skycolor")
        {
            Float3 v{c.SkyColor[0], c.SkyColor[1], c.SkyColor[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "AmbientLight.SkyColor must be (x, y, z)";
                return false;
            }
            c.SkyColor[0] = v.X;
            c.SkyColor[1] = v.Y;
            c.SkyColor[2] = v.Z;
        }
        else if (property == "equatorcolor")
        {
            Float3 v{c.EquatorColor[0], c.EquatorColor[1], c.EquatorColor[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "AmbientLight.EquatorColor must be (x, y, z)";
                return false;
            }
            c.EquatorColor[0] = v.X;
            c.EquatorColor[1] = v.Y;
            c.EquatorColor[2] = v.Z;
        }
        else if (property == "groundcolor")
        {
            Float3 v{c.GroundColor[0], c.GroundColor[1], c.GroundColor[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "AmbientLight.GroundColor must be (x, y, z)";
                return false;
            }
            c.GroundColor[0] = v.X;
            c.GroundColor[1] = v.Y;
            c.GroundColor[2] = v.Z;
        }
        else if (property == "intensity")
        {
            if (!ParseF32(value, c.Intensity, outError))
                return false;
        }
        else if (property == "affectspecular")
        {
            if (!ParseBool(value, c.AffectSpecular, outError))
                return false;
        }
        else
        {
            // Tolerate an unrecognized property name (older/renamed/newer field): skip and keep
            // loading rather than aborting the scene. A malformed VALUE still fails in its branch.
            return true;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::AmbientLight>(entity))
            return true;
        Components::AmbientLight c{};
        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::AmbientLight>(entity);
        return true;
    }
};

class SkyboxSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "Skybox"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::Skybox>(entity);
        if (!c)
            return;
        outLines.push_back(std::string("Skybox.HDRIIntensity = ") + FormatFloat(c->HDRIIntensity));
        outLines.push_back(std::string("Skybox.IblIntensity = ") + FormatFloat(c->IblIntensity));
        outLines.push_back(std::string("Skybox.IblLowerHemisphereDarkness = ") + FormatFloat(c->IblLowerHemisphereDarkness));
        outLines.push_back(std::string("Skybox.RotationDegrees = ") + FormatFloat(c->RotationDegrees));
        if (c->Resolution[0] != '\0')
            outLines.push_back(std::string("Skybox.Resolution = ") + FormatQuoted(c->GetResolution()));
        if (c->SourceSlug[0] != '\0')
            outLines.push_back(std::string("Skybox.SourceSlug = ") + FormatQuoted(c->GetSourceSlug()));
        // Single canonical AssetRef line. The save context's resolver heals
        // stale/missing components — see FormatAssetReferenceForSave docs.
        // Legacy split form (HDRIAssetGuid + HDRIPath) is still accepted on
        // load, but new saves emit only the consolidated property.
        const bool hasGuid = c->HDRIAssetGuid[0] != '\0';
        const bool hasPath = c->HDRIPath[0] != '\0';
        if (hasGuid || hasPath)
        {
            GUID g = GUID::Null();
            if (hasGuid)
            {
                try
                {
                    g = GUID(std::string(c->GetHDRIAssetGuid()));
                }
                catch (...)
                {
                }
            }
            outLines.push_back(std::string("Skybox.HDRI = ") +
                               FormatAssetReferenceForSave(ctx, g, c->GetHDRIPath()));
        }
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        Components::Skybox c{};
        if (auto* existing = world.GetComponent<Components::Skybox>(entity))
            c = *existing;

        if (property == "hdriintensity")
        {
            if (!ParseF32(value, c.HDRIIntensity, outError))
                return false;
            c.HDRIIntensity = std::max(0.0f, c.HDRIIntensity);
        }
        else if (property == "iblintensity")
        {
            if (!ParseF32(value, c.IblIntensity, outError))
                return false;
            c.IblIntensity = std::max(0.0f, c.IblIntensity);
        }
        else if (property == "ibllowerhemispheredarkness")
        {
            if (!ParseF32(value, c.IblLowerHemisphereDarkness, outError))
                return false;
            c.IblLowerHemisphereDarkness = std::clamp(c.IblLowerHemisphereDarkness, 0.0f, 1.0f);
        }
        else if (property == "rotationdegrees")
        {
            if (!ParseF32(value, c.RotationDegrees, outError))
                return false;
        }
        else if (property == "resolution")
        {
            std::string s;
            if (!ParseQuotedString(value, s))
            {
                if (outError)
                    *outError = "Skybox.Resolution must be a quoted string";
                return false;
            }
            std::memset(c.Resolution, 0, sizeof(c.Resolution));
            std::memcpy(c.Resolution, s.data(), std::min(sizeof(c.Resolution) - 1, s.size()));
        }
        else if (property == "sourceslug")
        {
            std::string s;
            if (!ParseQuotedString(value, s))
            {
                if (outError)
                    *outError = "Skybox.SourceSlug must be a quoted string";
                return false;
            }
            std::memset(c.SourceSlug, 0, sizeof(c.SourceSlug));
            std::memcpy(c.SourceSlug, s.data(), std::min(sizeof(c.SourceSlug) - 1, s.size()));
        }
        else if (property == "hdriassetguid")
        {
            // Legacy split form — accept for back-compat with scenes saved
            // before HDRI consolidation. New saves emit `Skybox.HDRI` instead.
            std::string s;
            if (!ParseQuotedString(value, s))
            {
                if (outError)
                    *outError = "Skybox.HDRIAssetGuid must be a quoted string";
                return false;
            }
            std::memset(c.HDRIAssetGuid, 0, sizeof(c.HDRIAssetGuid));
            std::memcpy(c.HDRIAssetGuid, s.data(), std::min(sizeof(c.HDRIAssetGuid) - 1, s.size()));
        }
        else if (property == "hdripath")
        {
            // Legacy split form — see hdriassetguid above.
            std::string s;
            if (!ParseQuotedString(value, s))
            {
                if (outError)
                    *outError = "Skybox.HDRIPath must be a quoted string";
                return false;
            }
            std::memset(c.HDRIPath, 0, sizeof(c.HDRIPath));
            std::memcpy(c.HDRIPath, s.data(), std::min(sizeof(c.HDRIPath) - 1, s.size()));
        }
        else if (property == "hdri")
        {
            // Canonical form: Skybox.HDRI = [path="..." guid="..."]
            // Falls back to legacy bare-GUID strings via TryResolveAssetReference.
            SceneValue sv{};
            if (!ParseValue(value, sv, outError))
                return false;
            if (sv.Kind == SceneValueKind::Int && sv.IntValue == 0)
            {
                std::memset(c.HDRIAssetGuid, 0, sizeof(c.HDRIAssetGuid));
                std::memset(c.HDRIPath, 0, sizeof(c.HDRIPath));
            }
            else
            {
                AssetReference ref{};
                std::string err;
                if (!TryResolveAssetReference(ctx, sv, AssetType::Texture, ref, &err))
                {
                    if (outError)
                        *outError = err.empty() ? "Skybox.HDRI: invalid reference" : err;
                    return false;
                }
                std::memset(c.HDRIAssetGuid, 0, sizeof(c.HDRIAssetGuid));
                if (!ref.guid.IsNull())
                {
                    const std::string gs = ref.guid.ToString();
                    const size_t n = std::min(gs.size(), sizeof(c.HDRIAssetGuid) - 1);
                    std::memcpy(c.HDRIAssetGuid, gs.data(), n);
                }
                std::memset(c.HDRIPath, 0, sizeof(c.HDRIPath));
                if (!ref.path.empty())
                {
                    const size_t n = std::min(ref.path.size(), sizeof(c.HDRIPath) - 1);
                    std::memcpy(c.HDRIPath, ref.path.data(), n);
                }
            }
        }
        else
        {
            if (outError)
                *outError = "Unknown Skybox property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::Skybox>(entity))
            return true;
        Components::Skybox c{};
        world.AddComponentImmediate(entity, c);
        return true;
    }

    void EnumerateAssetReferences(const ECS::World& world,
                                  ECS::EntityHandle entity,
                                  const AssetRefVisitor& visitor) const override
    {
        const auto* c = world.GetComponent<Components::Skybox>(entity);
        if (!c || !visitor || c->HDRIAssetGuid[0] == '\0')
            return;
        GUID g = GUID::Null();
        try
        {
            g = GUID(std::string(c->GetHDRIAssetGuid()));
        }
        catch (...)
        {
        }
        if (!g.IsNull())
        {
            const std::string_view path = c->GetHDRIPath();
            visitor(g, AssetType::Texture, path, "hdri");
        }
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::Skybox>(entity);
        return true;
    }
};

class CameraSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "Camera"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::Camera>(entity);
        if (!c)
            return;
        outLines.push_back(std::string("Camera.perspective = ") + (c->Perspective ? "true" : "false"));
        outLines.push_back(std::string("Camera.fovY = ") + FormatFloat(c->FovY));
        outLines.push_back(std::string("Camera.orthographicSize = ") + FormatFloat(c->OrthographicSize));
        outLines.push_back(std::string("Camera.nearZ = ") + FormatFloat(c->NearZ));
        outLines.push_back(std::string("Camera.farZ = ") + FormatFloat(c->FarZ));
        outLines.push_back(std::string("Camera.cullingMask = ") + std::to_string(c->CullingMask));
        outLines.push_back(std::string("Camera.postProcessProfileId = ") + std::to_string(c->PostProcessProfileId));
        outLines.push_back(std::string("Camera.msaaSamples = ") + std::to_string(c->MSAASamples));
        outLines.push_back(std::string("Camera.antiAliasing = ") + std::to_string(c->AntiAliasing));
        outLines.push_back(std::string("Camera.renderScale = ") + FormatFloat(c->RenderScale));
        outLines.push_back(std::string("Camera.postProcessMask = ") + std::to_string(c->PostProcessMask));
        outLines.push_back(std::string("Camera.aspectPreset = ") + std::to_string(c->AspectPreset));
        outLines.push_back(std::string("Camera.customAspectWidth = ") + FormatFloat(c->CustomAspectWidth));
        outLines.push_back(std::string("Camera.customAspectHeight = ") + FormatFloat(c->CustomAspectHeight));
        outLines.push_back(std::string("Camera.pixelPerfect = ") + (c->PixelPerfect ? "true" : "false"));
        outLines.push_back(std::string("Camera.pixelPerfectPixelsPerUnit = ") + std::to_string(c->PixelPerfectPixelsPerUnit));
        outLines.push_back(std::string("Camera.pixelPerfectReferenceWidth = ") + std::to_string(c->PixelPerfectReferenceWidth));
        outLines.push_back(std::string("Camera.pixelPerfectReferenceHeight = ") + std::to_string(c->PixelPerfectReferenceHeight));
        outLines.push_back(std::string("Camera.pixelPerfectPixelSnap = ") + (c->PixelPerfectPixelSnap ? "true" : "false"));
        outLines.push_back(std::string("Camera.pixelPerfectExpand = ") + (c->PixelPerfectExpand ? "true" : "false"));
        outLines.push_back(std::string("Camera.exposureControl = ") + std::to_string(static_cast<int>(c->ExposureControl)));
        outLines.push_back(std::string("Camera.exposure = ") + FormatFloat(c->Exposure));
        outLines.push_back(std::string("Camera.manualExposureEV = ") + FormatFloat(c->ManualExposureEV));
        outLines.push_back(std::string("Camera.exposureCompensation = ") + FormatFloat(c->ExposureCompensation));
        outLines.push_back(std::string("Camera.aperture = ") + FormatFloat(c->Aperture));
        outLines.push_back(std::string("Camera.focusDistance = ") + FormatFloat(c->FocusDistance));
        outLines.push_back(std::string("Camera.focusDebugMode = ") + std::to_string(c->FocusDebugMode));
        outLines.push_back(std::string("Camera.focusDebugAlpha = ") + FormatFloat(c->FocusDebugAlpha));
        outLines.push_back(std::string("Camera.apertureBladeCount = ") + std::to_string(c->ApertureBladeCount));
        outLines.push_back(std::string("Camera.apertureRoundness = ") + FormatFloat(c->ApertureRoundness));
        outLines.push_back(std::string("Camera.apertureRotation = ") + FormatFloat(c->ApertureRotation));
        outLines.push_back(std::string("Camera.anamorphicSqueeze = ") + FormatFloat(c->AnamorphicSqueeze));
        outLines.push_back(std::string("Camera.sensorPreset = ") + std::to_string(static_cast<uint32>(c->SensorPreset)));
        outLines.push_back(std::string("Camera.sensorHeightMm = ") + FormatFloat(c->SensorHeightMm));
        outLines.push_back(std::string("Camera.shutterTime = ") + FormatFloat(c->ShutterTime));
        outLines.push_back(std::string("Camera.iso = ") + FormatFloat(c->Iso));
        outLines.push_back(std::string("Camera.autoExposureMinEv = ") + FormatFloat(c->AutoExposureMinEv));
        outLines.push_back(std::string("Camera.autoExposureMaxEv = ") + FormatFloat(c->AutoExposureMaxEv));
        outLines.push_back(std::string("Camera.autoExposureSpeedUp = ") + FormatFloat(c->AutoExposureSpeedUp));
        outLines.push_back(std::string("Camera.autoExposureSpeedDown = ") + FormatFloat(c->AutoExposureSpeedDown));
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        Components::Camera c{};
        if (auto* existing = world.GetComponent<Components::Camera>(entity))
            c = *existing;

        if (property == "perspective")
        {
            if (!ParseBool(value, c.Perspective, outError))
                return false;
        }
        else if (property == "fovy")
        {
            if (!ParseF32(value, c.FovY, outError))
                return false;
        }
        else if (property == "orthographicsize")
        {
            if (!ParseF32(value, c.OrthographicSize, outError))
                return false;
        }
        else if (property == "nearz")
        {
            if (!ParseF32(value, c.NearZ, outError))
                return false;
        }
        else if (property == "farz")
        {
            if (!ParseF32(value, c.FarZ, outError))
                return false;
        }
        else if (property == "cullingmask")
        {
            if (!ParseU32(value, c.CullingMask, outError))
                return false;
        }
        else if (property == "postprocessprofileid")
        {
            if (!ParseU32(value, c.PostProcessProfileId, outError))
                return false;
        }
        else if (property == "msaasamples")
        {
            if (!ParseU32(value, c.MSAASamples, outError))
                return false;
        }
        else if (property == "antialiasing")
        {
            if (!ParseU32(value, c.AntiAliasing, outError))
                return false;
        }
        else if (property == "renderscale")
        {
            if (!ParseF32(value, c.RenderScale, outError))
                return false;
        }
        else if (property == "postprocessmask")
        {
            if (!ParseU32(value, c.PostProcessMask, outError))
                return false;
        }
        else if (property == "aspectpreset")
        {
            if (!ParseU32(value, c.AspectPreset, outError))
                return false;
        }
        else if (property == "customaspectwidth")
        {
            if (!ParseF32(value, c.CustomAspectWidth, outError))
                return false;
        }
        else if (property == "customaspectheight")
        {
            if (!ParseF32(value, c.CustomAspectHeight, outError))
                return false;
        }
        else if (property == "pixelperfect")
        {
            if (!ParseBool(value, c.PixelPerfect, outError))
                return false;
        }
        else if (property == "pixelperfectpixelsperunit")
        {
            if (!ParseU32(value, c.PixelPerfectPixelsPerUnit, outError))
                return false;
        }
        else if (property == "pixelperfectreferencewidth")
        {
            if (!ParseU32(value, c.PixelPerfectReferenceWidth, outError))
                return false;
        }
        else if (property == "pixelperfectreferenceheight")
        {
            if (!ParseU32(value, c.PixelPerfectReferenceHeight, outError))
                return false;
        }
        else if (property == "pixelperfectpixelsnap")
        {
            if (!ParseBool(value, c.PixelPerfectPixelSnap, outError))
                return false;
        }
        else if (property == "pixelperfectexpand")
        {
            if (!ParseBool(value, c.PixelPerfectExpand, outError))
                return false;
        }
        else if (property == "exposurecontrol")
        {
            uint32 m = 0;
            if (!ParseU32(value, m, outError))
                return false;
            c.ExposureControl = static_cast<Components::ExposureMode>(m > 3u ? 3u : m);
        }
        else if (property == "exposure")
        {
            if (!ParseF32(value, c.Exposure, outError))
                return false;
        }
        else if (property == "manualexposureev")
        {
            if (!ParseF32(value, c.ManualExposureEV, outError))
                return false;
        }
        else if (property == "exposurecompensation")
        {
            if (!ParseF32(value, c.ExposureCompensation, outError))
                return false;
        }
        else if (property == "aperture")
        {
            if (!ParseF32(value, c.Aperture, outError))
                return false;
            c.Aperture = std::max(c.Aperture, Components::Camera::kApertureMin);
        }
        else if (property == "focusdistance")
        {
            if (!ParseF32(value, c.FocusDistance, outError))
                return false;
            c.FocusDistance = std::max(c.FocusDistance, Components::Camera::kFocusDistanceMin);
        }
        else if (property == "focusdebugmode")
        {
            if (!ParseI32(value, c.FocusDebugMode, outError))
                return false;
            c.FocusDebugMode = c.FocusDebugMode != 0 ? 1 : 0;
        }
        else if (property == "focusdebugalpha")
        {
            if (!ParseF32(value, c.FocusDebugAlpha, outError))
                return false;
            c.FocusDebugAlpha = std::clamp(c.FocusDebugAlpha, 0.0f, 1.0f);
        }
        else if (property == "aperturebladecount")
        {
            if (!ParseU32(value, c.ApertureBladeCount, outError))
                return false;
            c.ApertureBladeCount = std::clamp(c.ApertureBladeCount,
                Components::Camera::kApertureBladeCountMin,
                Components::Camera::kApertureBladeCountMax);
        }
        else if (property == "apertureroundness")
        {
            if (!ParseF32(value, c.ApertureRoundness, outError))
                return false;
            c.ApertureRoundness = std::clamp(c.ApertureRoundness, 0.0f, 1.0f);
        }
        else if (property == "aperturerotation")
        {
            if (!ParseF32(value, c.ApertureRotation, outError))
                return false;
            c.ApertureRotation = std::clamp(c.ApertureRotation, 0.0f, 360.0f);
        }
        else if (property == "anamorphicsqueeze")
        {
            if (!ParseF32(value, c.AnamorphicSqueeze, outError))
                return false;
            c.AnamorphicSqueeze = std::clamp(c.AnamorphicSqueeze, 1.0f, 4.0f);
        }
        else if (property == "sensorpreset")
        {
            uint32 preset = 0;
            if (!ParseU32(value, preset, outError))
                return false;
            c.SensorPreset = static_cast<Components::CameraSensorPreset>(
                std::min(preset, static_cast<uint32>(Components::CameraSensorPreset::ArriAlexa65)));
            if (c.SensorPreset != Components::CameraSensorPreset::Custom)
                c.SensorHeightMm = Components::CameraSensorPresetHeightMm(c.SensorPreset);
        }
        else if (property == "sensorheightmm")
        {
            if (!ParseF32(value, c.SensorHeightMm, outError))
                return false;
            c.SensorHeightMm = std::clamp(c.SensorHeightMm, 1.0f, 100.0f);
        }
        else if (property == "shuttertime")
        {
            if (!ParseF32(value, c.ShutterTime, outError))
                return false;
        }
        else if (property == "iso")
        {
            if (!ParseF32(value, c.Iso, outError))
                return false;
        }
        else if (property == "autoexposureminev")
        {
            if (!ParseF32(value, c.AutoExposureMinEv, outError))
                return false;
        }
        else if (property == "autoexposuremaxev")
        {
            if (!ParseF32(value, c.AutoExposureMaxEv, outError))
                return false;
        }
        else if (property == "autoexposurespeedup")
        {
            if (!ParseF32(value, c.AutoExposureSpeedUp, outError))
                return false;
        }
        else if (property == "autoexposurespeeddown")
        {
            if (!ParseF32(value, c.AutoExposureSpeedDown, outError))
                return false;
        }
        else
        {
            if (outError)
                *outError = "Unknown Camera property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::Camera>(entity))
            return true;
        Components::Camera c{};
        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::Camera>(entity);
        return true;
    }
};

// LightUnit <-> stable text name. Serialized by name (not the raw enum int) so the physical-unit
// choice stays readable in a .scene and survives any future enum reordering; unknown names fail
// loudly. Omitted on load -> the struct default (Unitless), so pre-physical-units scenes are
// unaffected and the anchor flip stays inert for them.
static const char* LightUnitToName(Components::LightUnit u)
{
    switch (u)
    {
    case Components::LightUnit::Lux:     return "Lux";
    case Components::LightUnit::Lumen:   return "Lumen";
    case Components::LightUnit::Candela: return "Candela";
    case Components::LightUnit::Unitless:
    default:                             return "Unitless";
    }
}

static bool ParseLightUnit(std::string_view value, Components::LightUnit& out, std::string* outError)
{
    const std::string v = ToLowerAscii(value);
    if (v == "unitless") out = Components::LightUnit::Unitless;
    else if (v == "lux") out = Components::LightUnit::Lux;
    else if (v == "lumen") out = Components::LightUnit::Lumen;
    else if (v == "candela") out = Components::LightUnit::Candela;
    else
    {
        if (outError)
            *outError = "Light.intensityUnit must be Unitless/Lux/Lumen/Candela";
        return false;
    }
    return true;
}

class LightSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "Light"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::Light>(entity);
        if (!c)
            return;
        outLines.push_back(std::string("Light.type = ") + std::to_string(static_cast<uint32_t>(c->Type)));
        outLines.push_back(std::string("Light.color = ") + FormatFloat3(c->Color[0], c->Color[1], c->Color[2]));
        outLines.push_back(std::string("Light.intensity = ") + FormatFloat(c->Intensity));
        outLines.push_back(std::string("Light.range = ") + FormatFloat(c->Range));
        outLines.push_back(std::string("Light.innerAngle = ") + FormatFloat(c->InnerAngle));
        outLines.push_back(std::string("Light.outerAngle = ") + FormatFloat(c->OuterAngle));
        outLines.push_back(std::string("Light.areaShape = ") + std::to_string(static_cast<uint32_t>(c->AreaShape)));
        outLines.push_back(std::string("Light.areaWidth = ") + FormatFloat(c->AreaWidth));
        outLines.push_back(std::string("Light.areaHeight = ") + FormatFloat(c->AreaHeight));
        outLines.push_back(std::string("Light.areaRadius = ") + FormatFloat(c->AreaRadius));
        outLines.push_back(std::string("Light.falloff = ") + std::to_string(static_cast<uint32_t>(c->Falloff)));
        outLines.push_back(std::string("Light.decay = ") + FormatFloat(c->Decay));
        outLines.push_back(std::string("Light.fogContribution = ") + FormatFloat(c->FogContribution));
        outLines.push_back(std::string("Light.fogDensityBoost = ") + FormatFloat(c->FogDensityBoost));
        outLines.push_back(std::string("Light.fogAnisotropy = ") + FormatFloat(c->FogAnisotropy));
        outLines.push_back(std::string("Light.fogOriginFade = ") + FormatFloat(c->FogOriginFade));
        outLines.push_back(std::string("Light.castsLight = ") + (c->CastsLight ? "true" : "false"));
        outLines.push_back(std::string("Light.castsShadows = ") + (c->CastsShadows ? "true" : "false"));
        outLines.push_back(std::string("Light.cascadeCount = ") + std::to_string(c->CascadeCount));
        outLines.push_back(std::string("Light.shadowResolutionTier = ") +
                           std::to_string(static_cast<uint32_t>(c->ShadowResolutionTier)));
        outLines.push_back(std::string("Light.shadowAngularDiameter = ") +
                           FormatFloat(c->ShadowAngularDiameter));
        // Photometric authoring (resolved on the CPU at extraction). Serialized so the inspector's
        // unit dropdown + colour-temperature persist; defaults keep pre-physical-units scenes inert.
        outLines.push_back(std::string("Light.intensityUnit = ") + LightUnitToName(c->IntensityUnit));
        outLines.push_back(std::string("Light.useColorTemperature = ") + (c->UseColorTemperature ? "true" : "false"));
        outLines.push_back(std::string("Light.colorTemperature = ") + FormatFloat(c->ColorTemperature));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::Light c{};
        if (auto* existing = world.GetComponent<Components::Light>(entity))
            c = *existing;

        if (property == "type")
        {
            uint32_t v = 0;
            if (!ParseU32(value, v, outError))
                return false;
            if (v > 5)
            {
                if (outError)
                    *outError = "Light.type must be 0-5";
                return false;
            }
            c.Type = static_cast<Components::LightType>(v);
        }
        else if (property == "areashape")
        {
            uint32_t v = 0;
            if (!ParseU32(value, v, outError))
                return false;
            if (v > 3)
            {
                if (outError)
                    *outError = "Light.areaShape must be 0-3";
                return false;
            }
            c.AreaShape = static_cast<Components::AreaLightShape>(v);
        }
        else if (property == "falloff")
        {
            uint32_t v = 0;
            if (!ParseU32(value, v, outError))
                return false;
            if (v > 3)
            {
                if (outError)
                    *outError = "Light.falloff must be 0-3";
                return false;
            }
            c.Falloff = static_cast<Components::LightFalloff>(v);
        }
        else if (property == "color")
        {
            Float3 v{c.Color[0], c.Color[1], c.Color[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "Light.color must be (r, g, b)";
                return false;
            }
            c.Color[0] = v.X;
            c.Color[1] = v.Y;
            c.Color[2] = v.Z;
        }
        else if (property == "intensity")
        {
            if (!ParseF32(value, c.Intensity, outError))
                return false;
        }
        else if (property == "range")
        {
            if (!ParseF32(value, c.Range, outError))
                return false;
        }
        else if (property == "innerangle")
        {
            if (!ParseF32(value, c.InnerAngle, outError))
                return false;
        }
        else if (property == "outerangle")
        {
            if (!ParseF32(value, c.OuterAngle, outError))
                return false;
        }
        else if (property == "areawidth")
        {
            if (!ParseF32(value, c.AreaWidth, outError))
                return false;
        }
        else if (property == "areaheight")
        {
            if (!ParseF32(value, c.AreaHeight, outError))
                return false;
        }
        else if (property == "arearadius")
        {
            if (!ParseF32(value, c.AreaRadius, outError))
                return false;
        }
        else if (property == "shadowangulardiameter")
        {
            if (!ParseF32(value, c.ShadowAngularDiameter, outError))
                return false;
        }
        else if (property == "decay")
        {
            if (!ParseF32(value, c.Decay, outError))
                return false;
        }
        else if (property == "fogcontribution")
        {
            if (!ParseF32(value, c.FogContribution, outError))
                return false;
        }
        else if (property == "fogdensityboost")
        {
            if (!ParseF32(value, c.FogDensityBoost, outError))
                return false;
        }
        else if (property == "foganisotropy")
        {
            if (!ParseF32(value, c.FogAnisotropy, outError))
                return false;
        }
        else if (property == "fogoriginfade")
        {
            if (!ParseF32(value, c.FogOriginFade, outError))
                return false;
        }
        else if (property == "castslight")
        {
            if (!ParseBool(value, c.CastsLight, outError))
                return false;
        }
        else if (property == "castsshadows")
        {
            if (!ParseBool(value, c.CastsShadows, outError))
                return false;
        }
        else if (property == "cascadecount")
        {
            if (!ParseU32(value, c.CascadeCount, outError))
                return false;
        }
        else if (property == "shadowresolutiontier")
        {
            uint32_t tier = 0;
            if (!ParseU32(value, tier, outError))
                return false;
            c.ShadowResolutionTier = static_cast<Components::LightShadowTier>(tier > 3u ? 3u : tier);
        }
        else if (property == "intensityunit")
        {
            if (!ParseLightUnit(value, c.IntensityUnit, outError))
                return false;
        }
        else if (property == "usecolortemperature")
        {
            if (!ParseBool(value, c.UseColorTemperature, outError))
                return false;
        }
        else if (property == "colortemperature")
        {
            if (!ParseF32(value, c.ColorTemperature, outError))
                return false;
        }
        else
        {
            if (outError)
                *outError = "Unknown Light property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::Light>(entity))
            return true;
        Components::Light c{};
        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::Light>(entity);
        return true;
    }
};

// Prefer durable authoring data. A game that assigned a different clipGuid
// directly must not keep using an earlier source pair.
static bool FindAnimatorClipSource(const Components::Animator& c,
                                   const ISceneAssetResolver* resolver,
                                   GUID& model, uint32& index)
{
    const AssetRegistry* registry = nullptr;
    if (!resolver)
    {
        if (AssetManager* assetManager = EngineCore::GetInstance().TryGetAssetManager())
            registry = &assetManager->GetRegistry();
    }
    auto canonical = [&](const GUID& guid)
    { return resolver ? resolver->ResolveGuid(guid) : registry ? registry->ResolveGuid(guid) : guid; };
    const GUID selected = canonical(c.clipGuid.ToGuid());
    if (!c.clipSourceModelGuid.IsNull() && c.clipSourceAnimationIndex != UINT32_MAX)
    {
        model = canonical(c.clipSourceModelGuid.ToGuid());
        index = c.clipSourceAnimationIndex;
        if (selected.IsNull() || selected == ModelAsset::DeriveEmbeddedClipGuid(model, index) ||
            c.clipGuid.ToGuid() == ModelAsset::DeriveEmbeddedClipGuid(c.clipSourceModelGuid.ToGuid(), index))
            return true;
    }
    auto& store = Engine::Renderer::ClipStore::Instance();
    auto clip = store.Get(store.GetIndexIfPresent(selected));
    if (!clip || clip->GetSourcePath().empty()) return false;
    AssetType type = AssetType::Unknown;
    bool found = false;
    if (resolver)
        found = resolver->TryGetGuidAndType(clip->GetSourcePath(), model, type);
    else if (registry)
    {
        AssetMetadata metadata{};
        found = registry->TryGetAssetMetadata(clip->GetSourcePath(), metadata);
        model = metadata.Guid;
        type = metadata.Type;
    }
    if (!found || type != AssetType::Model || model.IsNull()) return false;
    model = canonical(model);
    index = clip->GetSourceAnimationIndex();
    return selected == ModelAsset::DeriveEmbeddedClipGuid(model, index);
}

static bool CompleteAnimatorClipSource(Components::Animator& c, std::string* error)
{
    const bool model = !c.clipSourceModelGuid.IsNull();
    const bool index = c.clipSourceAnimationIndex != UINT32_MAX;
    if (!model && !index) return true;
    if (model && index)
    {
        const GUID derived = ModelAsset::DeriveEmbeddedClipGuid(c.clipSourceModelGuid.ToGuid(), c.clipSourceAnimationIndex);
        if (c.clipGuid.IsNull() || c.clipGuid.ToGuid() == derived)
        {
            c.clipGuid.Set(derived);
            return true;
        }
    }
    if (error) *error = "Animator: embedded clip requires a complete, consistent model/index source";
    return false;
}

// Parse into staged bytes so grouped source assignments commit together.
static bool ParseAnimatorProperty(Components::Animator& c,
                                  const SceneLoadContext& ctx,
                                  std::string_view property,
                                  std::string_view value,
                                  std::string* outError)
{
    if (property == "autoplayonenterplaymode")
    {
        if (!ParseBool(value, c.autoPlayOnEnterPlayMode, outError))
            return false;
    }
    else if (property == "active")
    {
        if (!ParseBool(value, c.active, outError))
            return false;
    }
    else if (property == "loop")
    {
        if (!ParseBool(value, c.loop, outError))
            return false;
    }
    else if (property == "deterministic")
    {
        if (!ParseBool(value, c.deterministic, outError))
            return false;
    }
    else if (property == "resetonsave")
    {
        if (!ParseBool(value, c.resetOnSave, outError))
            return false;
    }
    else if (property == "rootmotionlocal")
    {
        if (!ParseBool(value, c.rootMotionLocal, outError))
            return false;
    }
    else if (property == "speedscale")
    {
        if (!ParseF32(value, c.speedScale, outError))
            return false;
    }
    else if (property == "blendseconds")
    {
        if (!ParseF32(value, c.blendSeconds, outError))
            return false;
    }
    else if (property == "seektimeseconds")
    {
        if (!ParseF32(value, c.seekTimeSeconds, outError))
            return false;
    }
    else if (property == "sectionstartseconds")
    {
        if (!ParseF32(value, c.sectionStartSeconds, outError))
            return false;
    }
    else if (property == "sectionendseconds")
    {
        if (!ParseF32(value, c.sectionEndSeconds, outError))
            return false;
    }
    else if (property == "audiomaxpolyphony")
    {
        int32 parsed = 0;
        if (!ParseI32(value, parsed, outError))
            return false;
        c.audioMaxPolyphony = static_cast<uint32>(std::max<int32>(1, parsed));
    }
    else if (property == "assignedanimation")
    {
        std::string parsed;
        if (!ParseSceneString(value, parsed, outError))
            return false;
        c.SetAssignedAnimation(parsed);
    }
    else if (property == "queuedanimation")
    {
        std::string parsed;
        if (!ParseSceneString(value, parsed, outError))
            return false;
        (void)parsed;
    }
    else if (property == "rootnode")
    {
        std::string parsed;
        if (!ParseSceneString(value, parsed, outError))
            return false;
        c.SetRootNode(parsed);
    }
    else if (property == "rootmotiontrack")
    {
        std::string parsed;
        if (!ParseSceneString(value, parsed, outError))
            return false;
        c.SetRootMotionTrack(parsed);
    }
    else if (property == "source")
    {
        std::string source;
        if (!ParseSceneString(value, source, outError))
            return false;
        if (source == "Clip")
            c.source = Components::AnimatorPlaybackSource::Clip;
        else if (source == "Timeline")
            c.source = Components::AnimatorPlaybackSource::Timeline;
        else if (source == "Library")
            c.source = Components::AnimatorPlaybackSource::Library;
        else if (source == "Controller")
            c.source = Components::AnimatorPlaybackSource::Controller;
        else if (source == "Graph")
            c.source = Components::AnimatorPlaybackSource::Graph;
        else
        {
            if (outError)
                *outError = "Animator.source: expected Clip, Timeline, Library, Controller, or Graph";
            return false;
        }
    }
    else if (property == "callbackprocess")
    {
        std::string mode;
        if (!ParseSceneString(value, mode, outError))
            return false;
        if (mode == "Idle")
            c.callbackProcess = Components::AnimatorCallbackProcess::Idle;
        else if (mode == "Physics")
            c.callbackProcess = Components::AnimatorCallbackProcess::Physics;
        else if (mode == "Manual")
            c.callbackProcess = Components::AnimatorCallbackProcess::Manual;
        else
        {
            if (outError)
                *outError = "Animator.callbackProcess: expected Idle, Physics, or Manual";
            return false;
        }
    }
    else if (property == "callbackmethod")
    {
        std::string mode;
        if (!ParseSceneString(value, mode, outError))
            return false;
        if (mode == "Deferred")
            c.callbackMethod = Components::AnimatorCallbackMethod::Deferred;
        else if (mode == "Immediate")
            c.callbackMethod = Components::AnimatorCallbackMethod::Immediate;
        else
        {
            if (outError)
                *outError = "Animator.callbackMethod: expected Deferred or Immediate";
            return false;
        }
    }
    else if (property == "discretecallbackmode")
    {
        std::string mode;
        if (!ParseSceneString(value, mode, outError))
            return false;
        if (mode == "Recessive")
            c.discreteCallbackMode = Components::AnimatorDiscreteCallbackMode::Recessive;
        else if (mode == "Dominant")
            c.discreteCallbackMode = Components::AnimatorDiscreteCallbackMode::Dominant;
        else
        {
            if (outError)
                *outError = "Animator.discreteCallbackMode: expected Recessive or Dominant";
            return false;
        }
    }
    else if (property == "clipsourcemodelguid")
    {
        SceneValue sv{};
        if (!ParseValue(value, sv, outError)) return false;
        AssetReference ref{};
        if (!TryResolveAssetReference(ctx, sv, AssetType::Model, ref, outError)) return false;
        if (ctx.Resolver && !ref.guid.IsNull())
        {
            std::filesystem::path path;
            AssetType type = AssetType::Unknown;
            if (ctx.Resolver->TryGetPathAndType(ref.guid, path, type) && type != AssetType::Model)
            {
                if (outError) *outError = "Animator.clipSourceModelGuid: source must be a model";
                return false;
            }
        }
        c.clipSourceModelGuid.Set(ref.guid);
        if (ref.guid.IsNull())
        {
            c.ClearClipSource();
            c.clipGuid.Clear();
        }
    }
    else if (property == "clipsourceanimationindex")
    {
        uint32 index = UINT32_MAX;
        if (!ParseU32(value, index, outError))
            return false;
        if (index == UINT32_MAX)
        {
            if (outError) *outError = "Animator.clipSourceAnimationIndex: 4294967295 is reserved for 'no embedded source'";
            return false;
        }
        c.clipSourceAnimationIndex = index;
    }
    else if (property == "clipguid")
    {
        // Accepts AssetRef [path="..." guid="..."] or legacy bare GUID/string.
        SceneValue sv{};
        if (!ParseValue(value, sv, outError))
            return false;
        AssetReference ref{};
        std::string err;
        if (!TryResolveAssetReference(ctx, sv, AssetType::Animation, ref, &err))
        {
            if (outError)
                *outError = err.empty() ? "Animator.clipGuid: invalid reference" : err;
            return false;
        }
        c.clipGuid.Set(ref.guid);
    }
    else if (property == "libraryguid")
    {
        SceneValue sv{};
        if (!ParseValue(value, sv, outError))
            return false;
        AssetReference ref{};
        std::string err;
        if (!TryResolveAssetReference(ctx, sv, AssetType::AnimationLibrary, ref, &err))
        {
            if (outError)
                *outError = err.empty() ? "Animator.libraryGuid: invalid reference" : err;
            return false;
        }
        c.libraryGuid.Set(ref.guid);
    }
    else if (property == "timelineguid")
    {
        SceneValue sv{};
        if (!ParseValue(value, sv, outError))
            return false;
        AssetReference ref{};
        std::string err;
        if (!TryResolveAssetReference(ctx, sv, AssetType::Timeline, ref, &err))
        {
            if (outError)
                *outError = err.empty() ? "Animator.timelineGuid: invalid reference" : err;
            return false;
        }
        c.timelineGuid.Set(ref.guid);
    }
    else if (property == "controllerguid")
    {
        SceneValue sv{};
        if (!ParseValue(value, sv, outError))
            return false;
        AssetReference ref{};
        std::string err;
        if (!TryResolveAssetReference(ctx, sv, AssetType::AnimationController, ref, &err))
        {
            if (outError)
                *outError = err.empty() ? "Animator.controllerGuid: invalid reference" : err;
            return false;
        }
        c.controllerGuid.Set(ref.guid);
    }
    else if (property == "graphguid")
    {
        SceneValue sv{};
        if (!ParseValue(value, sv, outError))
            return false;
        AssetReference ref{};
        std::string err;
        if (!TryResolveAssetReference(ctx, sv, AssetType::AnimationGraph, ref, &err))
        {
            if (outError)
                *outError = err.empty() ? "Animator.graphGuid: invalid reference" : err;
            return false;
        }
        c.graphGuid.Set(ref.guid);
    }
    else
    {
        if (outError)
            *outError = "Unknown Animator property";
        return false;
    }

    return true;
}

static bool ApplyAnimatorProperty(ECS::World& world, ECS::EntityHandle entity,
                                  const SceneLoadContext& ctx, std::string_view property,
                                  std::string_view value, std::string* outError)
{
    Components::Animator c{};
    if (const auto* existing = world.GetComponent<Components::Animator>(entity)) c = *existing;
    if (property == "clipguid") c.ClearClipSource();
    const bool sourceProperty = property == "clipsourcemodelguid" || property == "clipsourceanimationindex";
    if (sourceProperty) c.clipGuid.Clear();
    if (!ParseAnimatorProperty(c, ctx, property, value, outError)) return false;
    if (sourceProperty && (property != "clipsourcemodelguid" || !c.clipSourceModelGuid.IsNull()))
    {
        // SceneIO diagnoses a failed group by retrying individual properties.
        // Do not let that fallback install half of an invalid source pair.
        if (outError) *outError = "Animator: embedded model/index assignments must be applied together";
        return false;
    }
    world.AddComponentImmediate(entity, c);
    return true;
}

class AnimatorSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "Animator"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::Animator>(entity);
        if (!c)
            return;
        outLines.push_back(std::string("Animator.active = ") + (c->active ? "true" : "false"));
        outLines.push_back(std::string("Animator.autoPlayOnEnterPlayMode = ") + (c->autoPlayOnEnterPlayMode ? "true" : "false"));
        GUID clipModel;
        uint32 clipIndex = 0;
        if (FindAnimatorClipSource(*c, ctx.Resolver, clipModel, clipIndex))
        {
            outLines.push_back("Animator.clipSourceModelGuid = " + FormatAssetReferenceForSave(ctx, clipModel, ""));
            outLines.push_back("Animator.clipSourceAnimationIndex = " + std::to_string(clipIndex));
        }
        else if (c->clipSourceModelGuid.IsNull() != (c->clipSourceAnimationIndex == UINT32_MAX))
        {
            // Preserve malformed partial authoring as a rejected load, never
            // silently turn it into an unrelated default animation.
            if (!c->clipSourceModelGuid.IsNull())
                outLines.push_back("Animator.clipSourceModelGuid = " + FormatAssetReferenceForSave(ctx, c->clipSourceModelGuid.ToGuid(), ""));
            outLines.push_back("Animator.clipSourceAnimationIndex = " + std::to_string(c->clipSourceAnimationIndex));
        }
        else if (!c->clipGuid.IsNull())
            outLines.push_back(std::string("Animator.clipGuid = ") +
                               FormatAssetReferenceForSave(ctx, c->clipGuid.ToGuid(), ""));
        if (!c->libraryGuid.IsNull())
            outLines.push_back(std::string("Animator.libraryGuid = ") +
                               FormatAssetReferenceForSave(ctx, c->libraryGuid.ToGuid(), ""));
        if (!c->timelineGuid.IsNull())
            outLines.push_back(std::string("Animator.timelineGuid = ") +
                               FormatAssetReferenceForSave(ctx, c->timelineGuid.ToGuid(), ""));
        if (!c->controllerGuid.IsNull())
            outLines.push_back(std::string("Animator.controllerGuid = ") +
                               FormatAssetReferenceForSave(ctx, c->controllerGuid.ToGuid(), ""));
        if (!c->graphGuid.IsNull())
            outLines.push_back(std::string("Animator.graphGuid = ") +
                               FormatAssetReferenceForSave(ctx, c->graphGuid.ToGuid(), ""));
        if (!c->AssignedAnimation().empty())
            outLines.push_back(std::string("Animator.assignedAnimation = ") + FormatQuoted(std::string(c->AssignedAnimation())));
        if (!c->RootNode().empty())
            outLines.push_back(std::string("Animator.rootNode = ") + FormatQuoted(std::string(c->RootNode())));
        if (!c->RootMotionTrack().empty())
            outLines.push_back(std::string("Animator.rootMotionTrack = ") + FormatQuoted(std::string(c->RootMotionTrack())));
        outLines.push_back(std::string("Animator.speedScale = ") + FormatFloat(c->speedScale));
        outLines.push_back(std::string("Animator.blendSeconds = ") + FormatFloat(c->blendSeconds));
        outLines.push_back(std::string("Animator.seekTimeSeconds = ") + FormatFloat(c->seekTimeSeconds));
        outLines.push_back(std::string("Animator.sectionStartSeconds = ") + FormatFloat(c->sectionStartSeconds));
        outLines.push_back(std::string("Animator.sectionEndSeconds = ") + FormatFloat(c->sectionEndSeconds));
        outLines.push_back(std::string("Animator.audioMaxPolyphony = ") + std::to_string(c->audioMaxPolyphony));
        outLines.push_back(std::string("Animator.loop = ") + (c->loop ? "true" : "false"));
        outLines.push_back(std::string("Animator.deterministic = ") + (c->deterministic ? "true" : "false"));
        outLines.push_back(std::string("Animator.resetOnSave = ") + (c->resetOnSave ? "true" : "false"));
        outLines.push_back(std::string("Animator.rootMotionLocal = ") + (c->rootMotionLocal ? "true" : "false"));
        const char* source = "Clip";
        switch (c->source)
        {
        case Components::AnimatorPlaybackSource::Timeline:
            source = "Timeline";
            break;
        case Components::AnimatorPlaybackSource::Library:
            source = "Library";
            break;
        case Components::AnimatorPlaybackSource::Controller:
            source = "Controller";
            break;
        case Components::AnimatorPlaybackSource::Graph:
            source = "Graph";
            break;
        case Components::AnimatorPlaybackSource::Clip:
        default:
            break;
        }
        outLines.push_back(std::string("Animator.source = ") + source);
        const char* callbackProcess = "Idle";
        switch (c->callbackProcess)
        {
        case Components::AnimatorCallbackProcess::Physics:
            callbackProcess = "Physics";
            break;
        case Components::AnimatorCallbackProcess::Manual:
            callbackProcess = "Manual";
            break;
        case Components::AnimatorCallbackProcess::Idle:
        default:
            break;
        }
        const char* callbackMethod = c->callbackMethod == Components::AnimatorCallbackMethod::Immediate ? "Immediate" : "Deferred";
        const char* discreteMode = c->discreteCallbackMode == Components::AnimatorDiscreteCallbackMode::Dominant ? "Dominant" : "Recessive";
        outLines.push_back(std::string("Animator.callbackProcess = ") + callbackProcess);
        outLines.push_back(std::string("Animator.callbackMethod = ") + callbackMethod);
        outLines.push_back(std::string("Animator.discreteCallbackMode = ") + discreteMode);
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        return ApplyAnimatorProperty(world, entity, ctx, property, value, outError);
    }

    bool ApplyProperties(ECS::World& world, ECS::EntityHandle entity,
                         const SceneLoadContext& ctx,
                         std::span<const std::pair<std::string_view, std::string_view>> props,
                         std::string* outError, std::size_t* outFailedIndex) const override
    {
        Components::Animator staged{};
        if (const auto* existing = world.GetComponent<Components::Animator>(entity)) staged = *existing;
        bool source = false, clip = false;
        for (const auto& [name, value] : props)
        {
            source |= name == "clipsourcemodelguid" || name == "clipsourceanimationindex";
            clip |= name == "clipguid";
        }
        if (source && !clip) staged.clipGuid.Clear();
        if (clip && !source) staged.ClearClipSource();
        for (std::size_t i = 0; i < props.size(); ++i)
        {
            if (!ParseAnimatorProperty(staged, ctx, props[i].first, props[i].second, outError))
            {
                if (outFailedIndex) *outFailedIndex = i;
                return false;
            }
        }
        if (!CompleteAnimatorClipSource(staged, outError))
        {
            if (outFailedIndex) *outFailedIndex = props.empty() ? 0 : props.size() - 1;
            return false;
        }
        world.AddComponentImmediate(entity, staged);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::Animator>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::Animator{});
        return true;
    }

    void EnumerateAssetReferences(const ECS::World& world,
                                  ECS::EntityHandle entity,
                                  const AssetRefVisitor& visitor) const override
    {
        const auto* c = world.GetComponent<Components::Animator>(entity);
        if (!c || !visitor)
            return;
        GUID clipModel;
        uint32 clipIndex = 0;
        if (FindAnimatorClipSource(*c, nullptr, clipModel, clipIndex))
            visitor(clipModel, AssetType::Model, std::string_view{}, "clipsourcemodelguid");
        else if (!c->clipGuid.IsNull())
            visitor(c->clipGuid.ToGuid(), AssetType::Animation, std::string_view{}, "clipguid");
        if (!c->libraryGuid.IsNull())
            visitor(c->libraryGuid.ToGuid(), AssetType::AnimationLibrary, std::string_view{}, "libraryguid");
        if (!c->timelineGuid.IsNull())
            visitor(c->timelineGuid.ToGuid(), AssetType::Timeline, std::string_view{}, "timelineguid");
        if (!c->controllerGuid.IsNull())
            visitor(c->controllerGuid.ToGuid(), AssetType::AnimationController, std::string_view{}, "controllerguid");
        if (!c->graphGuid.IsNull())
            visitor(c->graphGuid.ToGuid(), AssetType::AnimationGraph, std::string_view{}, "graphguid");
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::Animator>(entity);
        return true;
    }
};

class GameLogicGraphRefSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "GameLogicGraph"; }

    void Serialize(const ECS::World& world,
                   ECS::EntityHandle entity,
                   const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::GameLogicGraphRef>(entity);
        if (!c)
            return;

        if (!c->graphGuid.IsNull())
        {
            outLines.push_back(std::string("GameLogicGraph.graph = ") +
                               FormatAssetReferenceForSave(ctx, c->graphGuid.ToGuid(), ""));
        }
        outLines.push_back(std::string("GameLogicGraph.runOnStart = ") + (c->runOnStart ? "true" : "false"));
        outLines.push_back(std::string("GameLogicGraph.runOnUpdate = ") + (c->runOnUpdate ? "true" : "false"));
        outLines.push_back(std::string("GameLogicGraph.entryFunction = ") + FormatQuoted(std::string(c->EntryFunction())));
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        Components::GameLogicGraphRef c{};
        if (auto* existing = world.GetComponent<Components::GameLogicGraphRef>(entity))
            c = *existing;

        if (property == "graph")
        {
            SceneValue sv{};
            if (!ParseValue(value, sv, outError))
                return false;

            AssetReference ref{};
            std::string err;
            if (!TryResolveAssetReference(ctx, sv, AssetType::Graph, ref, &err))
            {
                if (outError)
                    *outError = err.empty() ? "GameLogicGraph.graph: invalid graph asset reference" : err;
                return false;
            }
            c.graphGuid.Set(ref.guid);
        }
        else if (property == "runonstart")
        {
            if (!ParseBool(value, c.runOnStart, outError))
                return false;
        }
        else if (property == "runonupdate")
        {
            if (!ParseBool(value, c.runOnUpdate, outError))
                return false;
        }
        else if (property == "entryfunction")
        {
            std::string parsed;
            if (!ParseSceneString(value, parsed, outError))
                return false;
            c.SetEntryFunction(parsed);
        }
        else
        {
            if (outError)
                *outError = "Unknown GameLogicGraph property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::GameLogicGraphRef>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::GameLogicGraphRef{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::GameLogicGraphRef>(entity);
        return true;
    }

    void EnumerateAssetReferences(const ECS::World& world,
                                  ECS::EntityHandle entity,
                                  const AssetRefVisitor& visitor) const override
    {
        const auto* c = world.GetComponent<Components::GameLogicGraphRef>(entity);
        if (!c || !visitor || c->graphGuid.IsNull())
            return;

        visitor(c->graphGuid.ToGuid(), AssetType::Graph, std::string_view{}, "graph");
    }
};

// Legacy alias: loads scenes saved with the old "ModelAnimationPlayback" name.
class ModelAnimationPlaybackLegacySchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "ModelAnimationPlayback"; }
    void Serialize(const ECS::World&, ECS::EntityHandle, const SceneSaveContext&, std::vector<std::string>&) const override {}

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        return ApplyAnimatorProperty(world, entity, ctx, property, value, outError);
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::Animator>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::Animator{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::Animator>(entity);
        return true;
    }
};

class ValueCurveSchema final : public ISceneComponentSchema
{
  public:
    static std::string_view ToShapeModeName(Components::ValueCurveShapeMode mode)
    {
        using Mode = Components::ValueCurveShapeMode;
        switch (mode)
        {
        case Mode::CubicBezier:
            return "cubicBezier";
        case Mode::KeyCurve:
        default:
            return "keyCurve";
        }
    }

    static bool TryParseShapeMode(std::string_view name, Components::ValueCurveShapeMode& out)
    {
        std::string raw = std::string(name);
        std::string quoted;
        if (ParseQuotedString(name, quoted))
            raw = quoted;
        std::transform(raw.begin(), raw.end(), raw.begin(), [](unsigned char ch)
                       { return static_cast<char>(std::tolower(ch)); });
        if (raw == "keycurve" || raw == "keys" || raw == "curve" || raw == "mathcurve")
        {
            out = Components::ValueCurveShapeMode::KeyCurve;
            return true;
        }
        if (raw == "cubicbezier" || raw == "bezier" || raw == "cubic")
        {
            out = Components::ValueCurveShapeMode::CubicBezier;
            return true;
        }
        return false;
    }

    static std::string UnquoteValue(std::string_view value)
    {
        std::string raw = std::string(value);
        std::string quoted;
        if (ParseQuotedString(value, quoted))
            raw = quoted;
        return raw;
    }

    static std::string NormalizeLooseIdentifier(std::string_view value)
    {
        const std::string raw = UnquoteValue(value);
        std::string normalized;
        normalized.reserve(raw.size());
        for (unsigned char ch : raw)
        {
            if (std::isalnum(ch))
                normalized.push_back(static_cast<char>(std::tolower(ch)));
        }
        return normalized;
    }

    static bool TryParseBuiltinEasing(std::string_view name, Math::TweenEasing& out)
    {
        const std::string raw = UnquoteValue(name);
        if (Math::TryParseTweenEasingName(raw, out) && out != Math::TweenEasing::Custom)
            return true;

        const std::string normalized = NormalizeLooseIdentifier(raw);
        for (const auto& entry : Math::kTweenEasingEntries)
        {
            if (entry.Value == Math::TweenEasing::Custom)
                continue;
            if (normalized == NormalizeLooseIdentifier(entry.Name) ||
                normalized == NormalizeLooseIdentifier(Math::FormatTweenEasingDisplayName(entry.Name)))
            {
                out = entry.Value;
                return true;
            }
        }
        return false;
    }

    static std::string_view ToTargetName(Components::ValueCurveTarget target)
    {
        using Target = Components::ValueCurveTarget;
        switch (target)
        {
        case Target::None:
            return "none";
        case Target::TransformPositionX:
            return "transformPositionX";
        case Target::TransformPositionY:
            return "transformPositionY";
        case Target::TransformPositionZ:
            return "transformPositionZ";
        case Target::TransformScaleX:
            return "transformScaleX";
        case Target::TransformScaleY:
            return "transformScaleY";
        case Target::TransformScaleZ:
            return "transformScaleZ";
        case Target::LightIntensity:
            return "lightIntensity";
        case Target::PostProcessWeight:
            return "postProcessWeight";
        default:
            return "none";
        }
    }

    static bool TryParseTarget(std::string_view name, Components::ValueCurveTarget& out)
    {
        using Target = Components::ValueCurveTarget;
        if (name == "none")
        {
            out = Target::None;
            return true;
        }
        if (name == "transformpositionx")
        {
            out = Target::TransformPositionX;
            return true;
        }
        if (name == "transformpositiony")
        {
            out = Target::TransformPositionY;
            return true;
        }
        if (name == "transformpositionz")
        {
            out = Target::TransformPositionZ;
            return true;
        }
        if (name == "transformscalex")
        {
            out = Target::TransformScaleX;
            return true;
        }
        if (name == "transformscaley")
        {
            out = Target::TransformScaleY;
            return true;
        }
        if (name == "transformscalez")
        {
            out = Target::TransformScaleZ;
            return true;
        }
        if (name == "lightintensity")
        {
            out = Target::LightIntensity;
            return true;
        }
        if (name == "postprocessweight")
        {
            out = Target::PostProcessWeight;
            return true;
        }
        if (name == "postprocessexposure")
        {
            // Exposure moved off the volume onto the camera; the old curve target no longer exists.
            // Migrate legacy scenes to an inert target rather than failing the load.
            out = Target::None;
            return true;
        }
        return false;
    }

    std::string_view GetComponentName() const override { return "ValueCurve"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::ValueCurve>(entity);
        if (!c)
            return;
        SerializeProperties(*c, "ValueCurve.", outLines);
    }

    static void SerializeProperties(const Components::ValueCurve& curve, const std::string& prefix,
                                    std::vector<std::string>& outLines)
    {
        const auto* c = &curve;
        outLines.push_back((prefix + "enabled = ") + (c->Enabled ? "true" : "false"));
        outLines.push_back((prefix + "playing = ") + (c->Playing ? "true" : "false"));
        outLines.push_back((prefix + "loop = ") + (c->Loop ? "true" : "false"));
        outLines.push_back((prefix + "pingPong = ") + (c->PingPong ? "true" : "false"));
        outLines.push_back((prefix + "applyToTarget = ") + (c->ApplyToTarget ? "true" : "false"));
        outLines.push_back((prefix + "target = ") + std::string(ToTargetName(c->Target)));
        outLines.push_back((prefix + "durationSeconds = ") + FormatFloat(c->DurationSeconds));
        outLines.push_back((prefix + "startValue = ") + FormatFloat(c->StartValue));
        outLines.push_back((prefix + "endValue = ") + FormatFloat(c->EndValue));
        outLines.push_back((prefix + "shapeMode = ") + std::string(ToShapeModeName(c->ShapeMode)));
        outLines.push_back((prefix + "useBuiltinEasing = ") + (c->UseBuiltinEasing ? "true" : "false"));
        outLines.push_back((prefix + "builtinEasing = ") + std::string(Math::ToTweenEasingName(c->BuiltinEasing)));
        outLines.push_back((prefix + "cubicBezierControl1X = ") + FormatFloat(c->CubicBezierControl1X));
        outLines.push_back((prefix + "cubicBezierControl1Y = ") + FormatFloat(c->CubicBezierControl1Y));
        outLines.push_back((prefix + "cubicBezierControl2X = ") + FormatFloat(c->CubicBezierControl2X));
        outLines.push_back((prefix + "cubicBezierControl2Y = ") + FormatFloat(c->CubicBezierControl2Y));
        outLines.push_back((prefix + "cubicBezierAnchorStartY = ") + FormatFloat(c->CubicBezierAnchorStartY));
        outLines.push_back((prefix + "cubicBezierAnchorEndY = ") + FormatFloat(c->CubicBezierAnchorEndY));
        outLines.push_back((prefix + "shape = ") + FormatCurveKeys(c->Shape));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::ValueCurve c{};
        if (auto* existing = world.GetComponent<Components::ValueCurve>(entity))
            c = *existing;

        if (!ApplyValueProperty(c, property, value, outError))
            return false;
        world.AddComponentImmediate(entity, c);
        return true;
    }

    static bool ApplyValueProperty(Components::ValueCurve& c, std::string_view property,
                                   std::string_view value, std::string* outError)
    {

        if (property == "enabled")
        {
            if (!ParseBool(value, c.Enabled, outError))
                return false;
        }
        else if (property == "playing")
        {
            if (!ParseBool(value, c.Playing, outError))
                return false;
        }
        else if (property == "loop")
        {
            if (!ParseBool(value, c.Loop, outError))
                return false;
        }
        else if (property == "pingpong")
        {
            if (!ParseBool(value, c.PingPong, outError))
                return false;
        }
        else if (property == "applytotarget")
        {
            if (!ParseBool(value, c.ApplyToTarget, outError))
                return false;
        }
        else if (property == "target")
        {
            std::string raw = std::string(value);
            std::string quoted;
            if (ParseQuotedString(value, quoted))
                raw = quoted;
            std::transform(raw.begin(), raw.end(), raw.begin(), [](unsigned char ch)
                           { return static_cast<char>(std::tolower(ch)); });
            if (!TryParseTarget(raw, c.Target))
            {
                if (outError)
                    *outError = "ValueCurve.target must be a known target name";
                return false;
            }
        }
        else if (property == "durationseconds")
        {
            if (!ParseF32(value, c.DurationSeconds, outError))
                return false;
        }
        else if (property == "startvalue")
        {
            if (!ParseF32(value, c.StartValue, outError))
                return false;
        }
        else if (property == "endvalue")
        {
            if (!ParseF32(value, c.EndValue, outError))
                return false;
        }
        else if (property == "shapemode")
        {
            if (!TryParseShapeMode(value, c.ShapeMode))
            {
                if (outError)
                    *outError = "ValueCurve.shapeMode must be keyCurve or cubicBezier";
                return false;
            }
            if (c.ShapeMode == Components::ValueCurveShapeMode::CubicBezier)
                c.UseBuiltinEasing = false;
            Components::RefreshValueCurveCurrentValueFromEvaluation(c);
        }
        else if (property == "usebuiltineasing")
        {
            if (!ParseBool(value, c.UseBuiltinEasing, outError))
                return false;
            if (c.UseBuiltinEasing)
            {
                c.ShapeMode = Components::ValueCurveShapeMode::KeyCurve;
                c.Shape = Components::MakeValueCurveShapeFromEasing(c.BuiltinEasing);
            }
            Components::RefreshValueCurveCurrentValueFromEvaluation(c);
        }
        else if (property == "builtineasing")
        {
            Math::TweenEasing easing = Math::TweenEasing::Linear;
            if (!TryParseBuiltinEasing(value, easing))
            {
                if (outError)
                    *outError = "ValueCurve.builtinEasing must be a known built-in easing name";
                return false;
            }
            c.BuiltinEasing = easing;
            if (c.UseBuiltinEasing)
            {
                c.ShapeMode = Components::ValueCurveShapeMode::KeyCurve;
                c.Shape = Components::MakeValueCurveShapeFromEasing(easing);
            }
            Components::RefreshValueCurveCurrentValueFromEvaluation(c);
        }
        else if (property == "cubicbeziercontrol1x" || property == "customcontrol1x")
        {
            if (!ParseF32(value, c.CubicBezierControl1X, outError))
                return false;
            Components::RefreshValueCurveCurrentValueFromEvaluation(c);
        }
        else if (property == "cubicbeziercontrol1y" || property == "customcontrol1y")
        {
            if (!ParseF32(value, c.CubicBezierControl1Y, outError))
                return false;
            Components::RefreshValueCurveCurrentValueFromEvaluation(c);
        }
        else if (property == "cubicbeziercontrol2x" || property == "customcontrol2x")
        {
            if (!ParseF32(value, c.CubicBezierControl2X, outError))
                return false;
            Components::RefreshValueCurveCurrentValueFromEvaluation(c);
        }
        else if (property == "cubicbeziercontrol2y" || property == "customcontrol2y")
        {
            if (!ParseF32(value, c.CubicBezierControl2Y, outError))
                return false;
            Components::RefreshValueCurveCurrentValueFromEvaluation(c);
        }
        else if (property == "cubicbezieranchorstarty")
        {
            if (!ParseF32(value, c.CubicBezierAnchorStartY, outError))
                return false;
            Components::RefreshValueCurveCurrentValueFromEvaluation(c);
        }
        else if (property == "cubicbezieranchorendy")
        {
            if (!ParseF32(value, c.CubicBezierAnchorEndY, outError))
                return false;
            Components::RefreshValueCurveCurrentValueFromEvaluation(c);
        }
        else if (property == "curve")
        {
            std::string raw = std::string(value);
            std::string quoted;
            if (ParseQuotedString(value, quoted))
                raw = quoted;
            std::transform(raw.begin(), raw.end(), raw.begin(), [](unsigned char ch)
                           { return static_cast<char>(std::tolower(ch)); });
            if (raw == "custom")
            {
                c.ShapeMode = Components::ValueCurveShapeMode::CubicBezier;
                c.UseBuiltinEasing = false;
                Components::RefreshValueCurveCurrentValueFromEvaluation(c);
            }
            else
            {
                Math::TweenEasing easing = Math::TweenEasing::Linear;
                if (TryParseBuiltinEasing(value, easing))
                {
                    c.ShapeMode = Components::ValueCurveShapeMode::KeyCurve;
                    c.UseBuiltinEasing = true;
                    c.BuiltinEasing = easing;
                    c.Shape = Components::MakeValueCurveShapeFromEasing(easing);
                    Components::RefreshValueCurveCurrentValueFromEvaluation(c);
                }
            }
        }
        else if (property == "shape")
        {
            c.Shape = ParseCurveKeys(value);
            if (c.Shape.KeyCount == 0)
                c.Shape = Components::MakeDefaultValueCurveShape();
            Components::RefreshValueCurveCurrentValueFromEvaluation(c);
        }
        else
        {
            // Legacy point/preset-library fields are not modeled by the restored compact Bezier mode.
            // Keep ignoring them so old scenes still load without losing the component.
        }

        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::ValueCurve>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::ValueCurve{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::ValueCurve>(entity);
        return true;
    }
};

class PostProcessVolumeSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "PostProcessVolume"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::PostProcessVolume>(entity);
        if (!c)
            return;
        outLines.push_back(std::string("PostProcessVolume.weight = ") + FormatFloat(c->Weight));
        outLines.push_back(std::string("PostProcessVolume.priority = ") + std::to_string(c->Priority));
        outLines.push_back(std::string("PostProcessVolume.postProcessMask = ") + std::to_string(c->PostProcessMask));
        outLines.push_back(std::string("PostProcessVolume.tonemap = ") + std::to_string(static_cast<int32_t>(c->Tonemap)));
        outLines.push_back(std::string("PostProcessVolume.ditherMode = ") + std::to_string(c->DitherMode));
        outLines.push_back(std::string("PostProcessVolume.ictcpChromaCompression = ") +
                           FormatFloat(c->IctcpChromaCompression));
        outLines.push_back(std::string("PostProcessVolume.isGlobal = ") + (c->IsGlobal ? "true" : "false"));
        outLines.push_back(std::string("PostProcessVolume.shape = ") + std::to_string(static_cast<int32_t>(c->Shape)));
        outLines.push_back(std::string("PostProcessVolume.blendDistance = ") + FormatFloat(c->BlendDistance));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::PostProcessVolume c{};
        if (auto* existing = world.GetComponent<Components::PostProcessVolume>(entity))
            c = *existing;

        if (property == "weight")
        {
            if (!ParseF32(value, c.Weight, outError))
                return false;
        }
        else if (property == "priority")
        {
            if (!ParseI32(value, c.Priority, outError))
                return false;
        }
        else if (property == "postprocessmask")
        {
            if (!ParseU32(value, c.PostProcessMask, outError))
                return false;
        }
        else if (property == "exposure" || property == "exposurecontrol" ||
                 property == "manualexposureev" ||
                 property == "aperture" || property == "shuttertime" || property == "iso" ||
                 property == "autoexposurespeedup" || property == "autoexposurespeeddown")
        {
            // Exposure moved off the volume onto the camera (and the world default for camera-less
            // views). Silently accept and drop the legacy owning-semantics keys so older scenes
            // still load; the modifier-shaped keys below re-home onto ExposureAdjustmentEffect.
        }
        else if (property == "exposurecompensation" || property == "autoexposureminev" ||
                 property == "autoexposuremaxev")
        {
            // Legacy volume exposure keys with modifier semantics revive onto
            // ExposureAdjustmentEffect (like the bloom* keys below onto BloomEffect), but only
            // when authored away from the old defaults so untouched scenes stay component-free.
            float parsed = 0.0f;
            if (!ParseF32(value, parsed, outError))
                return false;
            // The defaults the legacy volume serializer wrote for an untouched sensor —
            // historical values, deliberately frozen (they do NOT track Camera defaults).
            constexpr float kLegacyDefaultMinEv = 4.0f;
            constexpr float kLegacyDefaultMaxEv = 18.0f;
            const bool authored =
                (property == "exposurecompensation" && parsed != 0.0f) ||
                (property == "autoexposureminev" && parsed != kLegacyDefaultMinEv) ||
                (property == "autoexposuremaxev" && parsed != kLegacyDefaultMaxEv);
            if (authored)
            {
                Components::ExposureAdjustmentEffect adjust{};
                if (auto* existing = world.GetComponent<Components::ExposureAdjustmentEffect>(entity))
                    adjust = *existing;
                if (property == "exposurecompensation")
                {
                    adjust.Compensation = parsed;
                }
                else if (property == "autoexposureminev")
                {
                    adjust.ClampMin = true;
                    adjust.MinEv = parsed;
                }
                else
                {
                    adjust.ClampMax = true;
                    adjust.MaxEv = parsed;
                }
                world.AddComponentImmediate(entity, adjust);
                world.AddComponentImmediate(entity, c);
                return true;
            }
        }
        else if (property == "bloomthreshold" || property == "bloomknee" || property == "bloomintensity")
        {
            Components::BloomEffect bloom{};
            if (auto* existing = world.GetComponent<Components::BloomEffect>(entity))
                bloom = *existing;

            if (property == "bloomthreshold")
            {
                if (!ParseF32(value, bloom.Threshold, outError))
                    return false;
            }
            else if (property == "bloomknee")
            {
                if (!ParseF32(value, bloom.Knee, outError))
                    return false;
            }
            else if (property == "bloomintensity")
            {
                if (!ParseF32(value, bloom.Intensity, outError))
                    return false;
            }

            world.AddComponentImmediate(entity, bloom);
            world.AddComponentImmediate(entity, c);
            return true;
        }
        else if (property == "tonemap")
        {
            int32_t v = 0;
            if (!ParseI32(value, v, outError))
                return false;
            if (!Components::IsValidTonemapModeValue(v))
            {
                if (outError)
                    *outError = "PostProcessVolume.tonemap out of range";
                return false;
            }
            c.Tonemap = static_cast<Components::TonemapMode>(v);
        }
        else if (property == "dithermode")
        {
            if (!ParseI32(value, c.DitherMode, outError))
                return false;
        }
        else if (property == "ictcpchromacompression")
        {
            if (!ParseF32(value, c.IctcpChromaCompression, outError))
                return false;
            c.IctcpChromaCompression = std::clamp(c.IctcpChromaCompression, 0.0f, 1.0f);
        }
        else if (property == "isglobal")
        {
            if (!ParseBool(value, c.IsGlobal, outError))
                return false;
        }
        else if (property == "shape")
        {
            int32_t v = 0;
            if (!ParseI32(value, v, outError))
                return false;
            if (v < 0 || v > static_cast<int32_t>(Components::PostProcessVolumeShape::Cylinder))
            {
                if (outError)
                    *outError = "PostProcessVolume.shape out of range (0=Box, 1=Sphere, 2=Capsule, 3=Cylinder)";
                return false;
            }
            c.Shape = static_cast<Components::PostProcessVolumeShape>(v);
        }
        else if (property == "blenddistance")
        {
            if (!ParseF32(value, c.BlendDistance, outError))
                return false;
        }
        else if (property.rfind("volumetricfog", 0) == 0)
        {
            Components::VolumetricFogEffect fog{};
            if (auto* existing = world.GetComponent<Components::VolumetricFogEffect>(entity))
                fog = *existing;

            if (property == "volumetricfogenabled")
            {
                if (!ParseBool(value, fog.Enabled, outError))
                    return false;
            }
            else if (property == "volumetricfogintensity")
            {
                if (!ParseF32(value, fog.Intensity, outError))
                    return false;
            }
            else if (property == "volumetricfogmaxdistance")
            {
                if (!ParseF32(value, fog.MaxDistance, outError))
                    return false;
            }
            else if (property == "volumetricfogxycellsizepixels")
            {
                if (!ParseI32(value, fog.XYCellSizePixels, outError))
                    return false;
            }
            else if (property == "volumetricfogzslicecount")
            {
                if (!ParseI32(value, fog.ZSliceCount, outError))
                    return false;
            }
            else if (property == "volumetricfogdepthdistribution")
            {
                if (!ParseF32(value, fog.DepthDistribution, outError))
                    return false;
            }
            else if (property == "volumetricfogdensity")
            {
                if (!ParseF32(value, fog.Density, outError))
                    return false;
            }
            else if (property == "volumetricfogbaseheight")
            {
                if (!ParseF32(value, fog.BaseHeight, outError))
                    return false;
            }
            else if (property == "volumetricfogheightfalloff")
            {
                if (!ParseF32(value, fog.HeightFalloff, outError))
                    return false;
            }
            else if (property == "volumetricfogskyfade")
            {
                if (!ParseF32(value, fog.SkyFade, outError))
                    return false;
            }
            else if (property == "volumetricfogalbedo")
            {
                Float3 v{fog.Albedo[0], fog.Albedo[1], fog.Albedo[2]};
                if (!ParseFloat3(value, v))
                {
                    if (outError)
                        *outError = "PostProcessVolume.volumetricFogAlbedo must be (r, g, b)";
                    return false;
                }
                fog.Albedo[0] = v.X;
                fog.Albedo[1] = v.Y;
                fog.Albedo[2] = v.Z;
            }
            else if (property == "volumetricfogemission")
            {
                Float3 v{fog.Emission[0], fog.Emission[1], fog.Emission[2]};
                if (!ParseFloat3(value, v))
                {
                    if (outError)
                        *outError = "PostProcessVolume.volumetricFogEmission must be (r, g, b)";
                    return false;
                }
                fog.Emission[0] = v.X;
                fog.Emission[1] = v.Y;
                fog.Emission[2] = v.Z;
            }
            else if (property == "volumetricfoganisotropy")
            {
                if (!ParseF32(value, fog.Anisotropy, outError))
                    return false;
            }
            else if (property == "volumetricfogtrackdirectionallight")
            {
                if (!ParseBool(value, fog.TrackDirectionalLight, outError))
                    return false;
            }
            else if (property == "volumetricfogsunintensityscale")
            {
                if (!ParseF32(value, fog.SunIntensityScale, outError))
                    return false;
            }
            else if (property == "volumetricfogsunscatteringtint")
            {
                Float3 v{fog.SunScatteringTint[0], fog.SunScatteringTint[1], fog.SunScatteringTint[2]};
                if (!ParseFloat3(value, v))
                {
                    if (outError)
                        *outError = "PostProcessVolume.volumetricFogSunScatteringTint must be (r, g, b)";
                    return false;
                }
                fog.SunScatteringTint[0] = v.X;
                fog.SunScatteringTint[1] = v.Y;
                fog.SunScatteringTint[2] = v.Z;
            }
            else if (property == "volumetricfogambientscatteringtint")
            {
                Float3 v{fog.AmbientScatteringTint[0], fog.AmbientScatteringTint[1], fog.AmbientScatteringTint[2]};
                if (!ParseFloat3(value, v))
                {
                    if (outError)
                        *outError = "PostProcessVolume.volumetricFogAmbientScatteringTint must be (r, g, b)";
                    return false;
                }
                fog.AmbientScatteringTint[0] = v.X;
                fog.AmbientScatteringTint[1] = v.Y;
                fog.AmbientScatteringTint[2] = v.Z;
            }
            else if (property == "volumetricfognoiseenabled")
            {
                if (!ParseBool(value, fog.NoiseEnabled, outError))
                    return false;
            }
            else if (property == "volumetricfognoisescale")
            {
                if (!ParseF32(value, fog.NoiseScale, outError))
                    return false;
            }
            else if (property == "volumetricfognoisestrength")
            {
                if (!ParseF32(value, fog.NoiseStrength, outError))
                    return false;
            }
            else if (property == "volumetricfognoisevelocity")
            {
                Float3 v{fog.NoiseVelocity[0], fog.NoiseVelocity[1], fog.NoiseVelocity[2]};
                if (!ParseFloat3(value, v))
                {
                    if (outError)
                        *outError = "PostProcessVolume.volumetricFogNoiseVelocity must be (x, y, z)";
                    return false;
                }
                fog.NoiseVelocity[0] = v.X;
                fog.NoiseVelocity[1] = v.Y;
                fog.NoiseVelocity[2] = v.Z;
            }
            else if (property == "volumetricfognoisecontrast")
            {
                if (!ParseF32(value, fog.NoiseContrast, outError))
                    return false;
            }
            else if (property == "volumetricfogtemporalenabled")
            {
                if (!ParseBool(value, fog.TemporalEnabled, outError))
                    return false;
            }
            else if (property == "volumetricfogtemporalblend")
            {
                if (!ParseF32(value, fog.TemporalBlend, outError))
                    return false;
            }
            else if (property == "volumetricfogjitterstrength")
            {
                if (!ParseF32(value, fog.JitterStrength, outError))
                    return false;
            }
            else
            {
                if (outError)
                    *outError = "Unknown legacy PostProcessVolume volumetricFog property";
                return false;
            }

            world.AddComponentImmediate(entity, fog);
            world.AddComponentImmediate(entity, c);
            return true;
        }
        else
        {
            if (outError)
                *outError = "Unknown PostProcessVolume property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::PostProcessVolume>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::PostProcessVolume{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::PostProcessVolume>(entity);
        return true;
    }
};

class WindVolumeSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "WindVolume"; }

    void Serialize(const ECS::World& world,
                   ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::WindVolume>(entity);
        if (!c)
            return;

        outLines.push_back(std::string("WindVolume.isGlobal = ") + (c->IsGlobal ? "true" : "false"));
        outLines.push_back("WindVolume.shape = " + std::to_string(static_cast<int32_t>(c->Shape)));
        outLines.push_back("WindVolume.blendMode = " + std::to_string(static_cast<int32_t>(c->BlendMode)));
        outLines.push_back("WindVolume.blendDistance = " + FormatFloat(c->BlendDistance));
        outLines.push_back("WindVolume.weight = " + FormatFloat(c->Weight));
        outLines.push_back("WindVolume.priority = " + std::to_string(c->Priority));
        outLines.push_back("WindVolume.layerMask = " + std::to_string(c->LayerMask));
        outLines.push_back("WindVolume.direction = (" +
                           FormatFloat(c->DirectionX) + ", " +
                           FormatFloat(c->DirectionY) + ", " +
                           FormatFloat(c->DirectionZ) + ")");
        outLines.push_back("WindVolume.speed = " + FormatFloat(c->Speed));
        outLines.push_back("WindVolume.turbulence = " + FormatFloat(c->Turbulence));
        outLines.push_back("WindVolume.gustFrequency = " + FormatFloat(c->GustFrequency));
        outLines.push_back("WindVolume.gustScale = " + FormatFloat(c->GustScale));
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        Components::WindVolume c{};
        if (const auto* existing = world.GetComponent<Components::WindVolume>(entity))
            c = *existing;

        if (property == "isglobal")
        {
            if (!ParseBool(value, c.IsGlobal, outError))
                return false;
        }
        else if (property == "shape")
        {
            int32_t v = 0;
            if (!ParseI32(value, v, outError))
                return false;
            if (v < 0 || v > static_cast<int32_t>(Components::WindVolumeShape::Cylinder))
            {
                if (outError)
                    *outError = "WindVolume.shape out of range";
                return false;
            }
            c.Shape = static_cast<Components::WindVolumeShape>(v);
        }
        else if (property == "blendmode")
        {
            int32_t v = 0;
            if (!ParseI32(value, v, outError))
                return false;
            if (v < 0 || v > static_cast<int32_t>(Components::WindVolumeBlendMode::Override))
            {
                if (outError)
                    *outError = "WindVolume.blendMode out of range";
                return false;
            }
            c.BlendMode = static_cast<Components::WindVolumeBlendMode>(v);
        }
        else if (property == "blenddistance")
        {
            if (!ParseF32(value, c.BlendDistance, outError))
                return false;
            c.BlendDistance = std::max(0.0f, c.BlendDistance);
        }
        else if (property == "weight")
        {
            if (!ParseF32(value, c.Weight, outError))
                return false;
            c.Weight = std::clamp(c.Weight, 0.0f, 1.0f);
        }
        else if (property == "priority")
        {
            if (!ParseI32(value, c.Priority, outError))
                return false;
        }
        else if (property == "layermask")
        {
            if (!ParseU32(value, c.LayerMask, outError))
                return false;
        }
        else if (property == "direction")
        {
            Float3 v{c.DirectionX, c.DirectionY, c.DirectionZ};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "WindVolume.direction must be (x, y, z)";
                return false;
            }
            c.DirectionX = v.X;
            c.DirectionY = v.Y;
            c.DirectionZ = v.Z;
        }
        else if (property == "speed")
        {
            if (!ParseF32(value, c.Speed, outError))
                return false;
            c.Speed = std::max(0.0f, c.Speed);
        }
        else if (property == "turbulence")
        {
            if (!ParseF32(value, c.Turbulence, outError))
                return false;
            c.Turbulence = std::max(0.0f, c.Turbulence);
        }
        else if (property == "gustfrequency")
        {
            if (!ParseF32(value, c.GustFrequency, outError))
                return false;
            c.GustFrequency = std::max(0.0f, c.GustFrequency);
        }
        else if (property == "gustscale")
        {
            if (!ParseF32(value, c.GustScale, outError))
                return false;
            c.GustScale = std::max(0.001f, c.GustScale);
        }
        else
        {
            if (outError)
                *outError = "Unknown WindVolume property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::WindVolume>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::WindVolume{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::WindVolume>(entity);
        return true;
    }
};

class BloomEffectSchema final : public ISceneComponentSchema
{
    static bool IsLegacyBuiltInLensDirtReference(const SceneValue& value)
    {
        constexpr std::string_view kLegacyLensDirtGuid =
            "a8fa27ad-08b8-42b7-9a67-3504f6910034";

        std::string_view guidText;
        if (value.Kind == SceneValueKind::AssetRef)
            guidText = AssetRefGuid(value);
        else if (value.Kind == SceneValueKind::String || value.Kind == SceneValueKind::GuidRef)
            guidText = value.StringValue;

        if (guidText.empty())
            return false;

        try
        {
            return GUID(std::string(guidText)) == GUID(std::string(kLegacyLensDirtGuid));
        }
        catch (...)
        {
            return false;
        }
    }

  public:
    std::string_view GetComponentName() const override { return "BloomEffect"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::BloomEffect>(entity);
        if (!c)
            return;
        outLines.push_back(std::string("BloomEffect.enabled = ") + (c->Enabled ? "true" : "false"));
        outLines.push_back(std::string("BloomEffect.threshold = ") + FormatFloat(c->Threshold));
        outLines.push_back(std::string("BloomEffect.knee = ") + FormatFloat(c->Knee));
        outLines.push_back(std::string("BloomEffect.antiFlicker = ") + (c->AntiFlicker ? "true" : "false"));
        outLines.push_back(std::string("BloomEffect.intensity = ") + FormatFloat(c->Intensity));
        outLines.push_back(std::string("BloomEffect.scatteringAmount = ") + FormatFloat(c->ScatteringAmount));
        outLines.push_back(std::string("BloomEffect.tintR = ") + FormatFloat(c->Tint[0]));
        outLines.push_back(std::string("BloomEffect.tintG = ") + FormatFloat(c->Tint[1]));
        outLines.push_back(std::string("BloomEffect.tintB = ") + FormatFloat(c->Tint[2]));
        outLines.push_back(std::string("BloomEffect.radius = ") + FormatFloat(c->Radius));
        outLines.push_back(std::string("BloomEffect.octaves = ") + std::to_string(c->Octaves));
        outLines.push_back(std::string("BloomEffect.scatter = ") + FormatFloat(c->Scatter));
        outLines.push_back(std::string("BloomEffect.depthVeilEnabled = ") + (c->DepthVeilEnabled ? "true" : "false"));
        outLines.push_back(std::string("BloomEffect.depthVeilIntensity = ") + FormatFloat(c->DepthVeilIntensity));
        outLines.push_back(std::string("BloomEffect.depthVeilStart = ") + FormatFloat(c->DepthVeilStart));
        outLines.push_back(std::string("BloomEffect.depthVeilEnd = ") + FormatFloat(c->DepthVeilEnd));
        outLines.push_back(std::string("BloomEffect.depthVeilTintR = ") + FormatFloat(c->DepthVeilTint[0]));
        outLines.push_back(std::string("BloomEffect.depthVeilTintG = ") + FormatFloat(c->DepthVeilTint[1]));
        outLines.push_back(std::string("BloomEffect.depthVeilTintB = ") + FormatFloat(c->DepthVeilTint[2]));
        outLines.push_back(std::string("BloomEffect.lensDirtEnabled = ") + (c->LensDirtEnabled ? "true" : "false"));
        outLines.push_back(std::string("BloomEffect.lensDirtVignette = ") + (c->LensDirtVignette ? "true" : "false"));
        outLines.push_back(std::string("BloomEffect.lensDirtVignetteIntensity = ") + FormatFloat(c->LensDirtVignetteIntensity));
        outLines.push_back(std::string("BloomEffect.lensDirtVignetteRadius = ") + FormatFloat(c->LensDirtVignetteRadius));
        outLines.push_back(std::string("BloomEffect.lensDirtVignetteSmoothness = ") + FormatFloat(c->LensDirtVignetteSmoothness));
        outLines.push_back(std::string("BloomEffect.lensDirtVignetteRounded = ") + (c->LensDirtVignetteRounded ? "true" : "false"));
        outLines.push_back(std::string("BloomEffect.lensDirtVignetteColorR = ") + FormatFloat(c->LensDirtVignetteColor[0]));
        outLines.push_back(std::string("BloomEffect.lensDirtVignetteColorG = ") + FormatFloat(c->LensDirtVignetteColor[1]));
        outLines.push_back(std::string("BloomEffect.lensDirtVignetteColorB = ") + FormatFloat(c->LensDirtVignetteColor[2]));
        outLines.push_back(std::string("BloomEffect.lensDirtIntensity = ") + FormatFloat(c->LensDirtIntensity));
        outLines.push_back(std::string("BloomEffect.lensDirtScatter = ") + FormatFloat(c->LensDirtScatter));
        if (!c->LensDirtTexture.IsNull())
            outLines.push_back(std::string("BloomEffect.lensDirtTexture = ") +
                               FormatAssetReferenceForSave(ctx, c->LensDirtTexture.ToGuid(), ""));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::BloomEffect c{};
        if (auto* existing = world.GetComponent<Components::BloomEffect>(entity))
            c = *existing;

        if (property == "enabled")
        {
            if (!ParseBool(value, c.Enabled, outError))
                return false;
        }
        else if (property == "threshold")
        {
            if (!ParseF32(value, c.Threshold, outError))
                return false;
        }
        else if (property == "knee")
        {
            if (!ParseF32(value, c.Knee, outError))
                return false;
        }
        else if (property == "antiflicker")
        {
            if (!ParseBool(value, c.AntiFlicker, outError))
                return false;
        }
        else if (property == "intensity")
        {
            if (!ParseF32(value, c.Intensity, outError))
                return false;
        }
        else if (property == "scatteringamount")
        {
            if (!ParseF32(value, c.ScatteringAmount, outError)) return false;
            c.ScatteringAmount = std::isfinite(c.ScatteringAmount)
                ? std::clamp(c.ScatteringAmount, 0.0f, 1.0f) : 0.0f;
        }
        else if (property == "tintr")
        {
            if (!ParseF32(value, c.Tint[0], outError))
                return false;
        }
        else if (property == "tintg")
        {
            if (!ParseF32(value, c.Tint[1], outError))
                return false;
        }
        else if (property == "tintb")
        {
            if (!ParseF32(value, c.Tint[2], outError))
                return false;
        }
        else if (property == "radius")
        {
            if (!ParseF32(value, c.Radius, outError))
                return false;
        }
        else if (property == "octaves")
        {
            if (!ParseI32(value, c.Octaves, outError))
                return false;
        }
        else if (property == "scatter")
        {
            if (!ParseF32(value, c.Scatter, outError))
                return false;
        }
        else if (property == "depthveilenabled")
        {
            if (!ParseBool(value, c.DepthVeilEnabled, outError))
                return false;
        }
        else if (property == "depthveilintensity")
        {
            if (!ParseF32(value, c.DepthVeilIntensity, outError))
                return false;
        }
        else if (property == "depthveilstart")
        {
            if (!ParseF32(value, c.DepthVeilStart, outError))
                return false;
        }
        else if (property == "depthveilend")
        {
            if (!ParseF32(value, c.DepthVeilEnd, outError))
                return false;
        }
        else if (property == "depthveiltintr")
        {
            if (!ParseF32(value, c.DepthVeilTint[0], outError))
                return false;
        }
        else if (property == "depthveiltintg")
        {
            if (!ParseF32(value, c.DepthVeilTint[1], outError))
                return false;
        }
        else if (property == "depthveiltintb")
        {
            if (!ParseF32(value, c.DepthVeilTint[2], outError))
                return false;
        }
        else if (property == "lensdirtenabled")
        {
            if (!ParseBool(value, c.LensDirtEnabled, outError))
                return false;
        }
        else if (property == "lensdirtvignette")
        {
            if (!ParseBool(value, c.LensDirtVignette, outError))
                return false;
        }
        else if (property == "lensdirtvignetteintensity")
        {
            if (!ParseF32(value, c.LensDirtVignetteIntensity, outError))
                return false;
        }
        else if (property == "lensdirtvignetteradius")
        {
            if (!ParseF32(value, c.LensDirtVignetteRadius, outError))
                return false;
        }
        else if (property == "lensdirtvignettesmoothness")
        {
            if (!ParseF32(value, c.LensDirtVignetteSmoothness, outError))
                return false;
        }
        else if (property == "lensdirtvignetterounded")
        {
            if (!ParseBool(value, c.LensDirtVignetteRounded, outError))
                return false;
        }
        else if (property == "lensdirtvignettecolorr")
        {
            if (!ParseF32(value, c.LensDirtVignetteColor[0], outError))
                return false;
        }
        else if (property == "lensdirtvignettecolorg")
        {
            if (!ParseF32(value, c.LensDirtVignetteColor[1], outError))
                return false;
        }
        else if (property == "lensdirtvignettecolorb")
        {
            if (!ParseF32(value, c.LensDirtVignetteColor[2], outError))
                return false;
        }
        else if (property == "lensdirtintensity")
        {
            if (!ParseF32(value, c.LensDirtIntensity, outError))
                return false;
        }
        else if (property == "lensdirtscatter")
        {
            if (!ParseF32(value, c.LensDirtScatter, outError))
                return false;
        }
        else if (property == "lensdirttexture")
        {
            SceneValue sv{};
            if (!ParseValue(value, sv, outError))
                return false;

            // The former Sonic Ether package used a stored GUID for the texture
            // that is now the path-derived built-in default. Clearing that exact
            // legacy reference selects the same texture without leaving existing
            // scenes bound to the removed package namespace.
            if (IsLegacyBuiltInLensDirtReference(sv))
            {
                c.LensDirtTexture.Clear();
            }
            else
            {
                AssetReference ref{};
                std::string err;
                if (!TryResolveAssetReference(ctx, sv, AssetType::Texture, ref, &err))
                {
                    if (outError)
                        *outError = err.empty() ? "BloomEffect.lensDirtTexture: invalid reference" : err;
                    return false;
                }
                c.LensDirtTexture.Set(ref.guid);
            }
        }
        else
        {
            if (outError)
                *outError = "Unknown BloomEffect property";
            return false;
        }

        c.Radius = std::clamp(c.Radius, 1.0f, 7.0f);
        for (float32& channel : c.Tint)
            channel = std::clamp(channel, 0.0f, 1.0f);
        c.Octaves = std::clamp(c.Octaves, 3, 8);
        c.Scatter = std::clamp(c.Scatter, 0.0f, 1.0f);
        c.DepthVeilIntensity = std::clamp(c.DepthVeilIntensity, 0.0f, 10.0f);
        c.DepthVeilStart = std::clamp(c.DepthVeilStart, 0.0f, 100000.0f);
        c.DepthVeilEnd = std::clamp(c.DepthVeilEnd, 0.0f, 100000.0f);
        for (float32& channel : c.DepthVeilTint)
            channel = std::clamp(channel, 0.0f, 1.0f);
        c.LensDirtVignetteIntensity = std::clamp(c.LensDirtVignetteIntensity, 0.0f, 1.0f);
        c.LensDirtVignetteRadius = std::clamp(c.LensDirtVignetteRadius, 0.0f, 1.0f);
        c.LensDirtVignetteSmoothness = std::clamp(c.LensDirtVignetteSmoothness, 0.0f, 1.0f);
        for (float32& channel : c.LensDirtVignetteColor)
            channel = std::clamp(channel, 0.0f, 1.0f);
        c.LensDirtIntensity = std::clamp(c.LensDirtIntensity, 0.0f, 10.0f);
        c.LensDirtScatter = std::clamp(c.LensDirtScatter, 0.0f, 1.0f);

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::BloomEffect>(entity))
            return true;
        Components::BloomEffect c{};
        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::BloomEffect>(entity);
        return true;
    }
};

class CubeLutEffectSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "CubeLutEffect"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::CubeLutEffect>(entity);
        if (!c)
            return;
        outLines.push_back(std::string("CubeLutEffect.enabled = ") + (c->Enabled ? "true" : "false"));
        outLines.push_back(std::string("CubeLutEffect.stackOrder = ") + std::to_string(c->StackOrder));
        outLines.push_back(std::string("CubeLutEffect.intensity = ") + FormatFloat(c->Intensity));
        outLines.push_back(std::string("CubeLutEffect.inputEncoding = ") + std::to_string(c->InputEncoding));
        outLines.push_back(std::string("CubeLutEffect.textureFormat = ") + std::to_string(c->TextureFormat));
        // AssetRef stores only a GUID; a cleared slot has nothing to reference, so skip the
        // line rather than emit an empty [] that would round-trip back to a parse error.
        if (!c->LutAssetGuid.IsNull())
            outLines.push_back(std::string("CubeLutEffect.lutAsset = ") +
                               FormatAssetReferenceForSave(ctx, c->LutAssetGuid.ToGuid(), ""));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::CubeLutEffect c{};
        if (auto* existing = world.GetComponent<Components::CubeLutEffect>(entity))
            c = *existing;

        if (property == "enabled")
        {
            if (!ParseBool(value, c.Enabled, outError))
                return false;
        }
        else if (property == "stackorder")
        {
            if (!ParseI32(value, c.StackOrder, outError))
                return false;
        }
        else if (property == "intensity")
        {
            if (!ParseF32(value, c.Intensity, outError))
                return false;
        }
        else if (property == "inputencoding")
        {
            if (!ParseU32(value, c.InputEncoding, outError))
                return false;
        }
        else if (property == "textureformat")
        {
            if (!ParseU32(value, c.TextureFormat, outError))
                return false;
        }
        else if (property == "lutasset")
        {
            // Canonical form: [path="..." guid="..."]. The converter emits a path-only
            // ref (guid=""); the loader's cross-project path recovery resolves it to a GUID.
            SceneValue sv{};
            if (!ParseValue(value, sv, outError))
                return false;
            AssetReference ref{};
            std::string err;
            if (!TryResolveAssetReference(ctx, sv, AssetType::CubeLut, ref, &err))
            {
                if (outError)
                    *outError = err.empty() ? "CubeLutEffect.lutAsset: invalid reference" : err;
                return false;
            }
            c.LutAssetGuid.Set(ref.guid);
        }
        else
        {
            if (outError)
                *outError = "Unknown CubeLutEffect property";
            return false;
        }

        c.Intensity = std::clamp(c.Intensity, 0.0f, 1.0f);
        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::CubeLutEffect>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::CubeLutEffect{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::CubeLutEffect>(entity);
        return true;
    }
};

class HeightFogEffectSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "HeightFogEffect"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::HeightFogEffect>(entity);
        if (!c)
            return;
        outLines.push_back(std::string("HeightFogEffect.enabled = ") + (c->Enabled ? "true" : "false"));
        outLines.push_back(std::string("HeightFogEffect.preset = ") + std::to_string(static_cast<int32>(c->Preset)));
        outLines.push_back(std::string("HeightFogEffect.intensity = ") + FormatFloat(c->Intensity));
        outLines.push_back(std::string("HeightFogEffect.density = ") + FormatFloat(c->Density));
        outLines.push_back(std::string("HeightFogEffect.maxOpacity = ") + FormatFloat(c->MaxOpacity));
        outLines.push_back(std::string("HeightFogEffect.distanceFogEnabled = ") + (c->DistanceFogEnabled ? "true" : "false"));
        outLines.push_back(std::string("HeightFogEffect.minDistance = ") + FormatFloat(c->MinDistance));
        outLines.push_back(std::string("HeightFogEffect.smoothLength = ") + FormatFloat(c->SmoothLength));
        outLines.push_back(std::string("HeightFogEffect.maxDistance = ") + FormatFloat(c->MaxDistance));
        outLines.push_back(std::string("HeightFogEffect.layerMode = ") + std::to_string(static_cast<int32>(c->LayerMode)));
        outLines.push_back(std::string("HeightFogEffect.heightFogEnabled = ") + (c->HeightFogEnabled ? "true" : "false"));
        outLines.push_back(std::string("HeightFogEffect.baseHeight = ") + FormatFloat(c->BaseHeight));
        outLines.push_back(std::string("HeightFogEffect.transitionLength = ") + FormatFloat(c->TransitionLength));
        outLines.push_back(std::string("HeightFogEffect.horizonHeightOffset = ") + FormatFloat(c->HorizonHeightOffset));
        outLines.push_back(std::string("HeightFogEffect.horizonHeightBlendStart = ") + FormatFloat(c->HorizonHeightBlendStart));
        outLines.push_back(std::string("HeightFogEffect.horizonHeightBlendEnd = ") + FormatFloat(c->HorizonHeightBlendEnd));
        outLines.push_back(std::string("HeightFogEffect.axisMode = ") + std::to_string(static_cast<int32>(c->AxisMode)));
        outLines.push_back(std::string("HeightFogEffect.customAxis = ") + FormatFloat3(c->CustomAxis[0], c->CustomAxis[1], c->CustomAxis[2]));
        outLines.push_back(std::string("HeightFogEffect.emissive = ") + FormatFloat3(c->Emissive[0], c->Emissive[1], c->Emissive[2]));
        outLines.push_back(std::string("HeightFogEffect.gradientMode = ") + std::to_string(static_cast<int32>(c->GradientMode)));
        outLines.push_back(std::string("HeightFogEffect.gradientStrength = ") + FormatFloat(c->GradientStrength));
        outLines.push_back(std::string("HeightFogEffect.gradientLowColor = ") + FormatFloat3(c->GradientLowColor[0], c->GradientLowColor[1], c->GradientLowColor[2]));
        outLines.push_back(std::string("HeightFogEffect.gradientHighColor = ") + FormatFloat3(c->GradientHighColor[0], c->GradientHighColor[1], c->GradientHighColor[2]));
        outLines.push_back(std::string("HeightFogEffect.trackDirectionalLight = ") + (c->TrackDirectionalLight ? "true" : "false"));
        outLines.push_back(std::string("HeightFogEffect.sunDirection = ") + FormatFloat3(c->SunDirection[0], c->SunDirection[1], c->SunDirection[2]));
        outLines.push_back(std::string("HeightFogEffect.sunColor = ") + FormatFloat3(c->SunColor[0], c->SunColor[1], c->SunColor[2]));
        outLines.push_back(std::string("HeightFogEffect.sunIntensity = ") + FormatFloat(c->SunIntensity));
        outLines.push_back(std::string("HeightFogEffect.sunIntensityScale = ") + FormatFloat(c->SunIntensityScale));
        outLines.push_back(std::string("HeightFogEffect.phase = ") + FormatFloat(c->Phase));
        outLines.push_back(std::string("HeightFogEffect.phaseWeight0 = ") + FormatFloat(c->PhaseWeight0));
        outLines.push_back(std::string("HeightFogEffect.phaseWeight1 = ") + FormatFloat(c->PhaseWeight1));
        outLines.push_back(std::string("HeightFogEffect.noiseEnabled = ") + (c->NoiseEnabled ? "true" : "false"));
        outLines.push_back(std::string("HeightFogEffect.noiseScale = ") + FormatFloat(c->NoiseScale));
        outLines.push_back(std::string("HeightFogEffect.noiseStrength = ") + FormatFloat(c->NoiseStrength));
        outLines.push_back(std::string("HeightFogEffect.noiseVelocity = ") + FormatFloat3(c->NoiseVelocity[0], c->NoiseVelocity[1], c->NoiseVelocity[2]));
        outLines.push_back(std::string("HeightFogEffect.noiseContrast = ") + FormatFloat(c->NoiseContrast));
        outLines.push_back(std::string("HeightFogEffect.noiseMin = ") + FormatFloat(c->NoiseMin));
        outLines.push_back(std::string("HeightFogEffect.noiseMax = ") + FormatFloat(c->NoiseMax));
        outLines.push_back(std::string("HeightFogEffect.noiseFadeStart = ") + FormatFloat(c->NoiseFadeStart));
        outLines.push_back(std::string("HeightFogEffect.noiseFadeEnd = ") + FormatFloat(c->NoiseFadeEnd));
        outLines.push_back(std::string("HeightFogEffect.useTimeOfDay = ") + (c->UseTimeOfDay ? "true" : "false"));
        outLines.push_back(std::string("HeightFogEffect.skyEnabled = ") + (c->SkyEnabled ? "true" : "false"));
        outLines.push_back(std::string("HeightFogEffect.skyPower = ") + FormatFloat(c->SkyPower));
        outLines.push_back(std::string("HeightFogEffect.skyFillStart = ") + FormatFloat(c->SkyFillStart));
        outLines.push_back(std::string("HeightFogEffect.skyFillEnd = ") + FormatFloat(c->SkyFillEnd));
        outLines.push_back(std::string("HeightFogEffect.skyHorizonOffset = ") + FormatFloat(c->SkyHorizonOffset));
        outLines.push_back(std::string("HeightFogEffect.skyBottomStrength = ") + FormatFloat(c->SkyBottomStrength));
        outLines.push_back(std::string("HeightFogEffect.fogGlowEnabled = ") + (c->FogGlowEnabled ? "true" : "false"));
        outLines.push_back(std::string("HeightFogEffect.fogGlowQuality = ") + std::to_string(static_cast<int32>(c->FogGlowQualityLevel)));
        outLines.push_back(std::string("HeightFogEffect.fogGlowIntensity = ") + FormatFloat(c->FogGlowIntensity));
        outLines.push_back(std::string("HeightFogEffect.fogGlowRadius = ") + FormatFloat(c->FogGlowRadius));
        outLines.push_back(std::string("HeightFogEffect.fogGlowOctaves = ") + std::to_string(c->FogGlowOctaves));
        outLines.push_back(std::string("HeightFogEffect.fogGlowScatter = ") + FormatFloat(c->FogGlowScatter));
        outLines.push_back(std::string("HeightFogEffect.fogGlowThreshold = ") + FormatFloat(c->FogGlowThreshold));
        outLines.push_back(std::string("HeightFogEffect.fogGlowKnee = ") + FormatFloat(c->FogGlowKnee));
        outLines.push_back(std::string("HeightFogEffect.fogGlowFadeStart = ") + FormatFloat(c->FogGlowFadeStart));
        outLines.push_back(std::string("HeightFogEffect.fogGlowFadeEnd = ") + FormatFloat(c->FogGlowFadeEnd));
        outLines.push_back(std::string("HeightFogEffect.fogGlowTint = ") + FormatFloat3(c->FogGlowTint[0], c->FogGlowTint[1], c->FogGlowTint[2]));
        outLines.push_back(std::string("HeightFogEffect.fogGlowAntiFlicker = ") + (c->FogGlowAntiFlicker ? "true" : "false"));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::HeightFogEffect c{};
        if (auto* existing = world.GetComponent<Components::HeightFogEffect>(entity))
            c = *existing;

        // SceneIO lowercases property names before ApplyProperty; keep comparisons lowercase.
        if (property == "enabled")
        {
            if (!ParseBool(value, c.Enabled, outError))
                return false;
        }
        else if (property == "preset")
        {
            int32 v = static_cast<int32>(c.Preset);
            if (!ParseI32(value, v, outError))
                return false;
            c.Preset = static_cast<Components::HeightFogPreset>(v);
        }
        else if (property == "intensity")
        {
            if (!ParseF32(value, c.Intensity, outError))
                return false;
        }
        else if (property == "density")
        {
            if (!ParseF32(value, c.Density, outError))
                return false;
        }
        else if (property == "maxopacity")
        {
            if (!ParseF32(value, c.MaxOpacity, outError))
                return false;
        }
        else if (property == "distancefogenabled")
        {
            if (!ParseBool(value, c.DistanceFogEnabled, outError))
                return false;
        }
        else if (property == "mindistance")
        {
            if (!ParseF32(value, c.MinDistance, outError))
                return false;
        }
        else if (property == "smoothlength")
        {
            if (!ParseF32(value, c.SmoothLength, outError))
                return false;
        }
        else if (property == "maxdistance")
        {
            if (!ParseF32(value, c.MaxDistance, outError))
                return false;
        }
        else if (property == "layermode")
        {
            int32 v = static_cast<int32>(c.LayerMode);
            if (!ParseI32(value, v, outError))
                return false;
            c.LayerMode = static_cast<Components::HeightFogLayerMode>(v);
        }
        else if (property == "heightfogenabled")
        {
            if (!ParseBool(value, c.HeightFogEnabled, outError))
                return false;
        }
        else if (property == "baseheight")
        {
            if (!ParseF32(value, c.BaseHeight, outError))
                return false;
        }
        else if (property == "transitionlength")
        {
            if (!ParseF32(value, c.TransitionLength, outError))
                return false;
        }
        else if (property == "horizonheightoffset")
        {
            if (!ParseF32(value, c.HorizonHeightOffset, outError))
                return false;
        }
        else if (property == "horizonheightblendstart")
        {
            if (!ParseF32(value, c.HorizonHeightBlendStart, outError))
                return false;
        }
        else if (property == "horizonheightblendend")
        {
            if (!ParseF32(value, c.HorizonHeightBlendEnd, outError))
                return false;
        }
        else if (property == "axismode")
        {
            int32 v = static_cast<int32>(c.AxisMode);
            if (!ParseI32(value, v, outError))
                return false;
            c.AxisMode = static_cast<Components::HeightFogAxisMode>(v);
        }
        else if (property == "customaxis")
        {
            Float3 v{c.CustomAxis[0], c.CustomAxis[1], c.CustomAxis[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "HeightFogEffect.customAxis must be (x, y, z)";
                return false;
            }
            c.CustomAxis[0] = v.X;
            c.CustomAxis[1] = v.Y;
            c.CustomAxis[2] = v.Z;
        }
        else if (property == "emissive")
        {
            Float3 v{c.Emissive[0], c.Emissive[1], c.Emissive[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "HeightFogEffect.emissive must be (r, g, b)";
                return false;
            }
            c.Emissive[0] = v.X;
            c.Emissive[1] = v.Y;
            c.Emissive[2] = v.Z;
        }
        else if (property == "gradientmode")
        {
            int32 v = static_cast<int32>(c.GradientMode);
            if (!ParseI32(value, v, outError))
                return false;
            c.GradientMode = static_cast<Components::HeightFogGradientMode>(v);
        }
        else if (property == "gradientstrength")
        {
            if (!ParseF32(value, c.GradientStrength, outError))
                return false;
        }
        else if (property == "gradientlowcolor")
        {
            Float3 v{c.GradientLowColor[0], c.GradientLowColor[1], c.GradientLowColor[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "HeightFogEffect.gradientLowColor must be (r, g, b)";
                return false;
            }
            c.GradientLowColor[0] = v.X;
            c.GradientLowColor[1] = v.Y;
            c.GradientLowColor[2] = v.Z;
        }
        else if (property == "gradienthighcolor")
        {
            Float3 v{c.GradientHighColor[0], c.GradientHighColor[1], c.GradientHighColor[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "HeightFogEffect.gradientHighColor must be (r, g, b)";
                return false;
            }
            c.GradientHighColor[0] = v.X;
            c.GradientHighColor[1] = v.Y;
            c.GradientHighColor[2] = v.Z;
        }
        else if (property == "trackdirectionallight")
        {
            if (!ParseBool(value, c.TrackDirectionalLight, outError))
                return false;
        }
        else if (property == "sundirection")
        {
            Float3 v{c.SunDirection[0], c.SunDirection[1], c.SunDirection[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "HeightFogEffect.sunDirection must be (x, y, z)";
                return false;
            }
            c.SunDirection[0] = v.X;
            c.SunDirection[1] = v.Y;
            c.SunDirection[2] = v.Z;
        }
        else if (property == "suncolor")
        {
            Float3 v{c.SunColor[0], c.SunColor[1], c.SunColor[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "HeightFogEffect.sunColor must be (r, g, b)";
                return false;
            }
            c.SunColor[0] = v.X;
            c.SunColor[1] = v.Y;
            c.SunColor[2] = v.Z;
        }
        else if (property == "sunintensity")
        {
            if (!ParseF32(value, c.SunIntensity, outError))
                return false;
        }
        else if (property == "sunintensityscale")
        {
            if (!ParseF32(value, c.SunIntensityScale, outError))
                return false;
        }
        else if (property == "phase")
        {
            if (!ParseF32(value, c.Phase, outError))
                return false;
        }
        else if (property == "phaseweight0")
        {
            if (!ParseF32(value, c.PhaseWeight0, outError))
                return false;
        }
        else if (property == "phaseweight1")
        {
            if (!ParseF32(value, c.PhaseWeight1, outError))
                return false;
        }
        else if (property == "noiseenabled")
        {
            if (!ParseBool(value, c.NoiseEnabled, outError))
                return false;
        }
        else if (property == "noisescale")
        {
            if (!ParseF32(value, c.NoiseScale, outError))
                return false;
        }
        else if (property == "noisestrength")
        {
            if (!ParseF32(value, c.NoiseStrength, outError))
                return false;
        }
        else if (property == "noisevelocity")
        {
            Float3 v{c.NoiseVelocity[0], c.NoiseVelocity[1], c.NoiseVelocity[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "HeightFogEffect.noiseVelocity must be (x, y, z)";
                return false;
            }
            c.NoiseVelocity[0] = v.X;
            c.NoiseVelocity[1] = v.Y;
            c.NoiseVelocity[2] = v.Z;
        }
        else if (property == "noisecontrast")
        {
            if (!ParseF32(value, c.NoiseContrast, outError))
                return false;
        }
        else if (property == "noisemin")
        {
            if (!ParseF32(value, c.NoiseMin, outError))
                return false;
        }
        else if (property == "noisemax")
        {
            if (!ParseF32(value, c.NoiseMax, outError))
                return false;
        }
        else if (property == "noisefadestart")
        {
            if (!ParseF32(value, c.NoiseFadeStart, outError))
                return false;
        }
        else if (property == "noisefadeend")
        {
            if (!ParseF32(value, c.NoiseFadeEnd, outError))
                return false;
        }
        else if (property == "usetimeofday")
        {
            if (!ParseBool(value, c.UseTimeOfDay, outError))
                return false;
        }
        else if (property == "skyenabled")
        {
            if (!ParseBool(value, c.SkyEnabled, outError))
                return false;
        }
        else if (property == "skypower")
        {
            if (!ParseF32(value, c.SkyPower, outError))
                return false;
        }
        else if (property == "skyfillstart")
        {
            if (!ParseF32(value, c.SkyFillStart, outError))
                return false;
        }
        else if (property == "skyfillend")
        {
            if (!ParseF32(value, c.SkyFillEnd, outError))
                return false;
        }
        else if (property == "skyhorizonoffset")
        {
            if (!ParseF32(value, c.SkyHorizonOffset, outError))
                return false;
        }
        else if (property == "skybottomstrength")
        {
            if (!ParseF32(value, c.SkyBottomStrength, outError))
                return false;
        }
        else if (property == "fogglowenabled")
        {
            if (!ParseBool(value, c.FogGlowEnabled, outError)) return false;
        }
        else if (property == "fogglowquality")
        {
            int32 v = static_cast<int32>(c.FogGlowQualityLevel);
            if (!ParseI32(value, v, outError)) return false;
            c.FogGlowQualityLevel = static_cast<Components::FogGlowQuality>(std::clamp(v, 0, 3));
        }
        else if (property == "fogglowintensity")
        {
            if (!ParseF32(value, c.FogGlowIntensity, outError)) return false;
        }
        else if (property == "fogglowradius")
        {
            if (!ParseF32(value, c.FogGlowRadius, outError)) return false;
        }
        else if (property == "fogglowoctaves")
        {
            if (!ParseI32(value, c.FogGlowOctaves, outError)) return false;
        }
        else if (property == "fogglowscatter")
        {
            if (!ParseF32(value, c.FogGlowScatter, outError)) return false;
        }
        else if (property == "fogglowthreshold")
        {
            if (!ParseF32(value, c.FogGlowThreshold, outError)) return false;
        }
        else if (property == "fogglowknee")
        {
            if (!ParseF32(value, c.FogGlowKnee, outError)) return false;
        }
        else if (property == "fogglowfadestart")
        {
            if (!ParseF32(value, c.FogGlowFadeStart, outError)) return false;
        }
        else if (property == "fogglowfadeend")
        {
            if (!ParseF32(value, c.FogGlowFadeEnd, outError)) return false;
        }
        else if (property == "fogglowtint")
        {
            Float3 v{c.FogGlowTint[0], c.FogGlowTint[1], c.FogGlowTint[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError) *outError = "HeightFogEffect.fogGlowTint must be (r, g, b)";
                return false;
            }
            c.FogGlowTint[0] = v.X; c.FogGlowTint[1] = v.Y; c.FogGlowTint[2] = v.Z;
        }
        else if (property == "fogglowantiflicker")
        {
            if (!ParseBool(value, c.FogGlowAntiFlicker, outError)) return false;
        }
        else
        {
            if (outError)
                *outError = "Unknown HeightFogEffect property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::HeightFogEffect>(entity))
            return true;
        Components::HeightFogEffect c{};
        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::HeightFogEffect>(entity);
        return true;
    }
};

class AtmosphericCloudLayerSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "AtmosphericCloudLayer"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::AtmosphericCloudLayer>(entity);
        if (!c)
            return;
        outLines.push_back(std::string("AtmosphericCloudLayer.enabled = ") + (c->Enabled ? "true" : "false"));
        outLines.push_back(std::string("AtmosphericCloudLayer.skyFill = ") + FormatFloat(c->SkyFill));
        outLines.push_back(std::string("AtmosphericCloudLayer.vaporMass = ") + FormatFloat(c->VaporMass));
        outLines.push_back(std::string("AtmosphericCloudLayer.cloudColor = ") + FormatFloat3(c->CloudColor[0], c->CloudColor[1], c->CloudColor[2]));
        outLines.push_back(std::string("AtmosphericCloudLayer.opacity = ") + FormatFloat(c->Opacity));
        outLines.push_back(std::string("AtmosphericCloudLayer.floorHeight = ") + FormatFloat(c->FloorHeight));
        outLines.push_back(std::string("AtmosphericCloudLayer.layerDepth = ") + FormatFloat(c->LayerDepth));
        outLines.push_back(std::string("AtmosphericCloudLayer.bodyFrequency = ") + FormatFloat(c->BodyFrequency));
        outLines.push_back(std::string("AtmosphericCloudLayer.edgeFrequency = ") + FormatFloat(c->EdgeFrequency));
        outLines.push_back(std::string("AtmosphericCloudLayer.edgeBreakup = ") + FormatFloat(c->EdgeBreakup));
        outLines.push_back(std::string("AtmosphericCloudLayer.driftAngle = ") + FormatFloat(c->DriftAngle));
        outLines.push_back(std::string("AtmosphericCloudLayer.driftRate = ") + FormatFloat(c->DriftRate));
        outLines.push_back(std::string("AtmosphericCloudLayer.sunFade = ") + FormatFloat(c->SunFade));
        outLines.push_back(std::string("AtmosphericCloudLayer.skyBounce = ") + FormatFloat(c->SkyBounce));
        outLines.push_back(std::string("AtmosphericCloudLayer.rimBoost = ") + FormatFloat(c->RimBoost));
        outLines.push_back(std::string("AtmosphericCloudLayer.occlusion = ") + FormatFloat(c->Occlusion));
        outLines.push_back(std::string("AtmosphericCloudLayer.historyWeight = ") + FormatFloat(c->HistoryWeight));
        outLines.push_back(std::string("AtmosphericCloudLayer.pixelScale = ") + FormatFloat(c->PixelScale));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::AtmosphericCloudLayer c{};
        if (auto* existing = world.GetComponent<Components::AtmosphericCloudLayer>(entity))
            c = *existing;

        if (property == "enabled")
        {
            if (!ParseBool(value, c.Enabled, outError))
                return false;
        }
        else if (property == "skyfill")
        {
            if (!ParseF32(value, c.SkyFill, outError))
                return false;
        }
        else if (property == "vapormass")
        {
            if (!ParseF32(value, c.VaporMass, outError))
                return false;
        }
        else if (property == "cloudcolor")
        {
            Float3 v;
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "AtmosphericCloudLayer.cloudColor must be (r, g, b)";
                return false;
            }
            c.CloudColor[0] = v.X;
            c.CloudColor[1] = v.Y;
            c.CloudColor[2] = v.Z;
        }
        else if (property == "opacity")
        {
            if (!ParseF32(value, c.Opacity, outError))
                return false;
        }
        else if (property == "floorheight")
        {
            if (!ParseF32(value, c.FloorHeight, outError))
                return false;
        }
        else if (property == "layerdepth")
        {
            if (!ParseF32(value, c.LayerDepth, outError))
                return false;
        }
        else if (property == "bodyfrequency")
        {
            if (!ParseF32(value, c.BodyFrequency, outError))
                return false;
        }
        else if (property == "edgefrequency")
        {
            if (!ParseF32(value, c.EdgeFrequency, outError))
                return false;
        }
        else if (property == "edgebreakup")
        {
            if (!ParseF32(value, c.EdgeBreakup, outError))
                return false;
        }
        else if (property == "driftangle")
        {
            if (!ParseF32(value, c.DriftAngle, outError))
                return false;
        }
        else if (property == "driftrate")
        {
            if (!ParseF32(value, c.DriftRate, outError))
                return false;
        }
        else if (property == "sunfade")
        {
            if (!ParseF32(value, c.SunFade, outError))
                return false;
        }
        else if (property == "skybounce")
        {
            if (!ParseF32(value, c.SkyBounce, outError))
                return false;
        }
        else if (property == "rimboost")
        {
            if (!ParseF32(value, c.RimBoost, outError))
                return false;
        }
        else if (property == "occlusion")
        {
            if (!ParseF32(value, c.Occlusion, outError))
                return false;
        }
        else if (property == "historyweight")
        {
            if (!ParseF32(value, c.HistoryWeight, outError))
                return false;
        }
        else if (property == "pixelscale")
        {
            if (!ParseF32(value, c.PixelScale, outError))
                return false;
        }
        else
        {
            if (outError)
                *outError = "Unknown AtmosphericCloudLayer property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::AtmosphericCloudLayer>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::AtmosphericCloudLayer{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::AtmosphericCloudLayer>(entity);
        return true;
    }
};

class VolumetricCloudsSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "VolumetricClouds"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::VolumetricClouds>(entity);
        if (!c)
            return;
        outLines.push_back(std::string("VolumetricClouds.enabled = ") + (c->Enabled ? "true" : "false"));
        outLines.push_back(std::string("VolumetricClouds.preset = ") + std::to_string(static_cast<int32>(c->Preset)));
        outLines.push_back(std::string("VolumetricClouds.radius = ") + FormatFloat(c->Radius));
        outLines.push_back(std::string("VolumetricClouds.altitude = ") + FormatFloat(c->Altitude));
        outLines.push_back(std::string("VolumetricClouds.thickness = ") + FormatFloat(c->Thickness));
        outLines.push_back(std::string("VolumetricClouds.numStepsLight = ") + std::to_string(c->NumStepsLight));
        outLines.push_back(std::string("VolumetricClouds.stepSize = ") + FormatFloat(c->StepSize));
        outLines.push_back(std::string("VolumetricClouds.rayOffsetStrength = ") + FormatFloat(c->RayOffsetStrength));
        outLines.push_back(std::string("VolumetricClouds.cloudScale = ") + FormatFloat(c->CloudScale));
        outLines.push_back(std::string("VolumetricClouds.densityMultiplier = ") + FormatFloat(c->DensityMultiplier));
        outLines.push_back(std::string("VolumetricClouds.densityOffset = ") + FormatFloat(c->DensityOffset));
        outLines.push_back(std::string("VolumetricClouds.shapeOffset = ") + FormatFloat3(c->ShapeOffset[0], c->ShapeOffset[1], c->ShapeOffset[2]));
        outLines.push_back(std::string("VolumetricClouds.shapeNoiseWeights = ") + FormatFloat4(c->ShapeNoiseWeights[0], c->ShapeNoiseWeights[1], c->ShapeNoiseWeights[2], c->ShapeNoiseWeights[3]));
        outLines.push_back(std::string("VolumetricClouds.detailNoiseScale = ") + FormatFloat(c->DetailNoiseScale));
        outLines.push_back(std::string("VolumetricClouds.detailNoiseWeight = ") + FormatFloat(c->DetailNoiseWeight));
        outLines.push_back(std::string("VolumetricClouds.detailNoiseWeights = ") + FormatFloat3(c->DetailNoiseWeights[0], c->DetailNoiseWeights[1], c->DetailNoiseWeights[2]));
        outLines.push_back(std::string("VolumetricClouds.detailOffset = ") + FormatFloat3(c->DetailOffset[0], c->DetailOffset[1], c->DetailOffset[2]));
        outLines.push_back(std::string("VolumetricClouds.lightAbsorptionThroughCloud = ") + FormatFloat(c->LightAbsorptionThroughCloud));
        outLines.push_back(std::string("VolumetricClouds.lightAbsorptionTowardSun = ") + FormatFloat(c->LightAbsorptionTowardSun));
        outLines.push_back(std::string("VolumetricClouds.darknessThreshold = ") + FormatFloat(c->DarknessThreshold));
        outLines.push_back(std::string("VolumetricClouds.forwardScattering = ") + FormatFloat(c->ForwardScattering));
        outLines.push_back(std::string("VolumetricClouds.backScattering = ") + FormatFloat(c->BackScattering));
        outLines.push_back(std::string("VolumetricClouds.baseBrightness = ") + FormatFloat(c->BaseBrightness));
        outLines.push_back(std::string("VolumetricClouds.phaseFactor = ") + FormatFloat(c->PhaseFactor));
        outLines.push_back(std::string("VolumetricClouds.timeScale = ") + FormatFloat(c->TimeScale));
        outLines.push_back(std::string("VolumetricClouds.baseSpeed = ") + FormatFloat(c->BaseSpeed));
        outLines.push_back(std::string("VolumetricClouds.detailSpeed = ") + FormatFloat(c->DetailSpeed));
        outLines.push_back(std::string("VolumetricClouds.historyWeight = ") + FormatFloat(c->HistoryWeight));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::VolumetricClouds c{};
        if (auto* existing = world.GetComponent<Components::VolumetricClouds>(entity))
            c = *existing;

        auto parseVec3 = [&](float32* dst, const char* errMsg) -> bool
        {
            Float3 v;
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = errMsg;
                return false;
            }
            dst[0] = v.X;
            dst[1] = v.Y;
            dst[2] = v.Z;
            return true;
        };

        if (property == "enabled")
        {
            if (!ParseBool(value, c.Enabled, outError))
                return false;
        }
        else if (property == "preset")
        {
            int32_t v = 0;
            if (!ParseI32(value, v, outError))
                return false;
            c.Preset = static_cast<Components::VolumetricCloudsPreset>(v);
        }
        else if (property == "radius")
        {
            if (!ParseF32(value, c.Radius, outError))
                return false;
        }
        else if (property == "altitude")
        {
            if (!ParseF32(value, c.Altitude, outError))
                return false;
        }
        else if (property == "thickness")
        {
            if (!ParseF32(value, c.Thickness, outError))
                return false;
        }
        else if (property == "numstepslight")
        {
            if (!ParseI32(value, c.NumStepsLight, outError))
                return false;
        }
        else if (property == "stepsize")
        {
            if (!ParseF32(value, c.StepSize, outError))
                return false;
        }
        else if (property == "rayoffsetstrength")
        {
            if (!ParseF32(value, c.RayOffsetStrength, outError))
                return false;
        }
        else if (property == "cloudscale")
        {
            if (!ParseF32(value, c.CloudScale, outError))
                return false;
        }
        else if (property == "densitymultiplier")
        {
            if (!ParseF32(value, c.DensityMultiplier, outError))
                return false;
        }
        else if (property == "densityoffset")
        {
            if (!ParseF32(value, c.DensityOffset, outError))
                return false;
        }
        else if (property == "shapeoffset")
        {
            if (!parseVec3(c.ShapeOffset, "VolumetricClouds.shapeOffset must be (x, y, z)"))
                return false;
        }
        else if (property == "shapenoiseweights")
        {
            Float4 v;
            if (!ParseFloat4(value, v))
            {
                if (outError)
                    *outError = "VolumetricClouds.shapeNoiseWeights must be (r, g, b, a)";
                return false;
            }
            c.ShapeNoiseWeights[0] = v.X;
            c.ShapeNoiseWeights[1] = v.Y;
            c.ShapeNoiseWeights[2] = v.Z;
            c.ShapeNoiseWeights[3] = v.W;
        }
        else if (property == "detailnoisescale")
        {
            if (!ParseF32(value, c.DetailNoiseScale, outError))
                return false;
        }
        else if (property == "detailnoiseweight")
        {
            if (!ParseF32(value, c.DetailNoiseWeight, outError))
                return false;
        }
        else if (property == "detailnoiseweights")
        {
            if (!parseVec3(c.DetailNoiseWeights, "VolumetricClouds.detailNoiseWeights must be (r, g, b)"))
                return false;
        }
        else if (property == "detailoffset")
        {
            if (!parseVec3(c.DetailOffset, "VolumetricClouds.detailOffset must be (x, y, z)"))
                return false;
        }
        else if (property == "lightabsorptionthroughcloud")
        {
            if (!ParseF32(value, c.LightAbsorptionThroughCloud, outError))
                return false;
        }
        else if (property == "lightabsorptiontowardsun")
        {
            if (!ParseF32(value, c.LightAbsorptionTowardSun, outError))
                return false;
        }
        else if (property == "darknessthreshold")
        {
            if (!ParseF32(value, c.DarknessThreshold, outError))
                return false;
        }
        else if (property == "forwardscattering")
        {
            if (!ParseF32(value, c.ForwardScattering, outError))
                return false;
        }
        else if (property == "backscattering")
        {
            if (!ParseF32(value, c.BackScattering, outError))
                return false;
        }
        else if (property == "basebrightness")
        {
            if (!ParseF32(value, c.BaseBrightness, outError))
                return false;
        }
        else if (property == "phasefactor")
        {
            if (!ParseF32(value, c.PhaseFactor, outError))
                return false;
        }
        else if (property == "timescale")
        {
            if (!ParseF32(value, c.TimeScale, outError))
                return false;
        }
        else if (property == "basespeed")
        {
            if (!ParseF32(value, c.BaseSpeed, outError))
                return false;
        }
        else if (property == "detailspeed")
        {
            if (!ParseF32(value, c.DetailSpeed, outError))
                return false;
        }
        else if (property == "historyweight")
        {
            if (!ParseF32(value, c.HistoryWeight, outError))
                return false;
        }
        else
        {
            if (outError)
                *outError = "Unknown VolumetricClouds property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::VolumetricClouds>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::VolumetricClouds{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::VolumetricClouds>(entity);
        return true;
    }
};

class VolumetricFogEffectSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "VolumetricFogEffect"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::VolumetricFogEffect>(entity);
        if (!c)
            return;
        outLines.push_back(std::string("VolumetricFogEffect.enabled = ") + (c->Enabled ? "true" : "false"));
        outLines.push_back(std::string("VolumetricFogEffect.intensity = ") + FormatFloat(c->Intensity));
        outLines.push_back(std::string("VolumetricFogEffect.maxDistance = ") + FormatFloat(c->MaxDistance));
        outLines.push_back(std::string("VolumetricFogEffect.xyCellSizePixels = ") + std::to_string(c->XYCellSizePixels));
        outLines.push_back(std::string("VolumetricFogEffect.zSliceCount = ") + std::to_string(c->ZSliceCount));
        outLines.push_back(std::string("VolumetricFogEffect.depthDistribution = ") + FormatFloat(c->DepthDistribution));
        outLines.push_back(std::string("VolumetricFogEffect.density = ") + FormatFloat(c->Density));
        outLines.push_back(std::string("VolumetricFogEffect.baseHeight = ") + FormatFloat(c->BaseHeight));
        outLines.push_back(std::string("VolumetricFogEffect.heightFalloff = ") + FormatFloat(c->HeightFalloff));
        outLines.push_back(std::string("VolumetricFogEffect.skyFade = ") + FormatFloat(c->SkyFade));
        outLines.push_back(std::string("VolumetricFogEffect.albedo = ") + FormatFloat3(c->Albedo[0], c->Albedo[1], c->Albedo[2]));
        outLines.push_back(std::string("VolumetricFogEffect.emission = ") + FormatFloat3(c->Emission[0], c->Emission[1], c->Emission[2]));
        outLines.push_back(std::string("VolumetricFogEffect.anisotropy = ") + FormatFloat(c->Anisotropy));
        outLines.push_back(std::string("VolumetricFogEffect.trackDirectionalLight = ") + (c->TrackDirectionalLight ? "true" : "false"));
        outLines.push_back(std::string("VolumetricFogEffect.sunIntensityScale = ") + FormatFloat(c->SunIntensityScale));
        outLines.push_back(std::string("VolumetricFogEffect.sunScatteringTint = ") + FormatFloat3(c->SunScatteringTint[0], c->SunScatteringTint[1], c->SunScatteringTint[2]));
        outLines.push_back(std::string("VolumetricFogEffect.ambientScatteringTint = ") + FormatFloat3(c->AmbientScatteringTint[0], c->AmbientScatteringTint[1], c->AmbientScatteringTint[2]));
        outLines.push_back(std::string("VolumetricFogEffect.noiseEnabled = ") + (c->NoiseEnabled ? "true" : "false"));
        outLines.push_back(std::string("VolumetricFogEffect.noiseScale = ") + FormatFloat(c->NoiseScale));
        outLines.push_back(std::string("VolumetricFogEffect.noiseStrength = ") + FormatFloat(c->NoiseStrength));
        outLines.push_back(std::string("VolumetricFogEffect.noiseVelocity = ") + FormatFloat3(c->NoiseVelocity[0], c->NoiseVelocity[1], c->NoiseVelocity[2]));
        outLines.push_back(std::string("VolumetricFogEffect.noiseContrast = ") + FormatFloat(c->NoiseContrast));
        outLines.push_back(std::string("VolumetricFogEffect.noiseChannelWeights = (") + FormatFloat(c->NoiseChannelWeights[0]) + ", " + FormatFloat(c->NoiseChannelWeights[1]) + ", " + FormatFloat(c->NoiseChannelWeights[2]) + ", " + FormatFloat(c->NoiseChannelWeights[3]) + ")");
        outLines.push_back(std::string("VolumetricFogEffect.densityThreshold = ") + FormatFloat(c->DensityThreshold));
        outLines.push_back(std::string("VolumetricFogEffect.densityThresholdSoftness = ") + FormatFloat(c->DensityThresholdSoftness));
        outLines.push_back(std::string("VolumetricFogEffect.densityMode = ") + std::to_string(static_cast<int32_t>(c->DensityMode)));
        outLines.push_back(std::string("VolumetricFogEffect.gradientMode = ") + std::to_string(static_cast<int32_t>(c->GradientMode)));
        outLines.push_back(std::string("VolumetricFogEffect.gradientStrength = ") + FormatFloat(c->GradientStrength));
        outLines.push_back(std::string("VolumetricFogEffect.gradientLowTint = ") + FormatFloat3(c->GradientLowTint[0], c->GradientLowTint[1], c->GradientLowTint[2]));
        outLines.push_back(std::string("VolumetricFogEffect.gradientHighTint = ") + FormatFloat3(c->GradientHighTint[0], c->GradientHighTint[1], c->GradientHighTint[2]));
        outLines.push_back(std::string("VolumetricFogEffect.temporalEnabled = ") + (c->TemporalEnabled ? "true" : "false"));
        outLines.push_back(std::string("VolumetricFogEffect.temporalBlend = ") + FormatFloat(c->TemporalBlend));
        outLines.push_back(std::string("VolumetricFogEffect.jitterStrength = ") + FormatFloat(c->JitterStrength));
        outLines.push_back(std::string("VolumetricFogEffect.jitterMotion = ") + (c->JitterMotion ? "true" : "false"));
        outLines.push_back(std::string("VolumetricFogEffect.compositeDepthBias = ") + FormatFloat(c->CompositeDepthBias));
        outLines.push_back(std::string("VolumetricFogEffect.shadowBias = ") + FormatFloat(c->ShadowBias));
        outLines.push_back(std::string("VolumetricFogEffect.fogGlowEnabled = ") + (c->FogGlowEnabled ? "true" : "false"));
        outLines.push_back(std::string("VolumetricFogEffect.fogGlowQuality = ") + std::to_string(static_cast<int32>(c->FogGlowQualityLevel)));
        outLines.push_back(std::string("VolumetricFogEffect.fogGlowIntensity = ") + FormatFloat(c->FogGlowIntensity));
        outLines.push_back(std::string("VolumetricFogEffect.fogGlowRadius = ") + FormatFloat(c->FogGlowRadius));
        outLines.push_back(std::string("VolumetricFogEffect.fogGlowOctaves = ") + std::to_string(c->FogGlowOctaves));
        outLines.push_back(std::string("VolumetricFogEffect.fogGlowScatter = ") + FormatFloat(c->FogGlowScatter));
        outLines.push_back(std::string("VolumetricFogEffect.fogGlowThreshold = ") + FormatFloat(c->FogGlowThreshold));
        outLines.push_back(std::string("VolumetricFogEffect.fogGlowKnee = ") + FormatFloat(c->FogGlowKnee));
        outLines.push_back(std::string("VolumetricFogEffect.fogGlowFadeStart = ") + FormatFloat(c->FogGlowFadeStart));
        outLines.push_back(std::string("VolumetricFogEffect.fogGlowFadeEnd = ") + FormatFloat(c->FogGlowFadeEnd));
        outLines.push_back(std::string("VolumetricFogEffect.fogGlowTint = ") + FormatFloat3(c->FogGlowTint[0], c->FogGlowTint[1], c->FogGlowTint[2]));
        outLines.push_back(std::string("VolumetricFogEffect.fogGlowAntiFlicker = ") + (c->FogGlowAntiFlicker ? "true" : "false"));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::VolumetricFogEffect c{};
        if (auto* existing = world.GetComponent<Components::VolumetricFogEffect>(entity))
            c = *existing;

        if (property == "enabled")
        {
            if (!ParseBool(value, c.Enabled, outError))
                return false;
        }
        else if (property == "intensity")
        {
            if (!ParseF32(value, c.Intensity, outError))
                return false;
        }
        else if (property == "maxdistance")
        {
            if (!ParseF32(value, c.MaxDistance, outError))
                return false;
        }
        else if (property == "xycellsizepixels")
        {
            if (!ParseI32(value, c.XYCellSizePixels, outError))
                return false;
        }
        else if (property == "zslicecount")
        {
            if (!ParseI32(value, c.ZSliceCount, outError))
                return false;
        }
        else if (property == "depthdistribution")
        {
            if (!ParseF32(value, c.DepthDistribution, outError))
                return false;
        }
        else if (property == "density")
        {
            if (!ParseF32(value, c.Density, outError))
                return false;
        }
        else if (property == "baseheight")
        {
            if (!ParseF32(value, c.BaseHeight, outError))
                return false;
        }
        else if (property == "heightfalloff")
        {
            if (!ParseF32(value, c.HeightFalloff, outError))
                return false;
        }
        else if (property == "skyfade")
        {
            if (!ParseF32(value, c.SkyFade, outError))
                return false;
        }
        else if (property == "albedo")
        {
            Float3 v{c.Albedo[0], c.Albedo[1], c.Albedo[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "VolumetricFogEffect.albedo must be (r, g, b)";
                return false;
            }
            c.Albedo[0] = v.X;
            c.Albedo[1] = v.Y;
            c.Albedo[2] = v.Z;
        }
        else if (property == "emission")
        {
            Float3 v{c.Emission[0], c.Emission[1], c.Emission[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "VolumetricFogEffect.emission must be (r, g, b)";
                return false;
            }
            c.Emission[0] = v.X;
            c.Emission[1] = v.Y;
            c.Emission[2] = v.Z;
        }
        else if (property == "anisotropy")
        {
            if (!ParseF32(value, c.Anisotropy, outError))
                return false;
        }
        else if (property == "trackdirectionallight")
        {
            if (!ParseBool(value, c.TrackDirectionalLight, outError))
                return false;
        }
        else if (property == "sunintensityscale")
        {
            if (!ParseF32(value, c.SunIntensityScale, outError))
                return false;
        }
        else if (property == "sunscatteringtint")
        {
            Float3 v{c.SunScatteringTint[0], c.SunScatteringTint[1], c.SunScatteringTint[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "VolumetricFogEffect.sunScatteringTint must be (r, g, b)";
                return false;
            }
            c.SunScatteringTint[0] = v.X;
            c.SunScatteringTint[1] = v.Y;
            c.SunScatteringTint[2] = v.Z;
        }
        else if (property == "ambientscatteringtint")
        {
            Float3 v{c.AmbientScatteringTint[0], c.AmbientScatteringTint[1], c.AmbientScatteringTint[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "VolumetricFogEffect.ambientScatteringTint must be (r, g, b)";
                return false;
            }
            c.AmbientScatteringTint[0] = v.X;
            c.AmbientScatteringTint[1] = v.Y;
            c.AmbientScatteringTint[2] = v.Z;
        }
        else if (property == "noiseenabled")
        {
            if (!ParseBool(value, c.NoiseEnabled, outError))
                return false;
        }
        else if (property == "noisescale")
        {
            if (!ParseF32(value, c.NoiseScale, outError))
                return false;
        }
        else if (property == "noisestrength")
        {
            if (!ParseF32(value, c.NoiseStrength, outError))
                return false;
        }
        else if (property == "noisevelocity")
        {
            Float3 v{c.NoiseVelocity[0], c.NoiseVelocity[1], c.NoiseVelocity[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "VolumetricFogEffect.noiseVelocity must be (x, y, z)";
                return false;
            }
            c.NoiseVelocity[0] = v.X;
            c.NoiseVelocity[1] = v.Y;
            c.NoiseVelocity[2] = v.Z;
        }
        else if (property == "noisecontrast")
        {
            if (!ParseF32(value, c.NoiseContrast, outError))
                return false;
        }
        else if (property == "noisechannelweights")
        {
            Float4 v{c.NoiseChannelWeights[0], c.NoiseChannelWeights[1], c.NoiseChannelWeights[2], c.NoiseChannelWeights[3]};
            if (!ParseFloat4(value, v))
            {
                if (outError)
                    *outError = "VolumetricFogEffect.noiseChannelWeights must be (r, g, b, a)";
                return false;
            }
            c.NoiseChannelWeights[0] = v.X;
            c.NoiseChannelWeights[1] = v.Y;
            c.NoiseChannelWeights[2] = v.Z;
            c.NoiseChannelWeights[3] = v.W;
        }
        else if (property == "densitythreshold")
        {
            if (!ParseF32(value, c.DensityThreshold, outError))
                return false;
        }
        else if (property == "densitythresholdsoftness")
        {
            if (!ParseF32(value, c.DensityThresholdSoftness, outError))
                return false;
        }
        else if (property == "densitymode")
        {
            uint32_t v = 0;
            if (!ParseU32(value, v, outError))
                return false;
            if (v > 2)
            {
                if (outError)
                    *outError = "VolumetricFogEffect.densityMode must be 0-2";
                return false;
            }
            c.DensityMode = static_cast<Components::VolumetricFogDensityMode>(v);
        }
        else if (property == "gradientmode")
        {
            uint32_t v = 0;
            if (!ParseU32(value, v, outError))
                return false;
            if (v > 5)
            {
                if (outError)
                    *outError = "VolumetricFogEffect.gradientMode must be 0-5";
                return false;
            }
            c.GradientMode = static_cast<Components::VolumetricFogGradientMode>(v);
        }
        else if (property == "gradientstrength")
        {
            if (!ParseF32(value, c.GradientStrength, outError))
                return false;
        }
        else if (property == "gradientlowtint")
        {
            Float3 v{c.GradientLowTint[0], c.GradientLowTint[1], c.GradientLowTint[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "VolumetricFogEffect.gradientLowTint must be (r, g, b)";
                return false;
            }
            c.GradientLowTint[0] = v.X;
            c.GradientLowTint[1] = v.Y;
            c.GradientLowTint[2] = v.Z;
        }
        else if (property == "gradienthightint")
        {
            Float3 v{c.GradientHighTint[0], c.GradientHighTint[1], c.GradientHighTint[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "VolumetricFogEffect.gradientHighTint must be (r, g, b)";
                return false;
            }
            c.GradientHighTint[0] = v.X;
            c.GradientHighTint[1] = v.Y;
            c.GradientHighTint[2] = v.Z;
        }
        else if (property == "temporalenabled")
        {
            if (!ParseBool(value, c.TemporalEnabled, outError))
                return false;
        }
        else if (property == "temporalblend")
        {
            if (!ParseF32(value, c.TemporalBlend, outError))
                return false;
        }
        else if (property == "jitterstrength")
        {
            if (!ParseF32(value, c.JitterStrength, outError))
                return false;
        }
        else if (property == "jittermotion")
        {
            if (!ParseBool(value, c.JitterMotion, outError))
                return false;
        }
        else if (property == "compositedepthbias")
        {
            if (!ParseF32(value, c.CompositeDepthBias, outError))
                return false;
        }
        else if (property == "shadowbias")
        {
            if (!ParseF32(value, c.ShadowBias, outError))
                return false;
        }
        else if (property == "fogglowenabled")
        {
            if (!ParseBool(value, c.FogGlowEnabled, outError))
                return false;
        }
        else if (property == "fogglowquality")
        {
            int32 v = static_cast<int32>(c.FogGlowQualityLevel);
            if (!ParseI32(value, v, outError)) return false;
            c.FogGlowQualityLevel = static_cast<Components::FogGlowQuality>(std::clamp(v, 0, 3));
        }
        else if (property == "fogglowintensity")
        {
            if (!ParseF32(value, c.FogGlowIntensity, outError))
                return false;
        }
        else if (property == "fogglowradius")
        {
            if (!ParseF32(value, c.FogGlowRadius, outError))
                return false;
        }
        else if (property == "fogglowoctaves")
        {
            if (!ParseI32(value, c.FogGlowOctaves, outError)) return false;
        }
        else if (property == "fogglowscatter")
        {
            if (!ParseF32(value, c.FogGlowScatter, outError)) return false;
        }
        else if (property == "fogglowthreshold")
        {
            if (!ParseF32(value, c.FogGlowThreshold, outError)) return false;
        }
        else if (property == "fogglowknee")
        {
            if (!ParseF32(value, c.FogGlowKnee, outError)) return false;
        }
        else if (property == "fogglowfadestart")
        {
            if (!ParseF32(value, c.FogGlowFadeStart, outError)) return false;
        }
        else if (property == "fogglowfadeend")
        {
            if (!ParseF32(value, c.FogGlowFadeEnd, outError)) return false;
        }
        else if (property == "fogglowtint")
        {
            Float3 v{c.FogGlowTint[0], c.FogGlowTint[1], c.FogGlowTint[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError) *outError = "VolumetricFogEffect.fogGlowTint must be (r, g, b)";
                return false;
            }
            c.FogGlowTint[0] = v.X; c.FogGlowTint[1] = v.Y; c.FogGlowTint[2] = v.Z;
        }
        else if (property == "fogglowantiflicker")
        {
            if (!ParseBool(value, c.FogGlowAntiFlicker, outError)) return false;
        }
        else if (property == "heathazeenabled" || property == "heathazestrength" ||
                 property == "heathazespeed" || property == "heathazescale" ||
                 property == "heathazemaskstrength")
        {
            Components::HeatDistortionEffect heat{};
            if (auto* existing = world.GetComponent<Components::HeatDistortionEffect>(entity))
                heat = *existing;

            if (property == "heathazeenabled")
            {
                if (!ParseBool(value, heat.Enabled, outError))
                    return false;
            }
            else if (property == "heathazestrength")
            {
                if (!ParseF32(value, heat.Strength, outError))
                    return false;
                heat.Strength = std::max(heat.Strength, 0.0f);
            }
            else if (property == "heathazespeed")
            {
                if (!ParseF32(value, heat.Speed, outError))
                    return false;
            }
            else if (property == "heathazescale")
            {
                if (!ParseF32(value, heat.Scale, outError))
                    return false;
                heat.Scale = std::max(heat.Scale, 0.01f);
            }
            else
            {
                if (!ParseF32(value, heat.MaskStrength, outError))
                    return false;
                heat.MaskStrength = std::max(heat.MaskStrength, 0.0f);
            }

            world.AddComponentImmediate(entity, heat);
        }
        else
        {
            if (outError)
                *outError = "Unknown VolumetricFogEffect property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::VolumetricFogEffect>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::VolumetricFogEffect{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::VolumetricFogEffect>(entity);
        return true;
    }
};


class SkinnedMeshRendererSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "SkinnedMeshRenderer"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::SkinnedMeshRenderer>(entity);
        if (!c)
            return;
        outLines.push_back(std::string("SkinnedMeshRenderer.meshId = ") + std::to_string(c->meshId));
        outLines.push_back(std::string("SkinnedMeshRenderer.renderLayerMask = ") + std::to_string(c->renderLayerMask));
        outLines.push_back(std::string("SkinnedMeshRenderer.castShadows = ") + (c->castShadows ? "true" : "false"));
        outLines.push_back(std::string("SkinnedMeshRenderer.receiveShadows = ") + (c->receiveShadows ? "true" : "false"));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::SkinnedMeshRenderer c{};
        if (auto* existing = world.GetComponent<Components::SkinnedMeshRenderer>(entity))
            c = *existing;

        if (property == "meshid")
        {
            if (!ParseU32(value, c.meshId, outError))
                return false;
        }
        else if (property == "materialid")
        {
            uint32_t v = 0;
            if (!ParseU32(value, v, outError))
                return false; /* legacy field, dropped on load */
        }
        else if (property == "skeletonid")
        {
            // Older scenes wrote a process-local SkeletonStore index. Consume
            // the field for compatibility; the model source owns reconstruction.
            uint32_t ignored = 0;
            if (!ParseU32(value, ignored, outError))
                return false;
        }
        else if (property == "renderlayermask")
        {
            if (!ParseU32(value, c.renderLayerMask, outError))
                return false;
        }
        else if (property == "castshadows")
        {
            if (!ParseBool(value, c.castShadows, outError))
                return false;
        }
        else if (property == "receiveshadows")
        {
            if (!ParseBool(value, c.receiveShadows, outError))
                return false;
        }
        else
        {
            if (outError)
                *outError = "Unknown SkinnedMeshRenderer property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::SkinnedMeshRenderer>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::SkinnedMeshRenderer{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::SkinnedMeshRenderer>(entity);
        return true;
    }
};

class AudioListenerSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "AudioListener"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::AudioListener>(entity);
        if (!c)
            return;
        outLines.push_back(std::string("AudioListener.listenerIndex = ") + std::to_string(c->listenerIndex));
        outLines.push_back(std::string("AudioListener.worldId = ") + std::to_string(c->worldId));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::AudioListener c{};
        if (auto* existing = world.GetComponent<Components::AudioListener>(entity))
            c = *existing;

        if (property == "listenerindex")
        {
            if (!ParseU8(value, c.listenerIndex, outError))
                return false;
        }
        else if (property == "worldid")
        {
            uint32_t v = 0;
            if (!ParseU32(value, v, outError))
                return false;
            if (v > 0xFFFFu)
            {
                if (outError)
                    *outError = "AudioListener.worldId out of range";
                return false;
            }
            c.worldId = static_cast<uint16>(v);
        }
        else
        {
            if (outError)
                *outError = "Unknown AudioListener property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::AudioListener>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::AudioListener{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::AudioListener>(entity);
        return true;
    }
};

class LODGroupSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "LODGroup"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::LODGroup>(entity);
        if (!c)
            return;
        outLines.push_back(std::string("LODGroup.bias = ") + FormatFloat(c->Bias));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::LODGroup c{};
        if (auto* existing = world.GetComponent<Components::LODGroup>(entity))
            c = *existing;

        if (property == "bias")
        {
            if (!ParseF32(value, c.Bias, outError))
                return false;
        }
        else if (property == "lodcount" || property.rfind("threshold", 0) == 0)
        {
            // Legacy fields (pre-2026-07 scenes): accepted and ignored. Per-mesh
            // thresholds live on the GPUMesh row (MeshGPURegistry), not per entity.
        }
        else
        {
            if (outError)
                *outError = "Unknown LODGroup property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::LODGroup>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::LODGroup{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::LODGroup>(entity);
        return true;
    }
};

class RenderLayerSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "RenderLayer"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::RenderLayer>(entity);
        if (!c)
            return;
        outLines.push_back(std::string("RenderLayer.mask = ") + std::to_string(c->mask));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::RenderLayer c{};
        if (auto* existing = world.GetComponent<Components::RenderLayer>(entity))
            c = *existing;

        if (property == "mask")
        {
            if (!ParseU32(value, c.mask, outError))
                return false;
        }
        else
        {
            if (outError)
                *outError = "Unknown RenderLayer property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::RenderLayer>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::RenderLayer{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::RenderLayer>(entity);
        return true;
    }
};

class VideoTextureSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "VideoTextureComponent"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::VideoTextureComponent>(entity);
        if (!c)
            return;
        outLines.push_back(std::string("VideoTextureComponent.videoPath = ") + FormatQuoted(c->VideoPath()));
        outLines.push_back(std::string("VideoTextureComponent.uniformName = ") + FormatQuoted(c->UniformName()));
        outLines.push_back(std::string("VideoTextureComponent.loop = ") + (c->loop ? "true" : "false"));
        outLines.push_back(std::string("VideoTextureComponent.playOnStart = ") + (c->playOnStart ? "true" : "false"));
        outLines.push_back(std::string("VideoTextureComponent.playbackSpeed = ") + FormatFloat(c->playbackSpeed));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::VideoTextureComponent c{};
        if (auto* existing = world.GetComponent<Components::VideoTextureComponent>(entity))
            c = *existing;

        auto copyString = [](char* dst, size_t dstSize, const std::string& src)
        {
            std::memset(dst, 0, dstSize);
            const size_t toCopy = std::min(dstSize - 1, src.size());
            std::memcpy(dst, src.data(), toCopy);
            dst[toCopy] = '\0';
        };

        if (property == "videopath")
        {
            std::string s;
            if (!ParseQuotedString(value, s))
            {
                if (outError)
                    *outError = "VideoTextureComponent.videoPath must be a quoted string";
                return false;
            }
            copyString(c.videoPath, sizeof(c.videoPath), s);
        }
        else if (property == "uniformname")
        {
            std::string s;
            if (!ParseQuotedString(value, s))
            {
                if (outError)
                    *outError = "VideoTextureComponent.uniformName must be a quoted string";
                return false;
            }
            // Scenes saved before the component defaulted to a real material
            // slot name carry "uBaseColor", which no slot resolver recognises.
            // Migrate to the canonical base-color slot on load; the next save
            // writes the migrated name.
            if (s == "uBaseColor")
                s = "albedoMap";
            copyString(c.uniformName, sizeof(c.uniformName), s);
        }
        else if (property == "loop")
        {
            if (!ParseBool(value, c.loop, outError))
                return false;
        }
        else if (property == "playonstart")
        {
            if (!ParseBool(value, c.playOnStart, outError))
                return false;
        }
        else if (property == "playbackspeed")
        {
            if (!ParseF32(value, c.playbackSpeed, outError))
                return false;
        }
        else
        {
            if (outError)
                *outError = "Unknown VideoTextureComponent property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::VideoTextureComponent>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::VideoTextureComponent{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::VideoTextureComponent>(entity);
        return true;
    }
};

// SplineComponent references heavyweight data (control points, LUT, segment
// bounds) stored in SplineService and addressed by a generational handle. The
// handle itself is runtime-only — serialization re-creates the spline in the
// service on load and restores its control points via indexed pointN properties.
class SplineComponentSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "Spline"; }
    ECS::ComponentTypeId GetOwnedComponentType() const override
    {
        return ECS::GetComponentTypeId<Components::SplineComponent>();
    }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::SplineComponent>(entity);
        if (!c)
            return;
        outLines.push_back(std::string("Spline.defaultRadius = ") + FormatFloat(c->DefaultRadius));

        auto* svc = SplineECS::SplineService::TryGet();
        if (!svc)
            return;
        SplineECS::SplineHandle h{c->SplineDataIndex, c->SplineDataGeneration};
        const auto* data = svc->GetSplineData(h);
        if (!data)
            return;

        outLines.push_back(std::string("Spline.type = ") + std::to_string(static_cast<uint32_t>(data->Type)));
        outLines.push_back(std::string("Spline.closed = ") + (data->Closed ? "true" : "false"));
        outLines.push_back(std::string("Spline.pointCount = ") + std::to_string(static_cast<uint32_t>(data->Points.size())));
        for (size_t i = 0; i < data->Points.size(); ++i)
        {
            const auto& p = data->Points[i];
            const std::string idx = std::to_string(i);
            outLines.push_back(std::string("Spline.point") + idx + ".position = " +
                               FormatFloat3(p.Position.x, p.Position.y, p.Position.z));
            outLines.push_back(std::string("Spline.point") + idx + ".rotation = " +
                               FormatFloat3(p.Rotation.x, p.Rotation.y, p.Rotation.z));
            outLines.push_back(std::string("Spline.point") + idx + ".scale = " +
                               FormatFloat3(p.Scale.x, p.Scale.y, p.Scale.z));
            outLines.push_back(std::string("Spline.point") + idx + ".radius = " + FormatFloat(p.Radius));
            outLines.push_back(std::string("Spline.point") + idx + ".roll = " + FormatFloat(p.Roll));
            outLines.push_back(std::string("Spline.point") + idx + ".tangentIn = " +
                               FormatFloat3(p.TangentIn.x, p.TangentIn.y, p.TangentIn.z));
            outLines.push_back(std::string("Spline.point") + idx + ".tangentOut = " +
                               FormatFloat3(p.TangentOut.x, p.TangentOut.y, p.TangentOut.z));
        }
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::SplineComponent c{};
        if (auto* existing = world.GetComponent<Components::SplineComponent>(entity))
            c = *existing;

        auto ensureSpline = [&](Spline::SplineData*& outData) -> bool
        {
            auto* svc = SplineECS::SplineService::TryGet();
            if (!svc)
            {
                if (outError)
                    *outError = "SplineService not initialized";
                return false;
            }
            SplineECS::SplineHandle h{c.SplineDataIndex, c.SplineDataGeneration};
            auto* data = svc->GetSplineData(h);
            if (!data)
            {
                h = svc->CreateSpline();
                c.SplineDataIndex = h.Index();
                c.SplineDataGeneration = h.Generation();
                data = svc->GetSplineData(h);
            }
            outData = data;
            return data != nullptr;
        };

        if (property == "defaultradius")
        {
            if (!ParseF32(value, c.DefaultRadius, outError))
                return false;
        }
        else if (property == "type")
        {
            uint32_t v = 0;
            if (!ParseU32(value, v, outError))
                return false;
            if (v > 2)
            {
                if (outError)
                    *outError = "Spline.type must be 0-2";
                return false;
            }
            Spline::SplineData* data = nullptr;
            if (!ensureSpline(data))
                return false;
            data->Type = static_cast<Spline::SplineType>(v);
            data->MarkDirty();
        }
        else if (property == "closed")
        {
            bool b = false;
            if (!ParseBool(value, b, outError))
                return false;
            Spline::SplineData* data = nullptr;
            if (!ensureSpline(data))
                return false;
            data->Closed = b;
            data->MarkDirty();
        }
        else if (property == "pointcount")
        {
            uint32_t n = 0;
            if (!ParseU32(value, n, outError))
                return false;
            Spline::SplineData* data = nullptr;
            if (!ensureSpline(data))
                return false;
            data->Points.resize(n);
            data->MarkDirty();
        }
        else if (property.rfind("point", 0) == 0)
        {
            // point<N>.<field>
            const std::string_view rest = property.substr(5);
            const auto dot = rest.find('.');
            if (dot == std::string_view::npos)
            {
                if (outError)
                    *outError = "Spline point property must be point<N>.<field>";
                return false;
            }
            const std::string_view idxStr = rest.substr(0, dot);
            const std::string_view field = rest.substr(dot + 1);
            uint32_t idx = 0;
            for (char ch : idxStr)
            {
                if (ch < '0' || ch > '9')
                {
                    if (outError)
                        *outError = "Spline point index must be numeric";
                    return false;
                }
                idx = idx * 10u + static_cast<uint32_t>(ch - '0');
            }

            Spline::SplineData* data = nullptr;
            if (!ensureSpline(data))
                return false;
            if (idx >= data->Points.size())
                data->Points.resize(idx + 1);
            auto& p = data->Points[idx];

            if (field == "position")
            {
                Float3 v{p.Position.x, p.Position.y, p.Position.z};
                if (!ParseFloat3(value, v))
                {
                    if (outError)
                        *outError = "Spline point position must be (x, y, z)";
                    return false;
                }
                p.Position = Mathematics::Vector3{v.X, v.Y, v.Z};
            }
            else if (field == "radius")
            {
                if (!ParseF32(value, p.Radius, outError))
                    return false;
            }
            else if (field == "roll")
            {
                if (!ParseF32(value, p.Roll, outError))
                    return false;
            }
            else if (field == "rotation")
            {
                Float3 v{p.Rotation.x, p.Rotation.y, p.Rotation.z};
                if (!ParseFloat3(value, v))
                {
                    if (outError)
                        *outError = "Spline point rotation must be (x, y, z)";
                    return false;
                }
                p.Rotation = Mathematics::Vector3{v.X, v.Y, v.Z};
            }
            else if (field == "scale")
            {
                Float3 v{p.Scale.x, p.Scale.y, p.Scale.z};
                if (!ParseFloat3(value, v))
                {
                    if (outError)
                        *outError = "Spline point scale must be (x, y, z)";
                    return false;
                }
                p.Scale = Mathematics::Vector3{v.X, v.Y, v.Z};
            }
            else if (field == "tangentin")
            {
                Float3 v{p.TangentIn.x, p.TangentIn.y, p.TangentIn.z};
                if (!ParseFloat3(value, v))
                {
                    if (outError)
                        *outError = "Spline point tangentIn must be (x, y, z)";
                    return false;
                }
                p.TangentIn = Mathematics::Vector3{v.X, v.Y, v.Z};
            }
            else if (field == "tangentout")
            {
                Float3 v{p.TangentOut.x, p.TangentOut.y, p.TangentOut.z};
                if (!ParseFloat3(value, v))
                {
                    if (outError)
                        *outError = "Spline point tangentOut must be (x, y, z)";
                    return false;
                }
                p.TangentOut = Mathematics::Vector3{v.X, v.Y, v.Z};
            }
            else
            {
                if (outError)
                    *outError = "Unknown Spline point field";
                return false;
            }
            data->MarkDirty();
        }
        else
        {
            if (outError)
                *outError = "Unknown Spline property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::SplineComponent>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::SplineComponent{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        if (const auto* c = world.GetComponent<Components::SplineComponent>(entity))
        {
            if (auto* svc = SplineECS::SplineService::TryGet())
            {
                SplineECS::SplineHandle h{c->SplineDataIndex, c->SplineDataGeneration};
                if (svc->IsValid(h))
                    svc->DestroySpline(h);
            }
        }
        world.RemoveComponentImmediate<Components::SplineComponent>(entity);
        return true;
    }
};

class MeasureComponentSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "Measure"; }
    ECS::ComponentTypeId GetOwnedComponentType() const override
    {
        return ECS::GetComponentTypeId<Components::MeasureComponent>();
    }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::MeasureComponent>(entity);
        if (!c)
            return;

        outLines.push_back(std::string("Measure.is2D = ") + (c->Is2D ? "true" : "false"));
        outLines.push_back(std::string("Measure.start = ") +
                           FormatFloat3(c->Start[0], c->Start[1], c->Start[2]));
        outLines.push_back(std::string("Measure.end = ") +
                           FormatFloat3(c->End[0], c->End[1], c->End[2]));
        outLines.push_back(std::string("Measure.color = ") +
                           FormatFloat4(c->Color[0], c->Color[1], c->Color[2], c->Color[3]));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::MeasureComponent c{};
        if (auto* existing = world.GetComponent<Components::MeasureComponent>(entity))
            c = *existing;

        if (property == "is2d")
        {
            if (!ParseBool(value, c.Is2D, outError))
                return false;
        }
        else if (property == "start")
        {
            Float3 v{c.Start[0], c.Start[1], c.Start[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "Measure.start must be (x, y, z)";
                return false;
            }
            c.Start[0] = v.X;
            c.Start[1] = v.Y;
            c.Start[2] = v.Z;
        }
        else if (property == "end")
        {
            Float3 v{c.End[0], c.End[1], c.End[2]};
            if (!ParseFloat3(value, v))
            {
                if (outError)
                    *outError = "Measure.end must be (x, y, z)";
                return false;
            }
            c.End[0] = v.X;
            c.End[1] = v.Y;
            c.End[2] = v.Z;
        }
        else if (property == "color")
        {
            Float4 v{c.Color[0], c.Color[1], c.Color[2], c.Color[3]};
            if (!ParseFloat4(value, v))
            {
                if (outError)
                    *outError = "Measure.color must be (r, g, b, a)";
                return false;
            }
            c.Color[0] = v.X;
            c.Color[1] = v.Y;
            c.Color[2] = v.Z;
            c.Color[3] = v.W;
        }
        else
        {
            if (outError)
                *outError = "Unknown Measure property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::MeasureComponent>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::MeasureComponent{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::MeasureComponent>(entity);
        return true;
    }
};

} // namespace

void RegisterBuiltInSceneSchemas()
{
    // Thread-safe idempotent init. This is the only registration of the built-in schemas.
    static std::once_flag flag;
    std::call_once(flag, []()
                   {

    SceneSchemaRegistry::Register(std::make_unique<LightSchema>());
    SceneSchemaRegistry::Register(std::make_unique<CameraSchema>());
    SceneSchemaRegistry::Register(std::make_unique<DisabledSchema>());
    SceneSchemaRegistry::Register(std::make_unique<NameSchema>());
    SceneSchemaRegistry::Register(std::make_unique<TransformSchema>());
    SceneSchemaRegistry::Register(std::make_unique<HierarchyOrderSchema>());
    SceneSchemaRegistry::Register(std::make_unique<MeshRendererSchema>());
    SceneSchemaRegistry::Register(std::make_unique<MorphTargetWeightsSchema>());
    SceneSchemaRegistry::Register(std::make_unique<SkyEnvironmentSchema>());
    SceneSchemaRegistry::Register(std::make_unique<AmbientLightSchema>());
    SceneSchemaRegistry::Register(std::make_unique<SkyboxSchema>());
    SceneSchemaRegistry::Register(std::make_unique<PostProcessVolumeSchema>());
    SceneSchemaRegistry::Register(std::make_unique<WindVolumeSchema>());
    SceneSchemaRegistry::Register(std::make_unique<BloomEffectSchema>());
    SceneSchemaRegistry::Register(std::make_unique<HeightFogEffectSchema>());
    SceneSchemaRegistry::Register(std::make_unique<AtmosphericCloudLayerSchema>());
    SceneSchemaRegistry::Register(std::make_unique<VolumetricCloudsSchema>());
    SceneSchemaRegistry::Register(std::make_unique<VolumetricFogEffectSchema>());
    SceneSchemaRegistry::Register(std::make_unique<CubeLutEffectSchema>());
    SceneSchemaRegistry::Register(std::make_unique<PhysicsWorldSettingsSchema>());
    SceneSchemaRegistry::Register(std::make_unique<PhysicsBodySchema>());
    SceneSchemaRegistry::Register(std::make_unique<CharacterControllerSchema>());
    SceneSchemaRegistry::Register(std::make_unique<PhysicsColliderSchema>());
    SceneSchemaRegistry::Register(std::make_unique<PhysicsColliderOwnerSchema>());
    SceneSchemaRegistry::Register(std::make_unique<BoxColliderShapeSchema>());
    SceneSchemaRegistry::Register(std::make_unique<SphereColliderShapeSchema>());
    SceneSchemaRegistry::Register(std::make_unique<CapsuleColliderShapeSchema>());
    SceneSchemaRegistry::Register(std::make_unique<PlaneColliderShapeSchema>());
    SceneSchemaRegistry::Register(std::make_unique<AnimatorSchema>());
    SceneSchemaRegistry::Register(std::make_unique<GameLogicGraphRefSchema>());
    SceneSchemaRegistry::Register(std::make_unique<ValueCurveSchema>());
    SceneSchemaRegistry::Register(std::make_unique<ModelAnimationPlaybackLegacySchema>());
    SceneSchemaRegistry::Register(std::make_unique<SkinnedMeshRendererSchema>());
    SceneSchemaRegistry::Register(std::make_unique<AudioListenerSchema>());
    SceneSchemaRegistry::Register(std::make_unique<LODGroupSchema>());
    SceneSchemaRegistry::Register(std::make_unique<RenderLayerSchema>());
    SceneSchemaRegistry::Register(std::make_unique<VideoTextureSchema>());
    SceneSchemaRegistry::Register(std::make_unique<SplineComponentSchema>());
    SceneSchemaRegistry::Register(std::make_unique<MeasureComponentSchema>());
    });
}

void EnsureBuiltInSchemasRegistered()
{
    RegisterBuiltInSceneSchemas();
    static std::once_flag flag;
    std::call_once(flag, []() { Plugins::EnginePluginRegistry::Get().RegisterSceneSchemas(); });
}

} // namespace GameEngine::Scene
