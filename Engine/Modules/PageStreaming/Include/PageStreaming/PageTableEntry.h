#pragma once

#include "Types/Types.h"

namespace GameEngine::PageStreaming
{

// One page-table entry: the physical-cache slot holding a page in the low 24 bits and the page's
// level in the top byte, or kNoPage. The planet sculpt's page table packs its entries the same way
// (CBTTerrain/CBTLayout.h, kSculptPageIdMask, kSculptPageLevelShift, kSculptNoPage); only the
// bit layout and the sentinel are common: in the sculpt the level is a refinement of one page,
// here it is the pyramid level the slot holds. Test kNoPage first: its top byte is not a level.

/// The entry of a page with no resident slot.
inline constexpr uint32 kNoPage = 0xFFFFFFFFu;
/// The slot bits of an entry.
inline constexpr uint32 kPageSlotMask = 0x00FFFFFFu;
/// The shift of the level byte of an entry.
inline constexpr uint32 kPageLevelShift = 24u;

/// The entry for `slot` (below 2^24) holding a page of `level` (below 255).
constexpr uint32 PackPageEntry(uint32 slot, uint32 level)
{
    return (slot & kPageSlotMask) | (level << kPageLevelShift);
}

/// The slot of an entry that is not kNoPage.
constexpr uint32 PageEntrySlot(uint32 entry)
{
    return entry & kPageSlotMask;
}

/// The level of an entry that is not kNoPage.
constexpr uint32 PageEntryLevel(uint32 entry)
{
    return entry >> kPageLevelShift;
}

} // namespace GameEngine::PageStreaming
