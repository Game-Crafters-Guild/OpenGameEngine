// A particle's opacity: a sheet's alpha as coverage, and which fragments draw nothing.
//
// Block compression leaves up to about 2/255 of alpha under bright colour where a sheet is
// transparent, and a particle multiplies whatever alpha it samples by unbounded HDR colour,
// lighting and emission, so that residue alone draws the quad's edges. Alpha at or below the
// residue is transparent; above it the coverage rescales so an opaque texel stays 1 and the ramp
// stays continuous.
#ifndef GE_PARTICLE_COVERAGE_GLSL
#define GE_PARTICLE_COVERAGE_GLSL

const float GE_PARTICLE_ALPHA_RESIDUE = 2.0 / 255.0;

float GE_ParticleCoverage(float alpha)
{
    return clamp((alpha - GE_PARTICLE_ALPHA_RESIDUE) / (1.0 - GE_PARTICLE_ALPHA_RESIDUE), 0.0, 1.0);
}

// Whether a fragment of `opacity` draws nothing under a straight-alpha blend (source alpha, one
// minus source alpha), so it can be discarded before the rest of it is shaded. Only opacity 0
// leaves the target unchanged: a dense plume stacks a hundred layers and more per pixel, so even
// fragments far fainter than an 8-bit level move some pixels by one when they are dropped.
bool GE_ParticleDrawsNothing(float opacity)
{
    return opacity <= 0.0;
}

// The factor a particle's colour or emission takes for its near-camera fade: the fade to the power the
// particle renderer derives from the material's blend (NearFadeColorExponent, ParticleMaterials.cpp,
// which states the rule per blend): 0 where the alpha already fades the colour as far as it should or
// fading it would darken the frame, 1 for premultiplied and SrcAlpha / One, 2 for One / One.
float GE_ParticleNearFadeColorScale(float nearFade, float exponent)
{
    if (exponent > 1.5)
        return nearFade * nearFade;
    return exponent > 0.5 ? nearFade : 1.0;
}

#endif // GE_PARTICLE_COVERAGE_GLSL
