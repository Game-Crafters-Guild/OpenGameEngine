// SettingsStore concurrency/merge contract: a store whose Load failed or went
// stale must never wipe keys another process wrote (the multi-editor
// Preferences.json clobber). Save merges per top-level key under an
// inter-process lock.

#include "Editor/Settings/SettingsStore.h"

#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using GameEngine::Editor::SettingsStore;

namespace
{

class SettingsStoreTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Dir = GameEngine::TestUtils::MakeUniqueTempDirectory("GameEngineSettingsStoreTests");
        fs::create_directories(m_Dir);
        m_File = m_Dir / "Preferences.json";
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_Dir, ec);
    }

    void WriteFile(const std::string& contents)
    {
        std::ofstream out(m_File, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << contents;
    }

    nlohmann::json ReadFileJson()
    {
        std::ifstream in(m_File, std::ios::binary);
        EXPECT_TRUE(in.is_open());
        return nlohmann::json::parse(in);
    }

    static constexpr const char* kFullPrefs = R"({
        "schemaVersion": 1,
        "lastProjectPath": "/projects/old",
        "toolbarVisible": true,
        "treeViewSize": 240
    })";

    fs::path m_Dir;
    fs::path m_File;
};

// The observed incident: an editor whose Load saw no file (Windows
// remove-then-rename window, transient IO) saves one key and the full prefs
// written by another editor in the meantime must survive.
TEST_F(SettingsStoreTests, EmptyLoadThenSavePreservesKeysWrittenMeanwhile)
{
    SettingsStore store(m_File);
    ASSERT_TRUE(store.Load());
    EXPECT_FALSE(store.Contains("toolbarVisible"));

    WriteFile(kFullPrefs);

    store.SetString("lastProjectPath", "/projects/new");
    ASSERT_TRUE(store.Save());

    const nlohmann::json onDisk = ReadFileJson();
    EXPECT_EQ(onDisk.value("lastProjectPath", ""), "/projects/new");
    EXPECT_TRUE(onDisk.value("toolbarVisible", false));
    EXPECT_EQ(onDisk.value("treeViewSize", 0), 240);
}

// Failed (unparseable) Load followed by Save, with the file valid again by
// save time: the merge must preserve every key we never loaded.
TEST_F(SettingsStoreTests, FailedLoadThenSavePreservesRecoveredDiskKeys)
{
    WriteFile("{ this is not json");

    SettingsStore store(m_File);
    std::string err;
    ASSERT_FALSE(store.Load(&err));

    WriteFile(kFullPrefs);

    store.SetString("lastProjectPath", "/projects/new");
    ASSERT_TRUE(store.Save());

    const nlohmann::json onDisk = ReadFileJson();
    EXPECT_EQ(onDisk.value("lastProjectPath", ""), "/projects/new");
    EXPECT_TRUE(onDisk.value("toolbarVisible", false));
    EXPECT_EQ(onDisk.value("treeViewSize", 0), 240);
}

// A file that is still corrupt at save time is overwritten with our state:
// recovery beats holding preferences hostage.
TEST_F(SettingsStoreTests, SaveOverwritesCorruptFileAsRecovery)
{
    WriteFile("{ this is not json");

    SettingsStore store(m_File);
    ASSERT_FALSE(store.Load());
    store.SetBool("toolbarVisible", true);
    ASSERT_TRUE(store.Save());

    const nlohmann::json onDisk = ReadFileJson();
    EXPECT_TRUE(onDisk.value("toolbarVisible", false));
    EXPECT_EQ(onDisk.value("schemaVersion", 0), 1);
}

// Two editors load, each changes a different key, both save: both changes and
// all untouched keys survive regardless of save order.
TEST_F(SettingsStoreTests, ConcurrentStoresMergePerKey)
{
    WriteFile(kFullPrefs);

    SettingsStore a(m_File);
    SettingsStore b(m_File);
    ASSERT_TRUE(a.Load());
    ASSERT_TRUE(b.Load());

    a.SetString("lastProjectPath", "/projects/from-a");
    ASSERT_TRUE(a.Save());

    b.SetInt64("treeViewSize", 300);
    ASSERT_TRUE(b.Save());

    const nlohmann::json onDisk = ReadFileJson();
    EXPECT_EQ(onDisk.value("lastProjectPath", ""), "/projects/from-a");
    EXPECT_EQ(onDisk.value("treeViewSize", 0), 300);
    EXPECT_TRUE(onDisk.value("toolbarVisible", false));

    // Save syncs the store with the merged on-disk state.
    std::string fromA;
    EXPECT_TRUE(b.TryGetString("lastProjectPath", fromA));
    EXPECT_EQ(fromA, "/projects/from-a");
}

// A key the caller explicitly removed after a successful Load is removed from
// disk; everything else survives.
TEST_F(SettingsStoreTests, RemoveDeletesOnlyThatKey)
{
    WriteFile(kFullPrefs);

    SettingsStore store(m_File);
    ASSERT_TRUE(store.Load());
    EXPECT_TRUE(store.Remove("toolbarVisible"));
    ASSERT_TRUE(store.Save());

    const nlohmann::json onDisk = ReadFileJson();
    EXPECT_FALSE(onDisk.contains("toolbarVisible"));
    EXPECT_EQ(onDisk.value("treeViewSize", 0), 240);
    EXPECT_EQ(onDisk.value("lastProjectPath", ""), "/projects/old");
}

#if !defined(_WIN32)
// An existing file that cannot be read back must not be overwritten; Save
// fails instead of clobbering. (Permission bits are not enforceable this way
// on Windows, so POSIX only.)
TEST_F(SettingsStoreTests, SaveRefusesWhenExistingFileUnreadable)
{
    WriteFile(kFullPrefs);

    SettingsStore store(m_File);
    ASSERT_TRUE(store.Load());
    store.SetString("lastProjectPath", "/projects/new");

    fs::permissions(m_File, fs::perms::none);
    std::string err;
    EXPECT_FALSE(store.Save(&err));
    EXPECT_FALSE(err.empty());

    fs::permissions(m_File, fs::perms::owner_read | fs::perms::owner_write);
    const nlohmann::json onDisk = ReadFileJson();
    EXPECT_EQ(onDisk.value("lastProjectPath", ""), "/projects/old");
}
#endif

} // namespace
