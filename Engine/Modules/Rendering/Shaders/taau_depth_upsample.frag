#version 450

// TAAU display-depth reconstitution. Under temporal upscaling the raster depth
// lives at the INTERNAL extent; consumers that depth-test at DISPLAY extent
// (the editor's gizmo/overlay passes attach the caller's depth target) need a
// display-res depth. Nearest-neighbor upsample via gl_FragDepth — point, not
// linear: interpolating reverse-Z depth across a silhouette invents surfaces
// that occlude gizmos mid-air. Runs as a fullscreen triangle
// (fullscreen_noinput.vert) with depth-write on, compare Always.

layout(location = 0) in vec2 vUV;

layout(set = 0, binding = 0) uniform sampler2D uDepth; // internal-extent D32

void main()
{
    ivec2 inExtent = textureSize(uDepth, 0);
    ivec2 p = clamp(ivec2(vUV * vec2(inExtent)), ivec2(0), inExtent - 1);
    gl_FragDepth = texelFetch(uDepth, p, 0).r;
}
