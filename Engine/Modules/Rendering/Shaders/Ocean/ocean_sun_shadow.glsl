#ifndef GE_OCEAN_SUN_SHADOW_GLSL
#define GE_OCEAN_SUN_SHADOW_GLSL
// Sun visibility at a world position from the engine's cascaded shadow map,
// shared by the temporal water shadows and the spray lighting. The includer
// declares the ShadowData prefix block (shadowVP[4], shadowSplits, shadowParams)
// and `shadowMap` as a sampler2DArrayShadow. Reverse-Z: the receiver bias in
// shadowParams.x is added to the NDC depth, and the comparison is GreaterOrEqual.
float OceanSunVisibility(vec3 world)
{
    for (int c = 0; c < clamp(int(shadowParams.z), 0, 4); ++c)
    {
        vec4 clip = shadowVP[c] * vec4(world, 1);
        vec3 ndc = clip.xyz / max(abs(clip.w), 1e-6);
        vec2 uv = ndc.xy * 0.5 + 0.5;
        uv.y = 1.0 - uv.y;
        if (all(greaterThan(uv, vec2(0.005))) && all(lessThan(uv, vec2(0.995))) && ndc.z > 0 && ndc.z < 1)
            return textureGrad(shadowMap, vec4(uv, float(c), ndc.z + shadowParams.x), vec2(0), vec2(0));
    }
    return 1.0;
}
#endif
