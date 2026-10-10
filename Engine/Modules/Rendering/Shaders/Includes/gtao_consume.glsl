// Screen-space GTAO consumer. Folds the AO node's result into the surface's
// occlusion + bent normal before ambient/IBL evaluation.
//
// ge_gtao is bound by name (RenderServices) for every StandardPBR world variant.
// When the AmbientOcclusion node is absent the binder supplies a (0,0,0,1)
// fallback: a = 1 (full visibility, a no-op) and rgb = 0 (the "no bent normal"
// sentinel), so this degrades to the pre-GTAO look.

#ifndef GE_GTAO_CONSUME_GLSL
#define GE_GTAO_CONSUME_GLSL

#include "surface_io.glsl"

#if defined(GE_COMPAT_PROFILE)
// Save a sampler on devices with WebGPU's per-stage limit of 16. The AO
// result is full-resolution, so the fragment can fetch its own texel.
#extension GL_EXT_samplerless_texture_functions : require
layout(set = 0, binding = 27) uniform texture2D ge_gtao;
#else
layout(set = 0, binding = 27) uniform sampler2D ge_gtao;
#endif

void GE_ApplyGTAO(inout SurfaceOutput so, vec2 screenUV)
{
#if defined(GE_COMPAT_PROFILE)
    const ivec2 texel = ivec2(screenUV * vec2(textureSize(ge_gtao, 0)));
    vec4 g = texelFetch(ge_gtao, texel, 0);
#else
    vec4 g = texture(ge_gtao, screenUV);
#endif
    // Combine with any material (ORM) AO the surface already wrote.
    so.ao = min(so.ao, g.a);
    // Real bent normals are unit length; the zero sentinel means "no GTAO here".
    so.bentNormalWS = dot(g.rgb, g.rgb) > 1e-4 ? normalize(g.rgb) : so.normalWS;
}

#endif // GE_GTAO_CONSUME_GLSL
