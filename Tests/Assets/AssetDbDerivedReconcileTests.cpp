// Derived-identity mounts keep db-vs-disk missing observations in the
// per-machine SQLite cache, never in the git-shared .assetdb journal. These
// tests pin the missing-detection half of the derived reconcile: the startup
// scan's tail marks ghost records (record exists, file doesn't) missing in
// the cache, the dir-mtime gate proves clean directories without a single
// exists() syscall, and a file that returns re-clears its flag — all while
// the journal stays byte-identical. The rename-healing half (redirect
// emission) is pinned by AssetDbDerivedRedirectTests.cpp.

#include <gtest/gtest.h>

#include "AssetDatabase/AssetDatabasePaths.h"
#include "AssetDatabase/IAssetDbCache.h"
#include "AssetDatabase/IAssetStore.h"
#include "Assets/AssetRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace GameEngine;
using namespace GameEngine::AssetDatabase;

namespace
{

void WriteTextFile(const std::filesystem::path& p, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << p.string();
    out << text;
}

std::string ReadTextFile(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open())
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

AssetSourceDesc MakeDerivedProjectDesc(const std::filesystem::path& root)
{
    const AssetDatabasePaths dbPaths = GetDefaultPathsForAssetRoot(root, {}, {});
    AssetSourceDesc desc{};
    desc.Alias = "project";
    desc.Root = root;
    desc.DerivedIdentity = true;
    desc.AuthoritativeDbFile = dbPaths.authoritativeFile;
    desc.CacheRoot = dbPaths.cacheRoot;
    desc.Priority = 100;
    return desc;
}

// One editor session over a derived mount: mount, drain the startup scan
// (whose tail runs the derived reconcile), then hand control to `body`.
// Shutdown flushes the store and writes the warm-start snapshot when a
// cache is open.
template <typename Body>
void RunDerivedSession(const std::filesystem::path& root, Body&& body)
{
    JobSystem::WorkStealingThreadPool pool(2);
    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(&pool));
    ASSERT_TRUE(reg.RegisterSource(MakeDerivedProjectDesc(root)));
    reg.WaitForStartupScan();
    body(reg);
    reg.Shutdown();
}

GUID FindGuidByStorePath(const AssetRegistry::SourceEntry& entry, const std::string& canonicalRel)
{
    for (const auto& rec : entry.Store->EnumerateAssets())
    {
        if (rec.path == canonicalRel)
            return rec.guid;
    }
    return GUID::Null();
}

} // namespace

// An offline delete leaves a ghost record. The next session's startup scan
// must mark it missing in the per-machine cache and leave the journaled
// record — and the .assetdb bytes — untouched.
TEST(AssetDbDerivedReconcile, GhostRecordIsMarkedMissingInCacheNotTheJournal)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_reconcile_ghost");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "a.txt", "alpha content");
    WriteTextFile(root / "sub" / "b.txt", "bravo content");

    // Session 1: mount and persist the journal. A fresh mount has no cache
    // (the SQLite cache opens only when the .assetdb already exists).
    RunDerivedSession(root, [&](AssetRegistry& reg) { ASSERT_TRUE(reg.SaveToFile({})); });

    const fs::path dbFile = root / "AssetDatabase.assetdb";
    ASSERT_TRUE(fs::exists(dbFile, ec));
    const std::string journalBefore = ReadTextFile(dbFile);
    ASSERT_FALSE(journalBefore.empty());

    // Offline delete: sub/a.txt's record becomes a ghost.
    fs::remove(root / "sub" / "a.txt", ec);
    ASSERT_FALSE(fs::exists(root / "sub" / "a.txt", ec));

    // Session 2: the scan-tail reconcile runs before WaitForStartupScan returns.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        ASSERT_TRUE(pinned->Cache) << "with an existing .assetdb the SQLite cache must open";

        const GUID ghostGuid = FindGuidByStorePath(*pinned, "sub/a.txt");
        const GUID keptGuid = FindGuidByStorePath(*pinned, "sub/b.txt");
        ASSERT_FALSE(ghostGuid.IsNull()) << "ghost record vanished from the store";
        ASSERT_FALSE(keptGuid.IsNull());

        AssetRecord cacheRec{};
        ASSERT_TRUE(pinned->Cache->TryGetAsset(ghostGuid, cacheRec))
            << "reconcile did not write a cache row for the ghost";
        EXPECT_TRUE(cacheRec.missing) << "ghost record was not marked missing in the derived cache";

        // Positive control: the surviving file's row must not be missing —
        // the assertion above cannot pass by marking everything.
        AssetRecord keptRec{};
        ASSERT_TRUE(pinned->Cache->TryGetAsset(keptGuid, keptRec));
        EXPECT_FALSE(keptRec.missing) << "a present file was marked missing";

        // The journal is not the flag's resting place: the store record keeps
        // missing=false, the store stays clean, and the file bytes are
        // untouched even through a persistence tick.
        AssetRecord storeRec{};
        ASSERT_TRUE(pinned->Store->TryGetAsset(ghostGuid, storeRec));
        EXPECT_FALSE(storeRec.missing) << "derived reconcile wrote the missing flag into the journaled record";
        EXPECT_FALSE(pinned->StoreDirty.load(std::memory_order_relaxed))
            << "derived reconcile dirtied the project store";
        reg.TickPersistence();
        EXPECT_EQ(ReadTextFile(dbFile), journalBefore) << "derived reconcile changed the .assetdb";
    });

    fs::remove_all(root, ec);
}

// A warm start over an unchanged tree must prove every record via the
// dir-mtime gate: zero per-record exists() checks. The in-test positive
// control dirties a directory and expects the counter to move.
TEST(AssetDbDerivedReconcile, CleanDirsReconcileWithZeroExistenceChecks)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_reconcile_cleandirs");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "a.txt", "alpha content");
    WriteTextFile(root / "sub" / "b.txt", "bravo content");

    // Session 1 persists the journal; session 2 opens the cache, populates
    // fingerprints, and its Shutdown writes the warm-start snapshot with
    // per-directory mtimes.
    RunDerivedSession(root, [&](AssetRegistry& reg) { ASSERT_TRUE(reg.SaveToFile({})); });
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        ASSERT_TRUE(pinned->Cache) << "session 2 must open the cache or no snapshot gets written";
    });

    // Session 3: unchanged tree.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store && pinned->Cache);
        ASSERT_FALSE(pinned->SnapshotDirMtimeByPath.empty())
            << "no dir-mtime snapshot was loaded; the gate cannot arm and the test would prove nothing";

        EXPECT_EQ(pinned->LastReconcileExistenceChecks, 0u)
            << "clean-dir records were stat'd despite the dir-mtime gate";

        // Positive control: dirty sub/ (delete b) and rescan — the gate must
        // open and the counter must move, so a dead counter cannot satisfy
        // the zero assertion above.
        fs::remove(root / "sub" / "b.txt", ec);
        auto rescan = reg.ScanDirectoryAsync(root, true);
        (void)rescan.get();
        EXPECT_GE(pinned->LastReconcileExistenceChecks, 1u)
            << "a dirty dir produced no existence checks — counter or gate is dead";

        // The deletion is observed where it should be: in the cache.
        const GUID bGuid = FindGuidByStorePath(*pinned, "sub/b.txt");
        ASSERT_FALSE(bGuid.IsNull());
        AssetRecord bRec{};
        ASSERT_TRUE(pinned->Cache->TryGetAsset(bGuid, bRec));
        EXPECT_TRUE(bRec.missing) << "the dirty-dir pass did not mark the deleted file missing";
    });

    fs::remove_all(root, ec);
}

// A ghost whose file returns must have its cache flag re-cleared by the
// reconcile itself. The file is moved out and back with fs::rename, which
// preserves mtime+size — the returning file is snapshot-current, so the scan
// skips re-registration and only the reconcile can clear the flag.
TEST(AssetDbDerivedReconcile, ReturningFileReclearsTheCacheMissingFlag)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_reconcile_return");
    const fs::path hidden = TestUtils::MakeUniqueTempDirectory("ge_derived_reconcile_return_hidden");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove_all(hidden, ec);
    fs::create_directories(root, ec);
    fs::create_directories(hidden, ec);
    WriteTextFile(root / "sub" / "a.txt", "alpha content");

    // Session 1: journal. Session 2: cache + fingerprints + snapshot.
    RunDerivedSession(root, [&](AssetRegistry& reg) { ASSERT_TRUE(reg.SaveToFile({})); });
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        ASSERT_TRUE(pinned->Cache);
    });

    // Move the file OUT of the mount between sessions.
    fs::rename(root / "sub" / "a.txt", hidden / "a.txt", ec);
    ASSERT_FALSE(ec) << ec.message();

    const fs::path dbFile = root / "AssetDatabase.assetdb";

    // Session 3: startup marks the ghost; the file then returns and a full
    // rescan re-clears it.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store && pinned->Cache);
        ASSERT_FALSE(pinned->SnapshotByPath.empty())
            << "no snapshot was loaded; the returning file would be re-registered and the "
               "test could pass without the reconcile's re-clear path";

        const GUID guid = FindGuidByStorePath(*pinned, "sub/a.txt");
        ASSERT_FALSE(guid.IsNull());

        // Control: the startup pass marked the ghost.
        AssetRecord rec{};
        ASSERT_TRUE(pinned->Cache->TryGetAsset(guid, rec));
        ASSERT_TRUE(rec.missing) << "startup pass did not mark the moved-out file missing";

        const std::string journalBefore = ReadTextFile(dbFile);
        ASSERT_FALSE(journalBefore.empty());

        // The file returns with its original mtime+size.
        std::error_code mv;
        fs::rename(hidden / "a.txt", root / "sub" / "a.txt", mv);
        ASSERT_FALSE(mv) << mv.message();

        auto rescan = reg.ScanDirectoryAsync(root, true);
        (void)rescan.get();

        AssetRecord after{};
        ASSERT_TRUE(pinned->Cache->TryGetAsset(guid, after));
        EXPECT_FALSE(after.missing) << "a returning file did not re-clear the cache missing flag";

        // The whole mark/clear round-trip never touches the journal.
        EXPECT_FALSE(pinned->StoreDirty.load(std::memory_order_relaxed));
        reg.TickPersistence();
        EXPECT_EQ(ReadTextFile(dbFile), journalBefore) << "the re-clear pass changed the .assetdb";
    });

    fs::remove_all(root, ec);
    fs::remove_all(hidden, ec);
}
