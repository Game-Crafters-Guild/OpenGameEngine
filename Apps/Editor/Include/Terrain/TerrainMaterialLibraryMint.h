#pragma once

#include "ECS/ECS.h" // EntityHandle by value

#include <cstdint>
#include <filesystem>
#include <string>

namespace GameEngine
{
class AssetManager;
namespace ECS { class World; }
namespace Editor { class UndoRedoService; }
} // namespace GameEngine

namespace GameEngine::Editor
{

// Minting a terrain's material library from the per-layer fields it already carries.
//
// A terrain authored before libraries existed shades from Terrain::Layer* — and the records that
// path produces are, field for field, the records a library minted from those same fields would
// produce (TerrainMaterialTableTests pins the equality). So the mint is not a change of
// appearance; it is the moment those four materials become nameable, editable and extendable.
//
// The write is where it becomes real, which is why this lives on the editor's save path rather
// than in extraction: an in-memory mint would reproduce records the legacy path already produces
// and change nothing a user could see.

// Mint a library for one terrain and bind it, writing `baseName`.terrainmatlib into `directory`
// (numeric suffixes disambiguate). Returns true when the terrain came out bound to a readable
// library; false — with no change to the terrain — when it already binds one.
//
// Never binds an empty or unreadable library: the file is written, re-read, and checked to hold
// the four entries before the component reference moves. A terrain that fails any of those steps
// is left exactly as it was, shading from its per-layer fields — losing an authored material list
// to a half-written file is strictly worse than not migrating.
bool MintTerrainMaterialLibraryFor(ECS::World& world, ECS::EntityHandle entity,
                                   const std::filesystem::path& directory,
                                   const std::string& baseName, AssetManager& assets,
                                   UndoRedoService* undo);

// Mint for every terrain in the world that binds no library, writing beside `scenePath`. Returns
// how many terrains were migrated. Called from the scene save so an unmigrated scene gains its
// libraries on first save, per the migration plan; a terrain that already binds one is untouched.
std::uint32_t MintMissingTerrainMaterialLibraries(ECS::World& world,
                                                  const std::filesystem::path& scenePath,
                                                  AssetManager& assets);

} // namespace GameEngine::Editor
