#pragma once

#include "AssetCore/GUID.h"

#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace GameEngine::ECS { class World; }
namespace GameEngine { class AssetRegistry; }
namespace GameEngine::TerrainECS { class TerrainService; }

namespace GameEngine::Editor
{

// How a flush treats a zone whose PayloadRef is not yet backed by a .tzone file.
enum class ZonePayloadFlushMode
{
    // Explicit Save / SaveAs: mint a .tzone for a not-yet-file-backed zone,
    // register it, and swap the component's PayloadRef to the file-derived GUID
    // so a reload resolves it (the project source uses derived identity, so the
    // brush's random author-time GUID would not). The definitive persist — the
    // ONLY path that writes live .tzone bytes. Also deletes the zone's staged
    // autosave sibling: after a Save it would be stale (older than the live
    // file), and a later crash must not offer it as "newer" work.
    SaveMintAndSwap,

    // Autosave backup: stage bytes to a `<live>.tzone.backup` sibling for zones
    // whose PayloadRef already resolves to a .tzone file. Never touches the live
    // file (so close-without-saving genuinely discards strokes — the Option B
    // recovery flow promotes the staged sibling only when the user accepts),
    // never mints a file, and never mutates a live component from the timer. A
    // zone created since the last Save is left for the next explicit Save.
    // NeedsSave stays set — the live bytes are still unwritten.
    BackupExistingOnly,
};

// Autosave staging bookkeeping: the payload DataVersion each zone was last
// staged at. Staging is idempotent per stroke — an idle timer tick re-stages
// nothing. Owned by the scene document (reset on document load) because the
// staged files live beside that document's zones.
struct ZonePayloadStagingLedger
{
    std::unordered_map<GUID, uint64_t> StagedDataVersions;
    // The sphere sculpt version last staged by the autosave timer (0 = never) —
    // the planet's single-payload twin of the per-zone map above.
    uint64_t StagedSphereSculptVersion = 0;
};

// Write bytes to a sibling temp file then atomically rename over the target, removing the
// temp on any failure — a crash mid-write can never truncate the last good file. Shared by
// the zone (.tzone) and sphere sculpt (.tsculpt) flushes.
bool WriteBlobAtomic(const std::filesystem::path& target, const std::vector<uint8_t>& bytes);

// Persist dirty terrain zone payloads (brush-authored sculpt/paint data) to
// .tzone files next to the scene. Each write is atomic (temp + rename) so a
// crash mid-write cannot truncate the last good file. `mode` selects whether an
// unsaved zone may be minted + ref-swapped (Save) or is skipped (backup);
// backup mode requires `stagingLedger` and stages backup-side only.
// Returns the number of payloads written.
uint32_t FlushDirtyZonePayloads(ECS::World& world, const std::filesystem::path& scenePath,
                                AssetRegistry& registry, TerrainECS::TerrainService& service,
                                ZonePayloadFlushMode mode,
                                ZonePayloadStagingLedger* stagingLedger = nullptr);

} // namespace GameEngine::Editor
