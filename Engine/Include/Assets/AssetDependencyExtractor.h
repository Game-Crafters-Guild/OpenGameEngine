#pragma once

#include "AssetCore/GUID.h"
#include "AssetCore/AssetTypes.h"

#include <string>
#include <vector>

namespace GameEngine
{

class AssetRegistry;
struct AssetMetadata;

// Asset dependency information produced by the syntactic dependency extractor
// (regex-based scan of asset file content). Used by the registry's fallback
// path for asset types whose parsers don't implement ExtractDependencies.
struct AssetDependencyInfo
{
    GUID AssetGuid;
    AssetType Type;
    std::vector<GUID> Dependencies;
    std::vector<std::string> DependencyPaths; // For debugging / phase-2 resolution.

    AssetDependencyInfo(const GUID& guid, AssetType type)
        : AssetGuid(guid), Type(type) {}
};

// Syntactic-fallback dependency extractor. Reads the asset file as text and
// regex-scans for GUID-shaped tokens + path-shaped tokens. The registry uses
// this for asset types whose AssetParser doesn't override ExtractDependencies.
// A binary asset type (IsBinaryAssetType) is not read and has no dependencies.
//
// This is purely textual — no schema awareness — so the path patterns are
// shaped to match common asset file content (textures, materials, models,
// audio, UI). For .scene / .blueprint files an extra pass handles the INI
// schema's `path="..."` / `@"path"` forms.
class AssetDependencyExtractor
{
public:
    // Phase 1: scan the file at metadata.Path for GUIDs + path references.
    // Safe to use without a live registry (e.g. tools, tests).
    static AssetDependencyInfo ExtractDependencies(const AssetMetadata& metadata,
                                                   const AssetRegistry* registry = nullptr);

    // Phase 2: resolve the path references collected in phase 1 to GUIDs by
    // looking them up in the live registry (rename-safe). A path with a source
    // prefix ("project:...", "editor:...", "<package>:...") resolves in that
    // source only; a bare relative path across the mounts. A path holding a control
    // character resolves to nothing. Mutates info in place.
    static void ResolveDependencyPathsToGuids(AssetDependencyInfo& info,
                                              const AssetMetadata& metadata,
                                              AssetRegistry& registry);

    static std::vector<GUID> ExtractGUIDReferences(const std::string& content);
    static std::vector<std::string> ExtractPathReferences(const std::string& content);
};

} // namespace GameEngine
