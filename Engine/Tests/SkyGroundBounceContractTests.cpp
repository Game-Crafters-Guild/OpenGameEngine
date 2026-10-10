// Contract for the ground bounce baked into the IBL.
//
// The physical below-horizon modes describe how the VISIBLE backdrop is drawn, but the
// ground exists under the camera either way, so the baked environment must carry its
// bounce in both. Before this term existed the capture folded horizon SKY into the lower
// hemisphere, and a surface in shadow received clear-sky irradiance with no light from
// the ground — a shadowed neutral read blue instead of dark and desaturated.
//
// Properties worth pinning in source, because each failed silently in review:
//   1. The bounce MATH has ONE implementation. What a lit floor is worth must be computed
//      in one place; a second inlined copy is how the two consumers drift apart, and the
//      drift is invisible until someone compares a cube face against a background pixel.
//      This is NOT a claim that both consumers composite a floor in the same situations.
//      WHETHER to composite is each call site's own gate and the two deliberately differ
//      (see CaptureBakesTheBounceForEveryPhysicalMode): in the shipped configuration --
//      no scene pins PlanetGround -- the capture bakes a floor while the visible sky folds
//      the horizon haze down, and that split is intended, not a parity defect.
//   2. The capture bakes the bounce for EVERY physical mode, not just PlanetGround.
//      Every scene in the repo pins BelowHorizonMode = ContinueHorizon (0), so gating
//      the bake on `== 1u` would ship a feature that reaches no content at all while
//      still compiling, still passing a parity test, and still looking plausible.
//   3. The skylight the floor receives is a cosine-weighted hemispherical MEAN and carries
//      no tuning fraction. Under a uniform sky of radiance L a Lambertian floor of albedo
//      p leaves exactly p*L, so any constant in front of that term is an energy error
//      rather than a dial. The form this replaced was p * L_zenith * 0.30, measured 4.6x
//      low at midday and 5.8x low at sunset against the engine's own sky-view LUT
//      (physamb2 probe arms, 2026-08-17).
//
// Reads the repo shader source via GE_RENDERER_REPO_ROOT, the same dev-only anchor
// IblShaderContractTests uses: the source file is the artifact under test, and a staged
// copy only refreshes when its staging target rebuilds, which a shader-only edit does not.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include "Components/Rendering/SkyEnvironment.h"

namespace
{

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open())
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Strip //-comments so prose naming a symbol never counts as a use of it.
std::string StripLineComments(const std::string& source)
{
    std::string out;
    out.reserve(source.size());
    std::size_t pos = 0;
    while (pos < source.size())
    {
        const std::size_t comment = source.find("//", pos);
        const std::size_t lineEnd = source.find('\n', pos);
        if (comment == std::string::npos || (lineEnd != std::string::npos && comment > lineEnd))
        {
            if (lineEnd == std::string::npos)
            {
                out.append(source, pos, std::string::npos);
                break;
            }
            out.append(source, pos, lineEnd + 1 - pos);
            pos = lineEnd + 1;
            continue;
        }
        out.append(source, pos, comment - pos);
        if (lineEnd == std::string::npos)
            break;
        out.push_back('\n');
        pos = lineEnd + 1;
    }
    return out;
}

std::size_t CountOccurrences(const std::string& haystack, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size()))
        ++count;
    return count;
}

std::string LoadShaderWithoutComments(const char* relativePath)
{
#ifndef GE_RENDERER_REPO_ROOT
    (void)relativePath;
    return {};
#else
    const std::filesystem::path path =
        std::filesystem::path(GE_RENDERER_REPO_ROOT) / "Engine/Modules/Rendering/Shaders" / relativePath;
    return StripLineComments(ReadTextFile(path));
#endif
}

std::string LoadComposite()   { return LoadShaderWithoutComments("Includes/sky_composite.glsl"); }
std::string LoadSkyRender()   { return LoadShaderWithoutComments("sky_render.frag"); }
std::string LoadSkyCapture()  { return LoadShaderWithoutComments("sky_capture_cube.frag"); }

} // namespace

// The visible sky and the IBL capture must reach the floor through the SAME function.
// Deleting either call is the regression that decouples what a surface receives from
// what the camera sees; deleting the capture's call removes the feature entirely.
TEST(SkyGroundBounceContract, BothSkyConsumersCallTheSharedGroundFloor)
{
    const std::string render = LoadSkyRender();
    const std::string capture = LoadSkyCapture();
    ASSERT_FALSE(render.empty()) << "sky_render.frag not found via GE_RENDERER_REPO_ROOT";
    ASSERT_FALSE(capture.empty()) << "sky_capture_cube.frag not found via GE_RENDERER_REPO_ROOT";

    EXPECT_GE(CountOccurrences(render, "GE_CompositeGroundFloor("), 1u)
        << "the visible sky must composite its floor through the shared implementation";
    EXPECT_GE(CountOccurrences(capture, "GE_CompositeGroundFloor("), 1u)
        << "the IBL capture must bake the ground bounce through the shared implementation — "
           "without it the lower hemisphere is folded sky and shadows stay clear-sky blue";
}

// One implementation, and it lives in the shared include. A consumer that re-inlines the
// bounce is free to drift from the other, which is exactly the defect the sharing exists
// to prevent — and the drift only shows up in a cube-face-vs-background comparison.
TEST(SkyGroundBounceContract, GroundBounceMathHasASingleImplementation)
{
    const std::string composite = LoadComposite();
    const std::string render = LoadSkyRender();
    const std::string capture = LoadSkyCapture();
    ASSERT_FALSE(composite.empty()) << "sky_composite.glsl not found via GE_RENDERER_REPO_ROOT";
    ASSERT_FALSE(render.empty());
    ASSERT_FALSE(capture.empty());

    EXPECT_EQ(CountOccurrences(composite, "vec3 GE_GroundBounceRadiance("), 1u)
        << "the bounce must be defined exactly once, in the shared include";

    // The albedo/PI product is the signature of the direct term. It must not appear in
    // either consumer: that would be a second copy of the math.
    for (const auto& [name, source] :
         {std::pair<const char*, const std::string&>{"sky_render.frag", render},
          std::pair<const char*, const std::string&>{"sky_capture_cube.frag", capture}})
    {
        EXPECT_EQ(CountOccurrences(source, "groundAlbedo * (1.0 /"), 0u)
            << name << " inlines the ground-bounce direct term instead of calling the shared "
                       "implementation — the two consumers can now drift apart";
    }
}

// Every scene in the repo pins BelowHorizonMode = ContinueHorizon (0). Gating the BAKE on
// PlanetGround (== 1u) compiles, keeps parity, and reaches no content whatsoever — a
// silent no-op ship. The bake gate must therefore exclude only StylizedGround, which owns
// its authored ground colour.
TEST(SkyGroundBounceContract, CaptureBakesTheBounceForEveryPhysicalMode)
{
    const std::string capture = LoadSkyCapture();
    ASSERT_FALSE(capture.empty()) << "sky_capture_cube.frag not found via GE_RENDERER_REPO_ROOT";

    const std::size_t call = capture.find("GE_CompositeGroundFloor(");
    ASSERT_NE(call, std::string::npos);

    // The guard immediately preceding the call must exclude StylizedGround, not select
    // PlanetGround.
    const std::size_t guard = capture.rfind("belowHorizonMode", call);
    ASSERT_NE(guard, std::string::npos) << "no below-horizon guard found before the bake call";
    const std::string condition = capture.substr(guard, call - guard);

    EXPECT_NE(condition.find("!= 2u"), std::string::npos)
        << "the capture must bake the bounce for every mode except StylizedGround; gating it "
           "on PlanetGround reaches no scene in the repo, because they all pin ContinueHorizon";
}

// Physical invariants of the bounce that a refactor can silently drop. Both clamps are
// load-bearing: without the NdotL clamp a sun below the horizon drives the direct term
// negative (night scenes gain inverted bounce), and without the brightness clamp a
// negative authored brightness flips the whole term.
TEST(SkyGroundBounceContract, BounceClampsSunAngleAndBrightness)
{
    const std::string composite = LoadComposite();
    ASSERT_FALSE(composite.empty()) << "sky_composite.glsl not found via GE_RENDERER_REPO_ROOT";

    const std::size_t begin = composite.find("vec3 GE_GroundBounceRadiance(");
    ASSERT_NE(begin, std::string::npos);
    const std::size_t end = composite.find("\n}", begin);
    ASSERT_NE(end, std::string::npos);
    const std::string body = composite.substr(begin, end - begin);

    EXPECT_NE(body.find("max(dot(surfaceUp, sunDir), 0.0)"), std::string::npos)
        << "the sun angle must clamp at zero — an unclamped NdotL lets a below-horizon sun "
           "drive the bounce negative";
    EXPECT_NE(body.find("max(groundBrightness, 0.0)"), std::string::npos)
        << "ground brightness must clamp at zero — a negative value would invert the bounce";
}

// The ground bounce is the only lever that reaches a vertical surface, so the lower
// hemisphere must not be darkened by default: the darkening would subtract exactly the
// warm energy the bounce adds. (It attenuates by the RECEIVING normal, so it never
// touched verticals anyway — but on down-facing normals it directly cancels the bounce.)
TEST(SkyGroundBounceContract, LowerHemisphereDarkeningIsOffByDefault)
{
    const GameEngine::Components::SkyEnvironment sky{};
    EXPECT_FLOAT_EQ(sky.IblLowerHemisphereDarkness, 0.0f)
        << "darkening the lower hemisphere by default subtracts the baked ground bounce";
}

// The ground albedo is the measured stand-in for the scene's own floor: the largest neutral
// grey whose baked lower hemisphere stays at or below a whole-scene probe capture of the
// default scene's floor at noon. Neutral, because the model knows nothing of the ground's
// colour; the same at every key, because a ground does not change colour with the hour (the
// bounce's brightness already follows the sun, the sky and GroundBrightnessKeys).
TEST(SkyGroundBounceContract, GroundAlbedoIsTheMeasuredNeutralGreyAtEveryHour)
{
    const GameEngine::Components::SkyEnvironment sky{};
    const auto& keys = sky.GroundAlbedoKeys;
    for (const auto& [name, key] : {std::pair{"midnight", keys.Midnight}, std::pair{"dawn", keys.Dawn},
                                     std::pair{"midday", keys.Midday}, std::pair{"sunset", keys.Sunset}})
    {
        SCOPED_TRACE(name);
        EXPECT_FLOAT_EQ(key[0], 0.59f);
        EXPECT_FLOAT_EQ(key[1], key[0]) << "the default ground carries no hue of its own";
        EXPECT_FLOAT_EQ(key[2], key[0]) << "the default ground carries no hue of its own";
    }
}

// The ground's brightness multiplier is flat across the day: the bounce already falls with
// the sun's N.L and transmittance and with the sky's mean radiance, so a day curve on top
// would dim it twice and leave the stand-in darker than the plane it stands in for. Both
// representations are flat: the one the default mode evaluates, and the Bezier a scene that
// selects the cubic Bezier mode without authoring one receives.
TEST(SkyGroundBounceContract, GroundBrightnessIsFlatAcrossTheDay)
{
    const GameEngine::Components::SkyEnvironment sky{};
    for (const float hour : {0.0f, 3.0f, 6.0f, 9.0f, 12.0f, 16.0f, 18.0f, 21.0f})
    {
        SCOPED_TRACE(hour);
        EXPECT_FLOAT_EQ(GameEngine::Components::EvaluateSkyScalarDayCurve(sky.GroundBrightnessKeys,
                                                                          sky.GroundBrightnessShapeMode,
                                                                          sky.GroundBrightnessBezier, hour),
                        0.9f);
        EXPECT_NEAR(GameEngine::Components::EvaluateSkyScalarCubicBezier(sky.GroundBrightnessBezier, hour), 0.9f,
                    1e-6f);
    }
}

// PARITY ANCHOR. The ambient a surface receives must match the ground a camera sees, and
// the two call sites reach it through one function -- so parity is decided by the GEOMETRY
// each site feeds in, not by the math. The IBL capture renders from the world origin
// (SkyEnvironmentSource.cpp, FillCaptureUbo: cameraPositionWS = 0), and the shader places
// the planet centre at (0, -planetRadius, 0), so the capture camera sits EXACTLY on the
// sphere. There the ray-sphere solve is degenerate -- t0 = tca - thc = R*sin(t) - R*sin(t)
// = 0 for every below-horizon direction -- and a `t0 > 0` guard drops the entire lower
// hemisphere, leaving float32 rounding to decide which directions get a ground bounce.
//
// Measured on the shipped `<=` form: the capture's -Y face read (63.5, 100.1, 137.4),
// R/B 0.46 -- sky, not the warm floor -- while the floor itself is R/B ~2.6.
//
// The fix is the analytic surface case, and this pins its two halves.
TEST(SkyGroundBounceContract, AtSurfaceCaptureCompositesTheFloorAnalytically)
{
    const std::string composite = LoadComposite();
    ASSERT_FALSE(composite.empty()) << "sky_composite.glsl not found via GE_RENDERER_REPO_ROOT";

    const std::size_t begin = composite.find("vec3 GE_CompositeGroundFloor(");
    ASSERT_NE(begin, std::string::npos);
    const std::string body = composite.substr(begin);

    EXPECT_NE(body.find("kGroundFloorSurfaceToleranceRelative"), std::string::npos)
        << "the at-surface case must be detected by a named tolerance on the camera's altitude";
    EXPECT_NE(body.find("camR - planetRadius <= surfaceTolerance"), std::string::npos)
        << "the analytic branch must be selected by altitude, not by the ray-sphere result";

    // The tolerance must be RELATIVE to the planet radius. A fixed metre band is the
    // driver-precision hazard: camR arrives through a length(), SPIR-V permits 3 ULP in
    // sqrt, and one ULP at R = 6.36e6 is ~0.5 m -- so a 1 m band is ~2 ULP and a
    // conformant driver may miss it while the camera is exactly on the surface, silently
    // restoring the degenerate intersection this branch exists to avoid.
    EXPECT_NE(body.find("planetRadius * kGroundFloorSurfaceToleranceRelative"), std::string::npos)
        << "the tolerance must scale with the planet radius, not be a fixed distance";

    // The degenerate branch must not consult the intersection at all: that is the whole
    // point -- on the sphere its answer is 0-or-noise.
    const std::size_t analytic = body.find("kGroundFloorSurfaceToleranceRelative");
    const std::size_t elseSite = body.find("else", analytic);
    ASSERT_NE(elseSite, std::string::npos);
    const std::string analyticBranch = body.substr(analytic, elseSite - analytic);
    EXPECT_EQ(CountOccurrences(analyticBranch, "GE_RaySphereIntersect"), 0u)
        << "the at-surface branch must NOT use the ray-sphere solve -- it is degenerate there";
    EXPECT_NE(analyticBranch.find("distToFloor = 0.0"), std::string::npos)
        << "on the surface the ground is at distance 0, which also makes the haze term inert";
}

// The skylight reaching the floor is an irradiance, and the only thing standing between
// it and the outgoing radiance is the albedo. Concretely: E_sky = PI * Lbar and the
// Lambertian BRDF is p/PI, so the term is p * Lbar exactly. The shipped form multiplied a
// ZENITH sample by 0.30, which is wrong twice over -- wrong constant, and a sample the
// cosine weighting does not favour -- and both errors are invisible in a compile.
TEST(SkyGroundBounceContract, SkylightTermIsAHemisphericalMeanWithNoTuningFraction)
{
    const std::string composite = LoadComposite();
    ASSERT_FALSE(composite.empty()) << "sky_composite.glsl not found via GE_RENDERER_REPO_ROOT";

    EXPECT_EQ(CountOccurrences(composite, "vec3 GE_SkyDiffuseMeanRadiance("), 1u)
        << "the hemispherical mean must be defined exactly once, in the shared include";
    EXPECT_EQ(CountOccurrences(composite, "GE_GROUND_SKY_AMBIENT_FRACTION"), 0u)
        << "the skylight term must carry no tuning fraction: under a uniform sky of radiance "
           "L a Lambertian floor of albedo p leaves exactly p*L, so a constant in front of it "
           "is an energy error, not a dial";

    const std::size_t begin = composite.find("vec3 GE_GroundBounceRadiance(");
    ASSERT_NE(begin, std::string::npos);
    const std::size_t end = composite.find("\n}", begin);
    ASSERT_NE(end, std::string::npos);
    const std::string body = composite.substr(begin, end - begin);

    // Pin the WHOLE statement, not a substring of it: `albedo * mean * 0.30` contains
    // `albedo * mean`, so a substring check is satisfied by the exact defect this test
    // exists to catch. Compare the assigned expression with whitespace removed.
    const std::size_t assign = body.find("ambient =");
    ASSERT_NE(assign, std::string::npos) << "no ambient term found in the bounce";
    const std::size_t semi = body.find(';', assign);
    ASSERT_NE(semi, std::string::npos);
    std::string rhs = body.substr(assign + std::string("ambient =").size(),
                                  semi - assign - std::string("ambient =").size());
    rhs.erase(std::remove_if(rhs.begin(), rhs.end(),
                             [](unsigned char c) { return std::isspace(c) != 0; }),
              rhs.end());
    EXPECT_EQ(rhs, "groundAlbedo*skyDiffuseMeanRadiance")
        << "the skylight term must be exactly albedo x the cosine-weighted mean radiance. "
           "Any extra factor is an energy error: under a uniform sky of radiance L a "
           "Lambertian floor of albedo p leaves p*L, full stop. The shipped form was "
           "p * L_zenith * 0.30.";

    // Both consumers must reach the mean through the shared function rather than taking
    // their own sample: a private tap is the same drift GroundBounceMathHasASingleImplementation
    // guards against, one level down.
    const std::string render = LoadSkyRender();
    const std::string capture = LoadSkyCapture();
    for (const auto& [name, consumer] :
         {std::pair<const char*, const std::string&>{"sky_render.frag", render},
          std::pair<const char*, const std::string&>{"sky_capture_cube.frag", capture}})
    {
        ASSERT_FALSE(consumer.empty());
        EXPECT_EQ(CountOccurrences(consumer, "GE_SkyDiffuseMeanRadiance("), 1u)
            << name << " must take the skylight estimate from the shared implementation";
        EXPECT_EQ(CountOccurrences(consumer, "GE_SkyDirToViewLutUv(vec3(0.0, 1.0, 0.0))"), 0u)
            << name << " still samples the zenith for the ground bounce -- the zenith carries "
                       "no more cosine weight than any other single direction, and at a grazing "
                       "sun it is the coldest part of the sky";
    }
}

// The estimator itself, replicated here and checked against the integral it claims to
// compute. This is the test that would have caught the shipped 0.30: it fails for ANY
// constant factor, and it fails for a single-direction sample in either direction.
//
// Mirrors GE_SkyDiffuseMeanRadiance in sky_composite.glsl -- the tap count is asserted
// against the shader source below so the two cannot drift apart silently.
//
// The guarantee is stated per axis, because it is not the same on both. The taps are
// stratified in the ZENITH angle and only golden-angle-spaced in AZIMUTH, so an
// azimuthally-symmetric sky is integrated tightly while a narrow azimuthal lobe -- a low
// sun's bright quadrant -- is not. Both bars are pinned here rather than the tight one
// being quoted for the whole sampler.
TEST(SkyGroundBounceContract, SkylightEstimatorIntegratesTheCosineWeightedHemisphere)
{
    constexpr int kTaps = 16;
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kGoldenAngle = 2.39996323;
    const std::string composite = LoadComposite();
    ASSERT_FALSE(composite.empty());
    EXPECT_NE(composite.find("GE_SKY_AMBIENT_SAMPLE_COUNT 16"), std::string::npos)
        << "the shader's tap count changed; this test mirrors it and must change together";

    // The shader's taps: u stratified over [0,1), sin(theta) = sqrt(u) (which inverts the
    // cosine density's CDF, sin^2 theta), azimuth stepped by the golden angle. Every tap
    // therefore carries weight 1/N and the estimate is the plain mean.
    auto estimate = [&](auto&& radiance) {
        double sum = 0.0;
        for (int i = 0; i < kTaps; ++i)
        {
            const double u = (static_cast<double>(i) + 0.5) / kTaps;
            const double theta = std::asin(std::sqrt(std::min(u, 1.0)));
            const double phi = std::fmod(static_cast<double>(i) * kGoldenAngle, 2.0 * kPi);
            sum += radiance(theta, phi);
        }
        return sum / kTaps;
    };
    // Lbar = (1/PI) * integral of L cos(theta) dw over the upper hemisphere.
    auto reference = [&](auto&& radiance) {
        constexpr int kT = 2000, kP = 4000;
        const double dt = (kPi / 2.0) / kT;
        const double dp = (2.0 * kPi) / kP;
        double sum = 0.0;
        for (int a = 0; a < kT; ++a)
        {
            const double th = (a + 0.5) * dt;
            const double w = std::cos(th) * std::sin(th) * dt * dp;
            for (int b = 0; b < kP; ++b)
                sum += radiance(th, (b + 0.5) * dp) * w;
        }
        return sum / kPi;
    };

    struct Profile
    {
        const char* name;
        double (*radiance)(double, double);
        double tolerance;
    };
    const Profile profiles[] = {
        // Azimuthally symmetric: the axis the taps are stratified in.
        {"uniform", [](double, double) { return 1.0; }, 0.02},
        {"zenith-bright", [](double t, double) { return std::cos(t); }, 0.02},
        {"horizon-bright", [](double t, double) { return std::sin(t); }, 0.02},
        {"strong horizon band",
         [](double t, double) { return 1.0 + 3.0 * std::pow(std::sin(t), 4.0); }, 0.02},
        {"low sun", [](double t, double) { return std::exp(-3.0 * std::cos(t)); }, 0.02},
        // Azimuthally peaked: only golden-angle coverage, so the bar is looser BY
        // MEASUREMENT, not by hope -- a narrow lobe lands around +7% at N = 16.
        {"narrow azimuthal lobe",
         [](double, double p) { return 1.0 + 3.0 * std::pow(std::max(std::cos(p), 0.0), 8.0); },
         0.10},
        {"low sun with an azimuthal lobe",
         [](double t, double p) {
             return std::exp(-3.0 * std::cos(t)) *
                    (1.0 + 4.0 * std::pow(std::max(std::cos(p), 0.0), 6.0));
         },
         0.10},
    };

    for (const Profile& p : profiles)
    {
        const double got = estimate(p.radiance);
        const double want = reference(p.radiance);
        EXPECT_NEAR(got, want, p.tolerance * want)
            << "the tap set does not integrate the cosine-weighted hemisphere for a "
            << p.name << " sky (got " << got << ", want " << want << ")";
    }

    // A uniform sky is the case a constant-factor error hides in: the estimator must
    // return the radiance itself, so albedo x it is the textbook p*L.
    EXPECT_NEAR(estimate([](double, double) { return 1.0; }), 1.0, 1e-12);

    // Red arm: the two single-direction shortcuts this replaced must FAIL the same bar on
    // the low-sun sky, or the test above proves nothing about which sample is right.
    const auto& lowSun = profiles[4];
    const double want = reference(lowSun.radiance);
    const double zenithOnly = lowSun.radiance(0.0, 0.0);
    const double horizonOnly = lowSun.radiance(kPi / 2.0, 0.0);
    EXPECT_GT(std::abs(zenithOnly - want), 0.02 * want)
        << "a zenith-only sample would pass this bar, so the bar is not testing anything";
    EXPECT_GT(std::abs(horizonOnly - want), 0.02 * want)
        << "a horizon-only sample would pass this bar, so the bar is not testing anything";
}

// At night the physical bounce has nothing to reflect -- the sun is below the horizon so
// the N.L clamp zeroes the direct term, and the sky-view LUT the skylight integrates is
// near-black -- so an unguarded floor goes black. That is wrong for a mechanical reason
// rather than a taste one: the LUT is NOT the sky this engine renders at night. The night
// gradient is composited ON TOP of it by GE_ApplySkyGroundAndNight, so a floor derived
// from the LUT alone reflects a sky nobody sees, which is the exact capture-vs-visible
// divergence this whole path exists to remove.
//
// Measured, TimeOfDayHours 23 (physamb2, 2026-08-17): unguarded, a vertical surface loses
// 72% of its ambient and a down-facing one 98%. Guarded, the floor lands on the authored
// GroundNightColor and the down-facing face reads within 9% of that authored value.
TEST(SkyGroundBounceContract, BounceCrossesToTheAuthoredNightGroundColour)
{
    const std::string composite = LoadComposite();
    ASSERT_FALSE(composite.empty()) << "sky_composite.glsl not found via GE_RENDERER_REPO_ROOT";

    const std::size_t begin = composite.find("vec3 GE_GroundBounceRadiance(");
    ASSERT_NE(begin, std::string::npos);
    const std::size_t end = composite.find("\n}", begin);
    ASSERT_NE(end, std::string::npos);
    const std::string body = composite.substr(begin, end - begin);

    EXPECT_NE(body.find("groundNightColor"), std::string::npos)
        << "the bounce must reach the authored night ground colour; without it a night "
           "floor is black and the IBL reflects a sky the engine never renders";
    EXPECT_NE(body.find("mix(lit, groundNightColor, clamp(nightAmount, 0.0, 1.0))"),
              std::string::npos)
        << "the night crossover must be the same mix, on the same clamped nightAmount, that "
           "GE_ApplySkyGroundAndNight already uses for the stylized ground -- one night "
           "semantics, not two";

    // Both consumers must feed it, or the capture and the visible sky disagree at night.
    const std::string render = LoadSkyRender();
    const std::string capture = LoadSkyCapture();
    for (const auto& [name, consumer] :
         {std::pair<const char*, const std::string&>{"sky_render.frag", render},
          std::pair<const char*, const std::string&>{"sky_capture_cube.frag", capture}})
    {
        ASSERT_FALSE(consumer.empty());
        EXPECT_NE(consumer.find("groundNightColor, nightAmount"), std::string::npos)
            << name << " does not pass the night ground colour and amount to the floor "
                       "composite, so its night floor stays black";
    }
}

// The above-surface path is the visible sky's, and it must keep the intersection math:
// mis-gating the analytic branch to it would paint a floor for rays that miss the planet
// entirely (looking up, or from altitude past the horizon).
//
// Kept deliberately: the branch commit's own message flagged this test as redundant
// against one mutation, but review found two it is the only test to catch (mis-gating the
// analytic branch to the above-surface path, and dropping the miss guard).
TEST(SkyGroundBounceContract, AboveSurfacePathKeepsTheIntersectionAndItsMissGuard)
{
    const std::string composite = LoadComposite();
    ASSERT_FALSE(composite.empty());
    const std::size_t begin = composite.find("vec3 GE_CompositeGroundFloor(");
    ASSERT_NE(begin, std::string::npos);
    const std::string body = composite.substr(begin);

    EXPECT_EQ(CountOccurrences(body, "GE_RaySphereIntersect"), 1u)
        << "the above-surface path must still resolve a real hit distance";
    EXPECT_NE(body.find("|| tg0 <= 0.0"), std::string::npos)
        << "the above-surface path must still reject rays that miss the planet or hit behind";
}
