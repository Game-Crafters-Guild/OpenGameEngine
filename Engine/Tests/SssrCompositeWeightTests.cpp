// Execute the shipped composite policy and pin its producer/consumer wiring.
#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include "SssrShaderSource.h"

namespace
{
using vec3 = glm::vec3;
#include "../Modules/Rendering/Shaders/ScreenSpaceReflections/sssr_composite_policy.glsl"
void ExpectNear(vec3 actual, vec3 expected)
{
    for (int c = 0; c < 3; ++c) EXPECT_NEAR(actual[c], expected[c], 1e-5f);
}
}

TEST(SssrCompositeWeight, ReplacementPreservesDiffuseDirectAndOtherLobes)
{
    vec3 unaffected(2.0f, 0.7f, 0.3f);
    vec3 probe(1.0f, 2.0f, 4.0f), hit(4.0f, 0.5f, 1.0f);
    for (vec3 weight : {vec3(0.04f), vec3(0.9f, 0.5f, 0.1f), vec3(0.07f, 0.02f, 0.13f)})
    {
        vec3 shaded = unaffected + weight * probe;
        ExpectNear(GE_SssrComposite(shaded, hit, probe, weight, 1.0f), unaffected + weight * hit);
        ExpectNear(GE_SssrComposite(shaded, probe, probe, weight, 1.0f), shaded);
        ExpectNear(GE_SssrComposite(shaded, hit, probe, weight, 0.0f), shaded);
        ExpectNear(GE_SssrComposite(shaded, hit, probe, weight, 0.5f),
                   unaffected + weight * (hit + probe) * 0.5f);
    }
}

TEST(SssrCompositeWeight, ZeroResponseAddsNoFresnelFloor)
{
    ExpectNear(GE_SssrComposite(vec3(0.3f), vec3(1000.0f), vec3(1.0f), vec3(0.0f), 1.0f), vec3(0.3f));
}

TEST(SssrCompositeWeight, ForwardExportsTheActualIncidentRadianceAndLayeredWeight)
{
    const auto ibl = GE::Tests::ReadSssrShaderSource("Includes/ibl.glsl");
    const auto adapter = GE::Tests::ReadSssrShaderSource("Adapters/adapter_forward.glsl");
    ASSERT_FALSE(ibl.empty());
    ASSERT_FALSE(adapter.empty());
    EXPECT_NE(ibl.find("vec3 specular = pref * specularWeight;"), std::string::npos);
    EXPECT_NE(ibl.find("ge_sssrIncidentRadiance = pref;"), std::string::npos);
    EXPECT_NE(ibl.find("ge_sssrSpecularWeight = specularWeight * specAO;"), std::string::npos);
    EXPECT_NE(ibl.find("ge_sssrSpecularWeight *= coatDarken * so.coatColor * (1.0 - ccFc);"), std::string::npos);
    EXPECT_NE(ibl.find("ge_sssrSpecularWeight *= 1.0 - fuzzReflectance;"), std::string::npos);
    EXPECT_NE(adapter.find("ge_sssrSpecularWeight * ambientMultiplier"), std::string::npos);
    EXPECT_NE(adapter.find("vec4(ge_sssrIncidentRadiance, reflectionOct.y)"), std::string::npos);
}

TEST(SssrCompositeWeight, MissAndCompositeReadTheSameForwardTexel)
{
    const auto composite = GE::Tests::ReadSssrShaderSource("ScreenSpaceReflections/sssr_composite.comp");
    const auto intersect = GE::Tests::ReadSssrShaderSource("ScreenSpaceReflections/sssr_intersect.comp");
    ASSERT_FALSE(composite.empty());
    ASSERT_FALSE(intersect.empty());
    EXPECT_NE(composite.find("texelFetch(uSpecularRadiance, pixel, 0).rgb"), std::string::npos);
    EXPECT_NE(intersect.find("texelFetch(uSpecularRadiance, pixel, 0).rgb"), std::string::npos);
    EXPECT_NE(composite.find("GE_SssrComposite(scene.rgb, reflection.rgb, env, specBrdf, replace)"), std::string::npos);
    EXPECT_EQ(composite.find("uEnvCube"), std::string::npos);
    EXPECT_EQ(composite.find("uBrdfLut"), std::string::npos);
}
