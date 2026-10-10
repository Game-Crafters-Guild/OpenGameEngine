#ifndef GE_SHADOW_CASCADE_BLEND_GLSL
#define GE_SHADOW_CASCADE_BLEND_GLSL

// Cross-cascade blend. A fragment closer than the band to its cascade's far
// split also samples the next cascade and fades into it (smoothstep over the
// band), so the change of texel size at a split is a gradient, not a seam.
//
// The band is a fraction of the cascade's own depth range, in view-space linear
// depth (world units), with no world-unit floor. The trap: a band longer than
// the cascade's range covers the whole cascade, so the finer cascade never
// wins and every fragment in it evaluates the filter twice. A fixed floor does
// exactly that wherever the cascades are shallower than the floor (a scene a
// few metres across seen from a metre or two).
//
// The fraction is bounded by the cascade fit: the next cascade's box holds the
// band only as far as the slice overlap the fit adds before its split
// (kCascadeOverlapFraction, 20 % of the next cascade's range, in
// ShadowMapRenderFeature.cpp), and a lookup outside that box reads lit.
// CascadeReceiverFit.BoxesHoldTheAirInFrontOfTheReceivers pins it: 0.175
// holds, 0.2 does not at a shallow orthographic strategy camera. Within that
// bound the band is as wide as it can be, to hide the next cascade's coarser
// texels. ShadowMapRenderFeature::CascadeBlendBand mirrors this on the CPU.
const float kCascadeBlendFraction = 0.175;

// Band before the far split of a cascade `cascadeRange` deep (world units).
float GE_CascadeBlendBand(float cascadeRange)
{
    return cascadeRange * kCascadeBlendFraction;
}

#endif // GE_SHADOW_CASCADE_BLEND_GLSL
