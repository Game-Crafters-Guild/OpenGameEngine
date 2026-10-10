#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GameEngine
{

class AssetRegistry;

// Original identity to publish for a package file that was moved from another
// source (the extraction flow). The manifest entry for the package path gets
// GUID = AssetRegistry::DeriveGuidForSourcePath(OriginalAlias, OriginalRelPath)
// — the GUID the file had under its pre-extraction mount — instead of the
// package's own current GUID, so existing scene references keep resolving.
struct PackagePublishRemapEntry
{
    std::string OriginalAlias;   // e.g. "project"
    std::string OriginalRelPath; // e.g. "textures/eztree/leaf.png"
};

// Keyed by the canonical package-relative path of the file's NEW location
// (forward slashes; matched case-insensitively against the package store).
using PackagePublishRemap = std::unordered_map<std::string, PackagePublishRemapEntry>;

struct PackagePublishResult
{
    bool Success = false;
    size_t EntryCount = 0;    // records written to the manifest
    size_t RemappedCount = 0; // records whose GUID came from the remap
    size_t VanishedCount = 0; // records dropped because their file is not on disk
    std::filesystem::path ManifestPath;
    std::vector<std::string> Errors;   // loud failures (collision, IO, bad mount)
    std::vector<std::string> Warnings; // e.g. remap keys that matched no package asset
};

// Publish (or republish) `<packageAssetsRoot>/.assetmanifest` for a MOUNTED
// MUTABLE package: snapshots the package's current identity ({guid, path,
// type, kv} per asset, vanished files excluded) from its authoritative store
// and writes it as a stored-identity manifest — the same AssetStore_TextJsonl
// schema the build pipeline stages and MakePackageMount consumes.
//
// - GUIDs are emitted VERBATIM (current identity), except paths named in
//   `extractionRemap`, which emit their pre-extraction derived GUID instead.
//   A republish of an already-published package needs no remap: the mount
//   merged the manifest GUIDs into the working store, so "current" identity
//   already IS the published identity.
// - Idempotent: same package state → byte-identical manifest (deterministic
//   GUID-sorted snapshot rewrite).
// - Loud on collision: two package files emitting the same GUID (e.g. a bad
//   remap) fails the publish with no file written.
// - A record whose file is not on disk is dropped with a warning, never
//   published: the manifest is a committed identity contract that ships to
//   other machines, and nothing downstream re-checks it (the mount-time merge
//   trusts every entry), so publish is the only place a dangling entry can be
//   caught.
// - Call after the package's startup scan completed (WaitForStartupScan) so
//   freshly-added files are in the store.
//
// The target package must be mounted and mutable (not IsImmutable/IsReadOnly):
// shipped and git-cache mounts are published artifacts, not publish sources.
PackagePublishResult PublishPackageAssetManifest(const AssetRegistry& registry,
                                                 std::string_view packageAlias,
                                                 const PackagePublishRemap& extractionRemap = {});

} // namespace GameEngine
