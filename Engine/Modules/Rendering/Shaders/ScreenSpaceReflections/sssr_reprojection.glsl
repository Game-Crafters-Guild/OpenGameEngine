#ifndef GE_SSSR_REPROJECTION_GLSL
#define GE_SSSR_REPROJECTION_GLSL
#include "ScreenSpaceReflections/sssr_reprojection_math.glsl"

// View.MotionVectors stores UNJITTERED current-minus-previous viewport UV.
// Depth and hit UVs refer to the current jittered raster, so both phases must
// be applied when addressing the previous frame. Shared by classify,
// reprojection and multi-bounce hit sampling.
bool GE_SssrReprojectSurface(sampler2D motionTexture, vec2 uv, vec3 positionVS,
                            out vec2 previousUv, out float previousDepth,
                            out bool objectMotion)
{
    vec4 motion = texelFetch(motionTexture,
        clamp(ivec2(uv * ge_screenSize.xy), ivec2(0), textureSize(motionTexture, 0) - 1), 0);
    objectMotion = motion.a > 0.5 && abs(motion.x) < 50.0 && abs(motion.y) < 50.0;
    if (objectMotion)
    {
        previousUv = GE_SssrPreviousRasterUv(uv, motion.xy, ge_taaJitter.xy, ge_taaJitter.zw);
        previousDepth = motion.b;
    }
    else
    {
        vec4 clip = ge_prevViewProj * ge_invView * vec4(positionVS, 1.0);
        if (!(clip.w > 1e-6)) return false;
        vec3 ndc = clip.xyz / clip.w;
        previousUv = GE_NdcToUv(ndc.xy + ge_taaJitter.zw);
        previousDepth = ndc.z;
    }
    return all(greaterThan(previousUv, vec2(0.0))) &&
           all(lessThan(previousUv, vec2(1.0))) &&
           previousDepth > 0.0 && previousDepth <= 1.0;
}
#endif
