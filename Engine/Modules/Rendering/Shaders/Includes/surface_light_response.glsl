// The light a surface sends toward the viewer for one light. Every forward lighting path (the
// primary and secondary directionals, the clustered point, spot and area lights) asks this, so a
// surface whose response is not the standard BRDF answers here and the BRDF stays one model.
// Six-way lit particles answer from their baked response maps; every other surface is StandardPBR.

#ifndef GE_SURFACE_LIGHT_RESPONSE_GLSL
#define GE_SURFACE_LIGHT_RESPONSE_GLSL

#include "standard_pbr.glsl" // GE_EvaluateStandardPBR + GE_PI
#ifdef GE_USER_PARTICLE_LIT
#include "particle_six_way.glsl" // GE_SixWayResponse
#endif

// Radiance toward V per unit of light radiance arriving from L. V is the view direction and L the
// surface-to-light direction, both unit length in world space.
vec3 GE_EvaluateSurfaceLight(SurfaceOutput so, vec3 V, vec3 L)
{
#ifdef GE_USER_PARTICLE_LIT
    // The response maps hold the light each signed axis of the particle's basis lets through.
    return so.baseColor * GE_SixWayResponse(so.particlePositive, so.particleNegative,
                                           transpose(so.particleBasis) * L) / GE_PI;
#else
    return GE_EvaluateStandardPBR(so, V, L);
#endif
}

#endif
