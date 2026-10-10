// The motion target's payload, in one definition. FRAGMENT STAGE ONLY — the
// current endpoint is the fragment's own rasterized position.
//
// Two producers write this texture — the per-instance mover pass
// (taa_motion_vectors.frag) and the composed deforming variant
// (adapter_forward.glsl under GE_MOTION_VECTORS) — and both consumers, the
// temporal resolve and the screen-space reflection reprojection, decode one
// encoding from it. A second spelling of the encode is a second meaning for the
// same texels. The mover fragment still carries its own copy inline; it moves
// onto this definition with the producer slice, which reshapes that pass's
// target ownership anyway.
//
// Payload: rg = viewport-UV delta from the previous position to this one,
// b = previous NDC depth, a = 1 meaning "an exact motion vector exists here".
// The unwritten value is the sentinel the target is cleared to (taa_resolve.comp
// kMVSentinel) — a delta far outside the viewport with a = 0, which both
// consumers read as "no exact motion here, reproject analytically".

#ifndef GE_MOTION_VECTOR_PAYLOAD_GLSL
#define GE_MOTION_VECTOR_PAYLOAD_GLSL

#include "screen_position.glsl"

const vec4 GE_MOTION_VECTOR_SENTINEL = vec4(100.0, 100.0, 0.0, 0.0);

// The current endpoint is NOT an argument. A clip position interpolated to a
// fragment is that fragment's own position, so gl_FragCoord already carries it,
// exactly and in the space the consumer indexes by: the consumer reads this
// texel and reprojects `texelUv - delta`, which lands on the previous surface
// only when the delta was formed against this texel and not against a second
// projection of the same vertex. The raster position is jittered and the payload
// must not be, hence jitterUv — the per-view temporal jitter as a viewport-UV
// offset, a constant across the viewport because the jitter is a constant NDC
// offset applied after projection.
//
// prevClip is the previous endpoint's UNJITTERED clip position. A non-positive w
// is a vertex behind the eye of its own frame: there is no usable previous
// surface, so the fragment retains the sentinel rather than exporting a delta
// built from a mirrored projection. The current endpoint needs no such guard —
// a fragment exists only where the clip volume already admitted it.
vec4 GE_MotionVectorPayload(GE_ScreenRect viewport, vec2 jitterUv, vec4 prevClip)
{
    if (prevClip.w <= 1e-6)
        return GE_MOTION_VECTOR_SENTINEL;

    vec2 currUv = GE_PixelPositionToViewportUV(gl_FragCoord.xy, viewport) - jitterUv;
    vec2 prevUv = GE_YUpNdcToViewportUV(prevClip.xy / prevClip.w);
    return vec4(currUv - prevUv, prevClip.z / prevClip.w, 1.0);
}

#endif // GE_MOTION_VECTOR_PAYLOAD_GLSL
