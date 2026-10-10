#include "Components/ComponentRegistrationLite.h"
#include "ECS/Entity.h"
#include "ECS/ModuleRegistration.h"
#include "ECS/ComponentBase.h"
#include "ECS/ComponentLayoutReloadMigrator.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>

namespace LiteOwnerProbe
{
struct Blob : GameEngine::ECS::ComponentBase { int Value = 7; };
struct Typed : GameEngine::ECS::ComponentBase { int Value = 9; };
struct Resized : GameEngine::ECS::ComponentBase { int Value = 7; int Added = 13; };
struct Foreign : GameEngine::ECS::ComponentBase { int Value = 11; };
struct Runtime : GameEngine::ECS::ComponentBase { int Value = 31; };
struct HiddenRuntime : GameEngine::ECS::ComponentBase { int Value = 47; };
struct Durable : GameEngine::ECS::ComponentBase { int Value = 59; };

enum class Mode : std::uint32_t { Idle = 0, Busy = 1 };

// Carries one field of every kind the scanner attaches metadata to, so the whole
// macro surface a generated user translation unit can use is exercised here.
struct Annotated : GameEngine::ECS::ComponentBase
{
    float Amount = 0.5f;
    int Ratio = 3;
    Mode Kind = Mode::Busy;
};
}

// The exact registration form emitted by ComponentScanner in --detect-base mode.
GE_REGISTER_COMPONENT_BEGIN(LiteOwnerProbe::Runtime)
    GE_REGISTER_COMPONENT_FIELD(LiteOwnerProbe::Runtime, Value)
GE_REGISTER_COMPONENT_END(LiteOwnerProbe::Runtime)
GE_REFLECT_COMPONENT_DONOTSERIALIZE(LiteOwnerProbe::Runtime);

GE_REGISTER_COMPONENT_BEGIN(LiteOwnerProbe::HiddenRuntime)
    GE_REGISTER_COMPONENT_FIELD(LiteOwnerProbe::HiddenRuntime, Value)
GE_REGISTER_COMPONENT_END_NO_ADD(LiteOwnerProbe::HiddenRuntime)
GE_REFLECT_COMPONENT_DONOTSERIALIZE(LiteOwnerProbe::HiddenRuntime);

GE_REGISTER_COMPONENT_BEGIN(LiteOwnerProbe::Durable)
    GE_REGISTER_COMPONENT_FIELD(LiteOwnerProbe::Durable, Value)
GE_REGISTER_COMPONENT_END(LiteOwnerProbe::Durable)

GE_REFLECT_ENUM_TABLE_BEGIN(Mode)
    GE_REFLECT_ENUM_TABLE_VALUE("Idle", 0)
    GE_REFLECT_ENUM_TABLE_VALUE("Busy", 1)
GE_REFLECT_ENUM_TABLE_END()

GE_REGISTER_COMPONENT_BEGIN(LiteOwnerProbe::Annotated)
    GE_REGISTER_COMPONENT_FIELD(LiteOwnerProbe::Annotated, Amount)
    GE_REGISTER_COMPONENT_FIELD(LiteOwnerProbe::Annotated, Ratio)
    GE_REGISTER_COMPONENT_FIELD(LiteOwnerProbe::Annotated, Kind)
GE_REGISTER_COMPONENT_END(LiteOwnerProbe::Annotated)
GE_REFLECT_FIELD_RANGE(LiteOwnerProbe::Annotated, Amount, 0.0f, 1.0f);
GE_REFLECT_FIELD_TOOLTIP(LiteOwnerProbe::Annotated, Amount, "Blend amount");
GE_REFLECT_FIELD_FLAGS(LiteOwnerProbe::Annotated, Ratio, ::GameEngine::ECS::FieldFlags::ReadOnly);
GE_REFLECT_FIELD_TRANSIENT(LiteOwnerProbe::Annotated, Kind);
GE_REFLECT_ENUM_FIELD(LiteOwnerProbe::Annotated, Kind, Mode)
GE_REFLECT_COMPONENT_EDITORONLY(LiteOwnerProbe::Annotated);

GE_REFLECT_BEGIN(LiteOwnerProbe::Blob)
    GE_REFLECT_FIELD(LiteOwnerProbe::Blob, Value)
GE_REFLECT_END(LiteOwnerProbe::Blob);
GE_REFLECT_BEGIN(LiteOwnerProbe::Typed)
    GE_REFLECT_FIELD(LiteOwnerProbe::Typed, Value)
GE_REFLECT_END(LiteOwnerProbe::Typed);
GE_REFLECT_BEGIN(LiteOwnerProbe::Foreign)
    GE_REFLECT_FIELD(LiteOwnerProbe::Foreign, Value)
GE_REFLECT_END(LiteOwnerProbe::Foreign);

GE_REFLECT_BEGIN(LiteOwnerProbe::Resized)
    GE_REFLECT_FIELD(LiteOwnerProbe::Resized, Value)
    GE_REFLECT_FIELD(LiteOwnerProbe::Resized, Added)
GE_REFLECT_END(LiteOwnerProbe::Resized);

namespace
{
using namespace GameEngine::ECS;
struct RegistrationScope
{
    RegistrationScope(const char* module, uint64_t generation) { SetActiveRegistrationModule(module,generation); }
    ~RegistrationScope() { ClearActiveRegistrationModule(); }
};
template<class T> void RegisterLite(const char* module, uint64_t generation)
{
    RegistrationScope scope(module,generation);
    EXPECT_TRUE(GameEngine::Components::RegisterReflectedComponent<T>(true));
}
}

TEST(LiteComponentRegistration, BlobHandlerMovesToNewGenerationWithoutReplacement)
{
    using T = LiteOwnerProbe::Blob;
    const auto id = GetComponentTypeId<T>();
    RegisterLite<T>("LiteOwnerBlob",1);
    auto* handler = ComponentRegistry::GetHandler(id);
    ASSERT_NE(handler,nullptr);
    RegisterLite<T>("LiteOwnerBlob",2);
    EXPECT_EQ(ComponentRegistry::GetHandler(id),handler);
    EXPECT_EQ(ComponentRegistry::CountSupersededModuleComponents("LiteOwnerBlob",2),0u);
    EXPECT_EQ(ComponentRegistry::GetComponentInfo(id)->Module.Generation,2u);
}

TEST(LiteComponentRegistration, BlobRegistrationCannotReownTypedHandlerCode)
{
    using T = LiteOwnerProbe::Typed;
    const auto id = GetComponentTypeId<T>();
    const auto name = std::string(ComponentTypeName<T>());
    {
        RegistrationScope scope("LiteOwnerTyped",1);
        ComponentRegistry::RegisterComponent<T>(name);
    }
    auto* handler = ComponentRegistry::GetHandler(id);
    {
        RegistrationScope scope("LiteOwnerTyped",2);
        // Exercise the registry entry point as well as the lightweight caller.
        EXPECT_EQ(ComponentRegistry::RegisterBlobComponent(name,sizeof(T)),id);
        EXPECT_TRUE(GameEngine::Components::RegisterReflectedComponent<T>(true));
    }
    EXPECT_EQ(ComponentRegistry::GetHandler(id),handler);
    EXPECT_EQ(ComponentRegistry::CountSupersededModuleComponents("LiteOwnerTyped",2),1u);
    {
        RegistrationScope scope("LiteOwnerTyped",2);
        ComponentRegistry::RegisterComponent<T>(name);
    }
    EXPECT_EQ(ComponentRegistry::CountSupersededModuleComponents("LiteOwnerTyped",2),0u);
}

TEST(LiteComponentRegistration, SameOrForeignGenerationCannotStealBlobOwnership)
{
    using T = LiteOwnerProbe::Foreign;
    const auto id = GetComponentTypeId<T>();
    RegisterLite<T>("LiteOwnerOriginal",3);
    RegisterLite<T>("LiteOwnerOriginal",3);
    RegisterLite<T>("LiteOwnerOriginal",2);
    RegisterLite<T>("LiteOwnerOther",99);
    EXPECT_EQ(ComponentRegistry::GetComponentInfo(id)->Module.ModuleId,"LiteOwnerOriginal");
    EXPECT_EQ(ComponentRegistry::GetComponentInfo(id)->Module.Generation,3u);
}

TEST(LiteComponentRegistration, SizeChangeRefreshesOwnerButWaitsForMigration)
{
    using T = LiteOwnerProbe::Resized;
    const auto id = GetComponentTypeId<T>();
    const auto name = std::string(ComponentTypeName<T>());
    World world(nullptr);
    {
        RegistrationScope scope("LiteOwnerResized",1);
        ASSERT_EQ(ComponentRegistry::RegisterBlobComponent(name,sizeof(int)),id);
        const FieldInfo field{"Value",0,sizeof(int),FieldTypeId::Int32};
        ComponentFieldRegistry::RegisterOwned(id,std::span(&field,1),name);
        const int defaults = 7;
        ComponentFactory::RegisterDefaultBytes(id,&defaults,sizeof(defaults),true);
    }
    const auto entity = world.CreateEntity();
    const int placed = 42;
    ASSERT_TRUE(world.SetComponentBytesImmediate(entity,id,&placed,sizeof(placed)));
    ComponentLayoutReloadMigrator migrator;
    migrator.SnapshotLayouts();
    RegisterLite<T>("LiteOwnerResized",2);
    EXPECT_EQ(ComponentRegistry::GetHandler(id)->GetComponentSize(),sizeof(int));
    EXPECT_EQ(ComponentRegistry::CountSupersededModuleComponents("LiteOwnerResized",2),0u);
    ASSERT_EQ(migrator.MigrateChangedLayouts(world).size(),1u);
    EXPECT_EQ(ComponentRegistry::GetHandler(id)->GetComponentSize(),sizeof(T));
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(world.CaptureComponentBytes(entity,id,bytes));
    ASSERT_EQ(bytes.size(),sizeof(T));
    T restored{};
    std::memcpy(&restored,bytes.data(),sizeof(T));
    EXPECT_EQ(restored.Value,42);
    EXPECT_EQ(restored.Added,13);
}

TEST(LiteComponentRegistration, ComponentSerializationPolicyKeepsFieldsAndFactory)
{
    using T = LiteOwnerProbe::Runtime;
    const auto id = GetComponentTypeId<T>();
    EXPECT_TRUE(ComponentFieldRegistry::IsComponentDoNotSerialize(id));
    const auto fields = ComponentFieldRegistry::Get(id);
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_EQ(fields.front().Name, "Value");
    EXPECT_TRUE(ComponentFactory::Has(id));
    const auto addMenu = ComponentFactory::RegisteredTypes();
    EXPECT_NE(std::find(addMenu.begin(), addMenu.end(), id), addMenu.end());
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(ComponentFactory::GetDefaultBytes(id, bytes));
    ASSERT_EQ(bytes.size(), sizeof(T));
    T defaults{};
    std::memcpy(&defaults, bytes.data(), sizeof(T));
    EXPECT_EQ(defaults.Value, 31);
}

TEST(LiteComponentRegistration, ComponentSerializationPolicyAlsoSupportsNoAdd)
{
    using T = LiteOwnerProbe::HiddenRuntime;
    const auto id = GetComponentTypeId<T>();
    EXPECT_TRUE(ComponentFieldRegistry::IsComponentDoNotSerialize(id));
    EXPECT_EQ(ComponentFieldRegistry::Get(id).size(), 1u);
    // Has includes default bytes; RegisteredTypes is the Add Component menu.
    const auto addMenu = ComponentFactory::RegisteredTypes();
    EXPECT_EQ(std::find(addMenu.begin(), addMenu.end(), id), addMenu.end());
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(ComponentFactory::GetDefaultBytes(id, bytes));
    ASSERT_EQ(bytes.size(), sizeof(T));
    T defaults{};
    std::memcpy(&defaults, bytes.data(), sizeof(T));
    EXPECT_EQ(defaults.Value, 47);
}

TEST(LiteComponentRegistration, UnmarkedComponentRemainsSerializable)
{
    const auto id = GetComponentTypeId<LiteOwnerProbe::Durable>();
    EXPECT_FALSE(ComponentFieldRegistry::IsComponentDoNotSerialize(id));
    EXPECT_FALSE(ComponentFieldRegistry::IsComponentEditorOnly(id));
    EXPECT_EQ(ComponentFieldRegistry::Get(id).size(), 1u);
    EXPECT_TRUE(ComponentFactory::Has(id));
}

// Every metadata macro the scanner can emit must both compile and take effect
// against the lightweight header: a macro only the engine header defines is a compile
// error in the generated user translation unit that uses it.
TEST(LiteComponentRegistration, FieldMetadataMacrosApplyToLightweightRegistration)
{
    const auto id = GetComponentTypeId<LiteOwnerProbe::Annotated>();
    EXPECT_TRUE(ComponentFieldRegistry::IsComponentEditorOnly(id));
    const auto fields = ComponentFieldRegistry::Get(id);
    ASSERT_EQ(fields.size(), 3u);

    const FieldInfo& amount = fields[0];
    EXPECT_EQ(amount.Name, "Amount");
    EXPECT_TRUE(amount.HasRange);
    EXPECT_FLOAT_EQ(amount.MinValue, 0.0f);
    EXPECT_FLOAT_EQ(amount.MaxValue, 1.0f);
    EXPECT_EQ(amount.Tooltip, "Blend amount");

    const FieldInfo& ratio = fields[1];
    EXPECT_EQ(ratio.Name, "Ratio");
    EXPECT_EQ(ratio.Flags, FieldFlags::ReadOnly);
    EXPECT_FALSE(ratio.HasRange);

    const FieldInfo& kind = fields[2];
    EXPECT_EQ(kind.Name, "Kind");
    EXPECT_EQ(kind.Flags, FieldFlags::Transient);
    ASSERT_EQ(kind.EnumNames.size(), 2u);
    EXPECT_EQ(kind.EnumNames[0].Name, "Idle");
    EXPECT_EQ(kind.EnumNames[1].Value, 1);
}
