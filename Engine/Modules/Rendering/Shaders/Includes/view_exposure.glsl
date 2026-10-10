// The exposure the tonemap applies to the view being drawn, for forward shading whose output is
// display-referred rather than scene-linear: dividing by GE_ViewExposureScale() makes 1.0 draw as
// the display's reference white at whatever exposure the view has, in daylight or at night.
//
// The inputs are the ones the tonemap and the bloom threshold resolve through the same function:
// the view's static scale and auto-exposure flag (ViewParams.ge_exposureParams, written by
// ViewParamsUploadNode) and the metered scale in the view's ExposureHistory buffer. The forward
// passes run before this frame's metering, so they read the previous frame's adapted exposure,
// one frame behind the tonemap. A view whose render graph meters nothing binds the zero
// fallback, and the static scale applies.
#ifndef GE_VIEW_EXPOSURE_GLSL
#define GE_VIEW_EXPOSURE_GLSL

#include "exposed_brightness.glsl"
#include "view_params.glsl"

layout(set = 0, binding = 47, std430) readonly buffer ExposureHistoryBlock
{
    float exposureScale;
    uint valid;
} ExposureHistory;

float GE_ViewExposureScale()
{
    return GE_ResolveExposureScale(ge_exposureParams.x, int(ge_exposureParams.y), ExposureHistory.exposureScale);
}

// What an emission is multiplied by so that the tonemap, which multiplies by the view's exposure,
// shows emission x exposure^weight: exposure^(weight - 1). Weight 1 is physical (a scale of 1), 0
// cancels the exposure, values between interpolate in log exposure. `exposure` is the view's linear
// exposure scale, GE_ViewExposureScale(), which is always > 0.
float GE_EmissiveExposureScale(float weight, float exposure)
{
    return exp2((clamp(weight, 0.0, 1.0) - 1.0) * log2(exposure));
}

#endif // GE_VIEW_EXPOSURE_GLSL
