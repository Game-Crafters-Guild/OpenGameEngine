// Reconcile-resurrection performance arms at 50K files (and one real project).
//
// Arms (each test = one measured session; the driver runs each N times in a
// fresh process and aggregates medians):
//   Seed          — generate the 50K tree (kDirCount x kFilesPerDir), run the
//                   cold + cache-arming sessions untimed, snapshot the DB
//                   state dir as "golden".
//   ArmCold       — no journal, no cache, no snapshot: first-ever mount.
//   ArmFirstWarm  — journal only (the fresh-clone case): cache builds,
//                   existence loop runs without the dir-mtime gate.
//   ArmWarm       — journal + cache + snapshot, tree unchanged: THE
//                   every-session case. Asserts §9's steady-state criterion:
//                   zero existence checks and a byte- and mtime-identical
//                   .assetdb across the whole session.
//   ArmRename100  — 100 offline same-dir renames (d000..d099, f=0).
//   ArmRename1000 — 1000 offline renames over the SAME 100 dirs
//                   (d000..d099, f=0..9), so the existence-loop input is
//                   identical to ArmRename100 and the delta isolates the
//                   matcher's absentee scaling.
//   ArmMissing1000— 1000 files moved out of the mount (d100..d199, f=0..9):
//                   tombstone marking, cache-only, journal byte-identical.
//   Stored*       — the same warm/rename/missing arms over a stored-identity
//                   mount at equal N: §9's "costs more than the existing
//                   stored-identity pass at equal N is a regression" baseline.
//   Real*         — the project mount named by GE_BENCH_REAL_DB (read-only on
//                   the tree; the journal is a temp copy): fresh-clone and warm arms.
//
// Timing splits print as [reconcile-bench] lines; the isolated reconcile-pass
// cost prints from the engine's [recon-prof] lines (AssetRegistry.cpp) which
// carry the ReconcileStats counters as positive companions.
//
// DISABLED_ by default (creates 50K files; minutes of wall time). Run with:
//   AssetDbBenchmarks --gtest_also_run_disabled_tests \
//       --gtest_filter='ReconcilePerfBench.DISABLED_<Arm>'
// Environment:
//   GE_BENCH_ROOT      bench root (default C:/Dev/bench-50k)
//   GE_BENCH_REAL_ROOT real project asset root (default GE_BENCH_REAL_DB's
//                      parent)
//   GE_BENCH_REAL_DB   real project tracked journal (no default: the Real*
//                      arms skip when it is unset)
//
// The bench root is disposable state and is not in the repo. Its layout, the
// integrity check to run before reusing inherited state, the reseed/restore
// protocol, the goldens' schema trap and the measuring rules are in the
// reconcile-bench-harness record. A bench root you did not seed yourself is not
// trustworthy without that check: a perturbed tree measures a different shape
// and still exits 0.

#include <gtest/gtest.h>

#include "AssetDatabase/IAssetDbCache.h"
#include "AssetDatabase/IAssetStore.h"
#include "Assets/AssetRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::AssetDatabase;

namespace
{

namespace fs = std::filesystem;
using bclock = std::chrono::high_resolution_clock;
using msd = std::chrono::duration<double, std::milli>;

constexpr size_t kDirCount = 500;
constexpr size_t kFilesPerDir = 100;
constexpr size_t kTotalFiles = kDirCount * kFilesPerDir;
constexpr size_t kPoolThreads = 8; // matches the S9 scale-bench shape

// Rename/missing perturbation sets (disjoint by directory band):
//   R100:  d 0..99,   f 0        -> renamed_<f>.txt (same dir)
//   R1000: d 0..99,   f 0..9     -> renamed_<f>.txt (same dir, same 100 dirs)
//   M1000: d 100..199, f 0..9    -> <hidden>/<d>_<f>.txt (outside the mount)

std::string DirName(size_t d)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "d%03zu", d);
    return buf;
}

fs::path BenchRoot()
{
    if (const char* env = std::getenv("GE_BENCH_ROOT"))
        return fs::path(env);
    return fs::path("C:/Dev/bench-50k");
}

fs::path SrcRoot() { return BenchRoot() / "src"; }
fs::path HiddenDir() { return BenchRoot() / "hidden"; }
fs::path StateDir(const char* which) { return BenchRoot() / (std::string("state-") + which); }
fs::path GoldenDir(const char* which) { return BenchRoot() / (std::string("golden-") + which); }

constexpr const char* kRealDbUnsetSkip =
    "GE_BENCH_REAL_DB is unset: set it to a project's tracked AssetDatabase.assetdb to run the "
    "real-project arms";

// Empty when GE_BENCH_REAL_DB is unset; every Real* arm skips on empty.
fs::path RealDbFile()
{
    if (const char* env = std::getenv("GE_BENCH_REAL_DB"))
        return fs::path(env);
    return {};
}

fs::path RealRoot()
{
    if (const char* env = std::getenv("GE_BENCH_REAL_ROOT"))
        return fs::path(env);
    return RealDbFile().parent_path();
}

std::string ReadFileBytes(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open())
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

AssetSourceDesc MakeMountDesc(const fs::path& root, const fs::path& stateDir, bool derived)
{
    // Mirrors the editor/dev project mount (see AssetDbScaleBenchmark):
    // JSONL journal + SQLite fingerprint cache in a state dir outside the
    // scanned root.
    AssetSourceDesc desc{};
    desc.Alias = "project";
    desc.Root = root;
    desc.DerivedIdentity = derived;
    desc.AuthoritativeDbFile = stateDir / "AssetDatabase.assetdb";
    desc.CacheRoot = stateDir / "Cache";
    desc.Priority = 100;
    return desc;
}

struct SessionTimings
{
    double mountMs = 0.0;
    double scanMs = 0.0;
    double shutdownMs = 0.0;
    size_t registered = 0;
    size_t existenceChecks = 0;
    bool snapshotArmed = false;
};

// One full editor-session lifecycle over a mount: Initialize, RegisterSource
// (mount), WaitForStartupScan (scan + scan-tail reconcile for derived
// mounts; mount-time reconcile for stored mounts), body, Shutdown (store
// flush + warm-start snapshot write when a cache is open).
template <typename Body>
void RunTimedSession(const fs::path& root, const fs::path& stateDir, bool derived,
                     SessionTimings& out, Body&& body)
{
    JobSystem::WorkStealingThreadPool pool(kPoolThreads);
    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(&pool));

    const auto t0 = bclock::now();
    ASSERT_TRUE(reg.RegisterSource(MakeMountDesc(root, stateDir, derived)));
    const auto t1 = bclock::now();
    reg.WaitForStartupScan();
    const auto t2 = bclock::now();

    out.mountMs = msd(t1 - t0).count();
    out.scanMs = msd(t2 - t1).count();
    out.registered = reg.GetAssetCount();

    {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        out.existenceChecks = pinned->LastReconcileExistenceChecks;
        out.snapshotArmed = !pinned->SnapshotDirMtimeByPath.empty();
    }

    body(reg);

    const auto t3 = bclock::now();
    reg.Shutdown();
    out.shutdownMs = msd(bclock::now() - t3).count();
}

void RunTimedSession(const fs::path& root, const fs::path& stateDir, bool derived,
                     SessionTimings& out)
{
    RunTimedSession(root, stateDir, derived, out, [](AssetRegistry&) {});
}

void PrintArm(const char* arm, const SessionTimings& t, const char* extra = "")
{
    std::fprintf(stderr,
        "[reconcile-bench] arm=%s mount=%.1fms scan=%.1fms shutdown=%.1fms total=%.1fms "
        "registered=%zu exist-checks=%zu snapshot-armed=%d%s%s\n",
        arm, t.mountMs, t.scanMs, t.shutdownMs, t.mountMs + t.scanMs + t.shutdownMs,
        t.registered, t.existenceChecks, t.snapshotArmed ? 1 : 0,
        extra[0] ? " " : "", extra);
}

void GenerateTree()
{
    std::error_code ec;
    fs::create_directories(SrcRoot(), ec);
    fs::create_directories(HiddenDir(), ec);
    for (size_t d = 0; d < kDirCount; ++d)
    {
        const fs::path dir = SrcRoot() / DirName(d);
        fs::create_directories(dir, ec);
        for (size_t f = 0; f < kFilesPerDir; ++f)
        {
            const size_t i = d * kFilesPerDir + f;
            std::ofstream out(dir / ("asset_" + std::to_string(f) + ".txt"),
                              std::ios::binary | std::ios::trunc);
            // Content unique per file, length varied so content hashes and
            // sizes both differentiate fingerprints.
            out << "synthetic reconcile asset " << d << "/" << f << " "
                << std::string(1 + (i % 37), 'x') << "\n";
        }
    }
}

// Undo every perturbation an arm may have left behind. Arms restore at START
// so a crashed run cannot poison the next one.
void RestorePristineTree()
{
    std::error_code ec;
    for (size_t d = 0; d < 100; ++d)
    {
        const fs::path dir = SrcRoot() / DirName(d);
        for (size_t f = 0; f < 10; ++f)
        {
            const fs::path renamed = dir / ("renamed_" + std::to_string(f) + ".txt");
            if (fs::exists(renamed, ec))
                fs::rename(renamed, dir / ("asset_" + std::to_string(f) + ".txt"), ec);
        }
    }
    if (fs::exists(HiddenDir(), ec))
    {
        for (const auto& entry : fs::directory_iterator(HiddenDir(), ec))
        {
            size_t d = 0, f = 0;
            if (std::sscanf(entry.path().filename().string().c_str(), "%zu_%zu.txt", &d, &f) == 2)
                fs::rename(entry.path(), SrcRoot() / DirName(d) / ("asset_" + std::to_string(f) + ".txt"), ec);
        }
    }
    // Pristine spot checks.
    ASSERT_TRUE(fs::exists(SrcRoot() / "d000" / "asset_0.txt", ec));
    ASSERT_TRUE(fs::exists(SrcRoot() / "d199" / "asset_9.txt", ec));
    ASSERT_TRUE(fs::is_empty(HiddenDir(), ec));
}

void RestoreStateFromGolden(const char* which)
{
    std::error_code ec;
    fs::remove_all(StateDir(which), ec);
    fs::copy(GoldenDir(which), StateDir(which),
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << "golden restore failed: " << ec.message();
    ASSERT_TRUE(fs::exists(StateDir(which) / "AssetDatabase.assetdb", ec));
}

void SnapshotStateToGolden(const char* which)
{
    std::error_code ec;
    fs::remove_all(GoldenDir(which), ec);
    fs::copy(StateDir(which), GoldenDir(which),
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << "golden snapshot failed: " << ec.message();
}

void ApplyRenames(size_t dirCount, size_t filesPerDir)
{
    std::error_code ec;
    for (size_t d = 0; d < dirCount; ++d)
    {
        const fs::path dir = SrcRoot() / DirName(d);
        for (size_t f = 0; f < filesPerDir; ++f)
        {
            fs::rename(dir / ("asset_" + std::to_string(f) + ".txt"),
                       dir / ("renamed_" + std::to_string(f) + ".txt"), ec);
            ASSERT_FALSE(ec) << DirName(d) << "/asset_" << f << ": " << ec.message();
        }
    }
}

void ApplyMissing()
{
    std::error_code ec;
    for (size_t d = 100; d < 200; ++d)
    {
        for (size_t f = 0; f < 10; ++f)
        {
            fs::rename(SrcRoot() / DirName(d) / ("asset_" + std::to_string(f) + ".txt"),
                       HiddenDir() / (std::to_string(d) + "_" + std::to_string(f) + ".txt"), ec);
            ASSERT_FALSE(ec) << DirName(d) << "/asset_" << f << ": " << ec.message();
        }
    }
}

// Path -> GUID over the whole store (one enumeration, then O(1) lookups).
std::unordered_map<std::string, GUID> StorePathMap(const AssetRegistry::SourceEntry& entry)
{
    std::unordered_map<std::string, GUID> map;
    for (const auto& rec : entry.Store->EnumerateAssets())
        map.emplace(rec.path, rec.guid);
    return map;
}

// Extract the guid field from the golden journal's record line for a path —
// the GUID-preservation companion for the stored-identity heal.
std::string GoldenJournalGuidForPath(const char* which, const std::string& canonicalRel)
{
    std::ifstream in(GoldenDir(which) / "AssetDatabase.assetdb");
    std::string line;
    const std::string needle = "\"path\":\"" + canonicalRel + "\"";
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#' || line.find(needle) == std::string::npos)
            continue;
        const size_t g = line.find("\"guid\":\"");
        if (g == std::string::npos)
            continue;
        const size_t start = g + 8;
        const size_t end = line.find('"', start);
        if (end != std::string::npos)
            return line.substr(start, end - start);
    }
    return {};
}

void SeedMount(const char* which, bool derived)
{
    std::error_code ec;
    fs::remove_all(StateDir(which), ec);
    fs::create_directories(StateDir(which), ec);

    // Session 1 (cold): journal written at Shutdown. No cache — the SQLite
    // cache opens only when the .assetdb already exists.
    {
        SessionTimings t{};
        RunTimedSession(SrcRoot(), StateDir(which), derived, t);
        if (::testing::Test::HasFatalFailure())
            return;
        ASSERT_EQ(t.registered, kTotalFiles) << "seed session 1 (" << which << ")";
        PrintArm((std::string("seed1-") + which).c_str(), t);
    }
    // Session 2: cache opens, fingerprints populate, Shutdown writes the
    // warm-start snapshot.
    {
        SessionTimings t{};
        RunTimedSession(SrcRoot(), StateDir(which), derived, t, [&](AssetRegistry& reg) {
            auto pinned = reg.ProjectSourcePinned();
            ASSERT_TRUE(pinned && pinned->Cache)
                << "seed session 2 (" << which << ") did not open the SQLite cache";
        });
        if (::testing::Test::HasFatalFailure())
            return;
        ASSERT_EQ(t.registered, kTotalFiles) << "seed session 2 (" << which << ")";
        PrintArm((std::string("seed2-") + which).c_str(), t);
    }
    SnapshotStateToGolden(which);
}

} // namespace

// ---------------------------------------------------------------------------
// Synthetic 50K, derived identity (the shipped editor mount shape).
// ---------------------------------------------------------------------------

TEST(ReconcilePerfBench, DISABLED_Seed)
{
    std::error_code ec;
    if (!fs::exists(SrcRoot() / "d499" / ("asset_" + std::to_string(kFilesPerDir - 1) + ".txt"), ec))
    {
        const auto g0 = bclock::now();
        GenerateTree();
        std::fprintf(stderr, "[reconcile-bench] generated dirs=%zu files-per-dir=%zu total=%zu in %.1fms\n",
                     kDirCount, kFilesPerDir, kTotalFiles, msd(bclock::now() - g0).count());
    }
    RestorePristineTree();

    // Full-tree positive companion, once: exactly kTotalFiles files on disk.
    size_t onDisk = 0;
    for (const auto& entry : fs::recursive_directory_iterator(SrcRoot(), ec))
    {
        if (entry.is_regular_file(ec))
            ++onDisk;
    }
    ASSERT_EQ(onDisk, kTotalFiles) << "generated tree is not pristine";

    SeedMount("derived", /*derived=*/true);
}

TEST(ReconcilePerfBench, DISABLED_ArmCold)
{
    RestorePristineTree();
    std::error_code ec;
    fs::remove_all(StateDir("derived"), ec);
    ASSERT_FALSE(fs::exists(StateDir("derived"), ec)) << "state dir survived the wipe";

    SessionTimings t{};
    RunTimedSession(SrcRoot(), StateDir("derived"), true, t);
    if (::testing::Test::HasFatalFailure())
        return;

    EXPECT_EQ(t.registered, kTotalFiles);
    PrintArm("cold", t);
}

TEST(ReconcilePerfBench, DISABLED_ArmFirstWarm)
{
    RestorePristineTree();
    std::error_code ec;
    fs::remove_all(StateDir("derived"), ec);
    fs::create_directories(StateDir("derived"), ec);
    // Journal only — the fresh-clone shape: no Cache dir, no snapshot.
    fs::copy_file(GoldenDir("derived") / "AssetDatabase.assetdb",
                  StateDir("derived") / "AssetDatabase.assetdb",
                  fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    SessionTimings t{};
    RunTimedSession(SrcRoot(), StateDir("derived"), true, t);
    if (::testing::Test::HasFatalFailure())
        return;

    EXPECT_EQ(t.registered, kTotalFiles);
    EXPECT_FALSE(t.snapshotArmed) << "fresh-clone arm unexpectedly loaded a snapshot";
    PrintArm("first-warm", t);
}

TEST(ReconcilePerfBench, DISABLED_ArmWarm)
{
    RestorePristineTree();
    RestoreStateFromGolden("derived");

    const fs::path dbFile = StateDir("derived") / "AssetDatabase.assetdb";
    const std::string dbBefore = ReadFileBytes(dbFile);
    ASSERT_FALSE(dbBefore.empty());
    std::error_code ec;
    const auto mtimeBefore = fs::last_write_time(dbFile, ec);

    SessionTimings t{};
    RunTimedSession(SrcRoot(), StateDir("derived"), true, t, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Cache);
        // §5-A/§5-B observable, in-session half: the store never dirtied.
        EXPECT_FALSE(pinned->StoreDirty.load(std::memory_order_relaxed))
            << "an unchanged warm session dirtied the project store";
    });
    if (::testing::Test::HasFatalFailure())
        return;

    EXPECT_EQ(t.registered, kTotalFiles);
    ASSERT_TRUE(t.snapshotArmed)
        << "no dir-mtime snapshot was loaded; the warm arm would measure the wrong population";
    // §9 cost-shape criterion: clean dirs prove every record with zero
    // per-record syscalls.
    EXPECT_EQ(t.existenceChecks, 0u) << "warm session performed existence checks on a clean tree";

    // §9 steady-state criterion: no .assetdb mtime change at all — checked
    // across the WHOLE session including Shutdown's flush path.
    const auto mtimeAfter = fs::last_write_time(dbFile, ec);
    const bool bytesSame = (ReadFileBytes(dbFile) == dbBefore);
    const bool mtimeSame = (mtimeAfter == mtimeBefore);
    EXPECT_TRUE(bytesSame) << "warm session changed the .assetdb bytes";
    EXPECT_TRUE(mtimeSame) << "warm session changed the .assetdb mtime";

    char extra[64];
    std::snprintf(extra, sizeof(extra), "db-bytes-same=%d db-mtime-same=%d", bytesSame ? 1 : 0,
                  mtimeSame ? 1 : 0);
    PrintArm("warm", t, extra);
}

namespace
{

void RunDerivedRenameArm(const char* armName, size_t filesPerDir)
{
    RestorePristineTree();
    RestoreStateFromGolden("derived");
    ApplyRenames(100, filesPerDir);
    if (::testing::Test::HasFatalFailure())
        return;
    const size_t expectedHeals = 100 * filesPerDir;

    SessionTimings t{};
    std::vector<RedirectRecord> redirects;
    std::unordered_map<std::string, GUID> pathMap;
    RunTimedSession(SrcRoot(), StateDir("derived"), true, t, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        redirects = pinned->Store->EnumerateRedirects();
        pathMap = StorePathMap(*pinned);
    });
    if (::testing::Test::HasFatalFailure())
        return;

    // Positive companions: every rename healed by exactly one redirect, ghost
    // records removed (store back at N), old paths gone, new paths present.
    EXPECT_EQ(redirects.size(), expectedHeals);
    EXPECT_EQ(pathMap.size(), kTotalFiles);
    EXPECT_EQ(pathMap.count("d000/asset_0.txt"), 0u) << "ghost record survived the heal";
    ASSERT_EQ(pathMap.count("d000/renamed_0.txt"), 1u) << "renamed file has no record";
    const GUID newGuid = pathMap["d000/renamed_0.txt"];
    bool redirectTargetsNewGuid = false;
    for (const auto& rr : redirects)
    {
        if (rr.to == newGuid)
        {
            redirectTargetsNewGuid = true;
            break;
        }
    }
    EXPECT_TRUE(redirectTargetsNewGuid) << "no redirect targets the renamed file's derived GUID";
    // Existence-loop shape: 100 dirty dirs x kFilesPerDir records (+ the
    // just-registered new files also enumerated by the loop).
    EXPECT_GE(t.existenceChecks, 100 * kFilesPerDir);
    EXPECT_LE(t.existenceChecks, 100 * kFilesPerDir + expectedHeals + 200);

    char extra[64];
    std::snprintf(extra, sizeof(extra), "redirects=%zu", redirects.size());
    PrintArm(armName, t, extra);
}

} // namespace

TEST(ReconcilePerfBench, DISABLED_ArmRename100) { RunDerivedRenameArm("rename-100", 1); }
TEST(ReconcilePerfBench, DISABLED_ArmRename1000) { RunDerivedRenameArm("rename-1000", 10); }

TEST(ReconcilePerfBench, DISABLED_ArmMissing1000)
{
    RestorePristineTree();
    RestoreStateFromGolden("derived");
    ApplyMissing();
    if (::testing::Test::HasFatalFailure())
        return;

    const fs::path dbFile = StateDir("derived") / "AssetDatabase.assetdb";
    const std::string dbBefore = ReadFileBytes(dbFile);

    SessionTimings t{};
    size_t missingMarked = 0;
    size_t keptMarked = 0;
    RunTimedSession(SrcRoot(), StateDir("derived"), true, t, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store && pinned->Cache);
        const auto pathMap = StorePathMap(*pinned);
        for (size_t d = 100; d < 200; ++d)
        {
            for (size_t f = 0; f < 10; ++f)
            {
                const auto it = pathMap.find(DirName(d) + "/asset_" + std::to_string(f) + ".txt");
                ASSERT_NE(it, pathMap.end()) << "ghost record vanished from the store";
                AssetRecord rec{};
                if (pinned->Cache->TryGetAsset(it->second, rec) && rec.missing)
                    ++missingMarked;
            }
            // Positive control: a surviving neighbour in the same dirty dir
            // must NOT be marked — the count above cannot pass by marking
            // everything.
            const auto kept = pathMap.find(DirName(d) + "/asset_10.txt");
            ASSERT_NE(kept, pathMap.end());
            AssetRecord keptRec{};
            if (pinned->Cache->TryGetAsset(kept->second, keptRec) && keptRec.missing)
                ++keptMarked;
        }
    });
    if (::testing::Test::HasFatalFailure())
        return;

    EXPECT_EQ(missingMarked, 1000u) << "not every offline-deleted file was tombstoned in the cache";
    EXPECT_EQ(keptMarked, 0u) << "present files were marked missing";
    EXPECT_EQ(t.registered, kTotalFiles) << "tombstone marking removed records";
    EXPECT_GE(t.existenceChecks, 100 * kFilesPerDir);
    // Cache-only observation: the journal is byte-identical.
    const bool bytesSame = (ReadFileBytes(dbFile) == dbBefore);
    EXPECT_TRUE(bytesSame) << "missing-marking wrote the journal";

    char extra[80];
    std::snprintf(extra, sizeof(extra), "missing-marked=%zu db-bytes-same=%d", missingMarked,
                  bytesSame ? 1 : 0);
    PrintArm("missing-1000", t, extra);
}

// ---------------------------------------------------------------------------
// Stored-identity mirror at equal N — §9's regression baseline.
// ---------------------------------------------------------------------------

TEST(ReconcilePerfBench, DISABLED_StoredSeed)
{
    std::error_code ec;
    ASSERT_TRUE(fs::exists(SrcRoot() / "d499" / "asset_99.txt", ec))
        << "run DISABLED_Seed first — the stored arms share the synthetic tree";
    RestorePristineTree();
    SeedMount("stored", /*derived=*/false);
}

TEST(ReconcilePerfBench, DISABLED_StoredArmWarm)
{
    RestorePristineTree();
    RestoreStateFromGolden("stored");

    const fs::path dbFile = StateDir("stored") / "AssetDatabase.assetdb";
    const std::string dbBefore = ReadFileBytes(dbFile);

    SessionTimings t{};
    RunTimedSession(SrcRoot(), StateDir("stored"), false, t);
    if (::testing::Test::HasFatalFailure())
        return;

    EXPECT_EQ(t.registered, kTotalFiles);
    const bool bytesSame = (ReadFileBytes(dbFile) == dbBefore);

    char extra[48];
    std::snprintf(extra, sizeof(extra), "db-bytes-same=%d", bytesSame ? 1 : 0);
    PrintArm("stored-warm", t, extra);
}

TEST(ReconcilePerfBench, DISABLED_StoredArmRename100)
{
    RestorePristineTree();
    RestoreStateFromGolden("stored");

    const std::string goldenGuid = GoldenJournalGuidForPath("stored", "d000/asset_0.txt");
    ASSERT_FALSE(goldenGuid.empty()) << "could not read the golden journal's guid for d000/asset_0.txt";

    ApplyRenames(100, 1);
    if (::testing::Test::HasFatalFailure())
        return;

    SessionTimings t{};
    size_t redirectCount = 0;
    std::unordered_map<std::string, GUID> pathMap;
    RunTimedSession(SrcRoot(), StateDir("stored"), false, t, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        redirectCount = pinned->Store->EnumerateRedirects().size();
        pathMap = StorePathMap(*pinned);
    });
    if (::testing::Test::HasFatalFailure())
        return;

    // Stored heal = rebind in place: GUID preserved, no redirects, no ghosts.
    EXPECT_EQ(redirectCount, 0u) << "stored-identity heal emitted redirects";
    EXPECT_EQ(pathMap.size(), kTotalFiles);
    EXPECT_EQ(pathMap.count("d000/asset_0.txt"), 0u);
    ASSERT_EQ(pathMap.count("d000/renamed_0.txt"), 1u) << "stored heal did not rebind the record";
    EXPECT_EQ(pathMap["d000/renamed_0.txt"].ToString(), goldenGuid)
        << "stored-identity heal did not preserve the GUID";

    char extra[48];
    std::snprintf(extra, sizeof(extra), "guid-preserved=%d",
                  pathMap["d000/renamed_0.txt"].ToString() == goldenGuid ? 1 : 0);
    PrintArm("stored-rename-100", t, extra);
}

TEST(ReconcilePerfBench, DISABLED_StoredArmMissing1000)
{
    RestorePristineTree();
    RestoreStateFromGolden("stored");
    ApplyMissing();
    if (::testing::Test::HasFatalFailure())
        return;

    SessionTimings t{};
    size_t missingMarked = 0;
    RunTimedSession(SrcRoot(), StateDir("stored"), false, t, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        for (const auto& rec : pinned->Store->EnumerateAssets())
        {
            if (rec.missing)
                ++missingMarked;
        }
    });
    if (::testing::Test::HasFatalFailure())
        return;

    // Stored scheme journals the flag — the tombstones live in the store.
    EXPECT_EQ(missingMarked, 1000u);

    char extra[48];
    std::snprintf(extra, sizeof(extra), "missing-marked=%zu", missingMarked);
    PrintArm("stored-missing-1000", t, extra);
}

// ---------------------------------------------------------------------------
// Real project (read-only on the tree; journal is a temp copy).
// ---------------------------------------------------------------------------

TEST(ReconcilePerfBench, DISABLED_RealSeed)
{
    if (RealDbFile().empty())
        GTEST_SKIP() << kRealDbUnsetSkip;
    std::error_code ec;
    ASSERT_TRUE(fs::exists(RealDbFile(), ec)) << RealDbFile().string();
    fs::remove_all(StateDir("real"), ec);
    fs::create_directories(StateDir("real"), ec);
    fs::copy_file(RealDbFile(), StateDir("real") / "AssetDatabase.assetdb",
                  fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    // One session: cache opens (journal exists), fingerprints populate,
    // Shutdown writes the snapshot. Any db-vs-disk drift in the working tree
    // heals into the TEMP journal copy here, so the warm arms measure a
    // clean steady state.
    SessionTimings t{};
    RunTimedSession(RealRoot(), StateDir("real"), true, t, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Cache) << "real seed did not open the SQLite cache";
    });
    if (::testing::Test::HasFatalFailure())
        return;
    ASSERT_GT(t.registered, 0u);
    PrintArm("real-seed", t);
    SnapshotStateToGolden("real");
}

TEST(ReconcilePerfBench, DISABLED_RealArmFreshClone)
{
    if (RealDbFile().empty())
        GTEST_SKIP() << kRealDbUnsetSkip;
    std::error_code ec;
    fs::remove_all(StateDir("real"), ec);
    fs::create_directories(StateDir("real"), ec);
    // The tracked journal, no cache, no snapshot: what a fresh clone pays.
    fs::copy_file(RealDbFile(), StateDir("real") / "AssetDatabase.assetdb",
                  fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    SessionTimings t{};
    RunTimedSession(RealRoot(), StateDir("real"), true, t);
    if (::testing::Test::HasFatalFailure())
        return;

    EXPECT_GT(t.registered, 0u);
    EXPECT_FALSE(t.snapshotArmed);
    PrintArm("real-fresh-clone", t);
}

TEST(ReconcilePerfBench, DISABLED_RealArmWarm)
{
    if (RealDbFile().empty())
        GTEST_SKIP() << kRealDbUnsetSkip;
    RestoreStateFromGolden("real");

    const fs::path dbFile = StateDir("real") / "AssetDatabase.assetdb";
    const std::string dbBefore = ReadFileBytes(dbFile);

    SessionTimings t{};
    RunTimedSession(RealRoot(), StateDir("real"), true, t);
    if (::testing::Test::HasFatalFailure())
        return;

    EXPECT_GT(t.registered, 0u);
    ASSERT_TRUE(t.snapshotArmed);
    EXPECT_EQ(t.existenceChecks, 0u)
        << "real-project warm session performed existence checks — tree changed under the bench?";
    const bool bytesSame = (ReadFileBytes(dbFile) == dbBefore);
    EXPECT_TRUE(bytesSame) << "real-project warm session changed the journal copy";

    char extra[48];
    std::snprintf(extra, sizeof(extra), "db-bytes-same=%d", bytesSame ? 1 : 0);
    PrintArm("real-warm", t, extra);
}
