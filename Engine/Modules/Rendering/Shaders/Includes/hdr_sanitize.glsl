#include "compat_profile.glsl"

// FP16 finite ceiling. HDR ring targets are R16G16B16A16_FLOAT, so no
// representable scene value exceeds this — clamping rewrites only +Inf, never a
// real highlight. Gathers over HDR pre-tonemap keep bright taps at full weight
// (no Karis downweight, no energy clamp): a normalized weighted average of
// bounded HDR input stays bounded; only NaN/Inf must be scrubbed or a single
// upstream stray would smear across the whole gather.
const float kMaxHDR = 65504.0;

vec3 Sanitize(vec3 c)
{
    c = mix(c, vec3(0.0), vec3(GE_IS_NAN(c))); // NaN -> 0 (survives clamp, which does not)
    return clamp(c, vec3(0.0), vec3(kMaxHDR)); // +Inf -> kMaxHDR, negatives -> 0
}

// No channel NaN and none past kMaxHDR, which an RGBA16F store turns into an
// infinity. The magnitude test is false for NaN too, so the check holds where
// the compat NaN test folds away.
bool IsFiniteHDR(vec3 c)
{
    return !any(GE_IS_NAN(c)) && all(lessThanEqual(abs(c), vec3(kMaxHDR)));
}
