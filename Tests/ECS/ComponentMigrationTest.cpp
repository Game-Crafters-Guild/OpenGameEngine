// World::MigrateComponentLayout — hot-reload schema migration for placed components.
//
// When a user edits a component struct (adds / reorders a field) that is already placed on
// entities, the next reload changes its byte layout. These tests exercise the migration in
// isolation (no DLL reload): register a type-erased blob component + field table, place it,
// then call MigrateComponentLayout with old/new field tables and assert that same-named fields
// survive (by name+type+size, moved to their new offsets) and appended fields take their
// default — and that neighbours sharing the chunk are not corrupted.

#include "ECS/ComponentFactory.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ComponentMigration.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ECS.h"
#include "ECS/Entity.h"
#include "ECS/Reflection.h"
#include "ECS/World.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace GameEngine::ECS;

namespace
{
FieldInfo FloatField(std::string_view name, std::uint32_t offset)
{
    // Positional init is {Name, Offset, Size, Type, AssetCategory}; Flags is a
    // defaulted trailing member (None) since AssetCategory was inserted.
    return FieldInfo{name, offset, 4u, FieldTypeId::Float};
}

std::vector<std::uint8_t> Bytes(const float* data, std::size_t count)
{
    const auto* p = reinterpret_cast<const std::uint8_t*>(data);
    return std::vector<std::uint8_t>(p, p + count * sizeof(float));
}
} // namespace

// Appending a field (12B -> 16B) keeps the three existing fields (including a chunk neighbour's)
// and gives the new field its registered default.
TEST(ComponentMigrationTest, AppendedFieldDefaultsAndPreservesExisting)
{
    World world(nullptr);

    const std::string name = "MigTest::Appended";
    const ComponentTypeId id = ComponentRegistry::RegisterBlobComponent(name, 12);
    ASSERT_NE(id, 0u);
    std::vector<FieldInfo> oldFields = {FloatField("A", 0), FloatField("B", 4), FloatField("C", 8)};
    ComponentFieldRegistry::Register(id, oldFields, name);

    auto place = [&](float a, float b, float c) {
        EntityHandle e = world.CreateEntity();
        const float v[3] = {a, b, c};
        EXPECT_TRUE(world.SetComponentBytesImmediate(e, id, v, sizeof(v)));
        return e;
    };
    const EntityHandle e1 = place(1.0f, 2.0f, 3.0f);
    const EntityHandle e2 = place(10.0f, 20.0f, 30.0f); // shares the chunk with e1

    std::vector<FieldInfo> newFields = {FloatField("A", 0), FloatField("B", 4), FloatField("C", 8),
                                        FloatField("D", 12)};
    const float defaults[4] = {0.0f, 0.0f, 0.0f, 7.0f}; // D's default
    ComponentLayoutChange change{id, oldFields, 12, newFields, 16, Bytes(defaults, 4)};
    world.MigrateComponentLayout(change);

    ASSERT_NE(ComponentRegistry::GetHandler(id), nullptr);
    EXPECT_EQ(ComponentRegistry::GetHandler(id)->GetComponentSize(), 16u);

    auto check = [&](EntityHandle e, float a, float b, float c) {
        std::vector<std::uint8_t> bytes;
        ASSERT_TRUE(world.CaptureComponentBytes(e, id, bytes));
        ASSERT_EQ(bytes.size(), 16u);
        const float* f = reinterpret_cast<const float*>(bytes.data());
        EXPECT_FLOAT_EQ(f[0], a);
        EXPECT_FLOAT_EQ(f[1], b);
        EXPECT_FLOAT_EQ(f[2], c);
        EXPECT_FLOAT_EQ(f[3], 7.0f); // appended field took its default
    };
    check(e1, 1.0f, 2.0f, 3.0f);
    check(e2, 10.0f, 20.0f, 30.0f); // neighbour intact
}

// Removing a field (16B -> 12B) drops its value and carries the survivors to their new offsets.
TEST(ComponentMigrationTest, RemovedFieldDroppedSurvivorsCarried)
{
    World world(nullptr);

    const std::string name = "MigTest::Removed";
    const ComponentTypeId id = ComponentRegistry::RegisterBlobComponent(name, 16);
    ASSERT_NE(id, 0u);
    std::vector<FieldInfo> oldFields = {FloatField("A", 0), FloatField("B", 4), FloatField("C", 8),
                                        FloatField("D", 12)};
    ComponentFieldRegistry::Register(id, oldFields, name);

    const EntityHandle e = world.CreateEntity();
    const float v[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    ASSERT_TRUE(world.SetComponentBytesImmediate(e, id, v, sizeof(v)));

    // Reload drops B; survivors compact down.
    std::vector<FieldInfo> newFields = {FloatField("A", 0), FloatField("C", 4), FloatField("D", 8)};
    const float defaults[3] = {0.0f, 0.0f, 0.0f};
    world.MigrateComponentLayout(ComponentLayoutChange{id, oldFields, 16, newFields, 12, Bytes(defaults, 3)});

    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(world.CaptureComponentBytes(e, id, bytes));
    ASSERT_EQ(bytes.size(), 12u);
    const float* f = reinterpret_cast<const float*>(bytes.data());
    EXPECT_FLOAT_EQ(f[0], 1.0f); // A carried
    EXPECT_FLOAT_EQ(f[1], 3.0f); // C moved down over dropped B
    EXPECT_FLOAT_EQ(f[2], 4.0f); // D moved down
}

// A same-named field whose type changed is RESET to its default (never a raw byte copy);
// same-typed neighbours still carry.
TEST(ComponentMigrationTest, TypeChangedFieldResetsNeighboursCarry)
{
    World world(nullptr);

    const std::string name = "MigTest::Retyped";
    const ComponentTypeId id = ComponentRegistry::RegisterBlobComponent(name, 12);
    ASSERT_NE(id, 0u);
    std::vector<FieldInfo> oldFields = {FloatField("A", 0), FloatField("B", 4), FloatField("C", 8)};
    ComponentFieldRegistry::Register(id, oldFields, name);

    const EntityHandle e = world.CreateEntity();
    const float v[3] = {1.0f, 2.0f, 3.0f};
    ASSERT_TRUE(world.SetComponentBytesImmediate(e, id, v, sizeof(v)));

    // Reload retypes B (Float -> Int32, same size, same offset). Its float bits
    // must NOT be reinterpreted as an int; it takes the new default (7).
    std::vector<FieldInfo> newFields = {FloatField("A", 0),
                                        FieldInfo{"B", 4, 4u, FieldTypeId::Int32}, FloatField("C", 8)};
    float defaults[3] = {0.0f, 0.0f, 0.0f};
    reinterpret_cast<std::int32_t*>(defaults)[1] = 7;
    world.MigrateComponentLayout(ComponentLayoutChange{id, oldFields, 12, newFields, 12, Bytes(defaults, 3)});

    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(world.CaptureComponentBytes(e, id, bytes));
    ASSERT_EQ(bytes.size(), 12u);
    const float* f = reinterpret_cast<const float*>(bytes.data());
    EXPECT_FLOAT_EQ(f[0], 1.0f); // A carried
    EXPECT_EQ(reinterpret_cast<const std::int32_t*>(bytes.data())[1], 7) << "retyped field takes its default";
    EXPECT_FLOAT_EQ(f[2], 3.0f); // C carried
}

// Reordering fields (same 12B size) carries each value to its new offset (matched by name).
TEST(ComponentMigrationTest, ReorderedFieldsPreservedByName)
{
    World world(nullptr);

    const std::string name = "MigTest::Reorder";
    const ComponentTypeId id = ComponentRegistry::RegisterBlobComponent(name, 12);
    ASSERT_NE(id, 0u);
    std::vector<FieldInfo> oldFields = {FloatField("X", 0), FloatField("Y", 4), FloatField("Z", 8)};
    ComponentFieldRegistry::Register(id, oldFields, name);

    const EntityHandle e = world.CreateEntity();
    const float v[3] = {1.0f, 2.0f, 3.0f}; // X=1, Y=2, Z=3
    ASSERT_TRUE(world.SetComponentBytesImmediate(e, id, v, sizeof(v)));

    // Reload reorders to Z, Y, X (same size). New defaults are all zero.
    std::vector<FieldInfo> newFields = {FloatField("Z", 0), FloatField("Y", 4), FloatField("X", 8)};
    const float defaults[3] = {0.0f, 0.0f, 0.0f};
    world.MigrateComponentLayout(ComponentLayoutChange{id, oldFields, 12, newFields, 12, Bytes(defaults, 3)});

    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(world.CaptureComponentBytes(e, id, bytes));
    ASSERT_EQ(bytes.size(), 12u);
    const float* f = reinterpret_cast<const float*>(bytes.data());
    EXPECT_FLOAT_EQ(f[0], 3.0f); // Z moved to offset 0
    EXPECT_FLOAT_EQ(f[1], 2.0f); // Y stayed at offset 4
    EXPECT_FLOAT_EQ(f[2], 1.0f); // X moved to offset 8
}
