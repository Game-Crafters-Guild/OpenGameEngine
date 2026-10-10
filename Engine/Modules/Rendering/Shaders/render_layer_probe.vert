#version 450

// Test raster consumer of the production cull -> scatter indirection. No
// secondary mask test here: a leaked instance must reach the readback image.
struct GPUInstance {
#include "Includes/gpu_instance_fields.glsl"
};
layout(std430, set = 0, binding = 0) readonly buffer Instances { GPUInstance instances[]; };
layout(std430, set = 0, binding = 1) readonly buffer Indirection { uint indices[]; };
void main() {
    const vec2 corners[3] = vec2[3](vec2(-.12,-.2), vec2(.12,-.2), vec2(0,.2));
    gl_Position = instances[indices[gl_InstanceIndex]].transform * vec4(corners[gl_VertexIndex], .5, 1);
}
