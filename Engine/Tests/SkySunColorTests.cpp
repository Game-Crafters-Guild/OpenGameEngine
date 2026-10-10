// The sun's colour is the atmosphere's, and it is applied exactly once on each path.
//
// Two quantities, two consumers, and conflating them is the defect this suite exists to pin:
//   * ABOVE the atmosphere (SkySettings::primarySunColor) — what the sky-view LUT multiplies into
//     its in-scatter source term and what the sun disc scales. Both apply the transmittance LUT
//     themselves, so anything pre-reddened here is reddened twice.
//   * AT the ground (SkySettings::primarySunGroundColor) — what the linked directional light and
//     the fog sun carry, so a lit surface warms with the sky instead of drifting away from it.
//
// EvaluateGroundLevelSunColor is the CPU mirror of sky_transmittance_lut.comp: a light's colour is
// decided long before that LUT is sampled, so the model has to exist on both sides. The mirror is
// only worth having if it stays a mirror, which is what the analytic and contract tests below are
// for — the numeric ones pin the integral against closed form and against its own converged limit,
// and the source contract fails if the shader's integrand or quadrature moves out from under it.
//
// Reads the repo shader source via GE_RENDERER_REPO_ROOT, the same dev-only anchor
// SkyGroundBounceContractTests uses: the source file is the artifact under test, and a staged copy
// only refreshes when its staging target rebuilds, which a shader-only edit does not.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h" // ResolveLightColorIntensity
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h" // World::AddComponentImmediate/GetComponent definitions
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/SkyEnvironmentSystem.h"
#include "Engine/Rendering/FogSunLightingResolver.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Rendering/Sky/AtmosphereTransmittance.h"
#include "Rendering/Sky/SkyRenderer.h"
#include "Rendering/Sky/SkySettings.h"
#include "Rendering/Sky/SkySystem.h"

using namespace GameEngine;

namespace
{

constexpr float kWhite[3] = {1.0f, 1.0f, 1.0f};

Rendering::AtmosphereParametersGPU DefaultAtmosphere()
{
    Rendering::AtmosphereParametersGPU atmo{};
    Rendering::SkyRenderer::FillDefaultAtmosphere(atmo);
    return atmo;
}

float Luminance(const float rgb[3])
{
    return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

float MuForElevationDegrees(float degrees)
{
    return std::sin(degrees * 3.14159265358979323846f / 180.0f);
}

// Independent, converged reference for the optical depth the production code integrates: double
// precision, 128x the production step count, written out longhand rather than calling the
// production code so a change there cannot move the reference with it.
void ReferenceOpticalDepth(const Rendering::AtmosphereParametersGPU& atmo, double mu,
                           double outTau[3], int steps = 4096)
{
    outTau[0] = outTau[1] = outTau[2] = 0.0;
    const double r = atmo.planetRadius;
    auto intersect = [&](double sphereRadius) {
        const double b = 2.0 * r * mu;
        const double c = r * r - sphereRadius * sphereRadius;
        const double disc = b * b - 4.0 * c;
        if (disc <= 0.0)
            return 0.0;
        const double s = std::sqrt(disc);
        const double t0 = 0.5 * (-b - s);
        const double t1 = 0.5 * (-b + s);
        double t = 1e20;
        if (t0 > 0.0) t = std::min(t, t0);
        if (t1 > 0.0) t = std::min(t, t1);
        return (t < 1e19) ? t : 0.0;
    };

    double sMax = intersect(atmo.atmosphereRadius);
    if (mu < 0.0)
    {
        const double sGround = intersect(atmo.planetRadius);
        if (sGround > 0.0)
            sMax = sGround;
    }
    if (sMax <= 0.0)
        return;

    const double ds = sMax / steps;
    for (int i = 0; i < steps; ++i)
    {
        const double sMid = (i + 0.5) * ds;
        const double height =
            std::max(0.0, std::sqrt(r * r + sMid * sMid + 2.0 * r * mu * sMid) - atmo.planetRadius);
        const double rayleigh = std::exp(-height / atmo.rayleighScaleHeight);
        const double mie = std::exp(-height / atmo.mieScaleHeight);
        for (int c = 0; c < 3; ++c)
            outTau[c] += (atmo.betaRayleigh[c] * rayleigh + atmo.betaMie[c] * mie) * ds;
    }
}

// The reference above, expressed the way the production function reports it: relative to the
// zenith path, because that is the elevation at which a light's illuminance is authored.
void ReferenceGroundColor(const Rendering::AtmosphereParametersGPU& atmo, double mu, double out[3])
{
    double tau[3];
    double zenith[3];
    ReferenceOpticalDepth(atmo, mu, tau);
    ReferenceOpticalDepth(atmo, 1.0, zenith);
    for (int c = 0; c < 3; ++c)
        out[c] = std::exp(zenith[c] - tau[c]);
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string LoadShader(const char* relativePath)
{
#ifndef GE_RENDERER_REPO_ROOT
    (void)relativePath;
    return {};
#else
    return ReadTextFile(std::filesystem::path(GE_RENDERER_REPO_ROOT) /
                        "Engine/Modules/Rendering/Shaders" / relativePath);
#endif
}

// Engine sources under Engine/Source, read from the repo for the same reason LoadShader does: the
// source file is the artifact under test.
std::string LoadEngineSource(const char* relativePath)
{
#ifndef GE_RENDERER_REPO_ROOT
    (void)relativePath;
    return {};
#else
    return ReadTextFile(std::filesystem::path(GE_RENDERER_REPO_ROOT) / "Engine/Source" /
                        relativePath);
#endif
}

} // namespace

// Verify the yardstick before measuring with it. For a straight-up ray the path parameter IS the
// height, so the optical depth has a closed form, beta * H * (1 - exp(-thickness / H)). With Mie
// switched off (its 1200 m scale height is the term any quadrature resolves worst) the test's own
// converged reference must land on it — otherwise every comparison below is against a broken ruler.
TEST(SkySunColor, TheReferenceIntegralMatchesTheAnalyticRadialForm)
{
    Rendering::AtmosphereParametersGPU atmo = DefaultAtmosphere();
    atmo.betaMie[0] = atmo.betaMie[1] = atmo.betaMie[2] = 0.0f;
    const float thickness = atmo.atmosphereRadius - atmo.planetRadius;

    double reference[3];
    ReferenceOpticalDepth(atmo, 1.0, reference);

    for (int c = 0; c < 3; ++c)
    {
        const double expected = atmo.betaRayleigh[c] * atmo.rayleighScaleHeight *
                                (1.0 - std::exp(-thickness / atmo.rayleighScaleHeight));
        EXPECT_NEAR(reference[c], expected, expected * 0.001) << "channel " << c;
    }
}

// The 32-step quadrature is inherited from the shader, not a free parameter: it has to be close
// enough to the converged integral that the light's colour is the atmosphere's answer rather than
// the step count's.
TEST(SkySunColor, QuadratureAgreesWithItsConvergedLimit)
{
    const Rendering::AtmosphereParametersGPU atmo = DefaultAtmosphere();

    for (float elevation : {90.0f, 45.0f, 30.0f, 15.0f, 7.5f, 2.0f, 0.0f})
    {
        const float mu = MuForElevationDegrees(elevation);
        float actual[3];
        Rendering::EvaluateGroundLevelSunColor(atmo, kWhite, mu, actual);

        double reference[3];
        ReferenceGroundColor(atmo, mu, reference);

        for (int c = 0; c < 3; ++c)
        {
            // Relative on a floor, because the horizon's blue channel is ~1e-4 and an absolute
            // tolerance there would pass anything.
            const double tolerance = std::max(reference[c] * 0.01, 1e-4);
            EXPECT_NEAR(actual[c], reference[c], tolerance)
                << "elevation " << elevation << " channel " << c;
        }
    }
}

// An overhead source comes through unchanged. A directional light's authored illuminance is
// already a ground value, so re-applying the overhead extinction to it would dim every scene's key
// light at noon — the one hour this change is not allowed to touch.
TEST(SkySunColor, AnOverheadSourceIsUnchanged)
{
    const Rendering::AtmosphereParametersGPU atmo = DefaultAtmosphere();
    const float source[3] = {0.9f, 0.8f, 0.7f};

    float color[3];
    Rendering::EvaluateGroundLevelSunColor(atmo, source, 1.0f, color);

    for (int c = 0; c < 3; ++c)
        EXPECT_FLOAT_EQ(color[c], source[c]) << "channel " << c;
}

// The shape of an atmospheric sun: monotonically dimmer as the path lengthens, and reddening as it
// dims because blue scatters out first. Monotonic THROUGH the horizon crossing as well — the raw
// integral is not (a chord through the planet is shorter than the grazing path above it), which
// would make a setting sun flare before it went out.
TEST(SkySunColor, DimsAndReddensMonotonicallyAsTheSunDescends)
{
    const Rendering::AtmosphereParametersGPU atmo = DefaultAtmosphere();

    float previous[3];
    Rendering::EvaluateGroundLevelSunColor(atmo, kWhite, MuForElevationDegrees(90.0f), previous);
    EXPECT_FLOAT_EQ(Luminance(previous), 1.0f);

    for (float elevation : {60.0f, 45.0f, 30.0f, 15.0f, 7.5f, 2.0f, 0.5f, 0.0f})
    {
        float current[3];
        Rendering::EvaluateGroundLevelSunColor(atmo, kWhite, MuForElevationDegrees(elevation),
                                               current);
        EXPECT_LT(Luminance(current), Luminance(previous)) << "elevation " << elevation;
        for (int c = 0; c < 3; ++c)
            EXPECT_LT(current[c], previous[c]) << "elevation " << elevation << " channel " << c;
        EXPECT_GT(current[0] / std::max(current[2], 1e-9f),
                  previous[0] / std::max(previous[2], 1e-9f))
            << "elevation " << elevation << " must be redder than the one above it";
        std::copy(std::begin(current), std::end(current), std::begin(previous));
    }

    for (float elevation : {-0.5f, -1.0f, -2.0f, -3.0f, -5.0f})
    {
        float current[3];
        Rendering::EvaluateGroundLevelSunColor(atmo, kWhite, MuForElevationDegrees(elevation),
                                               current);
        EXPECT_LE(Luminance(current), Luminance(previous))
            << "elevation " << elevation << " — a set sun must not brighten";
        std::copy(std::begin(current), std::end(current), std::begin(previous));
    }
}

// A 15-degree sun is the case that made this lane: warm, and warm means the channels come out
// ordered R > G > B. Blue above green is a magenta light — no optical depth produces one, so an
// authored ramp is the only thing that can, and this is the assertion that catches one returning.
TEST(SkySunColor, LowSunIsWarmAndOrderedNotMagenta)
{
    const Rendering::AtmosphereParametersGPU atmo = DefaultAtmosphere();

    float color[3];
    Rendering::EvaluateGroundLevelSunColor(atmo, kWhite, MuForElevationDegrees(15.0f), color);

    EXPECT_GT(color[0], color[1]) << "red must survive the longest path";
    EXPECT_GT(color[1], color[2]) << "green above blue: blue scatters out first";
    EXPECT_GT(color[1] / color[2], 1.4f) << "the warmth has to be visible, not a rounding artefact";
    // Still a daylight sun, not a dusk one: a 15-degree sun keeps most of its luminous flux.
    EXPECT_GT(Luminance(color), 0.65f);
    EXPECT_LT(Luminance(color), 0.85f);
}

// Once the moon has taken the primary slot (SkySystemConfig's blend completes at sunY = -0.17,
// about -9.8 degrees) the sun must contribute nothing. A floor here would light a night scene with
// a phantom sun — which is exactly what an authored colour ramp that never reaches zero does.
TEST(SkySunColor, ASetSourceIsExtinguished)
{
    const Rendering::AtmosphereParametersGPU atmo = DefaultAtmosphere();

    for (float elevation : {-9.8f, -30.0f, -90.0f})
    {
        float color[3];
        Rendering::EvaluateGroundLevelSunColor(atmo, kWhite, MuForElevationDegrees(elevation),
                                               color);
        EXPECT_LT(Luminance(color), 1e-5f) << "elevation " << elevation;
    }

    // And it is already almost gone at the horizon itself.
    float atHorizon[3];
    Rendering::EvaluateGroundLevelSunColor(atmo, kWhite, 0.0f, atHorizon);
    EXPECT_LT(Luminance(atHorizon), 0.06f);
}

// The colour above the atmosphere is carried through untouched by anything except the extinction,
// so a moon (or an authored light tint) keeps its own chromaticity on the way down.
TEST(SkySunColor, ScalesTheSourceColourRatherThanReplacingIt)
{
    const Rendering::AtmosphereParametersGPU atmo = DefaultAtmosphere();
    const float mu = MuForElevationDegrees(40.0f);

    float white[3];
    Rendering::EvaluateGroundLevelSunColor(atmo, kWhite, mu, white);

    const float tinted[3] = {0.25f, 0.5f, 2.0f};
    float result[3];
    Rendering::EvaluateGroundLevelSunColor(atmo, tinted, mu, result);

    for (int c = 0; c < 3; ++c)
        EXPECT_NEAR(result[c], white[c] * tinted[c], white[c] * tinted[c] * 1e-5f)
            << "channel " << c;
}

// The daytime sun the sky shaders see must stay unattenuated: they apply the transmittance LUT
// themselves. This is the pairing that turns one reddening into two if it is ever broken.
TEST(SkySunColor, TheSourceAboveTheAtmosphereIsWhiteWhileTheGroundColourIsNot)
{
    Rendering::SkySettings settings{};
    settings.timeOfDayHours = 17.0f;
    const Rendering::SkySystemConfig config{};
    Rendering::SkySystemState state{};

    Rendering::ComputeSimpleSunMoon(settings, config, Rendering::MakeSolarFrame(Rendering::EarthPathAngles({})), state);
    Rendering::ApplySimpleLightingToSkySettings(state, config, kWhite, settings.primarySunIntensity, settings);

    ASSERT_GT(settings.primarySunDir[1], 0.0f) << "17:00 must still put the sun above the horizon";
    EXPECT_FLOAT_EQ(settings.primarySunColor[0], 1.0f);
    EXPECT_FLOAT_EQ(settings.primarySunColor[1], 1.0f);
    EXPECT_FLOAT_EQ(settings.primarySunColor[2], 1.0f);

    float ground[3];
    Rendering::EvaluateGroundLevelSunColor(DefaultAtmosphere(), settings.primarySunColor,
                                           settings.primarySunDir[1], ground);
    EXPECT_GT(ground[0], ground[1]);
    EXPECT_GT(ground[1], ground[2]);
}

// At full night the primary source is the moon, and its stylized illuminance lives in the colour's
// MAGNITUDE (SkySystemConfig::moonColor). Restoring a normalized tint there would brighten every
// night scene by more than two orders of magnitude while looking like a cosmetic edit.
TEST(SkySunColor, TheNightSourceCarriesTheStylizedMoonIlluminance)
{
    Rendering::SkySettings settings{};
    settings.timeOfDayHours = 0.0f;
    const Rendering::SkySystemConfig config{};
    Rendering::SkySystemState state{};

    Rendering::ComputeSimpleSunMoon(settings, config, Rendering::MakeSolarFrame(Rendering::EarthPathAngles({})), state);
    ASSERT_FLOAT_EQ(state.primaryMoonBlend, 1.0f) << "midnight must be full moon blend";
    Rendering::ApplySimpleLightingToSkySettings(state, config, kWhite, settings.primarySunIntensity, settings);

    const float nightLuminance = Luminance(settings.primarySunColor);
    EXPECT_GT(nightLuminance, 1.0f / 400.0f);
    EXPECT_LT(nightLuminance, 1.0f / 100.0f);
    EXPECT_GT(settings.primarySunColor[2], settings.primarySunColor[0]) << "moonlight reads cool";
}

// THE CHAIN, not the function. `DimsAndReddensMonotonicallyAsTheSunDescends` above pins the
// evaluator; it stayed green while the linked light snapped back on from the EAST 18 game-minutes
// after sunset, because the defect was in WHICH elevation the system handed it. This walks the
// production path — time of day -> ComputeSimpleSunMoon -> ApplySimpleLightingToSkySettings ->
// EvaluateBodyGroundColors -> the two mixes — across the sun/moon handoff and asserts what a lit
// surface may see.
//
// The quantity is the DELIVERED light — the luminance of the linked light's colour, the energy blend
// of the two bodies the system writes (MixLinkedLightGroundColor) — not the sky's ground colour
// (MixPrimaryGroundColor) alone, which mixes the bodies by colour and would let a flare in the
// light's own blend through untouched. This mixes the bodies rather than running the system, so it
// pins the chain's inputs; SkyLinkedSun.TheDeliveredLightMatchesItsGoldensAcrossTheDay pins what the
// system writes.
//
// Two properties, both violated by evaluating one extinction at the BLENDED direction (which, with
// the default antipodal moon, flips sign at blend 0.5):
//   * no step may brighten the delivered light by more than half again — a crossfading moon rises
//     gently, it does not flash (the defect stepped ~900x in one 0.05 h tick);
//   * once the sun is 2 degrees under, the source COLOUR may never exceed moonlight's — the sun has
//     set, so nothing may still be tinted as if it had not.
TEST(SkySunColor, TheLinkedLightNeverFlaresBackOnAcrossTheSunsetHandoff)
{
    const Rendering::AtmosphereParametersGPU atmo = DefaultAtmosphere();
    const Rendering::SkySystemConfig config{};
    const float moonSourceLuminance = Luminance(config.moonColor);

    float previous = -1.0f;
    float firstLuminance = 0.0f;
    float lastLuminance = 0.0f;
    for (int step = 0; step <= 18; ++step) // 17.90 .. 18.80 in 0.05 h
    {
        const float hour = 17.9f + 0.05f * static_cast<float>(step);

        Rendering::SkySettings settings{};
        settings.timeOfDayHours = hour;
        Rendering::SkySystemState state{};
        Rendering::ComputeSimpleSunMoon(settings, config, Rendering::MakeSolarFrame(Rendering::EarthPathAngles({})), state);
        Rendering::ApplySimpleLightingToSkySettings(state, config, kWhite, settings.primarySunIntensity, settings);

        const Rendering::SkyBodyGroundColors bodies = Rendering::EvaluateBodyGroundColors(atmo, config, state, kWhite);
        float ground[3];
        Rendering::MixPrimaryGroundColor(bodies, ground);

        // The colour SkyEnvironmentSystem writes to the linked light, the moon shown.
        float linkedLight[3];
        Rendering::MixLinkedLightGroundColor(bodies, Rendering::LinkedLightMoonScale(config, true), linkedLight);
        const float delivered = Luminance(linkedLight);

        if (previous >= 0.0f)
            EXPECT_LE(delivered, previous * 1.5f)
                << "hour " << hour << ": the delivered light brightened " << (delivered / previous)
                << "x in one 0.05 h step";

        const float sunElevationDeg = std::asin(std::clamp(state.sunDirWS[1], -1.0f, 1.0f)) *
                                      180.0f / 3.14159265358979323846f;
        if (sunElevationDeg <= -2.0f)
            EXPECT_LT(Luminance(ground), moonSourceLuminance * 1.2f)
                << "hour " << hour << ": sun is " << sunElevationDeg
                << " degrees under and the source colour is brighter than moonlight";

        if (step == 0)
            firstLuminance = delivered;
        lastLuminance = delivered;
        previous = delivered;
    }

    // And the window as a whole is a sunset: the delivered light ends far below where it started.
    EXPECT_LT(lastLuminance, firstLuminance / 1000.0f);
}

// The primary direction must stay on one of the two bodies. An antipodal moon at blend 0.5 makes
// the lerp a zero vector, and Normalize3's fallback aims it at the zenith — which is also what fed
// the flare above once the colour became a function of this direction.
TEST(SkySunColor, TheBlendedPrimaryDirectionNeverCollapsesToTheZenith)
{
    const Rendering::SkySystemConfig config{};
    Rendering::SkySettings settings{};
    Rendering::SkySystemState state{};

    const float sun[3] = {-0.9986f, -0.0523f, 0.0f}; // 3 degrees under, west
    state.sunDirWS[0] = sun[0];
    state.sunDirWS[1] = sun[1];
    state.sunDirWS[2] = sun[2];
    state.moonDirWS[0] = -sun[0];
    state.moonDirWS[1] = -sun[1];
    state.moonDirWS[2] = -sun[2];
    state.primaryMoonBlend = 0.5f;

    Rendering::ApplySimpleLightingToSkySettings(state, config, kWhite, settings.primarySunIntensity, settings);

    const float alignment = std::abs(settings.primarySunDir[0] * sun[0] +
                                     settings.primarySunDir[1] * sun[1] +
                                     settings.primarySunDir[2] * sun[2]);
    EXPECT_GT(alignment, 0.99f) << "primary direction (" << settings.primarySunDir[0] << ", "
                                << settings.primarySunDir[1] << ", " << settings.primarySunDir[2]
                                << ") is on neither body";
}

// Each body is extinguished where IT is. A sun below the horizon contributes nothing even while the
// blend still counts it, and a risen moon contributes its own attenuated colour.
TEST(SkySunColor, EachBodyIsExtinguishedAtItsOwnElevation)
{
    const Rendering::AtmosphereParametersGPU atmo = DefaultAtmosphere();
    const Rendering::SkySystemConfig config{};
    Rendering::SkySystemState state{};
    state.sunDirWS[1] = MuForElevationDegrees(-4.5f);
    state.moonDirWS[1] = MuForElevationDegrees(4.5f);
    state.primaryMoonBlend = 0.5f;

    float ground[3];
    Rendering::MixPrimaryGroundColor(Rendering::EvaluateBodyGroundColors(atmo, config, state, kWhite), ground);

    float moonOnly[3];
    Rendering::EvaluateGroundLevelSunColor(atmo, config.moonColor, state.moonDirWS[1], moonOnly);

    // Half the moon's own contribution, plus a set sun's ~nothing.
    for (int c = 0; c < 3; ++c)
        EXPECT_NEAR(ground[c], 0.5f * moonOnly[c], 0.5f * moonOnly[c] + 0.002f) << "channel " << c;
    EXPECT_LT(Luminance(ground), Luminance(config.moonColor))
        << "a set sun must not out-light the moon it is handing over to";
}

// The CPU mirror is only honest while the shader integrates the same thing. These are the pieces
// the mirror copies; if one moves, the light stops agreeing with the dome above it and nothing
// else in the build notices.
TEST(SkySunColor, TransmittanceShaderStillIntegratesTheMirroredModel)
{
    const std::string shader = LoadShader("sky_transmittance_lut.comp");
    ASSERT_FALSE(shader.empty()) << "sky_transmittance_lut.comp not found via GE_RENDERER_REPO_ROOT";

    EXPECT_NE(shader.find("const int kSteps = 32;"), std::string::npos)
        << "step count must match kOpticalDepthSteps in AtmosphereTransmittance.cpp";
    EXPECT_NE(shader.find("float sm = 0.5 * (s0 + s1);"), std::string::npos)
        << "midpoint sampling";
    EXPECT_NE(shader.find("float r2 = r * r + sm * sm + 2.0 * r * mu * sm;"), std::string::npos)
        << "sample radius along the ray";
    EXPECT_NE(shader.find("vec3 sigma = uAtmos.betaRayleigh * rayleigh + uAtmos.betaMie * mie;"),
              std::string::npos)
        << "extinction is Rayleigh + Mie with no tint or floor";
    EXPECT_NE(shader.find("vec3 transmittance = exp(-tau);"), std::string::npos);
    EXPECT_NE(shader.find("sMax = sGround;"), std::string::npos)
        << "downward rays clamp to their own hit with the ground";
}

// The sky-view LUT applies the transmittance toward the sun at every march step. That is the OTHER
// half of the pairing: as long as it does, the colour it is handed must be the above-atmosphere
// one.
TEST(SkySunColor, SkyViewLutStillAppliesItsOwnTransmittanceToTheSunColour)
{
    const std::string lut = LoadShader("sky_view_lut.comp");
    ASSERT_FALSE(lut.empty()) << "sky_view_lut.comp not found via GE_RENDERER_REPO_ROOT";
    EXPECT_NE(lut.find("transToSun * uSky.sunColor * uSky.sunIntensity"), std::string::npos)
        << "the in-scatter source term must keep applying the transmittance itself";
}

// ---------------------------------------------------------------------------------------------
// The stylistic Sun Tint, through the real system rather than a mirror of its arithmetic.
//
// The tests above pin the evaluator and the shader contract; none of them would notice if
// SkyEnvironmentSystem stopped applying the tint, or applied it twice on one path. These run
// SkyEnvironmentSystem::Update against a real World and RenderServices and read the values
// production actually consumes: the source above the atmosphere (the sky-view LUT's in-scatter,
// the sun disc and the IBL capture each take this one verbatim), the ground-level colour, the
// linked directional light, and the fog sun.
//
// A tint on ONE channel is what makes "exactly once" falsifiable. A second application squares the
// factor, so 2x would read 4x, and a missing one reads 1x — neither can hide inside a tolerance
// the way a uniform scale can.
// ---------------------------------------------------------------------------------------------

namespace
{

// The consumers a source-side tint must reach, read from the production objects after an Update.
struct SunColourFanout
{
    float SourceAboveAtmosphere[3];
    float GroundColour[3];
    float LinkedLightColour[3];
    float FogSunColour[3];
};

struct SkyTintHarness
{
    ECS::World World;
    Engine::Renderer::RenderServices Services;
    Engine::Renderer::SkyEnvironmentSystem System{&Services};
    ECS::EntityHandle SkyEntity;
    ECS::EntityHandle LightEntity;

    SkyTintHarness()
    {
        LightEntity = World.CreateEntity();
        Components::Light light{};
        light.Type = Components::LightType::Directional;
        light.Intensity = 100000.0f;
        World.AddComponentImmediate(LightEntity, light);
        World.AddComponentImmediate(LightEntity, Components::Transform{});

        SkyEntity = World.CreateEntity();
        Components::SkyEnvironment sky{};
        sky.TimeOfDayHours = 17.0f; // sun ~15 degrees up: warm, and far from the moon handoff
        sky.SunLight = LightEntity;
        sky.TimeOfDayDrivesSunLight = true;
        World.AddComponentImmediate(SkyEntity, sky);
    }

    void SetTimeOfDay(float hours)
    {
        auto* sky = World.GetComponent<Components::SkyEnvironment>(SkyEntity);
        ASSERT_NE(sky, nullptr);
        Components::SkyEnvironment updated = *sky;
        updated.TimeOfDayHours = hours;
        World.AddComponentImmediate(SkyEntity, updated);
    }

    void SetTint(float r, float g, float b)
    {
        auto* sky = World.GetComponent<Components::SkyEnvironment>(SkyEntity);
        ASSERT_NE(sky, nullptr);
        Components::SkyEnvironment updated = *sky;
        Components::SkyVec3DayKeysSetUniform(updated.SunTintKeys, r, g, b);
        World.AddComponentImmediate(SkyEntity, updated);
    }

    // One production tick, then read every consumer.
    SunColourFanout Run()
    {
        System.Update(World, 0.0f);

        auto* feature = Services.GetFeature<Engine::Renderer::SkyRenderFeature>();
        EXPECT_NE(feature, nullptr);
        EXPECT_TRUE(feature->HasActiveSettings());
        const Rendering::SkySettings& settings = feature->GetSettings();

        SunColourFanout out{};
        for (int c = 0; c < 3; ++c)
        {
            out.SourceAboveAtmosphere[c] = settings.primarySunColor[c];
            out.GroundColour[c] = settings.primarySunGroundColor[c];
        }

        const auto* light = World.GetComponent<Components::Light>(LightEntity);
        EXPECT_NE(light, nullptr);
        float lightIntensity = 0.0f;
        Components::ResolveLightColorIntensity(*light, out.LinkedLightColour, lightIntensity);

        // No view is registered on a headless RenderServices, so the fog takes its documented
        // sky-anchored fallback — the branch that reads primarySunGroundColor. The tracked-light
        // branch copies the same light LinkedLightColour already asserts on.
        const float fallbackDir[3] = {0.0f, -1.0f, 0.0f};
        const float fallbackColor[3] = {1.0f, 1.0f, 1.0f};
        const Engine::Renderer::ResolvedFogSun fog = Engine::Renderer::ResolveHeightFogSunLighting(
            Services, ::GameEngine::Rendering::ViewId{}, /*trackDirectionalLight=*/true,
            /*sunIntensityScale=*/1.0f, fallbackDir, fallbackColor, /*fallbackIntensity=*/1.0f);
        for (int c = 0; c < 3; ++c)
            out.FogSunColour[c] = fog.color[c];
        return out;
    }
};

float RelTolerance(float expected)
{
    return std::max(std::abs(expected) * 1e-5f, 1e-7f);
}

} // namespace

// The default tint is white at every key, so a scene that never touches it renders exactly the
// physical sky. If this fails, the lever is not free and every existing scene moved.
TEST(SkySunColor, TheDefaultSunTintIsIdentity)
{
    Components::SkyEnvironment defaults{};
    const float* keys[4] = {defaults.SunTintKeys.Midnight, defaults.SunTintKeys.Dawn,
                            defaults.SunTintKeys.Midday, defaults.SunTintKeys.Sunset};
    for (const float* key : keys)
        for (int c = 0; c < 3; ++c)
            EXPECT_FLOAT_EQ(key[c], 1.0f);
}

// EXACTLY ONCE, on every consumer at the same time. Doubling one channel of the source must double
// that channel of the sky's source term, of the ground colour, of the linked light and of the fog
// sun — and must leave the other two channels alone.
TEST(SkySunColor, AStylisticSunTintReachesEveryConsumerExactlyOnce)
{
    SkyTintHarness harness;
    const SunColourFanout physical = harness.Run();

    ASSERT_GT(physical.GroundColour[0], 0.0f) << "17:00 must deliver a lit sun to scale";
    for (int c = 0; c < 3; ++c)
        EXPECT_FLOAT_EQ(physical.SourceAboveAtmosphere[c], 1.0f)
            << "the untinted daytime source is white above the atmosphere, channel " << c;

    constexpr float kRedGain = 2.0f;
    harness.SetTint(kRedGain, 1.0f, 1.0f);
    const SunColourFanout tinted = harness.Run();

    const float expected[3] = {kRedGain, 1.0f, 1.0f};
    for (int c = 0; c < 3; ++c)
    {
        const float wantSource = physical.SourceAboveAtmosphere[c] * expected[c];
        const float wantGround = physical.GroundColour[c] * expected[c];
        const float wantLight = physical.LinkedLightColour[c] * expected[c];
        const float wantFog = physical.FogSunColour[c] * expected[c];
        EXPECT_NEAR(tinted.SourceAboveAtmosphere[c], wantSource, RelTolerance(wantSource))
            << "sky in-scatter / disc / IBL source, channel " << c;
        EXPECT_NEAR(tinted.GroundColour[c], wantGround, RelTolerance(wantGround))
            << "ground-level colour, channel " << c;
        EXPECT_NEAR(tinted.LinkedLightColour[c], wantLight, RelTolerance(wantLight))
            << "linked directional light, channel " << c;
        EXPECT_NEAR(tinted.FogSunColour[c], wantFog, RelTolerance(wantFog))
            << "fog sun, channel " << c;
    }

    // The fog sun and the light must be the SAME colour, not merely proportional: both take
    // primarySunGroundColor, so a divergence would mean one picked up an extra factor on the way.
    for (int c = 0; c < 3; ++c)
        EXPECT_NEAR(tinted.FogSunColour[c], tinted.LinkedLightColour[c],
                    RelTolerance(tinted.LinkedLightColour[c]));
}

// The IBL leg, as a source contract — the same way this file pins the shader side, and for the same
// reason: SkyEnvironmentSource::InputDigest and HasActiveSky need an initialized GPU sky, which a
// headless RenderServices has no way to provide, so the behaviour is pinned where it is decided.
//
// Two links carry the tint into ambient and reflections, and both are one line of this file:
//   * the capture's UBO takes primarySunColor VERBATIM, so the value the test above proved is
//     exactly 2x is the value the bake scatters (every convolve downstream is a weighted sum with
//     colour-independent weights);
//   * InputDigest MIXES primarySunColor, so authoring the tint dirties the bake. Without that, the
//     value would be right and the cube would still hold the untinted sky until something else
//     happened to dirty it — ambient silently a sky behind.
TEST(SkySunColor, TheIblCaptureTakesTheSourceColourAndRebakesWhenItChanges)
{
    const std::string source = LoadEngineSource("Engine/Rendering/SkyEnvironmentSource.cpp");
    ASSERT_FALSE(source.empty())
        << "SkyEnvironmentSource.cpp not found via GE_RENDERER_REPO_ROOT";

    EXPECT_NE(source.find("ubo.sunColor[i] = s.primarySunColor[i]"), std::string::npos)
        << "the capture must bake the above-atmosphere source colour verbatim";
    EXPECT_NE(source.find("mix(s.primarySunColor, sizeof(s.primarySunColor))"), std::string::npos)
        << "InputDigest must hash primarySunColor, or authoring the tint never rebakes the IBL";
}

// The tint is the SUN's, and at full night there is no sun. The moon owns the sky, its colour is
// SkySystemConfig::moonColor with nothing applied to it, and the visible moon DISC is drawn from
// that same untinted colour — so a tint that reached moonlight would light the ground in a colour
// the moon on screen does not have. An authored night key must therefore change nothing at all.
//
// This is the assertion that fails if the tint is ever folded back into the blended source: at
// midnight the blend is fully moon, so a (1, 0.5, 0.5) tint would halve green and blue everywhere.
TEST(SkySunColor, AtFullNightTheTintDoesNothingBecauseTheMoonIsNotTheSun)
{
    SkyTintHarness harness;
    harness.SetTimeOfDay(0.0f);
    const SunColourFanout untinted = harness.Run();

    harness.SetTint(1.0f, 0.5f, 0.5f);
    const SunColourFanout tinted = harness.Run();

    for (int c = 0; c < 3; ++c)
    {
        EXPECT_NEAR(tinted.SourceAboveAtmosphere[c], untinted.SourceAboveAtmosphere[c],
                    RelTolerance(untinted.SourceAboveAtmosphere[c]))
            << "the sky's night source must be the moon's own colour, channel " << c;
        EXPECT_NEAR(tinted.GroundColour[c], untinted.GroundColour[c],
                    RelTolerance(untinted.GroundColour[c]))
            << "moonlight on the ground must be untinted, channel " << c;
        EXPECT_NEAR(tinted.LinkedLightColour[c], untinted.LinkedLightColour[c],
                    RelTolerance(untinted.LinkedLightColour[c]))
            << "the linked light at night must be untinted, channel " << c;
    }

    // And the reading is meaningful: midnight really is full moon blend, so this is not a green
    // light bought by testing an hour where the tint happened to be white anyway.
    Rendering::SkySettings probe{};
    probe.timeOfDayHours = 0.0f;
    const Rendering::SkySystemConfig config{};
    Rendering::SkySystemState state{};
    Rendering::ComputeSimpleSunMoon(probe, config, Rendering::MakeSolarFrame(Rendering::EarthPathAngles({})), state);
    ASSERT_FLOAT_EQ(state.primaryMoonBlend, 1.0f);
}

// The tint is a SOURCE term, so the atmosphere still gets to redden it. A tinted run must keep the
// same extinction ratio the untinted run had — if a consumer applied the tint after extinction, or
// the extinction were re-derived from the tinted colour, this ratio would move.
TEST(SkySunColor, TheTintChangesTheSourceAndLeavesTheTransportPhysical)
{
    SkyTintHarness harness;
    const SunColourFanout physical = harness.Run();
    harness.SetTint(1.0f, 0.62f, 0.35f); // the orange evening key from the look call
    const SunColourFanout tinted = harness.Run();

    for (int c = 0; c < 3; ++c)
    {
        const float physicalExtinction =
            physical.GroundColour[c] / physical.SourceAboveAtmosphere[c];
        const float tintedExtinction = tinted.GroundColour[c] / tinted.SourceAboveAtmosphere[c];
        EXPECT_NEAR(tintedExtinction, physicalExtinction, RelTolerance(physicalExtinction))
            << "the atmosphere's transmittance must not depend on the tint, channel " << c;
    }
}


