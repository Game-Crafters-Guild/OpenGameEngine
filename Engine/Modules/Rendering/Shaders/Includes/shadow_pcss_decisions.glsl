// PCSS decisions as pure functions of their inputs: no resources, no derivatives,
// so the shadow sampler (shadow_sampling.glsl) and its compute probe
// (shadow_pcss_probe.comp, ShadowPcssDecisionTests) share one definition.
//
// Depths are raw reverse-Z shadow-map depths: nearer the light is LARGER.
// Offsets are in shadow-map UV; `rpBias` is the receiver plane's depth gradient
// per UV (zero when the receiver-plane bias is off).

#ifndef GE_SHADOW_PCSS_DECISIONS_GLSL
#define GE_SHADOW_PCSS_DECISIONS_GLSL

// Whether a raw depth at a tap `offsetUV` from the receiver centre occludes the
// receiver there. The reference is the receiver plane at the tap, the one the
// PCF loop compares against, so a sloped receiver's own surface inside the
// search disk is not its own blocker.
bool GE_PcssIsBlocker(float tapDepth, float refDepth, vec2 offsetUV, vec2 rpBias)
{
    return tapDepth > refDepth + dot(offsetUV, rpBias);
}

// Whether the pyramid's maximum raw depth over the widest kernel (`capUV`
// radius) proves every PCF tap in it lit. Each tap compares against the receiver
// plane at the tap, so the lowest reference in the kernel is refDepth -
// capUV * (|gradient.x| + |gradient.y|); no depth above that means no tap of any
// kernel up to the cap finds an occluder.
bool GE_PcssKernelProvesLit(float kernelMaxDepth, float refDepth, float capUV, vec2 rpBias)
{
    return kernelMaxDepth <= refDepth - capUV * (abs(rpBias.x) + abs(rpBias.y));
}

// The blocker-search radius in texels, bounded by the widest kernel the filter
// can pick (`capTexels`) but never below `minTexels`, the narrowest disk whose
// taps still average more than one texel.
float GE_PcssBoundedSearchTexels(float searchTexels, float capTexels, float minTexels)
{
    return min(searchTexels, max(capTexels, minTexels));
}

// The PCF kernel radius in texels from the blocker search's average blocker
// depth (negative when it found none): the physical penumbra, depth delta in
// world units times tan(half angle), clamped to [one texel, the world cap]. A
// search that found nothing has no depth delta, so it gets the minimum kernel
// and the filter still decides the result for a blocker inside the kernel the
// sparse search missed.
float GE_PcssPenumbraTexels(float avgBlocker, float refDepth, float depthSpanWorld, float tanHalfAngle,
                            float worldPerTexel, float capWorld)
{
    float penumbraWorld = max(0.0, avgBlocker - refDepth) * depthSpanWorld * tanHalfAngle;
    penumbraWorld = clamp(penumbraWorld, worldPerTexel, max(capWorld, worldPerTexel));
    return penumbraWorld / max(worldPerTexel, 1e-6);
}

#endif // GE_SHADOW_PCSS_DECISIONS_GLSL
