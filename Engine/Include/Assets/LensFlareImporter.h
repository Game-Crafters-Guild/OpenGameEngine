#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Importer that transcodes externally-authored lens-flare data into the
// engine-native JSON formats (.flareatlas / .lensflare). This is the ONLY place
// that understands the external authoring formats:
//   * Atlas: TexturePacker JSON ("frames" map of pixel rects + "meta.size").
//   * Flare: a custom, near-JSON document (a "meta" block with an "Elements"
//     map). The flare format is not strict JSON (it omits some commas), so a
//     tolerant pre-pass is applied before parsing.
//
// The runtime never sees the external formats — it only reads what these
// functions write.
namespace GameEngine::LensFlareImport {

// Read a TexturePacker-style atlas description and write an engine .flareatlas.
// `textureRef` is the GUID or mount-relative path of the already-imported atlas
// texture, stored verbatim in the output. Returns false and sets `outError`
// (when non-null) on failure.
bool ImportAtlas(const std::filesystem::path& sourceTexturePackerJson,
                 const std::string& textureRef,
                 const std::filesystem::path& outFlareAtlasPath,
                 std::string* outError = nullptr);

// Read an external flare document and write an engine .lensflare. `atlasRef` is
// the GUID or mount-relative path of the .flareatlas the flare's sprites belong
// to. `atlasSpriteNames` (optional, in atlas frame order) resolves legacy
// elements that carry only a numeric elementTextureID instead of a SpriteName.
// Returns false and sets `outError` (when non-null) on failure.
bool ImportFlare(const std::filesystem::path& sourceFlareDocument,
                 const std::string& atlasRef,
                 const std::filesystem::path& outLensFlarePath,
                 std::string* outError = nullptr,
                 const std::vector<std::string>* atlasSpriteNames = nullptr);

// One line of an ImportFolder report: what a source file became, or why it
// was skipped/failed.
struct FolderImportEntry
{
    std::filesystem::path Source;
    std::filesystem::path Output; // empty when skipped/failed
    std::string Note;             // "atlas", "flare (MegaAtlas)", or the error
    bool Ok = false;
};

// Batch-import every ProFlares-style document under `sourceDir` (recursive):
// TexturePacker atlas .txts (paired with their same-basename .png, copied next
// to the output) become .flareatlas; flare .txts become .lensflare, each bound
// to the imported atlas that covers the most of its sprite names. Outputs land
// in `outputDir` (created if needed) with whitespace stripped from filenames.
// Returns the number of assets written; per-file details in `outReport`.
int ImportFolder(const std::filesystem::path& sourceDir,
                 const std::filesystem::path& outputDir,
                 const std::string& outputMountPrefix,
                 std::vector<FolderImportEntry>& outReport);

} // namespace GameEngine::LensFlareImport
