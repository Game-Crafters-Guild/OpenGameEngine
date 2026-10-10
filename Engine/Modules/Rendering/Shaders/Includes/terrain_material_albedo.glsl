// The terrain's ALBEDO at a point, for one material record. SHARED by the two surfaces that shade
// one terrain — the CBT ground (CBT/cbt_surface.glsl) and the grass blades standing on it
// (TerrainGrass/terrain_grass_surface.glsl) — for the same reason the blend resolve next door is
// shared: a blade's grounded base IS the ground colour under it, so a second definition of "the
// terrain's albedo here" is a second answer, and the base lands on the wrong colour by exactly the
// difference between them.
//
// TRAP: a consumer that re-implements "the terrain's albedo" with the record's flat tint (or a
// single plain-REPEAT XZ tap) drops the value-noise variation, hex tiling, triplanar side
// projections and world-anchoring UV phase the ground applies to the same record — on the shipped
// untextured palette that alone is the whole variation amplitude (VariationStrength 0.16 on the
// grass role). Sample through this file; never restate the resolve.
//
// WHAT THE INCLUDER SUPPLIES. Everything here is a pure function of a record plus a position; the
// only external dependencies are the bindless texture table and the per-view mip bias, so include
// AFTER Includes/bindless_textures.glsl and in a stage that has ViewParams (ge_mipBiasParams).
//
// The includer must also define CBT_LAYER_TEX(texIdx) -> a sampler2D for that bindless slot, and it
// is deliberately NOT defaulted: the right answer differs between the two stages, and getting it
// wrong is undefined behaviour rather than a look difference.
//
//   * The GROUND draws ONE terrain, so a material's bindless index is a per-draw palette value,
//     identical across every invocation. Indexing the array plainly lets the driver hoist the
//     descriptor load once per draw.
//   * A GRASS draw carries blades from SEVERAL terrains — the terrain row arrives per instance
//     through custom0.w — so the index varies within a draw and the access must be decorated
//     nonuniformEXT (GE_BTEX). Sampling a descriptor array with a divergent index without that
//     decoration is UB: it reads another material's texture, or faults.
//
// A default here would be one of those two, and the stage that inherited the wrong one would look
// correct on a single-terrain scene and break on a multi-terrain one.
//
// WHY THE CBT_ PREFIX ON A SHARED FILE. Includes/terrain_blend_resolve.glsl set the convention:
// the terrain shader family's symbols carry it whichever file they live in, and renaming them here
// would only decorrelate this file from the one next to it.
//
// The accessor check stays OUTSIDE the include guard, so it fires on every include rather than only
// on the first. (terrain_blend_resolve.glsl keeps its width check INSIDE its guard — this
// placement is the stronger of the two.)
#ifndef CBT_LAYER_TEX
#error "Includes/terrain_material_albedo.glsl: define CBT_LAYER_TEX(texIdx) before including it"
#endif

// THE COMPAT SEAMS. A profile with no descriptor indexing (WebGPU class) cannot reach a texture
// by index at all: it binds a fixed set of named textures and selects one with a switch. Three
// things follow, and each gets a seam whose DEFAULT is the bindless spelling, so the full profile
// reads exactly as before and only the compat surface overrides them.
//
//   * Presence. There the bindless index is the unbound sentinel for EVERY record, so an
//     `AlbedoTex == 0` test answers "untextured" for a textured layer and the surface returns its
//     tint without taking a tap. Presence has to come from a record FLAG instead.
//   * Slot. The switch selects on the blend LAYER ORDINAL and texture kind, not on the index —
//     which is why every tap below carries `slot` alongside `tex`.
//   * Gradients. The taps sit inside weight branches, i.e. non-uniform control flow, where an
//     implicit-derivative tap is invalid under WGSL's uniformity rules (it merely works on
//     desktop). The compat tap is therefore explicit-gradient, and the gradients are computed
//     once in CBT_MaterialUVs — function-entry, uniform flow — rather than at the tap.
#ifndef CBT_MAT_HAS_ALBEDO
#define CBT_MAT_HAS_ALBEDO(mat) ((mat).AlbedoTex != 0u)
#endif
#ifndef CBT_LAYER_SLOT
#define CBT_LAYER_SLOT(ord, kind) (0u)
#endif
#ifndef CBT_LAYER_TAP
#define CBT_LAYER_TAP(slot, tex, uv, ddx, ddy, bias) texture(CBT_LAYER_TEX(tex), uv, bias).rgb
#endif
// The clamp-to-edge twin, for a Planar image that covers the terrain once: on a REPEAT sampler a
// tap at u or v = 0 or 1 filters the OPPOSITE edge in 50/50, and a coarse mip spreads a band of the
// far edge along every border. The includer defines CBT_LAYER_TEX_EDGE (the same texture through
// the LinearClampAnisotropic preset, GE_TS_CLAMP_ANISO: the plain LinearClamp is isotropic and would
// read the image up to four mips softer at a grazing angle than the materials beside it) or
// overrides this seam.
#ifndef CBT_LAYER_TAP_EDGE
#define CBT_LAYER_TAP_EDGE(slot, tex, uv, ddx, ddy, bias) \
    texture(CBT_LAYER_TEX_EDGE(tex), uv, bias).rgb
#endif

#ifndef GE_TERRAIN_MATERIAL_ALBEDO_DECLARED
#define GE_TERRAIN_MATERIAL_ALBEDO_DECLARED

// One terrain material (std430 mirror of TerrainMaterialRecord in TerrainMaterialRecord.h —
// 20 x 4B). Declared ONCE, here, because both surfaces read the same table off the same buffer: a
// mirror that drifts reinterprets every record the other consumer wrote.
//
// A texture slot of 0 is the bindless sentinel and means "use the paired scalar"; binding a texture
// leaves that scalar live as a multiplier.
struct TerrainMaterialRecordData
{
    float AlbedoR;
    float AlbedoG;
    float AlbedoB;
    float Tiling;            // multiplies the terrain's global MaterialTiling (1 = the global rate)
    uint AlbedoTex;          // bindless slot, 0 = unbound (tint + variation fallback)
    uint NormalTex;          // tangent-space, triplanar-reoriented
    uint OrmTex;             // R = ambient occlusion, G = roughness, B = metallic
    uint Flags;              // bit0 hex tiling, bit1 retired, bit2 ORM carries metallic,
                             //   bits3-5 texture presence, bit6 planar projection
    float Roughness;         // used when OrmTex is unbound; multiplies its G channel when bound
    float Ao;                // same contract against OrmTex's R channel
    float NormalStrength;    // scales the tangent XY of a bound NormalTex; 0 => the terrain normal
    float HexRotStrength;    // 0 = plain REPEAT tiling
    float VariationStrength; // value-noise brightness jitter [0,1]
    float VariationHue;      // value-noise chroma jitter [0,1]
    float VariationScale;    // value-noise frequency (1/m); 0 => variation off
    float PlanarUVScaleX;    // render extent / authored size (Planar only; see the C++ record)
    float UVPhaseX;          // world-anchoring UV phase per axis: fract(renderOriginWorld * tiling),
    float UVPhaseY;          //   computed in DOUBLE on the CPU. Zero while the render origin is
    float UVPhaseZ;          //   inactive (positionRelWS == positionWS).
    float PlanarUVScaleZ;
};

// Planar projection (kTerrainMaterialFlagPlanar in TerrainMaterialRecord.h; CBTLayoutTests pins
// the literal): the material's textures are sampled at the terrain's own footprint UV rather than
// through the three world-space projections below, so one image of THIS terrain (an orthophoto)
// lies on the heightmap it was captured with, and a cliff shows it stretched instead of showing an
// unrelated part of it. A record value, identical for every invocation of a draw, so the branch on
// it is dynamically uniform.
const uint CBT_MATFLAG_PLANAR = 64u;

bool CBT_MatIsPlanar(TerrainMaterialRecordData mat)
{
    return (mat.Flags & CBT_MATFLAG_PLANAR) != 0u;
}

// --- Triplanar --------------------------------------------------------------------------------
// Slope-sharpened triplanar. A higher exponent gives crisper axis transitions on steep faces (the
// old soft abs(n)-0.2 blend smeared the side projection into the top on cliffs). Precedent:
// triplanar_pbr.glsl / SG_TriplanarWeights.
const float CBT_TRIPLANAR_SHARPNESS = 6.0;
// Weight floor (texture-tiling S1): sharpened weights below this are cut to exactly zero and the
// rest renormalized, so a projection's contribution reaches 0 CONTINUOUSLY before its taps stop
// running. The old hard 1-tap gate (dominant top weight >= 0.8 -> single projection) jumped up to
// 20% of the blend at the gate contour — with a texture bound that read as slope-following bands.
// Flat ground still collapses to a single XZ tap (weights == (0,1,0)), so the perf gate survives,
// now seam-free. The floor is also what makes a near-flat surface EXACTLY one tap rather than one
// dominant tap plus two small ones: below ~31.5 degrees of slope the side weights are cut to zero.
const float CBT_TRIPLANAR_WEIGHT_FLOOR = 0.05;

// Everything the per-layer albedo taps share, computed ONCE per fragment.
//
// pos is the RENDER-ORIGIN-RELATIVE position (the adapter's vPosRel): identical to positionWS
// while the render origin is inactive (planar / small planets — byte-identical UVs to the old path).
// How much precision it carries depends on which vertex path produced it:
//   * DEEP slots (CBTVertexData.deepTag != 0): the adapter rebuilds it from the EXACT integer
//     sector delta plus a small sector-local offset (GE_ClipFromSectorLocal), so it really is
//     fp32-precise at planetary magnitude — where a raw world coordinate quantizes (ULP(50 km)
//     = 4 mm, ULP(Earth R) = 0.5 m) and shreds both the UVs and the screen-space derivatives
//     that drive mip selection.
//   * LEGACY slots (deepTag == 0): the vertex position IS an fp32 world coordinate and the
//     adapter only subtracts the origin (GE_ClipFromWorld). That makes it smaller in magnitude,
//     NOT more precise — the quantization already happened upstream and no bits come back.
// The per-layer UVPhase (computed CPU-side in double) offsets each projection's UV by a whole
// number of texture repeats, so a plain REPEAT tap lands on the same texels across an origin
// rebase. It does NOT re-anchor the hex lattice — see CBT_HexTileTap.
struct CBTTriplanarCtx
{
    vec3 pos;     // tiling-space position (render-origin-relative)
    vec3 weights; // sharpened + floored projection weights (sum 1); (0,1,0) => single XZ tap
    // Screen-space gradients of `pos`, for the explicit-gradient compat tap. Taken HERE because
    // this is the last uniform control flow before the blend loop, and WGSL rejects a derivative
    // inside it. A material's own UV rate is a uniform scale of `pos`, so each projection's
    // gradient is these scaled by that rate — exact, not an approximation.
    vec3 dposdx;
    vec3 dposdy;
    // The terrain's own footprint UV: 0..1 across the terrain, U along world +X and V along world
    // +Z (CBT_TerrainToWorldXZ), the coordinate the heightmap, normal map and splat are read at. A
    // Planar material samples here instead of at `pos`. Supplied by the surface rather than derived
    // from `pos`, because only the surface knows its terrain's origin and size (the ground's is
    // sIn.uv0; the grass derives the same value from the blade's world position).
    vec2 terrainUV;
    vec2 dUVdx; // compat explicit-gradient tap only, as dposdx/dposdy
    vec2 dUVdy;
};

vec3 CBT_TriplanarWeights(vec3 n)
{
    vec3 w = pow(abs(n), vec3(CBT_TRIPLANAR_SHARPNESS));
    w /= max(w.x + w.y + w.z, 1e-6);
    w = max(w - vec3(CBT_TRIPLANAR_WEIGHT_FLOOR), vec3(0.0));
    return w / max(w.x + w.y + w.z, 1e-6); // sum > 0: the dominant pre-floor weight is >= 1/3
}

CBTTriplanarCtx CBT_BuildTriplanarCtx(vec3 posRel, vec3 normalWS, vec2 terrainUV)
{
    CBTTriplanarCtx tc;
    tc.pos = posRel;
    tc.weights = CBT_TriplanarWeights(normalWS);
    tc.terrainUV = terrainUV;
#if defined(GE_COMPAT_PROFILE)
    tc.dposdx = dFdx(posRel);
    tc.dposdy = dFdy(posRel);
    tc.dUVdx = dFdx(terrainUV);
    tc.dUVdy = dFdy(terrainUV);
#else
    tc.dposdx = vec3(0.0);
    tc.dposdy = vec3(0.0);
    tc.dUVdx = vec2(0.0);
    tc.dUVdy = vec2(0.0);
#endif
    return tc;
}

// ---- Hex tiling (Mikkelsen, "Practical Real-Time Hex-Tiling", JCGT 2022) ---------------------
// Blends three hash-rotated copies of the texture on a hexagonal lattice, killing the grid
// repetition a plain REPEAT sampler shows at terrain scale. Per-layer opt-in (HexRotStrength > 0)
// at ~3x the layer's tap cost.
//
// The block between the two markers below is compiled TWICE: as GLSL here, and as C++ by
// CBTHexTilingTests, which extracts it verbatim (Tests/ExtractShaderBlock.cmake) and measures it
// against an independent CPU oracle. Two rules keep the two compilations meaning the same thing:
// the block stays pure lattice math — no samplers, no bindless arrays, no globals — and its float
// literals carry the `f` suffix, so neither compiler promotes an expression to double.
// GE_SHARED_HEX_LATTICE_BEGIN
const float CBT_HEX_FALLOFF = 0.6f;
const float CBT_HEX_EXP = 7.0f;

// One simplex of the hex-lattice dual: three lattice vertex ids and their barycentric weights.
// Slot k carries the vertex whose CBT_HexColor is k, and w[k] is that same vertex's barycentric.
struct CBTHexSimplex
{
    vec3 w;
    ivec2 v[3];
};

// Lattice 3-colouring. The three vertices of any simplex of this grid have consecutive values of
// (v.x - v.y), so c(v) = (v.x - v.y) mod 3 gives each simplex exactly one vertex of each colour,
// and crossing a simplex boundary replaces one vertex with another of the SAME colour.
//
// GLSL leaves % undefined for a negative operand, and abs() cannot make the dividend safe: abs()
// of the most negative int is itself negative. The difference is taken in uint, where subtraction
// and % are both defined for every pair of ids — including the pairs whose true difference does
// not fit in an int. uint wraps by 2^32, which is 1 (mod 3), so a borrow (exactly the case
// v.x < v.y) moves the residue by one, and that is folded back out.
int CBT_HexColor(ivec2 v)
{
    int m = int((uint(v.x) - uint(v.y)) % 3u) - (v.x < v.y ? 1 : 0);
    return m < 0 ? 2 : m;
}

// UV -> the simplex of the hex-lattice dual containing it, slotted by colour.
//
// The slot order is load-bearing, not cosmetic. Each slot becomes one tap whose mip comes from
// implicit derivatives, so a slot whose vertex id differs between the pixels of a quad hands the
// sampler an O(1) bogus gradient and it lands at the coarsest mip. Emitting the simplex in
// traversal order does exactly that: two slots exchange vertices at every s-flip edge and rotate at
// every lattice line, each time while carrying O(1) barycentric weight. Colour slotting confines
// the exchange to the one slot whose weight is exactly zero on the boundary being crossed, so the
// mis-mipped tap contributes nothing. CBTHexTilingTests measures the weighted discontinuity.
CBTHexSimplex CBT_HexTriangleGrid(vec2 st)
{
    st *= 3.4641016151f; // 2*sqrt(3): the paper's hex density per texture repeat
    const mat2 kGridToSkewed = mat2(1.0f, 0.0f, -0.57735027f, 1.15470054f);
    vec2 skewed = kGridToSkewed * st;
    ivec2 baseId = ivec2(floor(skewed));
    vec3 t = vec3(fract(skewed), 0.0f);
    t.z = 1.0f - t.x - t.y;
    float s = step(0.0f, -t.z);
    float s2 = 2.0f * s - 1.0f;
    vec3 bw = vec3(-t.z * s2, s - t.y * s2, s - t.x * s2);
    int si = int(s);
    ivec2 p0 = baseId + ivec2(si, si);
    ivec2 p1 = baseId + ivec2(si, 1 - si);
    ivec2 p2 = baseId + ivec2(1 - si, si);
    // p0 carries baseId's difference (the s offset cancels), and p1 and p2 carry it minus and plus
    // one in an order the parity decides, so one residue determines all three colours.
    int k0 = CBT_HexColor(p0);
    int kNext = k0 == 2 ? 0 : k0 + 1;
    int kPrev = k0 == 0 ? 2 : k0 - 1;
    int k1 = si == 0 ? kPrev : kNext;
    // The three colours are a permutation of (0,1,2), so each slot falls through to p2 without
    // needing to test it.
    CBTHexSimplex sx;
    sx.v[0] = k0 == 0 ? p0 : (k1 == 0 ? p1 : p2);
    sx.v[1] = k0 == 1 ? p0 : (k1 == 1 ? p1 : p2);
    sx.v[2] = k0 == 2 ? p0 : (k1 == 2 ? p1 : p2);
    sx.w.x = k0 == 0 ? bw.x : (k1 == 0 ? bw.y : bw.z);
    sx.w.y = k0 == 1 ? bw.x : (k1 == 1 ? bw.y : bw.z);
    sx.w.z = k0 == 2 ? bw.x : (k1 == 2 ? bw.y : bw.z);
    return sx;
}

vec2 CBT_HexHash(ivec2 v)
{
    vec2 r = mat2(127.1f, 269.5f, 311.7f, 183.3f) * vec2(v);
    return fract(sin(r) * 43758.5453f);
}

// Per-tile random rotation: angle hashed from the lattice id, scaled by the layer's strength.
mat2 CBT_HexRot(ivec2 idx, float rotStrength)
{
    float angle = abs(float(idx.x * idx.y)) + abs(float(idx.x + idx.y)) + 3.14159265f;
    angle = mod(angle, 6.28318531f);
    if (angle > 3.14159265f)
        angle -= 6.28318531f;
    angle *= rotStrength;
    float c = cos(angle);
    float s = sin(angle);
    return mat2(c, -s, s, c);
}

// Hex-lattice vertex -> its UV-space center (inverse of the skew above).
vec2 CBT_HexCenter(ivec2 v)
{
    const mat2 kInvSkew = mat2(1.0f, 0.0f, 0.5f, 0.8660254038f);
    return (kInvSkew * vec2(v)) / 3.4641016151f;
}

// Where one slot samples: the point rotated about its tile's center by the tile's hashed angle,
// then offset by the tile's hashed translation. Continuous in st for a fixed v, so the tap's
// implicit derivative is the true gradient wherever the slot keeps its vertex.
vec2 CBT_HexTapUV(vec2 st, ivec2 v, float rotStrength)
{
    vec2 cen = CBT_HexCenter(v);
    return CBT_HexRot(v, rotStrength) * (st - cen) + cen + CBT_HexHash(v);
}
// GE_SHARED_HEX_LATTICE_END

// SAMPLING MODE. These taps supply no LOD, and the colour slotting in CBT_HexTriangleGrid is what
// makes that sound. The guarantee is conditional, and the condition is the whole of it: a slot's UV
// is continuous wherever THAT SLOT CARRIES WEIGHT. At a simplex boundary one slot does still
// exchange its vertex and its UV does still jump — but its barycentric is zero there, so the blend
// gives it nothing, and the taps that carry the pixel derive the true gradient.
//
// A slot whose UV jumps WHILE carrying weight hands the sampler an O(1) gradient across the quad.
// That is a large LOD, so the tap reads the LAST mip, not the top one: a hard flat band along the
// boundary, which the luminance falloff blend does not suppress because it weights that tap at ~1
// on one side of the edge, not at 0.
//
// Layer-loop divergence is a separate, still-open case: derivatives are undefined where the loop
// diverges at layer-blend boundaries.
//
// textureGrad/textureLod (the paper's prescription) are unused here. The evidence behind that
// choice is a driver-fault bisect whose runs split identically by variant and by session order, so
// it does not discriminate. Reopening the question needs new evidence, not that bisect.
//
// NOT world-anchored. The lattice ids come from floor() of `st` scaled by 2*sqrt(3) and skewed,
// so the whole-repeat UV offset the per-layer UVPhase applies — invisible to a plain REPEAT
// sampler — lands mid-cell here and re-hashes every tile's rotation and offset. A render-origin
// rebase therefore re-randomizes a hex-tiled layer. Anchoring it needs the phase folded into the
// lattice (an integer lattice-id shift), not into the UV.
vec3 CBT_HexTileTap(uint slot, uint tex, float rotStrength, vec2 st, vec2 ddx, vec2 ddy)
{
    CBTHexSimplex sx = CBT_HexTriangleGrid(st);
    vec2 st1 = CBT_HexTapUV(st, sx.v[0], rotStrength);
    vec2 st2 = CBT_HexTapUV(st, sx.v[1], rotStrength);
    vec2 st3 = CBT_HexTapUV(st, sx.v[2], rotStrength);
    // The hash rotation is rigid, so it preserves |ddx| — the unrotated gradients select the
    // same mip the rotated ones would.
    vec3 c1 = CBT_LAYER_TAP(slot, tex, st1, ddx, ddy, ge_mipBiasParams.x);
    vec3 c2 = CBT_LAYER_TAP(slot, tex, st2, ddx, ddy, ge_mipBiasParams.x);
    vec3 c3 = CBT_LAYER_TAP(slot, tex, st3, ddx, ddy, ge_mipBiasParams.x);
    // Luminance-falloff contrast blend (the paper's de-ghosting): plain barycentric mixing
    // triple-exposes the unaligned copies near tile borders; weighting by luminance and
    // sharpening with w^7 keeps one copy locally dominant with organic transitions.
    const vec3 kLum = vec3(0.299, 0.587, 0.114);
    vec3 dw = vec3(dot(c1, kLum), dot(c2, kLum), dot(c3, kLum));
    dw = mix(vec3(1.0), dw, CBT_HEX_FALLOFF);
    vec3 w = dw * pow(sx.w, vec3(CBT_HEX_EXP));
    w /= (w.x + w.y + w.z);
    return w.x * c1 + w.y * c2 + w.z * c3;
}

// One triplanar projection tap: hex-tiled when the layer opts in, plain REPEAT otherwise.
// hexRot is a palette value, so the branch is dynamically uniform per draw.
//
// ge_mipBiasParams.x (ViewParams) rides every layer-colour tap here and in CBT_HexTileTap: these
// taps construct their sampler directly, so they bypass the adapter's material-alias overload that
// applies the per-view TAAU sharpness compensation everywhere else. Without it, terrain would be
// the one surface sampling un-biased under an upscaled render scale. Bias is an implicit-derivative
// tap, so it stays clear of the explicit-LOD family documented as driver-faulting above. The
// splat/normal/atlas CLAMP samples elsewhere are data lookups, not detail colour, and stay unbiased.
vec3 CBT_ProjTap(uint slot, uint tex, float hexRot, vec2 uv, vec2 ddx, vec2 ddy)
{
    if (hexRot > 0.0)
        return CBT_HexTileTap(slot, tex, hexRot, uv, ddx, ddy);
    return CBT_LAYER_TAP(slot, tex, uv, ddx, ddy, ge_mipBiasParams.x);
}

// --- Untextured variation ----------------------------------------------------------------------
// Cheap 3D value noise (hash lattice, trilinear) for the untextured fallback variation. This is NOT
// the relief fBM (that is analytic + gradient-exact and drives geometry); it only breaks up colour.
float CBT_Hash31(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}
float CBT_ValueNoise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = CBT_Hash31(i + vec3(0.0, 0.0, 0.0));
    float n100 = CBT_Hash31(i + vec3(1.0, 0.0, 0.0));
    float n010 = CBT_Hash31(i + vec3(0.0, 1.0, 0.0));
    float n110 = CBT_Hash31(i + vec3(1.0, 1.0, 0.0));
    float n001 = CBT_Hash31(i + vec3(0.0, 0.0, 1.0));
    float n101 = CBT_Hash31(i + vec3(1.0, 0.0, 1.0));
    float n011 = CBT_Hash31(i + vec3(0.0, 1.0, 1.0));
    float n111 = CBT_Hash31(i + vec3(1.0, 1.0, 1.0));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

// The value-noise variation must FADE toward the flat tint as a pixel's world footprint approaches
// the noise cell: past Nyquist the lattice can only alias (distant shimmer) or, on a grazing/steep
// wall, read as coarse tonal BANDS (round-8e "horizontal cliff banding"). Fade over this band of
// cells-per-pixel — full detail below Lo, gone by Hi. The footprint is PASSED IN (computed in uniform
// control flow): fwidth() here would be undefined behaviour inside the divergent per-layer loop.
const float kVariationFadeLo = 0.35; // cells/pixel below which variation is full strength
const float kVariationFadeHi = 1.2;  // cells/pixel at which variation has faded to the flat tint
// Fixed rotation of the sample lattice off the world axes. Value noise is an axis-aligned integer
// lattice, so on a world-axis-aligned wall its cells stack into HORIZONTAL bands (the terracing in the
// round-8e report). Rotating the sample coordinate breaks that alignment so the break-up reads as
// organic mottle instead. Orthonormal (0.9 rad about (1,2,3)/|.|), so it does not scale frequency.
const mat3 kVariationRot = mat3(
    +0.64864, +0.68211, -0.33762,
    -0.57400, +0.72972, +0.37152,
    +0.49979, -0.04719, +0.86486);

// Hue-preserving value-noise variation (brightness + saturation jitter) that breaks up the flat tint
// without shifting hue. The old term added three INDEPENDENT noises to R/G/B, which pushed grass
// toward magenta/purple (round-8e "purple grass"); jittering SATURATION along the texel's own hue
// (mix against its luminance) instead keeps green grass green and grey rock grey. footprintWS is the
// world-space pixel footprint (m) used for the Nyquist fade above. strength/hue 0 => flat tint.
vec3 CBT_ApplyMaterialVariation(vec3 base, vec3 posWS, TerrainMaterialRecordData mat, float footprintWS)
{
    if (mat.VariationScale <= 0.0 || (mat.VariationStrength <= 0.0 && mat.VariationHue <= 0.0))
        return base;
    // cells-per-pixel = footprint / cellSize = footprint * scale. A zero footprint (planar fallback /
    // unknown) leaves fade at 1 — full variation, byte-identical to the no-fade path.
    float cellsPerPixel = footprintWS * mat.VariationScale;
    float fade = 1.0 - smoothstep(kVariationFadeLo, kVariationFadeHi, cellsPerPixel);
    if (fade <= 0.0)
        return base;
    vec3 p = kVariationRot * (posWS * mat.VariationScale);
    // Three-octave fBM (normalized to [0,1]) rather than a single dominant frequency. A lone low
    // octave reads as coherent tonal BANDS on a wall (round-8e terracing); stacking a fractal of
    // octaves breaks that into organic mottle with no dominant band. Brightness is a uniform multiply,
    // so it is hue-preserving on its own.
    float n = (CBT_ValueNoise(p) + 0.5 * CBT_ValueNoise(p * 4.17 + 7.3) +
               0.25 * CBT_ValueNoise(p * 17.4 + 19.1)) * (1.0 / 1.75);
    float brightness = 1.0 + fade * mat.VariationStrength * (n * 2.0 - 1.0);
    vec3 col = base * brightness;
    // Saturation jitter along the texel's own hue axis (grey <-> saturated) — a decorrelated octave
    // drives it; mixing against luminance keeps the hue fixed, so no off-colour casts.
    float nc = CBT_ValueNoise(p * 1.7 + 11.3);
    float sat = 1.0 + fade * mat.VariationHue * (nc * 2.0 - 1.0);
    float lum = dot(col, vec3(0.2126, 0.7152, 0.0722));
    return max(mix(vec3(lum), col, sat), vec3(0.0));
}

// --- The albedo ---------------------------------------------------------------------------------
// The three projection UVs one material's taps share, derived ONCE per material so its albedo,
// normal and ORM land on exactly the same texels. Three independent derivations would agree today
// and decorrelate the moment one of them drifted — a normal map sliding against the colour it is
// supposed to be the relief of.
struct CBTMaterialUV
{
    vec2 XY; // Z-facing plane
    vec2 XZ; // Y-facing plane — flat ground's single tap
    vec2 YZ; // X-facing plane
    // Screen-space gradients of the three UV sets, for the explicit-gradient compat tap. Carried
    // on the struct because they must be taken HERE, in uniform control flow — the taps that
    // consume them sit inside weight branches. Zero and unread on the bindless profile, whose tap
    // takes an implicit derivative with a bias instead.
    vec2 dXYdx; vec2 dXYdy;
    vec2 dXZdx; vec2 dXZdy;
    vec2 dYZdx; vec2 dYZdy;
    // A Planar material's single UV set: the terrain's footprint UV, rescaled from the extent the
    // terrain's textures cover to its authored size (PlanarUVScaleX/Z, 1 unless a tiled terrain's
    // tile grid overhangs its size), times the material's Tiling, which for Planar means repeats
    // across the footprint (1 = one image over the whole terrain).
    // Neither the terrain's metre-based MaterialTiling nor the world-anchoring phase applies: the
    // footprint UV is already anchored to the terrain.
    //
    // V IS FLIPPED: image row 0 lies on the terrain's LARGEST-Z edge. The world is +Z north and an
    // image file stores its north row first, so a north-up orthophoto lands unmirrored; without
    // the flip it lands reflected north to south. U runs along +X, V along -Z, and the Planar
    // normal arm folds on exactly that frame (CBT_MaterialNormal).
    //
    // CLAMPED to the footprint before tiling: past the authored size (a tiled terrain's tile-grid
    // overhang) the image holds its edge texels instead of wrapping around.
    vec2 Planar;
    vec2 dPlanardx; vec2 dPlanardy;
    // Whether the Planar image covers the footprint at most once (Tiling <= 1), in which case it is
    // sampled clamp-to-edge so the borders and the overhang hold the image's own edge texels. Above
    // 1 it repeats and samples REPEAT.
    bool PlanarEdgeClamp;
};

CBTMaterialUV CBT_MaterialUVs(TerrainMaterialRecordData mat, CBTTriplanarCtx tc, float tiling)
{
    float t = tiling * (mat.Tiling > 0.0 ? mat.Tiling : 1.0);
    vec3 phase = vec3(mat.UVPhaseX, mat.UVPhaseY, mat.UVPhaseZ);
    CBTMaterialUV uv;
    uv.XY = tc.pos.xy * t + phase.xy;
    uv.XZ = tc.pos.xz * t + phase.xz;
    uv.YZ = tc.pos.yz * t + phase.yz;
    // Scaled from the context's gradients rather than taken here: this function runs inside the
    // caller's weight-gated blend loop, and a derivative there is non-uniform control flow. The
    // UV phase is a constant offset, so it does not appear.
    uv.dXYdx = tc.dposdx.xy * t; uv.dXYdy = tc.dposdy.xy * t;
    uv.dXZdx = tc.dposdx.xz * t; uv.dXZdy = tc.dposdy.xz * t;
    uv.dYZdx = tc.dposdx.yz * t; uv.dYZdy = tc.dposdy.yz * t;
    float planarTiling = mat.Tiling > 0.0 ? mat.Tiling : 1.0;
    vec2 footprintScale = vec2(mat.PlanarUVScaleX, mat.PlanarUVScaleZ);
    vec2 footprint = clamp(tc.terrainUV * footprintScale, vec2(0.0), vec2(1.0));
    uv.Planar = vec2(footprint.x, 1.0 - footprint.y) * planarTiling;
    vec2 planarRate = vec2(footprintScale.x, -footprintScale.y) * planarTiling;
    uv.dPlanardx = tc.dUVdx * planarRate; uv.dPlanardy = tc.dUVdy * planarRate;
    uv.PlanarEdgeClamp = planarTiling <= 1.0;
    return uv;
}

// One Planar tap at the footprint UV: clamp-to-edge when the image covers the terrain once, REPEAT
// when it tiles, hex-tiled when the layer opts in (which needs REPEAT: the lattice offsets wrap).
// Every branch is a record value, so the selection is dynamically uniform per draw.
vec3 CBT_PlanarTap(uint slot, uint tex, float hexRot, CBTMaterialUV uv)
{
    if (hexRot > 0.0)
        return CBT_HexTileTap(slot, tex, hexRot, uv.Planar, uv.dPlanardx, uv.dPlanardy);
    if (uv.PlanarEdgeClamp)
        return CBT_LAYER_TAP_EDGE(slot, tex, uv.Planar, uv.dPlanardx, uv.dPlanardy,
                                  ge_mipBiasParams.x);
    return CBT_LAYER_TAP(slot, tex, uv.Planar, uv.dPlanardx, uv.dPlanardy, ge_mipBiasParams.x);
}

// The terrain's global tiling as a UV RATE. The fallback is the shipped default rather than a
// vanishing epsilon: an unset MaterialTiling that divided by 0.001 would tile a thousand times
// finer than the terrain it stands on, which reads as noise rather than as a missing setting.
float CBT_MaterialUVTiling(float materialTiling)
{
    return 1.0 / (materialTiling > 0.0 ? materialTiling : 10.0);
}

// A material's albedo: its bound texture (slope-sharpened, weight-floored triplanar, or one tap at
// the footprint UV for a Planar material) multiplied by the authored tint, else the tint with
// procedural variation. AlbedoTex == 0 is the bindless
// sentinel, so "textured or not" needs no variant. A library entry's tint is the live multiplier
// (white = the texture unmodified); the legacy per-layer path authors white for textured layers,
// so both meet this expression correctly. posWS anchors the untextured value-noise variation
// only; the texture path derives every UV from the precision-safe context.
vec3 CBT_MaterialAlbedo(uint layerOrd, TerrainMaterialRecordData mat, CBTTriplanarCtx tc,
                        CBTMaterialUV uv, vec3 posWS, float footprintWS)
{
    vec3 tint = vec3(mat.AlbedoR, mat.AlbedoG, mat.AlbedoB);
    if (!CBT_MAT_HAS_ALBEDO(mat))
        return CBT_ApplyMaterialVariation(tint, posWS, mat, footprintWS);

    const uint slot = CBT_LAYER_SLOT(layerOrd, 0u); // 0 == albedo
    // Planar keeps the layer's hex opt-in as authored: hex tiling shuffles the image, which is what
    // a tiling texture wants and what a basemap must not have, so it is off unless the author set it.
    if (CBT_MatIsPlanar(mat))
        return CBT_PlanarTap(slot, mat.AlbedoTex, mat.HexRotStrength, uv) * tint;
    vec3 w = tc.weights;
    vec3 col = vec3(0.0);
    if (w.z > 0.0) // Z-facing plane (XY projection)
        col += w.z * CBT_ProjTap(slot, mat.AlbedoTex, mat.HexRotStrength, uv.XY, uv.dXYdx, uv.dXYdy);
    if (w.y > 0.0) // Y-facing plane (XZ projection — flat ground's single tap)
        col += w.y * CBT_ProjTap(slot, mat.AlbedoTex, mat.HexRotStrength, uv.XZ, uv.dXZdx, uv.dXZdy);
    if (w.x > 0.0) // X-facing plane (YZ projection)
        col += w.x * CBT_ProjTap(slot, mat.AlbedoTex, mat.HexRotStrength, uv.YZ, uv.dYZdx, uv.dYZdy);
    return col * tint;
}

#endif // GE_TERRAIN_MATERIAL_ALBEDO_DECLARED
