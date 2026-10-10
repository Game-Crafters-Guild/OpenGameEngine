// Depth-proxy parallax correction in the DDGI reflection gather
// (Includes/ddgi_probes.glsl), pinned against the shader source because the
// whole feature lives in GLSL with no CPU surface to link against — the same
// artifact-under-test approach as ShadowSamplingShaderContractTests and
// DDGIGlossyAtlasLayoutTests.
//
// Two properties are worth a gate here, and neither is legible in a screenshot
// as anything but "the reflection looks a bit off":
//
//   * The eight probes of a gather must aim at a SHARED hit proxy rather than
//     each sampling its own atlas in the same world direction from a different
//     origin. Dropping the correction is silent: reflections stay lit, they
//     just smear across silhouettes. So the atlas taps must use the corrected
//     direction, and the sphere intersection must start from the UNBIASED
//     receiver position — the grid bias exists to move the probe lookup, and
//     letting it move the ray origin moves the proxy hit with it.
//
//   * The sharp lobe weights each tap by how far the proxy supported the
//     reprojection. That factor only does anything because it VARIES per tap;
//     a constant would cancel in the normalized gather. The floor is therefore
//     pinned as a named constant rather than a literal, and the rough lobe is
//     pinned as NOT taking it (a broad power-8 lobe averages the same error
//     away, and paying confidence twice would double-count it).

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{

std::string ReadShaderSource(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file)
        return {};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string ProbesSource()
{
#ifdef GE_RENDERER_REPO_ROOT
    return ReadShaderSource(std::filesystem::path(GE_RENDERER_REPO_ROOT) /
                            "Engine/Modules/Rendering/Shaders/Includes/ddgi_probes.glsl");
#else
    return {};
#endif
}

}  // namespace

TEST(DDGIReflectionParallaxTests, ForwardShadingUsesOnlyTheScreenSpaceReflectionResolve)
{
#ifndef GE_RENDERER_REPO_ROOT
    GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined (dev-only shader-source anchor)";
#else
    const std::string source = ReadShaderSource(
        std::filesystem::path(GE_RENDERER_REPO_ROOT) /
        "Engine/Modules/Rendering/Shaders/Includes/ibl.glsl");
    ASSERT_FALSE(source.empty());
    EXPECT_EQ(source.find("GE_SampleDDGIReflectionBoth("), std::string::npos)
        << "an inline reflection gather makes every forward DDGI variant pay its register "
           "cost even when no fragment takes the branch";
    EXPECT_EQ(source.find("GE_SampleDDGIReflection("), std::string::npos);
    EXPECT_NE(source.find("textureLod(ge_ddgiResolveRough,"), std::string::npos);
    EXPECT_NE(source.find("textureLod(ge_ddgiResolveGlossy,"), std::string::npos);
#endif
}

TEST(DDGIReflectionParallaxTests, TrustConstantsMatchTheReferenceLibrary)
{
#ifndef GE_RENDERER_REPO_ROOT
    GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined (dev-only shader-source anchor)";
#else
    const std::string source = ProbesSource();
    ASSERT_FALSE(source.empty());

    // ROUGH_PARALLAX_VAR_START / VAR_END / INSIDE_FADE in the ported reference.
    EXPECT_NE(source.find("const float GE_DDGI_PARALLAX_VAR_START = 0.02;"), std::string::npos);
    EXPECT_NE(source.find("const float GE_DDGI_PARALLAX_VAR_END = 0.20;"), std::string::npos);
    EXPECT_NE(source.find("const float GE_DDGI_PARALLAX_INSIDE_FADE = 0.12;"), std::string::npos);
    EXPECT_NE(source.find("const float GE_DDGI_PARALLAX_MIN_CONFIDENCE = 0.2;"), std::string::npos);
#endif
}

TEST(DDGIReflectionParallaxTests, BothLobesSampleTheCorrectedDirection)
{
#ifndef GE_RENDERER_REPO_ROOT
    GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined (dev-only shader-source anchor)";
#else
    const std::string source = ProbesSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("GE_DDGIProbeTexelUV(grid, probeIdx, sampleDir)"), std::string::npos)
        << "the rough lobe no longer follows the parallax-corrected direction; the eight taps "
        << "of a gather would each sample their own origin's world direction again";
    EXPECT_NE(source.find("glossyAtlasSize, sampleDir)"), std::string::npos)
        << "the glossy lobe no longer follows the parallax-corrected direction";
#endif
}

TEST(DDGIReflectionParallaxTests, TheProxyRayStartsAtTheUnbiasedReceiverPosition)
{
#ifndef GE_RENDERER_REPO_ROOT
    GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined (dev-only shader-source anchor)";
#else
    const std::string source = ProbesSource();
    ASSERT_FALSE(source.empty());

    // `biasedPos` is the grid-lookup position. Handing it to the intersection
    // would slide the proxy hit by the bias, which is a silent, direction-
    // dependent shift of every reflection.
    EXPECT_NE(source.find("GE_DDGIParallaxCorrect(reflectMoments, posWS, probePosWS, reflectDir"),
              std::string::npos)
        << "the parallax ray no longer starts at the unbiased shading position";
#endif
}

TEST(DDGIReflectionParallaxTests, OnlyTheSharpLobeWeightsByParallaxConfidence)
{
#ifndef GE_RENDERER_REPO_ROOT
    GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined (dev-only shader-source anchor)";
#else
    const std::string source = ProbesSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("mix(GE_DDGI_PARALLAX_MIN_CONFIDENCE, 1.0, parallaxWeight)"),
              std::string::npos)
        << "the sharp lobe no longer prefers probes whose depth proxy supports the reprojection";

    // The rough accumulator must stay on the plain squared weight.
    EXPECT_NE(source.find("roughWsum += reflectionWeight;"), std::string::npos)
        << "the rough lobe picked up the glossy lobe's confidence weighting";
    EXPECT_NE(source.find("glossyWsum += glossyWeight;"), std::string::npos)
        << "the glossy accumulator and its weight sum disagree, so the lobe no longer normalizes "
        << "by the weights it actually applied";
#endif
}

TEST(DDGIReflectionParallaxTests, CascadedReflectionsBlendWithTheSameFeatherAsIrradiance)
{
#ifndef GE_RENDERER_REPO_ROOT
    GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined (dev-only shader-source anchor)";
#else
    const std::string source = ProbesSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("layout(set = 0, binding = 39) uniform sampler2D ge_ddgiRoughAtlasFine;"),
              std::string::npos)
        << "C1 rough atlas is no longer a dedicated binding; the world pass cannot feed it";
    EXPECT_NE(source.find("layout(set = 0, binding = 40) uniform sampler2D ge_ddgiGlossyAtlasFine;"),
              std::string::npos)
        << "C1 glossy atlas is no longer a dedicated binding; the world pass cannot feed it";

    EXPECT_NE(source.find("GE_DDGIGatherReflectionC1(posWS, reflectDir, true, true, roughFine, "
                          "glossyFine);"),
              std::string::npos)
        << "the fine cascade's reflection gather left the cascade blend";
    EXPECT_NE(source.find("GE_DDGIFineCascadeWeight(posWS)"), std::string::npos)
        << "cascaded reflections no longer share irradiance's spatial feather; a different blend "
           "band would disagree about which cascade owns a pixel";
    EXPECT_NE(source.find("roughOut = mix(roughOut, roughFine, c1Weight);"), std::string::npos);
    EXPECT_NE(source.find("glossyOut = mix(glossyOut, glossyFine, c1Weight);"), std::string::npos);

    // The gather must stay out of reach of a forward fragment: a per-fragment
    // entry point is what pulled two copies of this loop into the world
    // shader, costing ~5 ms/frame on the Sponza parity scene in register
    // pressure alone (measured with a build no fragment could reach).
    EXPECT_EQ(source.find("vec4 GE_SampleDDGIReflection("), std::string::npos)
        << "a crossfaded per-fragment entry point is back; forward shaders will pull the whole "
           "8-probe loop in with it";

    EXPECT_NE(source.find("ge_ddgiProbeStateFine[probeIdx]"), std::string::npos)
        << "C1 reflections no longer read C1 probe state";
    EXPECT_NE(source.find("ge_ddgiDepthAtlasFine"), std::string::npos)
        << "C1 reflections no longer parallax-correct against C1's depth atlas";
    EXPECT_NE(source.find("ge_ddgiRoughAtlasFine"), std::string::npos);
    EXPECT_NE(source.find("ge_ddgiGlossyAtlasFine"), std::string::npos);

    EXPECT_NE(source.find("wantGlossy = wantGlossy && glossyTilesX > 0;"), std::string::npos)
        << "a cascade whose sharp lobe was refused (TilesX == 0) would still tap a black atlas";
#endif
}
