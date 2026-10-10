// The per-fragment terrain blend resolve: what a splatmap's four channels become before any
// material is sampled. SHARED by the two surfaces that shade one terrain — the CBT ground
// (CBT/cbt_surface.glsl) and the grass blades standing on it
// (TerrainGrass/terrain_grass_surface.glsl) — because a blade's base tint IS the ground colour
// under it. Resolving the weights differently on the two sides makes a blade disagree with its own
// ground wherever four materials meet or one material is named by two channels.
//
// Pure weight arithmetic: no samplers, no bindless arrays, no globals. That is what lets the marked
// block below also compile as C++ on the host.
//
// THE INCLUDER DEFINES THE WIDTH. CBT_MAX_BLEND_MATERIALS must be defined before this file is
// included, and is deliberately NOT defaulted here: a default is a second copy of a shipping knob,
// and the copy that goes wrong is the one nobody reads. Includes/terrain_blend_width.glsl carries
// the single shipped value (CBTLayout.SurfaceBlendsThreeMaterialsPerFragment pins it), and both
// terrain surfaces include that file ahead of this one.
//
// THE GUARD AND THE WIDTH CHECK STAY OUTSIDE THE MARKERS. The marked block is extracted verbatim by
// ExtractShaderBlock.cmake and included THREE TIMES in ONE C++ translation unit by
// CBTMaterialBlendTests — once per width, each in its own namespace. A guard inside the markers
// would let only the FIRST of those three includes expand: the second and third namespaces would
// come out empty, and the suite would fail to compile against functions those namespaces do not
// hold. Loud rather than silent — but it would make the block unusable at more than one width.
#ifndef GE_TERRAIN_BLEND_RESOLVE_DECLARED
#define GE_TERRAIN_BLEND_RESOLVE_DECLARED

#ifndef CBT_MAX_BLEND_MATERIALS
#error "Includes/terrain_blend_resolve.glsl: define CBT_MAX_BLEND_MATERIALS before including it"
#endif

// GE_SHARED_BLEND_SELECTION_BEGIN
// A channel this light contributes less than a quantization step of the result, so it is dropped
// rather than fetched. Applied once, before the fold below, so that folding can never lift a pair
// of negligible weights back over the threshold.
const float CBT_MATERIAL_WEIGHT_FLOOR = 0.001f;

// Fold splat channels that resolve to the SAME material record into one contribution.
//
// LayerRole is a per-draw indirection and nothing forbids repeats — a partially authored
// terrain routinely points several roles at one record. Blending that material once per channel
// samples its albedo, normal and ORM once per channel for a result arithmetically identical to a
// single contribution at the summed weight. Folding first is therefore lossless in output and
// strictly cheaper in taps: same weights, same pixels, fewer fetches.
//
// Both loop bounds are compile-time constants, so this unrolls to a fixed comparison chain and the
// indices resolve to constants — no dynamically indexed scratch array.
vec4 CBT_FoldDuplicateRoles(vec4 weights, uint roles[4])
{
    for (int i = 0; i < 4; ++i)
    {
        if (weights[i] < CBT_MATERIAL_WEIGHT_FLOOR)
            weights[i] = 0.0f;
    }
    for (int i = 1; i < 4; ++i)
    {
        if (weights[i] <= 0.0f)
            continue;
        for (int j = 0; j < i; ++j)
        {
            if (weights[j] > 0.0f && roles[j] == roles[i])
            {
                weights[j] += weights[i];
                weights[i] = 0.0f;
                break;
            }
        }
    }
    return weights;
}

// Keep the CBT_MAX_BLEND_MATERIALS heaviest contributions, fade each by the weight of the
// strongest contender it beat, and renormalize the remainder to sum 1.
//
// THE SUBTRACTION IS WHAT REMOVES THE SEAM, and it is not optional. A plain top-K is discontinuous:
// along the contour where the K-th and (K+1)-th weights are equal, the surviving SET changes while
// the weights themselves do not, so the shaded colour steps by that shared weight times the
// difference between the two materials — a hard C0 edge running along the contour. Subtracting the
// heaviest DROPPED weight sends both candidates to exactly zero on that contour: the one that won
// contributes nothing there, the one that lost is absent, and which of them was picked stops
// mattering. The blend is continuous across every swap.
//
// It is free in bandwidth. The same K materials are sampled either way — only their weights change.
//
// Renormalization keeps the surface from darkening: the reduced weights no longer sum to 1, so the
// survivors are rescaled rather than left short.
//
// The fallback covers the one input the subtraction cannot: every survivor tied with the strongest
// dropped weight (a fragment whose channels are all exactly equal), which reduces the whole blend
// to zero. There the unreduced survivors are renormalized instead, so the pixel shades its
// materials evenly rather than going black. That input is an isolated point rather than a contour —
// approaching it from any direction the dominant material already wins outright — so resolving it
// by symmetry costs no continuity anywhere a terrain actually lands.
//
// Selection is by repeated max rather than a sort — K and the channel count are both small
// compile-time constants, so this unrolls to a fixed comparison chain. The dynamic component
// indices land on a vec4, which SPIR-V inserts and extracts natively; a float[4] here would spill
// the array to per-invocation scratch instead.
vec4 CBT_SelectTopWeights(vec4 weights)
{
    vec4 kept = vec4(0.0f);
    float sum = 0.0f;
    for (int k = 0; k < CBT_MAX_BLEND_MATERIALS; ++k)
    {
        int best = -1;
        float bestW = 0.0f;
        for (int i = 0; i < 4; ++i)
        {
            if (weights[i] > bestW)
            {
                bestW = weights[i];
                best = i;
            }
        }
        if (best < 0)
            break; // fewer contributors than K — nothing left to keep
        kept[best] = bestW;
        sum += bestW;
        weights[best] = 0.0f; // consumed, so the next pass finds the next-heaviest
    }

    // Whatever still stands in `weights` lost. The heaviest of them is the level at which the
    // weakest survivor would have been displaced, and so the level every survivor fades from.
    float dropMax = 0.0f;
    for (int i = 0; i < 4; ++i)
        dropMax = max(dropMax, weights[i]);

    vec4 soft = vec4(0.0f);
    float softSum = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        soft[i] = max(kept[i] - dropMax, 0.0f);
        softSum += soft[i];
    }

    if (softSum > 0.0f)
        return soft / softSum;
    return sum > 0.0f ? kept / sum : kept;
}

// The per-fragment weight resolve: what a splat's four channels become before anything is sampled.
//
// THE ORDER IS PART OF THE CONTRACT, which is why the two steps are composed here rather than
// called in sequence by the caller. Folding must precede ranking: a material two channels both name
// competes on its combined weight, and ranked first it would be judged on each half separately and
// can lose its place to a lighter material — or, once the soft cut reduces it against its own other
// half, vanish from the blend entirely. Composed here, the order is covered by the tests that
// execute this function; spelled out at the call site, it was not.
vec4 CBT_ResolveBlendWeights(vec4 weights, uint roles[4])
{
    return CBT_SelectTopWeights(CBT_FoldDuplicateRoles(weights, roles));
}
// GE_SHARED_BLEND_SELECTION_END

#endif // GE_TERRAIN_BLEND_RESOLVE_DECLARED
