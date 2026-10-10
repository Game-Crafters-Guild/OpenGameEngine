// Tile-list dispatch contract on the SSSR prefilter.
//
// The prefilter is the most expensive SSSR pass, and it used to run its dense
// 81-tap x 3-fetch loop over the whole screen — including sky and the
// roughness-rejected pixels the composite multiplies by zero. It now runs one
// workgroup per 8x8 tile that classify found at least one reflective pixel in,
// dispatched indirectly, with a surface early-out for the non-reflective pixels
// inside a listed tile.
//
// Two invariants carry the correctness of that change and neither is visible in a
// screenshot, which is why they are pinned here:
//
//  1. Every pixel of the prefilter target must be defined even though the pass
//     visits only listed tiles. Classify owns that zero. Without it the skipped
//     pixels are aliased transient RGBA16F — readily Inf/NaN — and the temporal
//     resolve's 5x5 neighbourhood reads them from reflective pixels one and two
//     texels inside the boundary.
//  2. The two indirect-args triples in one buffer are addressed by byte offset
//     from C++ into a std430 block declared in GLSL. A field inserted on one side
//     only would dispatch the prefilter off the ray count, which fails silently
//     in both directions (no tiles, or a wildly oversized dispatch).

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>

#include "SssrShaderSource.h"

using GE::Tests::ReadSssrNodeSource;
using GE::Tests::ReadSssrShaderSource;

namespace
{

// Field order of the shared IndirectArgsBuffer block, which is what the C++ byte
// offsets index into. std430 packs a uint array of scalars tightly, so a field's
// byte offset is its index times four. Each dispatch triple's fourth word carries
// that pass's append counter — vkCmdDispatchIndirect reads only three.
const char* const kArgsFields[] = {"rayGroupCountX",  "rayGroupCountY",  "rayGroupCountZ",
                                   "rayCount",        "tileGroupCountX", "tileGroupCountY",
                                   "tileGroupCountZ", "tileCount"};

} // namespace

// The dispatch is one-dimensional, so the workgroup's screen position has to come
// from the list. Reading gl_GlobalInvocationID under an indirect dispatch would
// filter a diagonal strip of the screen and leave the rest at zero.
TEST(SssrPrefilterTileDispatch, PrefilterTakesItsTileFromTheListNotTheGlobalId)
{
    const std::string source = ReadSssrShaderSource("ScreenSpaceReflections/sssr_prefilter.comp");
    ASSERT_FALSE(source.empty()) << "sssr_prefilter.comp not found via GE_RENDERER_REPO_ROOT";

    EXPECT_NE(source.find("TileList.tiles[tileIndex]"), std::string::npos)
        << "the prefilter must resolve its tile from the indirect tile list";
    EXPECT_NE(source.find("GE_SssrLinearGroupIndex(gl_WorkGroupID.xy)"), std::string::npos)
        << "the list index must be the workgroup's LINEAR position: a grid wide enough to need "
           "a second row would otherwise re-filter row 0's tiles and never touch the rest";
    EXPECT_EQ(source.find("ivec2 pixel = ivec2(gl_GlobalInvocationID.xy)"), std::string::npos)
        << "gl_GlobalInvocationID is meaningless under a 1D indirect dispatch: the pass would "
           "filter a strip and leave the rest of the screen unfiltered";
    EXPECT_NE(source.find("tile * ivec2(gl_WorkGroupSize.xy)"), std::string::npos)
        << "tile origin must scale by the workgroup size rather than a hard-coded 8";
}

// The early-out predicate must be the surface, never the centre's radiance alpha.
// A centre whose own ray missed with neighbours that hit is the reconstruction case
// the filter exists for; gating on alpha would punch holes in exactly the pixels it
// is supposed to fill.
TEST(SssrPrefilterTileDispatch, EarlyOutTestsTheSurfaceNotTheCentreRadianceAlpha)
{
    const std::string source = ReadSssrShaderSource("ScreenSpaceReflections/sssr_prefilter.comp");
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("if (!GE_SssrIsReflective(centerRawZ, GE_Roughness(centerNR)))"),
              std::string::npos)
        << "the prefilter must early-out on the shared reflection-eligibility predicate";

    // Locate the early-out and confirm nothing alpha-shaped gates it.
    const std::size_t at = source.find("GE_SssrIsReflective");
    ASSERT_NE(at, std::string::npos);
    const std::string guard = source.substr(at, source.find("return;", at) - at);
    EXPECT_EQ(guard.find(".a"), std::string::npos)
        << "gating the early-out on radiance alpha would skip the hole-filling this filter "
           "exists to do";
}

// Invariant 1. Classify is the sole writer of the prefilter target's cleared value.
TEST(SssrPrefilterTileDispatch, ClassifyDefinesEveryPixelOfThePrefilterTarget)
{
    const std::string classify = ReadSssrShaderSource("ScreenSpaceReflections/sssr_classify.comp");
    const std::string node = ReadSssrNodeSource();
    ASSERT_FALSE(classify.empty());
    ASSERT_FALSE(node.empty()) << "ScreenSpaceReflectionsNode.cpp not found";

    EXPECT_NE(classify.find("uniform writeonly image2D uPrefiltered"), std::string::npos)
        << "classify must bind the prefilter target to clear it";
    EXPECT_NE(classify.find("imageStore(uPrefiltered, pixel, vec4(0.0));"), std::string::npos)
        << "classify must zero the prefilter target: the prefilter visits only listed tiles, "
           "so every skipped pixel would otherwise be aliased transient memory in the temporal "
           "resolve's 5x5 neighbourhood";

    EXPECT_NE(node.find("p.Write(prefiltered, RenderGraph::RGTextureWrite::Storage);"),
              std::string::npos)
        << "the classify pass must declare the prefilter target as a write, or the graph "
           "inserts no barrier and may alias it out from under the clear";
    EXPECT_NE(node.find("\"uPrefiltered\""), std::string::npos)
        << "classify's descriptor set must bind the prefilter target by name";
}

// Invariant 2. The GLSL block layout and the C++ dispatch offsets must agree.
TEST(SssrPrefilterTileDispatch, TileArgsOffsetMatchesTheShaderBlockLayout)
{
    const std::string dispatch = ReadSssrShaderSource("ScreenSpaceReflections/sssr_dispatch.glsl");
    const std::string node = ReadSssrNodeSource();
    ASSERT_FALSE(dispatch.empty()) << "ScreenSpaceReflections/sssr_dispatch.glsl not found";
    ASSERT_FALSE(node.empty());

    const std::size_t blockAt = dispatch.find("buffer IndirectArgsBuffer");
    ASSERT_NE(blockAt, std::string::npos)
        << "IndirectArgsBuffer block missing from the shared dispatch include";
    const std::size_t blockEnd = dispatch.find("} Args;", blockAt);
    ASSERT_NE(blockEnd, std::string::npos);
    const std::string block = dispatch.substr(blockAt, blockEnd - blockAt);

    // Fields must appear in the order the byte offsets assume.
    std::size_t cursor = 0;
    std::size_t tileGroupXIndex = 0;
    for (std::size_t i = 0; i < std::size(kArgsFields); ++i)
    {
        const std::size_t at = block.find(kArgsFields[i], cursor);
        ASSERT_NE(at, std::string::npos)
            << "field " << kArgsFields[i] << " missing or out of order in IndirectArgsBuffer";
        cursor = at;
        if (std::string(kArgsFields[i]) == "tileGroupCountX")
            tileGroupXIndex = i;
    }
    ASSERT_EQ(tileGroupXIndex, 4u)
        << "tileGroupCountX must be the fifth uint of the block: it is what the tile dispatch "
           "offset addresses";

    // One declaration, not one per shader: a std430 offset mismatch between copies
    // dispatches a pass off the wrong counter and reports nothing.
    for (const char* const shader : {"ScreenSpaceReflections/sssr_classify.comp", "ScreenSpaceReflections/sssr_prepare_args.comp",
                                     "ScreenSpaceReflections/sssr_intersect.comp", "ScreenSpaceReflections/sssr_prefilter.comp"})
    {
        const std::string source = ReadSssrShaderSource(shader);
        ASSERT_FALSE(source.empty()) << shader;
        EXPECT_NE(source.find("#include \"ScreenSpaceReflections/sssr_dispatch.glsl\""), std::string::npos)
            << shader << " must take the args block from the shared include";
        EXPECT_EQ(source.find("buffer IndirectArgsBuffer"), std::string::npos)
            << shader << " must not re-declare the args block: two std430 declarations of one "
                         "buffer are exactly how the offsets drift apart";
    }

    // The C++ side must derive the offset from that index, and the buffer must be
    // large enough for both triples.
    EXPECT_NE(node.find("kTileArgsOffset = sizeof(uint32_t) * 4u"), std::string::npos)
        << "the tile dispatch offset must be four uints in, matching tileGroupCountX's position";
    EXPECT_NE(node.find("kArgsBytes = sizeof(uint32_t) * 8u"), std::string::npos)
        << "the args buffer must cover both dispatch triples";
    EXPECT_NE(node.find("DispatchIndirect(ctx.GetBuffer(indirectArgs), kTileArgsOffset)"),
              std::string::npos)
        << "the prefilter must dispatch indirectly at the tile args offset";

    // The offset is a multiple of 4, which vkCmdDispatchIndirect requires.
    constexpr std::size_t kTileArgsOffsetBytes = sizeof(std::uint32_t) * 4u;
    EXPECT_EQ(kTileArgsOffsetBytes % 4u, 0u);
    // And it must not overlap the ray triple it sits behind.
    EXPECT_GE(kTileArgsOffsetBytes, sizeof(std::uint32_t) * 3u);
}

// The tile list still gets exactly one entry per tile from one thread. The count is
// no longer the dispatch width — the grid is derived from it by the args-prep pass,
// which is what bounds it — so the invariant that remains is the append shape.
TEST(SssrPrefilterTileDispatch, OneAppendPerTileFromOneThread)
{
    const std::string classify = ReadSssrShaderSource("ScreenSpaceReflections/sssr_classify.comp");
    const std::string prefilter = ReadSssrShaderSource("ScreenSpaceReflections/sssr_prefilter.comp");
    ASSERT_FALSE(classify.empty());
    ASSERT_FALSE(prefilter.empty());

    // Appended by one thread of the group, gated on the group-wide reduction.
    EXPECT_NE(classify.find("if (gl_LocalInvocationIndex == 0u && sTileHasReflectivePixel != 0u)"),
              std::string::npos)
        << "the tile append must happen once per tile from a single thread, not once per pixel";
    EXPECT_NE(classify.find("uint slot = atomicAdd(Args.tileCount, 1u);"), std::string::npos)
        << "the tile append must claim its slot from tileCount";

    // Classify's group and the prefilter's group must be the same shape, or one
    // list entry stops corresponding to one workgroup's worth of pixels.
    const char* const kGroup = "layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;";
    EXPECT_NE(classify.find(kGroup), std::string::npos);
    EXPECT_NE(prefilter.find(kGroup), std::string::npos)
        << "prefilter workgroup shape must match the tile shape classify appends";
}

// The bounds test must gate work rather than return, because the barriers backing
// the tile reduction have to be reached by every thread of the group. A `return`
// ahead of a barrier is non-uniform control flow: undefined behaviour, and in
// practice a hang or a dropped tile on partial edge groups.
TEST(SssrPrefilterTileDispatch, ClassifyReachesItsBarriersInUniformControlFlow)
{
    const std::string classify = ReadSssrShaderSource("ScreenSpaceReflections/sssr_classify.comp");
    ASSERT_FALSE(classify.empty());

    const std::size_t firstBarrier = classify.find("barrier();");
    ASSERT_NE(firstBarrier, std::string::npos) << "tile reduction needs a workgroup barrier";

    const std::size_t mainAt = classify.find("void main()");
    ASSERT_NE(mainAt, std::string::npos);
    const std::string beforeBarrier = classify.substr(mainAt, firstBarrier - mainAt);
    // The statement, not the word: prose above the bounds test legitimately mentions
    // returning early, and main() is void so a bare `return;` is the only form.
    EXPECT_EQ(beforeBarrier.find("return;"), std::string::npos)
        << "no thread may return before the barrier: non-uniform control flow at a workgroup "
           "barrier is undefined behaviour";
    EXPECT_NE(classify.find("bool inBounds = all(lessThan(pixel, size));"), std::string::npos)
        << "the bounds test must produce a flag that gates work, not an early return";

    // Two barriers: one after the shared flag is initialised, one after every
    // thread has contributed to it.
    const std::size_t secondBarrier = classify.find("barrier();", firstBarrier + 1);
    EXPECT_NE(secondBarrier, std::string::npos)
        << "the reduction needs a second barrier before the flag is read, or the append races "
           "the contributions";
}

// The three passes that must agree on which pixels matter have to share one
// predicate. Divergent copies are how a denoiser starts skipping pixels the
// composite still shows, or filtering pixels it discards.
TEST(SssrPrefilterTileDispatch, EligibilityPredicateIsSharedNotCopied)
{
    const std::string common = ReadSssrShaderSource("ScreenSpaceReflections/sssr_common.glsl");
    const std::string classify = ReadSssrShaderSource("ScreenSpaceReflections/sssr_classify.comp");
    const std::string prefilter = ReadSssrShaderSource("ScreenSpaceReflections/sssr_prefilter.comp");
    const std::string composite = ReadSssrShaderSource("ScreenSpaceReflections/sssr_composite.comp");
    ASSERT_FALSE(common.empty());
    ASSERT_FALSE(classify.empty());
    ASSERT_FALSE(prefilter.empty());
    ASSERT_FALSE(composite.empty());

    EXPECT_NE(common.find("bool GE_SssrIsReflective(float rawDepth, float roughness)"),
              std::string::npos)
        << "the shared eligibility predicate must live in the shared include";
    EXPECT_NE(common.find("const float kSssrMaxRoughness = 0.92;"), std::string::npos);
    EXPECT_NE(common.find("const float kSssrMinRawDepth = 1e-7;"), std::string::npos);

    EXPECT_NE(classify.find("GE_SssrIsReflective(depth, roughness)"), std::string::npos)
        << "classify must trace exactly the shared set";
    EXPECT_NE(prefilter.find("GE_SssrIsReflective("), std::string::npos)
        << "the prefilter must skip exactly the complement of the shared set";

    // The composite's fade must reach zero at the same threshold, which is what
    // makes skipping those pixels invisible rather than merely cheap.
    EXPECT_NE(composite.find("smoothstep(0.65, kSssrMaxRoughness, roughness)"),
              std::string::npos)
        << "the composite's roughness fade must reach zero at kSssrMaxRoughness, or the "
           "prefilter would be skipping pixels the frame still shows";

    // No pass may re-hardcode the thresholds the predicate owns.
    EXPECT_EQ(classify.find("roughness >= 0.92"), std::string::npos)
        << "classify must not keep a private copy of the roughness reject";
    EXPECT_EQ(composite.find("smoothstep(0.65, 0.92,"), std::string::npos)
        << "the composite must not keep a private copy of the roughness threshold";
}
