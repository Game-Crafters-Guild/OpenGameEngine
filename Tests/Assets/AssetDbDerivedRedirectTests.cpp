// S2 of the derived-identity reconcile: offline move/rename/delete
// discrimination. Under derived (path-hash) identity a rename changes the
// GUID by construction, so healing is redirect emission, never rebinding:
// the startup scan's tail matches absentees against the scan's !hadExisting
// registrations and, on a match, journals a redirect old→new, migrates the
// record's kv per-key copy-if-absent, and removes the ghost record —
// references heal at resolve time (AssetRegistry::ResolveGuid chases store
// redirects). Deletes stay tombstoned (no match → no redirect). Copies,
// case-only renames, safe-save replaces, and path swaps are identity-neutral
// and must emit nothing. The editor-open halves of the same matrix route
// through TryRenameAssetPath (live redirect) and TryUnregisterAssetByPath
// (tombstone); both are pinned here too.

#include <gtest/gtest.h>

#include "AssetCore/AssetIgnoreRules.h"
#include "AssetCore/SubassetDeriveKeys.h"
#include "AssetDatabase/AssetDatabasePaths.h"
#include "AssetDatabase/AssetDbCache_Sqlite.h"
#include "AssetDatabase/AssetStoreReconciler.h"
#include "AssetDatabase/AssetStore_TextJsonl.h"
#include "AssetDatabase/IAssetDbCache.h"
#include "AssetDatabase/IAssetStore.h"
#include "AssetDatabase/RedirectChain.h"
#include "Assets/AssetRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <algorithm>
#include <cstdio>
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

// Sessions 1+2 of the standard choreography: session 1 persists the journal
// (a fresh mount has no cache), session 2 opens the SQLite cache, populates
// fingerprints, and writes the warm-start snapshot at Shutdown. Offline
// mutations then happen between session 2 and the session under test.
void PrimeDerivedMount(const std::filesystem::path& root)
{
    RunDerivedSession(root, [&](AssetRegistry& reg) { ASSERT_TRUE(reg.SaveToFile({})); });
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        ASSERT_TRUE(pinned->Cache) << "with an existing .assetdb the SQLite cache must open";
    });
}

} // namespace

// ---------------------------------------------------------------------------
// Editor-closed matrix (offline mutation between sessions)
// ---------------------------------------------------------------------------

// The user-facing acceptance case: a referenced asset file is moved while
// nothing runs; the next startup emits a journaled redirect old->new,
// migrates the record's kv, removes the ghost record, and resolves
// references to the new identity — including after yet another restart
// (the redirect is journaled, not session state).
TEST(AssetDbDerivedRedirect, OfflineRenameHealsWithRedirectAndRemovesGhost)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_s2_rename");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "a.txt", "alpha content");
    WriteTextFile(root / "sub" / "keep.txt", "keeper content");

    GUID oldGuid = GUID::Null();
    GUID keepGuid = GUID::Null();

    // Session 1: persist the journal and author kv on a's record.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        oldGuid = FindGuidByStorePath(*pinned, "sub/a.txt");
        keepGuid = FindGuidByStorePath(*pinned, "sub/keep.txt");
        ASSERT_FALSE(oldGuid.IsNull());
        ASSERT_FALSE(keepGuid.IsNull());
        ASSERT_TRUE(pinned->Store->SetKeyValue(oldGuid, "importer_setting", "custom", nullptr));
        ASSERT_TRUE(reg.SaveToFile({}));
    });
    // Session 2: cache + fingerprints + snapshot.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        ASSERT_TRUE(pinned->Cache);
    });

    // Offline move: cross-directory rename (fs::rename keeps the OS file id,
    // so the tier-1 matcher must bind it).
    fs::create_directories(root / "other", ec);
    fs::rename(root / "sub" / "a.txt", root / "other" / "a_moved.txt", ec);
    ASSERT_FALSE(ec) << ec.message();

    GUID newGuid = GUID::Null();

    // Session 3: the scan tail heals.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store && pinned->Cache);

        newGuid = FindGuidByStorePath(*pinned, "other/a_moved.txt");
        ASSERT_FALSE(newGuid.IsNull()) << "moved file was not registered under its new path";
        ASSERT_NE(newGuid, oldGuid) << "derived identity must change with the path";

        // Redirect landed and the resolve seam chases it.
        const auto target = pinned->Store->ResolveRedirect(oldGuid);
        ASSERT_TRUE(target.has_value()) << "no redirect was emitted for the offline rename";
        EXPECT_EQ(*target, newGuid);
        EXPECT_EQ(reg.ResolveGuid(oldGuid), newGuid)
            << "a reference holding the old GUID does not resolve to the new identity";

        // kv migrated onto the new record.
        std::string v;
        EXPECT_TRUE(pinned->Store->TryGetKeyValue(newGuid, "importer_setting", v));
        EXPECT_EQ(v, "custom");

        // Ghost record removed from the journal; the stale resident entry
        // dropped from the raw in-memory maps...
        AssetRecord ghost{};
        EXPECT_FALSE(pinned->Store->TryGetAsset(oldGuid, ghost))
            << "healed ghost record still present in the store";
        EXPECT_FALSE(reg.IsAssetRegistered(oldGuid))
            << "healed ghost still resident in the in-memory registry";
        // ...while the redirect-aware metadata seam resolves the OLD guid to
        // the NEW identity (Unreal-style: references keep working).
        AssetMetadata md{};
        ASSERT_TRUE(reg.TryGetAssetMetadata(oldGuid, md))
            << "old-GUID metadata lookup does not resolve through the redirect";
        EXPECT_EQ(md.Guid, newGuid);
        EXPECT_TRUE(reg.TryGetAssetMetadata(newGuid, md));

        // The heal is identity movement: it must dirty the store so the
        // journal persists it.
        EXPECT_TRUE(pinned->StoreDirty.load(std::memory_order_relaxed))
            << "heal did not mark the store dirty — the redirect would never flush";

        // Control: the untouched sibling is unaffected.
        AssetRecord keepRec{};
        ASSERT_TRUE(pinned->Cache->TryGetAsset(keepGuid, keepRec));
        EXPECT_FALSE(keepRec.missing);
        EXPECT_TRUE(pinned->Store->ResolveRedirect(keepGuid) == std::nullopt);
    });

    // Session 4: the heal survives a restart — journaled, not session state.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        const auto target = pinned->Store->ResolveRedirect(oldGuid);
        ASSERT_TRUE(target.has_value()) << "redirect did not persist across sessions";
        EXPECT_EQ(*target, newGuid);
        EXPECT_EQ(reg.ResolveGuid(oldGuid), newGuid);
        AssetRecord ghost{};
        EXPECT_FALSE(pinned->Store->TryGetAsset(oldGuid, ghost));
    });

    fs::remove_all(root, ec);
}

// Offline delete: no rename target exists, so the record stays a tombstone —
// cache-flagged missing, journal record intact, no redirect. Pins that the
// matcher never invents a redirect for a genuine delete.
TEST(AssetDbDerivedRedirect, OfflineDeleteStaysTombstonedWithoutRedirect)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_s2_delete");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "a.txt", "alpha content");
    WriteTextFile(root / "sub" / "keep.txt", "keeper content");

    PrimeDerivedMount(root);

    fs::remove(root / "sub" / "a.txt", ec);

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store && pinned->Cache);

        const GUID ghostGuid = FindGuidByStorePath(*pinned, "sub/a.txt");
        ASSERT_FALSE(ghostGuid.IsNull()) << "tombstone record vanished from the store";

        // Tombstoned: cache-flagged missing, journal record intact.
        AssetRecord cacheRec{};
        ASSERT_TRUE(pinned->Cache->TryGetAsset(ghostGuid, cacheRec));
        EXPECT_TRUE(cacheRec.missing);

        // No redirect; the GUID resolves to itself (the tombstone), never
        // silently to another identity.
        EXPECT_TRUE(pinned->Store->ResolveRedirect(ghostGuid) == std::nullopt)
            << "a delete must not emit a redirect";
        EXPECT_EQ(reg.ResolveGuid(ghostGuid), ghostGuid);

        // The tombstone's identity + metadata remain resolvable in-memory
        // this session (loaded at mount; the resting missing flag is in the
        // cache), and GetMissingAssets reads BOTH residences, so a derived
        // ghost surfaces from the cache exactly as a stored one surfaces from
        // the journal. Supersedes the S1-era assertion that pinned the
        // opposite as a known gap.
        AssetMetadata md{};
        EXPECT_TRUE(reg.TryGetAssetMetadata(ghostGuid, md));
        const auto missingList = reg.GetMissingAssets();
        const bool surfaced =
            std::any_of(missingList.begin(), missingList.end(),
                        [&](const auto& mi) { return mi.guid == ghostGuid; });
        EXPECT_TRUE(surfaced)
            << "derived ghost not surfaced by GetMissingAssets — the cache residence is unread";

        // Positive control: the surviving file must NOT surface, so the
        // assertion above cannot pass by listing everything.
        const GUID keepGuid = FindGuidByStorePath(*pinned, "sub/keep.txt");
        ASSERT_FALSE(keepGuid.IsNull());
        EXPECT_FALSE(std::any_of(missingList.begin(), missingList.end(),
                                 [&](const auto& mi) { return mi.guid == keepGuid; }))
            << "a present file surfaced as missing";
    });

    fs::remove_all(root, ec);
}

// Rename + edit within the same folder: the OS file id is destroyed
// (delete + fresh write, as a git pull does) and the content hash no longer
// matches the cached fingerprint, so the per-(dir, ext) 1:1 uniqueness tier is
// the only one that fires — and co-location is not evidence. Her decision,
// verbatim: "SF-4, demote using purely extension as a self heal matcher."
//
// So the pairing is recorded as an advisory cache suggestion and NOTHING else
// happens: no redirect, no kv migration, no ghost removal. The absentee's
// outcome is byte-for-byte the no-match outcome — it stays a tombstone.
//
// Supersedes OfflineRenameWithEditHealsViaDirExtUniqueness, which pinned the
// auto-heal this replaces.
TEST(AssetDbDerivedRedirect, OfflineRenameWithEditSuggestsAndLeavesTombstone)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_s2_renameedit");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "a.txt", "alpha content");
    WriteTextFile(root / "sub" / "keep.png", "not a real png");

    GUID oldGuid = GUID::Null();
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        oldGuid = FindGuidByStorePath(*pinned, "sub/a.txt");
        ASSERT_FALSE(oldGuid.IsNull());
        ASSERT_TRUE(reg.SaveToFile({}));
    });
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        ASSERT_TRUE(pinned->Cache);
    });

    // Offline: new name, edited content, old file gone. Same dir, same ext,
    // exactly one missing and one new .txt in sub/.
    WriteTextFile(root / "sub" / "a_renamed.txt", "alpha content EDITED with extra bytes");
    fs::remove(root / "sub" / "a.txt", ec);

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store && pinned->Cache);

        const GUID newGuid = FindGuidByStorePath(*pinned, "sub/a_renamed.txt");
        ASSERT_FALSE(newGuid.IsNull());

        // No identity movement on dir+ext evidence.
        EXPECT_FALSE(pinned->Store->ResolveRedirect(oldGuid).has_value())
            << "dir-ext uniqueness must not emit a redirect";
        EXPECT_EQ(reg.ResolveGuid(oldGuid), oldGuid);

        // The tombstone survives, at its own path, missing-marked in the cache.
        AssetRecord ghost{};
        ASSERT_TRUE(pinned->Store->TryGetAsset(oldGuid, ghost))
            << "the ghost record must survive — no heal, no removal";
        EXPECT_EQ(ghost.path, "sub/a.txt");
        AssetRecord cachedGhost{};
        ASSERT_TRUE(pinned->Cache->TryGetAsset(oldGuid, cachedGhost));
        EXPECT_TRUE(cachedGhost.missing);

        // The guess is durable, and it names both sides plus its evidence tier.
        const auto suggestions = pinned->Cache->EnumerateRenameSuggestions();
        ASSERT_EQ(suggestions.size(), 1u);
        EXPECT_EQ(suggestions[0].MissingGuid, oldGuid);
        EXPECT_EQ(suggestions[0].MissingPath, "sub/a.txt");
        EXPECT_EQ(suggestions[0].CandidateGuid, newGuid);
        EXPECT_EQ(suggestions[0].CandidatePath, "sub/a_renamed.txt");
        EXPECT_EQ(suggestions[0].EvidenceTier, kRenameEvidenceTierDirExt);
    });

    fs::remove_all(root, ec);
}

// The suggestion is cache-resident by design. It survives reopening the cache,
// and losing the cache degrades to the no-match outcome — never to a heal.
//
// Cache loss does NOT resurrect the pairing: every tier, this one included,
// indexes an absentee only when the cache still holds its fingerprint (see the
// matcher's `TryGetFileFingerprint` guard), and an absentee's file is gone, so
// nothing can re-fingerprint it. That is a property of the whole matcher, not
// of the demotion, and it fails safe: the record stays a tombstone.
TEST(AssetDbDerivedRedirect, RenameSuggestionSurvivesReopenAndCacheLossFailsSafe)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_s2_suggestroundtrip");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "a.txt", "alpha content");

    GUID oldGuid = GUID::Null();
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        oldGuid = FindGuidByStorePath(*pinned, "sub/a.txt");
        ASSERT_FALSE(oldGuid.IsNull());
        ASSERT_TRUE(reg.SaveToFile({}));
    });
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        ASSERT_TRUE(reg.ProjectSourcePinned()->Cache);
    });

    WriteTextFile(root / "sub" / "a_renamed.txt", "alpha content EDITED with extra bytes");
    fs::remove(root / "sub" / "a.txt", ec);

    // Session that detects the pairing.
    IAssetDbCache::RenameSuggestion recorded{};
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        const auto suggestions = reg.ProjectSourcePinned()->Cache->EnumerateRenameSuggestions();
        ASSERT_EQ(suggestions.size(), 1u);
        recorded = suggestions[0];
        EXPECT_EQ(recorded.MissingGuid, oldGuid);
    });

    // Reopen: the row is on disk, not session state. Nothing new is detected
    // this session (the absentee already carries its suggestion), so what
    // comes back is the persisted row.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        const auto suggestions = reg.ProjectSourcePinned()->Cache->EnumerateRenameSuggestions();
        ASSERT_EQ(suggestions.size(), 1u);
        EXPECT_EQ(suggestions[0].MissingGuid, recorded.MissingGuid);
        EXPECT_EQ(suggestions[0].MissingPath, recorded.MissingPath);
        EXPECT_EQ(suggestions[0].CandidateGuid, recorded.CandidateGuid);
        EXPECT_EQ(suggestions[0].CandidatePath, recorded.CandidatePath);
        EXPECT_EQ(suggestions[0].EvidenceTier, recorded.EvidenceTier);
    });

    // Delete the cache — the "always safe to delete" contract.
    const AssetDatabasePaths dbPaths = GetDefaultPathsForAssetRoot(root, {}, {});
    ASSERT_TRUE(fs::exists(dbPaths.sqliteCacheFile)) << dbPaths.sqliteCacheFile.string();
    fs::remove(dbPaths.sqliteCacheFile, ec);
    ASSERT_FALSE(fs::exists(dbPaths.sqliteCacheFile));

    // What the absentee must NOT do after the loss is heal. Two mounts, so the
    // second one sees a cache that has had a full session to repopulate.
    RunDerivedSession(root, [](AssetRegistry&) {});
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store && pinned->Cache);
        EXPECT_FALSE(pinned->Store->ResolveRedirect(oldGuid).has_value())
            << "cache loss must never upgrade a suggestion into a heal";
        AssetRecord ghost{};
        EXPECT_TRUE(pinned->Store->TryGetAsset(oldGuid, ghost));
        EXPECT_TRUE(pinned->Cache->EnumerateRenameSuggestions().empty())
            << "an absentee the cache never fingerprinted is invisible to every tier";
        // The mechanism behind that, pinned so the claim above can be
        // falsified: the absentee's file is gone, so nothing re-fingerprints
        // it, and the matcher indexes only fingerprinted absentees.
        IAssetDbCache::FileFingerprint fp{};
        EXPECT_FALSE(pinned->Cache->TryGetFileFingerprint(oldGuid, fp));
    });

    fs::remove_all(root, ec);
}

// A copy is NOT a move: the original is still present, no record goes
// missing, and no redirect may be emitted for either identity.
TEST(AssetDbDerivedRedirect, OfflineCopyDoesNotRedirect)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_s2_copy");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "a.txt", "alpha content");

    GUID origGuid = GUID::Null();
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        origGuid = FindGuidByStorePath(*pinned, "sub/a.txt");
        ASSERT_FALSE(origGuid.IsNull());
        ASSERT_TRUE(reg.SaveToFile({}));
    });
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        ASSERT_TRUE(pinned->Cache);
    });

    fs::copy_file(root / "sub" / "a.txt", root / "sub" / "a_copy.txt", ec);
    ASSERT_FALSE(ec) << ec.message();

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store && pinned->Cache);

        // Both files registered under distinct identities; nothing missing,
        // nothing redirected.
        const GUID copyGuid = FindGuidByStorePath(*pinned, "sub/a_copy.txt");
        ASSERT_FALSE(copyGuid.IsNull());
        EXPECT_NE(copyGuid, origGuid);
        EXPECT_EQ(FindGuidByStorePath(*pinned, "sub/a.txt"), origGuid);

        EXPECT_TRUE(pinned->Store->EnumerateRedirects().empty())
            << "a duplicate-content copy emitted a redirect";
        AssetRecord rec{};
        ASSERT_TRUE(pinned->Cache->TryGetAsset(origGuid, rec));
        EXPECT_FALSE(rec.missing);
        EXPECT_EQ(reg.ResolveGuid(origGuid), origGuid);
        EXPECT_EQ(reg.ResolveGuid(copyGuid), copyGuid);
    });

    fs::remove_all(root, ec);
}

#ifdef _WIN32
// Case-only rename on a case-insensitive filesystem: the derive key
// casefolds, so the GUID is unchanged — identity-neutral, no ghost, no
// redirect. Registration casefolds canonical paths on this platform
// (NormalizePathForMap), so the record keeps its original path key; the
// new-cased path must still resolve to the same GUID.
TEST(AssetDbDerivedRedirect, OfflineCaseOnlyRenameKeepsIdentity)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_s2_caseonly");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "asset.txt", "alpha content");

    GUID guid = GUID::Null();
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        guid = FindGuidByStorePath(*pinned, "sub/asset.txt");
        ASSERT_FALSE(guid.IsNull());
        ASSERT_TRUE(reg.SaveToFile({}));
    });
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        ASSERT_TRUE(pinned->Cache);
    });

    fs::rename(root / "sub" / "asset.txt", root / "sub" / "ASSET.txt", ec);
    ASSERT_FALSE(ec) << ec.message();

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store && pinned->Cache);

        // Identity-neutral: registration casefolds canonical paths on this
        // platform (NormalizePathForMap), so the record keeps its original
        // path key and no identity moves; what must hold is that the
        // new-cased path resolves to the same GUID.
        EXPECT_EQ(FindGuidByStorePath(*pinned, "sub/asset.txt"), guid)
            << "case-only rename minted a new identity or dropped the record";
        EXPECT_EQ(reg.GetAssetGUID(root / "sub" / "ASSET.txt"), guid)
            << "new-cased path does not resolve to the original identity";
        EXPECT_TRUE(pinned->Store->EnumerateRedirects().empty())
            << "case-only rename emitted a redirect";
        AssetRecord rec{};
        ASSERT_TRUE(pinned->Cache->TryGetAsset(guid, rec));
        EXPECT_FALSE(rec.missing) << "case-only rename ghost-marked the record";
        AssetMetadata md{};
        EXPECT_TRUE(reg.TryGetAssetMetadata(guid, md));
    });

    fs::remove_all(root, ec);
}
#endif // _WIN32

// Replace-style safe-save performed offline (write temp, rename over the
// target): the destination path keeps its identity — content replacement,
// not identity movement.
TEST(AssetDbDerivedRedirect, OfflineSafeSaveReplaceKeepsDestinationIdentity)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_s2_safesave");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "a.txt", "alpha content v1");

    GUID guid = GUID::Null();
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        guid = FindGuidByStorePath(*pinned, "sub/a.txt");
        ASSERT_FALSE(guid.IsNull());
        ASSERT_TRUE(reg.SaveToFile({}));
    });
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        ASSERT_TRUE(pinned->Cache);
    });

    // Offline safe-save: temp file renamed over the destination.
    WriteTextFile(root / "sub" / "a.txt.saving", "alpha content v2 - replaced bytes");
    fs::rename(root / "sub" / "a.txt.saving", root / "sub" / "a.txt", ec);
    ASSERT_FALSE(ec) << ec.message();

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store && pinned->Cache);

        EXPECT_EQ(FindGuidByStorePath(*pinned, "sub/a.txt"), guid)
            << "safe-save replace changed the destination's identity";
        EXPECT_TRUE(pinned->Store->EnumerateRedirects().empty())
            << "safe-save replace emitted a redirect";
        AssetRecord rec{};
        ASSERT_TRUE(pinned->Cache->TryGetAsset(guid, rec));
        EXPECT_FALSE(rec.missing);
    });

    fs::remove_all(root, ec);
}

// Swapping two paths offline: under derived identity the path IS the
// identity, so a swap is a content edit of both paths — no records go
// missing, no redirects are emitted, both GUIDs stay bound to their paths.
// This matches the editor-open behaviour (replace-style renames keep the
// destination's identity).
TEST(AssetDbDerivedRedirect, OfflineSwapKeepsPathIdentities)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_s2_swap");
    const fs::path hidden = TestUtils::MakeUniqueTempDirectory("ge_derived_s2_swap_tmp");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove_all(hidden, ec);
    fs::create_directories(root, ec);
    fs::create_directories(hidden, ec);
    WriteTextFile(root / "sub" / "f1.txt", "content of file one");
    WriteTextFile(root / "sub" / "f2.txt", "content of file TWO (different length)");

    GUID guid1 = GUID::Null();
    GUID guid2 = GUID::Null();
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        guid1 = FindGuidByStorePath(*pinned, "sub/f1.txt");
        guid2 = FindGuidByStorePath(*pinned, "sub/f2.txt");
        ASSERT_FALSE(guid1.IsNull());
        ASSERT_FALSE(guid2.IsNull());
        ASSERT_TRUE(reg.SaveToFile({}));
    });
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        ASSERT_TRUE(pinned->Cache);
    });

    // Offline swap via a temp outside the mount.
    fs::rename(root / "sub" / "f1.txt", hidden / "swap.tmp", ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::rename(root / "sub" / "f2.txt", root / "sub" / "f1.txt", ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::rename(hidden / "swap.tmp", root / "sub" / "f2.txt", ec);
    ASSERT_FALSE(ec) << ec.message();

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store && pinned->Cache);

        EXPECT_EQ(FindGuidByStorePath(*pinned, "sub/f1.txt"), guid1);
        EXPECT_EQ(FindGuidByStorePath(*pinned, "sub/f2.txt"), guid2);
        EXPECT_TRUE(pinned->Store->EnumerateRedirects().empty())
            << "a path swap emitted redirects";
        AssetRecord rec{};
        ASSERT_TRUE(pinned->Cache->TryGetAsset(guid1, rec));
        EXPECT_FALSE(rec.missing);
        ASSERT_TRUE(pinned->Cache->TryGetAsset(guid2, rec));
        EXPECT_FALSE(rec.missing);
    });

    fs::remove_all(root, ec);
    fs::remove_all(hidden, ec);
}

// Interrupted heal: the redirect committed but the ghost record's removal was
// lost (the heal's last step). The next startup's completion sweep must
// finish the removal — and must not require the rename target to reappear as
// a scan candidate (it has been a registered record since the heal).
TEST(AssetDbDerivedRedirect, InterruptedHealIsCompletedNextSession)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_s2_interrupted");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "c.txt", "charlie content");

    GUID oldGuid = GUID::Null();
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        oldGuid = FindGuidByStorePath(*pinned, "sub/c.txt");
        ASSERT_FALSE(oldGuid.IsNull());
        ASSERT_TRUE(reg.SaveToFile({}));
    });
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        ASSERT_TRUE(pinned->Cache);
    });

    fs::rename(root / "sub" / "c.txt", root / "sub" / "c_moved.txt", ec);
    ASSERT_FALSE(ec) << ec.message();

    GUID newGuid = GUID::Null();

    // Session 3: normal heal, then reconstruct the interrupted state —
    // redirect present, ghost record present, file absent — exactly what a
    // crash between the redirect flush and the removal flush leaves behind.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        newGuid = FindGuidByStorePath(*pinned, "sub/c_moved.txt");
        ASSERT_FALSE(newGuid.IsNull());
        const auto target = pinned->Store->ResolveRedirect(oldGuid);
        ASSERT_TRUE(target.has_value());
        ASSERT_EQ(*target, newGuid);

        AssetRecord ghost{};
        ghost.guid = oldGuid;
        ghost.path = "sub/c.txt";
        ghost.type = AssetType::Unknown;
        ASSERT_TRUE(pinned->Store->UpsertAsset(ghost, nullptr));
        ASSERT_TRUE(reg.SaveToFile({}));
    });

    // A crash never writes the shutdown snapshot; drop it so the dir-mtime
    // gate cannot vouch for the manufactured ghost's directory.
    const AssetDatabasePaths dbPaths = GetDefaultPathsForAssetRoot(root, {}, {});
    fs::remove(dbPaths.cacheRoot / "watcher.snapshot.bin", ec);

    // Session 4: the completion sweep finishes the removal.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);

        AssetRecord ghost{};
        EXPECT_FALSE(pinned->Store->TryGetAsset(oldGuid, ghost))
            << "completion sweep did not remove the interrupted heal's ghost record";
        const auto target = pinned->Store->ResolveRedirect(oldGuid);
        ASSERT_TRUE(target.has_value()) << "completion sweep dropped the redirect";
        EXPECT_EQ(*target, newGuid);
        EXPECT_EQ(reg.ResolveGuid(oldGuid), newGuid);
        EXPECT_FALSE(reg.IsAssetRegistered(oldGuid))
            << "completed ghost still resident in the in-memory registry";
        // The redirect-aware metadata seam resolves the old GUID forward.
        AssetMetadata md{};
        ASSERT_TRUE(reg.TryGetAssetMetadata(oldGuid, md))
            << "old-GUID metadata lookup does not resolve through the redirect";
        EXPECT_EQ(md.Guid, newGuid);
    });

    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// Editor-open matrix (watcher-routed live paths)
// ---------------------------------------------------------------------------

// The watcher routes rename events through TryRenameAssetPath
// (AssetManager::HandleFileChange). For a derived mount that call must
// append the old->new-derived redirect live, and the next session must
// converge on {new record + redirect} once the file re-registers.
TEST(AssetDbDerivedRedirect, WatcherRenameEmitsRedirectAndConverges)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_s2_liverename");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "a.txt", "alpha content");

    GUID oldGuid = GUID::Null();
    GUID redirectTarget = GUID::Null();

    // Live session: rename on disk, then the watcher's registry route.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        oldGuid = reg.GetAssetGUID(root / "sub" / "a.txt");
        ASSERT_FALSE(oldGuid.IsNull());

        fs::rename(root / "sub" / "a.txt", root / "sub" / "renamed.txt", ec);
        ASSERT_FALSE(ec) << ec.message();
        ASSERT_TRUE(reg.TryRenameAssetPath(root / "sub" / "a.txt", root / "sub" / "renamed.txt"));

        // The live redirect landed in the store and the resolve seam chases
        // it, while the in-memory registry keeps the old GUID attached to
        // the new path for this session (no consumer churn).
        const auto target = pinned->Store->ResolveRedirect(oldGuid);
        ASSERT_TRUE(target.has_value())
            << "TryRenameAssetPath did not append a redirect for the derived mount";
        redirectTarget = *target;
        EXPECT_NE(redirectTarget, oldGuid);
        EXPECT_EQ(reg.ResolveGuid(oldGuid), redirectTarget);
        EXPECT_EQ(reg.GetAssetGUID(root / "sub" / "renamed.txt"), oldGuid);
        ASSERT_TRUE(reg.SaveToFile({}));
    });

    // Offline edit so the next scan re-registers the file (a byte appended
    // breaks snapshot-currency) — that re-registration is what displaces the
    // live-rename's {old GUID, new path} store row.
    WriteTextFile(root / "sub" / "renamed.txt", "alpha content plus an edit");

    // Next session: the new path re-derives its own GUID, which must equal
    // the redirect's target; the old record is displaced by the store's
    // path-conflict override; references still heal.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);

        const GUID newGuid = reg.GetAssetGUID(root / "sub" / "renamed.txt");
        EXPECT_EQ(newGuid, redirectTarget)
            << "re-derived GUID does not match the live redirect's target";
        EXPECT_EQ(reg.ResolveGuid(oldGuid), redirectTarget);
        AssetRecord ghost{};
        EXPECT_FALSE(pinned->Store->TryGetAsset(oldGuid, ghost))
            << "the live-rename's old-GUID row survived re-registration";
        EXPECT_TRUE(pinned->Store->EnumerateRedirects().size() == 1)
            << "expected exactly the one live redirect";
    });

    fs::remove_all(root, ec);
}

// Rename-kept identity heal: a live rename keeps the renamed asset's session
// GUID attached to its NEW path, so the first registration of the OLD path —
// the next `Create > Material` minting the same default stem — derives that
// same GUID and used to be refused as a derive collision, leaving the new
// asset identity-dead. The registrar must instead migrate the renamed asset
// onto its own path-derived GUID and register the new file normally: both
// assets healthy, distinct GUIDs, the rename redirect retired (the old GUID
// speaks for itself again).
TEST(AssetDbDerivedRedirect, RegisteringTheOldPathAfterALiveRenameHealsTheKeptIdentity)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_rename_heal");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "a.txt", "alpha content");

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        const GUID pathDerived = reg.GetAssetGUID(root / "sub" / "a.txt");
        ASSERT_FALSE(pathDerived.IsNull());

        fs::rename(root / "sub" / "a.txt", root / "sub" / "b.txt", ec);
        ASSERT_FALSE(ec) << ec.message();
        ASSERT_TRUE(reg.TryRenameAssetPath(root / "sub" / "a.txt", root / "sub" / "b.txt"));

        // A remap through the E5 seam must reach the manager's callback.
        GUID remapOld = GUID::Null();
        GUID remapNew = GUID::Null();
        reg.SetGuidRemapCallback(
            [&](const GUID& oldGuid, const GUID& newGuid, const std::filesystem::path&) {
                remapOld = oldGuid;
                remapNew = newGuid;
            });

        // A new file takes the old path.
        WriteTextFile(root / "sub" / "a.txt", "a fresh, unrelated asset");
        ASSERT_TRUE(reg.RegisterAsset(root / "sub" / "a.txt"))
            << "derive-collision refusal: the rename-kept identity was not released";

        // The registrant owns the identity its path derives; the renamed
        // asset owns its own path-derived GUID. Distinct, both resolvable.
        const GUID registrantGuid = reg.GetAssetGUID(root / "sub" / "a.txt");
        const GUID renamedGuid = reg.GetAssetGUID(root / "sub" / "b.txt");
        EXPECT_EQ(registrantGuid, pathDerived);
        ASSERT_FALSE(renamedGuid.IsNull());
        EXPECT_NE(renamedGuid, registrantGuid);

        AssetMetadata registrantMd{};
        ASSERT_TRUE(reg.TryGetAssetMetadata(registrantGuid, registrantMd));
        EXPECT_EQ(registrantMd.Path.filename(), fs::path("a.txt"));
        AssetMetadata renamedMd{};
        ASSERT_TRUE(reg.TryGetAssetMetadata(renamedGuid, renamedMd));
        EXPECT_EQ(renamedMd.Path.filename(), fs::path("b.txt"));

        // The manager was told to re-key its maps.
        EXPECT_EQ(remapOld, pathDerived);
        EXPECT_EQ(remapNew, renamedGuid);

        // Store rows follow: each GUID's record names its own path, and the
        // rename redirect is retired — the re-registered GUID would otherwise
        // be permanently shadowed by it.
        AssetRecord row{};
        ASSERT_TRUE(pinned->Store->TryGetAsset(registrantGuid, row));
        EXPECT_EQ(row.path, "sub/a.txt");
        ASSERT_TRUE(pinned->Store->TryGetAsset(renamedGuid, row));
        EXPECT_EQ(row.path, "sub/b.txt");
        EXPECT_FALSE(pinned->Store->ResolveRedirect(pathDerived).has_value())
            << "the rename redirect must retire when its source GUID re-registers";
    });

    fs::remove_all(root, ec);
}

// The live editor's version of the heal scenario: after the rename, the
// WATCHER re-registers the renamed path WITH its source alias, and that
// overload's reassign already migrates the renamed asset onto its
// path-derived GUID — recording the S10c session alias
// {old-derived → new-derived}. The old path's re-registration then finds its
// derived GUID reachable only through that alias, which a real registration
// shadows by design: the derive-collision guard must read primary occupancy
// and register normally. (Alias-chased occupancy refused this exact shape in
// the editor — every watcher retry re-logged the corruption error and the
// created asset stayed identity-dead — while the alias-free variant above
// stayed green.)
TEST(AssetDbDerivedRedirect, RegisteringTheOldPathAfterTheWatcherReassignedTheRenamedPath)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_rename_reassign");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "a.txt", "alpha content");

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        const GUID pathDerived = reg.GetAssetGUID(root / "sub" / "a.txt");
        ASSERT_FALSE(pathDerived.IsNull());

        fs::rename(root / "sub" / "a.txt", root / "sub" / "b.txt", ec);
        ASSERT_FALSE(ec) << ec.message();
        ASSERT_TRUE(reg.TryRenameAssetPath(root / "sub" / "a.txt", root / "sub" / "b.txt"));

        // Watcher leg: the aliased re-registration of the NEW path reassigns
        // the renamed asset onto its own path-derived GUID and leaves the
        // session alias behind.
        ASSERT_TRUE(reg.RegisterAsset(root / "sub" / "b.txt", "project"));
        const GUID renamedGuid = reg.GetAssetGUID(root / "sub" / "b.txt");
        ASSERT_FALSE(renamedGuid.IsNull());
        EXPECT_NE(renamedGuid, pathDerived);

        // A new file takes the old path: its derived GUID is alias-only now,
        // so registration must shadow the alias, not refuse.
        WriteTextFile(root / "sub" / "a.txt", "a fresh, unrelated asset");
        ASSERT_TRUE(reg.RegisterAsset(root / "sub" / "a.txt"))
            << "an alias-only GUID was mistaken for an occupied one";
        EXPECT_EQ(reg.GetAssetGUID(root / "sub" / "a.txt"), pathDerived);
        EXPECT_EQ(reg.GetAssetGUID(root / "sub" / "b.txt"), renamedGuid);

        AssetMetadata md{};
        ASSERT_TRUE(reg.TryGetAssetMetadata(pathDerived, md));
        EXPECT_EQ(md.Path.filename(), fs::path("a.txt"));
        ASSERT_TRUE(reg.TryGetAssetMetadata(renamedGuid, md));
        EXPECT_EQ(md.Path.filename(), fs::path("b.txt"));
    });

    fs::remove_all(root, ec);
}

// The session alias an aliased re-registration leaves behind is consulted
// only after a primary probe misses, so the new file at the old path shadows
// it. Shadowed is not retired: once that file is gone the alias would answer
// old-GUID lookups with the renamed asset — a deleted NewSurface.material
// resolving to Rock.material. A registration retires the alias for good.
TEST(AssetDbDerivedRedirect, RegisteringAnAliasedGuidRetiresTheAliasForGood)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_alias_retire");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "a.txt", "alpha content");

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        const GUID pathDerived = reg.GetAssetGUID(root / "sub" / "a.txt");
        ASSERT_FALSE(pathDerived.IsNull());

        fs::rename(root / "sub" / "a.txt", root / "sub" / "b.txt", ec);
        ASSERT_FALSE(ec) << ec.message();
        ASSERT_TRUE(reg.TryRenameAssetPath(root / "sub" / "a.txt", root / "sub" / "b.txt"));
        ASSERT_TRUE(reg.RegisterAsset(root / "sub" / "b.txt", "project"));
        const GUID renamedGuid = reg.GetAssetGUID(root / "sub" / "b.txt");
        ASSERT_NE(renamedGuid, pathDerived);
        EXPECT_EQ(reg.ResolveSessionAlias(pathDerived), renamedGuid)
            << "until the old path is claimed again, the old GUID follows the renamed asset";

        WriteTextFile(root / "sub" / "a.txt", "a fresh, unrelated asset");
        ASSERT_TRUE(reg.RegisterAsset(root / "sub" / "a.txt"));
        EXPECT_EQ(reg.ResolveSessionAlias(pathDerived), pathDerived);

        ASSERT_TRUE(reg.TryUnregisterAssetByPath(root / "sub" / "a.txt"));
        EXPECT_EQ(reg.ResolveSessionAlias(pathDerived), pathDerived);
        AssetMetadata md{};
        EXPECT_FALSE(reg.TryGetAssetMetadata(pathDerived, md))
            << "a retired alias must not resurface as " << md.Path.string();
        ASSERT_TRUE(reg.TryGetAssetMetadata(renamedGuid, md));
        EXPECT_EQ(md.Path.filename(), fs::path("b.txt"));
    });

    fs::remove_all(root, ec);
}

#ifdef _WIN32
// Case-only rename through the live path: the new path derives the SAME
// GUID (the derive key casefolds), so no redirect may be recorded.
TEST(AssetDbDerivedRedirect, WatcherCaseOnlyRenameEmitsNoRedirect)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_s2_livecase");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "asset.txt", "alpha content");

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        const GUID guid = reg.GetAssetGUID(root / "sub" / "asset.txt");
        ASSERT_FALSE(guid.IsNull());

        fs::rename(root / "sub" / "asset.txt", root / "sub" / "ASSET.txt", ec);
        ASSERT_FALSE(ec) << ec.message();
        ASSERT_TRUE(reg.TryRenameAssetPath(root / "sub" / "asset.txt", root / "sub" / "ASSET.txt"));

        EXPECT_EQ(reg.GetAssetGUID(root / "sub" / "ASSET.txt"), guid);
        EXPECT_TRUE(pinned->Store->EnumerateRedirects().empty())
            << "case-only live rename recorded a redirect";
    });

    fs::remove_all(root, ec);
}
#endif // _WIN32

// A file registered one at a time (an editor action that writes it) and a file
// renamed through the live path are named as the caller's path spells them, as
// the scan names a file from its scanned path; the case-folded form is the map
// key only.
TEST(AssetDbDerivedRedirect, RegisteredAndRenamedAssetsKeepTheirFileNameCase)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_name_case");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        WriteTextFile(root / "sub" / "Particle Stack.txt", "alpha content");
        ASSERT_TRUE(reg.RegisterAsset(root / "sub" / "Particle Stack.txt"));
        AssetMetadata registered{};
        ASSERT_TRUE(reg.TryGetAssetMetadata(reg.GetAssetGUID(root / "sub" / "Particle Stack.txt"), registered));
        EXPECT_EQ(registered.Name, "Particle Stack");

        fs::rename(root / "sub" / "Particle Stack.txt", root / "sub" / "Smoke Stack.txt", ec);
        ASSERT_FALSE(ec) << ec.message();
        ASSERT_TRUE(reg.TryRenameAssetPath(root / "sub" / "Particle Stack.txt", root / "sub" / "Smoke Stack.txt"));
        AssetMetadata renamed{};
        ASSERT_TRUE(reg.TryGetAssetMetadata(reg.GetAssetGUID(root / "sub" / "Smoke Stack.txt"), renamed));
        EXPECT_EQ(renamed.Name, "Smoke Stack");
    });

    fs::remove_all(root, ec);
}

// The spelling survives restarts: the store record keeps it (the folded form is
// derived at comparison, never stored), and a warm start, whose scan skips
// files that match the snapshot, names the asset from the stored record.
TEST(AssetDbDerivedRedirect, RegisteredAssetKeepsItsNameCaseAcrossWarmStarts)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_name_case_restart");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    const fs::path file = root / "sub" / "Particle Stack.txt";

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        WriteTextFile(file, "alpha content");
        ASSERT_TRUE(reg.RegisterAsset(file));
        ASSERT_TRUE(reg.SaveToFile({}));
    });
    for (int session = 2; session <= 3; ++session)
    {
        RunDerivedSession(root, [&](AssetRegistry& reg) {
            AssetMetadata md{};
            ASSERT_TRUE(reg.TryGetAssetMetadata(reg.GetAssetGUID(file), md)) << "session " << session;
            EXPECT_EQ(md.Name, "Particle Stack") << "session " << session;
            auto pinned = reg.ProjectSourcePinned();
            ASSERT_TRUE(pinned && pinned->Store);
            AssetRecord row{};
            ASSERT_TRUE(pinned->Store->TryGetAsset(md.Guid, row));
            EXPECT_EQ(row.path, "sub/Particle Stack.txt") << "session " << session;
            ASSERT_TRUE(reg.SaveToFile({}));
        });
    }

    fs::remove_all(root, ec);
}

// The watcher routes delete events through TryUnregisterAssetByPath. A
// referenced asset must tombstone (record kept, flag set) so references
// resolve to a known identity, never silently to nothing; an unreferenced
// asset is removed outright (AggressiveTombstoneCleanup) — nothing
// referenced it, so nothing dangles.
//
// WHERE the flag rests follows the mount's identity scheme, per the design's
// §5-A residence rule: "the journal holds identity and user-authored metadata
// only — guid, path, type, kv. Per-machine observations about disk —
// existence/missing, fingerprints, missing-since — live in the derived SQLite
// cache." A live delete is such an observation, so on a derived mount the
// journaled record stays missing=false and the .assetdb bytes do not move;
// the tombstone lands in the cache, exactly where the offline reconcile pass
// puts it. Supersedes this test's pre-unification assertion that the delete
// journaled the flag.
TEST(AssetDbDerivedRedirect, WatcherDeleteTombstonesReferencedAsset)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_s2_livedelete");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "target.txt", "referenced content");
    WriteTextFile(root / "sub" / "referrer.txt", "refers to target");
    WriteTextFile(root / "sub" / "orphan.txt", "nobody references this");

    PrimeDerivedMount(root);

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store && pinned->Cache);

        const GUID targetGuid = FindGuidByStorePath(*pinned, "sub/target.txt");
        const GUID referrerGuid = FindGuidByStorePath(*pinned, "sub/referrer.txt");
        const GUID orphanGuid = FindGuidByStorePath(*pinned, "sub/orphan.txt");
        ASSERT_FALSE(targetGuid.IsNull());
        ASSERT_FALSE(referrerGuid.IsNull());
        ASSERT_FALSE(orphanGuid.IsNull());

        // A live dependent forces the tombstone branch.
        ASSERT_TRUE(pinned->Cache->ReplaceDependencies(
            referrerGuid, std::vector<GUID>{targetGuid}, nullptr));

        // Referenced delete: tombstone.
        fs::remove(root / "sub" / "target.txt", ec);
        ASSERT_TRUE(reg.TryUnregisterAssetByPath(root / "sub" / "target.txt"));

        AssetRecord rec{};
        ASSERT_TRUE(pinned->Store->TryGetAsset(targetGuid, rec))
            << "referenced delete removed the record instead of tombstoning";
        EXPECT_FALSE(rec.missing)
            << "live delete wrote a per-machine disk observation into the git-shared journal";
        AssetRecord cacheRec{};
        ASSERT_TRUE(pinned->Cache->TryGetAsset(targetGuid, cacheRec))
            << "live delete left no cache row for the tombstone";
        EXPECT_TRUE(cacheRec.missing) << "tombstone is not flagged missing in the derived cache";
        EXPECT_EQ(reg.ResolveGuid(targetGuid), targetGuid) << "a delete must not redirect";
        AssetMetadata md{};
        EXPECT_FALSE(reg.TryGetAssetMetadata(targetGuid, md))
            << "deleted asset still resident in the in-memory registry";
        const auto missingList = reg.GetMissingAssets();
        EXPECT_TRUE(std::any_of(missingList.begin(), missingList.end(),
                                [&](const auto& mi) { return mi.guid == targetGuid; }))
            << "live tombstone not surfaced by GetMissingAssets";

        // Unreferenced delete: removed outright.
        fs::remove(root / "sub" / "orphan.txt", ec);
        ASSERT_TRUE(reg.TryUnregisterAssetByPath(root / "sub" / "orphan.txt"));
        EXPECT_FALSE(pinned->Store->TryGetAsset(orphanGuid, rec))
            << "unreferenced delete kept a record (AggressiveTombstoneCleanup expected removal)";
    });

    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// Live-rename residue vs the completion sweep
//
// TryRenameAssetPath leaves {record old-guid @ new-path} + redirect
// old->new-derived until the next re-registration displaces the record. The
// completion sweep must not mistake that residue for an interrupted heal:
// the redirect's target has no record, whereas a genuine interrupted heal's
// target record always precedes its redirect into any flushed journal state.
// ---------------------------------------------------------------------------

namespace
{

// Runs the shared residue choreography: prime the mount, live-rename
// sub/a.txt -> sub/b.txt inside a session, and return the original GUID with
// the live redirect's target. On return the journal holds the residue state
// {record oldGuid @ sub/b.txt} + redirect oldGuid->outLiveTarget.
void MakeLiveRenameResidue(const std::filesystem::path& root, GUID& outOldGuid, GUID& outLiveTarget)
{
    namespace fs = std::filesystem;
    WriteTextFile(root / "sub" / "a.txt", "alpha content");
    PrimeDerivedMount(root);

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        outOldGuid = reg.GetAssetGUID(root / "sub" / "a.txt");
        ASSERT_FALSE(outOldGuid.IsNull());

        std::error_code ec;
        fs::rename(root / "sub" / "a.txt", root / "sub" / "b.txt", ec);
        ASSERT_FALSE(ec) << ec.message();
        ASSERT_TRUE(reg.TryRenameAssetPath(root / "sub" / "a.txt", root / "sub" / "b.txt"));

        const auto target = pinned->Store->ResolveRedirect(outOldGuid);
        ASSERT_TRUE(target.has_value());
        outLiveTarget = *target;

        // Residue precondition: the redirect's target has no record.
        AssetRecord rec{};
        ASSERT_FALSE(pinned->Store->TryGetAsset(outLiveTarget, rec))
            << "live-rename residue no longer recordless — the sweep discriminator "
               "and these tests need re-deriving";
        ASSERT_TRUE(reg.SaveToFile({}));
    });
}

} // namespace

// Residue + offline delete of the renamed file: the sweep must NOT treat the
// residue redirect as an interrupted heal — destroying the record would make
// references to the old GUID resolve to nothing. The record survives as the
// tombstone.
TEST(AssetDbDerivedRedirect, LiveRenameResidueThenOfflineDeleteKeepsTombstone)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_s2_residue_del");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    GUID oldGuid = GUID::Null();
    GUID liveTarget = GUID::Null();
    MakeLiveRenameResidue(root, oldGuid, liveTarget);

    // Offline delete of the renamed file while the residue is still in the
    // journal.
    fs::remove(root / "sub" / "b.txt", ec);

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store && pinned->Cache);

        // The tombstone record survives the sweep.
        AssetRecord rec{};
        ASSERT_TRUE(pinned->Store->TryGetAsset(oldGuid, rec))
            << "completion sweep destroyed a tombstone whose redirect target has no record";
        AssetRecord cacheRec{};
        ASSERT_TRUE(pinned->Cache->TryGetAsset(oldGuid, cacheRec));
        EXPECT_TRUE(cacheRec.missing) << "ghost not cache-marked missing";

        // The residue redirect itself is deliberately kept: stale-redirect
        // clearing (#965) fires only when the chased target has a live
        // record, and this redirect's target is recordless — it is the only
        // forward path for old-GUID references once the file re-registers.
        const auto target = pinned->Store->ResolveRedirect(oldGuid);
        ASSERT_TRUE(target.has_value());
        EXPECT_EQ(*target, liveTarget);
        AssetRecord targetRec{};
        EXPECT_FALSE(pinned->Store->TryGetAsset(liveTarget, targetRec));
    });

    fs::remove_all(root, ec);
}

// Residue + offline re-rename of the renamed file: the sweep must leave the
// absentee for the tier matcher, which heals it to the file's real new
// identity — the redirect is overwritten old->final and the ghost record is
// removed by the heal itself.
TEST(AssetDbDerivedRedirect, LiveRenameResidueThenOfflineRenameHealsThroughResidue)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_s2_residue_ren");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    GUID oldGuid = GUID::Null();
    GUID liveTarget = GUID::Null();
    MakeLiveRenameResidue(root, oldGuid, liveTarget);

    // Offline rename of the already-live-renamed file (fs::rename keeps the
    // OS file id, so tier 1 must bind the ghost to the final path).
    fs::rename(root / "sub" / "b.txt", root / "sub" / "c.txt", ec);
    ASSERT_FALSE(ec) << ec.message();

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);

        const GUID finalGuid = FindGuidByStorePath(*pinned, "sub/c.txt");
        ASSERT_FALSE(finalGuid.IsNull());
        ASSERT_NE(finalGuid, liveTarget);

        // Healed through the residue: the redirect now points at the real
        // final identity, and the ghost record is gone.
        const auto target = pinned->Store->ResolveRedirect(oldGuid);
        ASSERT_TRUE(target.has_value())
            << "heal dropped the redirect instead of overwriting it";
        EXPECT_EQ(*target, finalGuid)
            << "sweep consumed the absentee before the matcher could heal it";
        EXPECT_EQ(reg.ResolveGuid(oldGuid), finalGuid);
        AssetRecord ghost{};
        EXPECT_FALSE(pinned->Store->TryGetAsset(oldGuid, ghost));
    });

    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// Stale-redirect clearing on re-registration (#965)
//
// A file recreated at a redirected-from path re-registers the redirect's
// source GUID; the redirect-aware lookups prefer the chased target whenever
// the target record is live, so without clearing, the recreated file is
// permanently shadowed. Clearing fires in two places: at registration commit
// (registry helper — covers scan batch, watcher, and sync registration) and
// in the derived reconcile's startup sweep (covers residue journals whose
// files are snapshot-current and never re-upsert). Both use the same
// discriminator as the completion sweep: remove only when the chain-final
// target has a live record; recordless-target redirects (live-rename
// residue, derived→stable identity) are load-bearing and kept.
// ---------------------------------------------------------------------------

// (a) The core defect: rename a→b heals to redirect A→B; a.txt is later
// recreated offline. Its registration must clear A→B — the old GUID resolves
// to the LIVE recreated file, not the rename target — and the clearing is
// journaled (persists across a reload).
TEST(AssetDbDerivedRedirect, RecreateAtRenamedAwayPathClearsRedirect)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_965_recreate");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "a.txt", "alpha content");

    GUID oldGuid = GUID::Null();
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        oldGuid = FindGuidByStorePath(*pinned, "sub/a.txt");
        ASSERT_FALSE(oldGuid.IsNull());
        ASSERT_TRUE(reg.SaveToFile({}));
    });
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        ASSERT_TRUE(pinned->Cache);
    });

    fs::rename(root / "sub" / "a.txt", root / "sub" / "b.txt", ec);
    ASSERT_FALSE(ec) << ec.message();

    GUID newGuid = GUID::Null();
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        newGuid = FindGuidByStorePath(*pinned, "sub/b.txt");
        ASSERT_FALSE(newGuid.IsNull());
        const auto target = pinned->Store->ResolveRedirect(oldGuid);
        ASSERT_TRUE(target.has_value()) << "precondition: the offline rename must heal";
        ASSERT_EQ(*target, newGuid);
    });

    // The path speaks for itself again.
    WriteTextFile(root / "sub" / "a.txt", "alpha content reborn");

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);

        EXPECT_EQ(FindGuidByStorePath(*pinned, "sub/a.txt"), oldGuid)
            << "recreated file did not re-register under its derived GUID";
        EXPECT_TRUE(pinned->Store->ResolveRedirect(oldGuid) == std::nullopt)
            << "stale redirect survived the source GUID's re-registration";
        EXPECT_EQ(reg.ResolveGuid(oldGuid), oldGuid);

        // The recreated file is NOT shadowed by the rename target.
        AssetMetadata md{};
        ASSERT_TRUE(reg.TryGetAssetMetadata(oldGuid, md));
        EXPECT_EQ(md.Guid, oldGuid)
            << "recreated file is shadowed by the rename target's record";
        EXPECT_EQ(md.Path.filename().string(), "a.txt");

        // The rename target itself is untouched.
        EXPECT_EQ(FindGuidByStorePath(*pinned, "sub/b.txt"), newGuid);
        ASSERT_TRUE(reg.TryGetAssetMetadata(newGuid, md));
        EXPECT_EQ(md.Guid, newGuid);

        // The removal is identity movement and must reach the journal.
        EXPECT_TRUE(pinned->StoreDirty.load(std::memory_order_relaxed))
            << "redirect removal did not mark the store dirty";
    });

    // Journaled, not session state.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        EXPECT_TRUE(pinned->Store->ResolveRedirect(oldGuid) == std::nullopt)
            << "stale-redirect removal did not persist across a reload";
        EXPECT_EQ(FindGuidByStorePath(*pinned, "sub/a.txt"), oldGuid);
        EXPECT_EQ(FindGuidByStorePath(*pinned, "sub/b.txt"), newGuid);
        AssetMetadata md{};
        ASSERT_TRUE(reg.TryGetAssetMetadata(oldGuid, md));
        EXPECT_EQ(md.Guid, oldGuid);
    });

    fs::remove_all(root, ec);
}

// (b) Chain semantics: c→a→b leaves C→A and A→B. Recreating a.txt removes
// A's outgoing hop and RETARGETS the incoming one — C→A becomes C→B — so the
// reborn file at a.txt never inherits references that were following a rename.
//
// The ruling this pins, verbatim: "If we identified a rename, the direction
// should remain — asset C renamed to A: references to C now reference A; A
// renamed to B: anything referencing A (including C's chain) references B. A
// new file in A's path shouldn't replace the references."
TEST(AssetDbDerivedRedirect, RedirectChainRetargetsPastResurrectedSource)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_965_chain");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "c.txt", "gamma content");

    GUID guidC = GUID::Null();
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        guidC = FindGuidByStorePath(*pinned, "sub/c.txt");
        ASSERT_FALSE(guidC.IsNull());
        ASSERT_TRUE(reg.SaveToFile({}));
    });
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        ASSERT_TRUE(pinned->Cache);
    });

    // Hop 1: c.txt -> a.txt heals C→A.
    fs::rename(root / "sub" / "c.txt", root / "sub" / "a.txt", ec);
    ASSERT_FALSE(ec) << ec.message();
    GUID guidA = GUID::Null();
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        guidA = FindGuidByStorePath(*pinned, "sub/a.txt");
        ASSERT_FALSE(guidA.IsNull());
        const auto target = pinned->Store->ResolveRedirect(guidC);
        ASSERT_TRUE(target.has_value()) << "precondition: hop 1 must heal";
        ASSERT_EQ(*target, guidA);
    });

    // Hop 2: a.txt -> b.txt heals A→B; C now resolves transitively to B.
    fs::rename(root / "sub" / "a.txt", root / "sub" / "b.txt", ec);
    ASSERT_FALSE(ec) << ec.message();
    GUID guidB = GUID::Null();
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        guidB = FindGuidByStorePath(*pinned, "sub/b.txt");
        ASSERT_FALSE(guidB.IsNull());
        const auto target = pinned->Store->ResolveRedirect(guidA);
        ASSERT_TRUE(target.has_value()) << "precondition: hop 2 must heal";
        ASSERT_EQ(*target, guidB);
        EXPECT_EQ(reg.ResolveGuid(guidC), guidB);
    });

    // Resurrect a.txt: A→B goes, C→A is retargeted to C→B.
    WriteTextFile(root / "sub" / "a.txt", "alpha reborn in the chain");
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);

        EXPECT_EQ(FindGuidByStorePath(*pinned, "sub/a.txt"), guidA);
        EXPECT_TRUE(pinned->Store->ResolveRedirect(guidA) == std::nullopt)
            << "A's stale outgoing hop survived its re-registration";
        const auto cTarget = pinned->Store->ResolveRedirect(guidC);
        ASSERT_TRUE(cTarget.has_value())
            << "removing A's hop must not remove C's incoming hop";
        EXPECT_EQ(*cTarget, guidB)
            << "C's hop was not retargeted; the reborn a.txt captured C's references";

        EXPECT_EQ(reg.ResolveGuid(guidC), guidB)
            << "the rename direction did not survive A's rebirth";
        AssetMetadata md{};
        ASSERT_TRUE(reg.TryGetAssetMetadata(guidC, md))
            << "old-GUID metadata lookup broke when the chain target came alive";
        EXPECT_EQ(md.Guid, guidB);
        EXPECT_EQ(md.Path.filename().string(), "b.txt");

        // The reborn file speaks for itself — it just does not speak for C.
        EXPECT_EQ(reg.ResolveGuid(guidA), guidA);
        ASSERT_TRUE(reg.TryGetAssetMetadata(guidA, md));
        EXPECT_EQ(md.Guid, guidA);
        EXPECT_EQ(md.Path.filename().string(), "a.txt");

        // b.txt keeps its own identity and record.
        EXPECT_EQ(FindGuidByStorePath(*pinned, "sub/b.txt"), guidB);
        ASSERT_TRUE(reg.TryGetAssetMetadata(guidB, md));
        EXPECT_EQ(md.Guid, guidB);
    });

    // The retarget is journaled, not session state.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        const auto cTarget = pinned->Store->ResolveRedirect(guidC);
        ASSERT_TRUE(cTarget.has_value()) << "C's hop vanished across a reload";
        EXPECT_EQ(*cTarget, guidB) << "the retarget did not persist across a reload";
        EXPECT_EQ(reg.ResolveGuid(guidC), guidB);
    });

    fs::remove_all(root, ec);
}

// (c) The live watcher variant: the recreation happens mid-session (the
// watcher's Created funnel is RegisterAsset), after the startup sweep already
// ran — the registration-commit helper must clear the redirect immediately.
TEST(AssetDbDerivedRedirect, LiveRecreateViaWatcherPathClearsRedirectImmediately)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_965_live");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "a.txt", "alpha content");

    GUID oldGuid = GUID::Null();
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        oldGuid = FindGuidByStorePath(*pinned, "sub/a.txt");
        ASSERT_FALSE(oldGuid.IsNull());
        ASSERT_TRUE(reg.SaveToFile({}));
    });
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        ASSERT_TRUE(pinned->Cache);
    });

    fs::rename(root / "sub" / "a.txt", root / "sub" / "b.txt", ec);
    ASSERT_FALSE(ec) << ec.message();

    GUID newGuid = GUID::Null();
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        newGuid = FindGuidByStorePath(*pinned, "sub/b.txt");
        ASSERT_FALSE(newGuid.IsNull());
        ASSERT_TRUE(pinned->Store->ResolveRedirect(oldGuid).has_value())
            << "precondition: the offline rename must heal";
    });

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        ASSERT_TRUE(pinned->Store->ResolveRedirect(oldGuid).has_value())
            << "precondition: redirect must still be present at session start";

        // Mid-session recreation through the watcher's registration funnel.
        WriteTextFile(root / "sub" / "a.txt", "alpha content live reborn");
        ASSERT_TRUE(reg.RegisterAsset(root / "sub" / "a.txt"));

        EXPECT_EQ(reg.GetAssetGUID(root / "sub" / "a.txt"), oldGuid);
        EXPECT_TRUE(pinned->Store->ResolveRedirect(oldGuid) == std::nullopt)
            << "live re-registration did not clear the stale redirect";
        EXPECT_EQ(reg.ResolveGuid(oldGuid), oldGuid);
        AssetMetadata md{};
        ASSERT_TRUE(reg.TryGetAssetMetadata(oldGuid, md));
        EXPECT_EQ(md.Guid, oldGuid)
            << "recreated file is shadowed until the next restart";
        EXPECT_EQ(md.Path.filename().string(), "a.txt");
        ASSERT_TRUE(reg.SaveToFile({}));
    });

    // Persisted.
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        EXPECT_TRUE(pinned->Store->ResolveRedirect(oldGuid) == std::nullopt);
        EXPECT_EQ(FindGuidByStorePath(*pinned, "sub/a.txt"), oldGuid);
        EXPECT_EQ(FindGuidByStorePath(*pinned, "sub/b.txt"), newGuid);
    });

    fs::remove_all(root, ec);
}

// A derived→stable identity redirect has a LIVE source and a recordless
// target by construction (the stable GUID lands in scenes; only the redirect
// knows it). Re-registering the source file must NOT clear it — the
// target-record-exists discriminator is what protects this class.
TEST(AssetDbDerivedRedirect, StableIdentityRedirectSurvivesSourceReregistration)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_965_stable");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteTextFile(root / "sub" / "zone.txt", "payload v1");

    const GUID stable = GUID::Generate();
    GUID derived = GUID::Null();
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        derived = FindGuidByStorePath(*pinned, "sub/zone.txt");
        ASSERT_FALSE(derived.IsNull());
        ASSERT_TRUE(reg.AddRedirect(derived, stable));
        ASSERT_TRUE(reg.SaveToFile({}));
    });
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned);
        ASSERT_TRUE(pinned->Cache);
    });

    // Offline edit forces a genuine re-registration of the source GUID next
    // session (snapshot-currency broken).
    WriteTextFile(root / "sub" / "zone.txt", "payload v2 — edited offline");

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);

        const auto target = pinned->Store->ResolveRedirect(derived);
        ASSERT_TRUE(target.has_value())
            << "re-registration cleared a derived→stable identity redirect "
               "(recordless target must be kept)";
        EXPECT_EQ(*target, stable);

        // The stable GUID still resolves through the reverse-redirect
        // fallback, and the source record resolves directly.
        AssetMetadata md{};
        ASSERT_TRUE(reg.TryGetAssetMetadata(stable, md))
            << "stable identity lookup broke";
        EXPECT_EQ(md.Guid, derived);
        ASSERT_TRUE(reg.TryGetAssetMetadata(derived, md));
        EXPECT_EQ(md.Guid, derived);
    });

    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// Module-level heal semantics (direct reconciler calls over a bare
// store + cache — no registry, no scan)
// ---------------------------------------------------------------------------

namespace
{

struct DirectHarness
{
    std::filesystem::path Root;
    AssetStore_TextJsonl Store;
    AssetDbCache_Sqlite Cache;

    explicit DirectHarness(const std::string& tag)
    {
        namespace fs = std::filesystem;
        Root = TestUtils::MakeUniqueTempDirectory(tag);
        std::error_code ec;
        fs::remove_all(Root, ec);
        fs::create_directories(Root, ec);
        EXPECT_TRUE(Cache.Open(Root / "AssetDbCache.sqlite"));
        EXPECT_TRUE(Cache.EnsureSchema());
    }

    ~DirectHarness()
    {
        Cache.Close();
        std::error_code ec;
        std::filesystem::remove_all(Root, ec);
    }

    bool Reconcile(const std::vector<ReconcileScanNewFile>* scanNewFiles,
                   ReconcileStats& outStats,
                   std::vector<ReconcileHeal>* outHeals,
                   const ReconcileGhostRetention& retention = {},
                   std::vector<GUID>* outCollected = nullptr)
    {
        const AssetIgnoreRules rules = AssetIgnoreRules::CreateDefault();
        return StartupReconcileAssetDatabase(Root, rules, Store, &Cache,
                                             ReconcileIdentityScheme::Derived,
                                             nullptr, nullptr, scanNewFiles, retention, outStats,
                                             outHeals, outCollected);
    }
};

void WriteDirectFile(const std::filesystem::path& p, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << p.string();
    out << text;
}

} // namespace

// kv migration is per-key copy-if-absent: keys already on the new record
// win, absent keys copy over, and re-running the heal inputs is a no-op —
// the exact re-entrancy contract of design §4-S2.
TEST(AssetDbDerivedRedirect, DirectHealMigratesKvCopyIfAbsentAndIsReentrant)
{
    DirectHarness h("ge_derived_s2_direct_kv");

    const GUID oldGuid("aaaaaaaa-1111-2222-3333-444444444444");
    const GUID newGuid("bbbbbbbb-1111-2222-3333-444444444444");

    // Old record: ghost (no file on disk). New record: real file.
    AssetRecord oldRec{};
    oldRec.guid = oldGuid;
    oldRec.path = "sub/old.txt";
    oldRec.kv = {{"shared_key", "old_value"}, {"only_old", "keepme"}};
    ASSERT_TRUE(h.Store.UpsertAsset(oldRec, nullptr));

    AssetRecord newRec{};
    newRec.guid = newGuid;
    newRec.path = "sub/new.txt";
    newRec.kv = {{"shared_key", "new_value"}};
    ASSERT_TRUE(h.Store.UpsertAsset(newRec, nullptr));
    WriteDirectFile(h.Root / "sub" / "new.txt", "content");

    // Absentee fingerprint matches the candidate on file id (tier 1).
    ASSERT_TRUE(h.Cache.UpdateFileFingerprint(oldGuid, 111, 7, "HASH", "FILEID"));
    // Provenance row that must follow the identity.
    const GUID producedGuid("cccccccc-1111-2222-3333-444444444444");
    ASSERT_TRUE(h.Cache.RegisterProvenance(producedGuid, oldGuid, "importer"));

    ReconcileScanNewFile candidate{};
    candidate.Guid = newGuid;
    candidate.CanonicalRel = "sub/new.txt";
    candidate.Size = 7;
    candidate.Mtime = 111;
    candidate.FileId = "FILEID";
    candidate.ContentHash = "HASH";
    const std::vector<ReconcileScanNewFile> candidates{candidate};

    ReconcileStats stats;
    std::vector<ReconcileHeal> heals;
    EXPECT_TRUE(h.Reconcile(&candidates, stats, &heals));

    EXPECT_EQ(stats.Redirected, 1u);
    ASSERT_EQ(heals.size(), 1u);
    EXPECT_EQ(heals[0].From, oldGuid);
    EXPECT_EQ(heals[0].To, newGuid);

    // Redirect landed; ghost removed.
    const auto target = h.Store.ResolveRedirect(oldGuid);
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(*target, newGuid);
    AssetRecord ghost{};
    EXPECT_FALSE(h.Store.TryGetAsset(oldGuid, ghost));

    // kv: existing key wins, absent key copies.
    std::string v;
    ASSERT_TRUE(h.Store.TryGetKeyValue(newGuid, "shared_key", v));
    EXPECT_EQ(v, "new_value") << "kv migration clobbered a key the new record already owned";
    ASSERT_TRUE(h.Store.TryGetKeyValue(newGuid, "only_old", v));
    EXPECT_EQ(v, "keepme") << "kv migration dropped a key only the old record had";

    // Provenance followed the identity.
    EXPECT_EQ(h.Cache.GetProducer(producedGuid), newGuid);

    // Re-running with the same inputs is a no-op: the absentee is gone.
    ReconcileStats stats2;
    std::vector<ReconcileHeal> heals2;
    (void)h.Reconcile(&candidates, stats2, &heals2);
    EXPECT_EQ(stats2.Redirected, 0u);
    EXPECT_EQ(stats2.HealsCompleted, 0u);
    EXPECT_TRUE(heals2.empty());
    ASSERT_TRUE(h.Store.TryGetKeyValue(newGuid, "shared_key", v));
    EXPECT_EQ(v, "new_value");
}

// A candidate whose fingerprint matches more than one absentee is ambiguous:
// no redirect may be emitted and both ghosts stay tombstoned.
TEST(AssetDbDerivedRedirect, DirectAmbiguousMatchEmitsNothing)
{
    DirectHarness h("ge_derived_s2_direct_ambig");

    const GUID ghostA("aaaaaaaa-1111-2222-3333-444444444444");
    const GUID ghostB("bbbbbbbb-1111-2222-3333-444444444444");
    const GUID candGuid("cccccccc-1111-2222-3333-444444444444");

    AssetRecord recA{};
    recA.guid = ghostA;
    recA.path = "sub/x1.bin";
    ASSERT_TRUE(h.Store.UpsertAsset(recA, nullptr));
    AssetRecord recB{};
    recB.guid = ghostB;
    recB.path = "sub/x2.bin";
    ASSERT_TRUE(h.Store.UpsertAsset(recB, nullptr));

    AssetRecord recC{};
    recC.guid = candGuid;
    recC.path = "sub/y.bin";
    ASSERT_TRUE(h.Store.UpsertAsset(recC, nullptr));
    WriteDirectFile(h.Root / "sub" / "y.bin", "payload");

    // Identical content fingerprints, no file ids: the hash tier sees two
    // equally good absentees.
    ASSERT_TRUE(h.Cache.UpdateFileFingerprint(ghostA, 100, 7, "SAMEHASH", ""));
    ASSERT_TRUE(h.Cache.UpdateFileFingerprint(ghostB, 100, 7, "SAMEHASH", ""));

    ReconcileScanNewFile candidate{};
    candidate.Guid = candGuid;
    candidate.CanonicalRel = "sub/y.bin";
    candidate.Size = 7;
    candidate.Mtime = 200;
    candidate.ContentHash = "SAMEHASH";
    const std::vector<ReconcileScanNewFile> candidates{candidate};

    ReconcileStats stats;
    std::vector<ReconcileHeal> heals;
    (void)h.Reconcile(&candidates, stats, &heals);

    EXPECT_EQ(stats.Ambiguous, 1u);
    EXPECT_EQ(stats.Redirected, 0u);
    EXPECT_TRUE(heals.empty());
    EXPECT_TRUE(h.Store.EnumerateRedirects().empty())
        << "an ambiguous match emitted a redirect";
    AssetRecord rec{};
    EXPECT_TRUE(h.Store.TryGetAsset(ghostA, rec));
    EXPECT_TRUE(h.Store.TryGetAsset(ghostB, rec));
}

// A heal-carrying journal flush appends {new record, redirect old->new, old
// record delete} as one buffer whose section order is a crash-prefix
// invariant: a torn append persists a PREFIX, so redirect upserts must
// serialize before asset deletes — the recoverable interruption is
// {record + redirect} (the completion sweep's input), never
// {delete without redirect} (permanent identity loss). This test pins the
// serialized order, then simulates the torn write by truncating the delete
// line and proves the prefix reloads as the sweep-completable shape.
TEST(AssetDbDerivedRedirect, DirectTornHealAppendKeepsRedirectBeforeDelete)
{
    namespace fs = std::filesystem;
    DirectHarness h("ge_derived_s2_torn");
    const fs::path dbPath = h.Root / "TornAppend.assetdb";

    const GUID baseGuid("dddddddd-1111-2222-3333-444444444444");
    const GUID oldGuid("aaaaaaaa-1111-2222-3333-444444444444");
    const GUID newGuid("bbbbbbbb-1111-2222-3333-444444444444");

    // Baseline journal (compaction write, v2): a bystander and the record
    // that will become the ghost.
    AssetRecord baseRec{};
    baseRec.guid = baseGuid;
    baseRec.path = "sub/base.txt";
    ASSERT_TRUE(h.Store.UpsertAsset(baseRec, nullptr));
    AssetRecord oldRec{};
    oldRec.guid = oldGuid;
    oldRec.path = "sub/old.txt";
    ASSERT_TRUE(h.Store.UpsertAsset(oldRec, nullptr));
    ASSERT_TRUE(h.Store.SaveToFile(dbPath, nullptr));

    // One heal-shaped delta batch, flushed as a single append buffer.
    AssetRecord newRec{};
    newRec.guid = newGuid;
    newRec.path = "sub/new.txt";
    ASSERT_TRUE(h.Store.UpsertAsset(newRec, nullptr));
    ASSERT_TRUE(h.Store.AddRedirect(oldGuid, newGuid, nullptr));
    ASSERT_TRUE(h.Store.RemoveAsset(oldGuid, nullptr));
    ASSERT_TRUE(h.Store.SaveToFile(dbPath, nullptr));

    // Read the appended tail and pin the section order.
    std::vector<std::string> lines;
    {
        std::ifstream in(dbPath, std::ios::binary);
        ASSERT_TRUE(in.is_open());
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty())
                lines.push_back(line);
        }
    }
    ASSERT_GE(lines.size(), 3u);
    const std::string& upsertLine = lines[lines.size() - 3];
    const std::string& redirectLine = lines[lines.size() - 2];
    const std::string& deleteLine = lines[lines.size() - 1];
    EXPECT_NE(upsertLine.find("sub/new.txt"), std::string::npos)
        << "appended tail does not start with the new record's upsert: " << upsertLine;
    EXPECT_NE(redirectLine.find("redirect_from"), std::string::npos)
        << "redirect upsert does not precede the asset delete: " << redirectLine;
    EXPECT_NE(deleteLine.find("\"deleted\":true"), std::string::npos)
        << "asset delete is not the final heal line: " << deleteLine;

    // Torn write: the crash prefix ends after the redirect line — drop the
    // delete line and rewrite.
    {
        std::string prefix;
        for (size_t i = 0; i + 1 < lines.size(); ++i)
        {
            prefix += lines[i];
            prefix += '\n';
        }
        std::ofstream out(dbPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << prefix;
    }

    // The prefix reloads as the interrupted-heal shape: redirect present,
    // ghost record still present (its delete was in the lost suffix).
    AssetStore_TextJsonl reloaded;
    std::string err;
    ASSERT_TRUE(reloaded.LoadFromFile(dbPath, &err)) << err;
    const auto target = reloaded.ResolveRedirect(oldGuid);
    ASSERT_TRUE(target.has_value())
        << "torn heal flush lost the redirect — old GUID resolves to nothing, permanently";
    EXPECT_EQ(*target, newGuid);
    AssetRecord rec{};
    EXPECT_TRUE(reloaded.TryGetAsset(oldGuid, rec));
    EXPECT_TRUE(reloaded.TryGetAsset(newGuid, rec));

    // And the completion sweep finishes the removal on the next reconcile.
    WriteDirectFile(h.Root / "sub" / "new.txt", "content");
    const AssetIgnoreRules rules = AssetIgnoreRules::CreateDefault();
    ReconcileStats stats;
    std::vector<ReconcileHeal> heals;
    (void)StartupReconcileAssetDatabase(h.Root, rules, reloaded, &h.Cache,
                                        ReconcileIdentityScheme::Derived,
                                        nullptr, nullptr, nullptr, ReconcileGhostRetention{}, stats,
                                        &heals, nullptr);
    EXPECT_EQ(stats.HealsCompleted, 1u);
    ASSERT_EQ(heals.size(), 1u);
    EXPECT_EQ(heals[0].From, oldGuid);
    EXPECT_EQ(heals[0].To, newGuid);
    EXPECT_FALSE(reloaded.TryGetAsset(oldGuid, rec))
        << "completion sweep did not finish the torn heal's removal";
    EXPECT_TRUE(reloaded.ResolveRedirect(oldGuid).has_value());
}

// ---------------------------------------------------------------------------
// Stale-redirect sweep, module level (#965): the startup half of the
// clearing — residue journals whose files are snapshot-current never
// re-upsert, so the reconcile removes shadowing redirects itself.
// ---------------------------------------------------------------------------

// Both records live and file-backed with a redirect between them: the
// pre-#965 permanent-shadow residue. The sweep removes the redirect and
// touches neither record.
TEST(AssetDbDerivedRedirect, DirectSweepRemovesShadowingRedirect)
{
    const GUID guidA("aaaaaaaa-1111-2222-3333-444444444444");
    const GUID guidB("bbbbbbbb-1111-2222-3333-444444444444");

    DirectHarness h("ge_derived_965_sweep");
    AssetRecord recA{};
    recA.guid = guidA;
    recA.path = "sub/a.txt";
    ASSERT_TRUE(h.Store.UpsertAsset(recA, nullptr));
    AssetRecord recB{};
    recB.guid = guidB;
    recB.path = "sub/b.txt";
    ASSERT_TRUE(h.Store.UpsertAsset(recB, nullptr));
    ASSERT_TRUE(h.Store.AddRedirect(guidA, guidB, nullptr));
    WriteDirectFile(h.Root / "sub" / "a.txt", "alpha");
    WriteDirectFile(h.Root / "sub" / "b.txt", "beta");

    ReconcileStats stats;
    EXPECT_TRUE(h.Reconcile(nullptr, stats, nullptr))
        << "redirect removal must report the store dirty";
    EXPECT_EQ(stats.StaleRedirectsRemoved, 1u);
    EXPECT_TRUE(h.Store.ResolveRedirect(guidA) == std::nullopt);
    AssetRecord rec{};
    EXPECT_TRUE(h.Store.TryGetAsset(guidA, rec));
    EXPECT_TRUE(h.Store.TryGetAsset(guidB, rec));

    // Idempotent: a second run finds nothing to remove.
    ReconcileStats stats2;
    (void)h.Reconcile(nullptr, stats2, nullptr);
    EXPECT_EQ(stats2.StaleRedirectsRemoved, 0u);
}

// Chained residue A→B→C with A and C live, B recordless: only A's own hop is
// stale (its chain-final target C has a record). B→C stays — it is the
// forward path for references to B.
TEST(AssetDbDerivedRedirect, DirectSweepRemovesOnlyTheShadowingHopOfAChain)
{
    const GUID guidA("aaaaaaaa-1111-2222-3333-444444444444");
    const GUID guidB("bbbbbbbb-1111-2222-3333-444444444444");
    const GUID guidC("cccccccc-1111-2222-3333-444444444444");

    DirectHarness h("ge_derived_965_sweepchain");
    AssetRecord recA{};
    recA.guid = guidA;
    recA.path = "sub/a.txt";
    ASSERT_TRUE(h.Store.UpsertAsset(recA, nullptr));
    AssetRecord recC{};
    recC.guid = guidC;
    recC.path = "sub/c.txt";
    ASSERT_TRUE(h.Store.UpsertAsset(recC, nullptr));
    ASSERT_TRUE(h.Store.AddRedirect(guidA, guidB, nullptr));
    ASSERT_TRUE(h.Store.AddRedirect(guidB, guidC, nullptr));
    WriteDirectFile(h.Root / "sub" / "a.txt", "alpha");
    WriteDirectFile(h.Root / "sub" / "c.txt", "gamma");

    ReconcileStats stats;
    (void)h.Reconcile(nullptr, stats, nullptr);
    EXPECT_EQ(stats.StaleRedirectsRemoved, 1u);
    EXPECT_TRUE(h.Store.ResolveRedirect(guidA) == std::nullopt)
        << "the chain-final target has a live record — A's hop shadows";
    const auto bTarget = h.Store.ResolveRedirect(guidB);
    ASSERT_TRUE(bTarget.has_value())
        << "B's hop is not stale (B has no record) and must be kept";
    EXPECT_EQ(*bTarget, guidC);
}

// The keep case: a live, file-backed source whose chased target has NO
// record (live-rename residue / derived→stable identity shape). The sweep
// must not touch it.
TEST(AssetDbDerivedRedirect, DirectSweepKeepsRecordlessTargetRedirect)
{
    const GUID guidA("aaaaaaaa-1111-2222-3333-444444444444");
    const GUID guidX("eeeeeeee-1111-2222-3333-444444444444");

    DirectHarness h("ge_derived_965_sweepkeep");
    AssetRecord recA{};
    recA.guid = guidA;
    recA.path = "sub/b.txt"; // live-rename residue shape: old GUID at new path
    ASSERT_TRUE(h.Store.UpsertAsset(recA, nullptr));
    ASSERT_TRUE(h.Store.AddRedirect(guidA, guidX, nullptr));
    WriteDirectFile(h.Root / "sub" / "b.txt", "beta");

    ReconcileStats stats;
    (void)h.Reconcile(nullptr, stats, nullptr);
    EXPECT_EQ(stats.StaleRedirectsRemoved, 0u);
    const auto target = h.Store.ResolveRedirect(guidA);
    ASSERT_TRUE(target.has_value())
        << "sweep removed a recordless-target redirect — live-rename residue "
           "and derived→stable identity redirects just lost their forward path";
    EXPECT_EQ(*target, guidX);
}

// Journal round-trip of the removal: remove-then-readd across flushes must
// replay cleanly (v2 is last-write-wins; the delete line erases the mapping
// before the re-add lands) — no load conflicts, latest target wins.
TEST(AssetDbDerivedRedirect, RemoveThenReaddRedirectReloadsWithoutConflict)
{
    namespace fs = std::filesystem;
    DirectHarness h("ge_derived_965_journal");
    const fs::path dbPath = h.Root / "RemoveReadd.assetdb";

    const GUID guidA("aaaaaaaa-1111-2222-3333-444444444444");
    const GUID guidB("bbbbbbbb-1111-2222-3333-444444444444");
    const GUID guidC("cccccccc-1111-2222-3333-444444444444");

    ASSERT_TRUE(h.Store.AddRedirect(guidA, guidB, nullptr));
    ASSERT_TRUE(h.Store.SaveToFile(dbPath, nullptr));
    ASSERT_TRUE(h.Store.RemoveRedirect(guidA, nullptr));
    ASSERT_TRUE(h.Store.SaveToFile(dbPath, nullptr)); // appends the delete line
    ASSERT_TRUE(h.Store.AddRedirect(guidA, guidC, nullptr));
    ASSERT_TRUE(h.Store.SaveToFile(dbPath, nullptr)); // appends the re-add

    AssetStore_TextJsonl reloaded;
    std::string err;
    ASSERT_TRUE(reloaded.LoadFromFile(dbPath, &err)) << err;
    EXPECT_TRUE(reloaded.GetLoadConflicts().empty())
        << "remove-then-readd across sessions produced a load conflict";
    const auto target = reloaded.ResolveRedirect(guidA);
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(*target, guidC);
}

// ---------------------------------------------------------------------------
// Retarget on rebirth, and the shared chase seam
// ---------------------------------------------------------------------------

// The multi-hop shape of the ruling: D->C->A->B, and A is reborn at its old
// path. A's outgoing hop goes; C's hop inherits A's chain-final target, so both
// D and C still reach B. D's own hop is untouched — it points at C, which is
// not a rebirth, and chasing it lands on B all the same.
TEST(AssetDbDerivedRedirect, SweepRetargetsMultiHopChainPastRebornSource)
{
    DirectHarness h("ge_966_direct_multihop");

    const GUID guidA("aaaaaaaa-9966-2222-3333-444444444444");
    const GUID guidB("bbbbbbbb-9966-2222-3333-444444444444");
    const GUID guidC("cccccccc-9966-2222-3333-444444444444");
    const GUID guidD("dddddddd-9966-2222-3333-444444444444");

    // A is reborn (record + file on disk); B is the rename target it points at.
    // C and D are older identities whose records died with their renames.
    AssetRecord recA{};
    recA.guid = guidA;
    recA.path = "sub/a.txt";
    ASSERT_TRUE(h.Store.UpsertAsset(recA, nullptr));
    WriteDirectFile(h.Root / "sub" / "a.txt", "alpha reborn");

    AssetRecord recB{};
    recB.guid = guidB;
    recB.path = "sub/b.txt";
    ASSERT_TRUE(h.Store.UpsertAsset(recB, nullptr));
    WriteDirectFile(h.Root / "sub" / "b.txt", "beta");

    ASSERT_TRUE(h.Store.AddRedirect(guidD, guidC, nullptr));
    ASSERT_TRUE(h.Store.AddRedirect(guidC, guidA, nullptr));
    ASSERT_TRUE(h.Store.AddRedirect(guidA, guidB, nullptr));

    // The reconciler returns whether it dirtied the store.
    ReconcileStats stats;
    EXPECT_TRUE(h.Reconcile(nullptr, stats, nullptr))
        << "the sweep changed redirects without marking the store dirty";
    EXPECT_EQ(stats.StaleRedirectsRemoved, 1u);

    EXPECT_TRUE(h.Store.ResolveRedirect(guidA) == std::nullopt)
        << "the reborn source kept its outgoing hop";
    const auto cTarget = h.Store.ResolveRedirect(guidC);
    ASSERT_TRUE(cTarget.has_value()) << "C's incoming hop was removed instead of retargeted";
    EXPECT_EQ(*cTarget, guidB) << "C was captured by the reborn file at A's path";
    const auto dTarget = h.Store.ResolveRedirect(guidD);
    ASSERT_TRUE(dTarget.has_value());
    EXPECT_EQ(*dTarget, guidC) << "a hop that points at a non-victim must not move";

    EXPECT_EQ(ChaseRedirectChain(h.Store, guidD, RedirectTargetCheck::None, {}).Final, guidB);
    EXPECT_EQ(ChaseRedirectChain(h.Store, guidC, RedirectTargetCheck::None, {}).Final, guidB);
    EXPECT_EQ(ChaseRedirectChain(h.Store, guidA, RedirectTargetCheck::None, {}).Final, guidA);
}

// Cascade redirects are ordinary redirects: the subasset chain retargets in
// lockstep with its container, so Derive(C, key) still reaches Derive(B, key).
TEST(AssetDbDerivedRedirect, SweepRetargetsSubassetCascadeInLockstepWithContainer)
{
    DirectHarness h("ge_966_direct_cascade");

    const GUID guidA("aaaaaaaa-9967-2222-3333-444444444444");
    const GUID guidB("bbbbbbbb-9967-2222-3333-444444444444");
    const GUID guidC("cccccccc-9967-2222-3333-444444444444");
    const std::string key = "mesh_0";

    AssetRecord recA{};
    recA.guid = guidA;
    recA.path = "sub/model.txt";
    ASSERT_TRUE(h.Store.UpsertAsset(recA, nullptr));
    WriteDirectFile(h.Root / "sub" / "model.txt", "container reborn");

    // The heal migrated the container's derive keys onto the rename target.
    AssetRecord recB{};
    recB.guid = guidB;
    recB.path = "sub/model_renamed.txt";
    recB.kv = {{kSubassetDeriveKeysKvKey, key}};
    ASSERT_TRUE(h.Store.UpsertAsset(recB, nullptr));
    WriteDirectFile(h.Root / "sub" / "model_renamed.txt", "container");

    ASSERT_TRUE(h.Store.AddRedirect(guidC, guidA, nullptr));
    ASSERT_TRUE(h.Store.AddRedirect(guidA, guidB, nullptr));
    ASSERT_TRUE(h.Store.AddRedirect(GUID::Derive(guidC, key), GUID::Derive(guidA, key), nullptr));
    ASSERT_TRUE(h.Store.AddRedirect(GUID::Derive(guidA, key), GUID::Derive(guidB, key), nullptr));

    ReconcileStats stats;
    EXPECT_TRUE(h.Reconcile(nullptr, stats, nullptr))
        << "the sweep changed redirects without marking the store dirty";
    EXPECT_EQ(stats.StaleRedirectsRemoved, 1u);
    EXPECT_EQ(stats.SubassetRedirectsRemoved, 1u);

    EXPECT_TRUE(h.Store.ResolveRedirect(guidA) == std::nullopt);
    EXPECT_TRUE(h.Store.ResolveRedirect(GUID::Derive(guidA, key)) == std::nullopt)
        << "the reborn container's own subasset identity is still shadowed";

    const auto cTarget = h.Store.ResolveRedirect(guidC);
    ASSERT_TRUE(cTarget.has_value());
    EXPECT_EQ(*cTarget, guidB);
    const auto cascadeTarget = h.Store.ResolveRedirect(GUID::Derive(guidC, key));
    ASSERT_TRUE(cascadeTarget.has_value())
        << "the cascade's incoming hop was removed instead of retargeted";
    EXPECT_EQ(*cascadeTarget, GUID::Derive(guidB, key))
        << "the subasset chain diverged from its container";
}

// A recordless chain-final target still keeps the source's hop, and with no
// removal there is nothing to retarget — the load-bearing shape is untouched.
TEST(AssetDbDerivedRedirect, RecordlessTargetLeavesIncomingHopsAlone)
{
    DirectHarness h("ge_966_direct_recordless");

    const GUID guidA("aaaaaaaa-9968-2222-3333-444444444444");
    const GUID guidX("eeeeeeee-9968-2222-3333-444444444444");
    const GUID guidC("cccccccc-9968-2222-3333-444444444444");

    AssetRecord recA{};
    recA.guid = guidA;
    recA.path = "sub/a.txt";
    ASSERT_TRUE(h.Store.UpsertAsset(recA, nullptr));
    WriteDirectFile(h.Root / "sub" / "a.txt", "alpha");

    ASSERT_TRUE(h.Store.AddRedirect(guidC, guidA, nullptr));
    ASSERT_TRUE(h.Store.AddRedirect(guidA, guidX, nullptr)); // X has no record

    ReconcileStats stats;
    EXPECT_FALSE(h.Reconcile(nullptr, stats, nullptr))
        << "the sweep dirtied the store on a run that had nothing to remove";
    EXPECT_EQ(stats.StaleRedirectsRemoved, 0u);

    const auto aTarget = h.Store.ResolveRedirect(guidA);
    ASSERT_TRUE(aTarget.has_value());
    EXPECT_EQ(*aTarget, guidX);
    const auto cTarget = h.Store.ResolveRedirect(guidC);
    ASSERT_TRUE(cTarget.has_value());
    EXPECT_EQ(*cTarget, guidA) << "an incoming hop moved without a removal to justify it";
}

// A cycle resolves to the queried GUID rather than to whichever member the
// depth cap happened to land on, and reports itself as a cycle. Two-member
// cycles already resolved this way by even-parity accident; longer ones did
// not.
TEST(AssetDbDerivedRedirect, ChaseReportsCycleAndResolvesToQueriedGuid)
{
    DirectHarness h("ge_966_direct_cycle");

    const GUID guidA("aaaaaaaa-9969-2222-3333-444444444444");
    const GUID guidB("bbbbbbbb-9969-2222-3333-444444444444");
    const GUID guidC("cccccccc-9969-2222-3333-444444444444");

    ASSERT_TRUE(h.Store.AddRedirect(guidA, guidB, nullptr));
    ASSERT_TRUE(h.Store.AddRedirect(guidB, guidA, nullptr));

    const RedirectChain two = ChaseRedirectChain(h.Store, guidA, RedirectTargetCheck::None, {});
    EXPECT_TRUE(two.CycleDetected);
    EXPECT_FALSE(two.Redirected);
    EXPECT_FALSE(two.TargetAccepted);
    EXPECT_EQ(two.Final, guidA);

    // Retarget is a no-op over a cycle: nothing can be inherited.
    const GUID twoSources[] = {guidA};
    EXPECT_EQ(RetargetIncomingRedirects(h.Store, twoSources), 0u);

    ASSERT_TRUE(h.Store.AddRedirect(guidB, guidC, nullptr));
    ASSERT_TRUE(h.Store.AddRedirect(guidC, guidA, nullptr));

    const RedirectChain three = ChaseRedirectChain(h.Store, guidA, RedirectTargetCheck::None, {});
    EXPECT_TRUE(three.CycleDetected);
    EXPECT_EQ(three.Final, guidA);
    const RedirectChain fromB = ChaseRedirectChain(h.Store, guidB, RedirectTargetCheck::None, {});
    EXPECT_TRUE(fromB.CycleDetected);
    EXPECT_EQ(fromB.Final, guidB) << "a cycle must resolve to the GUID that was queried";

    const GUID threeSources[] = {guidA};
    EXPECT_EQ(RetargetIncomingRedirects(h.Store, threeSources), 0u);
}

// The depth cap is a truncation, not a cycle: a chain longer than the cap
// resolves to the deepest GUID reached and reports no cycle.
TEST(AssetDbDerivedRedirect, ChaseTruncatesOverlongChainWithoutReportingACycle)
{
    DirectHarness h("ge_966_direct_depthcap");

    std::vector<GUID> links;
    for (int i = 0; i <= kMaxRedirectChainDepth + 1; ++i)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%08d-9970-2222-3333-444444444444", i);
        links.emplace_back(std::string(buf));
    }
    for (size_t i = 0; i + 1 < links.size(); ++i)
        ASSERT_TRUE(h.Store.AddRedirect(links[i], links[i + 1], nullptr));

    const RedirectChain chain =
        ChaseRedirectChain(h.Store, links.front(), RedirectTargetCheck::None, {});
    EXPECT_FALSE(chain.CycleDetected);
    EXPECT_TRUE(chain.Redirected);
    EXPECT_EQ(chain.Final, links[kMaxRedirectChainDepth]);
}

// Journal round-trip of a retarget: the same source gains a new target with no
// intervening delete line. v2 replay is last-write-wins by design, so this must
// reload as the new target with no load conflict reported.
TEST(AssetDbDerivedRedirect, RetargetAppendReloadsWithoutConflict)
{
    namespace fs = std::filesystem;
    DirectHarness h("ge_966_direct_retarget_journal");
    const fs::path dbPath = h.Root / "Retarget.assetdb";

    const GUID guidA("aaaaaaaa-9971-2222-3333-444444444444");
    const GUID guidB("bbbbbbbb-9971-2222-3333-444444444444");
    const GUID guidC("cccccccc-9971-2222-3333-444444444444");

    ASSERT_TRUE(h.Store.AddRedirect(guidC, guidA, nullptr));
    ASSERT_TRUE(h.Store.SaveToFile(dbPath, nullptr));
    ASSERT_TRUE(h.Store.AddRedirect(guidC, guidB, nullptr)); // the retarget
    ASSERT_TRUE(h.Store.SaveToFile(dbPath, nullptr));

    AssetStore_TextJsonl reloaded;
    std::string err;
    ASSERT_TRUE(reloaded.LoadFromFile(dbPath, &err)) << err;
    EXPECT_TRUE(reloaded.GetLoadConflicts().empty())
        << "a retarget append was reported as a redirect conflict";
    const auto target = reloaded.ResolveRedirect(guidC);
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(*target, guidB);
}
