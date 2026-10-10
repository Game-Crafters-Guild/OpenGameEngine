// D1 — C# components as first-class citizens: a blob component registered together with
// its field schema through GE_ECSABI_RegisterBlobComponentWithSchema (the exact path the
// generated managed registration hub uses, no CLR needed here) must
//   * round-trip its values through .scene save/load via the reflection fallback,
//   * migrate placed instances by field name when a re-registration changes the layout
//     (same-size reorders AND size changes — the hot-reload relayout case),
//   * keep bytes intact when a schema merely attaches to an existing blob registration,
//   * survive the binary play-mode snapshot (World::SerializeEntity/DeserializeEntity).
//
// Offset contract: descriptors describe the managed (LayoutKind.Sequential) layout — the
// same bytes the C++ mirrors below produce with plain struct layout.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Core/Engine.h"
#include "Components/Name.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/UnresolvedComponentStore.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scripting/ECSABI.h"

namespace
{
using GameEngine::ECS::World;
using GameEngine::Components::Name;

// FieldTypeId values as the managed generator emits them (ECS/Reflection.h taxonomy).
constexpr uint16_t kFtBool = 1;
constexpr uint16_t kFtInt32 = 4;
constexpr uint16_t kFtFloat = 10;
constexpr uint16_t kFtVec3 = 13;

GE_ECS_FieldDesc Desc(const char* name, uint32_t offset, uint32_t size, uint16_t type)
{
    GE_ECS_FieldDesc d{};
    d.nameUtf8 = name;
    d.nameLen = static_cast<uint32_t>(std::strlen(name));
    d.offset = offset;
    d.size = size;
    d.fieldType = type;
    return d;
}

GE_Handle HandleOf(World* world)
{
    return static_cast<GE_Handle>(reinterpret_cast<uintptr_t>(world));
}

GE_ECS_ComponentTypeId RegisterSchema(const char* name, uint32_t sizeBytes,
                                      const std::vector<GE_ECS_FieldDesc>& fields)
{
    GE_ECS_ComponentTypeId id = 0;
    const GE_Result rc = GE_ECSABI_RegisterBlobComponentWithSchema(
        name, static_cast<uint32_t>(std::strlen(name)), sizeBytes,
        fields.empty() ? nullptr : fields.data(), static_cast<uint32_t>(fields.size()), &id);
    EXPECT_EQ(rc, GE_Result_Ok) << "schema registration failed for " << name;
    return id;
}

GameEngine::ECS::EntityHandle FindByName(World& world, const char* name)
{
    for (auto* archetype : world.GetAllArchetypes())
    {
        if (!archetype)
            continue;
        for (const auto& entity : archetype->CollectEntities())
        {
            if (!entity.IsValid() || !world.IsValid(entity))
                continue;
            const auto* n = world.GetComponent<Name>(entity);
            if (n && n->View() == name)
                return entity;
        }
    }
    return {};
}

std::string ReadFile(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

class BlobComponentSchemaAbiTests : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        GameEngine::Scene::EnsureBuiltInSchemasRegistered();
    }

    std::filesystem::path TempScene(const char* name) const
    {
        return std::filesystem::temp_directory_path() / name;
    }
};

// Mirror of a C# component: {float Health; int Ammo; bool Alive; <pad>; Vector3 Position}.
struct StatsBlob
{
    float Health;
    std::int32_t Ammo;
    std::uint8_t Alive;
    std::uint8_t Pad[3];
    float Position[3];
};
static_assert(sizeof(StatsBlob) == 24, "test mirror must match the declared schema size");

std::vector<GE_ECS_FieldDesc> StatsFields()
{
    return {
        Desc("Health", 0, 4, kFtFloat),
        Desc("Ammo", 4, 4, kFtInt32),
        Desc("Alive", 8, 1, kFtBool),
        Desc("Position", 12, 12, kFtVec3),
    };
}

TEST_F(BlobComponentSchemaAbiTests, SceneRoundTripThroughAbiSchema)
{
    const auto scenePath = TempScene("ge_blob_schema_roundtrip.scene");
    const GE_ECS_ComponentTypeId id = RegisterSchema("CsSchemaTest.Stats", sizeof(StatsBlob), StatsFields());
    ASSERT_NE(id, 0u);
    ASSERT_TRUE(GameEngine::ECS::ComponentFieldRegistry::Has(id));
    EXPECT_EQ(GameEngine::ECS::ComponentFieldRegistry::GetCanonicalName(id), "CsSchemaTest.Stats");

    {
        World world;
        GameEngine::ECS::Entity e = world.Create();
        Name name{};
        std::strncpy(name.value, "Subject", sizeof(name.value) - 1);
        e.Set(name);
        world.ProcessCommands();

        StatsBlob v{};
        v.Health = 12.5f;
        v.Ammo = -42;
        v.Alive = 1;
        v.Position[0] = 1.5f;
        v.Position[1] = -2.25f;
        v.Position[2] = 3.0f;
        ASSERT_EQ(GE_ECSABI_SetComponentBytes(HandleOf(&world), e.GetHandle().id, id,
                                              reinterpret_cast<const uint8_t*>(&v),
                                              sizeof(v)),
                  GE_Result_Ok);

        ASSERT_TRUE(GameEngine::Scene::SaveSceneToFile(world, scenePath));
    }

    // The C# component serializes under its SIMPLE name (the .scene component token cannot
    // contain '.'), with its values as reflected properties.
    const std::string text = ReadFile(scenePath);
    EXPECT_NE(text.find("Stats.Health = 12.5"), std::string::npos) << text;
    EXPECT_NE(text.find("Stats.Ammo = -42"), std::string::npos) << text;
    EXPECT_NE(text.find("Stats.Alive = true"), std::string::npos) << text;
    EXPECT_NE(text.find("Stats.Position = "), std::string::npos) << text;

    {
        World world;
        ASSERT_TRUE(GameEngine::Scene::LoadSceneFromFile(world, scenePath))
            << "load error: " << GameEngine::Scene::GetLastSceneIOError().message;

        const auto handle = FindByName(world, "Subject");
        ASSERT_TRUE(handle.IsValid());

        StatsBlob loaded{};
        uint32_t written = 0;
        ASSERT_EQ(GE_ECSABI_GetComponentBytes(HandleOf(&world), handle.id, id,
                                              reinterpret_cast<uint8_t*>(&loaded),
                                              sizeof(loaded), &written),
                  GE_Result_Ok);
        ASSERT_EQ(written, sizeof(StatsBlob));
        EXPECT_FLOAT_EQ(loaded.Health, 12.5f);
        EXPECT_EQ(loaded.Ammo, -42);
        EXPECT_EQ(loaded.Alive, 1);
        EXPECT_FLOAT_EQ(loaded.Position[0], 1.5f);
        EXPECT_FLOAT_EQ(loaded.Position[1], -2.25f);
        EXPECT_FLOAT_EQ(loaded.Position[2], 3.0f);
    }

    std::error_code ec;
    std::filesystem::remove(scenePath, ec);
}

// v1 {float A; float B} -> v2 {float B; float C}: same size, fields renamed/reordered. The
// re-registration must carry B by name, default C, and drop A — on instances placed in the
// primary world, through the same ABI call a managed hot-reload re-registration makes.
TEST_F(BlobComponentSchemaAbiTests, SameSizeRelayoutMigratesByFieldName)
{
    World* world = GameEngine::EngineCore::GetInstance().EnsurePrimaryWorld();
    ASSERT_NE(world, nullptr);

    const char* name = "CsSchemaTest.Migrate";
    const GE_ECS_ComponentTypeId id = RegisterSchema(
        name, 8, {Desc("A", 0, 4, kFtFloat), Desc("B", 4, 4, kFtFloat)});
    ASSERT_NE(id, 0u);

    const auto e = world->CreateEntity();
    ASSERT_TRUE(e.IsValid());
    const float v1[2] = {1.5f, 2.5f}; // A=1.5, B=2.5
    ASSERT_EQ(GE_ECSABI_SetComponentBytes(HandleOf(world), e.id, id,
                                          reinterpret_cast<const uint8_t*>(v1), sizeof(v1)),
              GE_Result_Ok);

    const GE_ECS_ComponentTypeId id2 = RegisterSchema(
        name, 8, {Desc("B", 0, 4, kFtFloat), Desc("C", 4, 4, kFtFloat)});
    EXPECT_EQ(id2, id) << "re-registration must resolve to the same type id";

    float v2[2] = {-1.0f, -1.0f};
    uint32_t written = 0;
    ASSERT_EQ(GE_ECSABI_GetComponentBytes(HandleOf(world), e.id, id,
                                          reinterpret_cast<uint8_t*>(v2), sizeof(v2), &written),
              GE_Result_Ok);
    ASSERT_EQ(written, 8u);
    EXPECT_FLOAT_EQ(v2[0], 2.5f) << "B must carry its value to its new offset";
    EXPECT_FLOAT_EQ(v2[1], 0.0f) << "C is a new field and must default to zero (A is dropped)";

    GE_ECSABI_DestroyEntity(HandleOf(world), e.id);
}

// v1 {float A; float B} (8) -> v2 {float B; float C; float D} (12): the size change that
// plain RegisterBlobComponent rejects. The schema path must migrate instances, update the
// recorded component size, and leave the name+size registration consistent.
TEST_F(BlobComponentSchemaAbiTests, SizeChangeRelayoutMigratesAndResizes)
{
    World* world = GameEngine::EngineCore::GetInstance().EnsurePrimaryWorld();
    ASSERT_NE(world, nullptr);

    const char* name = "CsSchemaTest.Grow";
    const GE_ECS_ComponentTypeId id = RegisterSchema(
        name, 8, {Desc("A", 0, 4, kFtFloat), Desc("B", 4, 4, kFtFloat)});
    ASSERT_NE(id, 0u);

    const auto e = world->CreateEntity();
    const float v1[2] = {7.0f, 9.0f};
    ASSERT_EQ(GE_ECSABI_SetComponentBytes(HandleOf(world), e.id, id,
                                          reinterpret_cast<const uint8_t*>(v1), sizeof(v1)),
              GE_Result_Ok);

    const GE_ECS_ComponentTypeId id2 = RegisterSchema(
        name, 12,
        {Desc("B", 0, 4, kFtFloat), Desc("C", 4, 4, kFtFloat), Desc("D", 8, 4, kFtFloat)});
    EXPECT_EQ(id2, id);

    auto* handler = GameEngine::ECS::ComponentRegistry::GetHandler(id);
    ASSERT_NE(handler, nullptr);
    EXPECT_EQ(handler->GetComponentSize(), 12u) << "recorded size must follow the relayout";

    float v2[3] = {-1.0f, -1.0f, -1.0f};
    uint32_t written = 0;
    ASSERT_EQ(GE_ECSABI_GetComponentBytes(HandleOf(world), e.id, id,
                                          reinterpret_cast<uint8_t*>(v2), sizeof(v2), &written),
              GE_Result_Ok);
    ASSERT_EQ(written, 12u);
    EXPECT_FLOAT_EQ(v2[0], 9.0f) << "B must carry across the size change";
    EXPECT_FLOAT_EQ(v2[1], 0.0f);
    EXPECT_FLOAT_EQ(v2[2], 0.0f);

    // The legacy name+size registration path must agree with the new size.
    GE_ECS_ComponentTypeId legacyId = 0;
    ASSERT_EQ(GE_ECSABI_RegisterBlobComponent(name, static_cast<uint32_t>(std::strlen(name)), 12, &legacyId),
              GE_Result_Ok);
    EXPECT_EQ(legacyId, id);

    GE_ECSABI_DestroyEntity(HandleOf(world), e.id);
}

// A schema attaching to a blob that was registered name+size only (and already has live
// instances) must NOT re-pack: the bytes already match the described layout.
TEST_F(BlobComponentSchemaAbiTests, SchemaAttachToExistingBlobKeepsBytes)
{
    World* world = GameEngine::EngineCore::GetInstance().EnsurePrimaryWorld();
    ASSERT_NE(world, nullptr);

    const char* name = "CsSchemaTest.Attach";
    GE_ECS_ComponentTypeId id = 0;
    ASSERT_EQ(GE_ECSABI_RegisterBlobComponent(name, static_cast<uint32_t>(std::strlen(name)), 8, &id),
              GE_Result_Ok);
    ASSERT_NE(id, 0u);
    EXPECT_FALSE(GameEngine::ECS::ComponentFieldRegistry::Has(id));

    const auto e = world->CreateEntity();
    const float v1[2] = {4.5f, 5.5f};
    ASSERT_EQ(GE_ECSABI_SetComponentBytes(HandleOf(world), e.id, id,
                                          reinterpret_cast<const uint8_t*>(v1), sizeof(v1)),
              GE_Result_Ok);

    const GE_ECS_ComponentTypeId id2 = RegisterSchema(
        name, 8, {Desc("A", 0, 4, kFtFloat), Desc("B", 4, 4, kFtFloat)});
    EXPECT_EQ(id2, id);
    EXPECT_TRUE(GameEngine::ECS::ComponentFieldRegistry::Has(id));

    float v2[2] = {0.0f, 0.0f};
    uint32_t written = 0;
    ASSERT_EQ(GE_ECSABI_GetComponentBytes(HandleOf(world), e.id, id,
                                          reinterpret_cast<uint8_t*>(v2), sizeof(v2), &written),
              GE_Result_Ok);
    EXPECT_FLOAT_EQ(v2[0], 4.5f);
    EXPECT_FLOAT_EQ(v2[1], 5.5f);

    GE_ECSABI_DestroyEntity(HandleOf(world), e.id);
}

// Re-registering the identical schema (every assembly load does this) must not disturb
// placed values.
TEST_F(BlobComponentSchemaAbiTests, IdempotentReRegistrationKeepsValues)
{
    World* world = GameEngine::EngineCore::GetInstance().EnsurePrimaryWorld();
    ASSERT_NE(world, nullptr);

    const char* name = "CsSchemaTest.Idempotent";
    const std::vector<GE_ECS_FieldDesc> fields = {Desc("A", 0, 4, kFtFloat),
                                                  Desc("B", 4, 4, kFtFloat)};
    const GE_ECS_ComponentTypeId id = RegisterSchema(name, 8, fields);
    ASSERT_NE(id, 0u);

    const auto e = world->CreateEntity();
    const float v1[2] = {3.25f, -8.5f};
    ASSERT_EQ(GE_ECSABI_SetComponentBytes(HandleOf(world), e.id, id,
                                          reinterpret_cast<const uint8_t*>(v1), sizeof(v1)),
              GE_Result_Ok);

    EXPECT_EQ(RegisterSchema(name, 8, fields), id);

    float v2[2] = {0.0f, 0.0f};
    uint32_t written = 0;
    ASSERT_EQ(GE_ECSABI_GetComponentBytes(HandleOf(world), e.id, id,
                                          reinterpret_cast<uint8_t*>(v2), sizeof(v2), &written),
              GE_Result_Ok);
    EXPECT_FLOAT_EQ(v2[0], 3.25f);
    EXPECT_FLOAT_EQ(v2[1], -8.5f);

    GE_ECSABI_DestroyEntity(HandleOf(world), e.id);
}

// The binary play-mode snapshot (World::SerializeWorld/DeserializeWorld — the restore loop
// at World::DeserializeEntity silently drops components with no handler) round-trips a blob
// component once its handler is registered, which is guaranteed after the assembly-load
// registration hub runs: blob handlers are process-lifetime, never unregistered on a swap.
TEST_F(BlobComponentSchemaAbiTests, BinarySnapshotRoundTripsBlobComponent)
{
    const GE_ECS_ComponentTypeId id = RegisterSchema(
        "CsSchemaTest.Snapshot", 8, {Desc("A", 0, 4, kFtFloat), Desc("B", 4, 4, kFtFloat)});
    ASSERT_NE(id, 0u);

    World world;
    const auto e = world.CreateEntity();
    const float v1[2] = {11.0f, 22.0f};
    ASSERT_EQ(GE_ECSABI_SetComponentBytes(HandleOf(&world), e.id, id,
                                          reinterpret_cast<const uint8_t*>(v1), sizeof(v1)),
              GE_Result_Ok);

    const std::vector<uint8_t> snapshot = world.SerializeWorld();
    ASSERT_FALSE(snapshot.empty());

    World restoredWorld;
    restoredWorld.DeserializeWorld(snapshot);

    // The restored world contains exactly one entity, carrying the blob bytes.
    bool found = false;
    for (auto* archetype : restoredWorld.GetAllArchetypes())
    {
        if (!archetype)
            continue;
        for (const auto& entity : archetype->CollectEntities())
        {
            if (!entity.IsValid() || !restoredWorld.IsValid(entity))
                continue;
            float v2[2] = {0.0f, 0.0f};
            uint32_t written = 0;
            if (GE_ECSABI_GetComponentBytes(HandleOf(&restoredWorld), entity.id, id,
                                            reinterpret_cast<uint8_t*>(v2), sizeof(v2),
                                            &written) == GE_Result_Ok &&
                written == sizeof(v2))
            {
                EXPECT_FLOAT_EQ(v2[0], 11.0f);
                EXPECT_FLOAT_EQ(v2[1], 22.0f);
                found = true;
            }
        }
    }
    EXPECT_TRUE(found) << "snapshot restore dropped the blob component";
}

// Guard rails: a native component's name is off-limits, and descriptors that lie about
// their spans are rejected whole.
TEST_F(BlobComponentSchemaAbiTests, RejectsNativeNamesAndInvalidSpans)
{
    // GameEngine::Components::Name is registered by the engine with a typed (non-blob) handler.
    GE_ECS_ComponentTypeId id = 0;
    const char* nativeName = "GameEngine::Components::Name";
    std::vector<GE_ECS_FieldDesc> fields = {Desc("value", 0, 4, kFtInt32)};
    EXPECT_EQ(GE_ECSABI_RegisterBlobComponentWithSchema(
                  nativeName, static_cast<uint32_t>(std::strlen(nativeName)), 256,
                  fields.data(), 1, &id),
              GE_Result_Fail);
    EXPECT_EQ(id, 0u);

    // Field span exceeds the declared component size -> InvalidArg, nothing registered.
    const char* badName = "CsSchemaTest.BadSpan";
    std::vector<GE_ECS_FieldDesc> bad = {Desc("A", 4, 8, kFtFloat)};
    EXPECT_EQ(GE_ECSABI_RegisterBlobComponentWithSchema(
                  badName, static_cast<uint32_t>(std::strlen(badName)), 8, bad.data(), 1, &id),
              GE_Result_InvalidArg);
    GE_ECS_ComponentTypeId lookedUp = 0;
    EXPECT_EQ(GE_ECSABI_GetComponentTypeIdByName(badName, static_cast<uint32_t>(std::strlen(badName)),
                                                 &lookedUp),
              GE_Result_NotFound);
}

// P4b FINDING B(2) — package ejection data safety. A C# blob component whose
// package DLL is truly gone next session must ride the preservation store
// through scene save/load without loss, and come back to life once the package
// (and its registration) returns. Timeline: register -> place -> save ->
// unregister (the test seam stands in for a process restart with the DLL
// pruned) -> load+save twice (byte-stable) -> re-register -> load -> bytes intact.
TEST_F(BlobComponentSchemaAbiTests, UnregisteredBlobRoundTripsAndRevivesOnReRegister)
{
    const auto pathA = TempScene("ge_blob_eject_a.scene");
    const auto pathB = TempScene("ge_blob_eject_b.scene");
    const auto pathC = TempScene("ge_blob_eject_c.scene");

    struct LocalTagBlob
    {
        float Value;
    };
    const char* kName = "EjectPack.LocalTag";
    const std::vector<GE_ECS_FieldDesc> fields = {Desc("Value", 0, 4, kFtFloat)};
    const GE_ECS_ComponentTypeId id = RegisterSchema(kName, sizeof(LocalTagBlob), fields);
    ASSERT_NE(id, 0u);

    // Session 1: place + save while the package is installed.
    {
        World world;
        GameEngine::ECS::Entity e = world.Create();
        Name name{};
        std::strncpy(name.value, "Subject", sizeof(name.value) - 1);
        e.Set(name);
        world.ProcessCommands();
        LocalTagBlob v{7.5f};
        ASSERT_EQ(GE_ECSABI_SetComponentBytes(HandleOf(&world), e.GetHandle().id, id,
                                              reinterpret_cast<const uint8_t*>(&v), sizeof(v)),
                  GE_Result_Ok);
        ASSERT_TRUE(GameEngine::Scene::SaveSceneToFile(world, pathA));
    }
    EXPECT_NE(ReadFile(pathA).find("LocalTag.Value = 7.5"), std::string::npos) << ReadFile(pathA);

    // Package removed: next session has no registration at all.
    ASSERT_TRUE(GameEngine::ECS::ComponentRegistry::UnregisterComponentForTests(id));
    GameEngine::ECS::ComponentFieldRegistry::UnregisterForTests(id);

    // Session 2: load + save with the type unregistered — preserved, not dropped.
    {
        World world;
        ASSERT_TRUE(GameEngine::Scene::LoadSceneFromFile(world, pathA))
            << GameEngine::Scene::GetLastSceneIOError().message;
        const auto h = FindByName(world, "Subject");
        ASSERT_TRUE(h.IsValid());
        const auto* store = world.TryGetUnresolvedComponents();
        ASSERT_NE(store, nullptr) << "component was dropped instead of preserved";
        ASSERT_NE(store->Map().find(h), store->Map().end());
        ASSERT_TRUE(GameEngine::Scene::SaveSceneToFile(world, pathB));
    }
    EXPECT_NE(ReadFile(pathB).find("LocalTag.Value = 7.5"), std::string::npos)
        << "preservation must echo the authored casing\n"
        << ReadFile(pathB);

    // Session 3: a second unregistered cycle is byte-stable from the entity body on.
    {
        World world;
        ASSERT_TRUE(GameEngine::Scene::LoadSceneFromFile(world, pathB));
        ASSERT_TRUE(GameEngine::Scene::SaveSceneToFile(world, pathC));
    }
    {
        std::string b = ReadFile(pathB);
        std::string c = ReadFile(pathC);
        const auto bodyB = b.find("[entity ");
        const auto bodyC = c.find("[entity ");
        ASSERT_NE(bodyB, std::string::npos);
        ASSERT_NE(bodyC, std::string::npos);
        EXPECT_EQ(b.substr(bodyB), c.substr(bodyC)) << "preserved payload must be byte-stable";
    }

    // Session 4: package re-added — the module initializer re-registers, a load
    // resolves the preserved lines, and the data is intact on the entity.
    ASSERT_EQ(RegisterSchema(kName, sizeof(LocalTagBlob), fields), id);
    {
        World world;
        ASSERT_TRUE(GameEngine::Scene::LoadSceneFromFile(world, pathC));
        const auto h = FindByName(world, "Subject");
        ASSERT_TRUE(h.IsValid());
        LocalTagBlob loaded{};
        uint32_t written = 0;
        ASSERT_EQ(GE_ECSABI_GetComponentBytes(HandleOf(&world), h.id, id,
                                              reinterpret_cast<uint8_t*>(&loaded), sizeof(loaded),
                                              &written),
                  GE_Result_Ok)
            << "re-registered component did not come back to life from the preserved lines";
        EXPECT_FLOAT_EQ(loaded.Value, 7.5f);
    }

    std::error_code ec;
    std::filesystem::remove(pathA, ec);
    std::filesystem::remove(pathB, ec);
    std::filesystem::remove(pathC, ec);
}

// P4b FINDING B(2), qualified-name shape. Scene lines carrying the NAMESPACE-
// QUALIFIED blob form ('QualPack.QTag.Value = 3' — written by external tools or
// by the preservation path itself) split at the first dot while unregistered,
// which is fine for byte-stable preservation — but once the package returns,
// the loader must regroup the line as the qualified component and apply it.
// Before the fix the first-dot split left it as unknown 'qualpack' forever.
TEST_F(BlobComponentSchemaAbiTests, QualifiedNameBlobLinesRegroupOnReRegister)
{
    const auto pathA = TempScene("ge_blob_qual_a.scene");
    const auto pathB = TempScene("ge_blob_qual_b.scene");

    // Author a scene with the qualified line while the type is UNREGISTERED.
    {
        World world;
        GameEngine::ECS::Entity e = world.Create();
        Name name{};
        std::strncpy(name.value, "QSubject", sizeof(name.value) - 1);
        e.Set(name);
        world.ProcessCommands();
        ASSERT_TRUE(GameEngine::Scene::SaveSceneToFile(world, pathA));

        std::string text = ReadFile(pathA);
        const auto hdr = text.find("[entity ");
        ASSERT_NE(hdr, std::string::npos);
        const auto eol = text.find('\n', hdr);
        ASSERT_NE(eol, std::string::npos);
        text.insert(eol + 1, "QualPack.QTag.Value = 3\n");
        std::ofstream out(pathA, std::ios::binary);
        out << text;
    }

    // Unregistered round trip: preserved byte-stably (under the first segment).
    {
        World world;
        ASSERT_TRUE(GameEngine::Scene::LoadSceneFromFile(world, pathA));
        const auto h = FindByName(world, "QSubject");
        ASSERT_TRUE(h.IsValid());
        const auto* store = world.TryGetUnresolvedComponents();
        ASSERT_NE(store, nullptr);
        ASSERT_NE(store->Map().find(h), store->Map().end());
        ASSERT_TRUE(GameEngine::Scene::SaveSceneToFile(world, pathB));
    }
    EXPECT_NE(ReadFile(pathB).find("QualPack.QTag.Value = 3"), std::string::npos)
        << "preservation must echo the authored casing\n"
        << ReadFile(pathB);

    // Package returns: the qualified registration makes the reload regroup and apply.
    struct QTagBlob
    {
        std::int32_t Value;
    };
    const std::vector<GE_ECS_FieldDesc> fields = {Desc("Value", 0, 4, kFtInt32)};
    const GE_ECS_ComponentTypeId id = RegisterSchema("QualPack.QTag", sizeof(QTagBlob), fields);
    ASSERT_NE(id, 0u);
    {
        World world;
        ASSERT_TRUE(GameEngine::Scene::LoadSceneFromFile(world, pathB))
            << GameEngine::Scene::GetLastSceneIOError().message;
        const auto h = FindByName(world, "QSubject");
        ASSERT_TRUE(h.IsValid());
        QTagBlob loaded{};
        uint32_t written = 0;
        ASSERT_EQ(GE_ECSABI_GetComponentBytes(HandleOf(&world), h.id, id,
                                              reinterpret_cast<uint8_t*>(&loaded), sizeof(loaded),
                                              &written),
                  GE_Result_Ok)
            << "qualified-name line did not regroup to the re-registered component";
        EXPECT_EQ(loaded.Value, 3);
    }

    std::error_code ec;
    std::filesystem::remove(pathA, ec);
    std::filesystem::remove(pathB, ec);
}
} // namespace
