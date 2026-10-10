// The relief march of Shaders/Includes/parallax_occlusion.glsl, lifted verbatim out of the shader
// at build time (ExtractShaderBlock.cmake) and compiled as C++ through GlslShim.h, so these tests
// execute the shipped march rather than a copy of it. The block is compiled twice: once as the
// desktop arm and once with GE_COMPAT_PROFILE defined, the fixed low arm WebGPU-class devices take.
// The steps view's colour (Shaders/Includes/parallax_steps_view.glsl) and the relief's depth
// (Shaders/Includes/parallax_depth.glsl) are lifted the same way.
//
// Every case runs on a flat surface whose height field is analytic, so each answer has an exact
// oracle: the hit of a ray against a ramp, a step, a one-texel ridge; the step count the pixel
// rule demands; the light a hit receives past the relief; the samples the steps view reports.

#include <gtest/gtest.h>

#include "GlslShim.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <random>
#include <utility>
#include <vector>

namespace
{
// The field the included march samples, as a function of the height-space UV. Installed per test.
std::function<float(float, float)> s_HeightField;
// Height samples taken since a test last reset it: the oracle for the march's own count.
int s_HeightFetchCount = 0;
// The lowest and highest LOD a height sample was taken at since a test last reset them.
float s_LowestSampledLod = 0.0f;
float s_HighestSampledLod = 0.0f;
// Where the height samples were taken, in order, while a test records them.
bool s_RecordingSamples = false;
std::vector<std::pair<float, float>> s_SampledPoints;

float SampleHeightField(float u, float v, float lod)
{
    ++s_HeightFetchCount;
    s_LowestSampledLod = std::min(s_LowestSampledLod, lod);
    s_HighestSampledLod = std::max(s_HighestSampledLod, lod);
    if (s_RecordingSamples)
        s_SampledPoints.emplace_back(u, v);
    return s_HeightField(u, v);
}

void ResetSampledLods()
{
    s_LowestSampledLod = std::numeric_limits<float>::infinity();
    s_HighestSampledLod = -std::numeric_limits<float>::infinity();
}
} // namespace

namespace GameEngine::GlslShim::ParallaxDesktop
{
inline float GE_ParallaxSampleHeight(vec2 uvHeight, float lod)
{
    return ::SampleHeightField(uvHeight.x, uvHeight.y, lod);
}
#include "ParallaxOcclusionExtracted.h"
} // namespace GameEngine::GlslShim::ParallaxDesktop

namespace GameEngine::GlslShim::ParallaxCompat
{
inline float GE_ParallaxSampleHeight(vec2 uvHeight, float lod)
{
    return ::SampleHeightField(uvHeight.x, uvHeight.y, lod);
}
#define GE_COMPAT_PROFILE
#include "ParallaxOcclusionExtracted.h"
#undef GE_COMPAT_PROFILE
} // namespace GameEngine::GlslShim::ParallaxCompat

namespace GameEngine::GlslShim::ParallaxStepsView
{
#include "ParallaxStepsViewExtracted.h"
} // namespace GameEngine::GlslShim::ParallaxStepsView

namespace GameEngine::GlslShim::ParallaxDepth
{
// GLSL's `precise` forbids reassociation; C++ evaluates as written.
#define precise
#include "ParallaxDepthExtracted.h"
#undef precise
} // namespace GameEngine::GlslShim::ParallaxDepth

// The desktop march with the relief's depth compiled in, as the prepass and the colour pass that reads
// the prepass's depth have it.
namespace GameEngine::GlslShim::ParallaxReliefDepth
{
inline float GE_ParallaxSampleHeight(vec2 uvHeight, float lod)
{
    return ::SampleHeightField(uvHeight.x, uvHeight.y, lod);
}
#define GE_PARALLAX_RELIEF_DEPTH
#define precise
#include "ParallaxOcclusionExtracted.h"
#include "ParallaxDepthExtracted.h"
#undef precise
#undef GE_PARALLAX_RELIEF_DEPTH
} // namespace GameEngine::GlslShim::ParallaxReliefDepth

namespace
{
namespace Glsl = GameEngine::GlslShim;
namespace Desktop = GameEngine::GlslShim::ParallaxDesktop;
namespace Compat = GameEngine::GlslShim::ParallaxCompat;
namespace StepsView = GameEngine::GlslShim::ParallaxStepsView;
namespace Depth = GameEngine::GlslShim::ParallaxDepth;
namespace ReliefDepth = GameEngine::GlslShim::ParallaxReliefDepth;
using Glsl::vec2;
using Glsl::vec3;
using Glsl::vec4;

// A flat surface through the origin with normal +Z. uv0 is world (x, y) divided by the metres per
// repeat along each axis, and one pixel covers pixelMetres of it along x and along y. The eye sits
// viewAngle off the normal toward the azimuth, so the view ray descends away from it.
struct PlaneView
{
    float ViewAngle = 0.7f;
    float Azimuth = 0.0f;
    float PixelMetres = 0.01f;
    float MetresPerRepeatU = 1.0f;
    float MetresPerRepeatV = 1.0f;
    float ReliefDepth = 0.05f;
    vec4 HeightRow0 = vec4(1.0f, 0.0f, 0.0f, 0.0f);
    vec4 HeightRow1 = vec4(0.0f, 1.0f, 0.0f, 0.0f);
    vec3 Tangent = vec3(0.0f, 0.0f, 0.0f);

    vec3 ViewDirection() const
    {
        return vec3(std::sin(ViewAngle) * std::cos(Azimuth), std::sin(ViewAngle) * std::sin(Azimuth),
                    std::cos(ViewAngle));
    }

    // The pixel size whose full-depth shift is `pixels` long on screen, for isotropic metres per
    // repeat and an identity height transform: E = tan(angle) * reliefDepth * metresPerRepeat / pixel.
    PlaneView WithShift(float pixels) const
    {
        PlaneView view = *this;
        view.PixelMetres = std::tan(ViewAngle) * ReliefDepth * MetresPerRepeatU / pixels;
        return view;
    }
};

// The height map size every self-shadow case passes, texels.
constexpr float kHeightTexels = 1024.0f;

// The two arms compile the same source into two namespaces; this binds a test to either.
struct DesktopArm
{
    using Frame = Desktop::GE_ParallaxFrame;
    using Ray = Desktop::GE_ParallaxRay;
    using Hit = Desktop::GE_ParallaxHit;
    static constexpr int kLinearBudget = Desktop::kParallaxLinearStepBudget;
    static Frame BuildFrame(vec3 n, vec3 t, vec3 px, vec3 py, vec2 ux, vec2 uy)
    {
        return Desktop::GE_ParallaxBuildFrame(n, t, px, py, ux, uy);
    }
    static Ray SetupRay(const Frame& f, vec3 v, float depth, vec4 r0, vec4 r1)
    {
        return Desktop::GE_ParallaxSetupRay(f, v, depth, r0, r1);
    }
    static Hit March(vec2 uv, const Ray& ray) { return Desktop::GE_ParallaxMarch(uv, ray, 0.0f); }
    static float SelfShadow(const Frame& f, const Ray& ray, const Hit& hit, vec2 uv, vec3 light, vec4 r0, vec4 r1)
    {
        return Desktop::GE_ParallaxSelfShadow(f, ray, hit, uv, light, r0, r1, vec2(kHeightTexels, kHeightTexels),
                                              0.0f)
            .light;
    }
};

struct ReliefDepthArm
{
    using Frame = ReliefDepth::GE_ParallaxFrame;
    using Ray = ReliefDepth::GE_ParallaxRay;
    using Hit = ReliefDepth::GE_ParallaxHit;
    static constexpr int kLinearBudget = ReliefDepth::kParallaxLinearStepBudget;
    static Frame BuildFrame(vec3 n, vec3 t, vec3 px, vec3 py, vec2 ux, vec2 uy)
    {
        return ReliefDepth::GE_ParallaxBuildFrame(n, t, px, py, ux, uy);
    }
    static Ray SetupRay(const Frame& f, vec3 v, float depth, vec4 r0, vec4 r1)
    {
        return ReliefDepth::GE_ParallaxSetupRay(f, v, depth, r0, r1);
    }
    static Hit March(vec2 uv, const Ray& ray) { return ReliefDepth::GE_ParallaxMarch(uv, ray, 0.0f); }
};

struct CompatArm
{
    using Frame = Compat::GE_ParallaxFrame;
    using Ray = Compat::GE_ParallaxRay;
    using Hit = Compat::GE_ParallaxHit;
    static constexpr int kLinearBudget = Compat::kParallaxLinearStepBudget;
    static Frame BuildFrame(vec3 n, vec3 t, vec3 px, vec3 py, vec2 ux, vec2 uy)
    {
        return Compat::GE_ParallaxBuildFrame(n, t, px, py, ux, uy);
    }
    static Ray SetupRay(const Frame& f, vec3 v, float depth, vec4 r0, vec4 r1)
    {
        return Compat::GE_ParallaxSetupRay(f, v, depth, r0, r1);
    }
    static Hit March(vec2 uv, const Ray& ray) { return Compat::GE_ParallaxMarch(uv, ray, 0.0f); }
    static float SelfShadow(const Frame& f, const Ray& ray, const Hit& hit, vec2 uv, vec3 light, vec4 r0, vec4 r1)
    {
        return Compat::GE_ParallaxSelfShadow(f, ray, hit, uv, light, r0, r1, vec2(kHeightTexels, kHeightTexels),
                                             0.0f)
            .light;
    }
};

template <class Arm>
struct Marched
{
    typename Arm::Frame Frame;
    typename Arm::Ray Ray;
    typename Arm::Hit Hit;
};

template <class Arm>
typename Arm::Frame BuildPlaneFrame(const PlaneView& view)
{
    return Arm::BuildFrame(vec3(0.0f, 0.0f, 1.0f), view.Tangent, vec3(view.PixelMetres, 0.0f, 0.0f),
                           vec3(0.0f, view.PixelMetres, 0.0f), vec2(view.PixelMetres / view.MetresPerRepeatU, 0.0f),
                           vec2(0.0f, view.PixelMetres / view.MetresPerRepeatV));
}

template <class Arm>
typename Arm::Ray SetupPlaneRay(const PlaneView& view)
{
    return Arm::SetupRay(BuildPlaneFrame<Arm>(view), view.ViewDirection(), view.ReliefDepth, view.HeightRow0,
                         view.HeightRow1);
}

vec2 HeightUv(const PlaneView& view, vec2 uv0)
{
    return vec2(view.HeightRow0.x * uv0.x + view.HeightRow0.y * uv0.y + view.HeightRow0.z,
                view.HeightRow1.x * uv0.x + view.HeightRow1.y * uv0.y + view.HeightRow1.z);
}

// Marches from uv0 with `field` given in terms of the march parameter s along the ray's path
// (0 at the undisplaced point, 1 where the ray reaches the full depth): the only coordinate the
// march can see, so an oracle written in s is exact whatever the path's direction.
template <class Arm>
Marched<Arm> MarchAlongPath(const PlaneView& view, vec2 uv0, const std::function<float(float)>& field)
{
    Marched<Arm> m{BuildPlaneFrame<Arm>(view), {}, {}};
    m.Ray = Arm::SetupRay(m.Frame, view.ViewDirection(), view.ReliefDepth, view.HeightRow0, view.HeightRow1);
    const vec2 start = HeightUv(view, uv0);
    const vec2 end = m.Ray.heightUvEnd;
    const float endLengthSquared = Glsl::dot(end, end);
    s_HeightField = [=](float u, float v)
    {
        const float s = endLengthSquared > 0.0f
            ? ((u - start.x) * end.x + (v - start.y) * end.y) / endLengthSquared
            : 0.0f;
        return field(s);
    };
    m.Hit = Arm::March(start, m.Ray);
    return m;
}

// A shift that the pixel rule turns into exactly eight linear steps, clear of the ceil boundary.
constexpr float kEightStepShift = 7.9f;

// The ray height at path parameter s is 1 - s, so a ramp field f(s) = base + slope * s meets it
// at s = (1 - base) / (1 + slope).
float RampHit(float base, float slope) { return (1.0f - base) / (1.0f + slope); }

// ---- The pixel rule --------------------------------------------------------------------------

TEST(ParallaxOcclusion, BelowHalfAPixelTheMarchIsTheIdentity)
{
    const PlaneView view = PlaneView{}.WithShift(0.49f);
    s_HeightFetchCount = 0;
    const auto marched = MarchAlongPath<DesktopArm>(view, vec2(0.3f, 0.2f), [](float) { return 0.0f; });
    EXPECT_NEAR(marched.Ray.shiftPixels, 0.49f, 1.0e-4f);
    EXPECT_EQ(marched.Ray.linearSteps, 0);
    EXPECT_EQ(s_HeightFetchCount, 0) << "a skipped march samples nothing";
    EXPECT_EQ(marched.Hit.fetches, 0) << "and reports that it sampled nothing";
    EXPECT_EQ(marched.Hit.surfaceUvOffset.x, 0.0f);
    EXPECT_EQ(marched.Hit.surfaceUvOffset.y, 0.0f);
    EXPECT_EQ(marched.Hit.rayDepth, 0.0f);
    EXPECT_NEAR(marched.Ray.reliefMetres, 0.05f, 1.0e-6f) << "the self-shadow still sees the full relief depth";
}

TEST(ParallaxOcclusion, TheReliefFadesInAcrossTheFirstPixel)
{
    // Continuous at the skip threshold: the offset grows from zero rather than jumping to half a
    // pixel, and the full relief depth (0.05 of a 1 m repeat) is reached by 1.5 px.
    const float fullReliefMetres = 0.05f;
    EXPECT_NEAR(Glsl::length(SetupPlaneRay<DesktopArm>(PlaneView{}.WithShift(0.5f)).surfaceUvEnd), 0.0f, 1.0e-7f);
    float previousEndLength = 0.0f;
    for (const float shift : {0.6f, 0.9f, 1.2f, 1.5f})
    {
        const auto ray = SetupPlaneRay<DesktopArm>(PlaneView{}.WithShift(shift));
        const float endLength = Glsl::length(ray.surfaceUvEnd);
        EXPECT_GT(endLength, previousEndLength) << "shift " << shift;
        previousEndLength = endLength;
    }
    for (const float shift : {1.5f, 2.0f, 9.0f})
        EXPECT_NEAR(SetupPlaneRay<DesktopArm>(PlaneView{}.WithShift(shift)).reliefMetres, fullReliefMetres, 1.0e-6f)
            << "shift " << shift;
}

template <class Arm>
void ExpectStepCountIsTheBudgetCappedCeiling()
{
    for (const float shift : {0.51f, 1.0f, 1.01f, 7.3f, 8.0f, 11.5f, 12.2f, 31.5f, 32.0f, 32.2f, 400.0f})
    {
        const auto ray = SetupPlaneRay<Arm>(PlaneView{}.WithShift(shift));
        EXPECT_NEAR(ray.shiftPixels, shift, shift * 1.0e-4f);
        const int expected = std::min(Arm::kLinearBudget, static_cast<int>(std::ceil(ray.shiftPixels)));
        EXPECT_EQ(ray.linearSteps, expected) << "shift " << shift;
    }
}

TEST(ParallaxOcclusion, StepCountIsTheBudgetCappedCeilingOfTheShift)
{
    EXPECT_EQ(DesktopArm::kLinearBudget, 32);
    ExpectStepCountIsTheBudgetCappedCeiling<DesktopArm>();
}

TEST(ParallaxOcclusion, CompatArmCapsTheStepsAtTwelve)
{
    EXPECT_EQ(CompatArm::kLinearBudget, 12);
    ExpectStepCountIsTheBudgetCappedCeiling<CompatArm>();
}

TEST(ParallaxOcclusion, ShiftMatchesTheAnalyticProjection)
{
    // h tan(theta) across the plane, measured in pixels: the pixel rule's own definition.
    for (const float angle : {0.2f, 0.7f, 1.2f})
    {
        PlaneView view{};
        view.ViewAngle = angle;
        view.PixelMetres = 0.002f;
        const auto ray = SetupPlaneRay<DesktopArm>(view);
        const float expected = std::tan(angle) * view.ReliefDepth * view.MetresPerRepeatU / view.PixelMetres;
        EXPECT_NEAR(ray.shiftPixels, expected, expected * 1.0e-5f) << "angle " << angle;
    }
}

// ---- The hit ---------------------------------------------------------------------------------

TEST(ParallaxOcclusion, RampHitIsWithinOneOverNSquaredAtEightSteps)
{
    struct Ramp
    {
        float Base;
        float Slope;
    };
    const PlaneView view = PlaneView{}.WithShift(kEightStepShift);
    for (const Ramp ramp : {Ramp{0.5f, 0.5f}, Ramp{0.2f, 0.3f}, Ramp{0.7f, -0.4f}, Ramp{0.0f, 0.0f}, Ramp{0.9f, 2.0f}})
    {
        const auto marched = MarchAlongPath<DesktopArm>(view, vec2(0.3f, 0.2f),
                                                        [=](float s) { return ramp.Base + ramp.Slope * s; });
        ASSERT_EQ(marched.Ray.linearSteps, 8);
        EXPECT_NEAR(marched.Hit.rayDepth, RampHit(ramp.Base, ramp.Slope), 1.0f / 64.0f) << ramp.Base << ", " << ramp.Slope;
    }
}

TEST(ParallaxOcclusion, StepHitIsWithinOneOverNSquaredAtEightSteps)
{
    // A floor at 0.2 and, from s0 on, a wall up to 0.95. The ray meets the wall at s0 when it is
    // still above the floor there; otherwise it lands on the floor at s = 0.8.
    const PlaneView view = PlaneView{}.WithShift(kEightStepShift);
    constexpr float kFloor = 0.2f;
    constexpr float kWallTop = 0.95f;
    float worst = 0.0f;
    for (float wall = 0.06f; wall < 0.95f; wall += 0.0137f)
    {
        const auto marched = MarchAlongPath<DesktopArm>(view, vec2(0.3f, 0.2f),
                                                        [=](float s) { return s < wall ? kFloor : kWallTop; });
        const float expected = wall <= 1.0f - kFloor ? wall : 1.0f - kFloor;
        worst = std::max(worst, std::abs(marched.Hit.rayDepth - expected));
    }
    EXPECT_LE(worst, 1.0f / 64.0f);
}

namespace
{
// A floor at 0.1 with a ridge of 0.9 one texel wide, on a grid of 50 texels along the path. The
// ridge sits inside the linear step that brackets the floor crossing (s = 0.9), so the linear
// pass steps between it and lands on the floor.
constexpr float kRidgeTexels = 50.0f;
constexpr int kRidgeTexel = 44; // s in [0.88, 0.90)
float RidgeField(float s)
{
    const int texel = static_cast<int>(std::floor(s * kRidgeTexels));
    return texel == kRidgeTexel ? 0.9f : 0.1f;
}
constexpr float kRidgeHit = kRidgeTexel / kRidgeTexels;
} // namespace

TEST(ParallaxOcclusion, RefinementCatchesAOneTexelRidgeTheLinearPassSteppedOver)
{
    const PlaneView view = PlaneView{}.WithShift(kEightStepShift);
    const auto marched = MarchAlongPath<DesktopArm>(view, vec2(0.3f, 0.2f), RidgeField);
    ASSERT_EQ(marched.Ray.linearSteps, 8);
    EXPECT_NEAR(marched.Hit.rayDepth, kRidgeHit, 1.0f / 64.0f);
}

TEST(ParallaxOcclusion, PositiveControlTheLinearPassAloneMissesTheRidge)
{
    // The compat arm has no refinement: the same eight linear steps interpolate between two floor
    // samples and report the floor crossing, beyond the ridge.
    const PlaneView view = PlaneView{}.WithShift(kEightStepShift);
    const auto marched = MarchAlongPath<CompatArm>(view, vec2(0.3f, 0.2f), RidgeField);
    ASSERT_EQ(marched.Ray.linearSteps, 8);
    EXPECT_GT(std::abs(marched.Hit.rayDepth - kRidgeHit), 1.0f / 64.0f);
    EXPECT_NEAR(marched.Hit.rayDepth, 0.9f, 1.0e-5f);
}

TEST(ParallaxOcclusion, PitFloorIsFoundAtEveryStepCount)
{
    // Height exactly 0 (a mortar line, a pit): the ray meets the field only at the full depth, so
    // the last linear sample must sit at depth 1 exactly, whatever the rounding of 1/n.
    for (int steps = 1; steps <= DesktopArm::kLinearBudget; ++steps)
    {
        const PlaneView view = PlaneView{}.WithShift(static_cast<float>(steps) - 0.5f);
        const auto marched = MarchAlongPath<DesktopArm>(view, vec2(0.3f, 0.2f), [](float) { return 0.0f; });
        ASSERT_EQ(marched.Ray.linearSteps, steps);
        EXPECT_NEAR(marched.Hit.rayDepth, 1.0f, 1.0e-5f) << "steps " << steps;
        EXPECT_NEAR(marched.Hit.surfaceUvOffset.x, marched.Ray.surfaceUvEnd.x, 1.0e-6f) << "steps " << steps;
    }
}

TEST(ParallaxOcclusion, UndisplacedPointOnTheTopSurfaceStaysPut)
{
    const auto marched =
        MarchAlongPath<DesktopArm>(PlaneView{}.WithShift(kEightStepShift), vec2(0.3f, 0.2f), [](float) { return 1.0f; });
    EXPECT_EQ(marched.Hit.rayDepth, 0.0f);
    EXPECT_EQ(marched.Hit.surfaceUvOffset.x, 0.0f);
    EXPECT_EQ(marched.Hit.surfaceUvOffset.y, 0.0f);
}

TEST(ParallaxOcclusion, TheRayDescendsAwayFromTheEye)
{
    // Eye toward +x: the ray, and so the offset of every map, runs toward -x.
    const auto marched =
        MarchAlongPath<DesktopArm>(PlaneView{}.WithShift(kEightStepShift), vec2(0.3f, 0.2f), [](float) { return 0.0f; });
    EXPECT_LT(marched.Hit.surfaceUvOffset.x, 0.0f);
    EXPECT_NEAR(marched.Hit.surfaceUvOffset.y, 0.0f, 1.0e-7f);
    // Hitting the bottom of the relief shifts by the full depth times tan(angle), in repeats.
    EXPECT_NEAR(marched.Hit.surfaceUvOffset.x, -std::tan(0.7f) * 0.05f, 1.0e-5f);
}

// ---- Tiling and parametrization --------------------------------------------------------------

namespace
{
// A smooth field defined on the SURFACE (uv0), so that every tiling of the height slot that keeps
// the relief's metric depth must find the same surface point.
constexpr float kWorldRampBase = 0.15f;
float WorldRamp(vec2 uv0) { return kWorldRampBase + 0.6f * (0.8f * uv0.x - 0.6f * uv0.y); }

template <class Arm>
typename Arm::Hit MarchWorldRamp(const PlaneView& view, vec2 uv0)
{
    // The height map as tiled: field(uvH) = WorldRamp(T^-1 (uvH - offset)).
    const float a = view.HeightRow0.x, b = view.HeightRow0.y, c = view.HeightRow1.x, d = view.HeightRow1.y;
    const float det = a * d - b * c;
    const float ox = view.HeightRow0.z, oy = view.HeightRow1.z;
    s_HeightField = [=](float u, float v)
    {
        const float x = u - ox, y = v - oy;
        return WorldRamp(vec2((d * x - b * y) / det, (-c * x + a * y) / det));
    };
    return Arm::March(HeightUv(view, uv0), SetupPlaneRay<Arm>(view));
}

PlaneView RotatedTiling(PlaneView view, float angle)
{
    view.HeightRow0 = vec4(std::cos(angle), -std::sin(angle), 0.25f, 0.0f);
    view.HeightRow1 = vec4(std::sin(angle), std::cos(angle), -0.5f, 0.0f);
    return view;
}
} // namespace

TEST(ParallaxOcclusion, RotatedTilingMatchesTheIsotropicResultRotated)
{
    PlaneView isotropic = PlaneView{}.WithShift(10.0f);
    isotropic.Azimuth = 0.9f;
    const auto reference = MarchWorldRamp<DesktopArm>(isotropic, vec2(0.3f, 0.2f));
    ASSERT_GT(reference.rayDepth, 0.0f);
    for (const float angle : {0.5f, std::numbers::pi_v<float> * 0.5f, 2.4f})
    {
        const PlaneView rotated = RotatedTiling(isotropic, angle);
        const auto hit = MarchWorldRamp<DesktopArm>(rotated, vec2(0.3f, 0.2f));
        EXPECT_NEAR(hit.rayDepth, reference.rayDepth, 1.0e-4f) << "angle " << angle;
        EXPECT_NEAR(hit.surfaceUvOffset.x, reference.surfaceUvOffset.x, 1.0e-5f) << "angle " << angle;
        EXPECT_NEAR(hit.surfaceUvOffset.y, reference.surfaceUvOffset.y, 1.0e-5f) << "angle " << angle;
        // In height space the same offset appears rotated by the tiling.
        const vec2 r = reference.heightUvOffset;
        EXPECT_NEAR(hit.heightUvOffset.x, std::cos(angle) * r.x - std::sin(angle) * r.y, 1.0e-5f) << angle;
        EXPECT_NEAR(hit.heightUvOffset.y, std::sin(angle) * r.x + std::cos(angle) * r.y, 1.0e-5f) << angle;
    }
}

TEST(ParallaxOcclusion, AnisotropicTilingOfEqualAreaFindsTheSameSurfacePoint)
{
    PlaneView isotropic = PlaneView{}.WithShift(10.0f);
    isotropic.Azimuth = 2.2f;
    const auto reference = MarchWorldRamp<DesktopArm>(isotropic, vec2(0.3f, 0.2f));
    PlaneView stretched = isotropic;
    stretched.HeightRow0 = vec4(2.0f, 0.0f, 0.0f, 0.0f);
    stretched.HeightRow1 = vec4(0.0f, 0.5f, 0.0f, 0.0f);
    const auto hit = MarchWorldRamp<DesktopArm>(stretched, vec2(0.3f, 0.2f));
    EXPECT_NEAR(hit.rayDepth, reference.rayDepth, 1.0e-4f);
    EXPECT_NEAR(hit.surfaceUvOffset.x, reference.surfaceUvOffset.x, 1.0e-5f);
    EXPECT_NEAR(hit.surfaceUvOffset.y, reference.surfaceUvOffset.y, 1.0e-5f);
}

TEST(ParallaxOcclusion, DenserTilingCarvesProportionallyShallower)
{
    // reliefDepth is a fraction of one height repeat: bricks half as wide are half as deep.
    const PlaneView single = PlaneView{}.WithShift(10.0f);
    PlaneView doubled = single;
    doubled.HeightRow0 = vec4(2.0f, 0.0f, 0.0f, 0.0f);
    doubled.HeightRow1 = vec4(0.0f, 2.0f, 0.0f, 0.0f);
    const auto a = SetupPlaneRay<DesktopArm>(single);
    const auto b = SetupPlaneRay<DesktopArm>(doubled);
    EXPECT_NEAR(b.reliefMetres, 0.5f * a.reliefMetres, 1.0e-7f);
    EXPECT_NEAR(b.shiftPixels, 0.5f * a.shiftPixels, 1.0e-4f);
}

TEST(ParallaxOcclusion, StretchedUvsStepTheSameDistanceAcrossTheSurface)
{
    // uv0 at 2 m per repeat along u and 1 m along v against an isotropic sqrt(2) m per repeat: the
    // same relief depth in metres, so the hit must be the same point on the surface in metres,
    // although its uv0 offsets differ.
    PlaneView isotropic{};
    isotropic.Azimuth = 0.6f;
    isotropic.PixelMetres = 0.004f;
    isotropic.MetresPerRepeatU = std::sqrt(2.0f);
    isotropic.MetresPerRepeatV = std::sqrt(2.0f);
    PlaneView stretched = isotropic;
    stretched.MetresPerRepeatU = 2.0f;
    stretched.MetresPerRepeatV = 1.0f;

    auto metresOf = [](const PlaneView& view, vec2 uvOffset)
    { return vec2(uvOffset.x * view.MetresPerRepeatU, uvOffset.y * view.MetresPerRepeatV); };
    const auto field = [](float s) { return 0.3f + 0.4f * s; };
    const auto a = MarchAlongPath<DesktopArm>(isotropic, vec2(0.1f, 0.1f), field);
    const auto b = MarchAlongPath<DesktopArm>(stretched, vec2(0.1f, 0.1f), field);
    EXPECT_NEAR(a.Ray.reliefMetres, b.Ray.reliefMetres, 1.0e-6f);
    EXPECT_NEAR(a.Ray.shiftPixels, b.Ray.shiftPixels, 1.0e-3f);
    const vec2 metresA = metresOf(isotropic, a.Hit.surfaceUvOffset);
    const vec2 metresB = metresOf(stretched, b.Hit.surfaceUvOffset);
    EXPECT_NEAR(metresA.x, metresB.x, 1.0e-5f);
    EXPECT_NEAR(metresA.y, metresB.y, 1.0e-5f);
}

TEST(ParallaxOcclusion, VertexTangentOfEitherHandednessMarchesTowardIncreasingUv)
{
    PlaneView view = PlaneView{}.WithShift(10.0f);
    view.Azimuth = 1.1f;
    const auto field = [](float s) { return 0.25f + 0.5f * s; };
    const auto reference = MarchAlongPath<DesktopArm>(view, vec2(0.3f, 0.2f), field);
    for (const vec3 tangent : {vec3(1.0f, 0.0f, 0.0f), vec3(-1.0f, 0.0f, 0.0f), vec3(2.0f, 0.0f, 0.3f)})
    {
        PlaneView withTangent = view;
        withTangent.Tangent = tangent;
        const auto marched = MarchAlongPath<DesktopArm>(withTangent, vec2(0.3f, 0.2f), field);
        EXPECT_NEAR(marched.Hit.surfaceUvOffset.x, reference.Hit.surfaceUvOffset.x, 1.0e-6f);
        EXPECT_NEAR(marched.Hit.surfaceUvOffset.y, reference.Hit.surfaceUvOffset.y, 1.0e-6f);
    }
}

TEST(ParallaxOcclusion, RankDeficientFootprintSkipsTheMarch)
{
    // Both pixel steps move along the same UV direction: a UV-degenerate triangle, or a silhouette.
    const auto frame = DesktopArm::BuildFrame(vec3(0.0f, 0.0f, 1.0f), vec3(0.0f, 0.0f, 0.0f),
                                              vec3(0.01f, 0.0f, 0.0f), vec3(0.0f, 0.01f, 0.0f),
                                              vec2(0.01f, 0.0f), vec2(0.02f, 0.0f));
    EXPECT_FALSE(frame.valid);
    const auto ray = DesktopArm::SetupRay(frame, PlaneView{}.ViewDirection(), 0.05f, vec4(1.0f, 0.0f, 0.0f, 0.0f),
                                          vec4(0.0f, 1.0f, 0.0f, 0.0f));
    EXPECT_EQ(ray.linearSteps, 0);
}

TEST(ParallaxOcclusion, BackFaceCarvesTheMirroredRelief)
{
    // Seen from below (two-sided), |cos| carves inward from the seen face: the same shift length.
    PlaneView front = PlaneView{}.WithShift(6.0f);
    PlaneView back = front;
    back.ViewAngle = std::numbers::pi_v<float> - front.ViewAngle;
    const auto a = SetupPlaneRay<DesktopArm>(front);
    const auto b = SetupPlaneRay<DesktopArm>(back);
    EXPECT_NEAR(a.shiftPixels, b.shiftPixels, 1.0e-3f);
    EXPECT_EQ(b.faceSign, -1.0f);
    EXPECT_EQ(a.faceSign, 1.0f);
}

// ---- Self-shadow -----------------------------------------------------------------------------

namespace
{
// The self-shadow of the point the view march hits on a floor at hitFieldHeight, with shadowField
// (of the height-space u) everywhere but that point.
template <class Arm>
float ShadowOnField(float hitFieldHeight, const std::function<float(float)>& shadowField, vec3 light)
{
    PlaneView view = PlaneView{}.WithShift(kEightStepShift);
    const vec2 uv0(0.3f, 0.2f);
    const auto marched = MarchAlongPath<Arm>(view, uv0, [=](float) { return hitFieldHeight; });
    const float hitU = HeightUv(view, uv0).x + marched.Hit.heightUvOffset.x;
    constexpr float kPointClearance = 1.0e-5f;
    s_HeightField = [=](float u, float)
    { return std::abs(u - hitU) < kPointClearance ? hitFieldHeight : shadowField(u); };
    return Arm::SelfShadow(marched.Frame, marched.Ray, marched.Hit, HeightUv(view, uv0), light, view.HeightRow0,
                           view.HeightRow1);
}

// A wall seen head on, so the view march is skipped and the self-shadow starts at the undisplaced
// point. The point sits in a joint at height 0.2; everything past it along +u stands at the top
// surface.
struct HeadOnJoint
{
    PlaneView View;
    Marched<DesktopArm> Point;
};

constexpr float kJointHeight = 0.2f;

HeadOnJoint HeadOnJointAt(float pixelMetres)
{
    HeadOnJoint joint;
    joint.View.ViewAngle = 0.0f;
    joint.View.PixelMetres = pixelMetres;
    const vec2 uv0(0.3f, 0.2f);
    joint.Point = MarchAlongPath<DesktopArm>(joint.View, uv0, [](float) { return kJointHeight; });
    const float pointU = HeightUv(joint.View, uv0).x;
    s_HeightField = [=](float u, float) { return u > pointU + 1.0e-5f ? 1.0f : kJointHeight; };
    return joint;
}

Desktop::GE_ParallaxShadow ShadowOfHeadOnJoint(const HeadOnJoint& joint, vec3 light)
{
    return Desktop::GE_ParallaxSelfShadow(joint.Point.Frame, joint.Point.Ray, joint.Point.Hit,
                                          HeightUv(joint.View, vec2(0.3f, 0.2f)), light, joint.View.HeightRow0,
                                          joint.View.HeightRow1, vec2(kHeightTexels, kHeightTexels), 0.0f);
}

float ShadowAtHeadOnJoint(const HeadOnJoint& joint, vec3 light)
{
    return ShadowOfHeadOnJoint(joint, light).light;
}

// The pixel size at which the full-depth shadow of a light `elevation` above a head-on wall is
// `pixels` long on screen (the relief depth is 0.05 of a 1 m repeat).
float PixelMetresForShadowLength(float pixels, float elevation)
{
    return 0.05f / std::tan(elevation) / pixels;
}

vec3 LightAt(float elevation, float azimuth)
{
    return vec3(std::cos(elevation) * std::cos(azimuth), std::cos(elevation) * std::sin(azimuth), std::sin(elevation));
}

// The light a head-on joint receives when the point lies at pointHeight, the path to the light at that
// height too, except one thin occluder standing `excess` (relief depths) above the light ray at the
// path's sample `step`.
float LightPastOneOccluder(const HeadOnJoint& joint, float pointHeight, vec3 light, int step, float excess)
{
    s_HeightField = [=](float, float) { return pointHeight; };
    s_SampledPoints.clear();
    s_RecordingSamples = true;
    const int steps = ShadowOfHeadOnJoint(joint, light).fetches - 1;
    s_RecordingSamples = false;
    // The first sample is the point itself, then one per step along the path.
    const float occluderU = s_SampledPoints[static_cast<size_t>(step)].first;
    const float rayHeight = pointHeight + (1.0f - pointHeight) * static_cast<float>(step) / static_cast<float>(steps);
    constexpr float kOccluderHalfWidthU = 5.0e-4f; // far narrower than any step these cases take
    s_HeightField = [=](float u, float)
    { return std::abs(u - occluderU) < kOccluderHalfWidthU ? rayHeight + excess : pointHeight; };
    return ShadowOfHeadOnJoint(joint, light).light;
}
} // namespace

TEST(ParallaxOcclusion, FlatFieldCastsNoShadow)
{
    // The hit sits on a flat floor at 0.4 and the path to the light crosses only that floor.
    EXPECT_EQ(ShadowOnField<DesktopArm>(0.4f, [](float) { return 0.4f; }, LightAt(0.5f, 0.3f)), 1.0f);
}

TEST(ParallaxOcclusion, WallTowardTheLightShadowsFully)
{
    // Everything between the hit and the light stands at the top surface.
    EXPECT_EQ(ShadowOnField<DesktopArm>(0.4f, [](float) { return 1.0f; }, LightAt(0.5f, 0.3f)), 0.0f);
}

TEST(ParallaxOcclusion, LightBelowTheSeenFaceIsLeftToTheLightingCosine)
{
    EXPECT_EQ(ShadowOnField<DesktopArm>(0.4f, [](float) { return 1.0f; }, LightAt(-0.3f, 0.3f)), 1.0f);
}

TEST(ParallaxOcclusion, AHeadOnWallStillShadowsItsJoints)
{
    // Seen head on the relief shifts nothing on screen and the view march is skipped, but the light,
    // 0.5 rad above the wall, casts the brick's edge 18 px across the joint below it.
    const HeadOnJoint joint = HeadOnJointAt(0.005f);
    ASSERT_EQ(joint.Point.Ray.linearSteps, 0);
    ASSERT_EQ(joint.Point.Hit.fetches, 0);
    EXPECT_EQ(ShadowAtHeadOnJoint(joint, LightAt(0.5f, 0.0f)), 0.0f);
}

TEST(ParallaxOcclusion, TheSelfShadowFadesInWithItsOwnLength)
{
    // The full-depth shadow's length on screen is 0.05 m x cot(0.5) over the pixel size: skipped
    // below 1 px and faded in with a smoothstep up to 3 px, so a relief too fine for the pixel grid
    // neither sparkles nor pops.
    const vec3 light = LightAt(0.5f, 0.0f);
    const float fullPathMetres = 0.05f / std::tan(0.5f);
    const auto shadowAt = [&](float pixels) { return ShadowAtHeadOnJoint(HeadOnJointAt(fullPathMetres / pixels), light); };
    EXPECT_EQ(shadowAt(0.95f), 1.0f);
    EXPECT_NEAR(shadowAt(1.05f), 1.0f, 0.01f) << "continuous at the skip threshold";
    EXPECT_NEAR(shadowAt(2.0f), 0.5f, 1.0e-3f);
    EXPECT_EQ(shadowAt(3.05f), 0.0f);
    EXPECT_EQ(shadowAt(40.0f), 0.0f);
}

TEST(ParallaxOcclusion, TheSelfShadowSamplesHalfwayBetweenItsPathAndTheMajorAxis)
{
    // One height texel per pixel along u and four along v (major axis level 2, geometric mean 1). The
    // self-shadow samples halfway between the footprint along its own path and the major axis: a
    // light along u (path level 0) at level 1, a light along v (path level 2) at level 2; the view
    // march stays at the geometric mean whatever the light does. The view's TAAU bias rides on top.
    PlaneView view;
    view.ViewAngle = 0.0f;
    view.MetresPerRepeatV = 0.25f;
    view.PixelMetres = 1.0f / kHeightTexels;
    const vec2 uv0(0.3f, 0.2f);
    const auto point = MarchAlongPath<DesktopArm>(view, uv0, [](float) { return kJointHeight; });
    const vec2 pointUv = HeightUv(view, uv0);
    // Everything past the point, along u or along v, stands at the top surface.
    s_HeightField = [=](float u, float v)
    { return (u - pointUv.x) + (v - pointUv.y) > 1.0e-6f ? 1.0f : kJointHeight; };
    const vec2 texels(kHeightTexels, kHeightTexels);
    struct Case
    {
        float Azimuth;
        float Level;
    };
    for (const Case c : {Case{0.0f, 1.0f}, Case{0.5f * std::numbers::pi_v<float>, 2.0f}})
    {
        for (const float mipBias : {0.0f, -0.5f})
        {
            ResetSampledLods();
            const auto shadow = Desktop::GE_ParallaxSelfShadow(point.Frame, point.Ray, point.Hit, pointUv,
                                                               LightAt(0.5f, c.Azimuth), view.HeightRow0,
                                                               view.HeightRow1, texels, mipBias);
            EXPECT_LT(shadow.light, 1.0f) << "the path was sampled, azimuth " << c.Azimuth;
            EXPECT_NEAR(s_LowestSampledLod, c.Level + mipBias, 1.0e-4f) << "azimuth " << c.Azimuth;
            EXPECT_NEAR(s_HighestSampledLod, c.Level + mipBias, 1.0e-4f) << "azimuth " << c.Azimuth;
        }
    }
    EXPECT_NEAR(Desktop::GE_ParallaxMarchLod(point.Frame, view.HeightRow0, view.HeightRow1, texels, 0.0f), 1.0f,
                1.0e-5f);
}

TEST(ParallaxOcclusion, TheShadowLevelStaysWithinOneAndAHalfLevelsOfTheMajorAxis)
{
    // A grazing footprint, 1 texel per pixel across and 256 along (major axis level 8): a path across
    // it is held at major - 3 as the march is, and the level halfway to the major axis is 6.5.
    PlaneView view;
    view.ViewAngle = 0.0f;
    view.MetresPerRepeatV = 1.0f / 256.0f;
    view.PixelMetres = 1.0f / kHeightTexels;
    const auto frame = BuildPlaneFrame<DesktopArm>(view);
    const vec2 texels(kHeightTexels, kHeightTexels);
    EXPECT_NEAR(Desktop::GE_ParallaxShadowLod(frame, view.HeightRow0, view.HeightRow1, texels, 1.0f, 0.0f), 6.5f,
                1.0e-4f);
    EXPECT_NEAR(Desktop::GE_ParallaxShadowLod(frame, view.HeightRow0, view.HeightRow1, texels, 256.0f, 0.0f), 8.0f,
                1.0e-4f);
    EXPECT_NEAR(Desktop::GE_ParallaxShadowLod(frame, view.HeightRow0, view.HeightRow1, texels, 64.0f, -0.5f), 6.5f,
                1.0e-4f);
}

TEST(ParallaxOcclusion, TheSelfShadowTakesThePointAndOneStepPerPixelOfItsPathUpToEight)
{
    // The point, then min(8, ceil(E_L)) steps; E_L is the path from the point (height 0.2) up to the
    // polygon, 0.8 of the full-depth shadow's length.
    const vec3 light = LightAt(0.5f, 0.0f);
    struct Case
    {
        float FullShadowPixels;
        int Fetches;
    };
    for (const Case c : {Case{3.1f, 1 + 3}, Case{6.1f, 1 + 5}, Case{9.6f, 1 + 8}, Case{30.0f, 1 + 8}})
    {
        const HeadOnJoint joint = HeadOnJointAt(PixelMetresForShadowLength(c.FullShadowPixels, 0.5f));
        s_HeightFetchCount = 0;
        const auto shadow = ShadowOfHeadOnJoint(joint, light);
        EXPECT_EQ(shadow.fetches, c.Fetches) << c.FullShadowPixels << " px";
        EXPECT_EQ(s_HeightFetchCount, c.Fetches) << "the count reports the samples taken, " << c.FullShadowPixels << " px";
    }
    // Skipped (under a pixel) and a point too close to the top for a path: nothing, then the point.
    const HeadOnJoint shortPath = HeadOnJointAt(PixelMetresForShadowLength(0.9f, 0.5f));
    EXPECT_EQ(ShadowOfHeadOnJoint(shortPath, light).fetches, 0);
    const HeadOnJoint nearTop = HeadOnJointAt(PixelMetresForShadowLength(2.0f, 0.5f));
    s_HeightField = [](float, float) { return 0.9f; };
    EXPECT_EQ(ShadowOfHeadOnJoint(nearTop, light).fetches, 1) << "a rise of 0.1 casts 0.2 px: only the point";
}

TEST(ParallaxOcclusion, ABackFaceShadowsTowardALightOnItsOwnSide)
{
    // A two-sided surface seen from behind carves its relief inward from the seen face; a light on
    // that side is shadowed by it, and a light on the other side is left to the lighting's cosine.
    HeadOnJoint joint;
    joint.View.ViewAngle = std::numbers::pi_v<float>;
    joint.View.PixelMetres = 0.005f;
    const vec2 uv0(0.3f, 0.2f);
    joint.Point = MarchAlongPath<DesktopArm>(joint.View, uv0, [](float) { return kJointHeight; });
    ASSERT_EQ(joint.Point.Ray.faceSign, -1.0f);
    const float pointU = HeightUv(joint.View, uv0).x;
    s_HeightField = [=](float u, float) { return u > pointU + 1.0e-5f ? 1.0f : kJointHeight; };
    EXPECT_EQ(ShadowAtHeadOnJoint(joint, LightAt(-0.5f, 0.0f)), 0.0f) << "the light on the seen side";
    EXPECT_EQ(ShadowAtHeadOnJoint(joint, LightAt(0.5f, 0.0f)), 1.0f) << "the light behind the seen face";
}

TEST(ParallaxOcclusion, AnOccluderGrazingTheLightRayShadowsInProportion)
{
    // A thin occluder standing above the light ray shadows by its height above the ray per relief depth
    // the ray has climbed from the point to it, against the penumbra width of 0.25 at which it shadows
    // fully. At half that width it lets half the light through, whether the point lies deep in a joint
    // or near the top, and at the path's first step or its middle. The full-depth shadow is 30 px long:
    // eight steps from either point.
    constexpr float kHalfThePenumbraWidth = 0.125f;
    const vec3 light = LightAt(0.5f, 0.0f);
    const HeadOnJoint joint = HeadOnJointAt(PixelMetresForShadowLength(30.0f, 0.5f));
    for (const float pointHeight : {0.2f, 0.6f})
    {
        for (const int step : {1, 4})
        {
            const float climb = static_cast<float>(step) / 8.0f * (1.0f - pointHeight);
            EXPECT_NEAR(LightPastOneOccluder(joint, pointHeight, light, step, kHalfThePenumbraWidth * climb), 0.5f, 0.01f)
                << "point at " << pointHeight << ", occluder at step " << step;
        }
    }
}

TEST(ParallaxOcclusion, TheSameOccluderShadowsAlikeAtAnyStepCount)
{
    // The step count follows the path's length on screen; the shadow a given relief casts must not. The
    // same occluder, 0.02 of the relief above the light ray halfway along the path, is the second of
    // four samples when the full-depth shadow is 4.5 px long and the fourth of eight at 12 px. A weight
    // indexed by the sample rather than by its place along the path would fail this.
    const vec3 light = LightAt(0.5f, 0.0f);
    const HeadOnJoint shortPath = HeadOnJointAt(PixelMetresForShadowLength(4.5f, 0.5f));
    const HeadOnJoint longPath = HeadOnJointAt(PixelMetresForShadowLength(12.0f, 0.5f));
    const float fourSteps = LightPastOneOccluder(shortPath, kJointHeight, light, 2, 0.02f);
    ASSERT_EQ(s_SampledPoints.size(), 1u + 4u);
    const float eightSteps = LightPastOneOccluder(longPath, kJointHeight, light, 4, 0.02f);
    ASSERT_EQ(s_SampledPoints.size(), 1u + 8u);
    EXPECT_GT(eightSteps, 0.0f) << "partial, so the comparison can fail";
    EXPECT_LT(eightSteps, 1.0f) << "partial, so the comparison can fail";
    EXPECT_NEAR(fourSteps, eightSteps, 1.0e-5f);
}

TEST(ParallaxOcclusion, CompatArmHasNoSelfShadow)
{
    EXPECT_EQ(ShadowOnField<CompatArm>(0.4f, [](float) { return 1.0f; }, LightAt(0.5f, 0.3f)), 1.0f);
}

TEST(ParallaxOcclusion, OnlyReliefTowardTheLightShadows)
{
    // A wall standing just past the hit on the light's side of it shadows the hit; the same wall
    // behind the hit, on the side away from the light, does not. The light's azimuth is mostly +u,
    // the height-space axis the wall's edge crosses.
    PlaneView view = PlaneView{}.WithShift(kEightStepShift);
    const vec2 uv0(0.3f, 0.2f);
    const auto marched = MarchAlongPath<DesktopArm>(view, uv0, [](float) { return 0.4f; });
    ASSERT_GT(marched.Hit.rayDepth, 0.0f);
    const float hitU = HeightUv(view, uv0).x + marched.Hit.heightUvOffset.x;
    const vec3 light = LightAt(0.5f, 0.3f);
    const auto shadowWith = [&](const std::function<float(float)>& field)
    {
        s_HeightField = [=](float u, float) { return field(u); };
        return DesktopArm::SelfShadow(marched.Frame, marched.Ray, marched.Hit, HeightUv(view, uv0), light,
                                      view.HeightRow0, view.HeightRow1);
    };
    constexpr float kEdgeClearance = 1.0e-4f;
    EXPECT_EQ(shadowWith([=](float u) { return u > hitU + kEdgeClearance ? 1.0f : 0.4f; }), 0.0f);
    EXPECT_EQ(shadowWith([=](float u) { return u < hitU - kEdgeClearance ? 1.0f : 0.4f; }), 1.0f);
}

// ---- The steps view --------------------------------------------------------------------------

namespace
{
template <class Arm>
void ExpectTheFetchCountIsTheSamplesTaken()
{
    const std::function<float(float)> fields[] = {
        [](float) { return 0.0f; },                       // a pit floor at the full depth
        [](float s) { return 0.3f + 0.4f * s; },          // a ramp
        [](float s) { return s < 0.55f ? 0.2f : 1.0f; },  // a step
        [](float) { return 1.0f; },                       // the top surface
    };
    for (const float shift : {0.49f, 0.9f, 3.2f, kEightStepShift, 20.0f, 64.0f})
    {
        for (size_t f = 0; f < std::size(fields); ++f)
        {
            s_HeightFetchCount = 0;
            const auto marched = MarchAlongPath<Arm>(PlaneView{}.WithShift(shift), vec2(0.3f, 0.2f), fields[f]);
            EXPECT_EQ(marched.Hit.fetches, s_HeightFetchCount) << "shift " << shift << ", field " << f;
        }
    }
}
} // namespace

TEST(ParallaxOcclusion, TheFetchCountIsTheHeightSamplesTaken)
{
    ExpectTheFetchCountIsTheSamplesTaken<DesktopArm>();
    ExpectTheFetchCountIsTheSamplesTaken<CompatArm>();
}

// The most height samples a desktop pixel takes, the scale the steps view's last band reaches to in
// its user doc legend and its tool text (set_parallax_steps_view): the view march's 44 (nine linear
// batches of four, 32 steps and the final sample at the full depth, and two refinement batches of
// four) and the self-shadow's 9 (the point and eight along the path).
constexpr int kStepsViewFullScale = 53;

TEST(ParallaxOcclusion, TheStepsViewFullScaleIsTheMostADesktopPixelTakes)
{
    // The worst case: the view march's step budget reached with the floor found only by its last
    // linear sample, so every linear batch and every refinement part runs, and a self-shadow with the
    // point and its full eight steps.
    const auto marched =
        MarchAlongPath<DesktopArm>(PlaneView{}.WithShift(64.0f), vec2(0.3f, 0.2f), [](float) { return 0.0f; });
    EXPECT_EQ(marched.Ray.linearSteps, DesktopArm::kLinearBudget);
    const HeadOnJoint joint = HeadOnJointAt(PixelMetresForShadowLength(30.0f, 0.5f));
    const auto shadow = ShadowOfHeadOnJoint(joint, LightAt(0.5f, 0.0f));
    EXPECT_EQ(marched.Hit.fetches + shadow.fetches, kStepsViewFullScale);
}

namespace
{
// The chromaticity of a colour: its channels over their sum.
vec3 Chromaticity(vec3 c)
{
    const float sum = c.x + c.y + c.z;
    return vec3(c.x / sum, c.y / sum, c.z / sum);
}

// The palette colour a reader decodes a drawn colour to: the nearest in chromaticity.
vec3 NearestPaletteColour(vec3 drawn)
{
    const vec3 palette[] = {StepsView::kParallaxStepsViewBlue,   StepsView::kParallaxStepsViewCyan,
                            StepsView::kParallaxStepsViewGreen,  StepsView::kParallaxStepsViewYellow,
                            StepsView::kParallaxStepsViewOrange, StepsView::kParallaxStepsViewRed};
    vec3 nearest = palette[0];
    float best = std::numeric_limits<float>::infinity();
    for (const vec3& colour : palette)
    {
        const vec3 d = Chromaticity(colour) - Chromaticity(drawn);
        const float distance = Glsl::dot(d, d);
        if (distance < best)
        {
            best = distance;
            nearest = colour;
        }
    }
    return nearest;
}

bool SameColour(vec3 a, vec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
} // namespace

TEST(ParallaxOcclusion, TheStepsViewHasOneColourPerBandOfSamples)
{
    // Bands of four up to 16, then 17 to 24 and 25 to the full scale; neighbouring bands and every
    // pair of bands differ in chromaticity by more than 8-bit quantisation of a capture can blur.
    const int bandUpperBounds[] = {4, 8, 12, 16, 24, kStepsViewFullScale};
    int bandStart = 1;
    std::vector<vec3> bandColours;
    for (const int upper : bandUpperBounds)
    {
        const vec3 colour = StepsView::GE_ParallaxStepsViewColor(bandStart);
        for (int samples = bandStart; samples <= upper; ++samples)
            EXPECT_TRUE(SameColour(StepsView::GE_ParallaxStepsViewColor(samples), colour)) << samples;
        bandColours.push_back(colour);
        bandStart = upper + 1;
    }
    for (size_t a = 0; a < bandColours.size(); ++a)
    {
        for (size_t b = a + 1; b < bandColours.size(); ++b)
        {
            const vec3 d = Chromaticity(bandColours[a]) - Chromaticity(bandColours[b]);
            EXPECT_GT(std::sqrt(Glsl::dot(d, d)), 0.05f) << "bands " << a << " and " << b;
        }
    }
    EXPECT_TRUE(SameColour(StepsView::GE_ParallaxStepsViewColor(kStepsViewFullScale + 9),
                           bandColours.back()))
        << "a count past the scale stays in the last band";
}

TEST(ParallaxOcclusion, TheStepsViewKeepsTheLitLuminanceAndCarriesTheBandInItsChromaticity)
{
    // What the view draws: the lit colour's luminance, so it reads under any exposure; the band in
    // the chromaticity, decoded as the view's readers do; neutral grey where the pixel took no sample.
    const vec3 luminanceWeights(0.2126f, 0.7152f, 0.0722f);
    for (const vec3 lit : {vec3(0.3f, 0.5f, 0.2f), vec3(4.0f, 3.0f, 2.0f), vec3(0.01f, 0.0f, 0.02f)})
    {
        const float luminance = Glsl::dot(lit, luminanceWeights);
        const vec3 grey = StepsView::GE_ParallaxStepsViewShade(lit, 0);
        EXPECT_NEAR(grey.x, luminance, luminance * 1.0e-5f);
        EXPECT_EQ(grey.y, grey.x);
        EXPECT_EQ(grey.z, grey.x);
        for (int samples = 1; samples <= kStepsViewFullScale; ++samples)
        {
            const vec3 shade = StepsView::GE_ParallaxStepsViewShade(lit, samples);
            EXPECT_NEAR(Glsl::dot(shade, luminanceWeights), luminance, luminance * 1.0e-5f) << samples;
            EXPECT_TRUE(SameColour(NearestPaletteColour(shade), StepsView::GE_ParallaxStepsViewColor(samples)))
                << samples;
        }
    }
    const vec3 unlit = StepsView::GE_ParallaxStepsViewShade(vec3(-1.0f, -1.0f, -1.0f), 10);
    EXPECT_EQ(unlit.x + unlit.y + unlit.z, 0.0f) << "a negative colour draws black, not an inverted hue";
}

// ---- The march LOD ---------------------------------------------------------------------------

TEST(ParallaxOcclusion, MarchLodIsTheGeometricMeanWithinThreeLevelsOfTheMajorAxis)
{
    const vec4 identity0(1.0f, 0.0f, 0.0f, 0.0f);
    const vec4 identity1(0.0f, 1.0f, 0.0f, 0.0f);
    const vec2 texels(1024.0f, 1024.0f);
    auto lodFor = [&](float texelsX, float texelsY, float bias)
    {
        const auto frame = DesktopArm::BuildFrame(vec3(0.0f, 0.0f, 1.0f), vec3(0.0f, 0.0f, 0.0f),
                                                  vec3(0.01f, 0.0f, 0.0f), vec3(0.0f, 0.01f, 0.0f),
                                                  vec2(texelsX / 1024.0f, 0.0f), vec2(0.0f, texelsY / 1024.0f));
        return Desktop::GE_ParallaxMarchLod(frame, identity0, identity1, texels, bias);
    };
    EXPECT_NEAR(lodFor(4.0f, 4.0f, 0.0f), 2.0f, 1.0e-5f);   // isotropic: the footprint's own level
    EXPECT_NEAR(lodFor(1.0f, 4.0f, 0.0f), 1.0f, 1.0e-5f);   // 1:4 -> the geometric mean
    EXPECT_NEAR(lodFor(1.0f, 256.0f, 0.0f), 5.0f, 1.0e-5f); // 1:256 -> held at major - 3
    EXPECT_NEAR(lodFor(4.0f, 4.0f, -0.5f), 1.5f, 1.0e-5f);  // the view's TAAU bias rides on top
}

TEST(ParallaxOcclusion, MarchLodIsInvariantUnderScreenRotation)
{
    // The same footprint ellipse (1 and 256 height texels per pixel, then 1 and 4) seen with the
    // screen axes rotated, as on a wall receding diagonally: the level must not move.
    const vec4 identity0(1.0f, 0.0f, 0.0f, 0.0f);
    const vec4 identity1(0.0f, 1.0f, 0.0f, 0.0f);
    const vec2 texels(1024.0f, 1024.0f);
    struct Case
    {
        float Minor;
        float Major;
        float Lod;
    };
    for (const Case footprint : {Case{1.0f, 256.0f, 5.0f}, Case{1.0f, 4.0f, 1.0f}})
    {
        for (const float angle : {0.0f, std::numbers::pi_v<float> / 8.0f, std::numbers::pi_v<float> / 4.0f, 2.0f})
        {
            const float c = std::cos(angle);
            const float sn = std::sin(angle);
            // J R(angle): the columns are the rotated pixel axes' footprints in repeats.
            const vec2 uvPerPixelX(c * footprint.Minor / 1024.0f, sn * footprint.Major / 1024.0f);
            const vec2 uvPerPixelY(-sn * footprint.Minor / 1024.0f, c * footprint.Major / 1024.0f);
            const auto frame = DesktopArm::BuildFrame(vec3(0.0f, 0.0f, 1.0f), vec3(0.0f, 0.0f, 0.0f),
                                                      vec3(0.01f, 0.0f, 0.0f), vec3(0.0f, 0.01f, 0.0f), uvPerPixelX,
                                                      uvPerPixelY);
            ASSERT_TRUE(frame.valid);
            EXPECT_NEAR(Desktop::GE_ParallaxMarchLod(frame, identity0, identity1, texels, 0.0f), footprint.Lod, 1.0e-4f)
                << footprint.Minor << ":" << footprint.Major << " at " << angle << " rad";
        }
    }
}

} // namespace

// ---- The relief's depth (Includes/parallax_depth.glsl) ----------------------------------------------------

// A reverse-Z perspective projection's depth of a point at view distance zView (LH, +Z forward), and its
// P[2][2], the same matrix MakePerspectiveLH_ZO_ReverseZ builds.
struct ReverseZPerspective
{
    float Near = 0.1f;
    float Far = 1000.0f;
    float DepthScale() const { return -Near / (Far - Near); }
    float Depth(float zView) const { return DepthScale() + (Near * Far / (Far - Near)) / zView; }
};

TEST(ParallaxDepth, APerspectiveHitLiesWhereTheRayReachesAndIsFartherUnderReverseZ)
{
    const ReverseZPerspective projection;
    // A fragment 20 m away along a ray 30 degrees off the view axis, and a hit 4 cm farther along it.
    const float eyeDistance = 20.0f;
    const float cosOffAxis = std::cos(30.0f * std::numbers::pi_v<float> / 180.0f);
    const float offset = 0.04f;
    const float depth = projection.Depth(eyeDistance * cosOffAxis);
    const float expected = projection.Depth((eyeDistance + offset) * cosOffAxis);
    const float shifted = Depth::GE_ParallaxDepthAlongRay(depth, projection.DepthScale(), true, offset, eyeDistance);
    EXPECT_NEAR(shifted, expected, 1.0e-6f * depth);
    EXPECT_LT(shifted, depth) << "a carve-down hit is farther, so its reverse-Z depth is smaller";
    EXPECT_EQ(Depth::GE_ParallaxExactDepth(depth, projection.DepthScale(), true, offset, eyeDistance), shifted);
}

TEST(ParallaxDepth, AnOrthographicHitMovesBySlopeTimesDistance)
{
    // MakeOrthographicLH_ZO_ReverseZ: depth = -z_view / (far - near) + far / (far - near).
    const float nearZ = 0.5f;
    const float farZ = 200.0f;
    const float depthScale = -1.0f / (farZ - nearZ);
    const float depth = depthScale * 30.0f + farZ / (farZ - nearZ);
    const float shifted = Depth::GE_ParallaxDepthAlongRay(depth, depthScale, false, 0.25f, 30.0f);
    EXPECT_NEAR(shifted, depthScale * 30.25f + farZ / (farZ - nearZ), 1.0e-6f);
    EXPECT_LT(shifted, depth);
}

TEST(ParallaxDepth, NoOffsetKeepsTheFragmentsOwnDepthToTheBit)
{
    const ReverseZPerspective projection;
    for (const float depth : {1.0f, 0.37123457f, 4.9e-5f, 0.0f})
    {
        EXPECT_EQ(Depth::GE_ParallaxExactDepth(depth, projection.DepthScale(), true, 0.0f, 12.5f), depth);
        EXPECT_EQ(Depth::GE_ParallaxExactDepth(depth, -0.01f, false, 0.0f, 12.5f), depth);
    }
}

TEST(ParallaxDepth, ThePrepassNeverWritesNearerThanThePolygon)
{
    // depth_less: every value is <= the fragment's depth, whatever the offset.
    const ReverseZPerspective projection;
    for (const float eyeDistance : {0.2f, 3.0f, 60.0f, 900.0f})
        for (const float offset : {0.0f, 1.0e-4f, 0.02f, 0.5f})
        {
            const float depth = projection.Depth(eyeDistance);
            EXPECT_LE(Depth::GE_ParallaxExactDepth(depth, projection.DepthScale(), true, offset, eyeDistance), depth);
        }
}

TEST(ParallaxDepth, TheColourPassRebuildsThePrepassHitFromItsDepth)
{
    // The prepass marches and writes its hit's depth; the colour pass turns that depth back into the
    // offset along the ray and the hit. The round trip lands on the march's hit, and the depth of the
    // rebuilt hit is the depth written to within one ulp, perspective and orthographic.
    PlaneView view;
    view = view.WithShift(kEightStepShift);
    const auto marched = MarchAlongPath<ReliefDepthArm>(view, vec2(0.25f, 0.5f),
                                                        [](float s) { return s < 0.6f ? 0.2f : 1.0f; });
    ASSERT_GT(marched.Hit.rayDepth, 0.0f);
    ASSERT_GT(marched.Ray.depthMetres, 0.0f);
    const float offset = marched.Hit.rayDepth * marched.Ray.depthMetres;

    const ReverseZPerspective perspective;
    const float orthographicScale = -1.0f / 199.5f;
    struct Projection
    {
        float DepthScale;
        bool Perspective;
        float Depth;
    };
    const float eyeDistance = 14.0f;
    for (const Projection projection :
         {Projection{perspective.DepthScale(), true, perspective.Depth(eyeDistance * 0.94f)},
          Projection{orthographicScale, false, orthographicScale * eyeDistance + 200.0f / 199.5f}})
    {
        SCOPED_TRACE(projection.Perspective ? "perspective" : "orthographic");
        const float written = ReliefDepth::GE_ParallaxExactDepth(projection.Depth, projection.DepthScale,
                                                                 projection.Perspective, offset, eyeDistance);
        ASSERT_LT(written, projection.Depth);
        const float back = ReliefDepth::GE_ParallaxOffsetToDepth(projection.Depth, projection.DepthScale,
                                                                 projection.Perspective, written, eyeDistance);
        const auto rebuilt = ReliefDepth::GE_ParallaxHitAtDepth(marched.Ray, back);
        // The rebuilt hit lies within the inverse's own error bound of the march's.
        const float rayDepthError = ReliefDepth::GE_ParallaxOffsetError(projection.Depth, projection.DepthScale,
                                                                        projection.Perspective, written, eyeDistance) /
                                    marched.Ray.depthMetres;
        if (projection.Perspective)
            ASSERT_LT(rayDepthError, 2.0e-4f) << "the bound is tight enough to mean something at this distance";
        EXPECT_NEAR(rebuilt.rayDepth, marched.Hit.rayDepth, rayDepthError);
        const float rewritten = ReliefDepth::GE_ParallaxExactDepth(
            projection.Depth, projection.DepthScale, projection.Perspective,
            rebuilt.rayDepth * marched.Ray.depthMetres, eyeDistance);
        EXPECT_LE(std::abs(rewritten - written), std::nextafter(written, 1.0f) - written)
            << "the rebuilt hit's depth is the depth the prepass wrote";
    }
}

TEST(ParallaxDepth, TheRebuiltHitIsTheReliefAndANearerPropIsNot)
{
    // A wall in the field at path parameter 0.6: the march's hit, interpolated just short of the wall,
    // reads as the relief. A point halfway there lies in the air above the low field: a prop standing in
    // the gap, nearer than the relief, which the colour pass must not cover.
    PlaneView view;
    view = view.WithShift(kEightStepShift);
    const auto marched = MarchAlongPath<ReliefDepthArm>(view, vec2(0.25f, 0.5f),
                                                        [](float s) { return s < 0.6f ? 0.2f : 1.0f; });
    const vec2 start = HeightUv(view, vec2(0.25f, 0.5f));
    EXPECT_TRUE(ReliefDepth::GE_ParallaxCheckRelief(start, marched.Ray, marched.Hit.rayDepth, 0.0f, 0.0f).onRelief);
    EXPECT_FALSE(
        ReliefDepth::GE_ParallaxCheckRelief(start, marched.Ray, 0.5f * marched.Hit.rayDepth, 0.0f, 0.0f).onRelief);
}

TEST(ParallaxDepth, EveryHitRebuiltFromItsDepthReadsAsTheRelief)
{
    // A relief fragment the check reads as air is discarded: a one-pixel hole. Over stepped and rippled
    // fields, view angles, step counts and eye distances out to 600 m, the hit rebuilt from the depth the
    // prepass wrote, with its error bound, reads as the relief wherever the march's own hit does.
    const ReverseZPerspective projection;
    std::mt19937 random(1234u);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    int rebuiltHits = 0;
    int falseAir = 0;
    for (const float shift : {1.5f, 3.9f, 7.9f, 15.9f, 31.9f, 64.0f})
        for (const float angle : {0.2f, 0.7f, 1.2f, 1.45f})
            for (int fieldIndex = 0; fieldIndex < 60; ++fieldIndex)
            {
                std::vector<std::pair<float, float>> steps;
                const int count = 1 + static_cast<int>(unit(random) * 6.0f);
                for (int i = 0; i < count; ++i)
                    steps.emplace_back(unit(random), unit(random));
                std::sort(steps.begin(), steps.end());
                const float base = unit(random) * 0.5f;
                const float wobble = unit(random) * 0.3f;
                const float frequency = 5.0f + unit(random) * 60.0f;
                const auto field = [=](float s)
                {
                    float h = base + wobble * 0.5f * (1.0f + std::sin(frequency * s));
                    for (const auto& step : steps)
                        if (s >= step.first)
                            h = step.second;
                    return std::clamp(h, 0.0f, 1.0f);
                };
                PlaneView view;
                view.ViewAngle = angle;
                view = view.WithShift(shift);
                const vec2 uv0(0.25f + 0.01f * static_cast<float>(fieldIndex), 0.5f);
                const auto marched = MarchAlongPath<ReliefDepthArm>(view, uv0, field);
                if (!(marched.Hit.rayDepth > 0.0f) || !(marched.Ray.depthMetres > 0.0f))
                    continue;
                const vec2 start = HeightUv(view, uv0);
                ASSERT_TRUE(
                    ReliefDepth::GE_ParallaxCheckRelief(start, marched.Ray, marched.Hit.rayDepth, 0.0f, 0.0f).onRelief)
                    << "the march's own hit, field " << fieldIndex;
                const float offset = marched.Hit.rayDepth * marched.Ray.depthMetres;
                for (const float eyeDistance : {0.3f, 1.0f, 3.0f, 10.0f, 40.0f, 150.0f, 600.0f})
                {
                    ++rebuiltHits;
                    const float depth = projection.Depth(eyeDistance * 0.94f);
                    const float written =
                        ReliefDepth::GE_ParallaxExactDepth(depth, projection.DepthScale(), true, offset, eyeDistance);
                    const float back = ReliefDepth::GE_ParallaxOffsetToDepth(depth, projection.DepthScale(), true,
                                                                             written, eyeDistance);
                    const float error = ReliefDepth::GE_ParallaxOffsetError(depth, projection.DepthScale(), true,
                                                                            written, eyeDistance);
                    const auto rebuilt = ReliefDepth::GE_ParallaxHitAtDepth(marched.Ray, back);
                    if (!ReliefDepth::GE_ParallaxCheckRelief(start, marched.Ray, rebuilt.rayDepth,
                                                             error / marched.Ray.depthMetres, 0.0f)
                             .onRelief)
                        ++falseAir;
                }
            }
    EXPECT_GT(rebuiltHits, 10000);
    EXPECT_EQ(falseAir, 0) << "of " << rebuiltHits << " rebuilt hits";
}
