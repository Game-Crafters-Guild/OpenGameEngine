#include "Assets/Packages/PackagePublish.h"

#include "AssetCore/PathNormalization.h"
#include "AssetDatabase/AssetStore_TextJsonl.h"
#include "Assets/AssetRegistry.h"
#include "Logger/Logger.h"

#include <filesystem>
#include <optional>
#include <system_error>

namespace GameEngine
{

namespace
{

// Store paths are platform-folded (lowercase on Windows/macOS, preserved on
// Linux); remap keys come from tooling that may preserve authored case. Match
// both through the registry's cross-platform key fold so the same logical
// path always pairs up.
std::string RemapMatchKey(std::string_view path)
{
    return AssetPaths::NormalizeForRegistryKey(
        AssetDatabase::AssetStore_TextJsonl::NormalizeCanonicalPath(std::string(path)));
}

} // namespace

PackagePublishResult PublishPackageAssetManifest(const AssetRegistry& registry,
                                                 std::string_view packageAlias,
                                                 const PackagePublishRemap& extractionRemap)
{
    PackagePublishResult result;

    // Locate + validate the mounted source.
    std::optional<AssetSourceDesc> source;
    for (const AssetSourceDesc& desc : registry.GetRegisteredSources())
    {
        if (desc.Alias == packageAlias)
        {
            source = desc;
            break;
        }
    }
    if (!source)
    {
        result.Errors.push_back("package '" + std::string(packageAlias) + "' is not a mounted source");
    }
    else if (source->IsImmutable || source->IsReadOnly)
    {
        result.Errors.push_back("package '" + std::string(packageAlias) +
                                "' is an immutable/read-only mount; publish from the mutable source package");
    }
    if (!result.Errors.empty())
    {
        Logger::Log::Error("PackagePublish: {}", result.Errors.front());
        return result;
    }
    result.ManifestPath = source->Root / ".assetmanifest";

    // Fold the remap keys once for case-robust matching.
    std::unordered_map<std::string, const PackagePublishRemapEntry*> remapByKey;
    remapByKey.reserve(extractionRemap.size());
    for (const auto& [rel, entry] : extractionRemap)
        remapByKey.emplace(RemapMatchKey(rel), &entry);
    std::unordered_map<std::string, size_t> remapHits; // matched fold-keys → count

    // Snapshot the package's current identity from its authoritative store.
    //
    // Disk is the authority for what is in the package, not the journal flag:
    // under derived identity the journal carries identity only and a vanished
    // file's tombstone rests in the per-machine cache (§5-A residence
    // contract), so `missing` reads false for a ghost and the record would sail
    // into the manifest. The manifest is git-committed and mounts as STORED
    // identity, where nothing revisits the question — so the existence check
    // belongs here, at the one step that decides what the contract says. The
    // journal flag stays in the predicate because for a stored mount it is that
    // mount's own answer, and a record it calls a tombstone is not publishable
    // identity even if a file has since reappeared at the path unregistered.
    const std::filesystem::path packageRoot = source->Root;
    AssetDatabase::AssetStore_TextJsonl manifest;
    std::unordered_map<GUID, std::string> emittedPathByGuid; // collision detection
    bool visited = registry.VisitSourceStoreRecords(
        packageAlias,
        [&](const AssetDatabase::AssetRecord& rec)
        {
            if (rec.guid.IsNull() || rec.path.empty() || rec.missing)
                return;

            std::error_code existsEc;
            if (!std::filesystem::is_regular_file(packageRoot / std::filesystem::path(rec.path),
                                                  existsEc))
            {
                ++result.VanishedCount;
                result.Warnings.push_back("'" + rec.path + "' has a record but no file in package '" +
                                          std::string(packageAlias) + "'; not published");
                return;
            }

            AssetDatabase::AssetRecord out = rec;
            const std::string matchKey = RemapMatchKey(rec.path);
            if (auto it = remapByKey.find(matchKey); it != remapByKey.end())
            {
                const GUID original = AssetRegistry::DeriveGuidForSourcePath(
                    it->second->OriginalAlias, it->second->OriginalRelPath);
                if (original.IsNull())
                {
                    result.Errors.push_back("remap for '" + rec.path + "' has invalid original identity '" +
                                            it->second->OriginalAlias + "/" + it->second->OriginalRelPath + "'");
                    return;
                }
                out.guid = original;
                ++remapHits[matchKey];
                ++result.RemappedCount;
            }

            if (auto [pos, inserted] = emittedPathByGuid.emplace(out.guid, out.path); !inserted)
            {
                result.Errors.push_back("GUID collision: '" + out.path + "' and '" + pos->second +
                                        "' both publish as " + out.guid.ToString());
                return;
            }

            (void)manifest.UpsertAsset(out, nullptr);
            ++result.EntryCount;
        });
    if (!visited)
    {
        result.Errors.push_back("package '" + std::string(packageAlias) +
                                "' has no authoritative store to publish from");
    }

    // Remap keys that matched nothing are almost certainly extraction-tooling
    // typos — surface them loudly instead of silently publishing wrong identity.
    for (const auto& [rel, entry] : extractionRemap)
    {
        if (!remapHits.contains(RemapMatchKey(rel)))
        {
            result.Warnings.push_back("remap entry '" + rel + "' matched no asset in package '" +
                                      std::string(packageAlias) + "'");
        }
    }
    for (const std::string& warning : result.Warnings)
        Logger::Log::Warning("PackagePublish: {}", warning);

    if (!result.Errors.empty())
    {
        for (const std::string& error : result.Errors)
            Logger::Log::Error("PackagePublish: {}", error);
        Logger::Log::Error("PackagePublish: aborted publish of '{}' — manifest not written",
                           result.ManifestPath.generic_string());
        return result;
    }

    // Fresh store + never-loaded file → SaveToFile takes the compact-snapshot
    // path: a deterministic, GUID-sorted full rewrite. Same state in → same
    // bytes out, which is what makes republishing idempotent.
    std::string err;
    if (!manifest.SaveToFile(result.ManifestPath, &err))
    {
        result.Errors.push_back("failed to write '" + result.ManifestPath.generic_string() + "': " + err);
        Logger::Log::Error("PackagePublish: {}", result.Errors.back());
        return result;
    }

    result.Success = true;
    Logger::Log::Info("PackagePublish: published '{}' ({} entries, {} remapped to original identity, "
                      "{} vanished records dropped)",
                      result.ManifestPath.generic_string(), result.EntryCount, result.RemappedCount,
                      result.VanishedCount);
    return result;
}

} // namespace GameEngine
