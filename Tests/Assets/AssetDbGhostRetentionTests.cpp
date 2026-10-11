// S3 of the derived-identity reconcile: ghost retention and garbage
// collection.
//
// A ghost is a record whose file is gone. Keeping one is what lets a dangling
// reference resolve to a known-missing identity rather than to nothing, so
// collecting one is never free — it is traded against the cost of carrying it:
// a standing ghost is exempt from the dir-mtime gate by construction, so it
// buys one exists() syscall per session forever, and it sits in every
// missing-assets surface permanently. Issue #989 measured that bill on a real
// project (1303 drift ghosts, none of them ever healable because no cache
// fingerprinted the vanished files).
//
// The mechanism here is deliberately inert by default: nothing is collected
// until a mount opts in. What the tests pin is that the opt-in is safe —
// heals win over collection, referenced ghosts survive any policy, and a
// ghost's age is measured rather than assumed.

#include <gtest/gtest.h>

#include "AssetCore/AssetIgnoreRules.h"
#include "AssetDatabase/AssetDatabasePaths.h"
#include "AssetDatabase/AssetDbCache_Sqlite.h"
#include "AssetDatabase/AssetStoreReconciler.h"
#include "AssetDatabase/AssetStore_TextJsonl.h"
#include "AssetDatabase/IAssetDbCache.h"
#include "AssetDatabase/IAssetStore.h"
#include "Assets/AssetRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::AssetDatabase;

namespace
{

void WriteGhostTestFile(const std::filesystem::path& p, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << p.string();
    out << text;
}

std::string ReadGhostTestFile(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open())
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A bare store + cache over a temp tree, driving the reconciler directly.
// Sessions are explicit: NextSession() closes and reopens the cache, which is
// what spends a tick of the per-machine session counter that ghost age is
// measured in.
struct RetentionHarness
{
    std::filesystem::path Root;
    std::filesystem::path CachePath;
    AssetStore_TextJsonl Store{nullptr};
    AssetDbCache_Sqlite Cache;

    explicit RetentionHarness(const std::string& tag)
    {
        namespace fs = std::filesystem;
        Root = TestUtils::MakeUniqueTempDirectory(tag);
        std::error_code ec;
        fs::remove_all(Root, ec);
        fs::create_directories(Root, ec);
        CachePath = Root / "AssetDbCache.sqlite";
        EXPECT_TRUE(Cache.Open(CachePath));
        EXPECT_TRUE(Cache.EnsureSchema());
    }

    ~RetentionHarness()
    {
        Cache.Close();
        std::error_code ec;
        std::filesystem::remove_all(Root, ec);
    }

    void NextSession()
    {
        Cache.Close();
        ASSERT_TRUE(Cache.Open(CachePath));
        ASSERT_TRUE(Cache.EnsureSchema());
    }

    bool Reconcile(ReconcileStats& outStats,
                   const ReconcileGhostRetention& retention = {},
                   std::vector<GUID>* outCollected = nullptr,
                   const std::vector<ReconcileScanNewFile>* scanNewFiles = nullptr)
    {
        const AssetIgnoreRules rules = AssetIgnoreRules::CreateDefault();
        return StartupReconcileAssetDatabase(Root, rules, Store, &Cache,
                                             ReconcileIdentityScheme::Derived, nullptr, nullptr,
                                             scanNewFiles, retention, outStats, nullptr,
                                             outCollected);
    }

    // A record the scan measured: store record, cache row, fingerprint. This
    // ghost CAN still rename-heal once its file vanishes.
    GUID AddFingerprintedRecord(const std::string& rel, const std::string& contents)
    {
        WriteGhostTestFile(Root / rel, contents);
        const GUID guid = GUID::Generate();
        AssetRecord rec{};
        rec.guid = guid;
        rec.path = rel;
        rec.type = AssetType::Unknown;
        EXPECT_TRUE(Store.UpsertAsset(rec, nullptr));
        EXPECT_TRUE(Cache.UpsertAsset(rec, nullptr));
        EXPECT_TRUE(Cache.UpdateFileFingerprint(guid, 1234, static_cast<int64_t>(contents.size()),
                                                "hash-" + rel, "fileid-" + rel, nullptr));
        return guid;
    }

    // The #989 shape: a journal record whose file the cache never measured.
    // No fingerprint column is set, so no matcher tier can ever index it.
    GUID AddUnfingerprintedRecord(const std::string& rel, const std::string& contents)
    {
        WriteGhostTestFile(Root / rel, contents);
        const GUID guid = GUID::Generate();
        AssetRecord rec{};
        rec.guid = guid;
        rec.path = rel;
        rec.type = AssetType::Unknown;
        EXPECT_TRUE(Store.UpsertAsset(rec, nullptr));
        EXPECT_TRUE(Cache.UpsertAsset(rec, nullptr));
        return guid;
    }

    void DeleteFile(const std::string& rel)
    {
        std::error_code ec;
        std::filesystem::remove(Root / rel, ec);
        ASSERT_FALSE(std::filesystem::exists(Root / rel, ec));
    }

    bool HasStoreRecord(const GUID& guid) const
    {
        AssetRecord rec{};
        return Store.TryGetAsset(guid, rec);
    }

    bool IsCacheGhost(const GUID& guid) const
    {
        AssetRecord rec{};
        return Cache.TryGetAsset(guid, rec) && rec.missing;
    }

    std::optional<IAssetDbCache::GhostRow> FindGhostRow(const GUID& guid) const
    {
        for (const IAssetDbCache::GhostRow& row : Cache.EnumerateGhostRows())
        {
            if (row.Guid == guid)
                return row;
        }
        return std::nullopt;
    }
};

AssetSourceDesc MakeRetentionProjectDesc(const std::filesystem::path& root,
                                         uint32_t retentionSessions,
                                         bool collectNeverHealable)
{
    const AssetDatabasePaths dbPaths = GetDefaultPathsForAssetRoot(root, {}, {});
    AssetSourceDesc desc{};
    desc.Alias = "project";
    desc.Root = root;
    desc.DerivedIdentity = true;
    desc.AuthoritativeDbFile = dbPaths.authoritativeFile;
    desc.CacheRoot = dbPaths.cacheRoot;
    desc.Priority = 100;
    desc.GhostRetentionSessions = retentionSessions;
    desc.CollectNeverHealableGhosts = collectNeverHealable;
    return desc;
}

template <typename Body>
void RunRetentionSession(const std::filesystem::path& root,
                         uint32_t retentionSessions,
                         bool collectNeverHealable,
                         Body&& body)
{
    JobSystem::WorkStealingThreadPool pool(2);
    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(&pool));
    ASSERT_TRUE(reg.RegisterSource(MakeRetentionProjectDesc(root, retentionSessions,
                                                            collectNeverHealable)));
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

// ---------------------------------------------------------------------------
// #1007: the journal→cache mirror carries identity, never observations
// ---------------------------------------------------------------------------

// The two writers differ in what they are entitled to say. UpsertAsset is for
// a caller that just looked at the file, so it writes `missing` and a
// registration uses it to clear a ghost flag. MirrorJournalRecord is for a
// caller reading the git-shared journal, which knows nothing about this
// machine's disk — under derived identity a ghost's journal flag is false by
// design, so a mirror that wrote it would leave the ghost flagged nowhere.
TEST(AssetDbGhostRetention, MirrorKeepsTheCacheGhostFlagThatAnUpsertClears)
{
    RetentionHarness h("ge_s3_mirror_contract");
    const GUID ghost = h.AddFingerprintedRecord("sub/gone.txt", "content");
    h.DeleteFile("sub/gone.txt");

    ReconcileStats stats;
    (void)h.Reconcile(stats);
    ASSERT_TRUE(h.IsCacheGhost(ghost)) << "the fixture never produced a ghost";
    const auto stamped = h.FindGhostRow(ghost);
    ASSERT_TRUE(stamped.has_value());

    // What the journal hands the mirror: identity, and missing=false.
    AssetRecord journalRec{};
    ASSERT_TRUE(h.Store.TryGetAsset(ghost, journalRec));
    ASSERT_FALSE(journalRec.missing) << "the derived journal flagged the ghost, so this proves nothing";

    ASSERT_TRUE(h.Cache.MirrorJournalRecord(journalRec, nullptr));
    EXPECT_TRUE(h.IsCacheGhost(ghost)) << "the mirror answered a question it had not asked the disk";
    const auto afterMirror = h.FindGhostRow(ghost);
    ASSERT_TRUE(afterMirror.has_value()) << "the mirror dropped the ghost out of the ghost set";
    EXPECT_EQ(afterMirror->MissingSinceSession, stamped->MissingSinceSession)
        << "the mirror restarted the ghost's retention clock";
    EXPECT_TRUE(afterMirror->HasFingerprint) << "the mirror erased the ghost's healability";

    // Contrast: the same record through UpsertAsset does clear it, which is
    // exactly why registration uses that one.
    ASSERT_TRUE(h.Cache.UpsertAsset(journalRec, nullptr));
    EXPECT_FALSE(h.IsCacheGhost(ghost))
        << "UpsertAsset stopped writing `missing`, so a heal can no longer clear a ghost flag";
}

// ---------------------------------------------------------------------------
// The default: nothing is ever collected
// ---------------------------------------------------------------------------

// The mechanism is inert until a mount opts in. A ghost that would qualify on
// every policy dimension — old, and unhealable by construction — survives an
// unarmed mount indefinitely.
TEST(AssetDbGhostRetention, UnarmedPolicyCollectsNothing)
{
    RetentionHarness h("ge_s3_unarmed");
    const GUID ghost = h.AddUnfingerprintedRecord("sub/gone.txt", "content");
    h.DeleteFile("sub/gone.txt");

    for (int session = 0; session < 8; ++session)
    {
        ReconcileStats stats;
        (void)h.Reconcile(stats, ReconcileGhostRetention{});
        EXPECT_EQ(stats.GhostsCollected, 0u) << "session " << session;
        EXPECT_EQ(stats.GhostsStamped, 0u)
            << "an unarmed pass wrote a retention stamp (session " << session << ")";
        h.NextSession();
    }

    EXPECT_TRUE(h.HasStoreRecord(ghost)) << "unarmed retention removed a record";
    EXPECT_TRUE(h.IsCacheGhost(ghost)) << "unarmed retention cleared the tombstone";
}

// ---------------------------------------------------------------------------
// Never-healable: the #989 population
// ---------------------------------------------------------------------------

// A ghost with no cache fingerprint enters no matcher tier — not even the
// advisory dir+ext one, whose index is built behind the same
// TryGetFileFingerprint guard as the byte-comparing tiers. Waiting cannot
// change its outcome, so CollectNeverHealable takes it without an age
// threshold. The fingerprinted ghost beside it is the positive control: it
// could still heal, so it is kept.
TEST(AssetDbGhostRetention, NeverHealableGhostIsCollectedWhileHealableOneSurvives)
{
    RetentionHarness h("ge_s3_neverhealable");
    const GUID unhealable = h.AddUnfingerprintedRecord("sub/unmeasured.txt", "alpha");
    const GUID healable = h.AddFingerprintedRecord("sub/measured.txt", "bravo");
    h.DeleteFile("sub/unmeasured.txt");
    h.DeleteFile("sub/measured.txt");

    ReconcileGhostRetention retention{};
    retention.CollectNeverHealable = true;

    ReconcileStats stats;
    std::vector<GUID> collected;
    (void)h.Reconcile(stats, retention, &collected);

    EXPECT_EQ(stats.MissingObserved, 2u);
    EXPECT_EQ(stats.GhostsCollected, 1u);
    ASSERT_EQ(collected.size(), 1u);
    EXPECT_EQ(collected[0], unhealable);

    EXPECT_FALSE(h.HasStoreRecord(unhealable)) << "unhealable ghost's record survived collection";
    EXPECT_FALSE(h.IsCacheGhost(unhealable)) << "unhealable ghost's cache row survived collection";

    EXPECT_TRUE(h.HasStoreRecord(healable))
        << "a ghost the matcher could still bind was collected on the never-healable rule";
    EXPECT_TRUE(h.IsCacheGhost(healable));
}

// The classification is the cache's own fingerprint-validity rule, so a ghost
// carrying only ONE fingerprint column still counts as healable — the size+ext
// tier binds on exactly that much.
TEST(AssetDbGhostRetention, PartialFingerprintCountsAsHealable)
{
    RetentionHarness h("ge_s3_partialfp");
    const GUID guid = h.AddUnfingerprintedRecord("sub/sized.bin", "payload");
    // Size only: no hash, no file id, no mtime.
    ASSERT_TRUE(h.Cache.UpdateFileFingerprint(guid, 0, 7, std::string(), std::string(), nullptr));
    h.DeleteFile("sub/sized.bin");

    ReconcileGhostRetention retention{};
    retention.CollectNeverHealable = true;

    ReconcileStats stats;
    (void)h.Reconcile(stats, retention);
    EXPECT_EQ(stats.GhostsCollected, 0u);
    EXPECT_TRUE(h.HasStoreRecord(guid));

    const auto row = h.FindGhostRow(guid);
    ASSERT_TRUE(row.has_value());
    EXPECT_TRUE(row->HasFingerprint)
        << "a row with a fingerprint column read as never-healable — the SQL predicate has "
           "drifted from TryGetFileFingerprint's validity rule";
}

// ---------------------------------------------------------------------------
// Age
// ---------------------------------------------------------------------------

// Age is counted in sessions from the one that first observed the record
// missing, and the ghost survives every session below the threshold.
TEST(AssetDbGhostRetention, AgedGhostIsCollectedOnlyAtTheThreshold)
{
    RetentionHarness h("ge_s3_aged");
    const GUID ghost = h.AddFingerprintedRecord("sub/old.txt", "content");
    h.DeleteFile("sub/old.txt");

    ReconcileGhostRetention retention{};
    retention.MaxMissingSessions = 3;

    // Session that observes the delete: the stamp lands, age is 0.
    {
        ReconcileStats stats;
        (void)h.Reconcile(stats, retention);
        EXPECT_EQ(stats.GhostsCollected, 0u) << "a ghost was collected the session it appeared";
    }

    // Two more sessions: ages 1 and 2, both under the threshold.
    for (int i = 0; i < 2; ++i)
    {
        h.NextSession();
        ReconcileStats stats;
        (void)h.Reconcile(stats, retention);
        EXPECT_EQ(stats.GhostsCollected, 0u) << "collected at age " << (i + 1) << " of 3";
        EXPECT_TRUE(h.HasStoreRecord(ghost));
    }

    // Age 3 meets the threshold.
    h.NextSession();
    ReconcileStats stats;
    (void)h.Reconcile(stats, retention);
    EXPECT_EQ(stats.GhostsCollected, 1u) << "ghost was not collected once it reached the threshold";
    EXPECT_FALSE(h.HasStoreRecord(ghost));
}

// The stamp is written ONCE, at the false->true transition. Re-stamping a
// standing ghost every session was the rejected shape: it is N ghosts x M
// sessions of write traffic AND it would pin every ghost's age at zero
// forever, so the threshold could never be reached.
TEST(AssetDbGhostRetention, MissingSinceIsStampedOnceNotEverySession)
{
    RetentionHarness h("ge_s3_stamponce");
    const GUID ghost = h.AddFingerprintedRecord("sub/old.txt", "content");
    h.DeleteFile("sub/old.txt");

    ReconcileGhostRetention retention{};
    retention.MaxMissingSessions = 100; // High enough that nothing is collected.

    ReconcileStats first;
    (void)h.Reconcile(first, retention);
    const auto stamped = h.FindGhostRow(ghost);
    ASSERT_TRUE(stamped.has_value());
    const int64_t originalStamp = stamped->MissingSinceSession;
    EXPECT_GT(originalStamp, 0) << "the transition left the ghost unstamped";

    for (int i = 0; i < 5; ++i)
    {
        h.NextSession();
        ReconcileStats stats;
        (void)h.Reconcile(stats, retention);
        EXPECT_EQ(stats.GhostsStamped, 0u)
            << "a standing ghost was re-stamped on session " << i;
        const auto row = h.FindGhostRow(ghost);
        ASSERT_TRUE(row.has_value());
        EXPECT_EQ(row->MissingSinceSession, originalStamp)
            << "the ghost's age was reset on session " << i;
    }

    // The session counter really did advance, so the assertion above is not
    // passing because nothing moved.
    EXPECT_GT(h.Cache.GetSessionCounter(), originalStamp);
}

// A ghost that predates retention has no stamp. Its age is unknown, and
// inventing one would collect a record this machine never actually watched sit
// missing — so it is stamped on sight and starts at zero.
TEST(AssetDbGhostRetention, UnstampedGhostStartsItsClockOnSightRatherThanAgingOut)
{
    RetentionHarness h("ge_s3_backfill");
    const GUID ghost = h.AddFingerprintedRecord("sub/legacy.txt", "content");
    h.DeleteFile("sub/legacy.txt");

    // Mark it the way a pre-retention session did: flagged, no stamp.
    AssetRecord rec{};
    ASSERT_TRUE(h.Store.TryGetAsset(ghost, rec));
    ASSERT_TRUE(h.Cache.SetAssetMissing(rec, true, /*missingSinceSession*/ 0, nullptr));
    {
        const auto row = h.FindGhostRow(ghost);
        ASSERT_TRUE(row.has_value());
        ASSERT_EQ(row->MissingSinceSession, 0) << "harness did not produce an unstamped ghost";
    }

    ReconcileGhostRetention retention{};
    retention.MaxMissingSessions = 1;

    // First armed session: stamped, not collected, even though "session 0"
    // read literally would make it older than any threshold.
    ReconcileStats stats;
    (void)h.Reconcile(stats, retention);
    EXPECT_EQ(stats.GhostsCollected, 0u) << "an unstamped ghost was treated as infinitely old";
    EXPECT_EQ(stats.GhostsStamped, 1u);
    EXPECT_TRUE(h.HasStoreRecord(ghost));

    // And from there it ages normally.
    h.NextSession();
    ReconcileStats later;
    (void)h.Reconcile(later, retention);
    EXPECT_EQ(later.GhostsCollected, 1u) << "a stamped ghost never aged out";
}

// ---------------------------------------------------------------------------
// The invariants that hold whatever the policy says
// ---------------------------------------------------------------------------

// Her acceptance requirement is that a deleted asset's references resolve to
// its tombstone rather than to nothing. Anything still pointing at a ghost
// keeps it alive regardless of age or healability — the same predicate the
// live-delete path uses to choose tombstone over removal.
TEST(AssetDbGhostRetention, ReferencedGhostsAreRetainedUnderAnyPolicy)
{
    RetentionHarness h("ge_s3_referenced");
    const GUID guidReferrer = h.AddFingerprintedRecord("sub/referrer.txt", "refers");
    const GUID byGuid = h.AddUnfingerprintedRecord("sub/by-guid.txt", "a");
    const GUID byPath = h.AddUnfingerprintedRecord("sub/by-path.txt", "b");
    const GUID producer = h.AddUnfingerprintedRecord("sub/producer.txt", "c");
    const GUID collectible = h.AddUnfingerprintedRecord("sub/nobody-cares.txt", "d");

    ASSERT_TRUE(h.Cache.ReplaceDependencies(guidReferrer, std::vector<GUID>{byGuid}, nullptr));

    DepEdge pathEdge{};
    pathEdge.Referrer = GUID::Generate();
    pathEdge.TargetPath = "sub/by-path.txt";
    ASSERT_TRUE(h.Cache.ReplaceDependencies(pathEdge.Referrer, std::vector<DepEdge>{pathEdge},
                                            nullptr));

    ASSERT_TRUE(h.Cache.RegisterProvenance(GUID::Generate(), producer, "TestImporter", nullptr));

    for (const char* rel : {"sub/by-guid.txt", "sub/by-path.txt", "sub/producer.txt",
                            "sub/nobody-cares.txt"})
    {
        h.DeleteFile(rel);
    }

    ReconcileGhostRetention retention{};
    retention.CollectNeverHealable = true;
    retention.MaxMissingSessions = 1;

    ReconcileStats stats;
    (void)h.Reconcile(stats, retention);

    EXPECT_TRUE(h.HasStoreRecord(byGuid)) << "a ghost with a GUID-form referrer was collected";
    EXPECT_TRUE(h.HasStoreRecord(byPath)) << "a ghost with a path-form referrer was collected";
    EXPECT_TRUE(h.HasStoreRecord(producer)) << "a ghost with a produced asset was collected";
    EXPECT_EQ(stats.GhostsRetained, 3u);

    // Negative control: an identical ghost with nothing pointing at it IS
    // collected, so the three assertions above are about the references and
    // not about the policy failing to fire.
    EXPECT_FALSE(h.HasStoreRecord(collectible))
        << "the unreferenced ghost was not collected — the policy never fired";
    EXPECT_EQ(stats.GhostsCollected, 1u);
}

// An outstanding rename suggestion is a question waiting on the user.
// Collecting its subject would answer it by destroying it.
TEST(AssetDbGhostRetention, SuggestedGhostIsRetained)
{
    RetentionHarness h("ge_s3_suggested");
    const GUID ghost = h.AddFingerprintedRecord("sub/maybe-renamed.txt", "content");
    h.DeleteFile("sub/maybe-renamed.txt");

    ReconcileGhostRetention retention{};
    retention.MaxMissingSessions = 1;

    ReconcileStats first;
    (void)h.Reconcile(first, retention);
    ASSERT_TRUE(h.HasStoreRecord(ghost));

    IAssetDbCache::RenameSuggestion suggestion{};
    suggestion.MissingGuid = ghost;
    suggestion.MissingPath = "sub/maybe-renamed.txt";
    suggestion.CandidateGuid = GUID::Generate();
    suggestion.CandidatePath = "sub/maybe-the-same-file.txt";
    suggestion.EvidenceTier = kRenameEvidenceTierDirExt;
    ASSERT_TRUE(h.Cache.RecordRenameSuggestion(suggestion, nullptr));

    h.NextSession();
    ReconcileStats stats;
    (void)h.Reconcile(stats, retention);

    EXPECT_EQ(stats.GhostsCollected, 0u) << "collecting the subject answered the rename question";
    EXPECT_EQ(stats.GhostsRetained, 1u);
    EXPECT_TRUE(h.HasStoreRecord(ghost));
    EXPECT_EQ(h.Cache.EnumerateRenameSuggestions().size(), 1u)
        << "the suggestion was purged while its subject is still missing";
}

// Ordering: the matcher runs before retention, so a ghost old enough to
// collect still heals when this session's scan brings its rename target. A
// collected ghost is only ever one whose chance to heal has been taken.
TEST(AssetDbGhostRetention, HealTakesPrecedenceOverCollection)
{
    RetentionHarness h("ge_s3_healfirst");
    const GUID ghost = h.AddFingerprintedRecord("sub/before.txt", "content");
    h.DeleteFile("sub/before.txt");

    ReconcileGhostRetention retention{};
    retention.MaxMissingSessions = 1;

    ReconcileStats first;
    (void)h.Reconcile(first, retention);
    ASSERT_TRUE(h.HasStoreRecord(ghost));

    // Next session the file reappears under a new name — the shape a rename
    // takes under derived identity, where a new path is a new GUID.
    h.NextSession();
    WriteGhostTestFile(h.Root / "sub" / "after.txt", "content");
    const GUID newGuid = GUID::Generate();
    AssetRecord newRec{};
    newRec.guid = newGuid;
    newRec.path = "sub/after.txt";
    ASSERT_TRUE(h.Store.UpsertAsset(newRec, nullptr));
    ASSERT_TRUE(h.Cache.UpsertAsset(newRec, nullptr));

    ReconcileScanNewFile candidate{};
    candidate.Guid = newGuid;
    candidate.CanonicalRel = "sub/after.txt";
    candidate.Size = 7;
    candidate.Mtime = 1234;
    candidate.FileId = "fileid-sub/before.txt"; // The OS identity the ghost carried.
    candidate.ContentHash = "hash-sub/before.txt";
    const std::vector<ReconcileScanNewFile> candidates{candidate};

    ReconcileStats stats;
    std::vector<GUID> collected;
    (void)h.Reconcile(stats, retention, &collected, &candidates);

    EXPECT_EQ(stats.Redirected, 1u) << "an aged ghost was not offered to the matcher";
    EXPECT_EQ(stats.GhostsCollected, 0u) << "retention consumed a ghost that healed this session";
    EXPECT_TRUE(collected.empty());
    const auto target = h.Store.ResolveRedirect(ghost);
    ASSERT_TRUE(target.has_value()) << "the heal emitted no redirect";
    EXPECT_EQ(*target, newGuid);
}

// ---------------------------------------------------------------------------
// Suggestion hygiene (the reconcile-time purge)
// ---------------------------------------------------------------------------

// A suggestion names a missing record. Nothing validated that claim after the
// fact: the re-clear path only fires for a record the pass watched come back,
// and RemoveAsset only fires when the record is destroyed. A row whose subject
// is simply not missing this run had no removal path at all.
TEST(AssetDbGhostRetention, SuggestionWhoseSubjectIsNotMissingIsPurged)
{
    RetentionHarness h("ge_s3_suggestionpurge");
    const GUID present = h.AddFingerprintedRecord("sub/present.txt", "still here");
    const GUID stillGone = h.AddFingerprintedRecord("sub/gone.txt", "vanished");
    h.DeleteFile("sub/gone.txt");

    // Two rows: one about a record that is NOT missing (stale by construction —
    // this run never observes it absent, so no other path can reach it), one
    // about a genuine standing absentee.
    for (const auto& [guid, path] :
         std::vector<std::pair<GUID, std::string>>{{present, "sub/present.txt"},
                                                   {stillGone, "sub/gone.txt"}})
    {
        IAssetDbCache::RenameSuggestion s{};
        s.MissingGuid = guid;
        s.MissingPath = path;
        s.CandidateGuid = GUID::Generate();
        s.CandidatePath = "sub/candidate.txt";
        s.EvidenceTier = kRenameEvidenceTierDirExt;
        ASSERT_TRUE(h.Cache.RecordRenameSuggestion(s, nullptr));
    }
    ASSERT_EQ(h.Cache.EnumerateRenameSuggestions().size(), 2u);

    ReconcileStats stats;
    (void)h.Reconcile(stats);

    EXPECT_EQ(stats.StaleSuggestionsPurged, 1u);
    const auto remaining = h.Cache.EnumerateRenameSuggestions();
    ASSERT_EQ(remaining.size(), 1u) << "the purge took the suggestion about a live absentee";
    EXPECT_EQ(remaining[0].MissingGuid, stillGone);
}

// The run that finds nothing missing is exactly the run where every standing
// suggestion has lost its subject, so the purge must run ahead of the
// absentee fast-path rather than behind it.
TEST(AssetDbGhostRetention, SuggestionsArePurgedEvenWhenNothingIsMissing)
{
    RetentionHarness h("ge_s3_purgenoabsentees");
    const GUID present = h.AddFingerprintedRecord("sub/present.txt", "still here");

    IAssetDbCache::RenameSuggestion s{};
    s.MissingGuid = present;
    s.MissingPath = "sub/present.txt";
    s.CandidateGuid = GUID::Generate();
    s.CandidatePath = "sub/candidate.txt";
    s.EvidenceTier = kRenameEvidenceTierDirExt;
    ASSERT_TRUE(h.Cache.RecordRenameSuggestion(s, nullptr));

    ReconcileStats stats;
    (void)h.Reconcile(stats);

    EXPECT_EQ(stats.MissingObserved, 0u) << "the fixture produced an absentee it should not have";
    EXPECT_EQ(stats.StaleSuggestionsPurged, 1u);
    EXPECT_TRUE(h.Cache.EnumerateRenameSuggestions().empty());
}

// ---------------------------------------------------------------------------
// Registry level: residence, surfacing, and what collection leaves behind
// ---------------------------------------------------------------------------

// §5-A's observable, for the live-delete path this slice unified: "an
// unchanged warm session leaves the .assetdb byte-identical". Deleting a
// referenced file on a derived mount is a per-machine disk observation, so it
// must not move the git-shared journal either — the tombstone lands in the
// cache and the record keeps its identity fields untouched.
TEST(AssetDbGhostRetention, LiveDeleteOnDerivedMountLeavesTheJournalByteIdentical)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_s3_livedelete_journal");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteGhostTestFile(root / "sub" / "target.txt", "referenced content");
    WriteGhostTestFile(root / "sub" / "referrer.txt", "refers to target");

    RunRetentionSession(root, 0, false, [&](AssetRegistry& reg) { ASSERT_TRUE(reg.SaveToFile({})); });
    RunRetentionSession(root, 0, false, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Cache);
    });

    const fs::path dbFile = root / "AssetDatabase.assetdb";
    ASSERT_TRUE(fs::exists(dbFile, ec));

    RunRetentionSession(root, 0, false, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store && pinned->Cache);

        const std::string journalBefore = ReadGhostTestFile(dbFile);
        ASSERT_FALSE(journalBefore.empty());

        const GUID targetGuid = FindGuidByStorePath(*pinned, "sub/target.txt");
        const GUID referrerGuid = FindGuidByStorePath(*pinned, "sub/referrer.txt");
        ASSERT_FALSE(targetGuid.IsNull());
        ASSERT_FALSE(referrerGuid.IsNull());

        // A live dependent forces the tombstone branch rather than outright
        // removal (which legitimately journals a delete record).
        ASSERT_TRUE(pinned->Cache->ReplaceDependencies(referrerGuid,
                                                       std::vector<GUID>{targetGuid}, nullptr));

        fs::remove(root / "sub" / "target.txt", ec);
        ASSERT_TRUE(reg.TryUnregisterAssetByPath(root / "sub" / "target.txt"));

        AssetRecord storeRec{};
        ASSERT_TRUE(pinned->Store->TryGetAsset(targetGuid, storeRec))
            << "the tombstone lost its identity record";
        EXPECT_FALSE(storeRec.missing)
            << "the live delete journaled a per-machine disk observation";

        AssetRecord cacheRec{};
        ASSERT_TRUE(pinned->Cache->TryGetAsset(targetGuid, cacheRec));
        EXPECT_TRUE(cacheRec.missing) << "the tombstone did not land in the cache";

        reg.TickPersistence();
        EXPECT_EQ(ReadGhostTestFile(dbFile), journalBefore)
            << "a delete on a derived mount rewrote the git-shared .assetdb";
    });

    fs::remove_all(root, ec);
}

// End to end through the registry: an aged ghost is collected by the mount's
// policy, disappears from the missing-assets surface it was previously
// surfaced on, and leaves no resident entry behind.
TEST(AssetDbGhostRetention, CollectedGhostLeavesTheRegistryAndTheMissingList)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_s3_registry_collect");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteGhostTestFile(root / "sub" / "doomed.txt", "content");
    WriteGhostTestFile(root / "sub" / "keep.txt", "keeper");

    // Prime with retention DISARMED so the ghost is established first.
    RunRetentionSession(root, 0, false, [&](AssetRegistry& reg) { ASSERT_TRUE(reg.SaveToFile({})); });
    RunRetentionSession(root, 0, false, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Cache);
    });

    fs::remove(root / "sub" / "doomed.txt", ec);

    GUID ghostGuid = GUID::Null();
    RunRetentionSession(root, 0, false, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        ghostGuid = FindGuidByStorePath(*pinned, "sub/doomed.txt");
        ASSERT_FALSE(ghostGuid.IsNull()) << "the ghost record vanished before the test began";

        const auto missingList = reg.GetMissingAssets();
        EXPECT_TRUE(std::any_of(missingList.begin(), missingList.end(),
                                [&](const auto& mi) { return mi.guid == ghostGuid; }))
            << "the ghost was never surfaced, so its disappearance proves nothing";
    });

    // Now arm the mount. The ghost is one session old.
    RunRetentionSession(root, 1, false, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store && pinned->Cache);

        AssetRecord rec{};
        EXPECT_FALSE(pinned->Store->TryGetAsset(ghostGuid, rec))
            << "the aged ghost's record survived collection";
        EXPECT_FALSE(pinned->Cache->TryGetAsset(ghostGuid, rec))
            << "the aged ghost's cache row survived collection";

        const auto missingList = reg.GetMissingAssets();
        EXPECT_FALSE(std::any_of(missingList.begin(), missingList.end(),
                                 [&](const auto& mi) { return mi.guid == ghostGuid; }))
            << "a collected ghost is still surfaced as missing";

        AssetMetadata md{};
        EXPECT_FALSE(reg.TryGetAssetMetadata(ghostGuid, md))
            << "a collected ghost is still resident in the registry";

        // Positive control: the surviving file is untouched by all of this.
        const GUID keepGuid = FindGuidByStorePath(*pinned, "sub/keep.txt");
        ASSERT_FALSE(keepGuid.IsNull()) << "retention collected a live record";
        EXPECT_TRUE(reg.TryGetAssetMetadata(keepGuid, md));
    });

    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// Cache deletion is documented as ALWAYS safe. Two files carry derived state.
// ---------------------------------------------------------------------------

namespace
{

std::filesystem::path CacheSqlitePathFor(const std::filesystem::path& root)
{
    return GetDefaultPathsForAssetRoot(root, {}, {}).sqliteCacheFile;
}

std::filesystem::path SnapshotPathFor(const std::filesystem::path& root)
{
    return CacheSqlitePathFor(root).parent_path() / "watcher.snapshot.bin";
}

} // namespace

// Deleting ONLY AssetDbCache.sqlite left watcher.snapshot.bin behind, and the
// snapshot does not self-invalidate: its load checks the mount root and the
// ignore-rules signature, nothing about cache contents. The scan then skipped
// every snapshot-current file, and a skipped file is never fingerprinted, so
// the fresh cache could never refill. A rebuilt schema must take the snapshot
// with it.
TEST(AssetDbGhostRetention, WipingOnlyTheCacheAlsoDiscardsTheStaleSnapshot)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_s3_cachewipe_snapshot");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteGhostTestFile(root / "sub" / "kept.txt", "content that must be re-measured");

    RunRetentionSession(root, 0, false, [&](AssetRegistry& reg) { ASSERT_TRUE(reg.SaveToFile({})); });
    RunRetentionSession(root, 0, false, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Cache);
    });

    const fs::path cacheFile = CacheSqlitePathFor(root);
    const fs::path snapshotFile = SnapshotPathFor(root);
    ASSERT_TRUE(fs::exists(cacheFile, ec)) << cacheFile.string();
    ASSERT_TRUE(fs::exists(snapshotFile, ec))
        << "fixture never produced a snapshot, so this test would prove nothing";

    GUID keptGuid = GUID::Null();
    RunRetentionSession(root, 0, false, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store && pinned->Cache);
        keptGuid = FindGuidByStorePath(*pinned, "sub/kept.txt");
        ASSERT_FALSE(keptGuid.IsNull());
        IAssetDbCache::FileFingerprint fp{};
        ASSERT_TRUE(pinned->Cache->TryGetFileFingerprint(keptGuid, fp))
            << "fixture never fingerprinted the file, so its loss would prove nothing";
    });

    // The documented always-safe act: delete the cache. Only the cache.
    ASSERT_TRUE(fs::remove(cacheFile, ec));
    ASSERT_TRUE(fs::exists(snapshotFile, ec)) << "the test did not reproduce the stranded snapshot";

    RunRetentionSession(root, 0, false, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store && pinned->Cache);

        const GUID guid = FindGuidByStorePath(*pinned, "sub/kept.txt");
        ASSERT_EQ(guid, keptGuid) << "identity moved across a cache wipe";

        EXPECT_TRUE(pinned->SnapshotByPath.empty())
            << "the rebuilt cache still loaded the stale snapshot, so this session skipped files "
               "instead of re-measuring them";

        IAssetDbCache::FileFingerprint fp{};
        EXPECT_TRUE(pinned->Cache->TryGetFileFingerprint(guid, fp))
            << "the fresh cache never re-measured the file: the stale snapshot told the scan to "
               "skip it, so deleting the cache permanently destroyed rename-healing for this mount";
    });

    // Shutdown legitimately writes a NEW snapshot, so the file exists again.
    // What must not survive is the STALE one, which the in-session assertion
    // above is the honest check for.
    EXPECT_TRUE(fs::exists(snapshotFile, ec));

    fs::remove_all(root, ec);
}

// The retention pass must not act on a cache it just built. Fingerprints
// repopulate only for files the scan processes, and dependency edges are built
// LAZILY on first query -- no scan path writes them -- so on a rebuilt cache
// every ghost reads never-healable with zero dependents. Collecting there would
// delete the tombstone of a genuinely referenced asset whose references simply
// had not been queried yet.
TEST(AssetDbGhostRetention, SessionThatRebuiltItsCacheCollectsNothing)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_s3_coldcache_noCollect");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteGhostTestFile(root / "sub" / "referenced.txt", "something points at me");
    WriteGhostTestFile(root / "sub" / "orphan.txt", "nobody points at me");
    WriteGhostTestFile(root / "sub" / "referrer.txt", "placeholder");

    RunRetentionSession(root, 0, false, [&](AssetRegistry& reg) { ASSERT_TRUE(reg.SaveToFile({})); });
    RunRetentionSession(root, 0, false, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Cache);
    });

    GUID referencedGuid = GUID::Null();
    GUID orphanGuid = GUID::Null();
    RunRetentionSession(root, 0, false, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store && pinned->Cache);
        referencedGuid = FindGuidByStorePath(*pinned, "sub/referenced.txt");
        orphanGuid = FindGuidByStorePath(*pinned, "sub/orphan.txt");
        const GUID referrerGuid = FindGuidByStorePath(*pinned, "sub/referrer.txt");
        ASSERT_FALSE(referencedGuid.IsNull());
        ASSERT_FALSE(orphanGuid.IsNull());
        ASSERT_FALSE(referrerGuid.IsNull());
        ASSERT_TRUE(pinned->Cache->ReplaceDependencies(referrerGuid,
                                                       std::vector<GUID>{referencedGuid}, nullptr));
    });

    // Both files go away offline; both become ghosts.
    fs::remove(root / "sub" / "referenced.txt", ec);
    fs::remove(root / "sub" / "orphan.txt", ec);

    RunRetentionSession(root, 0, false, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Cache);
        AssetRecord rec{};
        ASSERT_TRUE(pinned->Cache->TryGetAsset(referencedGuid, rec) && rec.missing);
        ASSERT_TRUE(pinned->Cache->TryGetAsset(orphanGuid, rec) && rec.missing);
    });

    // Wipe only the cache, then open with the most aggressive policy.
    ASSERT_TRUE(fs::remove(CacheSqlitePathFor(root), ec));

    RunRetentionSession(root, 1, true, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);

        AssetRecord rec{};
        EXPECT_TRUE(pinned->Store->TryGetAsset(referencedGuid, rec))
            << "a referenced ghost was collected on evidence a cache wipe had just erased";
        EXPECT_TRUE(pinned->Store->TryGetAsset(orphanGuid, rec))
            << "the rebuilt-cache session collected at all";
    });

    // Negative control: the guard is scoped to the rebuilt session, so the very
    // next session collects the unreferenced ghost — the assertions above are
    // about cold evidence, not about retention being broken.
    //
    // Nothing touches the tree between the two sessions, which is the point
    // (#1007). The mount mirrors the journal into the cache, and a derived
    // ghost's journal flag is false; a mirror that wrote it would clear the
    // cache tombstone that IS the ghost, and the dir-mtime gate — which only
    // re-checks records believed present — would hide it from the existence
    // loop for good. The ghost has to survive the mount on its own.
    RunRetentionSession(root, 1, true, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);

        // The gate has to be armed, or the existence loop would re-mark the
        // ghost and this would pass without the mount preserving anything.
        ASSERT_TRUE(pinned->SnapshotDirMtimeByPath.contains("sub"))
            << "no dir-mtime gate over 'sub', so the ghost survives by re-marking, not by "
               "surviving the mount";

        AssetRecord rec{};
        EXPECT_FALSE(pinned->Store->TryGetAsset(orphanGuid, rec))
            << "retention never resumed after the cache was rebuilt";
    });

    fs::remove_all(root, ec);
}
