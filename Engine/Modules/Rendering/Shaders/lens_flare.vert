#version 450

// Instanced lens-flare quad. One draw of 6 vertices per instance expands a unit
// quad in NDC from a per-instance record in a storage buffer. No vertex inputs.
// Compiled at runtime by LensFlareRenderFeature (GLSL -> SPIR-V -> MSL on Metal).

struct FlareInstance
{
    vec4 PosBasisX;  // xy = center in NDC, zw = local-X half-vector in NDC
    vec4 BasisYProbe; // xy = local-Y half-vector in NDC, zw = source screen UV
    vec4 ColorDepth;  // rgb = brightness/fades premultiplied in, w = source NDC depth
    vec4 UVRect;      // xy = atlas UV min (top-left), zw = UV size
};

layout(std430, set = 0, binding = 1) readonly buffer Instances
{
    FlareInstance uInstances[];
};

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec3 vProbe; // xy = source depth-probe UV, z = source NDC depth

// Two triangles (list) covering the unit quad; corner in [0,1].
const vec2 kCorners[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
    vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));

void main()
{
    FlareInstance inst = uInstances[gl_InstanceIndex];
    vec2 corner = kCorners[gl_VertexIndex];

    vec2 local = corner * 2.0 - 1.0; // [-1,1]
    // The CPU supplies an aspect-correct rotated basis. Shape is therefore
    // applied before rotation in pixel space, matching the authoring tool and
    // keeping thin streaks/arcs rectangular instead of shearing them in NDC.
    vec2 pos = inst.PosBasisX.xy +
               local.x * inst.PosBasisX.zw +
               local.y * inst.BasisYProbe.xy;
    gl_Position = vec4(pos, 0.0, 1.0);

    // Atlas UV: TexturePacker frames have a top-left origin, so corner.y maps
    // straight to V (the single Y-origin decision; flip here if a flare appears
    // mirrored vertically). Backend-independent — concerns texture data, not the
    // clip-space convention the engine already reconciles for Vulkan/Metal.
    vUV = inst.UVRect.xy + corner * inst.UVRect.zw;
    vColor = vec4(inst.ColorDepth.rgb, 1.0);
    vProbe = vec3(inst.BasisYProbe.zw, inst.ColorDepth.w);
}
