#pragma once

#include "Types/Types.h"

#include <span>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{

// One [first, last] station index range per ~chunkLengthMetres of walked run
// distance, inclusive on both ends. Consecutive ranges SHARE their boundary
// station: the two chunk meshes then meet on identical rings and there is no
// crack between them. `runDistance` is the cumulative distance at each station
// and must be non-decreasing; a run shorter than one chunk yields a single
// range covering all of it, and fewer than two stations yield none.
//
// `maxChunks` caps how many ranges a hostile spline can mint. The cap folds
// the remainder into the FINAL range rather than dropping it, so a capped run
// still covers every station — the last chunk is just longer than the pitch.
[[nodiscard]] std::vector<std::pair<uint32, uint32>> CarveChunkRanges(
    std::span<const float32> runDistance, float32 chunkLengthMetres, uint32 maxChunks);

// What the extrude rebuild must do to one chunk slot. Slots are addressed BY
// CHUNK INDEX, never compacted: a chunk whose geometry came back empty leaves
// a HOLE rather than shifting every later chunk's entity and mesh key down by
// one, which would bind a chunk to its neighbour's mesh and strand the
// displaced key with nothing to release it.
enum class ChunkSlotAction : uint8
{
    // Retire whatever the slot holds — destroy the entity if it is alive,
    // release the mesh key if one is registered — and leave the slot empty:
    // either this stretch of the run generated nothing, or the run no longer
    // reaches this slot.
    Retire = 0,
    // Valid geometry, no live entity in the slot: register the mesh under the
    // slot's key and create the chunk entity.
    Create,
    // Valid geometry, live entity already in the slot: re-register the mesh in
    // place under the same key and update the entity's components.
    Update,
};

struct ChunkSlotPlan
{
    // One action per slot, sized to cover both the new chunks and the old tail
    // the new chunking no longer reaches.
    std::vector<ChunkSlotAction> Slots;
    // The new chunk count; every slot at or past it is a tail retirement.
    uint32 ChunkCount = 0;
};

// Pure index bookkeeping for the rebuild. slotHasLiveEntity describes what the
// previous build left behind (per old slot: nonzero = a chunk entity still
// alive in the world); chunkHasGeometry describes what the new build produced
// (per new chunk index: nonzero = valid geometry). uint8 flags rather than
// bool because std::vector<bool> cannot view as a span. The world and registry
// side effects stay with the caller.
[[nodiscard]] ChunkSlotPlan ReconcileChunkSlots(std::span<const uint8> slotHasLiveEntity,
                                                std::span<const uint8> chunkHasGeometry);

} // namespace GameEngine::Editor
