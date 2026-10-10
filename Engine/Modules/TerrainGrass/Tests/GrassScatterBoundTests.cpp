// The grass scatter-gain deficit, and the BOUND that keeps it from inventing light.
//
// The term hands back the brightness the blade normal gives up as the settle turns a blade from its
// own near-horizontal normal toward the settle normal. What it hands back has to be what was lost,
// and it is pinned here from two directions because it failed in both:
//
//   * measuring the deficit between the two ENDPOINT normals and scaling it linearly by bladeWeight
//     OVER-pays in the interior, because the shaded normal is normalize(mix(settle, blade, w)) and
//     normalizing keeps that blend nearer the settle normal than a straight line does. The field
//     then renders BRIGHTER than shading purely on the settle normal — measured at +5.6 8-bit
//     levels — which a term that only returns a loss cannot legitimately do. That is the band.
//
//   * measuring it against the shaded normal alone UNDER-pays nothing but over-pays elsewhere:
//     ndlBlade is clamped at zero, so a blade turned away from the sun reports no blade-side light
//     while the blended normal still carries the negative dot, and the shaded-normal deficit then
//     exceeds the swing the endpoints describe. Unbounded, the term became a second, ungated
//     scattering contribution on the shade side — ~24% of grass pixels brightened, +5.8 levels on
//     the darkest decile — next to the authored, GrassTranslucency-gated backlight that already
//     exists. An unauthored term must not invent light.
//
// The shipped expression is EXECUTED here, lifted verbatim out of the shader at build time, not
// restated: a restatement is exactly the thing that can drift away from the file that ships.
//
// RED-ARM: delete the `min(...)` cap in GrassScatterDeficit and BoundNeverExceedsEndpointBudget
// fails on the shade-side cases. That is the regression this file exists to catch.

#include <gtest/gtest.h>

#include "GlslShim.h"

#include <cmath>
#include <vector>

namespace
{
using GameEngine::GlslShim::vec3;

namespace Shader
{
using GameEngine::GlslShim::vec3;
using GameEngine::GlslShim::clamp;
using GameEngine::GlslShim::dot;
using GameEngine::GlslShim::max;
using GameEngine::GlslShim::min;
using GameEngine::GlslShim::mix;
using GameEngine::GlslShim::normalize;
#include "GrassScatterDeficitExtracted.h"
} // namespace Shader

using GameEngine::GlslShim::dot;
using GameEngine::GlslShim::mix;
using GameEngine::GlslShim::normalize;

constexpr float kEps = 1e-5f;

vec3 Dir(float azimuthDeg, float elevationDeg)
{
    const float a = azimuthDeg * 3.14159265358979f / 180.0f;
    const float e = elevationDeg * 3.14159265358979f / 180.0f;
    return normalize(vec3{std::cos(e) * std::sin(a), std::sin(e), std::cos(e) * std::cos(a)});
}

// A SPREAD of settle normals to run the bound over. The surface settles onto the terrain normal
// at the blade's foot, which is upper-hemisphere and near-up but otherwise arbitrary, so this
// derives a near-up unit vector per sampled blade rather than pinning one value: the property being
// tested holds for ANY unit settle normal, and sampling a spread is what makes that claim mean
// something. It is not a copy of an expression in the shader and must not become one.
vec3 SettleNormal(const vec3& bladeNormal)
{
    return normalize(vec3{0.0f, 1.0f, 0.0f}
                     + vec3{std::fabs(bladeNormal.x) * 0.16f,
                            std::fabs(bladeNormal.y) * 0.28f,
                            std::fabs(bladeNormal.z) * 0.16f});
}

struct Sample
{
    vec3 settle;
    vec3 blade;
    vec3 shaded;
    vec3 light;
    float weight;
    float ndlSettle;
    float ndlBlade;
    float ndlShaded;
    float deficit;
};

// The swing the two endpoints describe: what the shipped term used, and the budget the bound caps
// against. Clamped at zero on each dot exactly as the shader does.
float EndpointBudget(const Sample& s)
{
    return s.weight * (s.ndlSettle - s.ndlBlade);
}

std::vector<Sample> Sweep()
{
    std::vector<Sample> out;
    for (float bladeAz = 0.0f; bladeAz < 360.0f; bladeAz += 30.0f)
    {
        // A blade is a near-vertical ribbon, so its geometric normal is near-HORIZONTAL.
        for (float bladeEl : {-25.0f, -5.0f, 5.0f, 25.0f})
        {
            const vec3 blade = Dir(bladeAz, bladeEl);
            const vec3 settle = SettleNormal(blade);
            for (float sunAz = 0.0f; sunAz < 360.0f; sunAz += 45.0f)
            {
                // Sun elevations spanning a high midday sun down to a near-horizon one, which is
                // the case the endpoint form and the shaded-normal form disagree about.
                // -10 covers the below-horizon regime: ndlSettle clamps to 0, both arms go
                // non-positive and the deficit must be exactly 0 rather than merely small.
                for (float sunEl : {-10.0f, 5.0f, 15.0f, 35.0f, 60.0f, 85.0f})
                {
                    const vec3 light = Dir(sunAz, sunEl);
                    for (float w : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f})
                    {
                        Sample s;
                        s.settle = settle;
                        s.blade = blade;
                        s.shaded = normalize(mix(settle, blade, w));
                        s.light = light;
                        s.weight = w;
                        s.ndlSettle = std::fmax(0.0f, dot(settle, light));
                        s.ndlBlade = std::fmax(0.0f, dot(blade, light));
                        s.ndlShaded = std::fmax(0.0f, dot(s.shaded, light));
                        s.deficit = Shader::GrassScatterDeficit(settle, blade, s.shaded, light, w);
                        out.push_back(s);
                    }
                }
            }
        }
    }
    return out;
}

// THE bound. Removing the cap from the shader makes this fail.
TEST(GrassScatterBound, BoundNeverExceedsEndpointBudget)
{
    int shadeSideCases = 0;
    for (const Sample& s : Sweep())
    {
        const float budget = std::fmax(0.0f, EndpointBudget(s));
        EXPECT_LE(s.deficit, budget + kEps)
            << "deficit " << s.deficit << " exceeds the endpoint budget " << budget
            << " at bladeWeight " << s.weight << " (ndlSettle " << s.ndlSettle
            << ", ndlBlade " << s.ndlBlade << ", ndlShaded " << s.ndlShaded << ")";
        if (dot(s.blade, s.light) < 0.0f && s.ndlSettle > 0.0f && s.weight > 0.0f
            && s.ndlSettle - s.ndlShaded > EndpointBudget(s) + kEps)
            ++shadeSideCases;
    }
    // The sweep must actually CONTAIN the case the bound exists for, or it passes vacuously and
    // would keep passing with the cap deleted.
    EXPECT_GT(shadeSideCases, 0)
        << "no sample where the shaded-normal deficit exceeds the endpoint budget — this sweep "
           "cannot detect an unbounded regression";
}

// The other half: it must not hand back more than the loss actually taken either.
TEST(GrassScatterBound, NeverExceedsTheLossActuallyTaken)
{
    for (const Sample& s : Sweep())
        EXPECT_LE(s.deficit, std::fmax(0.0f, s.ndlSettle - s.ndlShaded) + kEps)
            << "deficit " << s.deficit << " exceeds the shaded-normal loss at bladeWeight "
            << s.weight;
}

// Both ends are exact: at bladeWeight 0 the shaded normal IS the settle normal and nothing is
// owed; at 1 it IS the blade normal and the
// two arms of the cap agree, so the lift matches what the endpoint form always paid.
TEST(GrassScatterBound, EndpointsAreExact)
{
    for (float bladeAz = 0.0f; bladeAz < 360.0f; bladeAz += 45.0f)
    {
        const vec3 blade = Dir(bladeAz, 5.0f);
        const vec3 settle = SettleNormal(blade);
        for (float sunEl : {10.0f, 45.0f, 80.0f})
        {
            const vec3 light = Dir(20.0f, sunEl);
            EXPECT_NEAR(Shader::GrassScatterDeficit(settle, blade, settle, light, 0.0f), 0.0f, kEps);
            const float atOne = Shader::GrassScatterDeficit(settle, blade, blade, light, 1.0f);
            const float endpoint = GameEngine::GlslShim::clamp(
                std::fmax(0.0f, dot(settle, light)) - std::fmax(0.0f, dot(blade, light)),
                0.0f, 1.0f);
            EXPECT_NEAR(atOne, endpoint, kEps);
        }
    }
}

// Under a LOW sun the near-horizontal blade normal is the BETTER-lit of the two, so nothing is lost
// and nothing may be handed back. This is the failure the shipped code's own comment predicted for
// any alternative that lifts unconditionally, and the reason the result is clamped rather than
// signed.
TEST(GrassScatterBound, NoLiftWhereTheBladeNormalGains)
{
    const vec3 blade = Dir(0.0f, 0.0f);          // horizontal, facing the low sun
    const vec3 settle = SettleNormal(blade);
    const vec3 light = Dir(0.0f, 4.0f);          // just above the horizon, along the blade normal
    ASSERT_GT(dot(blade, light), dot(settle, light))
        << "this fixture no longer exercises a blade normal that is better lit than the settle";
    for (float w : {0.1f, 0.25f, 0.5f, 0.75f, 1.0f})
    {
        const vec3 shaded = normalize(mix(settle, blade, w));
        EXPECT_NEAR(Shader::GrassScatterDeficit(settle, blade, shaded, light, w), 0.0f, kEps)
            << "handed brightness back at bladeWeight " << w << " where the blade normal gains it";
    }
}

} // namespace
