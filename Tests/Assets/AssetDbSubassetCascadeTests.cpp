// B2 of the derived-identity reconcile: subasset derive-key persistence and
// cascade healing. Embedded subassets (model clips, bridge materials) derive
// their GUIDs from the container's GUID — Derive(containerGuid, key) — so a
// container rename breaks every persisted subasset reference while no file
// evidence exists to heal them. Containers journal their derive keys
// (kSubassetDeriveKeysKvKey, written by AssetRegistry::RegisterSubassetDeriveKeys);
// container redirect emission (offline heal + live TryRenameAssetPath) then
// cascades Derive(old, key) -> Derive(new, key) per key, and every path that
// clears a stale container redirect clears its cascades with it.

#include <gtest/gtest.h>

#include "AssetCore/SubassetDeriveKeys.h"
#include "AssetDatabase/AssetDatabasePaths.h"
#include "AssetDatabase/IAssetDbCache.h"
#include "AssetDatabase/IAssetStore.h"
#include "Assets/AssetRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

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

// The container's subasset population under test: two embedded clips and one
// bridge material — the two real derive schemes.
const Vector<String> kTestKeys = {"embedded:0", "embedded:1", "material/0"};

} // namespace

// ---------------------------------------------------------------------------
// kv-row codec
// ---------------------------------------------------------------------------

TEST(AssetDbSubassetCascade, JoinSplitRoundtrip)
{
    EXPECT_EQ(JoinSubassetDeriveKeys({}), "");
    EXPECT_TRUE(SplitSubassetDeriveKeys("").empty());

    const Vector<String> one = {"embedded:0"};
    EXPECT_EQ(SplitSubassetDeriveKeys(JoinSubassetDeriveKeys(one)), one);

    EXPECT_EQ(JoinSubassetDeriveKeys(kTestKeys), "embedded:0\nembedded:1\nmaterial/0");
    EXPECT_EQ(SplitSubassetDeriveKeys(JoinSubassetDeriveKeys(kTestKeys)), kTestKeys);
}

// ---------------------------------------------------------------------------
// Offline heal cascade (editor closed between sessions)
// ---------------------------------------------------------------------------

// A container with journaled derive keys is renamed offline; the heal emits
// the container redirect PLUS one cascade redirect per key, references to the
// old derived subasset GUIDs resolve to the new ones, and all of it survives
// a restart AND an idle session (the stale-redirect sweep must never eat
// recordless-source cascade entries).
TEST(AssetDbSubassetCascade, OfflineContainerRenameCascadesSubassetRedirects)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_b2_cascade_rename");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "model.txt", "container content");

    GUID oldGuid = GUID::Null();

    // Session 1: journal derive keys through the real registration API.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        oldGuid = FindGuidByStorePath(*pinned, "sub/model.txt");
        ASSERT_FALSE(oldGuid.IsNull());

        ASSERT_TRUE(reg.RegisterSubassetDeriveKeys(root / "sub" / "model.txt", kTestKeys));
        std::string row;
        ASSERT_TRUE(pinned->Store->TryGetKeyValue(oldGuid, kSubassetDeriveKeysKvKey, row));
        EXPECT_EQ(row, JoinSubassetDeriveKeys(kTestKeys));
        EXPECT_TRUE(pinned->StoreDirty.load(std::memory_order_relaxed))
            << "a new derive-key row must dirty the store";
        ASSERT_TRUE(reg.SaveToFile({}));
    });
    // Session 2: cache + fingerprints + warm-start snapshot.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        ASSERT_TRUE(pinned->Cache);
    });

    fs::rename(root / "sub" / "model.txt", root / "sub" / "model_renamed.txt", ec);
    ASSERT_FALSE(ec) << ec.message();

    GUID newGuid = GUID::Null();

    // Session 3: the heal cascades.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store && pinned->Cache);

        newGuid = FindGuidByStorePath(*pinned, "sub/model_renamed.txt");
        ASSERT_FALSE(newGuid.IsNull());
        ASSERT_NE(newGuid, oldGuid);

        const auto containerTarget = pinned->Store->ResolveRedirect(oldGuid);
        ASSERT_TRUE(containerTarget.has_value()) << "container heal did not emit its redirect";
        ASSERT_EQ(*containerTarget, newGuid);

        for (const String& key : kTestKeys)
        {
            const GUID oldSub = GUID::Derive(oldGuid, key);
            const GUID newSub = GUID::Derive(newGuid, key);
            const auto target = pinned->Store->ResolveRedirect(oldSub);
            ASSERT_TRUE(target.has_value()) << "no cascade redirect for key '" << key << "'";
            EXPECT_EQ(*target, newSub) << "cascade for key '" << key << "' mis-targeted";
            EXPECT_EQ(reg.ResolveGuid(oldSub), newSub)
                << "a reference holding the old subasset GUID does not resolve forward";
        }

        // The key row migrated onto the new record, so a later reborn sweep
        // can find the cascade sources.
        std::string row;
        EXPECT_TRUE(pinned->Store->TryGetKeyValue(newGuid, kSubassetDeriveKeysKvKey, row));
        EXPECT_EQ(row, JoinSubassetDeriveKeys(kTestKeys));

        // Exactly container + one per key; no bogus extras.
        EXPECT_EQ(pinned->Store->EnumerateRedirects().size(), 1u + kTestKeys.size());
    });

    // Session 4: journaled, not session state — and an idle session's sweep
    // leaves recordless-source cascades alone.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        EXPECT_EQ(pinned->Store->EnumerateRedirects().size(), 1u + kTestKeys.size())
            << "an idle session changed the redirect population";
        for (const String& key : kTestKeys)
        {
            EXPECT_EQ(reg.ResolveGuid(GUID::Derive(oldGuid, key)), GUID::Derive(newGuid, key));
        }
    });

    fs::remove_all(root, ec);
}

// A container WITHOUT journaled derive keys (fresh machine that never loaded
// the model, or a pre-B2 journal) degrades to today's behavior: the container
// redirect alone, no cascades, no crash.
TEST(AssetDbSubassetCascade, HealWithoutDeriveKeysEmitsOnlyContainerRedirect)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_b2_cascade_nokeys");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "model.txt", "container content");

    GUID oldGuid = GUID::Null();
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        oldGuid = FindGuidByStorePath(*pinned, "sub/model.txt");
        ASSERT_FALSE(oldGuid.IsNull());
        ASSERT_TRUE(reg.SaveToFile({}));
    });
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        ASSERT_TRUE(pinned->Cache);
    });

    fs::rename(root / "sub" / "model.txt", root / "sub" / "model_renamed.txt", ec);
    ASSERT_FALSE(ec) << ec.message();

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        const GUID newGuid = FindGuidByStorePath(*pinned, "sub/model_renamed.txt");
        ASSERT_FALSE(newGuid.IsNull());
        const auto target = pinned->Store->ResolveRedirect(oldGuid);
        ASSERT_TRUE(target.has_value());
        EXPECT_EQ(*target, newGuid);
        EXPECT_EQ(pinned->Store->EnumerateRedirects().size(), 1u)
            << "a keyless heal must emit exactly the container redirect";
    });

    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// Live rename cascade (editor open)
// ---------------------------------------------------------------------------

// TryRenameAssetPath keeps the old GUID on the record for the session but
// journals old->new-derived; the cascade must ride along so persisted
// subasset references survive the next re-derivation of the new path.
TEST(AssetDbSubassetCascade, LiveRenameCascadesSubassetRedirects)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_b2_cascade_live");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "model.txt", "container content");

    GUID oldGuid = GUID::Null();
    GUID redirectTarget = GUID::Null();

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        oldGuid = reg.GetAssetGUID(root / "sub" / "model.txt");
        ASSERT_FALSE(oldGuid.IsNull());
        ASSERT_TRUE(reg.RegisterSubassetDeriveKeys(root / "sub" / "model.txt", kTestKeys));

        fs::rename(root / "sub" / "model.txt", root / "sub" / "model_renamed.txt", ec);
        ASSERT_FALSE(ec) << ec.message();
        ASSERT_TRUE(reg.TryRenameAssetPath(root / "sub" / "model.txt",
                                           root / "sub" / "model_renamed.txt"));

        const auto target = pinned->Store->ResolveRedirect(oldGuid);
        ASSERT_TRUE(target.has_value());
        redirectTarget = *target;
        ASSERT_NE(redirectTarget, oldGuid);

        for (const String& key : kTestKeys)
        {
            const GUID oldSub = GUID::Derive(oldGuid, key);
            const GUID newSub = GUID::Derive(redirectTarget, key);
            const auto subTarget = pinned->Store->ResolveRedirect(oldSub);
            ASSERT_TRUE(subTarget.has_value())
                << "live rename emitted no cascade for key '" << key << "'";
            EXPECT_EQ(*subTarget, newSub);
            EXPECT_EQ(reg.ResolveGuid(oldSub), newSub);
        }
        EXPECT_EQ(pinned->Store->EnumerateRedirects().size(), 1u + kTestKeys.size());
        ASSERT_TRUE(reg.SaveToFile({}));
    });

    // Offline edit so the next scan re-registers the new path (displacing the
    // live-rename's {old GUID, new path} row); the cascade targets must equal
    // the re-derived subasset GUIDs.
    WriteTextFile(root / "sub" / "model_renamed.txt", "container content plus an edit");

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        const GUID newGuid = reg.GetAssetGUID(root / "sub" / "model_renamed.txt");
        EXPECT_EQ(newGuid, redirectTarget);
        for (const String& key : kTestKeys)
        {
            EXPECT_EQ(reg.ResolveGuid(GUID::Derive(oldGuid, key)), GUID::Derive(newGuid, key))
                << "subasset reference broken after the new path re-derived (key '" << key << "')";
        }
    });

    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// Reborn container: cascades die with the container redirect (#965 analog)
// ---------------------------------------------------------------------------

// After a heal, the file is re-created at the old path: the container's old
// GUID speaks for itself again, the stale container redirect is removed, and
// every cascade redirect sourced at the old container's derived subasset
// identities must be removed with it — otherwise the reborn container's own
// subassets stay shadowed forever.
TEST(AssetDbSubassetCascade, RebornContainerClearsCascadeRedirects)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_b2_cascade_reborn");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "model.txt", "container content");

    GUID oldGuid = GUID::Null();
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        oldGuid = FindGuidByStorePath(*pinned, "sub/model.txt");
        ASSERT_FALSE(oldGuid.IsNull());
        ASSERT_TRUE(reg.RegisterSubassetDeriveKeys(root / "sub" / "model.txt", kTestKeys));
        ASSERT_TRUE(reg.SaveToFile({}));
    });
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        ASSERT_TRUE(pinned->Cache);
    });

    fs::rename(root / "sub" / "model.txt", root / "sub" / "model_renamed.txt", ec);
    ASSERT_FALSE(ec) << ec.message();

    GUID newGuid = GUID::Null();
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        newGuid = FindGuidByStorePath(*pinned, "sub/model_renamed.txt");
        ASSERT_FALSE(newGuid.IsNull());
        ASSERT_TRUE(pinned->Store->ResolveRedirect(oldGuid).has_value())
            << "precondition: the offline rename must heal";
        ASSERT_EQ(pinned->Store->EnumerateRedirects().size(), 1u + kTestKeys.size())
            << "precondition: the heal must cascade";
    });

    // The old path speaks for itself again.
    WriteTextFile(root / "sub" / "model.txt", "container content reborn");

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);

        EXPECT_EQ(FindGuidByStorePath(*pinned, "sub/model.txt"), oldGuid);
        EXPECT_TRUE(pinned->Store->ResolveRedirect(oldGuid) == std::nullopt)
            << "stale container redirect survived rebirth";
        for (const String& key : kTestKeys)
        {
            const GUID oldSub = GUID::Derive(oldGuid, key);
            EXPECT_TRUE(pinned->Store->ResolveRedirect(oldSub) == std::nullopt)
                << "stale cascade redirect survived rebirth (key '" << key << "')";
            EXPECT_EQ(reg.ResolveGuid(oldSub), oldSub)
                << "reborn subasset identity is still shadowed (key '" << key << "')";
        }
        // The rename target and its own identity are untouched.
        EXPECT_EQ(FindGuidByStorePath(*pinned, "sub/model_renamed.txt"), newGuid);
    });

    // Journaled, not session state.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        EXPECT_TRUE(pinned->Store->EnumerateRedirects().empty())
            << "cascade removal did not persist across a reload";
    });

    fs::remove_all(root, ec);
}

// Torn-append shape: the journal holds the ghost record, the container
// redirect, and the kv-migrated target record, but the cascade lines were
// lost — within one flush the redirect section drains from an unordered set,
// so a torn append can persist the container redirect while cutting the
// cascades. The completion sweep must finish the ghost removal AND re-emit
// the cascades from the target record's journaled keys.
TEST(AssetDbSubassetCascade, CompletionSweepReEmitsCascadesLostToTornAppend)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_b2_cascade_torn");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "model.txt", "container content");

    GUID oldGuid = GUID::Null();
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        oldGuid = FindGuidByStorePath(*pinned, "sub/model.txt");
        ASSERT_FALSE(oldGuid.IsNull());
        ASSERT_TRUE(reg.RegisterSubassetDeriveKeys(root / "sub" / "model.txt", kTestKeys));
        ASSERT_TRUE(reg.SaveToFile({}));
    });
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        ASSERT_TRUE(pinned->Cache);
    });

    fs::rename(root / "sub" / "model.txt", root / "sub" / "model_renamed.txt", ec);
    ASSERT_FALSE(ec) << ec.message();

    GUID newGuid = GUID::Null();
    // Heal normally, then manufacture the torn journal at store level: ghost
    // record back, cascade lines removed; the container redirect and the
    // kv-carrying target record stay.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        newGuid = FindGuidByStorePath(*pinned, "sub/model_renamed.txt");
        ASSERT_FALSE(newGuid.IsNull());
        ASSERT_TRUE(pinned->Store->ResolveRedirect(oldGuid).has_value())
            << "precondition: the offline rename must heal";

        AssetRecord ghost{};
        ghost.guid = oldGuid;
        ghost.path = "sub/model.txt";
        ASSERT_TRUE(pinned->Store->UpsertAsset(ghost, nullptr));
        for (const String& key : kTestKeys)
            ASSERT_TRUE(pinned->Store->RemoveRedirect(GUID::Derive(oldGuid, key), nullptr));
        pinned->StoreDirty.store(true, std::memory_order_relaxed);
        ASSERT_TRUE(reg.SaveToFile({}));
    });

    // A crash never writes the shutdown snapshot; drop it so the dir-mtime
    // gate cannot vouch for the manufactured ghost's directory.
    const AssetDatabasePaths dbPaths = GetDefaultPathsForAssetRoot(root, {}, {});
    fs::remove(dbPaths.cacheRoot / "watcher.snapshot.bin", ec);

    // Next startup: the completion sweep finishes the interrupted heal and
    // rebuilds the cascades.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        AssetRecord ghost{};
        EXPECT_FALSE(pinned->Store->TryGetAsset(oldGuid, ghost))
            << "completion sweep did not finish the ghost removal";
        for (const String& key : kTestKeys)
        {
            const auto target = pinned->Store->ResolveRedirect(GUID::Derive(oldGuid, key));
            ASSERT_TRUE(target.has_value())
                << "completion sweep did not re-emit the cascade (key '" << key << "')";
            EXPECT_EQ(*target, GUID::Derive(newGuid, key));
            EXPECT_EQ(reg.ResolveGuid(GUID::Derive(oldGuid, key)), GUID::Derive(newGuid, key));
        }
    });

    fs::remove_all(root, ec);
}

// The loader-side half of the reborn rule: RegisterSubassetDeriveKeys is the
// first moment the reborn container's keys are known on a machine whose
// journal never carried them forward. When the container has NO outgoing
// redirect, registration clears redirects sourced at its own derived subasset
// identities; when it still has one (live-rename residue), the cascades are
// load-bearing and must be kept.
TEST(AssetDbSubassetCascade, RegisterKeysClearsOwnStaleCascadesUnlessRedirected)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_b2_cascade_selfclear");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "model.txt", "container content");

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        const GUID guid = reg.GetAssetGUID(root / "sub" / "model.txt");
        ASSERT_FALSE(guid.IsNull());
        const GUID elsewhere = GUID::Derive(GUID::Null(), "b2-test/some-other-container");

        // Leftover cascades from an earlier heal of this GUID, container
        // redirect already gone (the #965 removal ran before keys existed).
        for (const String& key : kTestKeys)
        {
            ASSERT_TRUE(pinned->Store->AddRedirect(
                GUID::Derive(guid, key), GUID::Derive(elsewhere, key), nullptr));
        }

        ASSERT_TRUE(reg.RegisterSubassetDeriveKeys(root / "sub" / "model.txt", kTestKeys));
        for (const String& key : kTestKeys)
        {
            EXPECT_TRUE(pinned->Store->ResolveRedirect(GUID::Derive(guid, key)) == std::nullopt)
                << "registration did not clear the stale self-sourced cascade (key '"
                << key << "')";
        }

        // Live-rename residue shape: outgoing container redirect present.
        // Registration must now KEEP the cascades.
        ASSERT_TRUE(pinned->Store->AddRedirect(guid, elsewhere, nullptr));
        for (const String& key : kTestKeys)
        {
            ASSERT_TRUE(pinned->Store->AddRedirect(
                GUID::Derive(guid, key), GUID::Derive(elsewhere, key), nullptr));
        }
        ASSERT_TRUE(reg.RegisterSubassetDeriveKeys(root / "sub" / "model.txt", kTestKeys));
        for (const String& key : kTestKeys)
        {
            EXPECT_TRUE(pinned->Store->ResolveRedirect(GUID::Derive(guid, key)).has_value())
                << "registration cleared a load-bearing cascade behind a live container "
                   "redirect (key '" << key << "')";
        }
    });

    fs::remove_all(root, ec);
}

// Re-registering identical keys is journal-free (value-equality no-op): the
// warm path every model load takes must not dirty the store.
TEST(AssetDbSubassetCascade, ReRegisteringUnchangedKeysDoesNotDirtyTheStore)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_b2_cascade_noop");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "model.txt", "container content");

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        ASSERT_TRUE(reg.RegisterSubassetDeriveKeys(root / "sub" / "model.txt", kTestKeys));
        ASSERT_TRUE(reg.SaveToFile({}));
    });

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        // Drain any scan-induced dirtiness first so the assertion isolates
        // the re-registration.
        ASSERT_TRUE(reg.SaveToFile({}));
        pinned->StoreDirty.store(false, std::memory_order_relaxed);

        ASSERT_TRUE(reg.RegisterSubassetDeriveKeys(root / "sub" / "model.txt", kTestKeys));
        EXPECT_FALSE(pinned->StoreDirty.load(std::memory_order_relaxed))
            << "re-registering unchanged keys dirtied the store";
    });

    fs::remove_all(root, ec);
}

// A derived subasset identity names no record of its own; the journal of its container's derive
// keys is how a later session finds the container, and a journal written now is found at once.
TEST(AssetDbSubassetCascade, AJournaledSubassetFindsItsContainer)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_b2_subasset_container");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "model.txt", "container content");

    GUID container;
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        container = reg.GetAssetGUID(root / "sub" / "model.txt");
        ASSERT_FALSE(container.IsNull());
        EXPECT_TRUE(reg.FindSubassetContainer(GUID::Derive(container, "embedded:1")).IsNull())
            << "a subasset was found before its container journaled it";
        ASSERT_TRUE(reg.RegisterSubassetDeriveKeys(root / "sub" / "model.txt", kTestKeys));
        EXPECT_EQ(reg.FindSubassetContainer(GUID::Derive(container, "embedded:1")), container)
            << "a journal written this session was not found";
        ASSERT_TRUE(reg.SaveToFile({}));
    });

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        for (const String& key : kTestKeys)
            EXPECT_EQ(reg.FindSubassetContainer(GUID::Derive(container, key)), container) << key;
        EXPECT_TRUE(reg.FindSubassetContainer(GUID::Derive(container, "embedded:7")).IsNull());
        EXPECT_TRUE(reg.FindSubassetContainer(GUID::Generate()).IsNull());
    });

    fs::remove_all(root, ec);
}

