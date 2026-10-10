// DDGI (dynamic diffuse global illumination) shared constants + grid/atlas
// addressing math. Ported from the speedball-gi library (MIT license; text,
// upstream URL and pinned revision in ThirdParty/SpeedballGi/; port adapted for
// this engine's GLSL/std430 conventions).
//
// One world-space grid of octahedral irradiance probes, traced in compute
// against the scene's ray-query acceleration structure. Each probe's
// hemisphere-integrated irradiance is encoded into a small square tile
// (GE_DDGI_OCT_RES x GE_DDGI_OCT_RES interior texels + a 1px gutter) via
// GE_OctEncode/GE_OctDecode (Includes/octahedral_normal.glsl); tiles are
// packed into one 2D atlas texture, one tile per probe. Depth moments get a
// parallel atlas for Chebyshev leak rejection (GE_DDGIVisibilityWeight), and
// a second cascade (C1) repeats the whole structure at finer spacing.
//
// The SAME traced rays additionally bake two specular reflection lobes at
// different angular resolutions -- see GE_DDGI_GLOSSY_OCT_RES below and
// Includes/ddgi_probes.glsl's consumer.

#ifndef GE_DDGI_COMMON_GLSL
#define GE_DDGI_COMMON_GLSL

#include "octahedral_normal.glsl"

// Interior octahedral resolution per probe (irradiance). Matches the
// upstream library's tuned default — do not change without re-tuning the
// hysteresis/ray-budget constants below, which were chosen against this tile
// size.
const int GE_DDGI_OCT_RES = 6;
const int GE_DDGI_BORDER = 1;               // 1px gutter on every side (bilinear wrap safety)
const int GE_DDGI_TILE = GE_DDGI_OCT_RES + 2 * GE_DDGI_BORDER; // 8x8 atlas tile

// Depth-moment tile resolution. DDGIDepthResolution::Shared keeps the moments
// in the irradiance tile (GE_DDGI_OCT_RES, one fused blend/upload); Fine gives
// them their own 14x14 interior tile, so the Chebyshev test resolves an
// occluder's direction about 2.3x more finely and leaks less at a given
// strength. One binary serves both modes, so every depth-atlas address takes
// the interior resolution as an argument (GE_DDGIDepthTexelUV) rather than
// reading a constant.
const int GE_DDGI_DEPTH_OCT_RES_FINE = 14;
const int GE_DDGI_DEPTH_TILE_FINE = GE_DDGI_DEPTH_OCT_RES_FINE + 2 * GE_DDGI_BORDER;  // 16x16

// Specular reflection lobes. TWO lobes are baked from the SAME traced rays,
// at DIFFERENT angular resolutions, because one octahedral map cannot serve
// both: a lobe is only resolvable if its angular width exceeds the texel
// width of the map storing it.
//
//   lobe    power  half-max angle   interior   texel half-width
//   rough     8        23.5deg        6x6          ~19.2deg
//   glossy   64         8.4deg       16x16          ~7.2deg
//
// Each lobe is paired with storage that can actually reconstruct it. (A
// power-24 lobe -- 13.7deg -- in a 6x6 map is narrower than one of its own
// texels, so bilinear reconstruction smears it back toward power-8 and the
// extra sharpness is spent for nothing. That mismatch is what this pairing
// exists to avoid.)
//
// The rough lobe shares the irradiance/depth atlas packing (probe-XYZ,
// z-major). The glossy lobe is packed INDEPENDENTLY in near-square tile rows
// (GE_DDGIGlossyTileOrigin) -- at 18x18 tiles the z-major layout's height is
// probeY*probeZ*18, which passes the 8192 texture limit at 32^3 probes, while
// near-square stays at 182*18 = 3276.
const int GE_DDGI_GLOSSY_OCT_RES = 16;
const int GE_DDGI_GLOSSY_TILE = GE_DDGI_GLOSSY_OCT_RES + 2 * GE_DDGI_BORDER;  // 18x18 atlas tile

// Evaluated by repeated squaring rather than pow(): the exponents are
// compile-time constants and this runs per texel per ray.
float GE_DDGIRoughLobeWeight(float cosDir)
{
    float d2 = cosDir * cosDir;
    float d4 = d2 * d2;
    return d4 * d4;  // power 8
}

float GE_DDGIGlossyLobeWeight(float cosDir)
{
    float d2 = cosDir * cosDir;
    float d4 = d2 * d2;
    float d8 = d4 * d4;
    float d16 = d8 * d8;
    float d32 = d16 * d16;
    return d32 * d32;  // power 64
}

// Directional COVERAGE of a traced ray: does this ray carry radiance the
// reflection lobes are entitled to claim authority over?
//
// Reflection is composited over the prefiltered environment cube rather than
// blended with it (Includes/ibl.glsl), so a lobe must report not just what it
// saw but how much of its solid angle it actually saw ANYTHING for. A local
// hit is real reflected radiance and always counts. A true miss (hitT -1)
// already carries this engine's sky term (the trace kernels resolve it at
// miss, ddgi_trace_hw.comp/ddgi_trace_sw.comp), but that term is a coarse
// probe-side approximation of the very cube the consumer would otherwise
// sample directly -- so by default a miss claims NO coverage and the real
// prefiltered cube shows through instead. This mirrors the upstream
// library's `reflectionSkyFallback`, which is likewise default-off.
//
// Rays that claim no coverage still count toward the lobe's DENOMINATOR (see
// each blend kernel): they are directions the lobe covers but has no
// authority over, so they must dilute the coverage fraction rather than be
// silently dropped, which would report full authority from a single grazing
// hit.
const float GE_DDGI_REFLECTION_SKY_FALLBACK = 0.0;
float GE_DDGIRayCoverage(float hitT)
{
    if (hitT >= 0.0)
        return 1.0;
    return GE_DDGI_REFLECTION_SKY_FALLBACK;
}

const int GE_DDGI_RAYS_PER_PROBE_DEFAULT = 64;
const int GE_DDGI_RAYS_MIN = 32;
const int GE_DDGI_RAYS_MAX = 256;

const float GE_DDGI_T_MAX = 1.0e30;
const float GE_DDGI_RAY_EPS = 1.0e-3;

// Sample-side normal bias: push the irradiance-lookup sample point off the
// shaded surface by this fraction of the grid's minimum cell size, so a
// probe does not sample its own surface's back-face. Distinct from the
// trace-side origin bias below (different purpose, same unit).
//
// 3% of a cell, along the surface normal only — the reference library's
// SURFACE_NORMAL_BIAS_CELL, which its authored NormalBiasScale (1.75 on the
// parity fixture) puts at ~5% of a cell, about 0.1 m on Sponza. A lookup
// that far off the wall still reads the probes that see the wall's own
// neighbourhood; the earlier 25% + a 25% view term moved it most of a cell
// away, and with it the colour a nearby drape or wall bounces onto that
// surface. Self-occlusion at grazing angles is the Chebyshev tolerance's job
// (GE_DDGI_CHEBY_BIAS_CELL below), not this bias's. A fraction of a CELL and
// not a world distance so it tracks probe spacing automatically.
const float GE_DDGI_SURFACE_NORMAL_BIAS_CELL = 0.03;

// Sample-side bias for an irradiance lookup.
vec3 GE_DDGISurfaceBias(vec3 N, float minCellWS, float normalBiasScale)
{
    return N * (GE_DDGI_SURFACE_NORMAL_BIAS_CELL * minCellWS * normalBiasScale);
}

// Chebyshev SELF-OCCLUSION tolerance, as a fraction of the grid's min cell.
// A lit surface must not shadow itself against its own low-resolution depth
// moments: the stored mean toward a receiver carries the octahedral averaging
// error of a 6x6 tile plus the surface normal bias already applied to the
// sample point, so a fragment on a flat floor routinely measures a hair
// FARTHER than its own probe's recorded mean and the raw test rejects it.
// Without this tolerance each near-surface probe stamps a dark trilinear tent
// on the surface under it. Large enough to absorb both error terms, and small
// enough to stay under wall thickness so leak rejection through real walls is
// unaffected.
const float GE_DDGI_CHEBY_BIAS_CELL = 0.08;
// The Fine depth tile's texel is 6/14 the angular width of the shared one, so
// its octahedral averaging error shrinks by the same ratio and the tolerance
// follows it — keeping 0.08 there would give back the leak the finer moments
// were bought to remove.
const float GE_DDGI_CHEBY_BIAS_CELL_FINE = 0.035;
float GE_DDGIChebyBiasCell(int depthOctRes)
{
    return depthOctRes > GE_DDGI_OCT_RES ? GE_DDGI_CHEBY_BIAS_CELL_FINE : GE_DDGI_CHEBY_BIAS_CELL;
}
// Trace/NEE ray origin bias (self-intersection guard at the HIT point),
// fraction of the grid's minimum cell size.
const float GE_DDGI_TRACE_SURFACE_BIAS_CELL = 0.005;

// Ceiling on the albedo used for the RECURSIVE bounce term only (each trace
// kernel's `bounce = albedo * <last frame's atlas>`). That term is a feedback
// loop with per-cycle gain = albedo, so it converges only for albedo < 1.
// The engine's default material baseColor is exactly (1,1,1,1), which makes
// the gain 1.0: with fresh direct light added every tick and nothing
// dissipating it, the field grows without bound (observed as the whole scene
// blowing out to white over ~10s). Physical surfaces are below this anyway —
// fresh snow, the brightest natural diffuse reflector, is ~0.9 — so this
// clamps only non-physical authoring, and does so at the one place where a
// gain of 1 stops being merely bright and becomes divergent.
const float GE_DDGI_MAX_BOUNCE_ALBEDO = 0.9;

// NOTE: GE_MeshGeometryDesc (the per-mesh vertex/index device-address
// descriptor) is NOT declared here even though it is conceptually "common"
// — its VertexAddress/IndexAddress fields are uint64_t, which needs
// GL_EXT_shader_explicit_arithmetic_types_int64. This file is included by
// every DDGI kernel, including the plain-compute ones (blend/upload/clear)
// that never enable that extension, so putting a uint64_t-bearing struct
// here would fail to compile for them. See Includes/ddgi_hit_shade.glsl,
// which already requires that extension for its buffer_reference
// declarations and is included only by kernels that do.

// One probe grid's placement + resolution. GLSL mirror of the C++-side
// DDGIVolumeDesc snapshot pushed by DDGIVolumeSystem into DDGIProbeFeature;
// uploaded once per volume change as part of DDGIVolumeParams (Includes/ddgi_probes.glsl).
struct GE_DDGIGridInfo
{
    vec3 gridMinWS;
    float pad0;
    vec3 gridSizeWS;      // world-space extents (max - min)
    float pad1;
    ivec3 probeCount;     // resX, resY, resZ
    int probeTotal;       // probeCount.x * probeCount.y * probeCount.z
    float minCellWS;      // smallest grid.size/max(res-1,1) axis — self-occlusion bias scale
    float normalBiasScale;
    float hysteresis;     // temporal blend retention in [0,1); see GE_DDGIHysteresisWeight
    float intensity;
};

// Toroidal probe storage.
//
// A probe's ATLAS SLOT is its absolute world-cell index modulo the grid
// resolution. The consequence is the whole point: when the grid scrolls by a
// whole cell, every probe still inside it keeps the same slot, so its converged
// irradiance, depth and relocation offset survive the move. Only the thin slab
// of newly entered cells inherits a slot whose previous occupant just left.
//
// There is deliberately no scroll counter anywhere. The mapping is a pure
// function of the grid's own min corner, so nothing can drift out of sync with
// it, and a grid that is not moving produces a fixed permutation of slots —
// behaviour identical to a direct mapping.
ivec3 GE_DDGIGridMinCell(GE_DDGIGridInfo grid)
{
    vec3 spans = vec3(max(grid.probeCount - ivec3(1), ivec3(1)));
    vec3 spacing = max(grid.gridSizeWS / spans, vec3(1e-4));
    return ivec3(round(grid.gridMinWS / spacing));
}

// GLSL's % follows C: negative operands give a negative result, which would
// index outside the atlas for any grid whose min corner sits below the origin.
int GE_DDGIWrap(int v, int m)
{
    return ((v % m) + m) % m;
}

// Grid coordinate (0..res-1 within the current box) of the probe held in
// storage slot `probeIdx` (row-major x + y*resX + z*resX*resY): the cell
// congruent to the slot modulo the resolution that lies inside the grid's
// current span.
ivec3 GE_DDGIProbeCoord(GE_DDGIGridInfo grid, int probeIdx)
{
    int resX = max(grid.probeCount.x, 1);
    int resY = max(grid.probeCount.y, 1);
    int resZ = max(grid.probeCount.z, 1);
    ivec3 slot = ivec3(probeIdx % resX, (probeIdx / resX) % resY, probeIdx / (resX * resY));
    ivec3 minCell = GE_DDGIGridMinCell(grid);
    return ivec3(GE_DDGIWrap(slot.x - minCell.x, resX),
                 GE_DDGIWrap(slot.y - minCell.y, resY),
                 GE_DDGIWrap(slot.z - minCell.z, resZ));
}

// Test exponent bits rather than ordered comparisons: NaN/Inf must not seed
// persistent probe history, including when the backend enables fast math.
bool GE_DDGIFinite(vec4 value)
{
    return all(lessThan(floatBitsToUint(value) & uvec4(0x7f800000u), uvec4(0x7f800000u)));
}

vec4 GE_DDGIFiniteRay(vec4 ray)
{
    // An invalid lighting sample contributes no energy this visit. Keep its
    // finite distance for visibility; the next valid visit can recover.
    if (!GE_DDGIFinite(vec4(ray.rgb, 0.0)))
        ray.rgb = vec3(0.0);
    if (!GE_DDGIFinite(vec4(ray.w)))
        ray.w = -1.0;
    return ray;
}

// Small placement corrections, particularly tangential steps at the 0.45-cell
// relocation cap, must not repeatedly throw away converging lighting history.
vec3 GE_DDGIStableRelocation(vec3 previous, vec3 proposed, float minCellWS)
{
    return length(proposed - previous) < minCellWS * 0.05 ? previous : proposed;
}

bool GE_DDGIResetRelocationHistory(bool wasActive, bool buried, bool crossedGeometry)
{
    // Motion inside the same connected free space keeps history. A state
    // transition or an actual surface crossing changes the lighting domain.
    // A still-buried probe contributes no lighting. Its escape steps may
    // cross several surfaces; activation will reset once it becomes usable.
    return wasActive == buried || (wasActive && crossedGeometry);
}

vec3 GE_DDGIConstrainRelocation(vec3 previous, vec3 proposed, bool buried, bool crossedGeometry)
{
    // Escape from inside geometry is allowed. A free probe must not walk
    // back through a wall toward a lattice point inside that wall, otherwise
    // it alternates between buried and active forever at the relocation cap.
    return crossedGeometry && !buried ? previous : proposed;
}

// Absolute world cell a slot stands for this tick, as floats so it can live
// in a clearable vec4 record (DDGIProbeCell). A scrolled grid hands a slot to
// a NEW cell while the slot keeps its old irradiance, depth moments, history
// count and relocation offset; every kernel that consumes that state compares
// the record against this value and, on a mismatch, treats the slot as
// freshly allocated. Without that, a slab that scrolled in from open sky
// converges toward the interior it now covers at the running-mean rate —
// minutes at a throttled budget — instead of in one sweep.
vec3 GE_DDGIProbeCellId(GE_DDGIGridInfo grid, int probeIdx)
{
    return vec3(GE_DDGIGridMinCell(grid) + GE_DDGIProbeCoord(grid, probeIdx));
}

// Record value of a slot that has never been stamped (clear-on-allocate).
const float GE_DDGI_PROBE_CELL_UNSTAMPED = -1.0e30;

bool GE_DDGIProbeCellChanged(vec4 record, GE_DDGIGridInfo grid, int probeIdx)
{
    return any(notEqual(record.xyz, GE_DDGIProbeCellId(grid, probeIdx)));
}

// World-space position of the probe held in storage slot `probeIdx`.
vec3 GE_DDGIProbePosition(GE_DDGIGridInfo grid, int probeIdx)
{
    ivec3 coord = GE_DDGIProbeCoord(grid, probeIdx);

    vec3 t = vec3(
        grid.probeCount.x > 1 ? float(coord.x) / float(grid.probeCount.x - 1) : 0.5,
        grid.probeCount.y > 1 ? float(coord.y) / float(grid.probeCount.y - 1) : 0.5,
        grid.probeCount.z > 1 ? float(coord.z) / float(grid.probeCount.z - 1) : 0.5);
    return grid.gridMinWS + t * grid.gridSizeWS;
}

// Grid-space (not yet clamped) fractional coordinate of a world position —
// the basis for both the 8-probe trilinear lookup (GE_DDGISampleIrradiance,
// Includes/ddgi_probes.glsl) and probe-grid nearest-index queries.
vec3 GE_DDGIWorldToGridSpace(GE_DDGIGridInfo grid, vec3 posWS)
{
    vec3 size = max(grid.gridSizeWS, vec3(1e-4));
    vec3 t = (posWS - grid.gridMinWS) / size;
    return t * vec3(max(grid.probeCount.x - 1, 1), max(grid.probeCount.y - 1, 1),
                    max(grid.probeCount.z - 1, 1));
}

// Storage slot for grid coordinate `coord` (0..res-1 within the current box).
// Inverse of GE_DDGIProbePosition; see its note on toroidal storage.
int GE_DDGIProbeIndex(GE_DDGIGridInfo grid, ivec3 coord)
{
    ivec3 c = clamp(coord, ivec3(0), grid.probeCount - ivec3(1));
    int resX = max(grid.probeCount.x, 1);
    int resY = max(grid.probeCount.y, 1);
    int resZ = max(grid.probeCount.z, 1);
    ivec3 minCell = GE_DDGIGridMinCell(grid);
    ivec3 slot = ivec3(GE_DDGIWrap(minCell.x + c.x, resX),
                       GE_DDGIWrap(minCell.y + c.y, resY),
                       GE_DDGIWrap(minCell.z + c.z, resZ));
    return slot.x + slot.y * resX + slot.z * resX * resY;
}

// Atlas tile origin (texel coords of the tile's top-left INCLUDING the
// gutter) for probe `probeIdx`. Atlas layout: probeCount.x columns of tiles
// across, probeCount.y * probeCount.z rows down (Y-slices stacked per Z-layer
// — mirrors the upstream library's packing exactly, see gi_probes.js).
ivec2 GE_DDGIProbeTileOriginRes(GE_DDGIGridInfo grid, int probeIdx, int tile)
{
    int resX = max(grid.probeCount.x, 1);
    int resY = max(grid.probeCount.y, 1);
    int x = probeIdx % resX;
    int y = (probeIdx / resX) % resY;
    int z = probeIdx / (resX * resY);
    return ivec2(x, y + z * resY) * tile;
}

ivec2 GE_DDGIProbeTileOrigin(GE_DDGIGridInfo grid, int probeIdx)
{
    return GE_DDGIProbeTileOriginRes(grid, probeIdx, GE_DDGI_TILE);
}

ivec2 GE_DDGIAtlasSizeRes(GE_DDGIGridInfo grid, int tile)
{
    return ivec2(max(grid.probeCount.x, 1) * tile,
                max(grid.probeCount.y, 1) * max(grid.probeCount.z, 1) * tile);
}

ivec2 GE_DDGIAtlasSize(GE_DDGIGridInfo grid)
{
    return GE_DDGIAtlasSizeRes(grid, GE_DDGI_TILE);
}

// Per-texel UV (within the atlas, [0,1]) sampling a probe's octahedral
// interior in direction `dir`. Used both by the trace kernel's last-frame
// atlas read (infinite bounce) and by the forward consumer's irradiance
// lookup.
vec2 GE_DDGIProbeTexelUV(GE_DDGIGridInfo grid, int probeIdx, vec3 dir)
{
    ivec2 origin = GE_DDGIProbeTileOrigin(grid, probeIdx);
    vec2 oct = GE_OctEncode(normalize(dir));  // [0,1]
    vec2 texel = vec2(origin) + float(GE_DDGI_BORDER) + oct * float(GE_DDGI_OCT_RES);
    return texel / vec2(GE_DDGIAtlasSize(grid));
}

// Depth-atlas twin of GE_DDGIProbeTexelUV. `depthOctRes` is the atlas's
// interior tile resolution — GE_DDGI_OCT_RES under DDGIDepthResolution::Shared
// (the depth atlas then has the irradiance atlas's exact layout) or
// GE_DDGI_DEPTH_OCT_RES_FINE under Fine. Every reader of a depth atlas
// (consumer, reflection gather, recursive bounce) MUST address it through
// this with the value its UBO carries; the irradiance addressing above is
// wrong for a Fine atlas by a factor of two in texel space.
vec2 GE_DDGIDepthTexelUV(GE_DDGIGridInfo grid, int probeIdx, vec3 dir, int depthOctRes)
{
    int tile = depthOctRes + 2 * GE_DDGI_BORDER;
    ivec2 origin = GE_DDGIProbeTileOriginRes(grid, probeIdx, tile);
    vec2 oct = GE_OctEncode(normalize(dir));  // [0,1]
    vec2 texel = vec2(origin) + float(GE_DDGI_BORDER) + oct * float(depthOctRes);
    return texel / vec2(GE_DDGIAtlasSizeRes(grid, tile));
}

// Octahedral gutter mirror: which INTERIOR texel of a tile the texel at
// `texel` corresponds to. An interior texel maps to itself; a gutter texel
// maps to the interior texel its octahedral wrap lands on -- the diagonally
// opposite interior corner for a corner texel, otherwise the adjacent
// interior row/column REVERSED along the edge.
//
// The reversal is the whole point and is why a plain inward clamp is wrong:
// the octahedral fold makes a left-gutter texel continue the interior at
// (lo, tileEdge - y), a different direction from (lo, y). A clamp would give
// the gutter a direction it does not represent, and GE_DDGIProbeTexelUV lands
// exactly on the texel boundary at the octahedral extremes -- giving that
// gutter up to half the bilinear weight -- so the error is visible as a
// quadrant cross on smooth surfaces, not a hidden edge case.
//
// Shared by the intra-tile denoise filter (ddgi_upload.comp, which needs the
// interior texel a gutter texel should be filtered ALONGSIDE) and by the
// glossy blend kernel (ddgi_glossy_blend.comp, which needs the interior texel
// whose DIRECTION a gutter texel must store). Same fold, same answer -- two
// copies of it would be exactly the drift this port keeps designing against.
ivec2 GE_DDGIOctMirrorTexel(ivec2 texel, int tile, int octRes)
{
    int lo = GE_DDGI_BORDER;
    int hi = GE_DDGI_BORDER + octRes - 1;
    int edge = tile - 1;
    bool onLeft = texel.x == 0;
    bool onTop = texel.y == 0;
    bool onColumn = onLeft || texel.x == edge;
    bool onRow = onTop || texel.y == edge;
    if (onColumn && onRow)
        return ivec2(onLeft ? hi : lo, onTop ? hi : lo);
    if (onRow)
        return ivec2(edge - texel.x, onTop ? lo : hi);
    if (onColumn)
        return ivec2(onLeft ? lo : hi, edge - texel.y);
    return texel;
}

// Glossy atlas packing: near-square tile rows, independent of probe XYZ (see
// GE_DDGI_GLOSSY_OCT_RES's doc on why the shared z-major layout does not
// scale to 18x18 tiles). `tilesX` is computed CPU-side and passed in rather
// than recomputed as ceil(sqrt(probeTotal)) per invocation, so the kernels,
// the consumer and the C++ allocation cannot disagree about the layout --
// see DDGIGlossyAtlasLayout.h, which owns that math and is unit-tested.
ivec2 GE_DDGIGlossyTileOrigin(int probeIdx, int tilesX)
{
    int cols = max(tilesX, 1);
    return ivec2(probeIdx % cols, probeIdx / cols) * GE_DDGI_GLOSSY_TILE;
}

// Per-texel UV into the glossy atlas for probe `probeIdx` in direction `dir`.
// Interior texels are generated at (i + 0.5)/octRes, so mapping back to texel
// space is BORDER + oct*octRes -- adding another half texel would shift the
// octahedral fold endpoints asymmetrically into an interior texel on one side
// and the gutter on the other.
vec2 GE_DDGIGlossyTexelUV(int probeIdx, int tilesX, vec2 atlasSize, vec3 dir)
{
    ivec2 origin = GE_DDGIGlossyTileOrigin(probeIdx, tilesX);
    vec2 oct = GE_OctEncode(normalize(dir));  // [0,1]
    vec2 texel = vec2(origin) + float(GE_DDGI_BORDER) + oct * float(GE_DDGI_GLOSSY_OCT_RES);
    return texel / max(atlasSize, vec2(1.0));
}

// A fully occluded probe still contributes a small floor rather than exact
// zero — avoids a hard, visible seam where a probe's visibility crosses from
// "barely lit" to "fully black" one texel apart; the ~5% residual is well
// below the noise floor of the trilinear blend itself.
const float GE_DDGI_MIN_VISIBILITY_WEIGHT = 0.05;

// Floor under the smooth-backface (wrap) weight: a fully back-facing probe
// still contributes this much rather than snapping to zero, which would
// trade dark cell diamonds for hard cell edges. The reference library's
// value; the same floor its visibility term uses.
const float GE_DDGI_MIN_BACKFACE_WEIGHT = 0.05;

// Cosine falloff toward a probe, remapped to [0,1] and squared: the reference
// library's (dot + 1) * 0.5 wrap term, floored, and dialled by the same
// strength as the visibility test so strength 0 is the exact pure-trilinear
// look. `towardProbe` points FROM the shading point TO the probe.
float GE_DDGIBackfaceWeight(vec3 towardProbe, vec3 N, float strength)
{
    float wrap = (dot(towardProbe, N) + 1.0) * 0.5;
    return mix(1.0, max(wrap * wrap, GE_DDGI_MIN_BACKFACE_WEIGHT), clamp(strength, 0.0, 1.0));
}

// Chebyshev (variance shadow map style) visibility weight for a sample point
// at distance `distToPoint` from a probe, given that probe's (mean, mean^2)
// hit-distance moments toward the point.
//
// Lives here, not in a consumer- or trace-side file, because BOTH the forward
// consumer (Includes/ddgi_probes.glsl's GE_DDGISampleC0/C1) and the trace
// kernels' recursive bounce fetch (Includes/ddgi_hit_shade.glsl's
// GE_DDGISampleBounce) weight their 8-probe gathers with it. Two copies of a
// leak-rejection rule that must agree is exactly the drift this port keeps
// designing against.
float GE_DDGIChebyshevVisibility(vec2 moments, float distToPoint, float bias)
{
    float mean = moments.x;
    float variance = abs(mean * mean - moments.y);
    float biasedDist = max(distToPoint - bias, 0.0);
    float diff = max(biasedDist - mean, 0.0);
    float chebyshev = variance / (variance + diff * diff);
    chebyshev = max(chebyshev * chebyshev * chebyshev, 0.0);
    float visibility = biasedDist <= mean ? 1.0 : chebyshev;
    return max(visibility, GE_DDGI_MIN_VISIBILITY_WEIGHT);
}

// The same test, dialled between "no visibility rejection at all" (strength 0,
// pure trilinear — every corner contributes its geometric weight) and the full
// test above (strength 1). Authored as one knob because full-strength Chebyshev
// over low-resolution depth moments over-rejects on thin and two-sided
// geometry: it errors on the side of darkening a surface that should be lit,
// and the fix is to admit a little leak back rather than to raise the probe
// density. Every gather that weights a corner MUST go through this, or the
// consumer and the recursive bounce fetch drift apart on where light stops.
//
// `depthOctRes` selects the self-occlusion tolerance for the atlas the moments
// came from (GE_DDGIChebyBiasCell): finer moments carry less averaging error
// and get a tighter tolerance.
float GE_DDGIVisibilityWeight(vec2 moments, float distToPoint, float strength, float minCellWS,
                              int depthOctRes)
{
    return mix(1.0,
               GE_DDGIChebyshevVisibility(moments, distToPoint,
                                          minCellWS * GE_DDGIChebyBiasCell(depthOctRes)),
               clamp(strength, 0.0, 1.0));
}

// How much of the classify pass's output (ddgi_classify.comp / its software
// twin) is applied at a gather corner, dialled by DDGIVolume::ClassifyStrength.
// Returns xyz = the relocation offset to add to the probe's lattice position,
// w = the weight multiplier for the probe's buried flag. Strength 0 returns
// (0,0,0,1) — the bare lattice, classification ignored, exactly what Grid
// placement renders; strength 1 returns the raw offset and a 0/1 flag, so a
// buried probe drops out of the gather entirely. Every gather that reads
// ge_ddgiProbeState MUST go through this, for the same reason
// GE_DDGIVisibilityWeight exists: the consumer, the reflection lookup, the
// recursive bounce fetch and the trace kernels' ray origin must agree on where
// a probe is and whether it counts.
vec4 GE_DDGIProbeStateApply(vec4 state, float classifyStrength)
{
    float s = clamp(classifyStrength, 0.0, 1.0);
    return vec4(state.xyz * s, mix(1.0, state.w, s));
}

// Spherical-Fibonacci ray set: a well-distributed, deterministic sampling of
// the full sphere shared VERBATIM by every DDGI kernel that needs to agree
// on "what direction did ray i point" (the trace kernel writes results
// indexed by ray i; the blend kernel must reconstruct the SAME direction for
// its cosine-weighted gather, or the two silently disagree). Unrotated: the
// per-tick basis rotation is GE_DDGIRayBasis below, applied by
// GE_DDGIRayDirection.
vec3 GE_DDGISphericalFibonacci(uint i, uint n)
{
    const float kGoldenAngle = 2.39996322972865332;  // pi*(3-sqrt(5))
    float t = (float(i) + 0.5) / float(max(n, 1u));
    float phi = kGoldenAngle * float(i);
    float cosTheta = 1.0 - 2.0 * t;
    float sinTheta = sqrt(clamp(1.0 - cosTheta * cosTheta, 0.0, 1.0));
    return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

// Integer hash (PCG-style) feeding the basis rotation below: three
// decorrelated uniforms per epoch from one 32-bit key.
uint GE_DDGIHashU32(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float GE_DDGIHashUnit(uint key)
{
    return float(GE_DDGIHashU32(key) & 0x00FFFFFFu) / 16777216.0;
}

// Per-epoch rotation of the whole ray set, keyed on the CPU's sampling epoch:
// DDGIJitterMode::Gated holds the epoch at 0 (identity — the fixed ray set,
// the flicker-free default), MonteCarlo advances it per tick so consecutive
// solves sample different points on the sphere.
//
// A uniformly random 3D rotation (Arvo, Graphics Gems III: a random rotation
// about Z followed by a Householder reflection through a random unit vector,
// negated to restore a proper rotation). An azimuth-only spin leaves every
// elevation band of the Fibonacci set fixed, so the same polar caps and rings
// are sampled every tick and the long-run angular coverage stays banded; a
// full rotation moves every ray over the whole sphere. Trace and blend MUST
// derive the basis from the identical epoch (both read the same
// DDGIVolumeParams/DDGIBlendParams frameIndex field).
mat3 GE_DDGIRayBasis(uint frameIndex)
{
    if (frameIndex == 0u)
        return mat3(1.0);
    const float kTwoPi = 6.28318530717958647692;
    float u1 = GE_DDGIHashUnit(frameIndex * 3u + 1u);
    float u2 = GE_DDGIHashUnit(frameIndex * 3u + 2u);
    float u3 = GE_DDGIHashUnit(frameIndex * 3u + 3u);
    float theta = kTwoPi * u1;
    float phi = kTwoPi * u2;
    float r = sqrt(u3);
    vec3 v = vec3(cos(phi) * r, sin(phi) * r, sqrt(max(1.0 - u3, 0.0)));
    float ct = cos(theta);
    float st = sin(theta);
    // Column-major: rz maps x -> (ct, st, 0), y -> (-st, ct, 0).
    mat3 rz = mat3(ct, st, 0.0, -st, ct, 0.0, 0.0, 0.0, 1.0);
#if defined(GE_COMPAT_PROFILE)
    // naga cannot translate OpOuterProduct. Keep the native instruction on
    // full-profile devices; the equivalent columns are only needed by WGSL.
    mat3 householder = mat3(1.0) - 2.0 * mat3(v * v.x, v * v.y, v * v.z);
#else
    mat3 householder = mat3(1.0) - 2.0 * outerProduct(v, v);
#endif
    return -(householder * rz);
}

// Direction of ray `i` of `n` under this tick's basis. The ONE entry point
// every kernel uses, so trace, blend, both reflection lobes and classify can
// never disagree about where a ray went.
vec3 GE_DDGIRayDirection(uint i, uint n, mat3 basis)
{
    return basis * GE_DDGISphericalFibonacci(i, n);
}

// Each depth-state texel carries two moment pairs from the same rays.
//
// xy, the diffuse moments: visibility between the eight neighbouring probes
// and a receiver. Distant hits and sky use the same local bound: a
// volume-sized miss mixed into a near-wall direction inflates both moments
// and makes a nearby floor or wall disappear from the visibility test. The
// irradiance consumer and the trace bounce read these.
//
// zw, the reflection moments: the true hit distance, a miss filled with the
// volume's diagonal. The reflection gather reads these for its visibility
// weight and its parallax proxy, both of which need the distance to the
// surface a lobe actually reaches; bounded moments put every wall past the
// bound at the bound, which rejects probes a mirror needs and aims the proxy
// at a surface that is not there.
float GE_DDGIProbeMaxDistance(GE_DDGIGridInfo grid)
{
    vec3 spans = max(vec3(grid.probeCount - ivec3(1)), vec3(1.0));
    vec3 spacing = max(grid.gridSizeWS / spans, vec3(1e-4));
    // Includes clearance for relocated corners and the receiver's bias.
    return 1.5 * length(spacing);
}

float GE_DDGIBoundedProbeDistance(float rayDistance, float maxDistance)
{
    return rayDistance < 0.0 ? maxDistance : min(rayDistance, maxDistance);
}

float GE_DDGIReflectionProbeDistance(float rayDistance, GE_DDGIGridInfo grid)
{
    return rayDistance < 0.0 ? length(grid.gridSizeWS) : rayDistance;
}

// The two moment pairs of a depth-atlas texel (layout above). Every tap names
// the pair it reads.
vec2 GE_DDGIBoundedMoments(vec4 depthTexel)
{
    return depthTexel.rg;
}

vec2 GE_DDGITrueDistanceMoments(vec4 depthTexel)
{
    return depthTexel.ba;
}

// Exponential temporal retention, normalized to a 60 Hz reference update
// rate: h^(dt/ref) so a faster or slower probe-solve cadence still absorbs
// fresh Monte-Carlo noise at the same PER-SECOND rate the slider implies at
// 60 Hz — matches the upstream library's hysteresisExponentForInterval
// (gi_probes.js), simplified to the unconditional-normalize path (this
// engine ticks DDGI once per render frame, not at a variable solve rate yet
// — M4 reintroduces the budgeted round-robin cadence this exists for).
//
// The exponent is ASYMMETRIC and that asymmetry is the flicker bound, not a
// rounding convenience. Fast side (dt < ref) unclamped: 120/240 Hz then
// consumes the same fresh Monte-Carlo noise per SECOND as 60 Hz instead of
// boiling harder the faster the machine renders. Slow side (dt > ref) clamped
// at 1: every per-texel policy is a reference-domain retention r applied as
// r^exponent, and exponent <= 1 guarantees r^exponent >= r, so no single
// update can ever blend in more fresh noise than the slider admits at 60 Hz —
// however sparse the cadence gets under low FPS, a throttled ray budget, or a
// round-robin revisit. Sparse service therefore converges slower in wall
// clock, never noisier. (Dropping this clamp keeps per-second decay exactly
// rate-invariant but dissolves the field into flicker precisely when the
// machine is struggling.)
const float GE_DDGI_HYSTERESIS_DT_REF_MS = 1000.0 / 60.0;
float GE_DDGIHysteresisWeight(float hysteresis, float updateDtMs)
{
    float dt = max(updateDtMs, 0.0);
    float exponent = min(dt / GE_DDGI_HYSTERESIS_DT_REF_MS, 1.0);
    return pow(clamp(hysteresis, 0.0, 0.999), exponent);
}

#endif // GE_DDGI_COMMON_GLSL
