#pragma once

// SphereSculptSaver — persists the active planet's sphere sculpt page store (dab + modifier
// virtual pages) to a .tsculpt sidecar next to the scene, the sphere analogue of
// TerrainZonePayloadSaver's .tzone flush. Same modes, same atomic write, same
// mint-and-swap-to-file-backed-GUID on explicit Save, same backup-side staging contract
// (existing file only, ledger-deduped, live bytes untouched by the autosave timer).

#include "Terrain/TerrainZonePayloadSaver.h" // ZonePayloadFlushMode + ZonePayloadStagingLedger

namespace GameEngine
{
class AssetRegistry;
namespace ECS { class World; }
namespace TerrainECS { class TerrainService; }
} // namespace GameEngine

namespace GameEngine::Editor
{

// Persist the sphere sculpt blob when the service reports unsaved sculpt edits and the world
// holds an enabled spherical Terrain. Save modes write the live .tsculpt (minting
// `<sceneStem>_Zones/<guid>.tsculpt` on first save and swapping the component's
// sphereSculpt ref to the file-backed GUID); BackupExistingOnly stages a `.backup` sibling
// of an EXISTING file only, deduped per sculpt version via the ledger. Returns the number
// of files written (0 or 1 — one planet, one payload).
uint32_t FlushDirtySphereSculpt(ECS::World& world, const std::filesystem::path& scenePath,
                                AssetRegistry& registry, TerrainECS::TerrainService& service,
                                ZonePayloadFlushMode mode,
                                ZonePayloadStagingLedger* stagingLedger = nullptr);

} // namespace GameEngine::Editor
