// The Parallax steps debug view: a marching variant of a height-mapped material draws the number of
// height samples it took, its view march's (GE_ParallaxHit::fetches) and its self-shadow's
// (GE_ParallaxShadow::fetches, Includes/parallax_occlusion.glsl) together, as a palette colour at
// the luminance of its lit colour. The world pass selects it per Scene View through the
// ParallaxStepsView pass keyword; nothing ships it.
//
// The block between the GE_SHARED_PARALLAX_STEPS_VIEW markers is lifted verbatim into the host tests
// (ExtractShaderBlock.cmake -> Tests/ParallaxOcclusionTests.cpp), which hold the palette to one colour
// per band that the view's readers can tell apart, up to the 53 samples a desktop pixel takes at most.
#ifndef GE_PARALLAX_STEPS_VIEW_GLSL
#define GE_PARALLAX_STEPS_VIEW_GLSL

// GE_SHARED_PARALLAX_STEPS_VIEW_BEGIN
// Relative luminance of a linear Rec. 709 colour.
const vec3 kParallaxStepsViewLuminance = vec3(0.2126f, 0.7152f, 0.0722f);

// The palette: one colour per band of samples, blue to red, each band a batch of four up to 16 and
// wider beyond, where only a close grazing surface reaches. The upper bound of each band, in samples;
// the last band takes every count above the orange band's.
const int kParallaxStepsViewBlueUpTo = 4;
const int kParallaxStepsViewCyanUpTo = 8;
const int kParallaxStepsViewGreenUpTo = 12;
const int kParallaxStepsViewYellowUpTo = 16;
const int kParallaxStepsViewOrangeUpTo = 24;
// The band colours, linear. Desaturated enough that, scaled to a lit luminance, no channel runs far
// past the others and saturates in the tonemapper before its neighbours do.
const vec3 kParallaxStepsViewBlue = vec3(0.35f, 0.55f, 1.0f);
const vec3 kParallaxStepsViewCyan = vec3(0.2f, 0.85f, 0.9f);
const vec3 kParallaxStepsViewGreen = vec3(0.35f, 0.9f, 0.35f);
const vec3 kParallaxStepsViewYellow = vec3(0.95f, 0.9f, 0.25f);
const vec3 kParallaxStepsViewOrange = vec3(1.0f, 0.55f, 0.15f);
const vec3 kParallaxStepsViewRed = vec3(1.0f, 0.25f, 0.25f);

// The palette colour of a pixel that took `samples` height samples (at least 1).
vec3 GE_ParallaxStepsViewColor(int samples)
{
    if (samples <= kParallaxStepsViewBlueUpTo)
        return kParallaxStepsViewBlue;
    if (samples <= kParallaxStepsViewCyanUpTo)
        return kParallaxStepsViewCyan;
    if (samples <= kParallaxStepsViewGreenUpTo)
        return kParallaxStepsViewGreen;
    if (samples <= kParallaxStepsViewYellowUpTo)
        return kParallaxStepsViewYellow;
    if (samples <= kParallaxStepsViewOrangeUpTo)
        return kParallaxStepsViewOrange;
    return kParallaxStepsViewRed;
}

// What the view draws in place of a lit colour: the palette colour at the lit colour's luminance, so
// it reads under whatever exposure the view has, or neutral grey at that luminance where the pixel
// took no sample (its relief and its shadow are both under a pixel on screen). The chromaticity
// carries the band: a reader finds the palette colour nearest the pixel's r, g, b over their sum.
vec3 GE_ParallaxStepsViewShade(vec3 litColor, int samples)
{
    float luminance = max(dot(litColor, kParallaxStepsViewLuminance), 0.0f);
    if (samples <= 0)
        return vec3(luminance, luminance, luminance);
    vec3 band = GE_ParallaxStepsViewColor(samples);
    return band * (luminance / dot(band, kParallaxStepsViewLuminance));
}
// GE_SHARED_PARALLAX_STEPS_VIEW_END

#endif // GE_PARALLAX_STEPS_VIEW_GLSL
