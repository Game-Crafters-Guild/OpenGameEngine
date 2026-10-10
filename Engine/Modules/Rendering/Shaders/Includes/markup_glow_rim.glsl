// The world mark-up glow's rim terms (markup_glow.frag), shared with the host: the block between
// the markers is extracted at build time and compiled as C++ through GlslShim.h by EditorTests,
// so the test runs this code. Written component-wise with f-suffixed literals for the shim.

// GE_SHARED_MARKUP_GLOW_RIM_BEGIN
// A box's rim band: it fades out over this fraction of the face's smaller half extent from the
// face's edges.
const float kMarkupGlowBoxRimWidth = 0.25f;

// How strongly the rim lights a surface point from its facing term: 0 where the point faces
// fully (facing = 1), rising toward facing = 0 as (1 - facing)^power, times gain, and saturated
// at 1 so a highlight gain widens the rim without brightening it past the rim budget. A sphere's
// facing is n.v (a fresnel rim), a box's MarkupGlowBoxEdgeFacing (an edge rim).
float MarkupGlowRim(float facing, float power, float gain)
{
    float clamped = clamp(facing, 0.0f, 1.0f);
    return min(pow(1.0f - clamped, power) * gain, 1.0f);
}

// A box face's facing term at the face point (u, v) in [-1, 1], for a face of half extents
// (halfU, halfV) in meters: 0 on the face's nearest edge (and at a corner), rising linearly to 1
// at kMarkupGlowBoxRimWidth of the smaller half extent inside the face. A flat face has one n.v,
// so its rim comes from the distance to its edges instead.
float MarkupGlowBoxEdgeFacing(float u, float v, float halfU, float halfV)
{
    float distance = min((1.0f - abs(u)) * halfU, (1.0f - abs(v)) * halfV);
    float width = max(kMarkupGlowBoxRimWidth * min(halfU, halfV), 0.0001f);
    return clamp(distance / width, 0.0f, 1.0f);
}
// The contact line where a volume meets scene geometry. Its band is at least
// kMarkupGlowContactBandMeters of view depth deep, and at least kMarkupGlowContactBandPixels of
// screen wherever the gap between the body and the scene changes faster than that per pixel, so
// it reads at every distance. Where the scene's depth jumps (a silhouette behind the body) the gap
// changes by meters per pixel: the band stops at kMarkupGlowContactBandMaxDepthFraction of the
// body's view depth, so a jump does not light as a contact.
const float kMarkupGlowContactBandMeters = 0.05f;
const float kMarkupGlowContactBandPixels = 2.0f;
const float kMarkupGlowContactBandMaxDepthFraction = 0.05f;

// The contact band's depth in meters at a pixel where the body-to-scene gap changes by
// `gapPerPixel` meters (fwidth) and the body lies `viewDepth` meters along the view.
float MarkupGlowContactBandWidth(float gapPerPixel, float viewDepth)
{
    float widest = max(kMarkupGlowContactBandMeters, kMarkupGlowContactBandMaxDepthFraction * viewDepth);
    return clamp(kMarkupGlowContactBandPixels * gapPerPixel, kMarkupGlowContactBandMeters, widest);
}

// How strongly the contact line lights a body point `depthGap` meters of view depth from the
// scene surface at the same pixel, for a band `width` meters deep: 1 on the intersection, falling
// smoothly to 0 at `width` either side.
float MarkupGlowContactBand(float depthGap, float width)
{
    return 1.0f - smoothstep(0.0f, width, abs(depthGap));
}
// GE_SHARED_MARKUP_GLOW_RIM_END
