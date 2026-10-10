#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>
#include <glm/gtc/packing.hpp>
#include "SssrShaderSource.h"

namespace
{
using uint = uint32_t;
using vec2 = glm::vec2;
using vec3 = glm::vec3;
uint max(uint a, uint b) { return std::max(a, b); }
uint min(uint a, uint b) { return std::min(a, b); }
float exp2(float x) { return std::exp2(x); }
// GLSL scalar intrinsics for executing the actual production policy on the CPU.
float max(float a, float b) { return std::max(a, b); }
float min(float a, float b) { return std::min(a, b); }
float abs(float a) { return std::abs(a); }
float mix(float a, float b, float t) { return a + (b - a) * t; }
float smoothstep(float a, float b, float x)
{
    float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
#include "../Modules/Rendering/Shaders/ScreenSpaceReflections/sssr_history_policy.glsl"
#include "../Modules/Rendering/Shaders/ScreenSpaceReflections/sssr_reprojection_math.glsl"
}

TEST(SssrHistoryPolicy, DisocclusionToleranceRemainsRelativeAtDistance)
{
    for (float z : {1.0f, 10.0f, 100.0f, 1000.0f})
    {
        EXPECT_TRUE(GE_SssrDepthHistoryValid(z * 1.01f, z));
        EXPECT_FALSE(GE_SssrDepthHistoryValid(z * 1.05f, z));
    }
    EXPECT_FALSE(GE_SssrDepthHistoryValid(0.0f, 1.0f));
    EXPECT_FALSE(GE_SssrDepthHistoryValid(std::numeric_limits<float>::quiet_NaN(), 1.0f));
}

TEST(SssrHistoryPolicy, MirrorHitChangesRejectButSparseRaysAreNotMisses)
{
    EXPECT_TRUE(GE_SssrHitHistoryValid(0.02f, 1.0f, 4.0f, 4.05f));
    EXPECT_FALSE(GE_SssrHitHistoryValid(0.02f, 1.0f, 4.0f, 8.0f));
    EXPECT_FALSE(GE_SssrHitHistoryValid(0.02f, 0.4f, 0.0f, 4.0f));
    EXPECT_FALSE(GE_SssrHitHistoryValid(0.02f, 1.0f, 4.0f, 0.0f));
    EXPECT_TRUE(GE_SssrHitHistoryValid(0.02f, 0.0f, 0.0f, 4.0f));
    EXPECT_TRUE(GE_SssrHitHistoryValid(0.02f, 1.0f, 4.0f, -1.0f));
    EXPECT_TRUE(GE_SssrHitHistoryValid(0.7f, 1.0f, 4.0f, 8.0f));
}

TEST(SssrHistoryPolicy, RejectionStartsFromCurrentAndStartupUsesSampleCount)
{
    EXPECT_FLOAT_EQ(GE_SssrHistoryWeight(false, true, 1.0f, 0.5f, 0.0f, 0.1f), 0.0f);
    EXPECT_FLOAT_EQ(GE_SssrHistoryWeight(true, true, 0.0f, 0.5f, 0.0f, 0.1f), 0.0f);
    EXPECT_FLOAT_EQ(GE_SssrHistoryWeight(true, true, 1.0f / 32.0f, 0.5f, 0.0f, 0.1f), 0.5f);
    EXPECT_FLOAT_EQ(GE_SssrHistoryWeight(true, false, 1.0f, 0.5f, 0.0f, 0.1f), 1.0f);
    EXPECT_FLOAT_EQ(GE_SssrHistoryWeight(false, false, 1.0f, 0.5f, 0.0f, 0.1f), 0.0f);
}

TEST(SssrHistoryPolicy, ChangedSignalAndMotionRespondFasterThanConvergedHistory)
{
    float stable = GE_SssrHistoryWeight(true, true, 1.0f, 0.5f, 0.0f, 0.1f);
    EXPECT_GT(stable, 0.9f);
    EXPECT_LT(GE_SssrHistoryWeight(true, true, 1.0f, 0.5f, 1.0f, 0.1f), 0.2f);
    EXPECT_LT(GE_SssrHistoryWeight(true, true, 0.1f, 0.5f, 0.0f, 0.1f), stable);
}

TEST(SssrHistoryPolicy, ShadersConsumeValidationAndSurfaceHistoryHasFullLifecycle)
{
    const auto reproject = GE::Tests::ReadSssrShaderSource("ScreenSpaceReflections/sssr_reproject.comp");
    const auto temporal = GE::Tests::ReadSssrShaderSource("ScreenSpaceReflections/sssr_temporal.comp");
    const auto node = GE::Tests::ReadSssrNodeSource();
    ASSERT_FALSE(reproject.empty());
    ASSERT_FALSE(temporal.empty());
    ASSERT_FALSE(node.empty());
    EXPECT_NE(reproject.find("GE_SssrDepthHistoryValid(GE_SssrDecodeHistoryDepth(surface.a), expectedZ)"), std::string::npos);
    EXPECT_NE(reproject.find("GE_SssrHitHistoryValid(roughness, hit.a, expectedOldHitDistance, moments.b)"), std::string::npos);
    EXPECT_NE(reproject.find("texelFetch(uMoments, q, 0)"), std::string::npos);
    EXPECT_NE(temporal.find("historyLength > 0.0"), std::string::npos);
    EXPECT_NE(temporal.find("GE_SssrHistoryWeight(historySampleValid, currentValid,"), std::string::npos);
    EXPECT_NE(node.find("!surfaceReadFresh"), std::string::npos);
    EXPECT_NE(node.find("p.Read(surfaceRead,"), std::string::npos);
    EXPECT_NE(node.find("p.Write(surfaceWrite,"), std::string::npos);
    EXPECT_NE(node.find("MarkPersistentTextureInitialized(surfaceWrite)"), std::string::npos);
}

TEST(SssrHistoryPolicy, HalfFloatLogDepthPreservesRelativeDisocclusionPrecision)
{
    for (float z = 0.01f; z < 1000000.0f; z *= 1.07f)
    {
        float encoded = glm::unpackHalf(glm::packHalf(glm::vec2(std::log2(z), 0.0f))).x;
        float decoded = GE_SssrDecodeHistoryDepth(encoded);
        EXPECT_LT(std::abs(decoded - z) / z, 0.006f) << z;
        EXPECT_TRUE(GE_SssrDepthHistoryValid(decoded, z)) << z;
        EXPECT_FALSE(GE_SssrDepthHistoryValid(decoded, z * 1.05f)) << z;
    }
    EXPECT_FLOAT_EQ(GE_SssrDecodeHistoryDepth(-65504.0f), 0.0f);
}

TEST(SssrHistoryPolicy, DisocclusionsAndVarianceSpendRaysWhileConvergedLobesSaveThem)
{
    EXPECT_EQ(GE_SssrAdaptiveRate(4u, 0.8f, false, 0.0f, 1.0f), 1u);
    EXPECT_EQ(GE_SssrAdaptiveRate(4u, 0.8f, true, 0.2f, 1.0f), 2u);
    EXPECT_EQ(GE_SssrAdaptiveRate(2u, 0.5f, true, 0.0f, 1.0f), 4u);
    EXPECT_EQ(GE_SssrAdaptiveRate(1u, 0.02f, true, 0.0f, 1.0f), 1u);
    for (uint rate : {1u, 2u, 4u})
        for (bool valid : {false, true})
            for (float variance : {0.0f, 0.05f, 0.5f})
            {
                uint result = GE_SssrAdaptiveRate(rate, 0.6f, valid, variance, 0.8f);
                EXPECT_TRUE(result == 1u || result == 2u || result == 4u);
            }
}

TEST(SssrHistoryPolicy, TowardCameraRaysEndBeforeTheNearPlane)
{
    EXPECT_FLOAT_EQ(GE_SssrTraceDistance(4.0f, 1.0f, 0.1f, 100.0f), 100.0f);
    EXPECT_FLOAT_EQ(GE_SssrTraceDistance(4.0f, 0.0f, 0.1f, 100.0f), 100.0f);
    EXPECT_FLOAT_EQ(GE_SssrTraceDistance(4.0f, -0.01f, 0.1f, 10.0f), 10.0f);
    for (float origin : {0.11f, 1.0f, 10.0f, 100.0f})
        for (float direction : {-1.0f, -0.5f, -0.01f})
        {
            float distance = GE_SssrTraceDistance(origin, direction, 0.1f, 200.0f);
            EXPECT_GT(distance, 0.0f);
            EXPECT_GE(origin + direction * distance, 0.1f);
            EXPECT_LE(distance, 200.0f);
        }
    EXPECT_FLOAT_EQ(GE_SssrTraceDistance(0.05f, -1.0f, 0.1f, 100.0f), 0.0f);
}

TEST(SssrHistoryPolicy, JitterDoesNotCountAsMotionAndMoverUvUsesBothPhases)
{
    vec2 uv(0.4f, 0.6f), extent(1920.0f, 1080.0f);
    vec2 currentJitter(0.25f / extent.x, -0.75f / extent.y);
    vec2 previousJitter(-0.5f / extent.x, 0.125f / extent.y);
    vec2 oldUv = GE_SssrPreviousRasterUv(uv, vec2(0.0f), currentJitter, previousJitter);
    EXPECT_NEAR(GE_SssrMotionPixels(uv, oldUv, currentJitter, previousJitter, extent), 0.0f, 0.001f);
    vec2 motion(3.0f / extent.x, 4.0f / extent.y);
    oldUv = GE_SssrPreviousRasterUv(uv, motion, currentJitter, previousJitter);
    EXPECT_NEAR(GE_SssrMotionPixels(uv, oldUv, currentJitter, previousJitter, extent), 5.0f, 0.001f);
}

TEST(SssrHistoryPolicy, ReflectedMotionFollowsTheMirrorPlane)
{
    vec3 normal = glm::normalize(vec3(1.0f, 1.0f, 0.0f));
    vec3 point(1.0f, 2.0f, 3.0f), receiver(0.0f);
    vec3 reflected = GE_SssrMirrorPoint(point, receiver, normal);
    EXPECT_NEAR(reflected.x, -2.0f, 1e-5f);
    EXPECT_NEAR(reflected.y, -1.0f, 1e-5f);
    EXPECT_NEAR(reflected.z, 3.0f, 1e-5f);
    vec3 moved = GE_SssrMirrorPoint(point + vec3(1.0f, 0.0f, 0.0f), receiver, normal);
    EXPECT_NEAR(moved.x - reflected.x, 0.0f, 1e-5f);
    EXPECT_NEAR(moved.y - reflected.y, -1.0f, 1e-5f);
}
