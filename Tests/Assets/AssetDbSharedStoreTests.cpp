// A store file more than one process can write (an engine package's committed
// .assetmanifest: every editor and developer Player built from one checkout
// resolves the same file). These pin the three decisions that make sharing it
// safe — the write lock excludes, a refused write keeps its deltas for the
// retry, and a write re-reads the file and merges rather than publishing a view
// this process has held since mount.
//
// ScopedFileLock.h pulls <windows.h>, whose ::GUID would make an unqualified
// GUID ambiguous under a using-directive, so this file qualifies its engine
// names instead of importing the namespace.

#include "FileSystem/ScopedFileLock.h"

#include <gtest/gtest.h>

#include "AssetDatabase/AssetStore_TextJsonl.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/Packages/PackageMounts.h"
#include "Assets/TextureCook.h"
#include "TestTempDir.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace
{
using GameEngine::AssetDatabase::AssetRecord;
using GameEngine::AssetDatabase::AssetStore_TextJsonl;
using GameEngine::FileSystem::ScopedFileLock;

// Record lines only — comments and the {format,version} header are structural.
size_t CountRecordLines(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    size_t count = 0;
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#' || line.find("\"format\"") != std::string::npos)
            continue;
        ++count;
    }
    return count;
}

AssetRecord MakeRecord(const char* guidText, const std::string& path)
{
    AssetRecord record{};
    record.guid = GameEngine::GUID(guidText);
    record.path = path;
    record.type = GameEngine::AssetType::Texture;
    record.typeId = "Texture";
    return record;
}

constexpr const char* kGuidA = "85dc98d2-9d86-4a50-b183-109a0b59c2f8";
constexpr const char* kGuidB = "17709c6e-89d6-4fca-81ba-efb6a856e81e";
constexpr const char* kGuidC = "3f8a1c22-5b4d-4e6f-9a01-7c2d8e5f1b39";

// A one-row v2 file, plus the store that opens it as a shared writer.
struct SharedStoreFixture
{
    std::filesystem::path Dir;
    std::filesystem::path DbFile;
    std::filesystem::path LockFile;

    explicit SharedStoreFixture(const char* name)
        : Dir(GameEngine::TestUtils::MakeUniqueTempDirectory(name))
        , DbFile(Dir / ".assetmanifest")
        , LockFile(Dir / ".assetmanifest.lock")
    {
        AssetStore_TextJsonl seed(nullptr);
        seed.UpsertAsset(MakeRecord(kGuidA, "textures/a.png"), nullptr);
        seed.SaveToFile(DbFile, nullptr);
    }

    void Open(AssetStore_TextJsonl& store) const
    {
        store.SetCrossProcessWriteLockFile(LockFile);
        ASSERT_TRUE(store.LoadFromFile(DbFile, nullptr));
    }
};

// Another process holding the store's write lock for a bounded moment: it
// signals once the lock is held, and releases when told to.
class PeerHoldingTheWriteLock
{
  public:
    explicit PeerHoldingTheWriteLock(std::filesystem::path lockFile)
        : m_Thread([this, lockFile = std::move(lockFile)] {
              ScopedFileLock lock(lockFile, ScopedFileLock::Mode::Exclusive, /*wait=*/true);
              m_Held.store(lock.IsLocked(), std::memory_order_release);
              m_Signalled.store(true, std::memory_order_release);
              while (!m_ReleaseRequested.load(std::memory_order_acquire))
                  std::this_thread::sleep_for(std::chrono::milliseconds(1));
          })
    {
        while (!m_Signalled.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    bool IsHoldingTheLock() const { return m_Held.load(std::memory_order_acquire); }

    // Hand the lock over after the caller is committed to waiting for it.
    void ReleaseAfter(std::chrono::milliseconds delay)
    {
        m_Releaser = std::thread([this, delay] {
            std::this_thread::sleep_for(delay);
            m_ReleaseRequested.store(true, std::memory_order_release);
        });
    }

    ~PeerHoldingTheWriteLock()
    {
        m_ReleaseRequested.store(true, std::memory_order_release);
        if (m_Releaser.joinable())
            m_Releaser.join();
        m_Thread.join();
    }

  private:
    std::atomic<bool> m_Held{false};
    std::atomic<bool> m_Signalled{false};
    std::atomic<bool> m_ReleaseRequested{false};
    std::thread m_Thread;
    std::thread m_Releaser;
};

constexpr std::chrono::milliseconds kPeerHoldTime{150};
} // namespace

// The write lock is held for the whole save and taken before the dirty drain, so
// a contended write costs the caller a retry, never the edit.
TEST(AssetDbSharedStore, ARefusedSaveKeepsItsDeltasForTheRetry)
{
    const SharedStoreFixture fixture("ge_shared_store_refusal");
    AssetStore_TextJsonl store(nullptr);
    fixture.Open(store);
    ASSERT_TRUE(store.UpsertAsset(MakeRecord(kGuidB, "textures/b.png"), nullptr));

    {
        ScopedFileLock heldByAnotherProcess(fixture.LockFile, ScopedFileLock::Mode::Exclusive,
                                            /*wait=*/true);
        ASSERT_TRUE(heldByAnotherProcess.IsLocked());
        std::string error;
        EXPECT_FALSE(store.SaveToFile(fixture.DbFile, &error));
        EXPECT_NE(error.find("Another process"), std::string::npos) << error;
        EXPECT_NE(error.find("could not lock '" + fixture.LockFile.string() + "'"), std::string::npos)
            << "the refusal does not say which lock failed and why: " << error;
        EXPECT_EQ(CountRecordLines(fixture.DbFile), 1u) << "a refused save wrote anyway";
    }

    ASSERT_TRUE(store.SaveToFile(fixture.DbFile, nullptr));
    AssetStore_TextJsonl reread(nullptr);
    ASSERT_TRUE(reread.LoadFromFile(fixture.DbFile, nullptr));
    EXPECT_TRUE(reread.LookupGuidByPath("textures/b.png").has_value())
        << "the delta was drained by the refused save and never written";
}

// Two processes on one file: each holds its whole view from mount and nothing
// refreshes it (a package manifest has no watcher and never reconciles), so a
// compaction that published that view would delete every row the other process
// committed since. Any editor open past kCompactionMaxAge hits this.
TEST(AssetDbSharedStore, CompactionFromAStaleViewKeepsAnotherProcessesRow)
{
    const SharedStoreFixture fixture("ge_shared_store_merge");
    AssetStore_TextJsonl first(nullptr);
    fixture.Open(first);
    AssetStore_TextJsonl second(nullptr);
    fixture.Open(second);

    // The second process commits a row the first has never seen.
    ASSERT_TRUE(second.UpsertAsset(MakeRecord(kGuidB, "textures/b.png"), nullptr));
    ASSERT_TRUE(second.SaveToFile(fixture.DbFile, nullptr));
    ASSERT_EQ(CountRecordLines(fixture.DbFile), 2u);

    // The first process is now past the age trigger, so its next save compacts.
    first.SetCompactionMaxAgeForTesting(std::chrono::steady_clock::duration::zero());
    ASSERT_TRUE(first.UpsertAsset(MakeRecord(kGuidC, "textures/c.png"), nullptr));
    ASSERT_TRUE(first.SaveToFile(fixture.DbFile, nullptr));

    AssetStore_TextJsonl reread(nullptr);
    ASSERT_TRUE(reread.LoadFromFile(fixture.DbFile, nullptr));
    EXPECT_TRUE(reread.LookupGuidByPath("textures/c.png").has_value())
        << "the compacting process lost its own row";
    EXPECT_TRUE(reread.LookupGuidByPath("textures/b.png").has_value())
        << "the compaction rewrote away a row another process had committed";
    EXPECT_TRUE(reread.LookupGuidByPath("textures/a.png").has_value());
}

// The merge reads the file because the file is the other writer's evidence. A
// file that is GONE is not evidence of anything, so a compaction that finds
// none writes this process's whole view — merging against nothing would reduce
// the store to one flush's deltas and drop every row it had loaded.
TEST(AssetDbSharedStore, CompactsItsWholeViewWhenTheFileWentMissing)
{
    const SharedStoreFixture fixture("ge_shared_store_missing");
    AssetStore_TextJsonl store(nullptr);
    fixture.Open(store);

    std::error_code ec;
    ASSERT_TRUE(std::filesystem::remove(fixture.DbFile, ec)) << ec.message();

    store.SetCompactionMaxAgeForTesting(std::chrono::steady_clock::duration::zero());
    ASSERT_TRUE(store.UpsertAsset(MakeRecord(kGuidB, "textures/b.png"), nullptr));
    ASSERT_TRUE(store.SaveToFile(fixture.DbFile, nullptr));

    AssetStore_TextJsonl reread(nullptr);
    ASSERT_TRUE(reread.LoadFromFile(fixture.DbFile, nullptr));
    EXPECT_TRUE(reread.LookupGuidByPath("textures/b.png").has_value());
    EXPECT_TRUE(reread.LookupGuidByPath("textures/a.png").has_value())
        << "the rewritten file kept only this flush's deltas";
}

// The merge trusts the file because the file is the other writer's evidence.
// A file that exists but re-reads with no records is not evidence, it is
// damage: no store publishes an empty database, and merging against it would
// reduce this process's whole view to one flush's deltas. The save is refused
// so the deltas survive for a retry, and the damaged file is named.
TEST(AssetDbSharedStore, RefusesToMergeAgainstAFileAnExternalWriterEmptied)
{
    const SharedStoreFixture fixture("ge_shared_store_emptied");
    AssetStore_TextJsonl store(nullptr);
    fixture.Open(store);

    { std::ofstream(fixture.DbFile, std::ios::binary | std::ios::trunc); }
    ASSERT_EQ(std::filesystem::file_size(fixture.DbFile), 0u);

    store.SetCompactionMaxAgeForTesting(std::chrono::steady_clock::duration::zero());
    ASSERT_TRUE(store.UpsertAsset(MakeRecord(kGuidB, "textures/b.png"), nullptr));
    std::string error;
    EXPECT_FALSE(store.SaveToFile(fixture.DbFile, &error));
    EXPECT_NE(error.find("no records"), std::string::npos) << error;
    EXPECT_EQ(std::filesystem::file_size(fixture.DbFile), 0u) << "a refused save wrote anyway";

    // Whoever repairs the file gets the held delta on the next save, because
    // the refusal never drained it.
    {
        AssetStore_TextJsonl repaired(nullptr);
        repaired.UpsertAsset(MakeRecord(kGuidA, "textures/a.png"), nullptr);
        ASSERT_TRUE(repaired.SaveToFile(fixture.DbFile, nullptr));
    }
    ASSERT_TRUE(store.SaveToFile(fixture.DbFile, &error)) << error;

    AssetStore_TextJsonl reread(nullptr);
    ASSERT_TRUE(reread.LoadFromFile(fixture.DbFile, nullptr));
    EXPECT_TRUE(reread.LookupGuidByPath("textures/b.png").has_value())
        << "the refused save drained the delta and never wrote it";
    EXPECT_TRUE(reread.LookupGuidByPath("textures/a.png").has_value());
}

// A caller with no next tick — shutdown, unmount, rebind — cannot treat a
// refused save as "try later", so it asks to wait. Same contention, opposite
// answer from the worker flush above.
TEST(AssetDbSharedStore, AWaitingSaveOutlastsThePeerAndLandsItsDelta)
{
    const SharedStoreFixture fixture("ge_shared_store_wait");
    AssetStore_TextJsonl store(nullptr);
    fixture.Open(store);
    ASSERT_TRUE(store.UpsertAsset(MakeRecord(kGuidB, "textures/b.png"), nullptr));

    PeerHoldingTheWriteLock peer(fixture.LockFile);
    ASSERT_TRUE(peer.IsHoldingTheLock());

    // The worker's answer to the same contention.
    std::string error;
    EXPECT_FALSE(store.SaveToFile(fixture.DbFile, &error,
                                  GameEngine::AssetDatabase::StoreSaveWait::NonBlocking));
    EXPECT_NE(error.find("Another process"), std::string::npos) << error;

    peer.ReleaseAfter(kPeerHoldTime);
    EXPECT_TRUE(store.SaveToFile(fixture.DbFile, nullptr,
                                 GameEngine::AssetDatabase::StoreSaveWait::WaitForPeers));

    AssetStore_TextJsonl reread(nullptr);
    ASSERT_TRUE(reread.LoadFromFile(fixture.DbFile, nullptr));
    EXPECT_TRUE(reread.LookupGuidByPath("textures/b.png").has_value());
}

// The registry's teardown paths are where a refused save costs the edit: they
// clear the dirty state and never tick again. Unmount is the one reachable
// without shutting the registry down; Shutdown and RebindSource make the same
// call with the same argument.
TEST(AssetDbSharedStore, UnmountingWaitsOutAPeerInsteadOfDroppingTheEdit)
{
    namespace fs = std::filesystem;
    const fs::path root = GameEngine::TestUtils::MakeUniqueTempDirectory("ge_shared_store_unmount");
    fs::create_directories(root / "Assets");

    GameEngine::AssetManager manager;
    ASSERT_TRUE(manager.Initialize(root / "Assets", nullptr, root / "AssetDatabase.assetdb",
                                   root / ".Cache" / "AssetDatabase"));

    // An engine package in a developer build: its manifest is the store, and
    // the descriptor declares it shared, which is what gives it the lock.
    GameEngine::ResolvedPackage package;
    const fs::path packageRoot = root / "Packages" / "shared-pack";
    package.Alias = "shared-pack";
    package.Priority = 25;
    package.SourceKind = GameEngine::PackageSourceKind::Engine;
    package.RootDir = packageRoot;
    package.AssetsDir = packageRoot / "Assets";
    package.AuthoringRootDir = packageRoot;
    {
        std::error_code ec;
        fs::create_directories(package.AssetsDir / "textures", ec);
        std::ofstream(package.AssetsDir / "textures" / "bark.png", std::ios::binary) << "PNGDATA";
        AssetStore_TextJsonl manifest(nullptr);
        AssetRecord record = MakeRecord(kGuidA, "textures/bark.png");
        manifest.UpsertAsset(record, nullptr);
        ASSERT_TRUE(manifest.SaveToFile(package.AssetsDir / ".assetmanifest", nullptr,
                                        GameEngine::AssetDatabase::StoreSaveWait::NonBlocking));
    }

    const GameEngine::AssetSourceDesc desc = GameEngine::MakeEnginePackageMount(package, root / "host-cache");
    ASSERT_TRUE(desc.StoreIsSharedAcrossProcesses);
    ASSERT_TRUE(manager.RegisterSource(desc));
    manager.WaitForStartupScan("project");

    const fs::path texture = package.AssetsDir / "textures" / "bark.png";
    ASSERT_TRUE(manager.GetRegistry().SetMetaValue(texture, GameEngine::kTextureUsageMetaKey,
                                                   "color"));

    PeerHoldingTheWriteLock peer(fs::path(desc.AuthoritativeDbFile.string() + ".lock"));
    ASSERT_TRUE(peer.IsHoldingTheLock());
    peer.ReleaseAfter(kPeerHoldTime);
    ASSERT_TRUE(manager.GetRegistry().UnregisterSource("shared-pack"));

    AssetStore_TextJsonl persisted(nullptr);
    ASSERT_TRUE(persisted.LoadFromFile(package.AssetsDir / ".assetmanifest", nullptr));
    AssetRecord record{};
    ASSERT_TRUE(persisted.TryGetAsset(GameEngine::GUID(kGuidA), record));
    ASSERT_EQ(record.kv.count(GameEngine::kTextureUsageMetaKey), 1u)
        << "the unmount flush was refused and the edit dropped";
    EXPECT_EQ(record.kv.at(GameEngine::kTextureUsageMetaKey), "color");

    manager.Shutdown();
    std::error_code ec;
    fs::remove_all(root, ec);
}
