#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine {

// Manifest entry from the Polyhaven API: one asset listing.
struct PolyhavenManifestEntry
{
    std::string slug;
    std::string name;
    std::string thumbUrl;
};

// Result of materializing a batch of manifest entries into local thumbnail paths.
struct PolyhavenLoadResult
{
    std::vector<std::pair<std::filesystem::path, std::string>> pathAndLabels;
    std::string typeKey;
};

// Service encapsulating all Polyhaven API interaction, asset downloading,
// thumbnail caching, and download verification. All network methods are
// blocking and intended to be called from background threads.
class PolyhavenService
{
public:
    // ---------- virtual path helpers ----------

    // Parse "polyhaven://type/category" into outType and outCategory.
    static bool ParseVirtualPath(const std::filesystem::path& p,
                                 std::string& outType,
                                 std::string& outCategory);

    // "hdris" -> "Polyhaven HDRI", "textures" -> "Polyhaven Texture", etc.
    static std::string TypeKeyForType(const std::string& type);

    // ---------- API (blocking, call from background threads) ----------

    struct CategoriesResult
    {
        std::vector<std::pair<std::filesystem::path, std::string>> pathAndLabels;
        std::string typeKey;
    };

    struct ManifestResult
    {
        std::vector<PolyhavenManifestEntry> manifest;
        std::string typeKey;
    };

    // Fetch category list for a type ("hdris", "textures", "models").
    static CategoriesResult FetchCategories(const std::string& type);

    // Fetch full asset manifest for a type + optional category filter.
    static ManifestResult FetchManifest(const std::string& type,
                                        const std::string& categoryFilter);

    // Download thumbnails for manifest[startIndex..endIndex) into temp cache.
    static PolyhavenLoadResult MaterializeBatch(
        const std::vector<PolyhavenManifestEntry>& manifest,
        size_t startIndex, size_t endIndex,
        const std::string& typeKey);

    // Temp directory for thumbnail cache.
    static std::filesystem::path GetCacheDir();

    // ---------- downloading ----------

    // Download asset files (model+textures, HDR, or texture) to destDir.
    // Blocking. Returns path to main file, or empty on failure.
    // onProgress (optional) reports file-count progress as (filesDone, filesTotal)
    // from the download thread: one call per file (the main glTF plus each
    // included buffer/texture), with total known once the file list is fetched.
    static std::filesystem::path DownloadAsset(const std::string& slug,
                                               const std::string& type,
                                               const std::filesystem::path& destDir,
                                               const std::function<void(size_t, size_t)>& onProgress = {});

    // Download a specific HDRI resolution to destDir as <slug>_<resolution>.hdr.
    static std::filesystem::path DownloadHDRI(const std::string& slug,
                                              const std::filesystem::path& destDir,
                                              const std::string& resolution);

    static std::string GetPreferredHDRIResolution();

    // Find the main downloaded file for a slug under assetsRoot/Polyhaven/<slug>/.
    // Returns the path to the first complete matching file (.gltf, .glb, .hdr,
    // .png, .jpg), or empty if none is ready. A .gltf is complete only when all
    // referenced buffers and images exist and are non-empty.
    static std::filesystem::path FindDownloadedFile(const std::string& slug,
                                                     const std::filesystem::path& assetsRoot);

    static std::filesystem::path FindDownloadedHDRI(const std::string& slug,
                                                    const std::filesystem::path& assetsRoot,
                                                    const std::string& resolution);

    static std::filesystem::path FindOrMoveDownloadedHDRI(const std::string& slug,
                                                          const std::filesystem::path& assetsRoot,
                                                          const std::string& resolution);

    // Find the main completed file in the temp cache downloads directory.
    static std::filesystem::path FindCachedDownloadFile(const std::string& slug);

    static std::filesystem::path FindCachedDownloadHDRI(const std::string& slug,
                                                        const std::string& resolution);

    // Move all regular files from srcDir to destDir (creating it if needed).
    // Returns the new path of mainFile after the move, or empty on failure.
    static std::filesystem::path MoveDownloadToProject(const std::filesystem::path& mainFile,
                                                        const std::filesystem::path& srcDir,
                                                        const std::filesystem::path& destDir);

    // Download specific PBR texture maps for a texture asset.
    // If mapType is empty, downloads all available maps.
    // mapType can be: "Diffuse", "nor_gl", "Rough", "Metallic", "AO", "Displacement", "arm"
    // Blocking. Returns true if at least one map was downloaded successfully.
    static bool DownloadTextureMaps(const std::string& slug,
                                    const std::filesystem::path& destDir,
                                    const std::string& mapType);

    // ---------- download verification ----------

    // Check whether an asset slug has been fully downloaded (main file + all
    // referenced buffers/textures for glTF). Uses an internal cache that can
    // be cleared with ClearDownloadCache().
    bool IsFullyDownloaded(const std::string& slug,
                           const std::filesystem::path& assetsRoot);

    void ClearDownloadCache() { m_DownloadedCache.clear(); }

private:
    std::unordered_map<std::string, bool> m_DownloadedCache;
};

} // namespace GameEngine
