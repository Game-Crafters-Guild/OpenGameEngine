// A C array of a reflected struct in a scene file.
//
// A fixed-capacity table of small records (a recipe's override list, say) is written one line per
// sub-field of each element that differs from the struct's defaults, keyed
// "<Field><index>.<SubField>", through the sub-field's own codec. An untouched table writes no line
// at all, and a filled one comes back slot-exact — the same contract the AssetGuid pools keep.

#include <gtest/gtest.h>

#include "ECS/ComponentFactory.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/Reflection.h"
#include "Scene/ReflectionSceneSchema.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneSchemaRegistry.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using namespace GameEngine;

enum class SceneStructArrayProbeKind : std::uint8_t
{
    None = 0,
    Open = 1,
    Pinned = 2,
};

struct SceneStructArrayProbeEntry
{
    std::uint32_t Point = 0;
    std::uint16_t Ordinal = 0;
    SceneStructArrayProbeKind Kind = SceneStructArrayProbeKind::None;
    std::uint8_t Slot = 0;

    bool operator==(const SceneStructArrayProbeEntry&) const = default;
};
GE_REFLECT(SceneStructArrayProbeEntry, Point, Ordinal, Kind, Slot);

inline constexpr std::uint32_t kSceneStructArrayProbeCapacity = 4;

struct SceneStructArrayProbe
{
    std::uint32_t Seed = 3;
    SceneStructArrayProbeEntry Entries[kSceneStructArrayProbeCapacity];
};
GE_REFLECT(SceneStructArrayProbe, Seed, Entries);

// A table whose owner defaults one element away from the element struct's own default: the
// reader starts a component from the owner's default bytes, so that is what "unchanged" means.
struct SceneStructArrayOwnerDefaultProbe
{
    SceneStructArrayProbeEntry Entries[2] = {{}, {11u, 0u, SceneStructArrayProbeKind::Open, 0u}};
};
GE_REFLECT(SceneStructArrayOwnerDefaultProbe, Entries);

// A table inside a table.
struct SceneStructArrayLeaf
{
    std::uint32_t A = 0;
    std::uint32_t B = 0;

    bool operator==(const SceneStructArrayLeaf&) const = default;
};
GE_REFLECT(SceneStructArrayLeaf, A, B);

struct SceneStructArrayBranch
{
    std::uint32_t Id = 0;
    SceneStructArrayLeaf Leaves[2];
};
GE_REFLECT(SceneStructArrayBranch, Id, Leaves);

struct SceneStructArrayTree
{
    SceneStructArrayBranch Branches[2];
};
GE_REFLECT(SceneStructArrayTree, Branches);

namespace
{

constexpr ECS::EnumNameValue kProbeKindNames[] = {
    {"None", 0},
    {"Open", 1},
    {"Pinned", 2},
};

// Registers one reflected probe type's field table and default bytes, and a scene schema for it
// when it is a component the scenes carry.
template <class T>
void RegisterProbe(const char* sceneName, bool addable)
{
    const auto type = ECS::GetComponentTypeId<T>();
    ECS::ComponentFieldRegistry::Register(type, ECS::Reflection<T>::Fields, ECS::ComponentTypeName<T>());
    const T defaults{};
    ECS::ComponentFactory::RegisterDefaultBytes(type, &defaults, sizeof(defaults), addable);
    if (sceneName)
        Scene::SceneSchemaRegistry::Register(std::make_unique<Scene::ReflectionSceneSchema>(type, sceneName));
}

// The registries are process-wide, so the probes join them once.
void EnsureProbesRegistered()
{
    static const bool registered = []
    {
        RegisterProbe<SceneStructArrayOwnerDefaultProbe>("SceneStructArrayOwnerDefaultProbe", true);
        RegisterProbe<SceneStructArrayLeaf>(nullptr, false);
        RegisterProbe<SceneStructArrayBranch>(nullptr, false);
        RegisterProbe<SceneStructArrayTree>("SceneStructArrayTree", true);
        const auto entryType = ECS::GetComponentTypeId<SceneStructArrayProbeEntry>();
        ECS::ComponentFieldRegistry::Register(entryType, ECS::Reflection<SceneStructArrayProbeEntry>::Fields,
                                              ECS::ComponentTypeName<SceneStructArrayProbeEntry>());
        const SceneStructArrayProbeEntry entryDefaults{};
        ECS::ComponentFactory::RegisterDefaultBytes(entryType, &entryDefaults, sizeof(entryDefaults), false);
        ECS::ComponentFieldRegistry::SetFieldEnum(entryType, "Kind", kProbeKindNames);

        const auto probeType = ECS::GetComponentTypeId<SceneStructArrayProbe>();
        ECS::ComponentFieldRegistry::Register(probeType, ECS::Reflection<SceneStructArrayProbe>::Fields,
                                              ECS::ComponentTypeName<SceneStructArrayProbe>());
        const SceneStructArrayProbe probeDefaults{};
        ECS::ComponentFactory::RegisterDefaultBytes(probeType, &probeDefaults, sizeof(probeDefaults), true);
        Scene::SceneSchemaRegistry::Register(
            std::make_unique<Scene::ReflectionSceneSchema>(probeType, "SceneStructArrayProbe"));
        return true;
    }();
    ASSERT_TRUE(registered);
}

class SceneStructArray : public testing::Test
{
  protected:
    std::filesystem::path directory;

    void SetUp() override
    {
        EnsureProbesRegistered();
        directory = std::filesystem::temp_directory_path() /
                    ("SceneStructArrayTests_" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    static std::string ReadText(const std::filesystem::path& path)
    {
        std::ifstream file(path);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    static std::string EntityBlock(const std::string& text)
    {
        const std::size_t at = text.find("[entity");
        return at == std::string::npos ? std::string() : text.substr(at);
    }

    static void WriteText(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    }

    static SceneStructArrayProbe OnlyProbe(ECS::World& world)
    {
        SceneStructArrayProbe result{};
        int count = 0;
        world.Query<ECS::Read<SceneStructArrayProbe>>().Each(
            [&](const SceneStructArrayProbe& value)
            {
                result = value;
                ++count;
            });
        EXPECT_EQ(count, 1);
        return result;
    }

    // Saves `probe` alone and returns the file's text.
    std::string SaveProbe(const SceneStructArrayProbe& probe, const std::filesystem::path& path)
    {
        ECS::World source;
        source.Create(probe);
        EXPECT_TRUE(Scene::SaveSceneToFile(source, path));
        return ReadText(path);
    }
};

TEST_F(SceneStructArray, ReflectionNamesTheElementStructOfAStructArrayOnly)
{
    const auto fields = ECS::GetReflectedFields<SceneStructArrayProbe>();
    ASSERT_EQ(fields.size(), 2u);
    EXPECT_EQ(fields[0].ElementStruct, 0u) << "a scalar names no element struct";
    EXPECT_EQ(fields[1].ElementStruct, ECS::GetComponentTypeId<SceneStructArrayProbeEntry>());
    for (const ECS::FieldInfo& field : ECS::GetReflectedFields<SceneStructArrayProbeEntry>())
        EXPECT_EQ(field.ElementStruct, 0u) << field.Name;
}

TEST_F(SceneStructArray, FilledElementsRoundTripSlotExactAndDefaultElementsWriteNoLine)
{
    SceneStructArrayProbe authored{};
    authored.Seed = 9;
    authored.Entries[1] = {7u, 2u, SceneStructArrayProbeKind::Pinned, 5u};
    authored.Entries[3] = {12u, 0u, SceneStructArrayProbeKind::Open, 0u};
    const auto path = directory / "struct_array.scene";
    const std::string text = SaveProbe(authored, path);

    EXPECT_NE(text.find("SceneStructArrayProbe.Entries1.Point = 7"), std::string::npos) << text;
    EXPECT_NE(text.find("SceneStructArrayProbe.Entries1.Ordinal = 2"), std::string::npos) << text;
    EXPECT_NE(text.find("SceneStructArrayProbe.Entries1.Kind = Pinned"), std::string::npos) << text;
    EXPECT_NE(text.find("SceneStructArrayProbe.Entries1.Slot = 5"), std::string::npos) << text;
    // Every sub-field of a written element is written, zeros included, so the element loads the
    // same whatever the struct's defaults become.
    EXPECT_NE(text.find("SceneStructArrayProbe.Entries3.Ordinal = 0"), std::string::npos) << text;
    EXPECT_EQ(text.find("Entries0."), std::string::npos) << "a default element wrote a line:\n" << text;
    EXPECT_EQ(text.find("Entries2."), std::string::npos) << "a default element wrote a line:\n" << text;

    for (int pass = 0; pass < 2; ++pass)
    {
        ECS::World loaded;
        Scene::SceneLoadDegradation degradation;
        Scene::LoadOptions options;
        options.outDegradation = &degradation;
        ASSERT_TRUE(Scene::LoadSceneFromFile(loaded, path, options)) << Scene::GetLastSceneIOError().message;
        EXPECT_TRUE(degradation.skips.empty());
        const SceneStructArrayProbe actual = OnlyProbe(loaded);
        EXPECT_EQ(actual.Seed, authored.Seed);
        for (std::uint32_t i = 0; i < kSceneStructArrayProbeCapacity; ++i)
            EXPECT_EQ(actual.Entries[i], authored.Entries[i]) << "element " << i;
        ASSERT_TRUE(Scene::SaveSceneToFile(loaded, path));
        // The entity block, not the whole file: the scene header's spacing is the
        // writer's business and not what this test is about.
        EXPECT_EQ(EntityBlock(ReadText(path)), EntityBlock(text)) << "a resave changed the entity";
    }
}

TEST_F(SceneStructArray, UntouchedTableWritesNothing)
{
    SceneStructArrayProbe authored{};
    authored.Seed = 11; // something to save
    const std::string text = SaveProbe(authored, directory / "untouched.scene");
    EXPECT_NE(text.find("SceneStructArrayProbe.Seed = 11"), std::string::npos) << text;
    EXPECT_EQ(text.find("Entries"), std::string::npos) << text;
}

TEST_F(SceneStructArray, OutOfRangeIndexAndUnknownSubFieldAreSkippedWithoutTouchingOtherElements)
{
    SceneStructArrayProbe authored{};
    authored.Entries[0] = {4u, 1u, SceneStructArrayProbeKind::Open, 0u};
    authored.Entries[1] = {6u, 0u, SceneStructArrayProbeKind::Open, 0u};
    const auto path = directory / "skips.scene";
    std::string text = SaveProbe(authored, path);

    // Element 1's point is retargeted past the capacity; element 1's slot is renamed to a
    // sub-field the struct does not have.
    const std::string pointLine = "SceneStructArrayProbe.Entries1.Point = 6";
    const std::string slotLine = "SceneStructArrayProbe.Entries1.Slot = 0";
    ASSERT_NE(text.find(pointLine), std::string::npos) << text;
    ASSERT_NE(text.find(slotLine), std::string::npos) << text;
    text.replace(text.find(pointLine), pointLine.size(), "SceneStructArrayProbe.Entries9.Point = 6");
    text.replace(text.find(slotLine), slotLine.size(), "SceneStructArrayProbe.Entries1.Width = 3");
    WriteText(path, text);

    ECS::World loaded;
    ASSERT_TRUE(Scene::LoadSceneFromFile(loaded, path)) << Scene::GetLastSceneIOError().message;
    const SceneStructArrayProbe actual = OnlyProbe(loaded);
    EXPECT_EQ(actual.Entries[0], authored.Entries[0]);
    EXPECT_EQ(actual.Entries[1].Point, 0u) << "the out-of-range line landed on a real element";
    EXPECT_EQ(actual.Entries[1].Kind, SceneStructArrayProbeKind::Open);
    for (std::uint32_t i = 2; i < kSceneStructArrayProbeCapacity; ++i)
        EXPECT_EQ(actual.Entries[i], SceneStructArrayProbeEntry{}) << "element " << i;
}

TEST_F(SceneStructArray, MalformedSubFieldValueIsReportedAsADegradedLoad)
{
    SceneStructArrayProbe authored{};
    authored.Entries[2] = {8u, 3u, SceneStructArrayProbeKind::Pinned, 1u};
    const auto path = directory / "malformed.scene";
    std::string text = SaveProbe(authored, path);
    const std::string ordinalLine = "SceneStructArrayProbe.Entries2.Ordinal = 3";
    ASSERT_NE(text.find(ordinalLine), std::string::npos) << text;
    text.replace(text.find(ordinalLine), ordinalLine.size(),
                 "SceneStructArrayProbe.Entries2.Ordinal = three");
    WriteText(path, text);

    ECS::World loaded;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions options;
    options.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(loaded, path, options)) << Scene::GetLastSceneIOError().message;
    EXPECT_TRUE(degradation.IsDegraded()) << "a malformed element value loaded silently";
}

// An element is written when it differs from what the owning component's default holds there,
// not from its struct's default: the reader starts from the owner's default, so an element
// authored back to its struct's default in a slot the owner defaults otherwise must be written,
// and an untouched element that equals the owner's default must not be.
TEST_F(SceneStructArray, ElementsAreComparedWithTheOwnersDefaultNotTheirStructs)
{
    SceneStructArrayOwnerDefaultProbe authored{};
    authored.Entries[1] = SceneStructArrayProbeEntry{};
    const auto path = directory / "owner_default.scene";
    ECS::World source;
    source.Create(authored);
    ASSERT_TRUE(Scene::SaveSceneToFile(source, path));
    const std::string text = ReadText(path);
    EXPECT_NE(text.find("SceneStructArrayOwnerDefaultProbe.Entries1.Point = 0"), std::string::npos) << text;
    EXPECT_EQ(text.find("Entries0."), std::string::npos)
        << "an element equal to the owner's default wrote a line:\n" << text;

    ECS::World loaded;
    ASSERT_TRUE(Scene::LoadSceneFromFile(loaded, path)) << Scene::GetLastSceneIOError().message;
    int count = 0;
    loaded.Query<ECS::Read<SceneStructArrayOwnerDefaultProbe>>().Each(
        [&](const SceneStructArrayOwnerDefaultProbe& value)
        {
            ++count;
            EXPECT_EQ(value.Entries[0], authored.Entries[0]);
            EXPECT_EQ(value.Entries[1], authored.Entries[1]) << "the authored element loaded back as the owner's default";
        });
    EXPECT_EQ(count, 1);

    // The untouched owner default writes nothing at all.
    ECS::World untouched;
    untouched.Create(SceneStructArrayOwnerDefaultProbe{});
    ASSERT_TRUE(Scene::SaveSceneToFile(untouched, path));
    EXPECT_EQ(ReadText(path).find("SceneStructArrayOwnerDefaultProbe.Entries"), std::string::npos) << ReadText(path);
}

// A table inside a table nests its keys one "<Field><index>." per level, and round-trips.
TEST_F(SceneStructArray, ATableInsideATableRoundTrips)
{
    SceneStructArrayTree authored{};
    authored.Branches[1].Id = 3;
    authored.Branches[1].Leaves[1] = {4u, 9u};
    const auto path = directory / "nested.scene";
    ECS::World source;
    source.Create(authored);
    ASSERT_TRUE(Scene::SaveSceneToFile(source, path));
    const std::string text = ReadText(path);
    EXPECT_NE(text.find("SceneStructArrayTree.Branches1.Id = 3"), std::string::npos) << text;
    EXPECT_NE(text.find("SceneStructArrayTree.Branches1.Leaves1.A = 4"), std::string::npos) << text;
    EXPECT_NE(text.find("SceneStructArrayTree.Branches1.Leaves1.B = 9"), std::string::npos) << text;
    EXPECT_EQ(text.find("Branches1.Leaves0."), std::string::npos) << text;
    EXPECT_EQ(text.find("Branches0."), std::string::npos) << text;

    ECS::World loaded;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions options;
    options.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(loaded, path, options)) << Scene::GetLastSceneIOError().message;
    EXPECT_TRUE(degradation.skips.empty());
    int count = 0;
    loaded.Query<ECS::Read<SceneStructArrayTree>>().Each(
        [&](const SceneStructArrayTree& value)
        {
            ++count;
            EXPECT_EQ(value.Branches[1].Id, 3u);
            EXPECT_EQ(value.Branches[1].Leaves[1], (SceneStructArrayLeaf{4u, 9u}));
            EXPECT_EQ(value.Branches[1].Leaves[0], SceneStructArrayLeaf{});
            EXPECT_EQ(value.Branches[0].Id, 0u);
        });
    EXPECT_EQ(count, 1);
}

} // namespace
