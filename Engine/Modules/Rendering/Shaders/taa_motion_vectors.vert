#version 450

// TAA movers motion-vector pass, vertex stage — RIGID variant. Draws the
// instances whose transform changed this frame (RenderServices::GetFrameMovers)
// with no skinning; deforming skinned movers use
// taa_motion_vectors_skinned.vert instead. The raster position uses the SAME
// jittered Cam UBO + camera-relative helpers as the world/depth passes so the
// read-only GreaterOrEqual depth test against the prepass depth passes exactly
// on the visible surface; the motion payload is computed in the UNJITTERED
// domain (current and previous view-proj) so the resolve's history walk never
// sees jitter as motion.
//
// P0 residuals (documented in the TAA design doc): motion is evaluated in
// full-world coordinates (render-origin content: MV precision at planetary
// distance is a rel-space follow-up), and geometry is drawn at LOD0 (a distant
// mover rasterized at a coarser LOD may leave partial MV holes that fall back
// to camera reprojection).

// Same bit-exact raster-position contract as adapter_vertex.glsl /
// shadow_depth_shared.vert (read-only GreaterOrEqual against prepass depth):
// all modules reproducing the chain declare gl_Position invariant so the
// driver cannot contract the math differently per module.
invariant gl_Position;

layout(location = 0) in vec3 aPosition;

#include "Includes/taa_motion_vectors_common.glsl"

void main()
{
    // Rigid: the object-space vertex is the same at both endpoints, so all
    // motion comes from transform vs prevTransform.
    ge_TaaMVEmit(aPosition, aPosition);
}
