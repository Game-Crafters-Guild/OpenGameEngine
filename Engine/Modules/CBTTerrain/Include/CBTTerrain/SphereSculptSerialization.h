#pragma once

// SphereSculptSerialization — the .tsculpt blob codec for a planet's sphere sculpt page store
// (sparse virtual pages, dab + modifier authoring layers). The persistence twin of the planar
// zones' .tzone codec (TerrainECS/TerrainZonePayload.cpp): little-endian header + raw payload,
// round-trips exactly, and the decoder caps every header field before any allocation so a
// crafted blob cannot force an overflow or a huge reserve.
//
// v1 layout:
//   header  (5 x uint32): magic 'TSCP', version, virtualDim (the SAVED page grid),
//                         poolPageCount (the pool the save ran with — diagnostic; the loader
//                         restores into the LIVE pool), pageCount
//   per page: face, pageX, pageY, layerFlags (4 x uint32), then the present layers in order —
//             dab (kSculptPageTexels floats) when bit 0, modifier when bit 1. An all-zero
//             layer is skipped via the flags; an all-zero PAGE is never encoded at all.
//
// v2 (S4 adaptive page levels) is v1 with the page's LEVEL packed into layerFlags bits 8-9;
// a level-L page's layers are SculptFineDim(L)^2 floats. The ENCODER emits v1 whenever every
// page is level 0 — an unescalated store's .tsculpt bytes are byte-identical to pre-S4 (the
// dark-ship contract) and stay readable by older builds. The v2 DECODER reads v1 blobs as
// all-level-0 (the required back-read direction).
//
// Device-free like the layer itself, so the round-trip / rejection oracles run headless.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "CBTTerrain/SphereSculptLayer.h" // SphereSculptPageContent + SphereSculptGeometry

namespace GameEngine::CBTTerrain
{

// Layer-presence bits in a page record's layerFlags field; v2 packs the page level into
// bits 8-9 of the same field (kSculptMaxPageLevel fits — the mask is sized to it).
inline constexpr uint32_t kSculptBlobHasDab = 1u << 0;
inline constexpr uint32_t kSculptBlobHasModifier = 1u << 1;
inline constexpr uint32_t kSculptBlobLevelShift = 8u;
inline constexpr uint32_t kSculptBlobLevelMask = 0x3u << kSculptBlobLevelShift;

// Serialize a page-store snapshot (geometry + the allocated pages' authored layers, from
// SphereSculptLayer::ExportPages) to the .tsculpt blob. Pages whose layers are BOTH all-zero
// are skipped; a layer that is all-zero is skipped via the flags.
std::vector<uint8_t> EncodeSphereSculpt(const SphereSculptGeometry& geom,
                                        const std::vector<SphereSculptPageContent>& pages);

// Parse a .tsculpt blob. Returns false on a bad magic/version, an inconsistent grid
// (virtualDim not a whole page multiple / beyond the fixed page-table cap), a page key outside
// the grid, unknown layer flags, or a truncated buffer — outGeom/outPages are left unchanged.
// On success outGeom carries the SAVED grid (VirtualDim/PagesPerAxis/Cap + the saved
// poolPageCount) and outPages the decoded page contents (absent layers zero-filled).
bool DecodeSphereSculpt(const uint8_t* data, std::size_t size, SphereSculptGeometry& outGeom,
                        std::vector<SphereSculptPageContent>& outPages);

} // namespace GameEngine::CBTTerrain
