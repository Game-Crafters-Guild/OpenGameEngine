#pragma once

#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "AssetCore/SnapshotFingerprint.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace GameEngine::AssetDatabase
{

// Pre-existing alias: SnapshotFingerprint moved to AssetCore so consumers
// of AssetRegistry.h don't need to link AssetDatabase. New code should
// reference AssetCore::SnapshotFingerprint directly.
using SnapshotFingerprint = AssetCore::SnapshotFingerprint;

// Canonical durable record for an asset identity + user-authored metadata.
// - Paths are stored as canonical strings relative to the asset root using forward slashes.
// - Volatile/derived info (hashes, mtimes, extracted deps) lives in the derived cache.
struct AssetRecord
{
    GUID guid = GUID::Null();

    // Canonical path relative to asset root (generic string with '/')
    std::string path;

    // Authoritative programmable type id string. This is persisted as the JSON field "type".
    // It may be a built-in id like "Model" or a user-defined id like "MyPlugin.CustomType".
    // For built-in types, this typically matches AssetTypeToString(type).
    std::string typeId;

    // Best-effort persisted type. Callers should still be able to derive type from extension
    // when missing or unknown.
    AssetType type = AssetType::Unknown;

    // Tombstone: record exists but file is missing on disk.
    bool missing = false;

    // Arbitrary custom metadata (authoritative). Example keys used today:
    // - shader_stage, shader_entry, shader_defines
    std::unordered_map<std::string, std::string> kv;

    // Every field is persisted, so equal records serialize to byte-identical
    // journal lines. The store's re-journal suppression and the registry's
    // scan-upsert guards rely on this comparison staying total over the
    // struct's fields.
    bool operator==(const AssetRecord&) const = default;
};

// What a registrar observed about an asset whose file it has just seen.
// IAssetStore::MergeObservation folds it onto the stored row inside the store's
// own lock; a caller that reads a record, merges into it and upserts the result
// loses every field a concurrent registrar wrote in between.
//
// Unknown and empty mean "this registrar has no answer", never "the answer is
// nothing" — the stored value survives. Classification applies as a unit:
// `type` and `typeId` are one fact in two representations, and the file carries
// only `typeId`, so moving one without the other writes a record that
// disagrees with itself and serializes to the wrong answer.
struct AssetObservation
{
    GUID guid = GUID::Null();

    // Canonical path relative to the owning source's root; empty keeps the
    // stored path.
    std::string path;

    AssetType type = AssetType::Unknown;
    std::string typeId;
};

// Outcome of a merging write. Callers mark their source dirty only on Changed:
// a rescan re-observes every asset, and dirtying the unchanged ones costs a
// full journal rewrite on every flush.
enum class StoreMergeResult
{
    Failed,
    Unchanged,
    Changed,
};

struct RedirectRecord
{
    GUID from = GUID::Null();
    GUID to = GUID::Null();
};

} // namespace GameEngine::AssetDatabase


