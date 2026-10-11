// Large asset-database loads parse on the job pool: a JSON Lines store and a
// warm-start snapshot past the parallel threshold load the same records with a
// pool as without one, in file order, and a cross-process store's re-read under
// its write lock forks its parse onto the store's pool too.

#include <gtest/gtest.h>

#include "AssetDatabase/AssetSourceSnapshot.h"
#include "AssetDatabase/AssetStore_TextJsonl.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::AssetDatabase;

namespace
{

// Past both loaders' 4000-entry parallel threshold.
constexpr int kRecords = 6000;

std::string RecordPath(int i)
{
    return "Textures/tex_" + std::to_string(i) + ".png";
}

} // namespace

TEST(AssetDbParallelLoad, ALargeStoreLoadsTheSameRecordsOnThePool)
{
    const TestUtils::ScopedTempDir root(TestUtils::MakeUniqueTempDirectory("ge_assetdb_parallel_store"));
    const std::filesystem::path dbFile = root.Path() / "AssetDatabase.assetdb";
    std::vector<GUID> guids;
    {
        AssetStore_TextJsonl writer(nullptr);
        for (int i = 0; i < kRecords; ++i)
        {
            AssetRecord record;
            record.guid = GUID::Generate();
            record.path = RecordPath(i);
            record.typeId = "Texture";
            ASSERT_TRUE(writer.UpsertAsset(record, nullptr));
            guids.push_back(record.guid);
        }
        // A later line for the first GUID: last write wins across the parse split.
        AssetRecord moved;
        moved.guid = guids[0];
        moved.path = "Textures/moved.png";
        moved.typeId = "Texture";
        ASSERT_TRUE(writer.UpsertAsset(moved, nullptr));
        std::string err;
        ASSERT_TRUE(writer.SaveToFile(dbFile, &err)) << err;
    }

    JobSystem::WorkStealingThreadPool pool(4);
    AssetStore_TextJsonl onPool(&pool);
    AssetStore_TextJsonl onCaller(nullptr);
    std::string err;
    ASSERT_TRUE(onPool.LoadFromFile(dbFile, &err)) << err;
    ASSERT_TRUE(onCaller.LoadFromFile(dbFile, &err)) << err;

    EXPECT_EQ(onPool.CountAssets(), static_cast<size_t>(kRecords));
    EXPECT_EQ(onPool.CountAssets(), onCaller.CountAssets());
    for (int i = 0; i < kRecords; i += 97)
    {
        AssetRecord fromPool, fromCaller;
        ASSERT_TRUE(onPool.TryGetAsset(guids[i], fromPool)) << i;
        ASSERT_TRUE(onCaller.TryGetAsset(guids[i], fromCaller)) << i;
        EXPECT_EQ(fromPool.path, fromCaller.path) << i;
    }
    AssetRecord first;
    ASSERT_TRUE(onPool.TryGetAsset(guids[0], first));
    EXPECT_EQ(first.path, "Textures/moved.png");
}

// The re-read is a load of the whole file on the saving thread, under the
// cross-process lock: past the threshold it publishes its parse to the pool
// (the census's cumulative global pushes grow during the save).
TEST(AssetDbParallelLoad, ACrossProcessStoreReReadsALargeFileOnThePool)
{
    const TestUtils::ScopedTempDir root(TestUtils::MakeUniqueTempDirectory("ge_assetdb_parallel_reread"));
    const std::filesystem::path dbFile = root.Path() / ".assetmanifest";
    {
        AssetStore_TextJsonl writer(nullptr);
        for (int i = 0; i < kRecords; ++i)
        {
            AssetRecord record;
            record.guid = GUID::Generate();
            record.path = RecordPath(i);
            record.typeId = "Texture";
            ASSERT_TRUE(writer.UpsertAsset(record, nullptr));
        }
        std::string err;
        ASSERT_TRUE(writer.SaveToFile(dbFile, &err)) << err;
    }

    JobSystem::WorkStealingThreadPool pool(4);
    AssetStore_TextJsonl store(&pool);
    store.SetCrossProcessWriteLockFile(root.Path() / ".assetmanifest.lock");
    std::string err;
    ASSERT_TRUE(store.LoadFromFile(dbFile, &err)) << err;

    AssetRecord added;
    added.guid = GUID::Generate();
    added.path = "Textures/added.png";
    added.typeId = "Texture";
    ASSERT_TRUE(store.UpsertAsset(added, &err)) << err;
    const auto before = pool.GetStatistics();
    ASSERT_TRUE(store.SaveToFile(dbFile, &err, StoreSaveWait::WaitForPeers)) << err;
    const auto after = pool.GetStatistics();
    EXPECT_GT(after.GlobalPushes - before.GlobalPushes, 0u) << "the re-read under the write lock parsed on the saving thread";

    AssetStore_TextJsonl reloaded(nullptr);
    ASSERT_TRUE(reloaded.LoadFromFile(dbFile, &err)) << err;
    EXPECT_EQ(reloaded.CountAssets(), static_cast<size_t>(kRecords) + 1);
}

TEST(AssetDbParallelLoad, ALargeSnapshotLoadsTheSameRecordsOnThePool)
{
    const TestUtils::ScopedTempDir root(TestUtils::MakeUniqueTempDirectory("ge_assetdb_parallel_snapshot"));
    const std::filesystem::path snapshotFile = root.Path() / "watcher.snapshot.bin";
    std::vector<AssetSourceSnapshotRecord> records;
    for (int i = 0; i < kRecords; ++i)
        records.push_back({RecordPath(i), 1700000000 + i, 1024 + i, "id-" + std::to_string(i), "hash"});
    std::string err;
    ASSERT_TRUE(AssetSourceSnapshot::Save(snapshotFile, root.Path(), 0, records, {}, &err)) << err;

    JobSystem::WorkStealingThreadPool pool(4);
    std::filesystem::path mountRoot;
    uint64_t ignoreSignature = 0;
    std::vector<AssetSourceSnapshotRecord> loaded;
    std::vector<AssetSourceSnapshotDirectory> directories;
    ASSERT_TRUE(AssetSourceSnapshot::Load(snapshotFile, &pool, mountRoot, ignoreSignature, loaded, directories, &err))
        << err;
    ASSERT_EQ(loaded.size(), records.size());
    for (size_t i = 0; i < records.size(); ++i)
    {
        EXPECT_EQ(loaded[i].CanonicalPath, records[i].CanonicalPath) << i;
        EXPECT_EQ(loaded[i].Mtime, records[i].Mtime) << i;
        EXPECT_EQ(loaded[i].FileId, records[i].FileId) << i;
    }
}
