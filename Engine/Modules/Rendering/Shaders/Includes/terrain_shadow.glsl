// Terrain shadows from the sun-space clearance map (issue #2699).
//
// The map is a grid laid along the sun's horizontal direction: axis u points toward the sun, axis
// v across it, one line per v. Each sample stores its clearance: the height by which the terrain
// upstream (larger u) rises above the sun's ray through the sample, measured from the ground
// there. Positive clearance is shadow up to that height above the ground; negative clearance is
// the margin by which the ground clears the sun, so lit ground needs no self-shadow bias. Beside
// it each sample stores its occluder's distance: the horizontal distance (metres along u) to the
// terrain that sizes its penumbra (GE_TerrainShadowSampleValue: in shadow the caster of the shadow
// volume's top; lit, the caster that blocks the sun disc's lower edge most, whose margin then
// replaces the clearance, both negative).
//
// Along one line the top of the shadow volume is a suffix maximum:
//   top(k) = max over j > k of (h(j) - (j - k) * texel * tan(elevation))
// so the bake (terrain_shadow_bake.comp) is one scan per line, linear in the samples and
// independent of the sun's elevation, and a receiver needs one bilinear read and a compare. The scan
// carries a second maximum for the ray through the sun disc's lower edge, which finds the occluder
// of a lit receiver's penumbra: the largest margin under the centre ray there is usually the ground
// just upwind, which says nothing about the ridge whose penumbra the receiver stands in.
//
// The penumbra is the uniform disc of the sun (angular radius a) cut by the edge ray over the
// occluder, the model the mesh shadows use (the cascades' PCSS disc, the ray-traced mask's cone).
// For a receiver whose margin under the shadow volume's top is m metres (vertically) and whose
// occluder is D metres away horizontally, the edge ray passes m * cos(e) from the receiver across
// the ray, and the sun's disc there has the radius r = (D / cos(e)) * tan(a): the receiver is fully
// shadowed at m * cos(e) >= r and fully lit at m * cos(e) <= -r. Across the penumbra the margin
// changes by 2 * D * tan(a) / cos^2(e) metres, and on level ground the penumbra is
// 2 * r / sin(e) metres wide.
//
// The block between the markers is compiled by the shaders and, through GlslShim, by
// CBTTerrainTests, so it uses only what the shim provides: no out parameters, no samplers.
// The includer defines, before this file:
//   vec2 GE_TerrainShadowReadClearance(vec2 gridCoord)   bilinear read of the map at a grid
//                                                         position in samples (clamped in range):
//                                                         x = margin, y = occluder distance
//   float GE_TerrainShadowReadGround(float x, float z)    the terrain's height at x, z relative to
//                                                         its base, clamped to the terrain's edge,
//                                                         the surface the terrain renders
// All positions share one frame (the grid's centre and the terrain's corner are given in it);
// heights are metres above the terrain's base height.

// GE_SHARED_TERRAIN_SHADOW_BEGIN
struct GE_TerrainShadowGrid
{
    // The grid's centre and the terrain's corner and extent, in the receivers' frame (x, z).
    float CenterX;
    float CenterZ;
    float TerrainX;
    float TerrainZ;
    float TerrainSizeX;
    float TerrainSizeZ;
    // The sun's horizontal direction (unit length): the u axis. v = (-SunZ, SunX).
    float SunX;
    float SunZ;
    // tan of the sun's elevation, metres of rise per metre along u.
    float TanElevation;
    // Metres between samples, and the first sample's u and v.
    float Texel;
    float UMin;
    float VMin;
    // Samples per line and lines in use.
    float SamplesU;
    float SamplesV;
};

// A sample with no caster (outside the terrain), and the magnitude the map can hold (RG16F; the
// limit bounds both the clearance and the occluder distance, in metres).
const float kGE_TerrainShadowNoCaster = -1.0e30f;
const float kGE_TerrainShadowClearanceLimit = 60000.0f;
// The smallest penumbra radius (metres across the ray), so an occluder at the receiver (a facet's
// own terminator) gives a hard edge without dividing by zero.
const float kGE_TerrainShadowMinPenumbraRadius = 1.0e-4f;
// The penumbra radius bound for a caller whose meshes bound theirs by nothing (the ray-traced mask).
const float kGE_TerrainShadowUncappedPenumbra = 1.0e30f;
const float kGE_TerrainShadowPi = 3.14159265f;

// The grid position, in samples, of the point (x, z).
vec2 GE_TerrainShadowGridCoord(GE_TerrainShadowGrid g, float x, float z)
{
    float dx = x - g.CenterX;
    float dz = z - g.CenterZ;
    float u = dx * g.SunX + dz * g.SunZ;
    float v = dz * g.SunX - dx * g.SunZ;
    return vec2((u - g.UMin) / g.Texel, (v - g.VMin) / g.Texel);
}

// The point (x, z) of the grid position `gridCoord` (in samples).
vec2 GE_TerrainShadowGridPoint(GE_TerrainShadowGrid g, vec2 gridCoord)
{
    float u = g.UMin + gridCoord.x * g.Texel;
    float v = g.VMin + gridCoord.y * g.Texel;
    return vec2(g.CenterX + u * g.SunX - v * g.SunZ, g.CenterZ + u * g.SunZ + v * g.SunX);
}

// 1 when (x, z) lies on the terrain (its casters and its ground), else 0.
float GE_TerrainShadowOnTerrain(GE_TerrainShadowGrid g, float x, float z)
{
    float tx = (x - g.TerrainX) / g.TerrainSizeX;
    float tz = (z - g.TerrainZ) / g.TerrainSizeZ;
    return (tx >= 0.0f && tx <= 1.0f && tz >= 0.0f && tz <= 1.0f) ? 1.0f : 0.0f;
}

// Sample k of `line`: x = the height that blocks the sun there (kGE_TerrainShadowNoCaster off the
// terrain or past the line's last sample), y = the ground its clearance is measured from (the
// terrain's height there, clamped to its edge off it).
vec2 GE_TerrainShadowCasterAndGround(GE_TerrainShadowGrid g, float k, float line)
{
    vec2 p = GE_TerrainShadowGridPoint(g, vec2(k, line));
    float ground = GE_TerrainShadowReadGround(p.x, p.y);
    float casts = (k <= g.SamplesU - 1.0f && GE_TerrainShadowOnTerrain(g, p.x, p.y) > 0.5f) ? 1.0f : 0.0f;
    return vec2(casts > 0.5f ? ground : kGE_TerrainShadowNoCaster, ground);
}

// A carry of the scan, seen from one sample: x = the shadow volume's top for the ray through the sun's
// centre, y = the horizontal distance to the caster that sets it; z, w = the same for the ray through
// the sun disc's lower edge (slope `tanLower`). Of two carries seen from the same sample the higher top
// wins, ray by ray (the carries' maximum); on a tie `a` is kept, so every caller passes the nearer
// carry as `a` and a tie keeps the nearer caster.
vec4 GE_TerrainShadowHigherCarry(vec4 a, vec4 b)
{
    bool centre = b.x > a.x;
    bool lower = b.z > a.z;
    return vec4(centre ? b.x : a.x, centre ? b.y : a.y, lower ? b.z : a.z, lower ? b.w : a.w);
}

// A caster's own carry at its sample: its height on both rays, at distance 0.
vec4 GE_TerrainShadowCasterCarry(float caster)
{
    return vec4(caster, 0.0f, caster, 0.0f);
}

// A carry seen `samplesDownwind` samples downwind of the sample it was seen from: each ray's top
// falls by its slope, and its caster recedes, per metre. The operator the line's runs are combined
// with, and one step of the walk.
vec4 GE_TerrainShadowCarryDownwind(GE_TerrainShadowGrid g, vec4 carry, float samplesDownwind, float tanLower)
{
    float metres = samplesDownwind * g.Texel;
    return vec4(carry.x - metres * g.TanElevation, carry.y + metres, carry.z - metres * tanLower, carry.w + metres);
}

// What the map stores at a sample whose ground is `ground`, from `seen`, the carry of every caster
// upwind of it: x = the margin, y = the distance of the occluder that sizes its penumbra. In shadow
// (the centre ray's top above the ground) that is the centre ray's caster: x = the clearance. Lit,
// it is the caster that blocks the disc's lower edge most, which the lit half of the penumbra
// depends on: x = that caster's margin under the centre ray, at most the clearance and so never
// positive (each sample's sign, the hard compare at a sample, is unchanged; between samples the
// bilinear zero crossing can move by up to a texel where the two rays pick different casters). With a
// point sun (`tanLower` = tan(elevation)) both are the clearance.
// One caster per side is kept, the one with the largest vertical margin, which can be a farther caster
// than the one that hides the most of the disc: a near low occluder's umbra inside a far ridge's
// penumbra reads as the ridge's penumbra (measured on a 1.5 m mound inside a 30 m ridge's penumbra,
// sun 15 degrees, uncapped: 0.325 lit where the marched disc reads 0 at a 0.53 degree sun).
vec2 GE_TerrainShadowSampleValue(GE_TerrainShadowGrid g, vec4 seen, float ground, float tanLower)
{
    float clearance = seen.x - ground;
    vec2 value = vec2(clearance, seen.y);
    if (!(clearance > 0.0f))
        value = vec2(min(seen.z - seen.w * (g.TanElevation - tanLower) - ground, clearance), seen.w);
    return vec2(clamp(value.x, -kGE_TerrainShadowClearanceLimit, kGE_TerrainShadowClearanceLimit),
                min(value.y, kGE_TerrainShadowClearanceLimit));
}

// The shadow volume's top (height above the terrain's base) at the grid position `gridCoord`, as the
// map's bilinear read weighs its four samples: their clearance plus the ground each was measured
// from; y = the occluder distance there. Off the terrain that ground is the terrain's edge height
// clamped outward, which can kink (a cliff at the edge); interpolating the clearance against the
// receiver's own ground there would misplace the shadow along the whole kink line.
vec2 GE_TerrainShadowTop(GE_TerrainShadowGrid g, vec2 gridCoord)
{
    float i0 = floor(gridCoord.x);
    float j0 = floor(gridCoord.y);
    float fx = gridCoord.x - i0;
    float fy = gridCoord.y - j0;
    vec2 p00 = GE_TerrainShadowGridPoint(g, vec2(i0, j0));
    vec2 p10 = GE_TerrainShadowGridPoint(g, vec2(i0 + 1.0f, j0));
    vec2 p01 = GE_TerrainShadowGridPoint(g, vec2(i0, j0 + 1.0f));
    vec2 p11 = GE_TerrainShadowGridPoint(g, vec2(i0 + 1.0f, j0 + 1.0f));
    float g00 = GE_TerrainShadowReadGround(p00.x, p00.y);
    float g10 = GE_TerrainShadowReadGround(p10.x, p10.y);
    float g01 = GE_TerrainShadowReadGround(p01.x, p01.y);
    float g11 = GE_TerrainShadowReadGround(p11.x, p11.y);
    float ga = g00 + (g10 - g00) * fx;
    float gb = g01 + (g11 - g01) * fx;
    vec2 read = GE_TerrainShadowReadClearance(gridCoord);
    return vec2(read.x + ga + (gb - ga) * fy, read.y);
}

// The receiver at (x, y, z) against the shadow volume: x = its margin, the metres by which the
// volume's top passes above it (positive: the terrain hides the sun's centre), y = the occluder
// distance. `onGround` is 1 for the terrain's own surface, which stands on the field whatever
// height its facet put it at, so its shadow does not follow the tessellation (one tap). Other
// receivers on the terrain keep their height, a base sunk into the terrain counting as on the
// ground (two taps); receivers off it (water beyond the terrain's edge) compare their height with
// the shadow's top (five).
vec2 GE_TerrainShadowMargin(GE_TerrainShadowGrid g, float x, float y, float z, float onGround)
{
    vec2 c = GE_TerrainShadowGridCoord(g, x, z);
    // Beside the grid or upwind of its last sample: no terrain between the receiver and the sun.
    if (c.y < 0.0f || c.y > g.SamplesV - 1.0f || c.x > g.SamplesU - 1.0f)
        return vec2(-kGE_TerrainShadowClearanceLimit, 0.0f);
    if (c.x < 0.0f)
    {
        // Downwind of the grid (off the terrain) nothing casts: the shadow volume's top falls by
        // tan(elevation) per metre from the grid's first sample on the receiver's line, and the
        // occluder recedes by the same metres.
        vec2 edge = GE_TerrainShadowTop(g, vec2(0.0f, c.y));
        float top = edge.x + c.x * g.Texel * g.TanElevation;
        return vec2(top - y, edge.y - c.x * g.Texel);
    }
    if (onGround > 0.5f)
        return GE_TerrainShadowReadClearance(c);
    if (GE_TerrainShadowOnTerrain(g, x, z) < 0.5f)
    {
        vec2 top = GE_TerrainShadowTop(g, c);
        return vec2(top.x - y, top.y);
    }
    float above = max(y - GE_TerrainShadowReadGround(x, z), 0.0f);
    vec2 read = GE_TerrainShadowReadClearance(c);
    return vec2(read.x - above, read.y);
}

// The share of the sun's disc that reaches the receiver at (x, y, z) past the terrain: 1 lit, 0 in
// full shadow. `tanHalfAngle` is tan of the sun's angular radius (0: a point sun, a hard edge, the
// one compare); `maxPenumbraRadius` bounds the penumbra's radius across the ray in metres, as the
// caller's mesh shadows bound theirs. See GE_TerrainShadowMargin for `onGround`.
float GE_TerrainShadowLit(GE_TerrainShadowGrid g, float x, float y, float z, float onGround, float tanHalfAngle,
                          float maxPenumbraRadius)
{
    vec2 margin = GE_TerrainShadowMargin(g, x, y, z, onGround);
    if (tanHalfAngle <= 0.0f)
        return margin.x > 0.0f ? 0.0f : 1.0f;
    float cosElevation = 1.0f / sqrt(1.0f + g.TanElevation * g.TanElevation);
    float radius = max(min(margin.y / cosElevation * tanHalfAngle, maxPenumbraRadius),
                       kGE_TerrainShadowMinPenumbraRadius);
    float t = margin.x * cosElevation / radius;
    if (t >= 1.0f)
        return 0.0f;
    if (t <= -1.0f)
        return 1.0f;
    // The share of a unit disc beyond a chord at signed distance t from its centre.
    return (acos(t) - t * sqrt(1.0f - t * t)) / kGE_TerrainShadowPi;
}
// GE_SHARED_TERRAIN_SHADOW_END
