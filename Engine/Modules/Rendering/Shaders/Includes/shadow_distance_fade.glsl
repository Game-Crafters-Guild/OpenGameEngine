#ifndef GE_SHADOW_DISTANCE_FADE_GLSL
#define GE_SHADOW_DISTANCE_FADE_GLSL

// Directional shadows end at MaxShadowDistance. Every directional shadow term
// (cascades, the ray-traced mask, the screen-space contact term) fades to fully
// lit over the same last fraction of that distance, so coverage ends in a
// falloff rather than a line that moves with the camera, and no term outlives
// another inside the band.

// 0 = the shadow term applies in full, 1 = fully faded (lit). Smooth from
// (1 - fadeFraction) * maxShadowDistance to maxShadowDistance,
// in view-space linear depth (world units). maxShadowDistance must be positive.
float GE_ShadowDistanceFade(float linearDepth, float maxShadowDistance, float fadeFraction)
{
    if (fadeFraction <= 0.0)
        return step(maxShadowDistance, linearDepth);
    return smoothstep(maxShadowDistance * (1.0 - fadeFraction), maxShadowDistance,
                      linearDepth);
}

// A directional shadow factor from its two kinds of term. `rangedTerm` (the
// meshes' occlusion, from the cascades or the traced rays) ends at
// maxShadowDistance, so it fades to lit over the band. `terrainTerm` (the
// terrain's clearance map, terrain_shadow.glsl: 0 or 1) has no range, so it
// joins unfaded, by a minimum, at every distance: a receiver the terrain
// shadows stays shadowed through the band and past it.
float GE_JoinTerrainAfterFade(float rangedTerm, float terrainTerm, float linearDepth,
                              float maxShadowDistance, float fadeFraction)
{
    return min(mix(rangedTerm, 1.0, GE_ShadowDistanceFade(linearDepth, maxShadowDistance, fadeFraction)), terrainTerm);
}

#endif // GE_SHADOW_DISTANCE_FADE_GLSL
