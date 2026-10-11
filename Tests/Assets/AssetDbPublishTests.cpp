#include <gtest/gtest.h>

#include "AssetDatabase/AssetStore_TextJsonl.h"
#include "Core/EngineLoggerBridge.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "TestTempDir.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#if defined(_WIN32)
namespace
{
class AssetDbPublish : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Root = GameEngine::TestUtils::MakeUniqueTempDirectory("ge_assetdb_publish");
        std::filesystem::create_directories(m_Root);
        m_File = m_Root / "AssetDatabase.assetdb";
        Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
        Logger::Log::Initialize({});
    }

    void TearDown() override
    {
        Logger::Log::Flush();
        std::error_code error;
        std::filesystem::remove_all(m_Root, error);
    }

    GameEngine::AssetDatabase::AssetRecord MakeRecord()
    {
        GameEngine::AssetDatabase::AssetRecord record{};
        record.guid = GameEngine::GUID::Generate();
        record.path = "scene.scene";
        record.type = GameEngine::AssetType::Scene;
        record.typeId = "Scene";
        return record;
    }

    std::string ReadContents()
    {
        std::ifstream input(m_File, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    }

    std::filesystem::path m_Root;
    std::filesystem::path m_File;
};
} // namespace

TEST_F(AssetDbPublish, EmptyStorePublishesAfterReadHandleIsReleased)
{
    { std::ofstream empty(m_File); }
    // The Windows C++ stream opens without delete sharing, as a scanner can.
    std::ifstream held(m_File, std::ios::binary);
    ASSERT_TRUE(held.is_open());
    GameEngine::AssetDatabase::AssetStore_TextJsonl store(nullptr);
    const auto record = MakeRecord();
    ASSERT_TRUE(store.UpsertAsset(record, nullptr));

    JobSystem::WorkStealingThreadPool workers(1);
    std::atomic<bool> started{false};
    bool saved = false;
    std::string error;
    auto save = workers.Submit([&] {
        started.store(true);
        saved = store.SaveToFile(m_File, &error);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!started.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_TRUE(started.load());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(save.IsCompleted()) << "The save must wait for the held destination";
    held.close();
    save.Wait();
    ASSERT_TRUE(saved) << error;

    GameEngine::AssetDatabase::AssetStore_TextJsonl reloaded(nullptr);
    ASSERT_TRUE(reloaded.LoadFromFile(m_File, &error)) << error;
    GameEngine::AssetDatabase::AssetRecord persisted{};
    ASSERT_TRUE(reloaded.TryGetAsset(record.guid, persisted));
    EXPECT_EQ(persisted.path, record.path);
    EXPECT_FALSE(std::filesystem::exists(m_File.string() + ".tmp"));
}

TEST_F(AssetDbPublish, PersistentReadHandleKeepsPreviousStoreAndAllowsLaterSave)
{
    GameEngine::AssetDatabase::AssetStore_TextJsonl store(nullptr);
    const auto record = MakeRecord();
    ASSERT_TRUE(store.UpsertAsset(record, nullptr));
    std::string error;
    ASSERT_TRUE(store.SaveToFile(m_File, &error)) << error;
    const auto previous = ReadContents();
    std::ifstream held(m_File, std::ios::binary);
    ASSERT_TRUE(held.is_open());
    // A save with nothing dirty leaves the file untouched, and a small dirty set
    // is appended in place, which a reader does not block. A dirty store with a
    // compaction threshold of one rewrites the file, so the held save has to
    // publish a replacement.
    store.SetCompactionThresholdForTesting(1u);
    const auto added = MakeRecord();
    ASSERT_TRUE(store.UpsertAsset(added, nullptr));

    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(store.SaveToFile(m_File, &error));
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));
    EXPECT_NE(error.find(m_File.string()), std::string::npos);
    EXPECT_EQ(ReadContents(), previous);
    EXPECT_FALSE(std::filesystem::exists(m_File.string() + ".tmp"));

    held.close();
    ASSERT_TRUE(store.SaveToFile(m_File, &error)) << error;
    EXPECT_NE(ReadContents(), previous);
    GameEngine::AssetDatabase::AssetStore_TextJsonl reloaded(nullptr);
    ASSERT_TRUE(reloaded.LoadFromFile(m_File, &error)) << error;
    GameEngine::AssetDatabase::AssetRecord persisted{};
    EXPECT_TRUE(reloaded.TryGetAsset(added.guid, persisted));
}

TEST_F(AssetDbPublish, FailedAppendKeepsTheChangeForTheNextSave)
{
    GameEngine::AssetDatabase::AssetStore_TextJsonl store(nullptr);
    ASSERT_TRUE(store.UpsertAsset(MakeRecord(), nullptr));
    std::string error;
    ASSERT_TRUE(store.SaveToFile(m_File, &error)) << error;
    const auto added = MakeRecord();
    ASSERT_TRUE(store.UpsertAsset(added, nullptr));

    constexpr auto kWrite = std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
                            std::filesystem::perms::others_write;
    std::filesystem::permissions(m_File, kWrite, std::filesystem::perm_options::remove);
    const bool savedReadOnly = store.SaveToFile(m_File, &error);
    std::filesystem::permissions(m_File, kWrite, std::filesystem::perm_options::add);
    EXPECT_FALSE(savedReadOnly);
    EXPECT_NE(error.find("for append"), std::string::npos) << error;

    ASSERT_TRUE(store.SaveToFile(m_File, &error)) << error;
    GameEngine::AssetDatabase::AssetStore_TextJsonl reloaded(nullptr);
    ASSERT_TRUE(reloaded.LoadFromFile(m_File, &error)) << error;
    GameEngine::AssetDatabase::AssetRecord persisted{};
    EXPECT_TRUE(reloaded.TryGetAsset(added.guid, persisted));
}

TEST_F(AssetDbPublish, SaveThatThrowsKeepsTheBatchForTheNextSave)
{
    GameEngine::AssetDatabase::AssetStore_TextJsonl store(nullptr);
    ASSERT_TRUE(store.UpsertAsset(MakeRecord(), nullptr));
    std::string error;
    ASSERT_TRUE(store.SaveToFile(m_File, &error)) << error;
    const auto kept = MakeRecord();
    ASSERT_TRUE(store.UpsertAsset(kept, nullptr));
    // A path that is not valid UTF-8 makes the JSON serializer throw mid-save.
    auto invalid = MakeRecord();
    invalid.path = "bad\xff\xfe.scene";
    ASSERT_TRUE(store.UpsertAsset(invalid, nullptr));
    EXPECT_ANY_THROW((void)store.SaveToFile(m_File, &error));

    invalid.path = "fixed.scene";
    ASSERT_TRUE(store.UpsertAsset(invalid, nullptr));
    ASSERT_TRUE(store.SaveToFile(m_File, &error)) << error;
    GameEngine::AssetDatabase::AssetStore_TextJsonl reloaded(nullptr);
    ASSERT_TRUE(reloaded.LoadFromFile(m_File, &error)) << error;
    GameEngine::AssetDatabase::AssetRecord persisted{};
    EXPECT_TRUE(reloaded.TryGetAsset(kept.guid, persisted));
}
#endif
