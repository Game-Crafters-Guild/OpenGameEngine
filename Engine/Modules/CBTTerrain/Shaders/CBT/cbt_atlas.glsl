// cbt_atlas.glsl — resident-window atlas resolve, the delicate indirection layer
// the atlas slice adds between CBT's terrain UV and the height/splat/normal texel.
//
// LOCKSTEP with the CPU mirror AtlasHeightSampler / AtlasGeometry
// (Engine/Modules/TerrainECS/.../TerrainAtlas.{h,cpp}). The CPU mirror is what
// the arc-law oracles (sampling parity, shared-edge continuity, out-of-window
// fallback) validate — exactly the CBTPlanetShading.h <-> cbt_domain.glsl
// discipline — so these functions MUST stay bit-for-bit equivalent to it:
//   * texel-centered bilinear: the CPU works in absolute texel space (integer
//     texel = the texel centre); the GLSL feeds a linear-clamp sampler a
//     normalized UV = (absTexel + 0.5) / atlasDim, which reproduces that bilinear.
//   * the apron (CBT_ATLAS_APRON = 1 texel/side, so slotStride = tileRes + 2)
//     absorbs the hardware-bilinear straddle at a slot's interior edge, so a
//     clamp sampler never bleeds into the neighbouring slot.
//
// Pure math only — NO bindings, NO extensions — so it compiles standalone in the
// compute kernel and (Phase E) in the surface. The texture/SSBO taps stay in the
// consumer; this file only resolves UV -> tile -> slot -> atlas UV. The one-line
// consumer swap (design §3.3), once the indirection SSBO + atlas texture are
// bound (Phase E editor proof), is:
//
//   AtlasResolve r = CBT_AtlasResolve(terrainUV, params, gIndirection.Rows);
//   float h = r.Resident
//       ? textureLod(gAtlasHeight, r.SlotUV, 0.0).r      // one indirection, one tap
//       : CBT_AtlasCoarseSample(terrainUV, coarseDim, s); // out-of-window coarse field (binding 19)
#ifndef CBT_ATLAS_GLSL
#define CBT_ATLAS_GLSL

// Mirror kAtlasNoSlot / kAtlasApron / kAtlasMaxTiles in AtlasSlotPool.h / TerrainAtlas.h / CBTLayout.h.
// CBT_ATLAS_MAX_TILES lives HERE (the file both the compute layout and the surface include) so the
// indirection ring's rows-per-slot cap is single-sourced across both GPU consumers; the C++<->GLSL
// value is locked by CBTLayoutTests.GlslAtlasMaxTilesMatchesCpp (parses this line).
const uint CBT_ATLAS_NO_SLOT = 0xFFFFFFFFu;
const uint CBT_ATLAS_APRON = 1u;
const uint CBT_ATLAS_MAX_TILES = 16384u;

// std430 indirection row (mirror TileAtlasSlot, 16 B). One per tile in the whole
// terrain, indexed by tileIndex = tileZ * tilesPerAxisX + tileX. Bound as an
// SSBO by the consumer; declared here only so the layout is single-sourced.
struct CBTTileAtlasSlot
{
    uint Slot;       // atlas slot index, or CBT_ATLAS_NO_SLOT (not resident)
    // RESERVED, and stays that way: no GPU consumer reads Generation. The stale-row hazard it
    // would guard is already prevented by the indirection ring + slot quarantine >= frames-in-flight
    // + single-queue serialization (full proof at cbt_layout.glsl binding 17). Kept only so the
    // SSBO layout is final if a future multi-queue path ever needs the check.
    uint Generation;
    float LodBias;   // coarse tile hint (never a correctness input to the sample)
    // Coarse->slot upgrade crossfade in [0,1]: 0 = show the out-of-window coarse field, 1 = show the
    // resident slot at full detail. Ramped CPU-side by AtlasResidencyController on tile assign; the
    // surface mixes coarse<->slot normal/splat by it so a newly-resident tile fades in instead of
    // popping. Not read by the height resolve (fragment-only). Occupies the old std430 pad word.
    float Fade;
};

// Terrain UV [0,1]^2 -> the normalized UV at which a linear-clamp sampler reads a terrain LATTICE
// texture at that point. Every planar height, normal and splat texture is a lattice: sample i sits
// at terrain UV i / (dim - 1), the first and last samples on the terrain's edges, which is where the
// CPU height field puts them (HeightfieldData::SampleBilinear). Fed the raw terrain UV, the sampler
// would place sample i at (i + 0.5) / dim instead, shifting the surface by (0.5 - u) of a lattice
// cell. dim is the texture's size in texels.
vec2 CBT_LatticeTextureUV(vec2 terrainUV, vec2 dim)
{
    return (clamp(terrainUV, 0.0, 1.0) * (dim - 1.0) + 0.5) / dim;
}

// Atlas geometry the consumer passes in (mirror AtlasGeometry). Small POD; the
// consumer fills it from the frame params so no hardcode leaks in.
struct CBTAtlasParams
{
    uint TileRes;       // interior samples per tile axis
    uint SlotStride;    // TileRes + 2*CBT_ATLAS_APRON
    uint SlotsPerRow;   // slots per atlas row
    uint AtlasDim;      // SlotsPerRow * SlotStride (square)
    uint TilesPerAxisX;
    uint TilesPerAxisZ;
};

// Terrain UV [0,1]^2 -> tile grid coord + tile-local UV [0,1]. The right/bottom
// tile owns a shared boundary UV (localUV = 0), matching the CPU mirror and
// SampleTiledHeightNormalized.
void CBT_AtlasTile(vec2 terrainUV, CBTAtlasParams p, out ivec2 tile, out vec2 localUV)
{
    vec2 clamped = clamp(terrainUV, 0.0, 1.0);
    vec2 tf = clamped * vec2(float(p.TilesPerAxisX), float(p.TilesPerAxisZ));
    tile.x = clamp(int(floor(tf.x)), 0, int(p.TilesPerAxisX) - 1);
    tile.y = clamp(int(floor(tf.y)), 0, int(p.TilesPerAxisZ) - 1);
    localUV = clamp(tf - vec2(float(tile.x), float(tile.y)), 0.0, 1.0);
}

uint CBT_AtlasTileIndex(ivec2 tile, CBTAtlasParams p)
{
    return uint(tile.y) * p.TilesPerAxisX + uint(tile.x);
}

// Slot index + tile-local UV -> normalized atlas UV (texel-centered) for a
// linear-clamp sampler. absTexel = slot interior origin + localUV*(tileRes-1);
// the +0.5 puts the sample on a texel centre so hardware bilinear matches the
// CPU mirror's SampleGridBilinearTexel exactly.
vec2 CBT_AtlasSlotUV(uint slot, vec2 localUV, CBTAtlasParams p)
{
    uint col = slot % p.SlotsPerRow;
    uint row = slot / p.SlotsPerRow;
    float interiorX = float(col * p.SlotStride + CBT_ATLAS_APRON);
    float interiorY = float(row * p.SlotStride + CBT_ATLAS_APRON);
    vec2 absTexel = vec2(interiorX, interiorY) + localUV * float(p.TileRes - 1u);
    return (absTexel + vec2(0.5)) / float(p.AtlasDim);
}

// Integer atlas texel for a slot's interior sample. localTexel is the tile-local
// integer sample index in [0, TileRes-1]^2. This is the WRITE-THROUGH twin of
// CBT_AtlasSlotUV: a compute bake that imageStores at CBT_AtlasSlotTexel(slot, s, p)
// writes exactly the texel the sampler reads at CBT_AtlasSlotUV(slot, s/(TileRes-1), p),
// so sample<->bake parity holds by construction (design §2.3 write-through-indirection).
// Pure math, no bindings — the compute writer includes this file for the same resolve
// the sampler uses.
ivec2 CBT_AtlasSlotTexel(uint slot, ivec2 localTexel, CBTAtlasParams p)
{
    uint col = slot % p.SlotsPerRow;
    uint row = slot / p.SlotsPerRow;
    int interiorX = int(col * p.SlotStride + CBT_ATLAS_APRON);
    int interiorY = int(row * p.SlotStride + CBT_ATLAS_APRON);
    return ivec2(interiorX + localTexel.x, interiorY + localTexel.y);
}

// Full resolve result: whether the tile is resident, its slot UV (valid only
// when Resident), and the tile-local UV (for the coarse fallback + LOD hint).
struct AtlasResolve
{
    bool Resident;
    vec2 SlotUV;
    vec2 LocalUV;
    ivec2 Tile;
    float LodBias;
    float Fade; // coarse->slot upgrade crossfade [0,1] from the row (1 when not resident / settled)
};

// terrainUV -> the atlas resolve. rows is the indirection SSBO the consumer
// binds; CBT_ATLAS_NO_SLOT (or a null-equivalent params) yields Resident=false so
// the consumer takes the coarse out-of-window fallback (never garbage — Risk 3).
AtlasResolve CBT_AtlasResolve(vec2 terrainUV, CBTAtlasParams p, CBTTileAtlasSlot row)
{
    AtlasResolve r;
    CBT_AtlasTile(terrainUV, p, r.Tile, r.LocalUV);
    r.LodBias = row.LodBias;
    r.Fade = row.Fade;
    if (row.Slot == CBT_ATLAS_NO_SLOT || p.AtlasDim == 0u)
    {
        r.Resident = false;
        r.SlotUV = vec2(0.0);
        r.Fade = 1.0; // out-of-window: pure coarse field, no crossfade
        return r;
    }
    r.Resident = true;
    r.SlotUV = CBT_AtlasSlotUV(row.Slot, r.LocalUV, p);
    return r;
}

#endif // CBT_ATLAS_GLSL
