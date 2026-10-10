#include "Engine/Build/TerrainPageExport.h"

#include "Engine/Build/AssetCollector.h"

#include "PageStreaming/HeightStoreCache.h"
#include "PageStreaming/TerrainPageContainer.h"
#include "TerrainECS/RawHeightmap.h"

namespace GameEngine
{

TerrainPageExportResult ExportTerrainPages(const std::filesystem::path& projectRoot,
                                           const std::filesystem::path& contentRoot, const AssetManifest& manifest,
                                           const AssetRegistry& registry, JobSystem::WorkStealingThreadPool* pool,
                                           const std::atomic<bool>* cancel)
{
    TerrainPageExportResult result;
    const PageStreaming::HeightStoreCache cache(projectRoot / ".Cache" / "TerrainPages");
    for (const AssetManifestEntry& entry : manifest.entries)
    {
        if (entry.type != AssetType::TerrainHeightmap)
            continue;
        if (cancel && cancel->load())
        {
            result.Warnings.push_back("Terrain pages: the export was cancelled before " +
                                      entry.sourcePath.filename().string());
            continue;
        }
        const std::string name = entry.sourcePath.filename().string();
        // The grid the terrain decodes the file on: the import settings, one of them, or a square file.
        TerrainECS::RawHeightmapLayout declared;
        TerrainECS::RawHeightmapLayout grid;
        std::string reason = TerrainECS::ResolveDeclaredRawHeightmapLayout(
            TerrainECS::ReadRawHeightmapSettings(registry, entry.sourcePath), declared);
        if (reason.empty())
            reason = TerrainECS::ResolveRawHeightmapFileLayout(entry.sourcePath, declared.Width, declared.Height, grid);
        if (!reason.empty())
        {
            result.Warnings.push_back("Terrain pages: " + name + ": " + reason);
            continue;
        }
        const PageStreaming::HeightStoreCache::Result store =
            cache.Ensure(entry.guid, entry.sourcePath, grid.Width, grid.Height, pool, nullptr, cancel);
        if (!store.Error.empty())
        {
            result.Warnings.push_back("Terrain pages: " + name + ": " + store.Error);
            continue;
        }
        const std::filesystem::path container =
            contentRoot / "Assets" / "Cooked" / "Terrain" / (entry.guid.ToString() + ".geterrain");
        const PageStreaming::TerrainContainerField field{PageStreaming::PageFieldKind::Height, store.Store};
        if (reason = PageStreaming::WriteTerrainContainer(container, {&field, 1}); !reason.empty())
        {
            result.Warnings.push_back("Terrain pages: " + name + ": " + reason);
            continue;
        }
        result.Containers.push_back(container);
    }
    return result;
}

} // namespace GameEngine
