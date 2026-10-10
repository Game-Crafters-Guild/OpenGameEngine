// Round-trip tests for reflection-driven scene serialization: a component with NO hand-written
// ISceneComponentSchema serializes to/from .scene purely via its GE_REFLECT field table + the
// FieldSerializerRegistry (the same reflection the editor inspector uses), and a field marked
// FieldFlags::Transient is skipped.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "AssetCore/GUID.h"
#include "Components/AssetRef.h"
#include "Components/ComponentRegistration.h"
#include "Components/Name.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/UnresolvedComponentStore.h"
#include "ECS/World.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Types/Color.h"

namespace GeReflectTest
{
// A component with one of each common field type and one runtime-only field. It has NO hand-written
// scene schema, so it exercises the reflection fallback end to end.
struct RoundTrip
{
    bool Flag = false;
    std::int32_t Count = 0;
    float Scalar = 0.0f;
    GameEngine::Mathematics::Vector3 Position{};
    GameEngine::Mathematics::Quaternion Rotation{};
    GameEngine::ColorLinear Tint{};
    char Label[32] = {};
    std::uint64_t RuntimeHandle = 0; // transient: rebuilt at runtime, must not serialize
};

// A whole component that is runtime/derived — excluded from scene serialization entirely.
struct RuntimeOnly
{
    float X = 0.0f;
};

// A component holding a typed asset reference — exercises the AssetGuid field serializer via the
// AssetRef<T> -> FieldTypeId::AssetGuid reflection path.
struct AssetHolder
{
    GameEngine::Components::MaterialRef Mat;
    std::int32_t Tag = 0;
};

// A component with an enum field — exercises the enum-name serializer. The enum has a gap (Turbo=7)
// to prove value-based (not index-based) mapping. The macros below mirror exactly what the
// ComponentScanner emits for an enum field: a constexpr table + a GE_REFLECT_ENUM_FIELD binding.
enum class Mode : std::uint32_t { Off = 0, Low = 1, High = 2, Turbo = 7 };

struct EnumHolder
{
    Mode mode = Mode::Off;
    std::int32_t Tag = 0;
};

// A component holding an entity reference — exercises the EntityHandle field serializer: the handle
// is saved as the referenced entity's stable scene id and re-resolved to the new handle on load.
struct RefHolder
{
    GameEngine::ECS::EntityHandle Target{};
    std::int32_t Tag = 0;
};
} // namespace GeReflectTest

GE_REGISTER_COMPONENT(GeReflectTest::RoundTrip, Flag, Count, Scalar, Position, Rotation, Tint, Label, RuntimeHandle);
GE_REFLECT_FIELD_TRANSIENT(GeReflectTest::RoundTrip, RuntimeHandle);

GE_REGISTER_COMPONENT(GeReflectTest::RuntimeOnly, X);
GE_REFLECT_COMPONENT_DONOTSERIALIZE(GeReflectTest::RuntimeOnly);

GE_REGISTER_COMPONENT(GeReflectTest::AssetHolder, Mat, Tag);

GE_REFLECT_ENUM_TABLE_BEGIN(GeReflectTestMode)
    GE_REFLECT_ENUM_TABLE_VALUE("Off", 0)
    GE_REFLECT_ENUM_TABLE_VALUE("Low", 1)
    GE_REFLECT_ENUM_TABLE_VALUE("High", 2)
    GE_REFLECT_ENUM_TABLE_VALUE("Turbo", 7)
GE_REFLECT_ENUM_TABLE_END()
GE_REGISTER_COMPONENT(GeReflectTest::EnumHolder, mode, Tag);
GE_REFLECT_ENUM_FIELD(GeReflectTest::EnumHolder, mode, GeReflectTestMode)

GE_REGISTER_COMPONENT(GeReflectTest::RefHolder, Target, Tag);

namespace
{
using GameEngine::ECS::Entity;
using GameEngine::ECS::World;
using GameEngine::Components::Name;
using GameEngine::Components::Transform;
using GameEngine::Mathematics::Quaternion;
using GameEngine::Mathematics::Vector3;

class ReflectionSceneSerializationTests : public ::testing::Test
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

TEST_F(ReflectionSceneSerializationTests, UserComponentRoundTripsViaReflection)
{
    const auto scenePath = TempScene("ge_reflect_roundtrip.scene");

    {
        World world;
        Entity e = world.Create();
        e.Set(Transform::FromTRS(Vector3(0, 0, 0), Quaternion(1, 0, 0, 0), Vector3(1, 1, 1)));
        Name name{};
        std::strncpy(name.value, "Subject", sizeof(name.value) - 1);
        e.Set(name);

        GeReflectTest::RoundTrip rt{};
        rt.Flag = true;
        rt.Count = -42;
        rt.Scalar = 4.5f;
        rt.Position = Vector3(1.5f, -2.25f, 3.0f);
        rt.Rotation = Quaternion(0.5f, 0.5f, 0.5f, 0.5f);
        rt.Tint = GameEngine::ColorLinear(0.5f, 0.25f, 0.75f, 1.0f);
        std::strncpy(rt.Label, "hello", sizeof(rt.Label) - 1);
        rt.RuntimeHandle = 0xDEADBEEFu; // must NOT survive save
        e.Set(rt);

        world.ProcessCommands();
        ASSERT_TRUE(GameEngine::Scene::SaveSceneToFile(world, scenePath));
    }

    // The text contains the reflected fields and does NOT contain the transient one.
    const std::string text = ReadFile(scenePath);
    EXPECT_NE(text.find("RoundTrip.Flag = true"), std::string::npos) << text;
    EXPECT_NE(text.find("RoundTrip.Count = -42"), std::string::npos) << text;
    EXPECT_NE(text.find("RoundTrip.Label = \"hello\""), std::string::npos) << text;
    EXPECT_EQ(text.find("RuntimeHandle"), std::string::npos) << "transient field must not serialize";

    {
        World world;
        const bool loaded = GameEngine::Scene::LoadSceneFromFile(world, scenePath);
        ASSERT_TRUE(loaded) << "load error: " << GameEngine::Scene::GetLastSceneIOError().message
                            << " @ line " << GameEngine::Scene::GetLastSceneIOError().line;

        const auto handle = FindByName(world, "Subject");
        ASSERT_TRUE(handle.IsValid());
        const auto* rt = world.GetComponent<GeReflectTest::RoundTrip>(handle);
        ASSERT_NE(rt, nullptr);

        EXPECT_EQ(rt->Flag, true);
        EXPECT_EQ(rt->Count, -42);
        EXPECT_FLOAT_EQ(rt->Scalar, 4.5f);
        EXPECT_FLOAT_EQ(rt->Position.x, 1.5f);
        EXPECT_FLOAT_EQ(rt->Position.y, -2.25f);
        EXPECT_FLOAT_EQ(rt->Position.z, 3.0f);
        const Quaternion expectedRot(0.5f, 0.5f, 0.5f, 0.5f);
        EXPECT_EQ(std::memcmp(&rt->Rotation, &expectedRot, sizeof(rt->Rotation)), 0) << "quat round-trip";
        EXPECT_FLOAT_EQ(rt->Tint.r, 0.5f);
        EXPECT_FLOAT_EQ(rt->Tint.g, 0.25f);
        EXPECT_FLOAT_EQ(rt->Tint.b, 0.75f);
        EXPECT_FLOAT_EQ(rt->Tint.a, 1.0f);
        EXPECT_STREQ(rt->Label, "hello");
        EXPECT_EQ(rt->RuntimeHandle, 0u) << "transient field must load as its default, not the saved value";
    }

    std::error_code ec;
    std::filesystem::remove(scenePath, ec);
}

TEST_F(ReflectionSceneSerializationTests, NonSerializableComponentIsSkippedOnSaveAndLoad)
{
    const auto scenePath = TempScene("ge_reflect_nonserializable.scene");

    {
        World world;
        Entity e = world.Create();
        Name name{};
        std::strncpy(name.value, "Runtime", sizeof(name.value) - 1);
        e.Set(name);
        GeReflectTest::RuntimeOnly ro{};
        ro.X = 7.0f;
        e.Set(ro);
        world.ProcessCommands();
        ASSERT_TRUE(GameEngine::Scene::SaveSceneToFile(world, scenePath));
    }

    // The [NonSerializable] component is absent from the file; the entity (its Name) still saves.
    const std::string text = ReadFile(scenePath);
    EXPECT_EQ(text.find("RuntimeOnly"), std::string::npos) << "non-serializable component must not be saved";
    EXPECT_NE(text.find("Name.value = \"Runtime\""), std::string::npos) << text;

    // A scene referencing it loads without error (the component line is tolerated/skipped).
    {
        World world;
        const bool loaded = GameEngine::Scene::LoadSceneFromFile(world, scenePath);
        EXPECT_TRUE(loaded) << "load error: " << GameEngine::Scene::GetLastSceneIOError().message;
    }

    std::error_code ec;
    std::filesystem::remove(scenePath, ec);
}

// A component whose TYPE is not registered at load time (simulating a user native component whose
// module hasn't compiled yet) must be PRESERVED verbatim — captured into the World's side-table and
// re-emitted on save — rather than dropped. This is the data-safety guarantee against the load-order
// race. (The "becomes live once the module registers" half is the editor's re-apply pass.)
TEST_F(ReflectionSceneSerializationTests, UnknownComponentIsPreservedAndReEmitted)
{
    const auto scenePath = TempScene("ge_reflect_unknown_preserve.scene");

    // Author a minimal scene with one known entity, then textually inject a component whose type is
    // not registered anywhere — the loader cannot resolve it.
    {
        World world;
        Entity e = world.Create();
        Name name{};
        std::strncpy(name.value, "Subject", sizeof(name.value) - 1);
        e.Set(name);
        world.ProcessCommands();
        ASSERT_TRUE(GameEngine::Scene::SaveSceneToFile(world, scenePath));
    }
    {
        std::string text = ReadFile(scenePath);
        const auto hdr = text.find("[entity ");
        ASSERT_NE(hdr, std::string::npos) << text;
        const auto eol = text.find('\n', hdr);
        ASSERT_NE(eol, std::string::npos);
        text.insert(eol + 1, "UnknownGadget.Power = 9\nUnknownGadget.Mode = \"turbo\"\n");
        std::ofstream out(scenePath, std::ios::binary);
        out << text;
    }

    // Load: the unknown component is preserved (not dropped) and the load still succeeds.
    {
        World world;
        const bool loaded = GameEngine::Scene::LoadSceneFromFile(world, scenePath);
        ASSERT_TRUE(loaded) << "load error: " << GameEngine::Scene::GetLastSceneIOError().message;

        const auto handle = FindByName(world, "Subject");
        ASSERT_TRUE(handle.IsValid());

        const auto* store = world.TryGetUnresolvedComponents();
        ASSERT_NE(store, nullptr) << "unknown component should have been preserved into the side-table";
        auto it = store->Map().find(handle);
        ASSERT_NE(it, store->Map().end()) << "no preserved entry for the Subject entity";
        ASSERT_EQ(it->second.size(), 1u);
        EXPECT_EQ(it->second[0].Name, "UnknownGadget"); // authored casing is preserved
        EXPECT_EQ(it->second[0].Props.size(), 2u);

        // Re-save: the preserved component is re-emitted with the authored casing
        // (case-normalizing it would make the round-trip value-stable but not byte-stable).
        const auto rePath = TempScene("ge_reflect_unknown_preserve2.scene");
        ASSERT_TRUE(GameEngine::Scene::SaveSceneToFile(world, rePath));
        const std::string out = ReadFile(rePath);
        EXPECT_NE(out.find("UnknownGadget.Power = 9"), std::string::npos) << out;
        EXPECT_NE(out.find("UnknownGadget.Mode = \"turbo\""), std::string::npos) << out;
        std::error_code ec2;
        std::filesystem::remove(rePath, ec2);
    }

    std::error_code ec;
    std::filesystem::remove(scenePath, ec);
}

// P2 item 5 — disabled-package data preservation. A scene authored while a package was
// enabled carries components registered by that package's modules (native C++ components
// and C# blob components alike — both serialize as Name.Prop lines). When the package is
// disabled its modules never load, so NOTHING registers those types: every one must ride
// the UnresolvedComponentStore as an unresolved payload and survive repeated
// load -> save -> load cycles without losing a property — the exact guarantee that makes
// disabling a package non-destructive.
TEST_F(ReflectionSceneSerializationTests, DisabledPackageComponentsRoundTripWithoutLoss)
{
    const auto scenePath = TempScene("ge_reflect_disabled_pkg.scene");

    // Author a scene, then inject two "package components" textually — the loader
    // sees exactly what a package-authored scene contains once the package is off.
    {
        World world;
        Entity e = world.Create();
        Name name{};
        std::strncpy(name.value, "PkgSubject", sizeof(name.value) - 1);
        e.Set(name);
        world.ProcessCommands();
        ASSERT_TRUE(GameEngine::Scene::SaveSceneToFile(world, scenePath));

        std::string text = ReadFile(scenePath);
        const auto hdr = text.find("[entity ");
        ASSERT_NE(hdr, std::string::npos) << text;
        const auto eol = text.find('\n', hdr);
        ASSERT_NE(eol, std::string::npos);
        // Names chosen to be certainly unregistered in this process (a real
        // package's types are equally absent when the package is disabled).
        text.insert(eol + 1,
                    "PkgTestBuoyancy.Density = 1.25\n"
                    "PkgTestBuoyancy.Label = \"north buoy\"\n"
                    "PkgTestPack.WaveEmitter.Amplitude = 3\n");
        std::ofstream out(scenePath, std::ios::binary);
        out << text;
    }

    // Round-trip TWICE through worlds where the types stay unregistered — the
    // second pass is what proves the preserved payload itself re-loads cleanly.
    std::filesystem::path current = scenePath;
    std::string firstResave;
    for (int pass = 0; pass < 2; ++pass)
    {
        World world;
        ASSERT_TRUE(GameEngine::Scene::LoadSceneFromFile(world, current))
            << GameEngine::Scene::GetLastSceneIOError().message;

        const auto handle = FindByName(world, "PkgSubject");
        ASSERT_TRUE(handle.IsValid());
        const auto* store = world.TryGetUnresolvedComponents();
        ASSERT_NE(store, nullptr) << "pass " << pass;
        const auto it = store->Map().find(handle);
        ASSERT_NE(it, store->Map().end()) << "pass " << pass;
        // Both package components preserved, all three properties intact.
        std::string preservedNames;
        for (const auto& pc : it->second)
            preservedNames += pc.Name + "(" + std::to_string(pc.Props.size()) + ") ";
        ASSERT_EQ(it->second.size(), 2u) << "pass " << pass << " preserved: " << preservedNames;
        size_t props = 0;
        for (const auto& pc : it->second)
            props += pc.Props.size();
        EXPECT_EQ(props, 3u) << "pass " << pass;

        const auto next = TempScene(pass == 0 ? "ge_reflect_disabled_pkg2.scene"
                                              : "ge_reflect_disabled_pkg3.scene");
        ASSERT_TRUE(GameEngine::Scene::SaveSceneToFile(world, next));
        std::string out = ReadFile(next);
        EXPECT_NE(out.find("PkgTestBuoyancy.Density = 1.25"), std::string::npos) << out;
        EXPECT_NE(out.find("PkgTestBuoyancy.Label = \"north buoy\""), std::string::npos) << out;
        EXPECT_NE(out.find("PkgTestPack.WaveEmitter.Amplitude = 3"), std::string::npos) << out;
        // Byte-stability from the first entity on (the [scene] header carries the
        // filename-derived scene name, which legitimately differs per pass).
        const auto body = out.find("[entity ");
        ASSERT_NE(body, std::string::npos) << out;
        out.erase(0, body);
        if (pass == 0)
            firstResave = out;
        else
            EXPECT_EQ(out, firstResave) << "preserved payload must be stable across cycles";
        current = next;
    }

    std::error_code ec;
    std::filesystem::remove(scenePath, ec);
    std::filesystem::remove(TempScene("ge_reflect_disabled_pkg2.scene"), ec);
    std::filesystem::remove(TempScene("ge_reflect_disabled_pkg3.scene"), ec);
}

// A typed AssetRef<T> field reflects as FieldTypeId::AssetGuid and round-trips through the AssetGuid
// field serializer (the [path/guid] form). With no resolver in the test, a set ref serializes
// guid-only and the GUID bytes survive the round-trip; a null ref is omitted from the file.
TEST_F(ReflectionSceneSerializationTests, AssetRefRoundTripsViaReflection)
{
    const auto scenePath = TempScene("ge_reflect_assetref.scene");
    const GameEngine::GUID assetGuid = GameEngine::GUID::Generate();

    {
        World world;
        Entity set = world.Create();
        Name setName{};
        std::strncpy(setName.value, "Holder", sizeof(setName.value) - 1);
        set.Set(setName);
        GeReflectTest::AssetHolder h{};
        h.Mat.Set(assetGuid);
        h.Tag = 7;
        set.Set(h);

        Entity empty = world.Create();
        Name emptyName{};
        std::strncpy(emptyName.value, "EmptyHolder", sizeof(emptyName.value) - 1);
        empty.Set(emptyName);
        empty.Set(GeReflectTest::AssetHolder{}); // null Mat -> must be omitted

        world.ProcessCommands();
        ASSERT_TRUE(GameEngine::Scene::SaveSceneToFile(world, scenePath));
    }

    const std::string text = ReadFile(scenePath);
    EXPECT_NE(text.find("AssetHolder.Mat"), std::string::npos) << "set asset ref must serialize\n" << text;
    EXPECT_NE(text.find(assetGuid.ToString()), std::string::npos) << "the GUID must appear in the asset ref\n" << text;
    EXPECT_NE(text.find("AssetHolder.Tag = 7"), std::string::npos) << text;
    // The null-ref holder still saves its other fields, but the unset Mat line is omitted.
    EXPECT_NE(text.find("AssetHolder.Tag = 0"), std::string::npos) << "null-holder still serializes\n" << text;

    {
        World world;
        ASSERT_TRUE(GameEngine::Scene::LoadSceneFromFile(world, scenePath))
            << "load error: " << GameEngine::Scene::GetLastSceneIOError().message;

        const auto handle = FindByName(world, "Holder");
        ASSERT_TRUE(handle.IsValid());
        const auto* h = world.GetComponent<GeReflectTest::AssetHolder>(handle);
        ASSERT_NE(h, nullptr);
        EXPECT_EQ(h->Mat.ToGuid(), assetGuid) << "asset GUID bytes must round-trip";
        EXPECT_EQ(h->Tag, 7);

        const auto emptyHandle = FindByName(world, "EmptyHolder");
        ASSERT_TRUE(emptyHandle.IsValid());
        const auto* eh = world.GetComponent<GeReflectTest::AssetHolder>(emptyHandle);
        ASSERT_NE(eh, nullptr);
        EXPECT_TRUE(eh->Mat.IsNull()) << "omitted asset ref must load as null";
    }

    std::error_code ec;
    std::filesystem::remove(scenePath, ec);
}

// An enum field bound to a scanner-emitted name table serializes the enumerator NAME (by value, not
// index — Turbo=7 has a gap) and round-trips. An out-of-table value falls back to its integer.
TEST_F(ReflectionSceneSerializationTests, EnumFieldSerializesByNameAndRoundTrips)
{
    const auto scenePath = TempScene("ge_reflect_enum.scene");

    {
        World world;
        Entity e = world.Create();
        Name name{};
        std::strncpy(name.value, "Gadget", sizeof(name.value) - 1);
        e.Set(name);
        GeReflectTest::EnumHolder h{};
        h.mode = GeReflectTest::Mode::Turbo; // value 7, a gap in the enum
        h.Tag = 3;
        e.Set(h);

        Entity oob = world.Create();
        Name oobName{};
        std::strncpy(oobName.value, "OobGadget", sizeof(oobName.value) - 1);
        oob.Set(oobName);
        GeReflectTest::EnumHolder oobH{};
        oobH.mode = static_cast<GeReflectTest::Mode>(99); // not in the table
        oob.Set(oobH);

        world.ProcessCommands();
        ASSERT_TRUE(GameEngine::Scene::SaveSceneToFile(world, scenePath));
    }

    const std::string text = ReadFile(scenePath);
    EXPECT_NE(text.find("EnumHolder.mode = Turbo"), std::string::npos) << "enum must serialize by name\n" << text;
    EXPECT_NE(text.find("EnumHolder.mode = 99"), std::string::npos) << "out-of-table value falls back to integer\n" << text;

    {
        World world;
        ASSERT_TRUE(GameEngine::Scene::LoadSceneFromFile(world, scenePath))
            << "load error: " << GameEngine::Scene::GetLastSceneIOError().message;

        const auto* h = world.GetComponent<GeReflectTest::EnumHolder>(FindByName(world, "Gadget"));
        ASSERT_NE(h, nullptr);
        EXPECT_EQ(static_cast<int>(h->mode), 7) << "named enum value round-trips";
        EXPECT_EQ(h->Tag, 3);

        const auto* oob = world.GetComponent<GeReflectTest::EnumHolder>(FindByName(world, "OobGadget"));
        ASSERT_NE(oob, nullptr);
        EXPECT_EQ(static_cast<int>(oob->mode), 99) << "out-of-table value round-trips as its integer";
    }

    std::error_code ec;
    std::filesystem::remove(scenePath, ec);
}

// Backward compat: a scene written before name serialization stores the enum as a bare integer. The
// dual-read codec must still accept it (integer -> value).
TEST_F(ReflectionSceneSerializationTests, EnumFieldDualReadsLegacyInteger)
{
    const auto scenePath = TempScene("ge_reflect_enum_legacy.scene");

    {
        World world;
        Entity e = world.Create();
        Name name{};
        std::strncpy(name.value, "Legacy", sizeof(name.value) - 1);
        e.Set(name);
        e.Set(GeReflectTest::EnumHolder{});
        world.ProcessCommands();
        ASSERT_TRUE(GameEngine::Scene::SaveSceneToFile(world, scenePath));
    }
    // Rewrite the enum line to the legacy integer form (as an older scene file would have it).
    {
        std::string text = ReadFile(scenePath);
        const auto pos = text.find("EnumHolder.mode = ");
        ASSERT_NE(pos, std::string::npos) << text;
        const auto eol = text.find('\n', pos);
        text.replace(pos, eol - pos, "EnumHolder.mode = 2"); // legacy integer for High
        std::ofstream out(scenePath, std::ios::binary);
        out << text;
    }

    {
        World world;
        ASSERT_TRUE(GameEngine::Scene::LoadSceneFromFile(world, scenePath))
            << "load error: " << GameEngine::Scene::GetLastSceneIOError().message;
        const auto* h = world.GetComponent<GeReflectTest::EnumHolder>(FindByName(world, "Legacy"));
        ASSERT_NE(h, nullptr);
        EXPECT_EQ(static_cast<int>(h->mode), 2) << "legacy integer enum value must still load";
    }

    std::error_code ec;
    std::filesystem::remove(scenePath, ec);
}

// An EntityHandle field is saved as the referenced entity's stable scene id and re-resolved to the
// (new) handle on load — surviving the fact that the runtime id changes across a save/load. A null
// reference is omitted on save and loads back as an invalid handle.
TEST_F(ReflectionSceneSerializationTests, EntityHandleRoundTripsViaReflection)
{
    const auto scenePath = TempScene("ge_reflect_entityref.scene");

    {
        World world;
        Entity target = world.Create();
        Name targetName{};
        std::strncpy(targetName.value, "Target", sizeof(targetName.value) - 1);
        target.Set(targetName);

        Entity holder = world.Create();
        Name holderName{};
        std::strncpy(holderName.value, "Holder", sizeof(holderName.value) - 1);
        holder.Set(holderName);

        GeReflectTest::RefHolder h{};
        h.Target = target.GetHandle();
        h.Tag = 5;
        holder.Set(h);

        Entity empty = world.Create();
        Name emptyName{};
        std::strncpy(emptyName.value, "EmptyRef", sizeof(emptyName.value) - 1);
        empty.Set(emptyName);
        empty.Set(GeReflectTest::RefHolder{}); // null Target -> must be omitted

        world.ProcessCommands();
        ASSERT_TRUE(GameEngine::Scene::SaveSceneToFile(world, scenePath));
    }

    const std::string text = ReadFile(scenePath);
    EXPECT_NE(text.find("RefHolder.Target = "), std::string::npos) << "set entity ref must serialize\n" << text;
    EXPECT_NE(text.find("RefHolder.Tag = 5"), std::string::npos) << text;
    EXPECT_NE(text.find("RefHolder.Tag = 0"), std::string::npos) << "null-ref holder still serializes its other fields\n" << text;

    {
        World world;
        ASSERT_TRUE(GameEngine::Scene::LoadSceneFromFile(world, scenePath))
            << "load error: " << GameEngine::Scene::GetLastSceneIOError().message;

        const auto holder = FindByName(world, "Holder");
        const auto target = FindByName(world, "Target");
        ASSERT_TRUE(holder.IsValid());
        ASSERT_TRUE(target.IsValid());
        const auto* h = world.GetComponent<GeReflectTest::RefHolder>(holder);
        ASSERT_NE(h, nullptr);
        EXPECT_EQ(h->Target.id, target.id) << "entity ref must resolve to the reloaded target handle";
        EXPECT_EQ(h->Tag, 5);

        const auto emptyHandle = FindByName(world, "EmptyRef");
        ASSERT_TRUE(emptyHandle.IsValid());
        const auto* eh = world.GetComponent<GeReflectTest::RefHolder>(emptyHandle);
        ASSERT_NE(eh, nullptr);
        EXPECT_FALSE(eh->Target.IsValid()) << "omitted entity ref must load as an invalid handle";
    }

    std::error_code ec;
    std::filesystem::remove(scenePath, ec);
}
} // namespace
