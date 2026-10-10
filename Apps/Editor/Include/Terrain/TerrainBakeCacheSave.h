#pragma once

#include <filesystem>
#include <optional>

namespace GameEngine
{
class AssetRegistry;
namespace ECS { class World; }
} // namespace GameEngine

namespace GameEngine::Editor
{

// The scene save's terrain step for the baked-terrain cache (TerrainBakeCache.h): the
// document's own terrains move from `previousScenePath` (the document's path before this
// save; none for a first save) to the saved scene's identity, terrains from a subscene or a
// blueprint keep theirs, and the modifier system stores each of the document's single
// terrains under its current key and prunes the saved scene's cache folder to them
// (TerrainModifierSystem::RequestBakeCacheStore). Called after the scene file is written,
// so a Save As and a first save name the new scene. Does nothing without a running
// modifier system.
void RequestTerrainBakeStoreForSave(ECS::World& world, const std::optional<std::filesystem::path>& previousScenePath,
                                    const std::filesystem::path& scenePath, AssetRegistry& registry);

} // namespace GameEngine::Editor
