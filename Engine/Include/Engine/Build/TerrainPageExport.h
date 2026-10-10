#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine
{

class AssetRegistry;
struct AssetManifest;

/// What the export did with one shipped heightmap.
struct TerrainPageExportResult
{
    std::vector<std::filesystem::path> Containers; ///< <contentRoot>/Assets/Cooked/Terrain/<guid>.geterrain, one per heightmap
    std::vector<std::string> Warnings;             ///< heightmaps that could not be cooked, each with the fix
};

/// Packs the cooked page stores of every terrain heightmap the package ships into one terrain
/// container per heightmap, <contentRoot>/Assets/Cooked/Terrain/<heightmap guid>.geterrain, under the
/// packaged asset root the Player reads it from, like the other cooked data a package ships (design
/// D5: the shipped form is one cooked container per terrain, never loose page files). Each heightmap's store
/// comes from the project's cache (<projectRoot>/.Cache/TerrainPages), cooked there first when the
/// heightmap or its import grid changed since the last cook. Two terrains on one heightmap share its
/// container, as they share its store. Raw heightmaps (.r16, .r32) only: a 16-bit PNG heightmap is a
/// texture asset and keeps its texture path. Raising `cancel` stops a cook in progress and every
/// heightmap after it; the stopped ones are reported as warnings.
TerrainPageExportResult ExportTerrainPages(const std::filesystem::path& projectRoot,
                                           const std::filesystem::path& contentRoot, const AssetManifest& manifest,
                                           const AssetRegistry& registry, JobSystem::WorkStealingThreadPool* pool,
                                           const std::atomic<bool>* cancel);

} // namespace GameEngine
