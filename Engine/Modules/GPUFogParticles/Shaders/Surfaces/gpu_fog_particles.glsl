// Procedural particle fog surface inspired by MirzaBeig/GPU-Fog-Particles.
// Optional albedoMap modulates color and alpha; when unassigned the default
// white texture passes the material tint through unchanged.
//
// Material param packing: uParams0 to uParams8 carry p0 to p8 of Includes/gpu_fog_shape.glsl.

#ifndef GE_GPU_FOG_PARTICLES_SURFACE_GLSL
#define GE_GPU_FOG_PARTICLES_SURFACE_GLSL

// albedoMap is provided by the adapter's bindless slot macro.

#include "Includes/gpu_fog_shape.glsl"

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();

    float timeSeconds = Light.uTimeParams.x;
    float particleStableRandom = GPUFogRandom(sIn.uv0 + sIn.positionWS.xz * 0.071);
    vec2 uv = sIn.uv0;

    vec4 p0 = Mat.uParams0;
    vec4 p1 = Mat.uParams1;
    vec4 p2 = Mat.uParams2;
    vec4 p4 = Mat.uParams4;
    vec4 p5 = Mat.uParams5;
    vec4 p6 = Mat.uParams6;
    vec4 p7 = Mat.uParams7;
    vec4 p8 = Mat.uParams8;

    float density;
    float radial;
    GPUFogEvaluateShape(uv, timeSeconds, particleStableRandom, p0, p1, p2, p4, p5, p6, p7, density, radial);

    vec4 albedo = texture(albedoMap, sIn.uv0);
    float surfaceFade = 1.0;
    float particleEyeDepth = abs(sIn.linearDepth);
    float surfaceDepthFade = max(p4.x, 0.0);
    if (surfaceDepthFade > 1e-4)
    {
        vec2 depthSize = vec2(textureSize(ge_sceneDepth, 0));
        vec2 screenUV = clamp(gl_FragCoord.xy / max(depthSize, vec2(1.0)), vec2(0.0), vec2(1.0));
        float sceneRawDepth = texelFetch(ge_sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
        if (sceneRawDepth > 1e-6)
        {
            float sceneEyeDepth = GPUFogSceneEyeDepth(sceneRawDepth, screenUV);
            float fragmentEyeDepth = GPUFogSceneEyeDepth(gl_FragCoord.z, screenUV);
            surfaceFade = clamp(abs(sceneEyeDepth - fragmentEyeDepth) / surfaceDepthFade, 0.0, 1.0);
        }
    }
    float cameraFade = GPUFogCameraFade(particleEyeDepth, p8);

    vec4 lifetimeColor = sIn.custom0;
    o.baseColor = albedo.rgb * Mat.uBaseColor.rgb * sIn.vertexColor.rgb * lifetimeColor.rgb;
    o.opacity = density * radial * surfaceFade * cameraFade * albedo.a * Mat.uBaseColor.a * sIn.vertexColor.a * lifetimeColor.a;
    o.normalWS = normalize(sIn.normalWS);
    return o;
}

#endif // GE_GPU_FOG_PARTICLES_SURFACE_GLSL
