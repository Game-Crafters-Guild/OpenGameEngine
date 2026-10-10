// How a roughness reads the two baked DDGI reflection lobes, and how their
// result composites over a prefiltered environment. Shared by the forward
// consume (ibl.glsl); SSR receives that forward incident radiance through
// the material exports. Binding-free: include anywhere.
#ifndef GE_DDGI_LOBE_MIX_GLSL
#define GE_DDGI_LOBE_MIX_GLSL

// Below the start the sharp power-64 lobe owns the reflection outright, above
// the end the broad power-8 one does.
const float GE_DDGI_ROUGH_LOBE_MIX_START = 0.22;
const float GE_DDGI_ROUGH_LOBE_MIX_END = 0.58;

float GE_DDGIRoughLobeMix(float roughness)
{
    return smoothstep(GE_DDGI_ROUGH_LOBE_MIX_START, GE_DDGI_ROUGH_LOBE_MIX_END, roughness);
}

// COMPOSITE, not a blend: a lobe sample is coverage-premultiplied radiance in
// rgb with the fraction of its solid angle that resolved geometry in a. Only
// that fraction displaces the environment; where the probes saw nothing the
// environment survives at full strength.
vec3 GE_DDGICompositeReflection(vec3 env, vec4 lobe, float intensity)
{
    float covered = clamp(lobe.a, 0.0, 1.0) * intensity;
    return env * (1.0 - covered) + lobe.rgb * intensity;
}

#endif
