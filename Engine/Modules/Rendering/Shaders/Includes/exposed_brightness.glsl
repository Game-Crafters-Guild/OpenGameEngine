// The bloom chain's single brightness metric: display-relative (exposed), not scene-linear.
//
// Every threshold, knee, clamp and Karis weight in the chain is written against a reference
// of 1.0 = display white. `1.0` only means that AFTER the exposure multiply — scene-linear
// radiance has no absolute scale, so the same constant means a different brightness in a
// 100 klx exterior than in a night interior.
//
// That matters most for the Karis weight w = 1 / (1 + B), whose whole behaviour is set by
// where B crosses 1: it bounds an isolated bright tap's contribution in units of B's own
// reference. One bright tap among three dark ones survives as B / (4 + 3B), which rises to
// 1/3 however bright the tap gets. Fed exposed values that ceiling is a third of a display
// white, wherever the exposure sits — the intended firefly suppression. Fed raw values under
// a small exposure, the same expression treats anything above 1.0 raw as a firefly, so a
// small but genuinely bright source (a sun disc, a lamp, a window) is damped to the same
// third of a raw unit, which is a hundredth of that on screen.
//
// Shared so the bright pass and the pyramid cannot desynchronize on the unit.
#ifndef GE_EXPOSED_BRIGHTNESS_GLSL
#define GE_EXPOSED_BRIGHTNESS_GLSL

// Exposed, non-negative max channel. Negative HDR undershoots clamp to 0; without the floor
// a tap at or below -1 exposed makes the Karis weight infinite or sign-flipped.
float GE_ExposedMaxChannel(vec3 c, float expScale)
{
    return max(max(max(c.r, c.g), c.b) * expScale, 0.0);
}

// Resolve the exposure the tonemap will apply. `historyScale` is the metered exposure from
// the per-view ExposureHistory buffer (zero-initialized on creation, so frame 0 and any
// unmetered view read 0); `staticScale` is the Fixed/Manual/Physical value. A non-positive
// result would turn every exposed comparison into a no-op, so it falls back to 1.0.
float GE_ResolveExposureScale(float staticScale, int useAutoExposure, float historyScale)
{
    float expScale = (useAutoExposure != 0 && historyScale > 0.0) ? historyScale : staticScale;
    return (expScale > 0.0) ? expScale : 1.0;
}

#endif // GE_EXPOSED_BRIGHTNESS_GLSL
