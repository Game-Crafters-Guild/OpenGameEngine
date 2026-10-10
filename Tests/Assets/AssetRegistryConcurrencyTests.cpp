#include <gtest/gtest.h>

#include "AssetDatabase/AssetStore_TextJsonl.h"
#include "AssetDatabase/IAssetStore.h"
#include "Assets/AssetRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace
{

static void WriteTextFile(const std::filesystem::path& p, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    ASSERT_TRUE(out.is_open()) << p.string();
    out << text;
}

} // namespace

TEST(AssetRegistryConcurrency, AsyncScanWithConcurrentReadsDoesNotCrash)
{
    namespace fs = std::filesystem;

    const fs::path tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_asset_registry_concurrency");
    std::error_code ec;
    fs::remove_all(tmpRoot, ec);
    fs::create_directories(tmpRoot, ec);

    // Create a bunch of assets (with stable AssetDatabase.assetdb entries) so repeated scans do not generate
    // different GUIDs for the same paths.
    constexpr int kNumAssets = 750;
    std::vector<GUID> guids;
    std::vector<fs::path> paths;
    guids.reserve(kNumAssets);
    paths.reserve(kNumAssets);

    const fs::path texDir = tmpRoot / "Textures";

    for (int i = 0; i < kNumAssets; ++i)
    {
        const fs::path assetPath = texDir / (std::string("tex_") + std::to_string(i) + ".png");
        const GUID g = GUID::Generate();

        // Dummy data; parsing is not required for registry tests.
        WriteTextFile(assetPath, "DUMMY");

        paths.push_back(assetPath);
        guids.push_back(g);
    }

    // Pre-create an authoritative AssetDatabase.assetdb file so the registry can load stable GUIDs
    // without relying on legacy .meta sidecars.
    {
        const fs::path dbPath = tmpRoot / "AssetDatabase.assetdb";
        std::ofstream out(dbPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open()) << "Failed to create: " << dbPath.string();
        for (int i = 0; i < kNumAssets; ++i)
        {
            const std::string rel = std::string("Textures/tex_") + std::to_string(i) + ".png";
            out << "{\"guid\":\"" << guids[i].ToString() << "\","
                << "\"path\":\"" << rel << "\","
                << "\"type\":\"Texture\","
                << "\"missing\":false,"
                << "\"kv\":{}}\n";
        }
    }

    JobSystem::WorkStealingThreadPool pool(4);
    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(tmpRoot, &pool));

    std::promise<void> firstRead;
    auto firstReadDone = firstRead.get_future();
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> readIters{0};

    std::thread reader([&]()
                       {
        size_t idx = 0;
        AssetMetadata tmp{};
        while (!stop.load(std::memory_order_acquire))
        {
            // Exercise thread-safe read APIs while the async scan mutates internal maps.
            (void)reg.GetAssetCount();

            idx = (idx + 1) % paths.size();
            (void)reg.IsAssetRegistered(guids[idx]);
            (void)reg.IsAssetRegistered(paths[idx]);
            (void)reg.GetAssetGUID(paths[idx]);
            (void)reg.TryGetAssetMetadata(guids[idx], tmp);
            (void)reg.TryGetAssetMetadata(paths[idx], tmp);

            if (readIters.fetch_add(1, std::memory_order_relaxed) == 0)
                firstRead.set_value();
            std::this_thread::yield();
        } });

    // The reader must have run before scan completion can stop it. A reader that never runs
    // fails the test instead of hanging the suite; the thread is joined first, since returning
    // with a joinable std::thread would abort the process.
    if (firstReadDone.wait_for(std::chrono::seconds(60)) != std::future_status::ready)
    {
        stop.store(true, std::memory_order_release);
        reader.join();
        FAIL() << "the reader thread did not complete one iteration within 60 s";
    }

    // Drive an async scan and wait for completion. The deadline only guards
    // against a hang: scanning 750 assets takes ~6s standalone but can exceed
    // 15s under parallel test load (#299), so keep generous headroom.
    auto scanFuture = reg.ScanDirectoryAsync(tmpRoot, true);
    const auto status = scanFuture.wait_for(std::chrono::seconds(60));

    // Stop the reader before any assertion: returning early with a joinable
    // std::thread would call std::terminate and abort the whole test process.
    stop.store(true, std::memory_order_release);
    reader.join();

    ASSERT_EQ(status, std::future_status::ready);
    (void)scanFuture.get();

    EXPECT_GE(reg.GetAssetCount(), static_cast<size_t>(kNumAssets));

    AssetMetadata md{};
    EXPECT_TRUE(reg.TryGetAssetMetadata(guids[0], md));

    reg.Shutdown();

    fs::remove_all(tmpRoot, ec);
}

// ---------------------------------------------------------------------------
// Atomic path→GUID claim probe (asset-splice-scale arc).
//
// Stored-identity sources used to mint GUIDs at two independent sites with no
// atomic path claim (RegisterAsset's own mint and GetOrCreateAssetGUID feeding
// RegisterAssetMetadataBatch); racing first-registrations of one path could
// mint two GUIDs and leave the in-memory maps disagreeing with the store —
// the PinnedSource flake family's residual mechanism (be72115da).
//
// This test IS that race, deliberately: three registrar shapes hammer the same
// fresh files concurrently — the startup scan (Initialize is not synchronized
// on), a direct-RegisterAsset thread, and a scan-shaped GetOrCreateAssetGUID +
// RegisterAssetMetadataBatch thread. Afterward, for every file, the map GUID,
// the store row, and the metadata lookup must agree on ONE identity.
//
// Run amplified (2-CPU affinity, ~200 reps) to gate the claim invariant.
// ---------------------------------------------------------------------------
TEST(AssetRegistryConcurrency, ConcurrentFirstRegistrationConvergesOnOneGuid)
{
    namespace fs = std::filesystem;

    constexpr int kRounds = 8;
    constexpr int kFilesPerRound = 12;

    for (int round = 0; round < kRounds; ++round)
    {
        const fs::path tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_atomic_claim");
        std::error_code ec;
        fs::remove_all(tmpRoot, ec);
        fs::create_directories(tmpRoot, ec);

        std::vector<fs::path> files;
        files.reserve(kFilesPerRound);
        for (int i = 0; i < kFilesPerRound; ++i)
        {
            const fs::path p = tmpRoot / ("claim_" + std::to_string(i) + ".txt");
            WriteTextFile(p, "claim probe " + std::to_string(i));
            files.push_back(p);
        }

        JobSystem::WorkStealingThreadPool pool(4);
        AssetRegistry reg;
        // Stored-identity project source (DerivedIdentity=false): the only
        // mount shape whose first registrations mint random GUIDs. The
        // startup scan this kicks off is racer #1 — deliberately NOT waited
        // on before the explicit racers start.
        ASSERT_TRUE(reg.Initialize(tmpRoot, &pool));

        std::atomic<bool> go{false};

        // Racer #2: direct RegisterAsset over every file.
        std::thread direct([&]
        {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (const auto& p : files)
                (void)reg.RegisterAsset(p);
        });

        // Racer #3: the scan fan-in shape — resolve identity via
        // GetOrCreateAssetGUID, then commit through RegisterAssetMetadataBatch
        // (mirrors AsyncRegistryTasks::RegisterWorkerTask).
        std::thread batch([&]
        {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            Vector<AssetMetadata> metadataBatch;
            metadataBatch.reserve(files.size());
            for (const auto& p : files)
            {
                AssetMetadata md{};
                md.Path = p;
                md.Guid = reg.GetOrCreateAssetGUID(p);
                if (!md.Guid.IsNull())
                    metadataBatch.push_back(std::move(md));
            }
            if (!metadataBatch.empty())
                (void)reg.RegisterAssetMetadataBatch(std::move(metadataBatch), nullptr);
        });

        go.store(true, std::memory_order_release);
        direct.join();
        batch.join();
        reg.WaitForStartupScan("project");

        // Convergence: one identity per path, agreed on by the path map, the
        // authoritative store, and the metadata map.
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        ASSERT_NE(pinned->Store.get(), nullptr);
        for (const auto& p : files)
        {
            const GUID mapGuid = reg.GetAssetGUID(p);
            EXPECT_FALSE(mapGuid.IsNull()) << p.string();

            const std::string rel = p.filename().string();
            const auto storeGuid = pinned->Store->LookupGuidByPath(rel);
            ASSERT_TRUE(storeGuid.has_value()) << rel << " missing from store";
            EXPECT_EQ(*storeGuid, mapGuid)
                << rel << ": store row and path map disagree (dual-mint window)";

            AssetDatabase::AssetRecord rec{};
            EXPECT_TRUE(pinned->Store->TryGetAsset(mapGuid, rec))
                << rel << ": map GUID " << mapGuid.ToString() << " has no store row";

            AssetMetadata md{};
            EXPECT_TRUE(reg.TryGetAssetMetadata(mapGuid, md))
                << rel << ": map GUID not resolvable to metadata";
        }

        reg.Shutdown();
        fs::remove_all(tmpRoot, ec);

        if (::testing::Test::HasFailure())
            break; // one divergent round is proof enough; keep logs small
    }
}

// ---------------------------------------------------------------------------
// Overlap re-claim reachability (S10c).
//
// Two sources mounted on the SAME root (the default-Player project/editor
// overlap). When the derived-identity source re-claims an already-registered
// path — RegisterAsset(path, preferredSourceAlias), the E5 remap — the old
// GUID's registry entry is re-keyed to the derived GUID. Callers that
// resolved the OLD GUID before the remap (scene refs mid-parse, queued
// loads) must still be able to reach the asset through the registry: the
// window where they couldn't is the overlapping-source load-null race
// (LoadAssetAsync(justResolvedGuid).get() == null, 3/100 amplified).
//
// The test resolves stable project GUIDs first, then races a remap thread
// against a resolve-and-query loop, and finally re-checks every pre-remap
// GUID after the remaps settle. Run amplified (2-CPU affinity, ~200 reps)
// alongside the claim probe above.
// ---------------------------------------------------------------------------
TEST(AssetRegistryConcurrency, OverlapRemapKeepsResolvedGuidReachable)
{
    namespace fs = std::filesystem;

    constexpr int kFiles = 24;

    const fs::path tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_overlap_remap");
    std::error_code ec;
    fs::remove_all(tmpRoot, ec);
    fs::create_directories(tmpRoot, ec);

    std::vector<fs::path> files;
    files.reserve(kFiles);
    for (int i = 0; i < kFiles; ++i)
    {
        const fs::path p = tmpRoot / ("overlap_" + std::to_string(i) + ".txt");
        WriteTextFile(p, "overlap probe " + std::to_string(i));
        files.push_back(p);
    }

    JobSystem::WorkStealingThreadPool pool(4);
    AssetRegistry reg;
    // Stored-identity project source rooted at tmpRoot.
    ASSERT_TRUE(reg.Initialize(tmpRoot, &pool));
    reg.WaitForStartupScan("project");

    // Legacy same-root overlap: the editor-shaped derived source shares the
    // project root (both keep RejectOverlappingRoots=false).
    AssetSourceDesc editorDesc{};
    editorDesc.Alias = "editor";
    editorDesc.Root = tmpRoot;
    editorDesc.DerivedIdentity = true;
    editorDesc.Priority = 50;
    editorDesc.RequiresScan = false;
    ASSERT_TRUE(reg.RegisterSource(editorDesc));

    // Resolve the pre-remap identities.
    std::vector<GUID> preRemapGuids;
    preRemapGuids.reserve(files.size());
    for (const auto& p : files)
    {
        const GUID g = reg.GetAssetGUID(p);
        ASSERT_FALSE(g.IsNull()) << p.string();
        preRemapGuids.push_back(g);
    }

    // Racer: the editor source re-claims every path (E5 remap).
    std::atomic<bool> go{false};
    std::atomic<bool> remapsDone{false};
    std::thread remapper([&]
    {
        while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (const auto& p : files)
            (void)reg.RegisterAsset(p, "editor");
        remapsDone.store(true, std::memory_order_release);
    });

    // Reader: a resolved GUID must stay reachable THROUGH the remap. This is
    // the exact caller shape that observed nulls: resolve, then query while
    // the re-claim lands.
    go.store(true, std::memory_order_release);
    size_t queried = 0;
    while (!remapsDone.load(std::memory_order_acquire))
    {
        for (size_t i = 0; i < files.size(); ++i)
        {
            AssetMetadata md{};
            EXPECT_TRUE(reg.TryGetAssetMetadata(preRemapGuids[i], md))
                << files[i].string() << ": pre-remap GUID " << preRemapGuids[i].ToString()
                << " became unreachable mid-remap (overlap load-null window)";
            ++queried;
        }
        if (::testing::Test::HasFailure())
            break;
    }
    remapper.join();
    EXPECT_GT(queried, 0u);

    // After the dust settles: old identities resolve via the remap alias,
    // and the path resolves to the (new) derived identity, which also
    // resolves.
    for (size_t i = 0; i < files.size(); ++i)
    {
        AssetMetadata md{};
        EXPECT_TRUE(reg.TryGetAssetMetadata(preRemapGuids[i], md))
            << files[i].string() << ": pre-remap GUID unreachable after remap settled";

        const GUID newGuid = reg.GetAssetGUID(files[i]);
        EXPECT_FALSE(newGuid.IsNull());
        EXPECT_TRUE(reg.TryGetAssetMetadata(newGuid, md));
    }

    reg.Shutdown();
    fs::remove_all(tmpRoot, ec);
}

// ---------------------------------------------------------------------------
// Rebind under concurrent readers (S11).
//
// RebindSource used to hold the writer lock across the whole store reload —
// readers stalled for the full rebind but could never observe intermediate
// state. The restructured rebind flips the source atomically and then
// drains/evicts in paced sections, so readers RUN during the transition;
// this probe pins down what they are allowed to see:
//   - a record identity (derived GUID, stable across equal-relpath roots)
//     must stay resolvable to metadata through every flip — no gap;
//   - the path must resolve through the OLD or the NEW root at any instant;
//   - GetAssetCount stays within [N, 2N] mid-flip (documented transient
//     union) and returns to exactly N once the rebind returns.
// ---------------------------------------------------------------------------
TEST(AssetRegistryConcurrency, RebindUnderConcurrentReadersStaysCoherent)
{
    namespace fs = std::filesystem;

    constexpr int kFiles = 300;
    constexpr int kFlips = 6;

    const fs::path tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_rebind_readers");
    std::error_code ec;
    fs::remove_all(tmpRoot, ec);

    const fs::path rootA = tmpRoot / "rootA";
    const fs::path rootB = tmpRoot / "rootB";
    for (const fs::path& root : {rootA, rootB})
    {
        fs::create_directories(root, ec);
        for (int i = 0; i < kFiles; ++i)
            WriteTextFile(root / ("f" + std::to_string(i) + ".txt"), "rebind probe");
    }
    for (int which = 0; which < 2; ++which)
    {
        const fs::path dbFile =
            tmpRoot / ("state" + std::to_string(which)) / "AssetDatabase.assetdb";
        fs::create_directories(dbFile.parent_path(), ec);
        std::ofstream out(dbFile, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        for (int i = 0; i < kFiles; ++i)
        {
            out << "{\"guid\":\"" << GUID::Generate().ToString() << "\","
                << "\"path\":\"f" << i << ".txt\","
                << "\"type\":\"Texture\",\"missing\":false,\"kv\":{}}\n";
        }
    }

    const auto makeDesc = [&](const fs::path& root, int stateIdx)
    {
        AssetSourceDesc desc{};
        desc.Alias = "project";
        desc.Root = root;
        desc.DerivedIdentity = true; // GUIDs derive from alias+relpath: stable across flips
        desc.AuthoritativeDbFile =
            tmpRoot / ("state" + std::to_string(stateIdx)) / "AssetDatabase.assetdb";
        desc.CacheRoot = tmpRoot / ("state" + std::to_string(stateIdx)) / "Cache";
        desc.Priority = 100;
        desc.RequiresScan = false;
        return desc;
    };

    JobSystem::WorkStealingThreadPool pool(4);
    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(&pool));
    ASSERT_TRUE(reg.RegisterSource(makeDesc(rootA, 0)));

    // Capture the stable identities once (rootA incarnation).
    std::vector<GUID> guids;
    guids.reserve(kFiles);
    for (int i = 0; i < kFiles; ++i)
    {
        const GUID g = reg.GetAssetGUID(rootA / ("f" + std::to_string(i) + ".txt"));
        ASSERT_FALSE(g.IsNull()) << i;
        guids.push_back(g);
    }

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> readIters{0};
    std::atomic<int> identityGaps{0};
    std::atomic<int> pathGaps{0};
    std::atomic<int> countTears{0};
    std::atomic<size_t> lastBadCount{0};
    // Bumped by the flipper BEFORE and AFTER each RebindSource. The path
    // check only counts a failure when both probes miss with NO epoch
    // change between them: the two probes are not atomic, so a reader
    // preempted across a flip boundary (or across the whole flip — it only
    // returns after draining) can legally pair an old-generation A-miss
    // with a new-generation B-miss, each individually consistent. A genuine
    // registry gap is a single state with neither binding, observable
    // between bumps — inside the long drain window — and is still caught.
    // The identity check above stays unconditional; it is the load-bearing
    // invariant.
    std::atomic<int> flipEpoch{0};

    const auto readerBody = [&](int seed)
    {
        AssetMetadata md{};
        size_t i = static_cast<size_t>(seed);
        while (!stop.load(std::memory_order_acquire))
        {
            i = (i + 1) % guids.size();
            // Identity must never have a resolution gap (single probe —
            // unconditionally epoch-safe).
            if (!reg.TryGetAssetMetadata(guids[i], md))
                identityGaps.fetch_add(1, std::memory_order_relaxed);
            // The path resolves through one root or the other at any instant.
            const std::string rel = "f" + std::to_string(i) + ".txt";
            // The pair check is only decisive OUTSIDE a flip (even epoch,
            // unchanged across both probes): the two probes are not atomic,
            // and during a flip's span — which includes ~100 ms of off-lock
            // staging before the atomic swap — a preempted reader can pair
            // an old-generation A-miss with a post-drain B-miss, each
            // individually legal (forensically confirmed: the failing pairs
            // straddled a flip-to-A's staging + drain; an immediate re-probe
            // of A hit). Mid-flip resolution is governed by the
            // unconditional identity check above.
            const int epochBefore = flipEpoch.load(std::memory_order_acquire);
            const bool viaA = !reg.GetAssetGUID(rootA / rel).IsNull();
            const bool viaB = !reg.GetAssetGUID(rootB / rel).IsNull();
            const int epochAfter = flipEpoch.load(std::memory_order_acquire);
            if (!viaA && !viaB && epochBefore == epochAfter && (epochBefore % 2) == 0)
                pathGaps.fetch_add(1, std::memory_order_relaxed);
            // Transient union upper bound; never torn below N.
            const size_t count = reg.GetAssetCount();
            if (count < static_cast<size_t>(kFiles) || count > static_cast<size_t>(2 * kFiles))
            {
                countTears.fetch_add(1, std::memory_order_relaxed);
                lastBadCount.store(count, std::memory_order_relaxed);
            }
            readIters.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread readerA(readerBody, 0);
    std::thread readerB(readerBody, kFiles / 2);

    for (int flip = 0; flip < kFlips; ++flip)
    {
        const bool toB = (flip % 2) == 0;
        flipEpoch.fetch_add(1, std::memory_order_release);
        ASSERT_TRUE(reg.RebindSource("project",
                                     toB ? makeDesc(rootB, 1) : makeDesc(rootA, 0)))
            << "flip " << flip;
        flipEpoch.fetch_add(1, std::memory_order_release);
        // At return the flip is complete: exact count, new root resolves.
        EXPECT_EQ(reg.GetAssetCount(), static_cast<size_t>(kFiles)) << "flip " << flip;
        const fs::path& liveRoot = toB ? rootB : rootA;
        EXPECT_FALSE(reg.GetAssetGUID(liveRoot / "f0.txt").IsNull()) << "flip " << flip;
    }

    stop.store(true, std::memory_order_release);
    readerA.join();
    readerB.join();

    EXPECT_GT(readIters.load(), 0u);
    EXPECT_EQ(identityGaps.load(), 0)
        << "a resolved GUID became unresolvable mid-rebind";
    // S12 drift tripwire: the lock-free occupancy counter must equal the
    // full-walk counts at this quiescent point after 6 flips' worth of
    // publish/drain/evict traffic.
    EXPECT_EQ(reg.GetAssetCount(), reg.GetRegisteredAssetPaths().size());
    EXPECT_EQ(pathGaps.load(), 0)
        << "a path resolved through neither the old nor the new root mid-rebind";
    EXPECT_EQ(countTears.load(), 0)
        << "GetAssetCount left [N, 2N] mid-rebind; last bad count = "
        << lastBadCount.load();

    reg.Shutdown();
    fs::remove_all(tmpRoot, ec);
}



// ---------------------------------------------------------------------------
// S12 occupancy-counter exactness through the full mount lifecycle.
//
// GetAssetCount is a lock-free relaxed load of m_AssetCount, maintained at
// the same compile-enforced choke points as the per-source index. This test
// is the drift tripwire: after every lifecycle mutation — mount (overlay
// publish + drain), single-asset register/unregister, rename, equal-size
// rebind, source unmount — the counter must equal BOTH full walks (the
// asset-shard walk via GetAssetsByType and the binding walk via
// GetRegisteredAssetPaths). A missed or double-applied delta anywhere fails
// here deterministically.
// ---------------------------------------------------------------------------
TEST(AssetRegistryConcurrency, AssetCountStaysExactThroughMountLifecycle)
{
    namespace fs = std::filesystem;

    constexpr int kFilesPerSource = 200;

    const fs::path tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_count_exact");
    std::error_code ec;
    fs::remove_all(tmpRoot, ec);

    const fs::path rootA = tmpRoot / "rootA";
    const fs::path rootB = tmpRoot / "rootB";
    const fs::path rootR = tmpRoot / "rootR"; // rebind target for source A
    for (const fs::path& root : {rootA, rootB, rootR})
    {
        fs::create_directories(root, ec);
        for (int i = 0; i < kFilesPerSource; ++i)
            WriteTextFile(root / ("f" + std::to_string(i) + ".png"), "px");
    }
    const auto writeDb = [&](const char* stateName)
    {
        const fs::path dbFile = tmpRoot / stateName / "AssetDatabase.assetdb";
        fs::create_directories(dbFile.parent_path(), ec);
        std::ofstream out(dbFile, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        for (int i = 0; i < kFilesPerSource; ++i)
        {
            out << "{\"guid\":\"" << GUID::Generate().ToString() << "\","
                << "\"path\":\"f" << i << ".png\","
                << "\"type\":\"Texture\",\"missing\":false,\"kv\":{}}\n";
        }
    };
    writeDb("stateA");
    writeDb("stateB");
    writeDb("stateR");

    const auto makeDesc = [&](const char* alias, const fs::path& root, const char* stateName,
                              int32_t priority)
    {
        AssetSourceDesc desc{};
        desc.Alias = alias;
        desc.Root = root;
        desc.DerivedIdentity = true;
        desc.AuthoritativeDbFile = tmpRoot / stateName / "AssetDatabase.assetdb";
        desc.CacheRoot = tmpRoot / stateName / "Cache";
        desc.Priority = priority;
        desc.RequiresScan = false;
        return desc;
    };

    JobSystem::WorkStealingThreadPool pool(4);
    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(&pool));

    const auto expectExact = [&](size_t expected, const char* stage)
    {
        const size_t counter = reg.GetAssetCount();
        const size_t assetWalk = reg.GetAssetsByType(AssetType::Texture).size();
        const size_t bindingWalk = reg.GetRegisteredAssetPaths().size();
        const Vector<AssetIndexRecord> indexSnapshot = reg.GetAssetIndexSnapshot();
        EXPECT_EQ(counter, expected) << stage;
        EXPECT_EQ(counter, assetWalk) << stage << ": counter diverged from the asset-shard walk";
        EXPECT_EQ(counter, bindingWalk) << stage << ": counter diverged from the binding walk";
        EXPECT_EQ(counter, indexSnapshot.size()) << stage << ": index snapshot diverged from the asset-shard walk";
        for (const AssetIndexRecord& record : indexSnapshot)
        {
            EXPECT_FALSE(record.Guid.IsNull()) << stage;
            EXPECT_FALSE(record.Path.empty()) << stage;
            EXPECT_EQ(record.Type, AssetType::Texture) << stage;
        }
    };

    expectExact(0, "empty registry");

    ASSERT_TRUE(reg.RegisterSource(makeDesc("project", rootA, "stateA", 100)));
    expectExact(kFilesPerSource, "after mount A");

    ASSERT_TRUE(reg.RegisterSource(makeDesc("pkgb", rootB, "stateB", 50)));
    expectExact(2 * kFilesPerSource, "after mount B");

    // Single-asset register (a fresh file the DBs don't know).
    const fs::path extra = rootA / "extra.png";
    WriteTextFile(extra, "px");
    ASSERT_TRUE(reg.RegisterAsset(extra));
    expectExact(2 * kFilesPerSource + 1, "after RegisterAsset");

    // Rename keeps identity: count unchanged.
    const fs::path renamed = rootA / "extra_renamed.png";
    fs::rename(extra, renamed, ec);
    ASSERT_TRUE(reg.TryRenameAssetPath(extra, renamed));
    expectExact(2 * kFilesPerSource + 1, "after rename");

    // Unregister by path: -1.
    ASSERT_TRUE(reg.TryUnregisterAssetByPath(renamed));
    expectExact(2 * kFilesPerSource, "after TryUnregisterAssetByPath");

    // Equal-size rebind of source A: publish + shared/unshared eviction +
    // drain nets to the same total.
    ASSERT_TRUE(reg.RebindSource("project", makeDesc("project", rootR, "stateR", 100)));
    expectExact(2 * kFilesPerSource, "after rebind A->R");

    // Unmount both sources.
    ASSERT_TRUE(reg.UnregisterSource("pkgb"));
    expectExact(kFilesPerSource, "after unmount B");
    ASSERT_TRUE(reg.UnregisterSource("project"));
    expectExact(0, "after unmount all");

    reg.Shutdown();
    fs::remove_all(tmpRoot, ec);
}

// Shutdown cancels the in-flight startup scan and joins it instead of waiting
// for it to hash and register the whole tree. The persisted store is the
// witness: a Shutdown that waits for the scan flushes a row for every file.
TEST(AssetRegistryConcurrency, ShutdownCancelsInFlightStartupScan)
{
    namespace fs = std::filesystem;

    // Spread over folders so a full scan spans several register chunks.
    constexpr int kFolders = 8;
    constexpr int kFilesPerFolder = 250;
    constexpr size_t kFileCount = static_cast<size_t>(kFolders) * kFilesPerFolder;

    const fs::path tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_shutdown_cancels_scan");
    std::error_code ec;
    fs::remove_all(tmpRoot, ec);
    for (int folder = 0; folder < kFolders; ++folder)
    {
        for (int i = 0; i < kFilesPerFolder; ++i)
        {
            WriteTextFile(tmpRoot / ("folder_" + std::to_string(folder)) / ("tex_" + std::to_string(i) + ".png"),
                          "DUMMY");
        }
    }

    JobSystem::WorkStealingThreadPool pool(4);
    {
        AssetRegistry reg;
        // Submits the startup scan and returns without waiting for it.
        ASSERT_TRUE(reg.Initialize(tmpRoot, &pool));

        const auto shutdownStart = std::chrono::steady_clock::now();
        reg.Shutdown();
        const auto shutdownTime = std::chrono::steady_clock::now() - shutdownStart;

        // A hang guard, generous for parallel test load; the store check
        // below is the machine-independent witness.
        EXPECT_LT(shutdownTime, std::chrono::seconds(5));
    }

    AssetDatabase::AssetStore_TextJsonl store;
    ASSERT_TRUE(store.LoadFromFile(tmpRoot / "AssetDatabase.assetdb", nullptr));
    EXPECT_LT(store.EnumerateAssets().size(), kFileCount)
        << "Shutdown waited for the startup scan to register every file";

    fs::remove_all(tmpRoot, ec);
}
