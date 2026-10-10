#include "AssetDatabase/AssetDbCache_Sqlite.h"

#include "Logger/Logger.h"

#include <sqlite3.h>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace GameEngine::AssetDatabase
{

namespace
{
static int64_t ToInt64(bool b) { return b ? 1 : 0; }

static bool StepDone(sqlite3_stmt* stmt)
{
    int rc = sqlite3_step(stmt);
    return rc == SQLITE_DONE;
}

static bool StepRow(sqlite3_stmt* stmt)
{
    int rc = sqlite3_step(stmt);
    return rc == SQLITE_ROW;
}

static void BindText(sqlite3_stmt* stmt, int idx, const std::string& s)
{
    sqlite3_bind_text(stmt, idx, s.c_str(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
}

static std::string ColumnText(sqlite3_stmt* stmt, int col)
{
    const unsigned char* txt = sqlite3_column_text(stmt, col);
    if (!txt)
        return {};
    return reinterpret_cast<const char*>(txt);
}

// Bind the GUID's 16 raw bytes as a BLOB. Null GUIDs bind a zero-length blob
// (the "no GUID, path-form edge" sentinel for the deps table). SQLITE_TRANSIENT
// makes SQLite copy the bytes during the bind call, so the caller's GUID can
// safely go out of scope before sqlite3_step runs.
static void BindGuidBlob(sqlite3_stmt* stmt, int idx, const GUID& g)
{
    if (g.IsNull())
    {
        // Static "" string lives forever; SQLITE_STATIC tells SQLite not to copy.
        sqlite3_bind_blob(stmt, idx, "", 0, SQLITE_STATIC);
        return;
    }
    const auto& bytes = g.GetData();
    sqlite3_bind_blob(stmt, idx, bytes.data(),
                      static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
}

// Read a 16-byte BLOB column back into a GUID. Zero-length / wrong-size /
// NULL columns return GUID::Null().
static GUID ColumnGuidBlob(sqlite3_stmt* stmt, int col)
{
    const void* data = sqlite3_column_blob(stmt, col);
    const int bytes = sqlite3_column_bytes(stmt, col);
    if (!data || bytes != static_cast<int>(GUID::kSize))
        return GUID::Null();
    GUID::Data buf{};
    std::memcpy(buf.data(), data, GUID::kSize);
    return GUID(buf);
}

} // namespace

AssetDbCache_Sqlite::~AssetDbCache_Sqlite()
{
    Close();
}

bool AssetDbCache_Sqlite::IsOpen() const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    return m_Db != nullptr;
}

std::string AssetDbCache_Sqlite::SqliteError(sqlite3* db)
{
    if (!db)
        return {};
    const char* msg = sqlite3_errmsg(db);
    return msg ? std::string(msg) : std::string();
}

bool AssetDbCache_Sqlite::Exec(const char* sql, std::string* outError) const
{
    if (!sql)
        return false;
    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_Db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        if (outError)
        {
            if (errMsg)
            {
                *outError = errMsg;
            }
            else
            {
                *outError = SqliteError(m_Db);
            }
        }
        if (errMsg)
            sqlite3_free(errMsg);
        return false;
    }
    if (errMsg)
        sqlite3_free(errMsg);
    return true;
}

bool AssetDbCache_Sqlite::Open(const std::filesystem::path& sqlitePath, std::string* outError)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (m_Db)
        return true;

    std::error_code ec;
    if (!sqlitePath.empty())
    {
        std::filesystem::create_directories(sqlitePath.parent_path(), ec);
    }

    int rc = SQLITE_ERROR;
#ifdef PLATFORM_WINDOWS
    const std::wstring ws = sqlitePath.wstring();
    rc = sqlite3_open16(ws.c_str(), &m_Db);
#else
    const std::string pathStr = sqlitePath.string();
    rc = sqlite3_open_v2(pathStr.c_str(), &m_Db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
#endif

    if (rc != SQLITE_OK || !m_Db)
    {
        if (outError)
            *outError = SqliteError(m_Db);
        if (m_Db)
        {
            sqlite3_close(m_Db);
            m_Db = nullptr;
        }
        return false;
    }

    // Pragmas for derived cache performance.
    (void)Exec("PRAGMA journal_mode=WAL;", nullptr);
    (void)Exec("PRAGMA synchronous=NORMAL;", nullptr);
    (void)Exec("PRAGMA temp_store=MEMORY;", nullptr);
    (void)Exec("PRAGMA foreign_keys=OFF;", nullptr);

    return EnsureSchema(outError);
}

void AssetDbCache_Sqlite::Close()
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db)
        return;
    FinalizeCachedStmts();
    sqlite3_close(m_Db);
    m_Db = nullptr;
    // A later Open on this object is a new session and spends a new counter.
    m_SessionCounter = 0;
    m_SchemaWasReset = false;
}

sqlite3_stmt* AssetDbCache_Sqlite::GetCachedStmt(sqlite3_stmt*& slot, const char* sql) const
{
    if (!slot)
    {
        if (sqlite3_prepare_v2(m_Db, sql, -1, &slot, nullptr) != SQLITE_OK)
        {
            slot = nullptr;
            return nullptr;
        }
        return slot; // fresh statement, nothing to reset
    }
    sqlite3_reset(slot);
    sqlite3_clear_bindings(slot);
    return slot;
}

void AssetDbCache_Sqlite::FinalizeCachedStmts()
{
    for (sqlite3_stmt** slot : {&m_StmtUpsertAsset, &m_StmtMirrorJournalRecord, &m_StmtEvictPathRow,
                                &m_StmtUpdateFingerprint, &m_StmtTryGetAsset,
                                &m_StmtLookupGuidByPath, &m_StmtSetKeyValue,
                                &m_StmtTryGetFingerprint})
    {
        if (*slot)
        {
            sqlite3_finalize(*slot);
            *slot = nullptr;
        }
    }
}

bool AssetDbCache_Sqlite::EnsureSchema(std::string* outError)
{
    // Caller holds mutex.
    if (!m_Db)
        return false;

    // Schema versioning (very light).
    if (!Exec("CREATE TABLE IF NOT EXISTS schema_version(version INTEGER NOT NULL);", outError))
        return false;

    constexpr int kCurrentVersion = 1;

    // Read current version.
    int current = 0;
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT COALESCE(MAX(version), 0) FROM schema_version;";
        if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        {
            if (outError)
                *outError = SqliteError(m_Db);
            return false;
        }
        if (StepRow(stmt))
        {
            current = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    // Cache is derived; if schema changes, rebuild from scratch. A brand-new
    // file lands here too (current == 0), which is the point: both leave every
    // derived index empty, and callers that gate destructive work on one need
    // to tell "measured nothing" from "measured nothing yet".
    m_SchemaWasReset = (current != kCurrentVersion);
    if (current != kCurrentVersion)
    {
        (void)Exec("DROP TABLE IF EXISTS deps;", nullptr);
        (void)Exec("DROP TABLE IF EXISTS kv;", nullptr);
        (void)Exec("DROP TABLE IF EXISTS assets;", nullptr);
        (void)Exec("DROP TABLE IF EXISTS provenance;", nullptr);
        (void)Exec("DROP TABLE IF EXISTS rename_suggestions;", nullptr);
        (void)Exec("DROP TABLE IF EXISTS meta;", nullptr);
        (void)Exec("DELETE FROM schema_version;", nullptr);
        const std::string insertSql =
            "INSERT INTO schema_version(version) VALUES(" + std::to_string(kCurrentVersion) + ");";
        if (!Exec(insertSql.c_str(), outError))
            return false;
    }

    // Core tables.
    if (!Exec(
            "CREATE TABLE IF NOT EXISTS assets("
            " guid TEXT PRIMARY KEY,"
            " path TEXT UNIQUE,"
            " type INTEGER NOT NULL,"
            " missing INTEGER NOT NULL DEFAULT 0,"
            " mtime INTEGER,"
            " size INTEGER,"
            " content_hash TEXT,"
            " file_id TEXT"
            ");",
            outError))
        return false;

    if (!Exec(
            "CREATE TABLE IF NOT EXISTS kv("
            " guid TEXT NOT NULL,"
            " key TEXT NOT NULL,"
            " value TEXT NOT NULL,"
            " PRIMARY KEY(guid,key)"
            ");",
            outError))
        return false;

    // deps table v5: GUID-form OR path-form edges. Parsers walking source
    // files often see references in path form ("textures/grass.png") long
    // before the target asset is scanned/registered; storing those as
    // first-class edges (rather than dropping them and rediscovering later)
    // lets the Phase 4 retarget UX show "missing reference X" with the path
    // the user authored.
    //
    //   guid          — referrer GUID, BLOB(16).
    //   dep_guid      — target GUID, BLOB(16); zero-length blob when path-targeted.
    //   target_path   — canonical mount-relative path; '' when GUID-targeted.
    //   edge_kind     — DepEdgeKind enum value. 0 = Other (default).
    //   field_locator — path within the referrer that holds the reference,
    //                   e.g. "Components.MeshRenderer.Materials[2]".
    //   ordinal       — disambiguates multiple edges with the same
    //                   (guid, target, edge_kind) tuple.
    //
    // CHECK enforces the "exactly one target" invariant. Zero-length blob and
    // empty-string sentinels (rather than NULL) keep PRIMARY KEY dedup
    // behaving right — SQLite treats each NULL as distinct in unique indexes.
    if (!Exec(
            "CREATE TABLE IF NOT EXISTS deps("
            " guid BLOB NOT NULL,"
            " dep_guid BLOB NOT NULL DEFAULT x'',"
            " target_path TEXT NOT NULL DEFAULT '',"
            " edge_kind INTEGER NOT NULL DEFAULT 0,"
            " field_locator TEXT NOT NULL DEFAULT '',"
            " ordinal INTEGER NOT NULL DEFAULT 0,"
            " PRIMARY KEY(guid, dep_guid, target_path, edge_kind, ordinal),"
            " CHECK ((length(dep_guid) = 16 AND target_path = '') OR (length(dep_guid) = 0 AND target_path != ''))"
            ");",
            outError))
        return false;

    // Additive column, no version bump: `missing_since` stamps when a record
    // went missing so the retention pass can age it. A bump would drop every
    // table — including the fingerprint columns that decide whether an
    // absentee can still rename-heal — to add one integer.
    if (!HasColumnLocked("assets", "missing_since") &&
        !Exec("ALTER TABLE assets ADD COLUMN missing_since INTEGER NOT NULL DEFAULT 0;", outError))
    {
        return false;
    }

    // Per-machine scalars that belong to the cache itself rather than to any
    // asset row. Today: session_counter, the unit ghost age is measured in.
    if (!Exec(
            "CREATE TABLE IF NOT EXISTS meta("
            " key TEXT PRIMARY KEY,"
            " value TEXT NOT NULL"
            ");",
            outError))
        return false;

    if (!Exec("CREATE INDEX IF NOT EXISTS idx_assets_path ON assets(path);", outError))
        return false;

    // Ghost-set index. EnumerateMissingAssets runs once per reconcile pass and
    // wants only the missing rows, but `missing` is 0 on essentially every row,
    // so an unindexed `WHERE missing<>0` full-scans the whole assets table
    // (~9ms at 50K records — paid every startup to return nothing).
    //
    // PARTIAL index: its WHERE must stay textually identical to the query's so
    // the planner can prove the query implies it — the plan then reads
    // `SCAN assets USING INDEX idx_assets_missing` over ghost rows only, making
    // the enumeration genuinely O(ghosts). Indexing `guid` (the sole selected
    // column) keeps it covering. Changing either WHERE clause without the other
    // silently restores the full scan.
    if (!Exec("CREATE INDEX IF NOT EXISTS idx_assets_missing ON assets(guid) WHERE missing<>0;",
              outError))
        return false;

    if (!Exec("CREATE INDEX IF NOT EXISTS idx_deps_dep ON deps(dep_guid);", outError))
        return false;
    // Reverse-lookup index by edge_kind for "give me all material→texture edges"
    // queries used by the Missing Assets retarget UX.
    if (!Exec("CREATE INDEX IF NOT EXISTS idx_deps_kind ON deps(edge_kind);", outError))
        return false;
    // Path-target lookup index: "what edges point at path X?" (rename / move
    // detection — when we discover a new asset at a path, we can fast-find
    // all referrers that were waiting on it).
    if (!Exec("CREATE INDEX IF NOT EXISTS idx_deps_target_path ON deps(target_path);", outError))
        return false;

    // provenance table v6: importer-time "produced_by" relationships.
    // Tracks sidecars produced by editor importers (e.g. ExportModelMaterials
    // writes Materials/X.material from a glb). Distinct from the deps table's
    // runtime "asset A's content references asset B" semantic — provenance
    // is import-time, has different consumers (delete-confirm UX, reimport
    // flow, asset panel "produced by X" badges), and intentionally does NOT
    // cascade-delete when the producer is removed.
    //
    //   produced_guid — PK; one row per produced asset. 1:1 produced→producer.
    //   producer_guid — indexed for forward-lookup ("what did this model produce?").
    //   importer_id   — string identifier ("ExportModelMaterials") so future
    //                   importer versions can vary behavior without schema bumps.
    //   created_at    — unix epoch ms; for diagnostics ("did you reimport recently?").
    if (!Exec(
            "CREATE TABLE IF NOT EXISTS provenance("
            " produced_guid BLOB NOT NULL,"
            " producer_guid BLOB NOT NULL,"
            " importer_id TEXT NOT NULL DEFAULT '',"
            " created_at INTEGER NOT NULL DEFAULT 0,"
            " PRIMARY KEY(produced_guid),"
            " CHECK (length(produced_guid) = 16 AND length(producer_guid) = 16)"
            ");",
            outError))
        return false;

    if (!Exec("CREATE INDEX IF NOT EXISTS idx_provenance_producer ON provenance(producer_guid);", outError))
        return false;

    // rename_suggestions: weak-evidence rename pairings the reconcile pass
    // declined to act on (see IAssetDbCache::RenameSuggestion). Advisory only —
    // nothing reads it to resolve a GUID, and deleting the cache merely costs
    // the next reconcile pass a re-derivation.
    //
    //   missing_guid   — PK; the tombstoned record. One suggestion per absentee,
    //                    so a re-derivation overwrites instead of accumulating.
    //   candidate_guid — '' under the Stored scheme, whose disk-walk candidates
    //                    carry no GUID yet. TEXT (not BLOB) to match the guid
    //                    columns in assets/kv, which this table joins against.
    if (!Exec(
            "CREATE TABLE IF NOT EXISTS rename_suggestions("
            " missing_guid TEXT PRIMARY KEY,"
            " missing_path TEXT NOT NULL DEFAULT '',"
            " candidate_guid TEXT NOT NULL DEFAULT '',"
            " candidate_path TEXT NOT NULL DEFAULT '',"
            " evidence_tier TEXT NOT NULL DEFAULT ''"
            ");",
            outError))
        return false;

    AdvanceSessionCounterLocked();

    return true;
}

bool AssetDbCache_Sqlite::HasColumnLocked(const char* table, const char* column) const
{
    // Caller holds mutex.
    if (!m_Db || !table || !column)
        return false;

    const std::string sql = std::string("PRAGMA table_info(") + table + ");";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    bool found = false;
    while (!found && StepRow(stmt))
    {
        // PRAGMA table_info columns: cid, name, type, notnull, dflt_value, pk.
        if (ColumnText(stmt, 1) == column)
            found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

void AssetDbCache_Sqlite::AdvanceSessionCounterLocked()
{
    // Caller holds mutex. Once per cache object: EnsureSchema is public and a
    // second call must not spend a session.
    if (m_SessionCounter != 0)
        return;

    int64_t previous = 0;
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT value FROM meta WHERE key='session_counter';";
        if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            if (StepRow(stmt))
            {
                // An unparseable row yields 0 and the counter starts again,
                // which costs standing ghosts their accumulated age — the
                // safe direction, never a premature collection.
                const std::string text = ColumnText(stmt, 0);
                previous = std::strtoll(text.c_str(), nullptr, 10);
            }
            sqlite3_finalize(stmt);
        }
    }

    // The counter starts at 1: 0 is reserved for "unstamped" in missing_since.
    m_SessionCounter = (previous > 0 ? previous : 0) + 1;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO meta(key, value) VALUES('session_counter', ?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        BindText(stmt, 1, std::to_string(m_SessionCounter));
        (void)StepDone(stmt);
        sqlite3_finalize(stmt);
    }
}

bool AssetDbCache_Sqlite::StepEvictPathRowLocked(const AssetRecord& record, std::string* outError)
{
    // The cache mirrors the authoritative store, so an incoming (guid, path)
    // pair is newer truth than whatever row currently holds that path. Evict
    // any row that owns the path under a different guid first — otherwise the
    // INSERTs below trip UNIQUE(path) (SQLITE_CONSTRAINT) whenever two racing
    // first-registrations of the same path minted different GUIDs, and inside
    // UpsertAssetBatch that single failure rolled back the whole transaction
    // including every other row's fingerprint (the empty-warm-start-snapshot
    // flake caught by AssetDbHardening.SnapshotInvalidatedWhenIgnoreRulesChange).
    if (record.path.empty())
        return true;

    const char* evictSql = "DELETE FROM assets WHERE path = ? AND guid <> ?;";
    sqlite3_stmt* evict = GetCachedStmt(m_StmtEvictPathRow, evictSql);
    if (!evict)
    {
        if (outError)
            *outError = SqliteError(m_Db);
        return false;
    }
    BindText(evict, 1, record.path);
    BindText(evict, 2, record.guid.ToString());
    if (!StepDone(evict))
    {
        if (outError)
            *outError = SqliteError(m_Db);
        return false;
    }
    return true;
}

bool AssetDbCache_Sqlite::StepUpsertAssetLocked(const AssetRecord& record, std::string* outError)
{
    if (!StepEvictPathRowLocked(record, outError))
        return false;

    const char* sql =
        "INSERT INTO assets(guid, path, type, missing) VALUES(?,?,?,?) "
        "ON CONFLICT(guid) DO UPDATE SET path=excluded.path, type=excluded.type, missing=excluded.missing;";
    sqlite3_stmt* stmt = GetCachedStmt(m_StmtUpsertAsset, sql);
    if (!stmt)
    {
        if (outError)
            *outError = SqliteError(m_Db);
        return false;
    }

    BindText(stmt, 1, record.guid.ToString());
    BindText(stmt, 2, record.path);
    sqlite3_bind_int(stmt, 3, static_cast<int>(record.type));
    sqlite3_bind_int64(stmt, 4, ToInt64(record.missing));

    const bool ok = StepDone(stmt);
    if (!ok && outError)
        *outError = SqliteError(m_Db);
    return ok;
}

bool AssetDbCache_Sqlite::StepUpdateFingerprintLocked(const GUID& guid,
                                                      int64_t mtime,
                                                      int64_t size,
                                                      const std::string& contentHash,
                                                      const std::string& fileId,
                                                      std::string* outError)
{
    // NULL path avoids violating UNIQUE(path) when we don't know it.
    const char* sql =
        "INSERT INTO assets(guid, path, type, missing, mtime, size, content_hash, file_id) "
        "VALUES(?, NULL, 0, 0, ?, ?, ?, ?) "
        "ON CONFLICT(guid) DO UPDATE SET "
        "mtime=excluded.mtime, size=excluded.size, content_hash=excluded.content_hash, file_id=excluded.file_id;";
    sqlite3_stmt* stmt = GetCachedStmt(m_StmtUpdateFingerprint, sql);
    if (!stmt)
    {
        if (outError)
            *outError = SqliteError(m_Db);
        return false;
    }

    BindText(stmt, 1, guid.ToString());
    sqlite3_bind_int64(stmt, 2, mtime);
    sqlite3_bind_int64(stmt, 3, size);
    if (!contentHash.empty())
        BindText(stmt, 4, contentHash);
    else
        sqlite3_bind_null(stmt, 4);
    if (!fileId.empty())
        BindText(stmt, 5, fileId);
    else
        sqlite3_bind_null(stmt, 5);

    const bool ok = StepDone(stmt);
    if (!ok && outError)
        *outError = SqliteError(m_Db);
    return ok;
}

bool AssetDbCache_Sqlite::UpsertAsset(const AssetRecord& record, std::string* outError)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || record.guid.IsNull())
        return false;

    const bool ok = StepUpsertAssetLocked(record, outError);
    if (ok)
        m_MutationCount.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

bool AssetDbCache_Sqlite::MirrorJournalRecord(const AssetRecord& record, std::string* outError)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || record.guid.IsNull())
        return false;

    if (!StepEvictPathRowLocked(record, outError))
        return false;

    // `missing` and `missing_since` appear in neither the INSERT list nor the
    // UPDATE list — same discipline the fingerprint columns get in
    // SetAssetMissing, and for the same reason: a writer that did not look at
    // the file must not answer the question "is it there". A new row therefore
    // inserts unflagged (nothing observed yet) and an existing row keeps the
    // observation the reconcile pass or a live delete put there.
    const char* sql =
        "INSERT INTO assets(guid, path, type) VALUES(?,?,?) "
        "ON CONFLICT(guid) DO UPDATE SET path=excluded.path, type=excluded.type;";
    sqlite3_stmt* stmt = GetCachedStmt(m_StmtMirrorJournalRecord, sql);
    if (!stmt)
    {
        if (outError)
            *outError = SqliteError(m_Db);
        return false;
    }

    BindText(stmt, 1, record.guid.ToString());
    BindText(stmt, 2, record.path);
    sqlite3_bind_int(stmt, 3, static_cast<int>(record.type));

    const bool ok = StepDone(stmt);
    if (!ok && outError)
        *outError = SqliteError(m_Db);
    if (ok)
        m_MutationCount.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

bool AssetDbCache_Sqlite::UpsertAssetBatch(const std::vector<AssetUpsertBatchEntry>& entries, std::string* outError)
{
    if (entries.empty())
        return true;

    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db)
        return false;

    if (!Exec("BEGIN IMMEDIATE TRANSACTION;", outError))
        return false;

    bool ok = true;
    size_t applied = 0;
    for (const AssetUpsertBatchEntry& e : entries)
    {
        if (e.Record.guid.IsNull())
            continue;
        if (!StepUpsertAssetLocked(e.Record, outError))
        {
            ok = false;
            break;
        }
        if (e.HasFingerprint &&
            !StepUpdateFingerprintLocked(e.Record.guid, e.Mtime, e.Size, e.ContentHash, e.FileId, outError))
        {
            ok = false;
            break;
        }
        ++applied;
    }

    if (ok)
    {
        ok = Exec("COMMIT;", outError);
    }
    if (!ok)
    {
        std::string rollbackErr;
        (void)Exec("ROLLBACK;", &rollbackErr);
        return false;
    }

    m_MutationCount.fetch_add(applied, std::memory_order_relaxed);
    return true;
}

bool AssetDbCache_Sqlite::RemoveAsset(const GUID& guid, std::string* outError)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || guid.IsNull())
        return false;

    // Atomic 5-statement cleanup:
    //   1. DELETE FROM deps WHERE guid=?               — outgoing edges owned by this asset
    //   2. DELETE FROM deps WHERE dep_guid=?           — incoming edges pointing AT this asset
    //   3. DELETE FROM kv WHERE guid=?                 — derived kv mirror rows
    //   4. DELETE FROM rename_suggestions WHERE
    //      missing_guid=?                              — advisory pairing about this absentee
    //   5. DELETE FROM assets WHERE guid=?             — the asset row itself (incl. fingerprint columns)
    // Cleaning the edge directions, the kv mirror and the suggestion here means
    // call sites can't forget and leak orphan rows that grow unbounded on
    // long-lived projects. A suggestion names a missing record; once that
    // record is gone (healed by a stronger tier, or deleted) the pairing has no
    // subject left. Provenance intentionally stays: it has its own lifecycle
    // API (UnregisterProvenance) and different consumers.
    if (!Exec("BEGIN IMMEDIATE TRANSACTION;", outError))
        return false;

    bool ok = true;
    auto runDelete = [&](const char* sql, auto bindFn) {
        if (!ok)
            return;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        {
            if (outError) *outError = SqliteError(m_Db);
            ok = false;
            return;
        }
        bindFn(stmt);
        if (!StepDone(stmt))
        {
            if (outError) *outError = SqliteError(m_Db);
            ok = false;
        }
        sqlite3_finalize(stmt);
    };

    runDelete("DELETE FROM deps WHERE guid=?;",
              [&](sqlite3_stmt* s) { BindGuidBlob(s, 1, guid); });
    runDelete("DELETE FROM deps WHERE dep_guid=?;",
              [&](sqlite3_stmt* s) { BindGuidBlob(s, 1, guid); });
    runDelete("DELETE FROM kv WHERE guid=?;",
              [&](sqlite3_stmt* s) { BindText(s, 1, guid.ToString()); });
    runDelete("DELETE FROM rename_suggestions WHERE missing_guid=?;",
              [&](sqlite3_stmt* s) { BindText(s, 1, guid.ToString()); });
    runDelete("DELETE FROM assets WHERE guid=?;",
              [&](sqlite3_stmt* s) { BindText(s, 1, guid.ToString()); });

    if (ok)
    {
        ok = Exec("COMMIT;", outError);
    }
    if (!ok)
    {
        std::string rollbackErr;
        (void)Exec("ROLLBACK;", &rollbackErr);
    }

    if (ok)
        m_MutationCount.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

bool AssetDbCache_Sqlite::TryGetAsset(const GUID& guid, AssetRecord& outRecord) const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || guid.IsNull())
        return false;

    const char* sql = "SELECT guid, path, type, missing FROM assets WHERE guid=?;";
    sqlite3_stmt* stmt = GetCachedStmt(m_StmtTryGetAsset, sql);
    if (!stmt)
    {
        return false;
    }
    BindText(stmt, 1, guid.ToString());

    bool ok = false;
    if (StepRow(stmt))
    {
        outRecord.guid = GUID(ColumnText(stmt, 0).c_str());
        outRecord.path = ColumnText(stmt, 1);
        outRecord.type = static_cast<AssetType>(sqlite3_column_int(stmt, 2));
        outRecord.missing = sqlite3_column_int(stmt, 3) != 0;
        ok = !outRecord.guid.IsNull();
    }
    sqlite3_reset(stmt);
    return ok;
}

std::optional<GUID> AssetDbCache_Sqlite::LookupGuidByPath(const std::string& canonicalPath) const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || canonicalPath.empty())
        return std::nullopt;

    const char* sql = "SELECT guid FROM assets WHERE path=?;";
    sqlite3_stmt* stmt = GetCachedStmt(m_StmtLookupGuidByPath, sql);
    if (!stmt)
    {
        return std::nullopt;
    }
    BindText(stmt, 1, canonicalPath);
    std::optional<GUID> out;
    if (StepRow(stmt))
    {
        GUID g(ColumnText(stmt, 0).c_str());
        if (!g.IsNull())
            out = g;
    }
    sqlite3_reset(stmt);
    return out;
}

std::vector<GUID> AssetDbCache_Sqlite::EnumerateMissingAssets() const
{
    std::vector<GUID> result;

    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db)
        return result;

    // One-shot statement: this runs once per reconcile pass and returns only
    // the (usually tiny) ghost set. The WHERE is served by the partial index
    // idx_assets_missing (EnsureSchema) and must match its WHERE verbatim —
    // otherwise the planner falls back to a full table scan. The plan-pin
    // test EXPLAINs kEnumerateMissingAssetsSql, so a drift here fails it.
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, kEnumerateMissingAssetsSql, -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    while (StepRow(stmt))
    {
        GUID guid(ColumnText(stmt, 0).c_str());
        if (!guid.IsNull())
            result.push_back(guid);
    }

    sqlite3_finalize(stmt);
    return result;
}

bool AssetDbCache_Sqlite::SetAssetMissing(const AssetRecord& record,
                                          bool missing,
                                          int64_t missingSinceSession,
                                          std::string* outError)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || record.guid.IsNull())
        return false;

    if (!StepEvictPathRowLocked(record, outError))
        return false;

    // Identity columns follow the record so a GUID the cache has never seen
    // still lands a usable row; the fingerprint columns are absent from both
    // the INSERT list and the UPDATE list, so an existing row keeps whatever
    // the scan measured. That is what makes a ghost's healability survive
    // being flagged.
    const char* sql =
        "INSERT INTO assets(guid, path, type, missing, missing_since) VALUES(?,?,?,?,?) "
        "ON CONFLICT(guid) DO UPDATE SET path=excluded.path, type=excluded.type, "
        "missing=excluded.missing, missing_since=excluded.missing_since;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        if (outError)
            *outError = SqliteError(m_Db);
        return false;
    }

    BindText(stmt, 1, record.guid.ToString());
    BindText(stmt, 2, record.path);
    sqlite3_bind_int(stmt, 3, static_cast<int>(record.type));
    sqlite3_bind_int64(stmt, 4, ToInt64(missing));
    sqlite3_bind_int64(stmt, 5, missing ? missingSinceSession : 0);

    const bool ok = StepDone(stmt);
    if (!ok && outError)
        *outError = SqliteError(m_Db);
    sqlite3_finalize(stmt);

    if (ok)
        m_MutationCount.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

std::vector<IAssetDbCache::GhostRow> AssetDbCache_Sqlite::EnumerateGhostRows() const
{
    std::vector<GhostRow> result;

    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db)
        return result;

    // The HasFingerprint expression is TryGetFileFingerprint's validity rule
    // (any non-null fingerprint column) spelled as SQL. It must stay identical
    // to it: the rename matcher indexes an absentee only when that call
    // succeeds, so this expression is precisely "could this ghost ever be
    // matched by any tier".
    //
    // The WHERE matches idx_assets_missing's partial predicate so the ghost
    // scan stays O(ghosts), but the extra columns make it non-covering — the
    // reason this is a separate query from EnumerateMissingAssets and runs
    // only when a retention policy is armed.
    const char* sql =
        "SELECT guid, missing_since, "
        "(mtime IS NOT NULL OR size IS NOT NULL OR content_hash IS NOT NULL OR file_id IS NOT NULL) "
        "FROM assets WHERE missing<>0;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    while (StepRow(stmt))
    {
        GhostRow row{};
        row.Guid = GUID(ColumnText(stmt, 0).c_str());
        if (row.Guid.IsNull())
            continue;
        row.MissingSinceSession = sqlite3_column_int64(stmt, 1);
        row.HasFingerprint = sqlite3_column_int(stmt, 2) != 0;
        result.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);
    return result;
}

bool AssetDbCache_Sqlite::UpdateFileFingerprint(const GUID& guid,
                                                int64_t mtime,
                                                int64_t size,
                                                const std::string& contentHash,
                                                const std::string& fileId,
                                                std::string* outError)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || guid.IsNull())
        return false;

    const bool ok = StepUpdateFingerprintLocked(guid, mtime, size, contentHash, fileId, outError);
    if (ok)
        m_MutationCount.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

std::unordered_map<GUID, AssetDbCache_Sqlite::FileFingerprint>
AssetDbCache_Sqlite::EnumerateAllFingerprints() const
{
    std::unordered_map<GUID, FileFingerprint> result;

    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db)
        return result;

    // Single SELECT instead of per-row prepare/finalize. Per-call
    // TryGetFileFingerprint pays ~17μs of SQLite prepare/finalize
    // overhead — at 50K records that's 850ms; this single statement
    // is ~50ms.
    const char* sql =
        "SELECT guid, mtime, size, content_hash, file_id, path FROM assets "
        "WHERE mtime IS NOT NULL OR size IS NOT NULL OR "
        "content_hash IS NOT NULL OR file_id IS NOT NULL;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    while (StepRow(stmt))
    {
        const std::string guidStr = ColumnText(stmt, 0);
        if (guidStr.empty())
            continue;
        GUID guid(guidStr);
        if (guid.IsNull())
            continue;

        FileFingerprint fp{};
        const bool mtimeIsNull = sqlite3_column_type(stmt, 1) == SQLITE_NULL;
        const bool sizeIsNull = sqlite3_column_type(stmt, 2) == SQLITE_NULL;
        const bool hashIsNull = sqlite3_column_type(stmt, 3) == SQLITE_NULL;
        const bool idIsNull = sqlite3_column_type(stmt, 4) == SQLITE_NULL;
        fp.mtime = mtimeIsNull ? 0 : sqlite3_column_int64(stmt, 1);
        fp.size = sizeIsNull ? 0 : sqlite3_column_int64(stmt, 2);
        fp.contentHash = hashIsNull ? std::string() : ColumnText(stmt, 3);
        fp.fileId = idIsNull ? std::string() : ColumnText(stmt, 4);
        fp.path = sqlite3_column_type(stmt, 5) == SQLITE_NULL ? std::string() : ColumnText(stmt, 5);
        fp.valid = !(mtimeIsNull && sizeIsNull && hashIsNull && idIsNull);
        if (!fp.valid)
            continue;
        result.emplace(std::move(guid), std::move(fp));
    }

    sqlite3_finalize(stmt);
    return result;
}

bool AssetDbCache_Sqlite::TryGetFileFingerprint(const GUID& guid, FileFingerprint& outFingerprint) const
{
    outFingerprint = FileFingerprint{};
    if (guid.IsNull())
        return false;

    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db)
        return false;

    const char* sql = "SELECT mtime, size, content_hash, file_id FROM assets WHERE guid=?;";
    sqlite3_stmt* stmt = GetCachedStmt(m_StmtTryGetFingerprint, sql);
    if (!stmt)
    {
        return false;
    }
    BindText(stmt, 1, guid.ToString());

    bool ok = false;
    if (StepRow(stmt))
    {
        const bool mtimeIsNull = sqlite3_column_type(stmt, 0) == SQLITE_NULL;
        const bool sizeIsNull = sqlite3_column_type(stmt, 1) == SQLITE_NULL;
        const bool hashIsNull = sqlite3_column_type(stmt, 2) == SQLITE_NULL;
        const bool idIsNull = sqlite3_column_type(stmt, 3) == SQLITE_NULL;

        outFingerprint.mtime = mtimeIsNull ? 0 : sqlite3_column_int64(stmt, 0);
        outFingerprint.size = sizeIsNull ? 0 : sqlite3_column_int64(stmt, 1);
        outFingerprint.contentHash = hashIsNull ? std::string() : ColumnText(stmt, 2);
        outFingerprint.fileId = idIsNull ? std::string() : ColumnText(stmt, 3);
        outFingerprint.valid = !(mtimeIsNull && sizeIsNull && hashIsNull && idIsNull);
        ok = true;
    }

    sqlite3_reset(stmt);
    return ok && outFingerprint.valid;
}

bool AssetDbCache_Sqlite::SetKeyValue(const GUID& guid, const std::string& key, const std::string& value, std::string* outError)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || guid.IsNull() || key.empty())
        return false;

    const char* sql =
        "INSERT INTO kv(guid, key, value) VALUES(?,?,?) "
        "ON CONFLICT(guid,key) DO UPDATE SET value=excluded.value;";
    sqlite3_stmt* stmt = GetCachedStmt(m_StmtSetKeyValue, sql);
    if (!stmt)
    {
        if (outError)
            *outError = SqliteError(m_Db);
        return false;
    }
    BindText(stmt, 1, guid.ToString());
    BindText(stmt, 2, key);
    BindText(stmt, 3, value);
    const bool ok = StepDone(stmt);
    if (!ok && outError)
        *outError = SqliteError(m_Db);
    if (ok)
        m_MutationCount.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

bool AssetDbCache_Sqlite::TryGetKeyValue(const GUID& guid, const std::string& key, std::string& outValue) const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || guid.IsNull() || key.empty())
        return false;

    const char* sql = "SELECT value FROM kv WHERE guid=? AND key=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        return false;
    }
    BindText(stmt, 1, guid.ToString());
    BindText(stmt, 2, key);
    bool ok = false;
    if (StepRow(stmt))
    {
        outValue = ColumnText(stmt, 0);
        ok = true;
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool AssetDbCache_Sqlite::RemoveKeyValue(const GUID& guid, const std::string& key, std::string* outError)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || guid.IsNull() || key.empty())
        return false;

    const char* sql = "DELETE FROM kv WHERE guid=? AND key=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        if (outError)
            *outError = SqliteError(m_Db);
        return false;
    }
    BindText(stmt, 1, guid.ToString());
    BindText(stmt, 2, key);
    const bool ok = StepDone(stmt);
    if (!ok && outError)
        *outError = SqliteError(m_Db);
    sqlite3_finalize(stmt);
    if (ok)
        m_MutationCount.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

bool AssetDbCache_Sqlite::RecordRenameSuggestion(const RenameSuggestion& suggestion, std::string* outError)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || suggestion.MissingGuid.IsNull() || suggestion.CandidatePath.empty())
        return false;

    // One row per absentee: a re-derived suggestion overwrites its predecessor
    // rather than piling up, so the table's size is bounded by the number of
    // tombstones that currently have a weak-evidence candidate.
    const char* sql =
        "INSERT INTO rename_suggestions"
        "(missing_guid, missing_path, candidate_guid, candidate_path, evidence_tier) "
        "VALUES(?,?,?,?,?) "
        "ON CONFLICT(missing_guid) DO UPDATE SET "
        "missing_path=excluded.missing_path, "
        "candidate_guid=excluded.candidate_guid, "
        "candidate_path=excluded.candidate_path, "
        "evidence_tier=excluded.evidence_tier;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        if (outError)
            *outError = SqliteError(m_Db);
        return false;
    }
    BindText(stmt, 1, suggestion.MissingGuid.ToString());
    BindText(stmt, 2, suggestion.MissingPath);
    // A null candidate GUID stores as '' — the Stored scheme's disk-walk
    // candidates have no registered GUID, and the empty string round-trips
    // back through GUID(str) as null.
    BindText(stmt, 3, suggestion.CandidateGuid.IsNull() ? std::string() : suggestion.CandidateGuid.ToString());
    BindText(stmt, 4, suggestion.CandidatePath);
    BindText(stmt, 5, suggestion.EvidenceTier);

    const bool ok = StepDone(stmt);
    if (!ok && outError)
        *outError = SqliteError(m_Db);
    sqlite3_finalize(stmt);
    if (ok)
        m_MutationCount.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

bool AssetDbCache_Sqlite::RemoveRenameSuggestion(const GUID& missingGuid, std::string* outError)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || missingGuid.IsNull())
        return false;

    const char* sql = "DELETE FROM rename_suggestions WHERE missing_guid=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        if (outError)
            *outError = SqliteError(m_Db);
        return false;
    }
    BindText(stmt, 1, missingGuid.ToString());

    const bool ok = StepDone(stmt);
    if (!ok && outError)
        *outError = SqliteError(m_Db);
    sqlite3_finalize(stmt);
    if (ok)
        m_MutationCount.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

std::vector<IAssetDbCache::RenameSuggestion> AssetDbCache_Sqlite::EnumerateRenameSuggestions() const
{
    std::vector<RenameSuggestion> result;

    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db)
        return result;

    // One-shot statement: read by the reconcile diagnostics and the review UX,
    // never on a hot path. Ordered so callers see a stable list.
    const char* sql =
        "SELECT missing_guid, missing_path, candidate_guid, candidate_path, evidence_tier "
        "FROM rename_suggestions ORDER BY missing_path;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    while (StepRow(stmt))
    {
        RenameSuggestion row{};
        row.MissingGuid = GUID(ColumnText(stmt, 0).c_str());
        row.MissingPath = ColumnText(stmt, 1);
        row.CandidateGuid = GUID(ColumnText(stmt, 2).c_str());
        row.CandidatePath = ColumnText(stmt, 3);
        row.EvidenceTier = ColumnText(stmt, 4);
        if (!row.MissingGuid.IsNull())
            result.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);
    return result;
}

bool AssetDbCache_Sqlite::ReplaceDependencies(const GUID& guid, const std::vector<GUID>& deps, std::string* outError)
{
    // Adapt simple GUID list onto the richer DepEdge writer. All edges land
    // with kind=Other, empty locator, ordinal in input order — the same
    // values the legacy schema's column defaults gave us.
    std::vector<DepEdge> edges;
    edges.reserve(deps.size());
    uint32_t ordinal = 0;
    for (const GUID& dep : deps)
    {
        if (dep.IsNull())
            continue;
        DepEdge e;
        e.Referrer = guid;
        e.Target = dep;
        e.Kind = DepEdgeKind::Other;
        e.Ordinal = ordinal++;
        edges.push_back(std::move(e));
    }
    return ReplaceDependencies(guid, edges, outError);
}

bool AssetDbCache_Sqlite::ReplaceDependencies(const GUID& guid, const std::vector<DepEdge>& edges, std::string* outError)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || guid.IsNull())
        return false;

    if (!Exec("BEGIN IMMEDIATE TRANSACTION;", outError))
        return false;

    bool ok = true;
    {
        sqlite3_stmt* del = nullptr;
        if (sqlite3_prepare_v2(m_Db, "DELETE FROM deps WHERE guid=?;", -1, &del, nullptr) != SQLITE_OK)
        {
            ok = false;
        }
        else
        {
            BindGuidBlob(del, 1, guid);
            ok = StepDone(del);
            sqlite3_finalize(del);
        }
    }

    if (ok && !edges.empty())
    {
        // INSERT OR IGNORE so duplicate
        // (guid, dep_guid, target_path, edge_kind, ordinal) tuples — which
        // would happen if a parser re-emits the same edge — are dropped
        // silently rather than failing the transaction.
        sqlite3_stmt* ins = nullptr;
        const char* sql =
            "INSERT OR IGNORE INTO deps(guid, dep_guid, target_path, edge_kind, field_locator, ordinal) "
            "VALUES(?,?,?,?,?,?);";
        if (sqlite3_prepare_v2(m_Db, sql, -1, &ins, nullptr) != SQLITE_OK)
        {
            ok = false;
        }
        else
        {
            for (const DepEdge& edge : edges)
            {
                if (!IsValidDepEdge(edge))
                    continue; // exactly-one-target invariant violation; drop
                sqlite3_reset(ins);
                sqlite3_clear_bindings(ins);
                BindGuidBlob(ins, 1, guid);
                BindGuidBlob(ins, 2, edge.Target); // null GUID -> zero-length blob sentinel
                BindText(ins, 3, edge.TargetPath);
                sqlite3_bind_int(ins, 4, static_cast<int>(edge.Kind));
                BindText(ins, 5, edge.FieldLocator);
                sqlite3_bind_int64(ins, 6, static_cast<int64_t>(edge.Ordinal));
                if (!StepDone(ins))
                {
                    ok = false;
                    break;
                }
            }
            sqlite3_finalize(ins);
        }
    }

    if (ok)
    {
        ok = Exec("COMMIT;", outError);
    }
    else
    {
        (void)Exec("ROLLBACK;", nullptr);
        if (outError && outError->empty())
            *outError = SqliteError(m_Db);
    }

    if (ok)
        m_MutationCount.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

std::vector<DepEdge> AssetDbCache_Sqlite::GetDependencyEdges(const GUID& guid) const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    std::vector<DepEdge> out;
    if (!m_Db || guid.IsNull())
        return out;

    const char* sql =
        "SELECT dep_guid, target_path, edge_kind, field_locator, ordinal FROM deps "
        "WHERE guid=? ORDER BY edge_kind, ordinal;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        return out;
    }
    BindGuidBlob(stmt, 1, guid);

    while (StepRow(stmt))
    {
        DepEdge e;
        e.Referrer = guid;
        e.Target = ColumnGuidBlob(stmt, 0); // null when zero-length blob (path-form edge)
        e.TargetPath = ColumnText(stmt, 1);
        e.Kind = static_cast<DepEdgeKind>(sqlite3_column_int(stmt, 2));
        e.FieldLocator = ColumnText(stmt, 3);
        e.Ordinal = static_cast<uint32_t>(sqlite3_column_int64(stmt, 4));
        if (IsValidDepEdge(e))
        {
            out.push_back(std::move(e));
        }
        else
        {
            // CHECK constraint should prevent this on writes; if a row passes
            // the read filter it implies DB corruption or a schema-bypass bug.
            // Surface it so we don't debug silent data loss later.
            Logger::Log::Warning(
                "AssetDbCache_Sqlite: dropped invalid dep edge for {} (kind={}, locator='{}', "
                "guid_target='{}', path_target='{}')",
                guid.ToString(), static_cast<int>(e.Kind), e.FieldLocator,
                e.Target.IsNull() ? "" : e.Target.ToString(), e.TargetPath);
        }
    }
    sqlite3_finalize(stmt);
    return out;
}

void AssetDbCache_Sqlite::IterateDependents(const GUID& depGuid,
                                              const std::function<bool(const GUID&)>& visit) const
{
    if (!visit)
        return;

    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || depGuid.IsNull())
        return;

    // GUID-form referrers only (16-byte dep_guid). Path-form refs live in a
    // separate index and are streamed via IterateDependentsByPath.
    const char* sql = "SELECT guid FROM deps WHERE dep_guid=? AND length(dep_guid)=16;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;
    BindGuidBlob(stmt, 1, depGuid);

    while (StepRow(stmt))
    {
        GUID g = ColumnGuidBlob(stmt, 0);
        if (g.IsNull())
            continue;
        if (!visit(g))
            break;
    }
    sqlite3_finalize(stmt);
}

void AssetDbCache_Sqlite::IterateDependentsByPath(const std::string& canonicalPath,
                                                    const std::function<bool(const GUID&)>& visit) const
{
    if (!visit)
        return;

    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || canonicalPath.empty())
        return;

    // Path-form edges: dep_guid is zero-length BLOB, target_path matches the
    // canonical mount-relative path. Uses idx_deps_target_path.
    const char* sql = "SELECT guid FROM deps WHERE target_path=? AND length(dep_guid)=0;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, canonicalPath.data(),
                      static_cast<int>(canonicalPath.size()), SQLITE_TRANSIENT);

    while (StepRow(stmt))
    {
        GUID g = ColumnGuidBlob(stmt, 0);
        if (g.IsNull())
            continue;
        if (!visit(g))
            break;
    }
    sqlite3_finalize(stmt);
}

size_t AssetDbCache_Sqlite::CountDependents(const GUID& depGuid) const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || depGuid.IsNull())
        return 0;

    const char* sql = "SELECT COUNT(*) FROM deps WHERE dep_guid=? AND length(dep_guid)=16;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return 0;
    BindGuidBlob(stmt, 1, depGuid);
    size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    sqlite3_finalize(stmt);
    return count;
}

size_t AssetDbCache_Sqlite::CountDependentsByPath(const std::string& canonicalPath) const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || canonicalPath.empty())
        return 0;

    const char* sql = "SELECT COUNT(*) FROM deps WHERE target_path=? AND length(dep_guid)=0;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, canonicalPath.data(),
                      static_cast<int>(canonicalPath.size()), SQLITE_TRANSIENT);
    size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    sqlite3_finalize(stmt);
    return count;
}

std::vector<GUID> AssetDbCache_Sqlite::GetDependencies(const GUID& guid) const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    std::vector<GUID> out;
    if (!m_Db || guid.IsNull())
        return out;

    // Returns only GUID-form edges (dep_guid is a 16-byte blob). Path-form
    // edges are exposed via GetDependencyEdges() where the caller can resolve them.
    const char* sql = "SELECT dep_guid FROM deps WHERE guid=? AND length(dep_guid)=16;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        return out;
    }
    BindGuidBlob(stmt, 1, guid);

    while (StepRow(stmt))
    {
        GUID g = ColumnGuidBlob(stmt, 0);
        if (!g.IsNull())
            out.push_back(g);
    }
    sqlite3_finalize(stmt);
    return out;
}

// ----------------------------------------------------------------------------
// Provenance: importer-time "produced_by" relationships.
// ----------------------------------------------------------------------------

bool AssetDbCache_Sqlite::RegisterProvenance(const GUID& produced,
                                             const GUID& producer,
                                             std::string_view importerId,
                                             std::string* outError)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    // Reject null GUIDs and self-loops. The registry wrapper already rejects
    // self-loops, but the cache also enforces the invariant so direct callers
    // (tests, future code) can't bypass it.
    if (!m_Db || produced.IsNull() || producer.IsNull() || produced == producer)
        return false;

    // INSERT OR REPLACE so reimport (same produced GUID, possibly new producer
    // or new importer_id) overwrites the existing row atomically. The PK on
    // produced_guid enforces 1:1 produced -> producer.
    const char* sql =
        "INSERT INTO provenance(produced_guid, producer_guid, importer_id, created_at) "
        "VALUES(?,?,?,?) "
        "ON CONFLICT(produced_guid) DO UPDATE SET "
        "producer_guid=excluded.producer_guid, "
        "importer_id=excluded.importer_id, "
        "created_at=excluded.created_at;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        if (outError)
            *outError = SqliteError(m_Db);
        return false;
    }

    BindGuidBlob(stmt, 1, produced);
    BindGuidBlob(stmt, 2, producer);
    sqlite3_bind_text(stmt, 3, importerId.data(),
                      static_cast<int>(importerId.size()), SQLITE_TRANSIENT);
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    sqlite3_bind_int64(stmt, 4, nowMs);

    const bool ok = StepDone(stmt);
    if (!ok && outError)
        *outError = SqliteError(m_Db);
    sqlite3_finalize(stmt);
    if (ok)
        m_MutationCount.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

bool AssetDbCache_Sqlite::UnregisterProvenance(const GUID& produced, std::string* outError)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || produced.IsNull())
        return false;

    const char* sql = "DELETE FROM provenance WHERE produced_guid=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        if (outError)
            *outError = SqliteError(m_Db);
        return false;
    }
    BindGuidBlob(stmt, 1, produced);

    const bool ok = StepDone(stmt);
    if (!ok && outError)
        *outError = SqliteError(m_Db);
    sqlite3_finalize(stmt);
    if (ok)
        m_MutationCount.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

GUID AssetDbCache_Sqlite::GetProducer(const GUID& produced) const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || produced.IsNull())
        return GUID::Null();

    const char* sql = "SELECT producer_guid FROM provenance WHERE produced_guid=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return GUID::Null();
    BindGuidBlob(stmt, 1, produced);

    GUID result = GUID::Null();
    if (StepRow(stmt))
        result = ColumnGuidBlob(stmt, 0);
    sqlite3_finalize(stmt);
    return result;
}

std::vector<GUID> AssetDbCache_Sqlite::EnumerateProducedAssets(const GUID& producer) const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    std::vector<GUID> out;
    if (!m_Db || producer.IsNull())
        return out;

    // idx_provenance_producer makes this a fast index scan.
    const char* sql = "SELECT produced_guid FROM provenance WHERE producer_guid=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return out;
    BindGuidBlob(stmt, 1, producer);

    while (StepRow(stmt))
    {
        const GUID g = ColumnGuidBlob(stmt, 0);
        if (!g.IsNull())
            out.push_back(g);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<AssetDbCache_Sqlite::ProvenanceRow> AssetDbCache_Sqlite::EnumerateAllProvenance() const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    std::vector<ProvenanceRow> out;
    if (!m_Db)
        return out;

    const char* sql = "SELECT produced_guid, producer_guid, importer_id FROM provenance;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return out;

    while (StepRow(stmt))
    {
        ProvenanceRow row;
        row.Produced = ColumnGuidBlob(stmt, 0);
        row.Producer = ColumnGuidBlob(stmt, 1);
        row.ImporterId = ColumnText(stmt, 2);
        if (!row.Produced.IsNull() && !row.Producer.IsNull())
            out.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return out;
}

size_t AssetDbCache_Sqlite::UnregisterAllProducedBy(const GUID& producer, std::string* outError)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || producer.IsNull())
        return 0;

    const char* sql = "DELETE FROM provenance WHERE producer_guid=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        if (outError)
            *outError = SqliteError(m_Db);
        return 0;
    }
    BindGuidBlob(stmt, 1, producer);

    const bool ok = StepDone(stmt);
    const size_t changed = ok ? static_cast<size_t>(sqlite3_changes(m_Db)) : 0;
    if (!ok && outError)
        *outError = SqliteError(m_Db);
    sqlite3_finalize(stmt);
    if (changed > 0)
        m_MutationCount.fetch_add(1, std::memory_order_relaxed);
    return changed;
}

size_t AssetDbCache_Sqlite::RedirectProvenance(const GUID& from, const GUID& to, std::string* outError)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Db || from.IsNull() || to.IsNull() || from == to)
        return 0;

    // Both updates wrapped in a transaction so a redirect that touches
    // both columns (rare but possible — same GUID appearing as producer
    // and produced in different rows) is atomic.
    if (!Exec("BEGIN IMMEDIATE;", outError))
        return 0;

    size_t totalChanged = 0;
    bool ok = true;

    auto runUpdate = [&](const char* col) -> bool {
        std::string sql = std::string("UPDATE provenance SET ") + col + "=? WHERE " + col + "=?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        {
            if (outError)
                *outError = SqliteError(m_Db);
            return false;
        }
        BindGuidBlob(stmt, 1, to);
        BindGuidBlob(stmt, 2, from);
        const bool stepOk = StepDone(stmt);
        if (stepOk)
        {
            totalChanged += static_cast<size_t>(sqlite3_changes(m_Db));
        }
        else
        {
            // Most likely a UNIQUE/PK conflict on produced_guid: the redirect
            // target already has its own row, so we can't move the source row
            // onto the same key. The redirect itself succeeds (deps table)
            // but the user will observe stale provenance — surface it.
            const std::string err = SqliteError(m_Db);
            Logger::Log::Warning(
                "AssetDbCache_Sqlite::RedirectProvenance: UPDATE on {} from {} -> {} "
                "failed ({}); provenance row not migrated.",
                col, from.ToString(), to.ToString(), err);
            if (outError)
                *outError = err;
        }
        sqlite3_finalize(stmt);
        return stepOk;
    };

    // Order matters because produced_guid is the PK: if both `from` and `to`
    // already appear as produced_guid (highly unusual — would require the
    // redirect target to also have its own provenance row), the update could
    // hit a UNIQUE violation. In practice redirects retire old GUIDs that
    // shouldn't have a row at the target anymore, but we serialize the two
    // updates so the failure mode is at least diagnosable.
    if (ok) ok = runUpdate("produced_guid");
    if (ok) ok = runUpdate("producer_guid");

    if (!ok)
    {
        (void)Exec("ROLLBACK;", nullptr);
        return 0;
    }

    if (!Exec("COMMIT;", outError))
    {
        (void)Exec("ROLLBACK;", nullptr);
        return 0;
    }

    if (totalChanged > 0)
        m_MutationCount.fetch_add(1, std::memory_order_relaxed);
    return totalChanged;
}

} // namespace GameEngine::AssetDatabase


