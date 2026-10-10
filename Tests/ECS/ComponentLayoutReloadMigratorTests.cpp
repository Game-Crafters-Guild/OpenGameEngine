// ComponentLayoutReloadMigrator — the native-reload half of the S8 layout
// migration (C12 module reload family). The migrator brackets a module load:
// snapshot field tables + sizes (owned name copies) before the new DLL
// overwrites the registries, then re-pack placed instances of every component
// whose layout changed. These tests drive the bracket at the registry level
// (no DLL): re-registering a field table + default bytes between Snapshot and
// Migrate IS what a reloaded module's registrars do.

#include "ECS/ComponentFactory.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ComponentLayoutReloadMigrator.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/ModuleRegistration.h"
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
struct StampScope
{
    StampScope(std::string_view moduleId, std::uint64_t generation)
    {
        SetActiveRegistrationModule(moduleId, generation);
    }
    ~StampScope() { ClearActiveRegistrationModule(); }
};

FieldInfo FloatField(std::string_view name, std::uint32_t offset)
{
    return FieldInfo{name, offset, 4u, FieldTypeId::Float};
}

std::vector<float> Floats(const World& world, EntityHandle e, ComponentTypeId id, std::size_t count)
{
    std::vector<std::uint8_t> bytes;
    if (!const_cast<World&>(world).CaptureComponentBytes(e, id, bytes) || bytes.size() < count * sizeof(float))
        return {};
    std::vector<float> out(count);
    std::memcpy(out.data(), bytes.data(), count * sizeof(float));
    return out;
}
} // namespace

// A reflected component's relayout carries same-named fields, defaults the new
// one, and a re-fired after-hook (the optimistic-load double fire) is a no-op.
TEST(ComponentLayoutReloadMigratorTest, ReflectedComponentCarriesByNameAndIsIdempotent)
{
    World world(nullptr);
    const std::string name = "MigratorTest::Reflected";
    const ComponentTypeId id = ComponentRegistry::RegisterBlobComponent(name, 12);
    ASSERT_NE(id, 0u);

    std::vector<FieldInfo> oldFields = {FloatField("A", 0), FloatField("B", 4), FloatField("C", 8)};
    ComponentFieldRegistry::RegisterOwned(id, oldFields, name);
    const float oldDefaults[3] = {0.0f, 0.0f, 0.0f};
    ComponentFactory::RegisterDefaultBytes(id, oldDefaults, sizeof(oldDefaults), /*addable=*/false);

    const EntityHandle e = world.CreateEntity();
    const float v[3] = {1.0f, 2.0f, 3.0f};
    ASSERT_TRUE(world.SetComponentBytesImmediate(e, id, v, sizeof(v)));

    ComponentLayoutReloadMigrator migrator;
    migrator.SnapshotLayouts(); // before-hook

    // "New DLL" registrars: B dropped, D appended, A/C keep their names.
    std::vector<FieldInfo> newFields = {FloatField("A", 0), FloatField("C", 4), FloatField("D", 8)};
    ComponentFieldRegistry::RegisterOwned(id, newFields, name);
    const float newDefaults[3] = {0.0f, 0.0f, 9.0f};
    ComponentFactory::RegisterDefaultBytes(id, newDefaults, sizeof(newDefaults), /*addable=*/false);

    const std::vector<ComponentTypeId> migrated = migrator.MigrateChangedLayouts(world);
    ASSERT_EQ(migrated.size(), 1u);
    EXPECT_EQ(migrated[0], id);

    const std::vector<float> f = Floats(world, e, id, 3);
    ASSERT_EQ(f.size(), 3u);
    EXPECT_FLOAT_EQ(f[0], 1.0f); // A carried
    EXPECT_FLOAT_EQ(f[1], 3.0f); // C carried to its new offset
    EXPECT_FLOAT_EQ(f[2], 9.0f); // D default-initialized

    // Double fire must not re-remap the already-migrated bytes.
    EXPECT_TRUE(migrator.MigrateChangedLayouts(world).empty());
    const std::vector<float> f2 = Floats(world, e, id, 3);
    ASSERT_EQ(f2.size(), 3u);
    EXPECT_FLOAT_EQ(f2[0], 1.0f);
    EXPECT_FLOAT_EQ(f2[1], 3.0f);
    EXPECT_FLOAT_EQ(f2[2], 9.0f);
}

// A component with default bytes but no field table cannot carry fields: a
// size change resets every placed instance to the new defaults (loudly).
TEST(ComponentLayoutReloadMigratorTest, NoFieldTableSizeChangeResetsToDefaults)
{
    World world(nullptr);
    const std::string name = "MigratorTest::Opaque";
    const ComponentTypeId id = ComponentRegistry::RegisterBlobComponent(name, 8);
    ASSERT_NE(id, 0u);
    const float oldDefaults[2] = {0.0f, 0.0f};
    ComponentFactory::RegisterDefaultBytes(id, oldDefaults, sizeof(oldDefaults), /*addable=*/false);

    const EntityHandle e = world.CreateEntity();
    const float v[2] = {1.0f, 2.0f};
    ASSERT_TRUE(world.SetComponentBytesImmediate(e, id, v, sizeof(v)));

    ComponentLayoutReloadMigrator migrator;
    migrator.SnapshotLayouts();

    const float newDefaults[3] = {5.0f, 6.0f, 7.0f}; // grew to 12B, still no field table
    ComponentFactory::RegisterDefaultBytes(id, newDefaults, sizeof(newDefaults), /*addable=*/false);

    ASSERT_EQ(migrator.MigrateChangedLayouts(world).size(), 1u);
    const std::vector<float> f = Floats(world, e, id, 3);
    ASSERT_EQ(f.size(), 3u);
    EXPECT_FLOAT_EQ(f[0], 5.0f); // no names to match — full reset to defaults
    EXPECT_FLOAT_EQ(f[1], 6.0f);
    EXPECT_FLOAT_EQ(f[2], 7.0f);
}

namespace
{
// Typed (non-blob) components for the module-typed coverage tests. Trivially
// copyable + standard layout, as the Component concept requires.
struct TypedReloadComp
{
    float A = 1.5f;
    float B = 2.5f;
};
struct TypedNeighborComp
{
    std::int32_t Sentinel = 0;
};
struct EngineTypedComp
{
    float Value = 0.0f;
};
} // namespace

// RegisterComponent<T> under a module bracket must record default bytes —
// the type joins ComponentFactory::DefaultByteTypes() (= the migration set)
// with no author action. Engine (unstamped) registrations stay out.
TEST(ComponentLayoutReloadMigratorTest, ModuleTypedRegistrationJoinsMigrationSet)
{
    // Unstamped: engine-style registration records nothing.
    ComponentRegistry::RegisterComponent<EngineTypedComp>("MigratorTest::EngineTyped");
    std::vector<std::uint8_t> bytes;
    EXPECT_FALSE(
        ComponentFactory::GetDefaultBytes(GetComponentTypeId<EngineTypedComp>(), bytes))
        << "engine components cannot reload — they must stay out of the migration set";

    // Module-stamped: defaults recorded automatically at sizeof(T).
    {
        StampScope stamp("TypedPack", 1);
        ComponentRegistry::RegisterComponent<TypedReloadComp>("MigratorTest::TypedReload");
    }
    const ComponentTypeId id = GetComponentTypeId<TypedReloadComp>();
    ASSERT_TRUE(ComponentFactory::GetDefaultBytes(id, bytes));
    ASSERT_EQ(bytes.size(), sizeof(TypedReloadComp));
    const TypedReloadComp expected{};
    EXPECT_EQ(std::memcmp(bytes.data(), &expected, sizeof(expected)), 0)
        << "recorded bytes must be the default-constructed instance";

    // Reload re-own (newer generation) refreshes the recorded defaults.
    {
        StampScope stamp("TypedPack", 2);
        ComponentRegistry::RegisterComponent<TypedReloadComp>("MigratorTest::TypedReload");
    }
    ASSERT_TRUE(ComponentFactory::GetDefaultBytes(id, bytes));
    EXPECT_EQ(bytes.size(), sizeof(TypedReloadComp));
}

// The corruption scenario from the C12 findings: a typed-only module component
// reloads at a DIFFERENT size. With the structural auto-defaults it is now in
// the migration set, so the bracket resets it loudly at the new size and
// updates the world stride — and the archetype neighbours survive intact
// (the old behavior left stride vs recorded size disagreeing: corruption).
//
// The reloading component is registered ONLY inside the module bracket and
// driven through the type-erased APIs: any typed World call would instantiate
// AutoComponentRegistrar<T>, whose static init pre-registers the type
// UNSTAMPED at process start. In a real module DLL that same static init runs
// during LoadLibrary INSIDE the loader bracket (stamped) — and it routes
// through RegisterComponent<T>, so the auto-defaults hook covers it too.
TEST(ComponentLayoutReloadMigratorTest, TypedOnlySizeChangeResetsInsteadOfCorrupting)
{
    World world(nullptr);
    struct TypedGrowsComp
    {
        float A = 3.0f;
        float B = 4.0f;
    };
    {
        StampScope stamp("TypedPack2", 1);
        ComponentRegistry::RegisterComponent<TypedGrowsComp>("MigratorTest::TypedGrows");
    }
    const ComponentTypeId id = GetComponentTypeId<TypedGrowsComp>();
    {
        std::vector<std::uint8_t> check;
        ASSERT_TRUE(ComponentFactory::GetDefaultBytes(id, check))
            << "typed module registration must auto-record defaults (the structural invariant)";
        ASSERT_EQ(check.size(), sizeof(TypedGrowsComp));
    }

    const EntityHandle e = world.CreateEntity();
    const float placed[2] = {7.0f, 8.0f};
    ASSERT_TRUE(world.SetComponentBytesImmediate(e, id, placed, sizeof(placed)));
    world.AddComponentImmediate(e, TypedNeighborComp{42}); // host component, auto-registered

    ComponentLayoutReloadMigrator migrator;
    migrator.SnapshotLayouts(); // before-hook: records the 8B layout

    // "New DLL" registrars under generation 2: the typed re-registration
    // re-owns the handler, and its auto-recorded defaults land at the NEW
    // sizeof(T). The same type cannot change sizeof in-process, so the test
    // stands in for the new compile with an explicit new-size default record —
    // exactly the call RecordModuleTypedDefaults makes with the recompiled T.
    {
        StampScope stamp("TypedPack2", 2);
        ComponentRegistry::RegisterComponent<TypedGrowsComp>("MigratorTest::TypedGrows");
        const float grownDefaults[3] = {9.0f, 10.0f, 11.0f}; // grew 8B -> 12B
        ComponentFactory::RegisterDefaultBytes(id, grownDefaults, sizeof(grownDefaults),
                                               /*addable=*/false);
    }

    const std::vector<ComponentTypeId> migrated = migrator.MigrateChangedLayouts(world);
    ASSERT_EQ(migrated.size(), 1u) << "size change must be DETECTED, never silent";
    EXPECT_EQ(migrated[0], id);

    // The neighbour sharing the archetype row must be untouched by the resize.
    const auto* neighbor = world.GetComponent<TypedNeighborComp>(e);
    ASSERT_NE(neighbor, nullptr);
    EXPECT_EQ(neighbor->Sentinel, 42) << "archetype rebuild must carry neighbour columns intact";

    // The reset landed in storage: the in-test handler still reports the old
    // 8-byte sizeof (the recompiled type does not exist in this process), so a
    // capture returns the cell's first 8 bytes — which must be the NEW
    // defaults' prefix, not the placed values.
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(world.CaptureComponentBytes(e, id, bytes));
    ASSERT_EQ(bytes.size(), sizeof(TypedGrowsComp));
    const float* f = reinterpret_cast<const float*>(bytes.data());
    EXPECT_FLOAT_EQ(f[0], 9.0f);
    EXPECT_FLOAT_EQ(f[1], 10.0f);

    // Idempotent: the after-hook double fire must not re-remap.
    EXPECT_TRUE(migrator.MigrateChangedLayouts(world).empty());

    // Process-global hygiene: later tests' snapshots sweep the same factory
    // map; restore the recorded defaults to the in-process handler's size so
    // the stand-in's 12B record cannot phantom-migrate in an unrelated test.
    const TypedGrowsComp inProcessDefaults{};
    ComponentFactory::RegisterDefaultBytes(id, &inProcessDefaults, sizeof(inProcessDefaults),
                                           /*addable=*/false);
}

// A reload while a SECOND live world holds the component: the across-worlds
// sweep (World::MigrateComponentLayoutAcrossWorlds via the live-world
// registry) migrates both, so no world keeps an old stride while the
// process-global handler reports the new size — the old primary-world-only
// precondition is gone, not guarded.
TEST(ComponentLayoutReloadMigratorTest, SecondLiveWorldMigratesWithTarget)
{
    World primary(nullptr);
    World second(nullptr);
    const std::string name = "MigratorTest::TwoWorlds";
    const ComponentTypeId id = ComponentRegistry::RegisterBlobComponent(name, 12);
    ASSERT_NE(id, 0u);
    std::vector<FieldInfo> oldFields = {FloatField("A", 0), FloatField("B", 4), FloatField("C", 8)};
    ComponentFieldRegistry::RegisterOwned(id, oldFields, name);
    const float oldDefaults[3] = {0.0f, 0.0f, 0.0f};
    ComponentFactory::RegisterDefaultBytes(id, oldDefaults, sizeof(oldDefaults), /*addable=*/false);

    const EntityHandle pe = primary.CreateEntity();
    const float pv[3] = {1.0f, 2.0f, 3.0f};
    ASSERT_TRUE(primary.SetComponentBytesImmediate(pe, id, pv, sizeof(pv)));
    const EntityHandle se = second.CreateEntity();
    const float sv[3] = {10.0f, 20.0f, 30.0f};
    ASSERT_TRUE(second.SetComponentBytesImmediate(se, id, sv, sizeof(sv)));

    ComponentLayoutReloadMigrator migrator;
    migrator.SnapshotLayouts();

    // New build: D appended (12 -> 16), default 7.
    std::vector<FieldInfo> newFields = {FloatField("A", 0), FloatField("B", 4), FloatField("C", 8),
                                        FloatField("D", 12)};
    ComponentFieldRegistry::RegisterOwned(id, newFields, name);
    const float newDefaults[4] = {0.0f, 0.0f, 0.0f, 7.0f};
    ComponentFactory::RegisterDefaultBytes(id, newDefaults, sizeof(newDefaults), /*addable=*/false);

    ASSERT_EQ(migrator.MigrateChangedLayouts(primary).size(), 1u);

    const auto check = [&](World& w, EntityHandle e, float a, float b, float c) {
        std::vector<std::uint8_t> bytes;
        ASSERT_TRUE(w.CaptureComponentBytes(e, id, bytes));
        ASSERT_EQ(bytes.size(), 16u);
        const float* f = reinterpret_cast<const float*>(bytes.data());
        EXPECT_FLOAT_EQ(f[0], a);
        EXPECT_FLOAT_EQ(f[1], b);
        EXPECT_FLOAT_EQ(f[2], c);
        EXPECT_FLOAT_EQ(f[3], 7.0f);
    };
    check(primary, pe, 1.0f, 2.0f, 3.0f);
    // The load-bearing assertion: the world the caller did NOT pass migrated
    // in the same sweep, carrying its own values by name.
    check(second, se, 10.0f, 20.0f, 30.0f);

    EXPECT_TRUE(migrator.MigrateChangedLayouts(primary).empty()) << "double fire stays a no-op";
}

// Nothing changed across the bracket: no migration, placed bytes untouched.
// (A SAME-SIZE relayout of a field-table-less component is indistinguishable
// from this case — that blindness is exactly what field reflection removes.)
TEST(ComponentLayoutReloadMigratorTest, UnchangedLayoutUntouched)
{
    World world(nullptr);
    const std::string name = "MigratorTest::Stable";
    const ComponentTypeId id = ComponentRegistry::RegisterBlobComponent(name, 8);
    ASSERT_NE(id, 0u);
    const float defaults[2] = {0.0f, 0.0f};
    ComponentFactory::RegisterDefaultBytes(id, defaults, sizeof(defaults), /*addable=*/false);

    const EntityHandle e = world.CreateEntity();
    const float v[2] = {4.0f, 8.0f};
    ASSERT_TRUE(world.SetComponentBytesImmediate(e, id, v, sizeof(v)));

    ComponentLayoutReloadMigrator migrator;
    migrator.SnapshotLayouts();
    ComponentFactory::RegisterDefaultBytes(id, defaults, sizeof(defaults), /*addable=*/false);

    EXPECT_TRUE(migrator.MigrateChangedLayouts(world).empty());
    const std::vector<float> f = Floats(world, e, id, 2);
    ASSERT_EQ(f.size(), 2u);
    EXPECT_FLOAT_EQ(f[0], 4.0f);
    EXPECT_FLOAT_EQ(f[1], 8.0f);
}
