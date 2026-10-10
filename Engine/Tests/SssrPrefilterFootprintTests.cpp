// Footprint contract on the SSSR prefilter's groupshared tile.
//
// The prefilter loads a neighbourhood into LDS once and takes a sparse, rotated,
// variance-scaled tap set out of it. Three things about that are load-bearing and
// invisible in a screenshot:
//
//  1. THE APRON BOUNDS THE FOOTPRINT. Every tap indexes the LDS tile directly. A
//     tap set whose widest member exceeds the apron would clamp — silently
//     folding the outer ring of the kernel onto the tile edge, which is a
//     directional bias, not a smaller filter. The bound is arithmetic between
//     three constants and one literal table, so it is checked as arithmetic.
//
//  2. THE READ FOOTPRINT LEAVES THE TILE; THE WRITE SET DOES NOT. The apron
//     reaches into tiles the denoise tile list need not contain. That is safe
//     only while every input the pass reads is defined over the WHOLE frame by a
//     full-screen writer. Classify owns that for radiance and for the prefilter's
//     own target; reproject owns it for the average-radiance reference, and it
//     owns it *because its dispatch is full-screen*. Move that reduction onto the
//     tile list and boundary tiles start reading undefined transient RGBA16F.
//
//  3. THE DRIVE IS HYBRID. The measured contact fringe is a static geometric
//     alias: a perfect 8-frame mean removes 0.3% of it, so temporal variance
//     arrives at that pixel near zero. A temporal-only drive — which is what
//     upstream FidelityFX uses — would pin the filter at its bias floor exactly
//     where it is needed. The spatial term is what makes it act.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "SssrShaderSource.h"

using GE::Tests::ReadSssrNodeSource;
using GE::Tests::ReadSssrShaderSource;

namespace
{

// Value of a GLSL `const <type> <name> = <number>;` declaration. Returns nothing
// when the initialiser is not a bare literal, so a test asserting on a number
// cannot silently pass against an expression it did not evaluate.
std::optional<double> ParseLiteralConstant(const std::string& source, const std::string& name)
{
    const std::size_t at = source.find(name + " = ");
    if (at == std::string::npos)
        return std::nullopt;
    const std::size_t valueAt = at + name.size() + 3;
    const std::size_t end = source.find(';', valueAt);
    if (end == std::string::npos)
        return std::nullopt;
    const std::string text = source.substr(valueAt, end - valueAt);
    try
    {
        std::size_t consumed = 0;
        const double value = std::stod(text, &consumed);
        while (consumed < text.size() && std::isspace(static_cast<unsigned char>(text[consumed])))
            ++consumed;
        if (consumed != text.size())
            return std::nullopt;
        return value;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

// Body of the named pass's builder lambda in the node, i.e. the resource
// declarations between `d.PassName("<pass>")` and the end of that builder.
std::string PassBuilderBody(const std::string& node, const std::string& passName)
{
    const std::size_t at = node.find("d.PassName(\"" + passName + "\")");
    if (at == std::string::npos)
        return {};
    const std::size_t end = node.find("        },", at);
    if (end == std::string::npos)
        return {};
    return node.substr(at, end - at);
}

} // namespace

// Invariant 1, as arithmetic. Every tap indexes the LDS tile directly, so the
// widest one has to land inside it. Tying the apron to the footprint radius is
// what makes that true by construction rather than by review.
TEST(SssrPrefilterFootprint, WidestTapLandsInsideTheGroupsharedApron)
{
    const std::string prefilter = ReadSssrShaderSource("ScreenSpaceReflections/sssr_prefilter.comp");
    ASSERT_FALSE(prefilter.empty()) << "sssr_prefilter.comp not found via GE_RENDERER_REPO_ROOT";

    const auto narrow = ParseLiteralConstant(prefilter, "kRadiusNarrow");
    const auto aliased = ParseLiteralConstant(prefilter, "kRadiusAliased");
    ASSERT_TRUE(narrow.has_value()) << "kRadiusNarrow must be a literal";
    ASSERT_TRUE(aliased.has_value()) << "kRadiusAliased must be a literal";
    EXPECT_NEAR(*narrow, 2.0, 1e-9) << "the narrow end is the radius this pass used on a mirror";
    EXPECT_NEAR(*aliased, 6.0, 1e-9)
        << "the wide end is the reach a dense-box prototype measured on this fringe";

    EXPECT_NE(prefilter.find("const int kTileApron = kRadiusAliased;"), std::string::npos)
        << "the apron must BE the widest radius, not a separately tuned number: every tap reads "
           "the LDS tile directly, so a footprint wider than the apron reads the wrong texels";
    EXPECT_NE(prefilter.find("for (int y = -kRadiusAliased; y <= kRadiusAliased; ++y)"),
              std::string::npos)
        << "the loop must be bounded by the same constant the apron is";

    // The sparse tap set upstream ships is deliberately absent: measured on this
    // artifact, a subsampled kernel re-samples a static alias instead of
    // low-passing it, and both rotated variants were worse than the dense filter.
    EXPECT_EQ(prefilter.find("kHaltonOffsets"), std::string::npos)
        << "the sparse Halton tap set was measured to regress this artifact and must not return "
           "without new evidence";
    EXPECT_EQ(prefilter.find("uBlueNoise"), std::string::npos)
        << "the tap rotation went with the sparse set; a dense footprint needs no jitter, and a "
           "binding nothing reads is dead weight in the descriptor set";
}

// The tile is four uint arrays over (8 + 2*apron)^2 texels. Groupshared memory is
// what caps how many of these groups a CU can hold, and Vulkan only guarantees
// 16 KB, so the budget is pinned rather than left to whoever next widens the
// apron.
TEST(SssrPrefilterFootprint, GroupsharedTileFitsTheGuaranteedComputeLimit)
{
    const std::string prefilter = ReadSssrShaderSource("ScreenSpaceReflections/sssr_prefilter.comp");
    ASSERT_FALSE(prefilter.empty());

    const auto apron = ParseLiteralConstant(prefilter, "kRadiusAliased");
    ASSERT_TRUE(apron.has_value());

    EXPECT_NE(prefilter.find("kTileSpan = 8 + 2 * kTileApron"), std::string::npos)
        << "the tile span must be the workgroup plus two aprons, or taps read the wrong texels";

    const long long span = 8 + 2 * static_cast<long long>(*apron);
    // Four packed uint arrays: radiance.rg, radiance.ba, the octahedral normal,
    // and (roughness, rawDepth).
    constexpr long long kArrays = 4;
    const long long bytes = kArrays * span * span * 4;
    // VkPhysicalDeviceLimits::maxComputeSharedMemorySize floor.
    constexpr long long kGuaranteedSharedMemory = 16384;
    EXPECT_LE(bytes, kGuaranteedSharedMemory)
        << "groupshared tile is " << bytes << " B for span " << span;

    std::size_t arrayCount = 0;
    for (std::size_t at = prefilter.find("shared uint sTile"); at != std::string::npos;
         at = prefilter.find("shared uint sTile", at + 1))
        ++arrayCount;
    EXPECT_EQ(arrayCount, static_cast<std::size_t>(kArrays))
        << "the byte budget above counts " << kArrays << " arrays; the shader declares "
        << arrayCount;
}

// Invariant 2. The average-radiance reference is produced by the FULL-SCREEN
// reproject dispatch, and the prefilter reads it across tile boundaries.
TEST(SssrPrefilterFootprint, AverageRadianceIsProducedFullScreenNotOverTheTileList)
{
    const std::string node = ReadSssrNodeSource();
    ASSERT_FALSE(node.empty()) << "ScreenSpaceReflectionsNode.cpp not found";

    const std::string reproject = PassBuilderBody(node, "Reproject");
    const std::string prefilter = PassBuilderBody(node, "Prefilter");
    ASSERT_FALSE(reproject.empty()) << "Reproject pass builder not located";
    ASSERT_FALSE(prefilter.empty()) << "Prefilter pass builder not located";

    EXPECT_NE(reproject.find("p.Write(avgRadiance"), std::string::npos)
        << "reproject must own the average-radiance write";
    EXPECT_EQ(prefilter.find("p.Write(avgRadiance"), std::string::npos)
        << "the prefilter must not write the average-radiance target: it is dispatched over the "
           "tile list, so the texels it skipped would stay undefined and its own apron reads "
           "them";
    EXPECT_NE(prefilter.find("p.Read(avgRadiance"), std::string::npos)
        << "the prefilter must declare the average-radiance read, or the graph inserts no "
           "barrier between the reduction and this sample";

    // Reproject stays a direct, whole-frame dispatch. An indirect dispatch here
    // would leave average-radiance texels undefined for unlisted tiles.
    const std::size_t reprojectAt = node.find("d.PassName(\"Reproject\")");
    const std::size_t prefilterAt = node.find("d.PassName(\"Prefilter\")");
    ASSERT_NE(reprojectAt, std::string::npos);
    ASSERT_NE(prefilterAt, std::string::npos);
    ASSERT_LT(reprojectAt, prefilterAt) << "reproject must run before the prefilter reads it";
    const std::string reprojectPass = node.substr(reprojectAt, prefilterAt - reprojectAt);
    EXPECT_NE(reprojectPass.find("ctx.Cmd->Dispatch(gx, gy, 1)"), std::string::npos)
        << "the reduction that defines every average-radiance texel must be a full-screen "
           "dispatch";
    EXPECT_EQ(reprojectPass.find("DispatchIndirect"), std::string::npos)
        << "an indirect reproject would leave average-radiance texels undefined for the tiles it "
           "skipped, and the prefilter's apron reads across tile boundaries";
}

// The block reference is stored premultiplied by confidence and divided after the
// bilinear fetch. Storing an already-divided mean and interpolating THAT would
// weight a block with one traced pixel the same as a fully traced one, which on a
// rate-limited surface is most blocks.
TEST(SssrPrefilterFootprint, BlockReferenceIsPremultipliedAndDividedAfterInterpolation)
{
    const std::string reproject = ReadSssrShaderSource("ScreenSpaceReflections/sssr_reproject.comp");
    const std::string prefilter = ReadSssrShaderSource("ScreenSpaceReflections/sssr_prefilter.comp");
    ASSERT_FALSE(reproject.empty());
    ASSERT_FALSE(prefilter.empty());

    EXPECT_NE(reproject.find("vec4(current.rgb * blockWeight, blockWeight)"), std::string::npos)
        << "the block store must be premultiplied: radiance*confidence in rgb, confidence in a";
    EXPECT_NE(prefilter.find("avg.rgb / max(avg.a, kConfidenceEpsilon)"), std::string::npos)
        << "the prefilter must divide after the bilinear fetch, not before it";

    // A non-finite sample is selected away rather than multiplied by zero: Inf*0
    // and NaN*0 are both NaN, which is how one bad texel reaches a whole block
    // and, through the bilinear read, its neighbours.
    EXPECT_NE(reproject.find("blockUsable ? vec4(current.rgb * blockWeight, blockWeight) : vec4(0.0)"),
              std::string::npos)
        << "the non-finite guard must select, not scale";
    EXPECT_NE(reproject.find("inBounds && GE_SssrIsFinite(current)"), std::string::npos)
        << "the reduction must reject non-finite samples";

    // And the drive must not carry a non-finite value into the tap offsets, which
    // index groupshared memory directly.
    EXPECT_NE(prefilter.find("drive > kDriveOnset ? smoothstep("), std::string::npos)
        << "the drive's threshold comparison is also its non-finite trap: NaN > x is false, so "
           "the filter falls back to its narrow footprint instead of indexing LDS with garbage";
}

// The reduction added to reproject put barriers in a shader that used to return
// early on its bounds test. A `return` ahead of a barrier is non-uniform control
// flow: undefined behaviour, and in practice a hang or a corrupted block average
// on the partial groups at the right and bottom edges.
TEST(SssrPrefilterFootprint, ReprojectReachesItsBarriersInUniformControlFlow)
{
    const std::string reproject = ReadSssrShaderSource("ScreenSpaceReflections/sssr_reproject.comp");
    ASSERT_FALSE(reproject.empty());

    const std::size_t mainAt = reproject.find("void main()");
    ASSERT_NE(mainAt, std::string::npos);
    const std::size_t firstBarrier = reproject.find("barrier();", mainAt);
    ASSERT_NE(firstBarrier, std::string::npos) << "the block reduction needs a workgroup barrier";

    const std::string beforeBarrier = reproject.substr(mainAt, firstBarrier - mainAt);
    EXPECT_EQ(beforeBarrier.find("return;"), std::string::npos)
        << "no thread may return before the barrier: the partial edge groups must still "
           "contribute their zero weight to the block reduction";
    EXPECT_NE(reproject.find("bool inBounds = all(lessThan(pixel, size));"), std::string::npos)
        << "the bounds test must produce a flag that gates the stores, not an early return";
}

// Invariant 3. Both drive terms are present, both are dimensionless, and the
// filter's reach and amplitude are both taken from their maximum.
TEST(SssrPrefilterFootprint, DriveIsTheMaxOfTemporalAndSpatialVariation)
{
    const std::string prefilter = ReadSssrShaderSource("ScreenSpaceReflections/sssr_prefilter.comp");
    ASSERT_FALSE(prefilter.empty());

    EXPECT_NE(prefilter.find("max(temporalCoV, spatialCoV)"), std::string::npos)
        << "the drive must be the maximum of the two terms: the measured contact fringe is 99.7% "
           "temporally irreducible, so a temporal-only drive arrives there at its bias floor";

    // Both terms are coefficients of variation, so one calibration pair covers
    // any exposure and the two are comparable at all.
    EXPECT_NE(prefilter.find("sqrt(temporalVariance) / max(moments.r"), std::string::npos)
        << "the temporal term must be normalised by its own mean, or its calibration is tied to "
           "one scene's brightness";
    EXPECT_NE(prefilter.find("max(localMean, kDriveEpsilon)"), std::string::npos)
        << "the spatial term must be normalised by its own mean for the same reason";

    // The two thresholds were calibrated on a dense 5x5 of the traced radiance. A
    // wider estimator reads systematically higher on legitimately detailed
    // reflections, so it would engage on the clean floor and cost sharpness there.
    const auto window = ParseLiteralConstant(prefilter, "kDriveWindow");
    ASSERT_TRUE(window.has_value());
    EXPECT_NEAR(*window, 2.0, 1e-9)
        << "the spatial estimator must stay the 5x5 window kDriveOnset/kDriveSaturation were "
           "calibrated on";

    // Untraced pixels read zero radiance. Counting them would report every
    // rate-limited rough surface as aliased and widen the filter everywhere.
    EXPECT_NE(prefilter.find("step(kConfidenceEpsilon, ba.y)"), std::string::npos)
        << "the spatial estimator must gate on confidence";

    // Reach and amplitude both follow the drive.
    EXPECT_NE(prefilter.find("mix(float(kRadiusNarrow), float(kRadiusAliased), aliasing)"),
              std::string::npos)
        << "the footprint radius must follow the drive";
    EXPECT_NE(prefilter.find("mix(kNeighborWeightFloor, 1.0, aliasing)"), std::string::npos)
        << "the neighbour amplitude must follow the drive";
}

// Sample scarcity is the second driver, and it is the one a variance estimate
// cannot supply. Where the rate control traced 1 pixel in 16 the estimator's
// window holds one sample, whose variance is zero by construction — so a
// variance-only drive would hand a rate-limited rough surface the narrowest,
// lowest-amplitude filter, and the untraced centre's zero radiance would carry a
// third of the result. Coverage measures the tracing lattice directly.
TEST(SssrPrefilterFootprint, ScarcityAlsoDrivesTheFootprint)
{
    const std::string prefilter = ReadSssrShaderSource("ScreenSpaceReflections/sssr_prefilter.comp");
    ASSERT_FALSE(prefilter.empty());

    EXPECT_NE(prefilter.find("float coverage = localWeight / kDriveSamples;"), std::string::npos)
        << "coverage must come from the same confidence-gated window as the variance";
    EXPECT_NE(prefilter.find("max(variation, 1.0 - coverage)"), std::string::npos)
        << "the footprint must widen on scarcity as well as on variance";

    // Coverage measuring the lattice is what retired the roughness branch. Two
    // mechanisms for one job is how they drift apart.
    EXPECT_EQ(prefilter.find("kRoughLobeRoughness"), std::string::npos)
        << "the roughness threshold this pass used to branch on is subsumed by coverage; keeping "
           "both leaves two mechanisms deciding one thing";
}

// The drive is compared in coefficient of variation, NOT in cv squared. Squaring
// both thresholds looks like the same curve and is not one — a smoothstep is not
// invariant under squaring its argument and its bounds. At the band's measured cv
// of 0.128 the squared form engages 0.80 where this one engages 0.91, which is the
// difference between filtering at radius 5 and at radius 6, and measured as the
// difference between missing the quality floor and clearing it.
TEST(SssrPrefilterFootprint, DriveIsCalibratedInCoefficientOfVariationNotItsSquare)
{
    const std::string prefilter = ReadSssrShaderSource("ScreenSpaceReflections/sssr_prefilter.comp");
    ASSERT_FALSE(prefilter.empty());

    const auto onset = ParseLiteralConstant(prefilter, "kDriveOnset");
    const auto saturation = ParseLiteralConstant(prefilter, "kDriveSaturation");
    ASSERT_TRUE(onset.has_value());
    ASSERT_TRUE(saturation.has_value());
    EXPECT_NEAR(*onset, 0.03, 1e-9) << "the prototype's engagement point, in cv";
    EXPECT_NEAR(*saturation, 0.15, 1e-9) << "the prototype's saturation point, in cv";

    // Both terms must be square-rooted into cv before the comparison.
    EXPECT_NE(prefilter.find("sqrt(max(localMeanSq - localMean * localMean, 0.0)) /"),
              std::string::npos)
        << "the spatial term must be a standard deviation over a mean, not a variance over a "
           "mean squared";
    EXPECT_NE(prefilter.find("sqrt(temporalVariance) / max(moments.r, kDriveEpsilon)"),
              std::string::npos)
        << "the temporal term must be in the same cv units as the spatial one, or max() compares "
           "two different quantities";
    EXPECT_EQ(prefilter.find("CoV2"), std::string::npos)
        << "no term may remain in squared units: the calibration points are cv";
}

// Our edge stops, not upstream's. FFX's depth sigma is 1/(4z) — it tightens as
// the surface recedes, where ours is a fixed fraction of linear depth — and its
// normal exponent of 512 assumes geometrically smooth G-buffer normals. Both
// would shut this filter at grazing incidence on a normal-mapped floor, which is
// exactly the case it exists for.
TEST(SssrPrefilterFootprint, KeepsScaleInvariantDepthStopAndNormalMapTolerantNormalStop)
{
    const std::string prefilter = ReadSssrShaderSource("ScreenSpaceReflections/sssr_prefilter.comp");
    ASSERT_FALSE(prefilter.empty());

    EXPECT_NE(prefilter.find("max(centerLinZ * kDepthSigmaRelative, kDepthSigmaFloor)"),
              std::string::npos)
        << "the depth sigma must stay proportional to linear depth";
    const auto depthSigma = ParseLiteralConstant(prefilter, "kDepthSigmaRelative");
    ASSERT_TRUE(depthSigma.has_value());
    EXPECT_NEAR(*depthSigma, 0.05, 1e-9) << "the 5%-of-depth sigma is the calibrated value";

    const auto normalSigma = ParseLiteralConstant(prefilter, "kNormalSigma");
    ASSERT_TRUE(normalSigma.has_value());
    EXPECT_NEAR(*normalSigma, 32.0, 1e-9)
        << "32 keeps an 8-degree normal deviation at 0.72; upstream's 512 puts it at 0.006, which "
           "makes the filter same-normal-only and useless on normal-mapped surfaces";

    // The one upstream form that must not appear: a depth weight that multiplies
    // by depth instead of dividing by it.
    EXPECT_EQ(prefilter.find("* centerLinZ * 4.0"), std::string::npos)
        << "FFX's exp(-|dz|*z*4) depth stop is scale-dependent and ~20x stricter at 10 m";
}

// Confidence is our channel and upstream's weight expression has no slot for it.
// It must weight every tap — an untraced or missed neighbour contributes nothing
// — and it must survive into the filtered alpha, which the temporal hold and the
// composite both key on.
TEST(SssrPrefilterFootprint, ConfidenceWeightsEveryTapAndSurvivesIntoTheOutput)
{
    const std::string prefilter = ReadSssrShaderSource("ScreenSpaceReflections/sssr_prefilter.comp");
    ASSERT_FALSE(prefilter.empty());

    EXPECT_NE(prefilter.find("q.Confidence;"), std::string::npos)
        << "every tap's weight must include its confidence";
    EXPECT_NE(prefilter.find("vec4(q.Radiance, q.Confidence) * w"), std::string::npos)
        << "confidence must accumulate alongside radiance so the filtered alpha stays meaningful";
    EXPECT_NE(prefilter.find("max(center.Confidence, kCenterConfidenceFloor)"), std::string::npos)
        << "an untraced centre must still anchor its own pixel slightly, which is what the "
           "floor is for";
}

// One block size spans three files: the reproject workgroup that reduces to a
// texel, the prefilter's sample of that texel, and the C++ extent of the target.
// Two of the three agreeing is a silently misaligned reference.
TEST(SssrPrefilterFootprint, BlockSizeIsOneSharedConstantAcrossShadersAndNode)
{
    const std::string common = ReadSssrShaderSource("ScreenSpaceReflections/sssr_common.glsl");
    const std::string reproject = ReadSssrShaderSource("ScreenSpaceReflections/sssr_reproject.comp");
    const std::string prefilter = ReadSssrShaderSource("ScreenSpaceReflections/sssr_prefilter.comp");
    const std::string node = ReadSssrNodeSource();
    ASSERT_FALSE(common.empty());
    ASSERT_FALSE(reproject.empty());
    ASSERT_FALSE(prefilter.empty());
    ASSERT_FALSE(node.empty());

    const auto block = ParseLiteralConstant(common, "kSssrAvgRadianceBlock");
    ASSERT_TRUE(block.has_value()) << "the block size must live in the shared include";
    EXPECT_NEAR(*block, 8.0, 1e-9)
        << "the block must equal the 8x8 workgroup every SSSR pass dispatches";

    EXPECT_NE(reproject.find("kBlockThreads = kSssrAvgRadianceBlock * kSssrAvgRadianceBlock"),
              std::string::npos)
        << "the reduction's width must come from the shared constant, not a second literal";
    EXPECT_NE(prefilter.find("float(kSssrAvgRadianceBlock) * vec2(textureSize(uAvgRadiance, 0))"),
              std::string::npos)
        << "the prefilter's sample position must come from the same constant, or the reference is "
           "offset by a fraction of a block";
    EXPECT_NE(node.find("constexpr uint32_t kDenoiseTileSize = 8u;"), std::string::npos)
        << "the C++ side sizes the target and the tile grid from the same number";
}
