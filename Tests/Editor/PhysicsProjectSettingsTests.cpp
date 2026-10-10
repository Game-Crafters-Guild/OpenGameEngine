#include <gtest/gtest.h>

#include "Editor/Settings/PhysicsProjectSettings.h"

#include "../TestTempDir.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace
{
using namespace GameEngine;
namespace fs = std::filesystem;

// The Physics settings page owns four keys -- gravity, fixedTimeStep,
// maxSubSteps, collisionSteps -- but the block also carries keys only the world
// bootstrap reads (the Jolt capacity limits, numThreads, layerCollisionMatrix)
// and, in a project touched by a newer build, keys this build has never heard
// of. A save that rebuilds the block instead of merging into it deletes all of
// them, silently, on any slider drag.

// Written as a literal rather than built from the implementation's constants:
// a fixture that derives its expectations from the code under test cannot fail
// when that code changes its mind about what the block contains.
constexpr const char* kSeedSettingsJson = R"({
  "schemaVersion": 1,
  "physics": {
    "gravity": [0.0, -9.81, 0.0],
    "fixedTimeStep": 0.02,
    "maxSubSteps": 4,
    "collisionSteps": 1,
    "maxBodies": 12345,
    "maxBodyPairs": 23456,
    "maxContactConstraints": 34567,
    "tempAllocatorBytes": 45678901,
    "numThreads": 7,
    "layerCollisionMatrix": [
      1, 2, 4, 8, 16, 32, 64, 128,
      256, 512, 1024, 2048, 4096, 8192, 16384, 32768,
      65536, 131072, 262144, 524288, 1048576, 2097152, 4194304, 8388608,
      16777216, 33554432, 67108864, 134217728, 268435456, 536870912, 1073741824, 2147483648
    ],
    "keyFromANewerBuild": "must-survive"
  },
  "rendering": {
    "msaa": "off"
  }
})";

class PhysicsProjectSettingsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_WorkspaceRoot = TestUtils::MakeUniqueTempDirectory("ge_physics_project_settings");
        fs::create_directories(m_WorkspaceRoot / ".Editor");
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_WorkspaceRoot, ec);
    }

    fs::path SettingsFile() const { return m_WorkspaceRoot / ".Editor" / "ProjectSettings.json"; }

    void WriteSettingsFile(const std::string& contents) const
    {
        std::ofstream out(SettingsFile(), std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << contents;
    }

    nlohmann::json ReadSettingsFile() const
    {
        std::ifstream in(SettingsFile(), std::ios::binary);
        EXPECT_TRUE(in.is_open());
        return nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    }

    // Raw bytes, for the cases whose whole point is that the file was not rewritten.
    std::string ReadSettingsFileText() const
    {
        std::ifstream in(SettingsFile(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    fs::path m_WorkspaceRoot;
};

// The regression pin. Every key the page does not own must come back byte-for-byte.
TEST_F(PhysicsProjectSettingsTest, SaveKeepsKeysThePageDoesNotOwn)
{
    WriteSettingsFile(kSeedSettingsJson);

    Physics::PhysicsWorldSettings settings = Editor::PhysicsProjectSettings::Load(m_WorkspaceRoot);
    settings.gravity.y = -3.5f;
    ASSERT_TRUE(Editor::PhysicsProjectSettings::SaveSimulationSettings(m_WorkspaceRoot, settings));

    const nlohmann::json root = ReadSettingsFile();
    ASSERT_TRUE(root.is_object());
    ASSERT_TRUE(root.contains("physics"));
    const nlohmann::json& physics = root.at("physics");

    EXPECT_EQ(physics.value("maxBodies", 0u), 12345u);
    EXPECT_EQ(physics.value("maxBodyPairs", 0u), 23456u);
    EXPECT_EQ(physics.value("maxContactConstraints", 0u), 34567u);
    EXPECT_EQ(physics.value("tempAllocatorBytes", 0u), 45678901u);
    EXPECT_EQ(physics.value("numThreads", 0), 7);
    EXPECT_EQ(physics.value("keyFromANewerBuild", std::string{}), "must-survive");

    ASSERT_TRUE(physics.contains("layerCollisionMatrix"));
    const nlohmann::json& matrix = physics.at("layerCollisionMatrix");
    ASSERT_TRUE(matrix.is_array());
    ASSERT_EQ(matrix.size(), 32u);
    // Distinct per slot, so a merge that kept the array but zeroed or truncated
    // it fails here rather than passing on a size check alone.
    for (size_t i = 0; i < 32; ++i)
    {
        EXPECT_EQ(matrix[i].get<uint64_t>(), uint64_t{1} << i) << "layer mask " << i;
    }
}

// Blocks beside "physics" are equally not ours to rewrite.
TEST_F(PhysicsProjectSettingsTest, SaveKeepsSiblingTopLevelBlocks)
{
    WriteSettingsFile(kSeedSettingsJson);

    Physics::PhysicsWorldSettings settings = Editor::PhysicsProjectSettings::Load(m_WorkspaceRoot);
    settings.collisionSteps = 3;
    ASSERT_TRUE(Editor::PhysicsProjectSettings::SaveSimulationSettings(m_WorkspaceRoot, settings));

    const nlohmann::json root = ReadSettingsFile();
    ASSERT_TRUE(root.contains("rendering"));
    EXPECT_EQ(root.at("rendering").value("msaa", std::string{}), "off");
}

// The adjacent trap: merging must not make an owned key unwritable. All four
// are rewritten every save, so none can be left holding a stale value.
TEST_F(PhysicsProjectSettingsTest, SaveOverwritesEveryKeyThePageOwns)
{
    WriteSettingsFile(kSeedSettingsJson);

    Physics::PhysicsWorldSettings settings{};
    settings.gravity = {1.0f, 2.0f, 3.0f};
    settings.fixedTimeStep = 0.005f;
    settings.maxSubSteps = 11;
    settings.collisionSteps = 5;
    ASSERT_TRUE(Editor::PhysicsProjectSettings::SaveSimulationSettings(m_WorkspaceRoot, settings));

    const nlohmann::json root = ReadSettingsFile();
    const nlohmann::json& physics = root.at("physics");
    ASSERT_TRUE(physics.at("gravity").is_array());
    ASSERT_EQ(physics.at("gravity").size(), 3u);
    EXPECT_FLOAT_EQ(physics.at("gravity")[0].get<float>(), 1.0f);
    EXPECT_FLOAT_EQ(physics.at("gravity")[1].get<float>(), 2.0f);
    EXPECT_FLOAT_EQ(physics.at("gravity")[2].get<float>(), 3.0f);
    EXPECT_FLOAT_EQ(physics.at("fixedTimeStep").get<float>(), 0.005f);
    EXPECT_EQ(physics.at("maxSubSteps").get<int>(), 11);
    EXPECT_EQ(physics.at("collisionSteps").get<int>(), 5);
}

// The bug's cause was two readers disagreeing about how far the block extends.
// Load owns the whole block, so a save round-trip cannot narrow it.
TEST_F(PhysicsProjectSettingsTest, LoadReadsEveryKeyTheBlockCarries)
{
    WriteSettingsFile(kSeedSettingsJson);

    const Physics::PhysicsWorldSettings settings =
        Editor::PhysicsProjectSettings::Load(m_WorkspaceRoot);

    EXPECT_FLOAT_EQ(settings.gravity.y, -9.81f);
    EXPECT_FLOAT_EQ(settings.fixedTimeStep, 0.02f);
    EXPECT_EQ(settings.maxSubSteps, 4);
    EXPECT_EQ(settings.collisionSteps, 1);
    EXPECT_EQ(settings.maxBodies, 12345u);
    EXPECT_EQ(settings.maxBodyPairs, 23456u);
    EXPECT_EQ(settings.maxContactConstraints, 34567u);
    EXPECT_EQ(settings.tempAllocatorBytes, 45678901u);
    EXPECT_EQ(settings.numThreads, 7);
    for (uint32 i = 0; i < Physics::kMaxCollisionLayers; ++i)
    {
        EXPECT_EQ(settings.layerCollisionMatrix.mask[i], uint32{1} << i) << "layer mask " << i;
    }
}

// A project that never opened the page keeps the engine defaults rather than
// inheriting whatever the last project set.
TEST_F(PhysicsProjectSettingsTest, LoadFallsBackToDefaultsWhenTheBlockIsAbsent)
{
    WriteSettingsFile(R"({"schemaVersion": 1, "rendering": {"msaa": "off"}})");

    const Physics::PhysicsWorldSettings settings =
        Editor::PhysicsProjectSettings::Load(m_WorkspaceRoot);
    const Physics::PhysicsWorldSettings defaults{};

    EXPECT_FLOAT_EQ(settings.gravity.y, defaults.gravity.y);
    EXPECT_EQ(settings.maxBodies, defaults.maxBodies);
    EXPECT_EQ(settings.numThreads, defaults.numThreads);
}

TEST_F(PhysicsProjectSettingsTest, SaveCreatesTheBlockWhenAbsentAndKeepsTheRest)
{
    WriteSettingsFile(R"({"schemaVersion": 1, "rendering": {"msaa": "off"}})");

    Physics::PhysicsWorldSettings settings{};
    settings.maxSubSteps = 9;
    ASSERT_TRUE(Editor::PhysicsProjectSettings::SaveSimulationSettings(m_WorkspaceRoot, settings));

    const nlohmann::json root = ReadSettingsFile();
    EXPECT_EQ(root.at("physics").at("maxSubSteps").get<int>(), 9);
    EXPECT_EQ(root.at("rendering").value("msaa", std::string{}), "off");
}

// A hand-edited block of the wrong type has no keys worth keeping, so it is
// replaced -- but that must not take the rest of the document with it.
TEST_F(PhysicsProjectSettingsTest, SaveReplacesAPhysicsValueThatIsNotAnObject)
{
    WriteSettingsFile(R"({"schemaVersion": 1, "physics": "oops", "rendering": {"msaa": "off"}})");

    Physics::PhysicsWorldSettings settings{};
    settings.collisionSteps = 2;
    ASSERT_TRUE(Editor::PhysicsProjectSettings::SaveSimulationSettings(m_WorkspaceRoot, settings));

    const nlohmann::json root = ReadSettingsFile();
    ASSERT_TRUE(root.at("physics").is_object());
    EXPECT_EQ(root.at("physics").at("collisionSteps").get<int>(), 2);
    EXPECT_EQ(root.at("rendering").value("msaa", std::string{}), "off");
}

TEST_F(PhysicsProjectSettingsTest, MalformedKeysLeaveTheirDefaultsStanding)
{
    WriteSettingsFile(R"({
      "schemaVersion": 1,
      "physics": {
        "gravity": "not-an-array",
        "maxBodies": "not-a-number",
        "layerCollisionMatrix": [1, 2, 3],
        "collisionSteps": 6
      }
    })");

    const Physics::PhysicsWorldSettings settings =
        Editor::PhysicsProjectSettings::Load(m_WorkspaceRoot);
    const Physics::PhysicsWorldSettings defaults{};

    EXPECT_FLOAT_EQ(settings.gravity.y, defaults.gravity.y);
    EXPECT_EQ(settings.maxBodies, defaults.maxBodies);
    // Wrong length: a partial fill would leave a half-configured matrix.
    EXPECT_EQ(settings.layerCollisionMatrix.mask[0], defaults.layerCollisionMatrix.mask[0]);
    // Every value above is rejected by its whole-key type guard, so nothing here
    // reaches a read: this pins that a rejected key does not stop the keys after
    // it. A bad element inside an accepted array is the other shape --
    // MalformedGravityElementCostsOnlyGravity below.
    EXPECT_EQ(settings.collisionSteps, 6);
}

// Gravity's element reads are the first thing Load performs inside its try, so a
// single bad element is the input with the widest blast radius: unguarded, it
// throws past every key read after it -- including layerCollisionMatrix, whose
// default is collide-with-everything, making a typo a project-wide physics change.
TEST_F(PhysicsProjectSettingsTest, MalformedGravityElementCostsOnlyGravity)
{
    for (const char* badGravity : {R"([1.0, "two", 3.0])", "[1.0, null, 3.0]", "[1.0, true, 3.0]",
                                   "[1.0, {}, 3.0]", "[1.0, [], 3.0]"})
    {
        WriteSettingsFile(
            std::string(R"({"schemaVersion":1,"physics":{"gravity":)") + badGravity +
            R"(,"fixedTimeStep":0.02,"maxSubSteps":11,"collisionSteps":3,"maxBodies":12345,)"
            R"("maxBodyPairs":23456,"maxContactConstraints":34567,"tempAllocatorBytes":45678901,)"
            R"("numThreads":7,"layerCollisionMatrix":[1,2,4,8,16,32,64,128,256,512,1024,2048,)"
            R"(4096,8192,16384,32768,65536,131072,262144,524288,1048576,2097152,4194304,)"
            R"(8388608,16777216,33554432,67108864,134217728,268435456,536870912,1073741824,)"
            R"(2147483648]}})");

        const Physics::PhysicsWorldSettings settings =
            Editor::PhysicsProjectSettings::Load(m_WorkspaceRoot);
        const Physics::PhysicsWorldSettings defaults{};

        // Gravity is fully defaulted -- not half-applied from a partial read.
        EXPECT_FLOAT_EQ(settings.gravity.x, defaults.gravity.x) << badGravity;
        EXPECT_FLOAT_EQ(settings.gravity.y, defaults.gravity.y) << badGravity;
        EXPECT_FLOAT_EQ(settings.gravity.z, defaults.gravity.z) << badGravity;

        // Every other key is still honoured.
        EXPECT_FLOAT_EQ(settings.fixedTimeStep, 0.02f) << badGravity;
        EXPECT_EQ(settings.maxSubSteps, 11) << badGravity;
        EXPECT_EQ(settings.collisionSteps, 3) << badGravity;
        EXPECT_EQ(settings.maxBodies, 12345u) << badGravity;
        EXPECT_EQ(settings.maxBodyPairs, 23456u) << badGravity;
        EXPECT_EQ(settings.maxContactConstraints, 34567u) << badGravity;
        EXPECT_EQ(settings.tempAllocatorBytes, 45678901u) << badGravity;
        EXPECT_EQ(settings.numThreads, 7) << badGravity;
        for (uint32 i = 0; i < Physics::kMaxCollisionLayers; ++i)
        {
            EXPECT_EQ(settings.layerCollisionMatrix.mask[i], uint32{1} << i)
                << badGravity << " layer " << i;
        }
    }
}

// An unreadable document has no keys to merge into and no way to tell which of
// them the user still wants, so the save is refused rather than replacing the
// file with a four-key one. Saving anyway would cost every sibling block.
TEST_F(PhysicsProjectSettingsTest, SaveRefusesAnUnparseableDocumentAndLeavesItUntouched)
{
    const std::string corrupt =
        R"({ "schemaVersion": 1, "rendering": { "msaa": "off" }, "lod": { "bias": 1.5 )";
    WriteSettingsFile(corrupt);

    Physics::PhysicsWorldSettings settings{};
    settings.collisionSteps = 3;
    EXPECT_FALSE(Editor::PhysicsProjectSettings::SaveSimulationSettings(m_WorkspaceRoot, settings));
    EXPECT_EQ(ReadSettingsFileText(), corrupt) << "the corrupt file must be byte-identical afterwards";
}

// A merge-conflicted settings file is the realistic instance of the same case.
TEST_F(PhysicsProjectSettingsTest, SaveRefusesAMergeConflictedFileAndLeavesItUntouched)
{
    const std::string conflicted = "<<<<<<< HEAD\n{ \"rendering\": { \"msaa\": \"off\" } }\n"
                                   "=======\n{ \"rendering\": { \"msaa\": \"4x\" } }\n"
                                   ">>>>>>> theirs\n";
    WriteSettingsFile(conflicted);

    EXPECT_FALSE(Editor::PhysicsProjectSettings::SaveSimulationSettings(
        m_WorkspaceRoot, Physics::PhysicsWorldSettings{}));
    EXPECT_EQ(ReadSettingsFileText(), conflicted);
}

// A genuinely missing file is not the unreadable case: it loads as empty and
// must still save, or a project could never write its physics block at all.
TEST_F(PhysicsProjectSettingsTest, SaveStillCreatesTheFileWhenItDoesNotExist)
{
    std::error_code ec;
    fs::remove(SettingsFile(), ec);
    ASSERT_FALSE(fs::exists(SettingsFile()));

    Physics::PhysicsWorldSettings settings{};
    settings.maxSubSteps = 9;
    ASSERT_TRUE(Editor::PhysicsProjectSettings::SaveSimulationSettings(m_WorkspaceRoot, settings));

    const nlohmann::json root = ReadSettingsFile();
    ASSERT_TRUE(root.is_object());
    EXPECT_EQ(root.at("physics").at("maxSubSteps").get<int>(), 9);
}

// With no project open there is nowhere to write; the caller must not be told
// the settings were persisted.
TEST_F(PhysicsProjectSettingsTest, SaveRefusesAnEmptyWorkspaceRoot)
{
    EXPECT_FALSE(
        Editor::PhysicsProjectSettings::SaveSimulationSettings({}, Physics::PhysicsWorldSettings{}));
}

} // namespace
