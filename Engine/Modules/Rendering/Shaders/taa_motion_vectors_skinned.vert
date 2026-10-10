#version 450

// TAA movers motion-vector pass, vertex stage — SKINNED variant. Same contract
// as taa_motion_vectors.vert, but each vertex is skinned TWICE: once with this
// frame's bone palette and once with the previous frame's, so the motion vector
// carries pose deformation as well as object motion. A character animating in
// place has an identity object motion vector and a large per-vertex one; the
// rigid variant would report zero motion for it and the resolve would smear it.
//
// Two palettes, two bindings: current and previous live in DIFFERENT ring
// buffers of the same PerFrameWritePool usage class (the previous slot is kept
// alive by an extra ring — see PerFrameWritePool's ExtraSlots), and GLSL cannot
// select an SSBO at runtime.

invariant gl_Position;

layout(location = 0) in vec3 aPosition;
// Locations match adapter_vertex.glsl's SKINNED block so the same core vertex
// streams bind unchanged.
layout(location = 6) in uvec4 aJoints;
layout(location = 7) in vec4 aWeights;

#include "Includes/taa_motion_vectors_common.glsl"

layout(std430, set = 0, binding = 12) readonly buffer BonePaletteAtlasSSBO
{
    vec4 rows[];
} BonePaletteAtlas;

layout(std430, set = 0, binding = 13) readonly buffer PrevBonePaletteAtlasSSBO
{
    vec4 rows[];
} PrevBonePaletteAtlas;

#define GE_BONE_PALETTE_READABLE
#define GE_BONE_PALETTE_PREV_READABLE
#include "Includes/bone_palette.glsl"

void main()
{
    GPUInstance inst = ge_instances[pc.uInstanceSlot];

    // Current pose: the SAME blend the world pass used (ge_ApplyBonePalette's
    // position math), so the raster position lands bit-exact on the prepass
    // depth this pass tests against.
    vec3 currLocal =
        ge_ApplyBonePalettePosition(aPosition, aJoints, aWeights, inst.skinPaletteOffset);

    // Previous pose. With no pose last frame (spawn, animation start, no
    // previous ring) the previous atlas holds nothing that belongs to this
    // instance — whatever runtime occupied that offset then still owns those
    // bytes — so the endpoint collapses onto the current pose. Pose motion is
    // then exactly zero and only the instance transform contributes, which is
    // the correct answer for something that has no pose history. The branch is
    // uniform across the draw (push constant), so no divergence.
    vec3 prevLocal = currLocal;
    if (pc.uPrevPaletteOffset != GE_NO_PREV_SKIN_PALETTE)
    {
        prevLocal =
            ge_ApplyPrevBonePalettePosition(aPosition, aJoints, aWeights, pc.uPrevPaletteOffset);
    }

    ge_TaaMVEmit(currLocal, prevLocal);
}
