#pragma once

// ReflectionSceneSchema — a generic ISceneComponentSchema that serializes ANY reflected component
// via ComponentFieldRegistry + the FieldSerializerRegistry, reusing the same byte get/set the
// editor inspector uses (World::CaptureComponentBytes / ApplyComponentBytesImmediate) and the same
// default-construct-add the Add-Component menu uses (ComponentFactory::Create).
//
// It is synthesized per ComponentTypeId by SceneSchemaRegistry::Find and
// ReflectionSchemaForUnhandledType when a component has no hand-written schema, so it slots into the
// existing scene-IO call sites unchanged. Engine
// and user components share this one code path; hand-written schemas remain only as overrides for
// components with genuine non-field logic.

#include "ECS/ComponentFieldRegistry.h" // ComponentTypeId
#include "Scene/SceneSchemaRegistry.h"  // ISceneComponentSchema

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace GameEngine::Scene
{

class ReflectionSceneSchema final : public ISceneComponentSchema
{
  public:
    // `simpleName` is the unqualified component name written to files (e.g. "Jumpable"); it must
    // resolve back via ComponentFieldRegistry::FindByName on load.
    ReflectionSceneSchema(ECS::ComponentTypeId typeId, std::string simpleName);

    std::string_view GetComponentName() const override { return m_Name; }
    bool IsPresent(const ECS::World& world, ECS::EntityHandle entity) const override;
    void Serialize(const ECS::World& world, ECS::EntityHandle entity, const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override;
    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value, std::string* outError) const override;
    bool ApplyProperties(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                         std::span<const std::pair<std::string_view, std::string_view>> props,
                         std::string* outError, std::size_t* outFailedIndex) const override;
    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const override;
    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override;
    // Every non-null asset reference field (AssetRef<T> and arrays of them), named by the key its
    // scene line uses ("Texture", "Meshes2"), so ApplyProperty clears it.
    void EnumerateAssetReferences(const ECS::World& world, ECS::EntityHandle entity,
                                  const AssetRefVisitor& visitor) const override;

  private:
    ECS::ComponentTypeId m_TypeId;
    std::string m_Name;
};

} // namespace GameEngine::Scene
