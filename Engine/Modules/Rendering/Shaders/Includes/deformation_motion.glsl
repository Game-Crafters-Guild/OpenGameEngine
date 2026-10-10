// The per-view block of the composed motion variant (GE_MOTION_VECTORS), read
// by BOTH stages.
//
// A motion vector for a vertex-deformed surface is the difference between two
// complete evaluations of the same vertex modifier: one against this frame's
// InstanceData and one against the previous endpoint's. The transform half of
// the previous endpoint rides GPUInstance.prevTransform; this block carries the
// rest of it — the clock the previous frame deformed at and the view-projection
// that endpoint projects through — plus what the fragment stage needs to turn
// its own rasterized position into the current endpoint.
//
// uMotionPrevViewProj is UNJITTERED. Cam.uVP carries this frame's temporal
// jitter, and a delta taken through two jittered matrices carries
// jitter(n) - jitter(n-1) on top of the real motion, which is precisely the term
// the temporal resolve must not reproject by. The current endpoint is the
// fragment's own position and that one IS jittered, which is what uMotionJitterUv
// takes back off.
//
// The clock lanes pair with Light.uTimeParams.zw (the current endpoint) and
// share its origin: both are seconds since the one deformation origin every
// view holds (ViewTemporalHistory.h), so the two endpoints of a frame
// difference to exactly that frame's delta at any uptime. A sample carrying a
// different origin is not differenceable against this one and the producer
// treats it as absent history rather than writing a wrong vector.

#ifndef GE_DEFORMATION_MOTION_GLSL
#define GE_DEFORMATION_MOTION_GLSL

layout(set = 0, binding = 45, std140) uniform DeformationMotionParams
{
    mat4 uMotionPrevViewProj;   // unjittered view-proj of the frame this view last rendered
    // x = previous animation lane, y = previous scroll lane. zw unused; the
    // block stays vec4-aligned so a std140 mirror needs no tail padding rule.
    vec4 uMotionPrevTimeParams;
    // xy = viewport origin in pixels, zw = viewport size in pixels. gl_FragCoord
    // is a window position; this is what makes it a viewport UV.
    vec4 uMotionViewportRect;
    // xy = this frame's temporal jitter as a viewport-UV offset. zw unused.
    vec4 uMotionJitterUv;
} MotionParams;

// The scatter's per-instance rendered level and phase history for THIS frame —
// the buffer draw_command_scatter.comp wrote at binding 13, read here by a
// later graphics pass of the same frame. Indexed by the global instance index.
//
// An indirect range mixes instances whose history is comparable with instances
// whose is not: a brand new tenant of a slot, a recycled one, an instance that
// crossed a level threshold, one whose mesh was reloaded behind a stable
// handle. Membership in the range cannot express that, so validity arrives per
// instance, on the channel the scatter already writes.
layout(set = 0, binding = 46, std430) restrict readonly buffer DeformationMotionHistory
{
    uint uMotionRenderedHistory[];
} MotionHistory;

// Bit 9 of an entry: this instance claimed a record this frame AND the
// previous rendered frame's entry existed with the same level and fading
// state. Mirrors kRenderedHistoryContinuousBit in draw_command_scatter.comp.
const uint kMotionHistoryContinuousBit = 0x200u;

bool ge_DeformationHistoryContinuous(uint instanceIndex)
{
    return (MotionHistory.uMotionRenderedHistory[instanceIndex]
            & kMotionHistoryContinuousBit) != 0u;
}

#endif // GE_DEFORMATION_MOTION_GLSL
