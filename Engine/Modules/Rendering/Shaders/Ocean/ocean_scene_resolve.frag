#version 450

// Resolve the opaque color represented by the depth resolve's sample zero.
// Keep this reference in sync with depth_resolve.comp. Averaging
// unrelated sky/background samples makes partially covered objects glow when the
// water applies the object's short fog distance to that mixed color.
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2DMS uSrc;
layout(set = 0, binding = 1) uniform sampler2DMS uDepth;
layout(std140, set = 0, binding = 2) uniform ResolveParams { uvec4 options; };

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    int count = textureSamples(uSrc);
    float reference = options.x != 0u ? texelFetch(uDepth, p, 0).r : 0.0;
    float tolerance = max(reference * 0.005, 1e-6);
    vec4 color = vec4(0.0);
    float weight = 0.0;
    for (int i = 0; i < count; ++i)
    {
        float accept = options.x == 0u || abs(reference - texelFetch(uDepth, p, i).r) <= tolerance ? 1.0 : 0.0;
        color += texelFetch(uSrc, p, i) * accept;
        weight += accept;
    }
    outColor = color / max(weight, 1.0);
}
