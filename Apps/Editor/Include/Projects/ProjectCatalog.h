#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine::Editor
{

// Where a catalog entry's project content comes from.
enum class ProjectSourceType : uint8_t
{
    None,        // no payload — creation just scaffolds an empty project
    Local,       // folder relative to the manifest's directory (staged templates)
    Git,         // repository URL, optionally a subfolder within it
    Zip,         // https .zip URL (or a local archive via Import), optionally a folder inside it
    Unsupported, // recognized manifest field but this build can't acquire it
};

struct ProjectCatalogSource
{
    ProjectSourceType Type = ProjectSourceType::None;
    std::string Url;  // git: repository URL
    std::string Ref;  // git: optional branch/tag; empty = remote HEAD
    std::string Path; // local: folder relative to manifest dir; git: subfolder = project root
};

struct ProjectCatalogEntry
{
    std::string Id;   // stable slug, unique within a manifest
    std::string Name; // display name
    std::string Description;
    std::string Author;
    std::string EngineVersion;
    int64_t Stars = -1; // optional static display value; <0 = unset
    // Resolved thumbnail reference: an http(s) URL or an absolute local path.
    // Empty = use the placeholder. The catalog service rewrites remote refs to
    // cached local files after download.
    std::string ThumbnailRef;
    // Card preview backdrop, 0xFFRRGGBB ("accentColor": "#RRGGBB"); 0 = unset,
    // the picker assigns one from its palette.
    uint32_t AccentColor = 0;
    // "thumbnailFit": "contain" letterboxes the art (icons); default covers.
    bool ThumbnailContain = false;
    ProjectCatalogSource Source;
};

// One parsed manifest. Serves both the staged templates catalog and the
// fetched community catalog — the schema is shared.
struct ProjectCatalog
{
    int SchemaVersion = 0;
    std::string Name;
    std::vector<ProjectCatalogEntry> Entries;
};

// Parse a manifest JSON document. thumbnailBase is the manifest's own location
// (directory path or URL prefix, no trailing separator needed) used to resolve
// relative thumbnail references; pass empty to keep them untouched.
// Entries missing id/name and duplicate ids are dropped. Unknown source types
// are kept as Unsupported so newer manifests degrade gracefully. Returns false
// only on malformed JSON / wrong document shape.
bool ParseProjectCatalog(const std::string& jsonText,
                         const std::string& thumbnailBase,
                         ProjectCatalog& outCatalog,
                         std::string* outError);

} // namespace GameEngine::Editor
