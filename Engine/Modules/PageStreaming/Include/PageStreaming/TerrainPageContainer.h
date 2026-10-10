#pragma once

#include "PageStreaming/PageStoreFormat.h"

#include <filesystem>
#include <span>
#include <string>

namespace GameEngine::PageStreaming
{

class PageStoreReader;

// The shipped form of a terrain's page stores (.geterrain): one file per terrain holding every
// field's store, so a packaged game ships one cooked container per terrain and never loose page
// files or the source. Little-endian: "GETR", the format version, the field count, then per field
// its kind, the offset of its store from the start of the container and the store's size; then
// the stores, each byte-for-byte as its .gepage, so a field is read at its offset unchanged.

/// One field to pack: its kind and its cooked store.
struct TerrainContainerField
{
    PageFieldKind Kind = PageFieldKind::Height;
    std::filesystem::path Store;
};

/// Packs `fields` (at most one per kind) into a container at `target`, assembled beside it and
/// published by rename. Returns an empty string, else the reason.
std::string WriteTerrainContainer(const std::filesystem::path& target, std::span<const TerrainContainerField> fields);

/// Opens the `kind` field of the container at `container` into `out`. Returns an empty string,
/// else the reason (a container without that field names the field).
std::string OpenTerrainContainerField(const std::filesystem::path& container, PageFieldKind kind,
                                      PageStoreReader& out);

} // namespace GameEngine::PageStreaming
