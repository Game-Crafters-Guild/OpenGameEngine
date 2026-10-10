#include <gtest/gtest.h>

#include "Components/Measure/MeasureComponent.h"
#include "Components/Spline/SplineComponent.h"
#include "ECS/ComponentFactory.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h"
#include "Scene/SceneSchemaRegistry.h"

#include <string>
#include <vector>

using namespace GameEngine;

namespace
{
// Every serializable component type has exactly one schema. A hand-written schema owns its type
// through its file token or, when the token differs from the reflected name, through
// GetOwnedComponentType; the reflection fallback covers only types no hand-written schema owns.
class SceneSchemaOwnership : public testing::Test
{
  protected:
    void SetUp() override { Scene::EnsureBuiltInSchemasRegistered(); }
};

TEST_F(SceneSchemaOwnership, TokenOwnedTypeHasOneSchemaReachedByItsToken)
{
    struct Owned
    {
        const char* token;
        const char* reflectedName;
        ECS::ComponentTypeId typeId;
    };
    const Owned owned[] = {
        {"Spline", "SplineComponent", ECS::GetComponentTypeId<Components::SplineComponent>()},
        {"Measure", "MeasureComponent", ECS::GetComponentTypeId<Components::MeasureComponent>()},
    };
    for (const Owned& o : owned)
    {
        SCOPED_TRACE(o.token);
        ASSERT_EQ(ECS::ComponentFieldRegistry::FindByName(o.reflectedName), o.typeId);
        const auto* byToken = Scene::SceneSchemaRegistry::Find(o.token);
        ASSERT_NE(byToken, nullptr);
        EXPECT_EQ(byToken->GetComponentName(), o.token);
        EXPECT_EQ(byToken->GetOwnedComponentType(), o.typeId);
        EXPECT_EQ(Scene::SceneSchemaRegistry::FindForComponentType(o.typeId), byToken);
        EXPECT_EQ(Scene::SceneSchemaRegistry::Find(o.reflectedName), nullptr);
        EXPECT_EQ(Scene::SceneSchemaRegistry::ReflectionSchemaForUnhandledType(o.typeId), nullptr);
        EXPECT_TRUE(Scene::SceneSchemaRegistry::HasRegistered(o.token));
        EXPECT_FALSE(Scene::SceneSchemaRegistry::HasRegistered(o.reflectedName));
    }
}

TEST_F(SceneSchemaOwnership, RegistrationLookupNeverSynthesizesAReflectionSchema)
{
    // A reflected type with no hand-written schema: the loader reaches it through the reflection
    // fallback, but a registration guard must not mistake that fallback for a registered schema.
    const char* reflectedOnly = nullptr;
    for (const char* candidate : {"SkeletonRef", "AnimatorRef", "RenderLayer"})
    {
        const ECS::ComponentTypeId typeId = ECS::ComponentFieldRegistry::FindByName(candidate);
        if (typeId != 0 && Scene::SceneSchemaRegistry::FindForComponentType(typeId) == nullptr &&
            !ECS::ComponentFieldRegistry::IsComponentDoNotSerialize(typeId))
        {
            reflectedOnly = candidate;
            break;
        }
    }
    ASSERT_NE(reflectedOnly, nullptr) << "no reflection-only component available to probe";
    EXPECT_NE(Scene::SceneSchemaRegistry::Find(reflectedOnly), nullptr);
    EXPECT_FALSE(Scene::SceneSchemaRegistry::HasRegistered(reflectedOnly));
    EXPECT_TRUE(Scene::SceneSchemaRegistry::HasRegistered("Transform"));
    EXPECT_TRUE(Scene::SceneSchemaRegistry::HasRegistered("transform"));
    EXPECT_TRUE(Scene::SceneSchemaRegistry::HasRegistered("ValueCurve"));
}

// The class the type claim exists for, enumerated mechanically: for every reflected, serializable
// component type that the reflection fallback would write, no hand-written schema may also report
// it present on an entity holding only that component. A hit means a save writes the component
// twice under two names and a load applies the second block over the first.
TEST_F(SceneSchemaOwnership, NoReflectedComponentIsWrittenByBothAHandWrittenSchemaAndReflection)
{
    const std::vector<const Scene::ISceneComponentSchema*> handWritten =
        Scene::SceneSchemaRegistry::GetAllSorted();
    ASSERT_GT(handWritten.size(), 30u);

    std::vector<std::string> doubleWritten;
    size_t reflectedFallbackTypes = 0;
    size_t notCreatable = 0;
    for (const ECS::ComponentTypeId typeId : ECS::ComponentFactory::DefaultByteTypes())
    {
        const auto* fallback = Scene::SceneSchemaRegistry::ReflectionSchemaForUnhandledType(typeId);
        if (!fallback)
            continue;
        ++reflectedFallbackTypes;
        ECS::World world;
        const ECS::EntityHandle entity = world.CreateEntity();
        if (!ECS::ComponentFactory::Create(world, entity, typeId))
        {
            ++notCreatable; // reflected but not an ECS component of this process
            continue;
        }
        for (const Scene::ISceneComponentSchema* schema : handWritten)
        {
            if (schema->IsPresent(world, entity))
                doubleWritten.push_back(std::string(ECS::ComponentFieldRegistry::GetCanonicalName(typeId)) +
                                        " is written by '" + std::string(schema->GetComponentName()) +
                                        "' and by reflection as '" +
                                        std::string(fallback->GetComponentName()) + "'");
        }
    }
    EXPECT_GT(reflectedFallbackTypes, 0u);
    EXPECT_TRUE(doubleWritten.empty()) << [&]
    {
        std::string joined;
        for (const auto& line : doubleWritten)
            joined += line + '\n';
        return joined;
    }();
    EXPECT_GT(reflectedFallbackTypes, notCreatable);
    RecordProperty("hand_written_schemas", static_cast<int>(handWritten.size()));
    RecordProperty("reflection_fallback_types", static_cast<int>(reflectedFallbackTypes));
    RecordProperty("reflection_fallback_types_not_creatable", static_cast<int>(notCreatable));
}
} // namespace
