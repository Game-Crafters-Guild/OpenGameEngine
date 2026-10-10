// The .assetdb file is an append-only journal with compaction. These tests pin
// the properties that keep it from being rewritten or growing without bound
// across editor sessions: identical upserts must not append, a loaded journal
// must carry its existing debt toward the compaction threshold, the wall-clock
// trigger must be armed by a load rather than only by a compaction the process
// performed itself, the registry's register fan-ins must not dirty-mark the
// store when a re-registration changes nothing, and a save with nothing to
// write must leave the file the store read alone unless that file loaded with
// conflicts.

#include <gtest/gtest.h>

#include "Assets/AssetRegistry.h"
#include "AssetDatabase/AssetDatabasePaths.h"
#include "AssetDatabase/AssetStore_TextJsonl.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::AssetDatabase;

namespace
{
// Record lines only — comments and the {format,version} header are structural.
size_t CountRecordLines(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open())
        return 0;
    size_t n = 0;
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        if (line.find("\"format\"") != std::string::npos)
            continue;
        ++n;
    }
    return n;
}

AssetRecord MakeRecord(const char* guidText, const std::string& path)
{
    AssetRecord r{};
    r.guid = GUID(guidText);
    r.path = path;
    r.type = AssetType::Scene;
    r.typeId = "Scene";
    return r;
}

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

// One asset row as the store writes it.
std::string AssetLine(const char* guidText, const std::string& path, const std::string& kvJson = {})
{
    std::string line = R"({"guid":")" + GUID(guidText).ToString() + R"(","path":")" + path +
                       R"(","type":"Scene")";
    if (!kvJson.empty())
        line += R"(,"kv":)" + kvJson;
    return line + "}";
}

std::string JoinLines(const std::vector<std::string>& lines)
{
    std::string text;
    for (const std::string& line : lines)
        text += line + "\n";
    return text;
}

// The editor's project mount: identity derived from the path, the .assetdb
// kept beside the assets.
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

// One editor session: mount, let the startup scan finish, run `body`, close.
// Closing flushes a dirty store.
template <typename Body>
void RunEditorSession(const std::filesystem::path& root, Body&& body)
{
    JobSystem::WorkStealingThreadPool pool(2);
    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(&pool));
    ASSERT_TRUE(reg.RegisterSource(MakeDerivedProjectDesc(root)));
    reg.WaitForStartupScan();
    body(reg);
    reg.Shutdown();
}

constexpr const char* kFormatLine = R"({"format":"assetdb","version":2})";
constexpr const char* kGuidA = "85dc98d2-9d86-4a50-b183-109a0b59c2f8";
constexpr const char* kGuidB = "17709c6e-89d6-4fca-81ba-efb6a856e81e";
constexpr const char* kGuidC = "3f8a1c22-5b4d-4e6f-9a01-7c2d8e5f1b39";
} // namespace

// Re-registering an unchanged asset is the common case every project scan hits.
// If it dirties the record, each flush appends a byte-identical line forever.
TEST(AssetDbJournalGrowth, IdenticalUpsertsDoNotGrowTheJournal)
{
    namespace fs = std::filesystem;
    const fs::path dir = TestUtils::MakeUniqueTempDirectory("ge_assetdb_journal_noop");
    const fs::path dbFile = dir / "AssetDatabase.assetdb";

    AssetStore_TextJsonl store;
    const AssetRecord rec = MakeRecord(kGuidA, "scenes/shadowstress.scene");

    ASSERT_TRUE(store.UpsertAsset(rec, nullptr));
    ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
    const size_t afterFirst = CountRecordLines(dbFile);
    EXPECT_EQ(afterFirst, 1u);

    // Three more flushes of the identical record must add nothing.
    for (int i = 0; i < 3; ++i)
    {
        ASSERT_TRUE(store.UpsertAsset(rec, nullptr));
        ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
    }
    EXPECT_EQ(CountRecordLines(dbFile), afterFirst)
        << "identical upserts appended redundant journal lines";

    // Positive control: a genuinely changed record MUST still be journaled, so
    // the assertion above cannot be satisfied by suppressing every write.
    AssetRecord moved = rec;
    moved.path = "scenes/shadowstress_moved.scene";
    ASSERT_TRUE(store.UpsertAsset(moved, nullptr));
    ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
    EXPECT_GT(CountRecordLines(dbFile), afterFirst) << "a real change was not journaled";
}

// Same contract for MarkMissing: re-asserting the flag's current value is the
// common case for a per-startup reconcile pass and must not dirty the record.
TEST(AssetDbJournalGrowth, MarkMissingNoOpDoesNotGrowTheJournal)
{
    namespace fs = std::filesystem;
    const fs::path dir = TestUtils::MakeUniqueTempDirectory("ge_assetdb_journal_missnoop");
    const fs::path dbFile = dir / "AssetDatabase.assetdb";

    AssetStore_TextJsonl store;
    ASSERT_TRUE(store.UpsertAsset(MakeRecord(kGuidA, "scenes/ghost.scene"), nullptr));
    ASSERT_TRUE(store.MarkMissing(GUID(kGuidA), true, nullptr));
    ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
    const size_t afterFirst = CountRecordLines(dbFile);

    for (int i = 0; i < 3; ++i)
    {
        ASSERT_TRUE(store.MarkMissing(GUID(kGuidA), true, nullptr));
        ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
    }
    EXPECT_EQ(CountRecordLines(dbFile), afterFirst)
        << "re-marking an already-missing record appended redundant journal lines";

    // Positive control: a real flag transition MUST still be journaled.
    ASSERT_TRUE(store.MarkMissing(GUID(kGuidA), false, nullptr));
    ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
    EXPECT_GT(CountRecordLines(dbFile), afterFirst) << "a real flag transition was not journaled";
}

// Same contract for SetKeyValue: rewriting an identical key/value pair must
// not dirty the record.
TEST(AssetDbJournalGrowth, SetKeyValueNoOpDoesNotGrowTheJournal)
{
    namespace fs = std::filesystem;
    const fs::path dir = TestUtils::MakeUniqueTempDirectory("ge_assetdb_journal_kvnoop");
    const fs::path dbFile = dir / "AssetDatabase.assetdb";

    AssetStore_TextJsonl store;
    ASSERT_TRUE(store.UpsertAsset(MakeRecord(kGuidA, "scenes/kv.scene"), nullptr));
    ASSERT_TRUE(store.SetKeyValue(GUID(kGuidA), "shader_stage", "vertex", nullptr));
    ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
    const size_t afterFirst = CountRecordLines(dbFile);

    for (int i = 0; i < 3; ++i)
    {
        ASSERT_TRUE(store.SetKeyValue(GUID(kGuidA), "shader_stage", "vertex", nullptr));
        ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
    }
    EXPECT_EQ(CountRecordLines(dbFile), afterFirst)
        << "rewriting an identical key/value appended redundant journal lines";

    // Positive control: a real value change MUST still be journaled.
    ASSERT_TRUE(store.SetKeyValue(GUID(kGuidA), "shader_stage", "fragment", nullptr));
    ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
    EXPECT_GT(CountRecordLines(dbFile), afterFirst) << "a real value change was not journaled";
}

// Same contract for AddRedirect: re-emitting an existing redirect must not
// dirty the store.
TEST(AssetDbJournalGrowth, AddRedirectNoOpDoesNotGrowTheJournal)
{
    namespace fs = std::filesystem;
    const fs::path dir = TestUtils::MakeUniqueTempDirectory("ge_assetdb_journal_redirnoop");
    const fs::path dbFile = dir / "AssetDatabase.assetdb";

    AssetStore_TextJsonl store;
    ASSERT_TRUE(store.AddRedirect(GUID(kGuidA), GUID(kGuidB), nullptr));
    ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
    const size_t afterFirst = CountRecordLines(dbFile);
    EXPECT_EQ(afterFirst, 1u);

    for (int i = 0; i < 3; ++i)
    {
        ASSERT_TRUE(store.AddRedirect(GUID(kGuidA), GUID(kGuidB), nullptr));
        ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
    }
    EXPECT_EQ(CountRecordLines(dbFile), afterFirst)
        << "re-adding an identical redirect appended redundant journal lines";

    // Positive control: retargeting the redirect MUST still be journaled.
    ASSERT_TRUE(store.AddRedirect(GUID(kGuidA), GUID(kGuidC), nullptr));
    ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
    EXPECT_GT(CountRecordLines(dbFile), afterFirst) << "a redirect retarget was not journaled";
}

// A journal's redundancy is on disk, not in the process that opens it. If a load
// resets the append counter, short sessions can never reach the threshold and
// the file grows forever.
TEST(AssetDbJournalGrowth, ALoadedJournalCarriesItsDebtAndCompacts)
{
    namespace fs = std::filesystem;
    const fs::path dir = TestUtils::MakeUniqueTempDirectory("ge_assetdb_journal_debt");
    const fs::path dbFile = dir / "AssetDatabase.assetdb";
    std::error_code ec;
    fs::create_directories(dir, ec);

    // A v2 journal holding one live asset written 50 times — what many sessions
    // of unchanged rescans leave behind.
    constexpr size_t kRedundantLines = 50;
    {
        std::ofstream out(dbFile, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << "# GameEngine AssetDatabase.assetdb (authoritative)\n";
        out << R"({"format":"assetdb","version":2})" << "\n";
        for (size_t i = 0; i < kRedundantLines; ++i)
        {
            out << R"({"guid":")" << kGuidA
                << R"(","path":"scenes/shadowstress.scene","type":"Scene"})" << "\n";
        }
    }
    ASSERT_EQ(CountRecordLines(dbFile), kRedundantLines);

    AssetStore_TextJsonl store;
    ASSERT_TRUE(store.LoadFromFile(dbFile, nullptr));
    ASSERT_EQ(store.CountAssets(), 1u);

    // Debt is what compaction would reclaim: 50 replayed lines collapsing to 1
    // live record. A load that resets this to zero can never reach the threshold.
    EXPECT_EQ(store.GetAppendsSinceCompactionForTesting(), kRedundantLines - 1)
        << "load discarded the journal's existing debt";

    // With the debt carried, a threshold below it must force the next flush to
    // compact rather than append.
    store.SetCompactionThresholdForTesting(kRedundantLines / 2);
    ASSERT_TRUE(store.UpsertAsset(MakeRecord(kGuidB, "scenes/other.scene"), nullptr));
    ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));

    EXPECT_EQ(CountRecordLines(dbFile), 2u)
        << "flush appended to a journal already past the compaction threshold";
    EXPECT_EQ(store.GetAppendsSinceCompactionForTesting(), 0u);
}

// The wall-clock trigger is guarded on a non-zero last-compaction time. If only
// a compaction sets it, a process that exclusively appends never arms it and the
// age trigger is dead for that session's whole lifetime.
TEST(AssetDbJournalGrowth, LoadArmsTheWallClockCompactionTrigger)
{
    namespace fs = std::filesystem;
    const fs::path dir = TestUtils::MakeUniqueTempDirectory("ge_assetdb_journal_age");
    const fs::path dbFile = dir / "AssetDatabase.assetdb";
    std::error_code ec;
    fs::create_directories(dir, ec);

    {
        std::ofstream out(dbFile, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << R"({"format":"assetdb","version":2})" << "\n";
        for (int i = 0; i < 4; ++i)
        {
            out << R"({"guid":")" << kGuidA
                << R"(","path":"scenes/shadowstress.scene","type":"Scene"})" << "\n";
        }
    }

    AssetStore_TextJsonl store;
    ASSERT_TRUE(store.LoadFromFile(dbFile, nullptr));

    // Any elapsed time counts as past the max age, and the threshold is left
    // high so age is the only trigger that can fire.
    store.SetCompactionMaxAgeForTesting(std::chrono::steady_clock::duration::zero());
    store.SetCompactionThresholdForTesting(100000u);

    ASSERT_TRUE(store.UpsertAsset(MakeRecord(kGuidB, "scenes/other.scene"), nullptr));
    ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));

    EXPECT_EQ(CountRecordLines(dbFile), 2u)
        << "age trigger never fired: a load-only session leaves it disarmed";
}

// A store that loads its file and changes nothing has nothing to write. The
// registry marks its store dirty on paths that change nothing, so a save that
// rewrote the file anyway turned an editor session in which no asset changed
// into a rewrite of the tracked .assetdb: the comment block re-added, the rows
// an earlier session appended re-sorted, hundreds of lines in the diff. A store
// several processes share (an engine package's authoring manifest) re-reads its
// file under the cross-process lock and leaves it alone the same way.
TEST(AssetDbJournalGrowth, ASaveWithNothingToWriteLeavesTheFileAlone)
{
    namespace fs = std::filesystem;
    const fs::path dir = TestUtils::MakeUniqueTempDirectory("ge_assetdb_save_nothing");
    const fs::path dbFile = dir / "AssetDatabase.assetdb";

    // A journal as sessions leave it: rows in the order they were appended,
    // which is not GUID order.
    ASSERT_TRUE(GUID(kGuidB) < GUID(kGuidC) && GUID(kGuidC) < GUID(kGuidA));
    const std::string journal = JoinLines({kFormatLine, AssetLine(kGuidA, "scenes/a.scene"),
                                           AssetLine(kGuidC, "scenes/c.scene"),
                                           AssetLine(kGuidB, "scenes/b.scene")});

    for (const bool shared : {false, true})
    {
        SCOPED_TRACE(shared ? "a store shared across processes" : "a store one process owns");
        WriteTextFile(dbFile, journal);

        AssetStore_TextJsonl store;
        if (shared)
            store.SetCrossProcessWriteLockFile(dbFile.string() + ".lock");
        ASSERT_TRUE(store.LoadFromFile(dbFile, nullptr));
        ASSERT_EQ(store.CountAssets(), 3u);

        // What a rescan does: re-register a row exactly as it stands.
        ASSERT_TRUE(store.UpsertAsset(MakeRecord(kGuidA, "scenes/a.scene"), nullptr));
        ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
        EXPECT_EQ(ReadTextFile(dbFile), journal) << "a save with nothing to write rewrote the file";

        // Positive control: a real change is still written, as one appended line.
        ASSERT_TRUE(store.SetKeyValue(GUID(kGuidB), "shader_stage", "vertex", nullptr));
        ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
        EXPECT_EQ(ReadTextFile(dbFile),
                  journal + JoinLines({AssetLine(kGuidB, "scenes/b.scene", R"({"shader_stage":"vertex"})")}))
            << "a real change was not appended to the journal as it stood";
    }
}

// Only the file the store read or wrote is known to hold its rows. A save to
// any other file (AssetRegistry::SaveToFile takes a path), or to the store's
// own file after something deleted it, emptied it or put a v1 file in its
// place, still writes every row there.
TEST(AssetDbJournalGrowth, ASaveWithNothingToWriteSkipsOnlyTheFileTheStoreRead)
{
    namespace fs = std::filesystem;
    const fs::path dir = TestUtils::MakeUniqueTempDirectory("ge_assetdb_save_nothing_elsewhere");
    const fs::path dbFile = dir / "AssetDatabase.assetdb";
    const fs::path otherFile = dir / "Other.assetdb";
    WriteTextFile(dbFile, JoinLines({kFormatLine, AssetLine(kGuidA, "scenes/a.scene"),
                                     AssetLine(kGuidB, "scenes/b.scene")}));
    WriteTextFile(otherFile, JoinLines({kFormatLine, AssetLine(kGuidC, "scenes/c.scene")}));

    {
        AssetStore_TextJsonl store;
        ASSERT_TRUE(store.LoadFromFile(dbFile, nullptr));
        ASSERT_TRUE(store.SaveToFile(otherFile, nullptr));
    }
    AssetStore_TextJsonl other;
    ASSERT_TRUE(other.LoadFromFile(otherFile, nullptr));
    AssetRecord row{};
    EXPECT_EQ(other.CountAssets(), 2u) << "a save to another existing file left that file as it was";
    EXPECT_TRUE(other.TryGetAsset(GUID(kGuidA), row));
    EXPECT_FALSE(other.TryGetAsset(GUID(kGuidC), row));

    {
        AssetStore_TextJsonl store;
        ASSERT_TRUE(store.LoadFromFile(dbFile, nullptr));
        std::error_code ec;
        ASSERT_TRUE(fs::remove(dbFile, ec));
        ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
    }
    AssetStore_TextJsonl recreated;
    ASSERT_TRUE(recreated.LoadFromFile(dbFile, nullptr));
    EXPECT_EQ(recreated.CountAssets(), 2u) << "a save after the file was deleted did not write it again";

    const std::string v1Rows = JoinLines({AssetLine(kGuidA, "scenes/a.scene"), AssetLine(kGuidB, "scenes/b.scene")});
    for (const std::string& replacement : {std::string(), v1Rows})
    {
        SCOPED_TRACE(replacement.empty() ? "the file was emptied" : "the file was replaced by a v1 file");
        {
            AssetStore_TextJsonl store;
            ASSERT_TRUE(store.LoadFromFile(dbFile, nullptr));
            WriteTextFile(dbFile, replacement);
            ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
        }
        EXPECT_NE(ReadTextFile(dbFile).find(kFormatLine), std::string::npos)
            << "the save left the file without its v2 header";
        AssetStore_TextJsonl rewritten;
        ASSERT_TRUE(rewritten.LoadFromFile(dbFile, nullptr));
        EXPECT_EQ(rewritten.CountAssets(), 2u) << "the save did not write the file whole";
    }
}

// A save to another file drains the rows that were pending, so the store's own
// file no longer holds every row the store has. The next save to it, with
// nothing left dirty, writes it whole, whether the other file took the rows as
// an append or as a compaction.
TEST(AssetDbJournalGrowth, RowsSavedToAnotherFileStillReachTheStoresOwnFile)
{
    namespace fs = std::filesystem;
    const fs::path dir = TestUtils::MakeUniqueTempDirectory("ge_assetdb_rows_saved_elsewhere");
    const fs::path dbFile = dir / "AssetDatabase.assetdb";
    const fs::path otherFile = dir / "Other.assetdb";

    for (const bool compactOnEveryWrite : {false, true})
    {
        SCOPED_TRACE(compactOnEveryWrite ? "the other file took the rows as a compaction"
                                         : "the other file took the rows as an append");
        WriteTextFile(dbFile, JoinLines({kFormatLine, AssetLine(kGuidA, "scenes/a.scene"),
                                         AssetLine(kGuidB, "scenes/b.scene")}));
        WriteTextFile(otherFile, JoinLines({kFormatLine, AssetLine(kGuidC, "scenes/c.scene")}));
        {
            AssetStore_TextJsonl store;
            ASSERT_TRUE(store.LoadFromFile(dbFile, nullptr));
            if (compactOnEveryWrite)
                store.SetCompactionThresholdForTesting(1u);
            ASSERT_TRUE(store.SetKeyValue(GUID(kGuidB), "shader_stage", "vertex", nullptr));
            ASSERT_TRUE(store.SaveToFile(otherFile, nullptr));
            ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
        }
        AssetStore_TextJsonl reread;
        ASSERT_TRUE(reread.LoadFromFile(dbFile, nullptr));
        std::string stage;
        EXPECT_TRUE(reread.TryGetKeyValue(GUID(kGuidB), "shader_stage", stage) && stage == "vertex")
            << "the store's own file never got the row the other file took";
    }
}

// Git leaves conflict markers in a .assetdb that two branches appended rows to
// (nothing merges the file for git), and the load reports each marker as a
// conflict. Only a rewrite removes them, so a save with nothing to write still
// writes that file whole, once: the clean file is then left alone again. A
// shared store judges the file it re-reads under the lock, which may have
// gained the markers after the store's own load.
TEST(AssetDbJournalGrowth, ASaveWithNothingToWriteRewritesAFileThatLoadedWithConflicts)
{
    namespace fs = std::filesystem;
    const fs::path dir = TestUtils::MakeUniqueTempDirectory("ge_assetdb_save_nothing_conflicts");
    const fs::path dbFile = dir / "AssetDatabase.assetdb";
    const std::string conflicted = JoinLines({kFormatLine, AssetLine(kGuidA, "scenes/a.scene"), "<<<<<<< HEAD",
                                              AssetLine(kGuidC, "scenes/c.scene"), "=======",
                                              AssetLine(kGuidB, "scenes/b.scene"), ">>>>>>> feature"});

    struct Case
    {
        const char* name;
        bool shared;
        bool markersAfterLoad;
    };
    for (const Case& c : {Case{"a store one process owns", false, false},
                          Case{"a store shared across processes", true, false},
                          Case{"a shared store whose file gained the markers after its load", true, true}})
    {
        SCOPED_TRACE(c.name);
        WriteTextFile(dbFile, c.markersAfterLoad ? JoinLines({kFormatLine, AssetLine(kGuidA, "scenes/a.scene"),
                                                              AssetLine(kGuidC, "scenes/c.scene"),
                                                              AssetLine(kGuidB, "scenes/b.scene")})
                                                 : conflicted);
        AssetStore_TextJsonl store;
        if (c.shared)
            store.SetCrossProcessWriteLockFile(dbFile.string() + ".lock");
        ASSERT_TRUE(store.LoadFromFile(dbFile, nullptr));
        ASSERT_EQ(store.HasLoadConflicts(), !c.markersAfterLoad);
        if (c.markersAfterLoad)
            WriteTextFile(dbFile, conflicted);

        ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
        const std::string rewritten = ReadTextFile(dbFile);
        EXPECT_EQ(rewritten.find("<<<<<<<"), std::string::npos) << "the save left the conflict markers in the file";
        AssetStore_TextJsonl reread;
        ASSERT_TRUE(reread.LoadFromFile(dbFile, nullptr));
        EXPECT_FALSE(reread.HasLoadConflicts());
        EXPECT_EQ(reread.CountAssets(), 3u);

        // The rewritten file holds every row and nothing else: a real change is
        // appended to it, and a save with nothing to write leaves it alone.
        ASSERT_TRUE(store.SetKeyValue(GUID(kGuidB), "shader_stage", "vertex", nullptr));
        ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
        const std::string appended = ReadTextFile(dbFile);
        ASSERT_EQ(appended,
                  rewritten + JoinLines({AssetLine(kGuidB, "scenes/b.scene", R"({"shader_stage":"vertex"})")}));
        ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
        EXPECT_EQ(ReadTextFile(dbFile), appended) << "the file was rewritten again after its conflicts were gone";
    }
}

// The editor's mount, opened again with nothing changed. The .assetdb a session
// leaves holds the rows it appended after the rows the file already had, and
// that is the file a project commits. Re-applying a setting an asset already
// has (an inspector re-picking the current value) marks the store dirty, and
// the save that closing then runs rewrote the whole file in GUID order.
TEST(AssetDbJournalGrowth, ReopeningAnUnchangedProjectLeavesItsDatabaseByteIdentical)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_assetdb_reopen_unchanged");
    std::error_code ec;
    fs::create_directories(root, ec);
    WriteTextFile(root / "a.txt", "A");
    WriteTextFile(root / "b.txt", "B");
    const fs::path dbFile = GetDefaultPathsForAssetRoot(root, {}, {}).authoritativeFile;

    // The first session writes the file; the second appends a new asset's row
    // and a setting, which is what the project then commits.
    RunEditorSession(root, [](AssetRegistry& reg) { ASSERT_TRUE(reg.SaveToFile({})); });
    WriteTextFile(root / "c.txt", "C");
    RunEditorSession(root, [&](AssetRegistry& reg) {
        ASSERT_TRUE(reg.SetMetaValue(root / "a.txt", "tags", "terrain"));
    });
    const std::string committed = ReadTextFile(dbFile);
    ASSERT_NE(committed.find("terrain"), std::string::npos) << "the second session did not save its setting";

    RunEditorSession(root, [&](AssetRegistry& reg) {
        ASSERT_TRUE(reg.SetMetaValue(root / "a.txt", "tags", "terrain"));
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        // The precondition the test is about: closing saves this store.
        ASSERT_TRUE(pinned->StoreDirty.load(std::memory_order_relaxed));
    });
    EXPECT_EQ(ReadTextFile(dbFile), committed) << "reopening an unchanged project rewrote its .assetdb";

    fs::remove_all(root, ec);
}

// The registry's batch register fan-in (the scan path) must not dirty-mark the
// project store when every merged row matches its stored row. A dirty store on
// an unchanged project ends in TickPersistence flushing a zero-delta save,
// which full-rewrites the .assetdb every cold session.
TEST(AssetDbJournalGrowth, UnchangedProjectRescanLeavesTheStoreClean)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_assetdb_rescan_clean");
    std::error_code ec;
    fs::create_directories(root, ec);

    constexpr size_t kFileCount = 3;
    WriteTextFile(root / "a.txt", "A");
    WriteTextFile(root / "b.txt", "B");
    WriteTextFile(root / "sub" / "c.txt", "C");

    JobSystem::WorkStealingThreadPool pool(2);
    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(root, &pool));
    {
        // Drain the Initialize-kicked startup scan so the registry is quiet.
        auto scan = reg.ScanDirectoryAsync(root, true);
        (void)scan.get();
    }
    ASSERT_TRUE(reg.SaveToFile({}));

    auto pinned = reg.ProjectSourcePinned();
    ASSERT_TRUE(pinned && pinned->Store);
    ASSERT_FALSE(pinned->StoreDirty.load(std::memory_order_relaxed))
        << "SaveToFile left the store dirty";

    const fs::path dbFile = root / "AssetDatabase.assetdb";
    const std::string before = ReadTextFile(dbFile);
    ASSERT_FALSE(before.empty());

    // Rescan with nothing changed. This fresh project has no warm-start
    // snapshot, so every file re-traverses the register fan-in; the returned
    // count proves the unchanged entries reached the store pass rather than
    // being filtered upstream.
    {
        auto scan = reg.ScanDirectoryAsync(root, true);
        EXPECT_GE(scan.get(), kFileCount) << "rescan skipped the register fan-in";
    }
    EXPECT_FALSE(pinned->StoreDirty.load(std::memory_order_relaxed))
        << "an unchanged rescan dirtied the project store";
    reg.TickPersistence();
    EXPECT_EQ(ReadTextFile(dbFile), before) << "an unchanged rescan rewrote the .assetdb";

    // Positive control: a genuinely new file must still dirty the store, so
    // the assertions above cannot pass by the scan never writing anything.
    WriteTextFile(root / "d.txt", "D");
    {
        auto scan = reg.ScanDirectoryAsync(root, true);
        EXPECT_GE(scan.get(), kFileCount + 1);
    }
    EXPECT_TRUE(pinned->StoreDirty.load(std::memory_order_relaxed))
        << "a new file did not dirty the store";

    reg.Shutdown();
    fs::remove_all(root, ec);
}

// Same contract for the single-asset fan-in (RegisterAssetMetadata): an
// identical re-registration must not dirty the project store.
TEST(AssetDbJournalGrowth, IdenticalMetadataReregistrationLeavesTheStoreClean)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_assetdb_rereg_clean");
    std::error_code ec;
    fs::create_directories(root, ec);
    const fs::path assetPath = root / "a.txt";
    WriteTextFile(assetPath, "A");

    JobSystem::WorkStealingThreadPool pool(2);
    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(root, &pool));
    {
        auto scan = reg.ScanDirectoryAsync(root, true);
        (void)scan.get();
    }

    AssetMetadata md{};
    md.Guid = reg.GetOrCreateAssetGUID(assetPath);
    ASSERT_FALSE(md.Guid.IsNull());
    md.Path = assetPath;
    md.Name = "a";
    md.Extension = ".txt";
    md.Type = AssetType::Unknown;
    ASSERT_TRUE(reg.RegisterAssetMetadata(md));
    ASSERT_TRUE(reg.SaveToFile({}));

    auto pinned = reg.ProjectSourcePinned();
    ASSERT_TRUE(pinned && pinned->Store);
    ASSERT_FALSE(pinned->StoreDirty.load(std::memory_order_relaxed));

    ASSERT_TRUE(reg.RegisterAssetMetadata(md));
    EXPECT_FALSE(pinned->StoreDirty.load(std::memory_order_relaxed))
        << "re-registering identical metadata dirtied the project store";

    // Positive control: a changed persisted field must still dirty it.
    md.TypeId = "Test.CustomType";
    ASSERT_TRUE(reg.RegisterAssetMetadata(md));
    EXPECT_TRUE(pinned->StoreDirty.load(std::memory_order_relaxed))
        << "a real metadata change did not dirty the store";

    reg.Shutdown();
    fs::remove_all(root, ec);
}
