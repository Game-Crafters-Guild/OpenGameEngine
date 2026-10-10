#pragma once

#include "TerrainECS/TerrainZonePayload.h"
#include "Types/Types.h"

namespace GameEngine::TerrainECS
{

// Axis-aligned world-space footprint of an auto-authored zone. Brush-created
// zones start world-aligned (identity rotation), so the payload maps directly
// to world XZ; the user's later rotation/scale still apply through the entity
// transform. Half-extents are in meters.
struct ZoneFootprint
{
    float32 CenterX = 0.0f;
    float32 CenterZ = 0.0f;
    float32 ExtentX = 0.0f;
    float32 ExtentZ = 0.0f;
};

// Sampling density and cap for auto-authored zones (design §3.2 auto-create /
// auto-grow up to a configurable cap). Defaults are conservative — a 64 m cap
// keeps a full zone's payload in the low-MB range at 2 texels/m.
struct ZoneAuthoringSettings
{
    float32 TexelsPerMeter = 2.0f; // payload resolution
    float32 Padding = 4.0f;        // skirt added around the brush footprint
    float32 MaxExtent = 64.0f;     // half-extent cap; crossing it starts a new zone
    uint32 MinDim = 8;
    uint32 MaxDim = 1024;
};

// Fit a fresh zone around the first brush dab: a square padded by the brush
// radius, each half-extent clamped to MaxExtent.
ZoneFootprint FitZoneToDab(float32 hitX, float32 hitZ, float32 brushRadius,
                           const ZoneAuthoringSettings& settings);

// Grow `footprint` to include a new dab (brush circle). Returns false when the
// grown footprint would exceed MaxExtent in either axis — the caller then
// starts a new zone; on false, `footprint` is left unchanged. Returns true (and
// mutates `footprint`) when the dab fits, whether or not growth was needed.
bool GrowZoneToDab(ZoneFootprint& footprint, float32 hitX, float32 hitZ,
                   float32 brushRadius, const ZoneAuthoringSettings& settings);

// Payload texel count along an axis for a half-extent at the given density,
// clamped to [MinDim, MaxDim]. Always odd-safe (>= MinDim).
uint32 PayloadDimForExtent(float32 extent, const ZoneAuthoringSettings& settings);

// Map a world XZ point to a fractional payload texel coordinate for an
// axis-aligned footprint (payload spans [-Extent, +Extent] across [0, dim-1]).
void WorldToPayloadTexel(const ZoneFootprint& footprint, uint32 width, uint32 height,
                         float32 worldX, float32 worldZ, float32& outTx, float32& outTz);

// Reallocate `old` for a grown footprint, copying existing texels to their new
// positions by matching world texel centers (design §3.2: "growth reallocates
// the payload and offsets existing texels"). Density is constant, so old texels
// land on an integer offset sub-block. Returns the new payload; DataVersion and
// dirty state are reset (the caller re-marks the dab's rect).
TerrainZonePayload RegrowPayload(const TerrainZonePayload& old,
                                 const ZoneFootprint& oldFootprint,
                                 const ZoneFootprint& newFootprint, uint32 newWidth,
                                 uint32 newHeight);

} // namespace GameEngine::TerrainECS
