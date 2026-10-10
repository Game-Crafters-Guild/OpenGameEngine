#pragma once

#include "AssetCore/GUID.h"

#include <filesystem>
#include <string>

namespace GameEngine {

struct MaterialDocument;
struct EditorContext;

namespace Editor {

/** Where generated preview assets live, and the asset source alias they should
    register under. */
struct GraphPreviewCacheSource
{
    std::filesystem::path Root;
    std::string Alias;
};

GraphPreviewCacheSource ResolveGraphPreviewCacheSource(const EditorContext* ctx);

/** What SyncGraphPreviewMaterial should do with the material's shader. */
enum class GraphPreviewCompile
{
    /** Push the document's parameter values to the runtime. No shader work and
        no cache invalidation — the surface on disk has not moved. */
    PropertiesOnly,
    /** Reload the material and compile its base pipeline on a worker. The
        material stays uncompiled until its pipeline publishes on a later
        BeginFrame; a preview whose pipeline has not landed simply skips its
        draw. Never inline: a base-shader compile is hundreds of milliseconds,
        and the frame thread is what would pay them. */
    Shader,
};

/** Registers `matPath` if needed and brings its runtime material in line with
    `doc`. Returns the material's GUID, or a null GUID when it cannot resolve. */
GUID SyncGraphPreviewMaterial(const EditorContext* ctx, const std::filesystem::path& matPath,
                              const MaterialDocument& doc, GraphPreviewCompile compile,
                              const std::string& preferredSourceAlias);

/** Serializes `doc` to `matPath`, skipping the write when the content already
    matches — every write here wakes the file watcher into a hot-reload and
    thumbnail-invalidation round. */
bool WriteGraphPreviewMaterialFile(const std::filesystem::path& matPath,
                                   const MaterialDocument& doc);

} // namespace Editor
} // namespace GameEngine
