#include "Rendering/Sky/AtmosphereTransmittance.h"

#include <algorithm>
#include <cmath>

namespace GameEngine {
namespace Rendering {
namespace {

// Steps of the optical-depth quadrature. Mirrors kSteps in sky_transmittance_lut.comp: the CPU
// colour and the GPU LUT are the same integral, so they must also be the same approximation of it.
constexpr int kOpticalDepthSteps = 32;

// Path length from the planet surface along a ray of zenith cosine `mu`. The evaluator sits ON the
// surface (r == planetRadius), so both intersections are closed-form. Do not replace this with a
// general ray/sphere solve: at this origin its `r*r - R*R` term is an exact cancellation, and a
// compiler that contracts it into a fused multiply-add leaves a residual of a few hundred thousand
// square metres that resolves to a near root of a few METRES, so the chord below is never taken and
// a set sun stays lit.
//
// Upward and grazing (mu >= 0): out to the top of the atmosphere. Downward (mu < 0): the chord
// through the planet — hundreds of kilometres of full-density air, so the source is occluded. The
// GPU LUT's bottom row sits a few metres above the surface and takes the short path down to the
// ground instead, which is why nothing below the horizon is shared with it.
float SurfacePathLength(const AtmosphereParametersGPU& atmosphere, float mu)
{
    const float r = atmosphere.planetRadius;
    if (mu < 0.0f)
        return -2.0f * r * mu;

    const float shellDepth =
        (atmosphere.atmosphereRadius - r) * (atmosphere.atmosphereRadius + r);
    const float rMu = r * mu;
    return std::sqrt(shellDepth + rMu * rMu) - rMu;
}

// Optical depth from the planet surface out along `mu`.
void OpticalDepth(const AtmosphereParametersGPU& atmosphere, float mu, float outTau[3])
{
    outTau[0] = outTau[1] = outTau[2] = 0.0f;

    const float r = atmosphere.planetRadius;
    const float sMax = SurfacePathLength(atmosphere, mu);
    if (sMax <= 0.0f)
        return;

    const float ds = sMax / static_cast<float>(kOpticalDepthSteps);
    for (int step = 0; step < kOpticalDepthSteps; ++step)
    {
        const float sMid = (static_cast<float>(step) + 0.5f) * ds;
        const float r2 = r * r + sMid * sMid + 2.0f * r * mu * sMid;
        const float height = std::max(0.0f, std::sqrt(std::max(r2, 0.0f)) - atmosphere.planetRadius);

        const float rayleigh = std::exp(-height / atmosphere.rayleighScaleHeight);
        const float mie = std::exp(-height / atmosphere.mieScaleHeight);
        for (int c = 0; c < 3; ++c)
            outTau[c] += (atmosphere.betaRayleigh[c] * rayleigh + atmosphere.betaMie[c] * mie) * ds;
    }
}

} // namespace

const AtmosphereParametersGPU& ScatteringAtmosphere()
{
    static const AtmosphereParametersGPU atmosphere = [] {
        AtmosphereParametersGPU atmo{};
        SkyRenderer::FillDefaultAtmosphere(atmo);
        return atmo;
    }();
    return atmosphere;
}

void EvaluateGroundLevelSunColor(const AtmosphereParametersGPU& atmosphere,
                                 const float colorAboveAtmosphere[3],
                                 float sourceUpDot,
                                 float outColor[3])
{
    const float mu = std::clamp(sourceUpDot, -1.0f, 1.0f);

    float tau[3];
    float zenithTau[3];
    OpticalDepth(atmosphere, mu, tau);
    OpticalDepth(atmosphere, 1.0f, zenithTau);

    if (mu < 0.0f)
    {
        // A chord through the planet is SHORTER than the grazing path just above it, so the raw
        // integral makes a source get brighter for the first degree after it sets. The extinction
        // a set source suffers is at least the horizon's, and the chord takes over from there —
        // which keeps the whole descent monotonic through the crossing.
        float grazingTau[3];
        OpticalDepth(atmosphere, 0.0f, grazingTau);
        for (int c = 0; c < 3; ++c)
            tau[c] = std::max(tau[c], grazingTau[c]);
    }

    // Relative to the zenith path, so an overhead source comes through unchanged: a directional
    // light's authored illuminance is already its GROUND value, and subtracting the overhead
    // extinction from it a second time would dim every scene's key light by ~10% at noon. What
    // varies over the day is the EXTRA air mass a lower sun's light travels through.
    for (int c = 0; c < 3; ++c)
        outColor[c] = colorAboveAtmosphere[c] * std::exp(zenithTau[c] - tau[c]);
}

SkyBodyGroundColors EvaluateBodyGroundColors(const AtmosphereParametersGPU& atmosphere,
                                             const SkySystemConfig& config,
                                             const SkySystemState& state,
                                             const float sunTint[3])
{
    SkyBodyGroundColors bodies;
    EvaluateGroundLevelSunColor(atmosphere, sunTint, state.sunDirWS[1], bodies.Sun);

    // The moon's source is its own colour, NOT the sun's tint times it. The tint is art direction
    // for the sun, and the moon disc is drawn from this same untinted colour, so tinting moonlight
    // here would light the ground in a colour the visible moon does not have.
    EvaluateGroundLevelSunColor(atmosphere, config.moonColor, state.moonDirWS[1], bodies.Moon);

    bodies.MoonBlend =
        config.useMoonWhenSunBelowHorizon ? std::clamp(state.primaryMoonBlend, 0.0f, 1.0f) : 0.0f;
    return bodies;
}

void MixPrimaryGroundColor(const SkyBodyGroundColors& bodies, float outColor[3])
{
    for (int c = 0; c < 3; ++c)
        outColor[c] = bodies.Sun[c] * (1.0f - bodies.MoonBlend) + bodies.Moon[c] * bodies.MoonBlend;
}

void MixLinkedLightGroundColor(const SkyBodyGroundColors& bodies, float moonLightScale, float outColor[3])
{
    const float moonWeight = bodies.MoonBlend * moonLightScale;
    for (int c = 0; c < 3; ++c)
        outColor[c] = bodies.Sun[c] * (1.0f - bodies.MoonBlend) + bodies.Moon[c] * moonWeight;
}

} // namespace Rendering
} // namespace GameEngine
