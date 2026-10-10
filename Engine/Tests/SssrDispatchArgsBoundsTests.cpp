// Indirect-dispatch width bounds on the SSSR ray and denoise-tile dispatches.
//
// Both dispatch widths used to be raw counts: ceil(rayCount / 64), maintained by an
// atomicMax once per traced pixel, and the denoise-tile append counter used directly
// as groupCountX. A count is not a legal dispatch width. maxComputeWorkGroupCount[0]
// is only guaranteed to be 65535, and both counts reach 129,600 on a fully
// reflective 4K frame — past which a driver reporting the minimum silently drops the
// excess workgroups. There is no validation error for it: the frame simply stops
// having reflections partway through, which is the worst possible failure shape.
//
// A one-thread args-prep pass now derives both grids from the final counts, bounding
// the width and spilling the remainder into Y. Two properties carry that:
//
//   * The spill is EXACT, never a clamp. Clamping at 65535 would trade a silent
//     driver drop for a silent engine drop.
//   * Below the limit the grid is (count, 1) and the linear index is
//     gl_WorkGroupID.x, so nothing changes at any resolution shipping today. That
//     is what makes the change correctness-neutral rather than merely correct.
//
// The consumers' side of it is pinned here too: a grid the producer widened and a
// consumer decodes with a different width would re-run row 0 and never reach the
// rest, which no single-shader test can catch.

#include <gtest/gtest.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "SssrShaderSource.h"

using GE::Tests::ReadSssrNodeSource;
using GE::Tests::ReadSssrShaderSource;

namespace
{

// Value of a GLSL `const uint <name> = <digits>u;` declaration. Rejects anything
// that is not a bare unsigned literal, so a test asserting on a number cannot pass
// against an expression it never evaluated.
std::optional<std::uint64_t> ParseUintConstant(const std::string& source, const std::string& name)
{
    const std::size_t at = source.find(name + " = ");
    if (at == std::string::npos)
        return std::nullopt;
    std::size_t i = at + name.size() + 3;
    const std::size_t end = source.find(';', i);
    if (end == std::string::npos)
        return std::nullopt;
    std::uint64_t value = 0;
    bool anyDigit = false;
    for (; i < end && std::isdigit(static_cast<unsigned char>(source[i])); ++i)
    {
        value = value * 10u + static_cast<std::uint64_t>(source[i] - '0');
        anyDigit = true;
    }
    if (!anyDigit)
        return std::nullopt;
    if (i < end && (source[i] == 'u' || source[i] == 'U'))
        ++i;
    while (i < end && std::isspace(static_cast<unsigned char>(source[i])))
        ++i;
    if (i != end)
        return std::nullopt;
    return value;
}

constexpr std::uint64_t CeilDiv(std::uint64_t a, std::uint64_t b) { return (a + b - 1u) / b; }

enum class ShaderProfile
{
    Desktop,
    Compat
};

// The source as one profile's compiler sees it: a `#if defined(GE_COMPAT_PROFILE)`
// arm survives only for Compat, its `#else` arm only for Desktop, and the gate's own
// directives are dropped so a scan cannot match them. Any other conditional is left
// in place, with its nesting tracked so an inner `#endif` does not close the gate.
// Without this a text scan reads both arms at once and asserts on a program neither
// profile compiles.
std::string ProfileView(const std::string& source, ShaderProfile profile)
{
    std::string view;
    view.reserve(source.size());
    bool inGate = false;
    bool inElseArm = false;
    int innerDepth = 0;
    std::size_t at = 0;
    while (at <= source.size())
    {
        const std::size_t eol = source.find('\n', at);
        const std::size_t lineEnd = eol == std::string::npos ? source.size() : eol + 1;
        const std::string line = source.substr(at, lineEnd - at);
        at = lineEnd;

        const std::size_t firstGlyph = line.find_first_not_of(" \t");
        const std::string_view directive =
            firstGlyph == std::string::npos ? std::string_view{} : std::string_view(line).substr(firstGlyph);
        const bool keep = inGate ? (profile == ShaderProfile::Compat) != inElseArm : true;

        if (!inGate && directive.rfind("#if defined(GE_COMPAT_PROFILE)", 0) == 0)
        {
            inGate = true;
            inElseArm = false;
            innerDepth = 0;
        }
        else if (inGate && innerDepth == 0 && directive.rfind("#else", 0) == 0)
        {
            inElseArm = true;
        }
        else if (inGate && innerDepth == 0 && directive.rfind("#endif", 0) == 0)
        {
            inGate = false;
        }
        else
        {
            if (inGate && directive.rfind("#if", 0) == 0)
                ++innerDepth;
            else if (inGate && directive.rfind("#endif", 0) == 0)
                --innerDepth;
            if (keep)
                view += line;
        }

        if (eol == std::string::npos)
            break;
    }
    return view;
}

} // namespace

// The width is the spec minimum, and the counts that must fit under it genuinely
// exceed it. Without the second half this whole change would be theatre.
TEST(SssrDispatchArgsBounds, WidthIsTheSpecMinimumAndBothCountsReachIt)
{
    const std::string dispatch = ReadSssrShaderSource("ScreenSpaceReflections/sssr_dispatch.glsl");
    ASSERT_FALSE(dispatch.empty()) << "ScreenSpaceReflections/sssr_dispatch.glsl not found";

    const auto width = ParseUintConstant(dispatch, "kSssrMaxDispatchWidth");
    ASSERT_TRUE(width.has_value()) << "kSssrMaxDispatchWidth must be a bare unsigned literal";
    EXPECT_EQ(*width, 65535u)
        << "the bound must be maxComputeWorkGroupCount's guaranteed minimum: anything larger is "
           "a device capability this code does not query";

    const auto raysPerGroup = ParseUintConstant(
        ReadSssrShaderSource("ScreenSpaceReflections/sssr_prepare_args.comp"), "kRaysPerGroup");
    ASSERT_TRUE(raysPerGroup.has_value());

    // 4K, every pixel reflective, trace rate 1 — the case the bound exists for.
    constexpr std::uint64_t kWidth4K = 3840u;
    constexpr std::uint64_t kHeight4K = 2160u;
    constexpr std::uint64_t kTile = 8u;
    const std::uint64_t tiles = CeilDiv(kWidth4K, kTile) * CeilDiv(kHeight4K, kTile);
    const std::uint64_t rayGroups = CeilDiv(kWidth4K * kHeight4K, *raysPerGroup);
    EXPECT_GT(tiles, *width) << "denoise-tile count at 4K must exceed the bound, or this change "
                                "is guarding against nothing";
    EXPECT_GT(rayGroups, *width) << "ray-group count at 4K must exceed the bound too: both "
                                    "dispatch paths share the threshold";
}

// Exact spill, not a clamp. A clamp would drop work in the engine instead of in the
// driver, which is not an improvement.
TEST(SssrDispatchArgsBounds, OverflowSpillsIntoYAndIsNeverClamped)
{
    const std::string dispatch = ReadSssrShaderSource("ScreenSpaceReflections/sssr_dispatch.glsl");
    ASSERT_FALSE(dispatch.empty());

    const std::size_t at = dispatch.find("uvec2 GE_SssrDispatchGrid(uint groupCount)");
    ASSERT_NE(at, std::string::npos) << "the grid helper must live in the shared include";
    const std::string body = dispatch.substr(at, dispatch.find("\n}", at) - at);

    EXPECT_NE(body.find("(groupCount + kSssrMaxDispatchWidth - 1u) / kSssrMaxDispatchWidth"),
              std::string::npos)
        << "the Y extent must be the ceiling division of the count by the width, so the grid "
           "covers every group";
    EXPECT_EQ(body.find("min(groupCount"), std::string::npos)
        << "clamping the count to the width silently drops workgroups";
    EXPECT_EQ(body.find("clamp("), std::string::npos)
        << "clamping the count to the width silently drops workgroups";
}

// Below the limit the emitted grid is byte-identical to the pre-bound one. This is
// the claim that lets the change be reviewed as correctness-neutral.
TEST(SssrDispatchArgsBounds, GridIsUnchangedBelowTheLimit)
{
    const std::string dispatch = ReadSssrShaderSource("ScreenSpaceReflections/sssr_dispatch.glsl");
    ASSERT_FALSE(dispatch.empty());

    EXPECT_NE(dispatch.find("if (groupCount <= kSssrMaxDispatchWidth)\n"
                            "        return uvec2(groupCount, 1u);"),
              std::string::npos)
        << "at or below the width the grid must be exactly (count, 1): one dispatch row, Y = 1, "
           "which is what every resolution shipping today produces";

    // And the decode must reduce to gl_WorkGroupID.x for that grid.
    EXPECT_NE(dispatch.find("return groupId.x + groupId.y * kSssrMaxDispatchWidth;"),
              std::string::npos)
        << "the linear index must be x + y*width, which is x whenever y is 0";
}

// Producer and consumers must share the width. Two copies that disagree re-run the
// first row and never reach the tail, with no error anywhere.
TEST(SssrDispatchArgsBounds, ProducerAndConsumersShareOneWidthConstant)
{
    for (const char* const shader :
         {"ScreenSpaceReflections/sssr_prepare_args.comp",
          "ScreenSpaceReflections/sssr_intersect.comp",
          "ScreenSpaceReflections/sssr_prefilter.comp"})
    {
        const std::string source = ReadSssrShaderSource(shader);
        ASSERT_FALSE(source.empty()) << shader;
        EXPECT_EQ(source.find("65535"), std::string::npos)
            << shader << " must not spell the dispatch width: it comes from the shared include";
    }

    const std::string intersect = ReadSssrShaderSource("ScreenSpaceReflections/sssr_intersect.comp");
    const std::string prefilter = ReadSssrShaderSource("ScreenSpaceReflections/sssr_prefilter.comp");
    EXPECT_NE(intersect.find("GE_SssrLinearGroupIndex(gl_WorkGroupID.xy)"), std::string::npos)
        << "intersect must decode the spilled grid, not read gl_GlobalInvocationID.x";
    EXPECT_EQ(intersect.find("uint listIndex = gl_GlobalInvocationID.x;"), std::string::npos)
        << "gl_GlobalInvocationID.x ignores the Y spill and would trace row 0's rays repeatedly";
    EXPECT_NE(prefilter.find("GE_SssrLinearGroupIndex(gl_WorkGroupID.xy)"), std::string::npos)
        << "the prefilter must decode the spilled grid too";
}

// The grid overshoots the count in both the rounding and the spill. Consumers gate
// on the appended count, which covers both.
TEST(SssrDispatchArgsBounds, ConsumersGateOnTheAppendedCount)
{
    const std::string intersect = ReadSssrShaderSource("ScreenSpaceReflections/sssr_intersect.comp");
    const std::string prefilter = ReadSssrShaderSource("ScreenSpaceReflections/sssr_prefilter.comp");
    ASSERT_FALSE(intersect.empty());
    ASSERT_FALSE(prefilter.empty());

    const std::string compat = ProfileView(prefilter, ShaderProfile::Compat);
    const std::string desktop = ProfileView(prefilter, ShaderProfile::Desktop);

    EXPECT_NE(intersect.find("if (listIndex >= Args.rayCount)"), std::string::npos)
        << "intersect must discard lanes past the appended ray count";
    EXPECT_NE(compat.find("const bool tileValid = tileIndex < Args.tileCount;"),
              std::string::npos)
        << "the prefilter must gate on the appended tile count; a workgroup past it would "
           "otherwise read tile-list slots nothing wrote";
    EXPECT_NE(compat.find("tileValid ? TileList.tiles[tileIndex] : 0u"), std::string::npos)
        << "an invalid workgroup must not index the tile list at all: its slot was never "
           "written and may be past the buffer's appended end";
    EXPECT_NE(compat.find("if (!tileValid || any(greaterThanEqual(pixel, size)))"),
              std::string::npos)
        << "invalid workgroups must exit at the post-barrier gate before storing anything";

    // Desktop gates the same workgroups with the early return, which is what keeps
    // its program free of the flag's branch and selects.
    EXPECT_NE(desktop.find("if (tileIndex >= Args.tileCount)"), std::string::npos)
        << "the desktop profile must gate on the appended tile count too";
    EXPECT_NE(desktop.find("uint packedTile = TileList.tiles[tileIndex];"), std::string::npos)
        << "past the early return the tile-list slot is unconditionally valid";
    EXPECT_EQ(desktop.find("tileValid"), std::string::npos)
        << "the validity flag is the compat profile's shape; nothing selects on it here";
}

// The prefilter's tile-validity gate interacts with the cooperative tile load's
// barrier. A return ahead of the barrier is safe because the condition is
// workgroup-uniform — which WGSL's uniformity analysis cannot prove, so the compat
// profile spends a flag instead: invalid tiles load zero texels, reach the barrier
// with everyone else, and exit at the post-barrier gate. Two things keep that sound:
// no thread returns ahead of the barrier at all, and the flag is fed only by the
// workgroup id. Desktop needs no such proof and keeps the return.
TEST(SssrDispatchArgsBounds, PrefilterTileGateIsUniformAcrossTheWorkgroup)
{
    const std::string prefilter = ReadSssrShaderSource("ScreenSpaceReflections/sssr_prefilter.comp");
    ASSERT_FALSE(prefilter.empty());
    const std::string compat = ProfileView(prefilter, ShaderProfile::Compat);
    const std::string desktop = ProfileView(prefilter, ShaderProfile::Desktop);

    const std::size_t mainAt = compat.find("void main()");
    ASSERT_NE(mainAt, std::string::npos);
    const std::size_t flagAt =
        compat.find("const bool tileValid = tileIndex < Args.tileCount;", mainAt);
    ASSERT_NE(flagAt, std::string::npos) << "the tile gate is a flag, not an early return";
    const std::size_t firstBarrier = compat.find("barrier();", mainAt);
    ASSERT_NE(firstBarrier, std::string::npos);

    EXPECT_LT(flagAt, firstBarrier)
        << "the flag must be established before the barriered region it gates";
    const std::string preBarrier = compat.substr(mainAt, firstBarrier - mainAt);
    EXPECT_EQ(preBarrier.find("return;"), std::string::npos)
        << "no thread may return ahead of the cooperative tile load's barrier: a workgroup "
           "that splits around a barrier is undefined behaviour, in practice a hang";
    const std::string flagFeed = compat.substr(mainAt, flagAt - mainAt);
    EXPECT_EQ(flagFeed.find("gl_LocalInvocationID"), std::string::npos)
        << "nothing per-thread may feed the flag: every thread of the workgroup must agree "
           "on how many texels the cooperative load loads";
    EXPECT_EQ(flagFeed.find("gl_LocalInvocationIndex"), std::string::npos)
        << "nothing per-thread may feed the flag";
    EXPECT_NE(compat.find("if (!tileValid || any(greaterThanEqual(pixel, size)))", firstBarrier),
              std::string::npos)
        << "the exit for invalid tiles must sit after the barrier";

    const std::size_t desktopMainAt = desktop.find("void main()");
    ASSERT_NE(desktopMainAt, std::string::npos);
    const std::size_t desktopGate = desktop.find("if (tileIndex >= Args.tileCount)", desktopMainAt);
    const std::size_t desktopBarrier = desktop.find("barrier();", desktopMainAt);
    ASSERT_NE(desktopGate, std::string::npos);
    ASSERT_NE(desktopBarrier, std::string::npos);
    EXPECT_LT(desktopGate, desktopBarrier)
        << "the whole workgroup leaves together, ahead of the barrier it would otherwise split";
}

// Classify appends and nothing else. The per-ray atomicMax it used to maintain
// recomputed one division a million times a frame, on the same address.
TEST(SssrDispatchArgsBounds, ClassifyAppendsWithoutMaintainingADispatchGrid)
{
    const std::string classify = ReadSssrShaderSource("ScreenSpaceReflections/sssr_classify.comp");
    ASSERT_FALSE(classify.empty());

    EXPECT_EQ(classify.find("atomicMax"), std::string::npos)
        << "the ray group count is derivable from the final ray count by one thread; a per-ray "
           "atomicMax computes it once per traced pixel instead";
    for (const char* const field : {"Args.rayGroupCountX", "Args.rayGroupCountY",
                                    "Args.rayGroupCountZ", "Args.tileGroupCountX",
                                    "Args.tileGroupCountY", "Args.tileGroupCountZ"})
    {
        EXPECT_EQ(classify.find(field), std::string::npos)
            << "classify must not write " << field << ": the grids are the args-prep pass's "
               "output, and two writers would race";
    }
    // Exactly two atomics remain, one per list.
    EXPECT_NE(classify.find("atomicAdd(Args.rayCount, 1u)"), std::string::npos);
    EXPECT_NE(classify.find("atomicAdd(Args.tileCount, 1u)"), std::string::npos);
}

// One thread, and ordered between the appends and the first pass that consumes a
// grid. Recorded after Intersect it would prepare the args a pass too late.
TEST(SssrDispatchArgsBounds, PrepareArgsIsOneThreadOrderedBeforeIntersect)
{
    const std::string prepare = ReadSssrShaderSource("ScreenSpaceReflections/sssr_prepare_args.comp");
    const std::string node = ReadSssrNodeSource();
    ASSERT_FALSE(prepare.empty()) << "sssr_prepare_args.comp not found";
    ASSERT_FALSE(node.empty());

    EXPECT_NE(prepare.find("layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;"),
              std::string::npos)
        << "the args-prep pass is a single thread writing six words";
    EXPECT_NE(node.find("ctx.Cmd->Dispatch(1, 1, 1);"), std::string::npos)
        << "the args-prep pass must dispatch exactly one workgroup";

    const std::size_t classifyAt = node.find("d.PassName(\"ClassifyTiles\")");
    const std::size_t prepareAt = node.find("d.PassName(\"PrepareArgs\")");
    const std::size_t intersectAt = node.find("d.PassName(\"Intersect\")");
    ASSERT_NE(classifyAt, std::string::npos);
    ASSERT_NE(prepareAt, std::string::npos) << "the node must declare a PrepareArgs pass";
    ASSERT_NE(intersectAt, std::string::npos);
    EXPECT_LT(classifyAt, prepareAt) << "args-prep reads counts classify has to have finished";
    EXPECT_LT(prepareAt, intersectAt) << "args-prep writes the grid Intersect dispatches off";

    // Intersect reads the args buffer twice over: as indirect args and as an SSBO
    // for the count gate. Dropping the storage read drops it from the barrier scope.
    EXPECT_NE(node.find("p.Read(indirectArgs, RenderGraph::RGBufferRead::Storage);"),
              std::string::npos)
        << "the count gate is an SSBO read and must be declared as one, or the graph barriers "
           "only the indirect-args access";
}

// The ray grid is expressed in units of intersect's workgroup, so the two have to
// agree. A mismatch traces a fraction of the rays or runs off the end of the list.
TEST(SssrDispatchArgsBounds, RaysPerGroupMatchesTheIntersectWorkgroupSize)
{
    const std::string prepare = ReadSssrShaderSource("ScreenSpaceReflections/sssr_prepare_args.comp");
    const std::string intersect = ReadSssrShaderSource("ScreenSpaceReflections/sssr_intersect.comp");
    ASSERT_FALSE(prepare.empty());
    ASSERT_FALSE(intersect.empty());

    const auto raysPerGroup = ParseUintConstant(prepare, "kRaysPerGroup");
    ASSERT_TRUE(raysPerGroup.has_value());
    EXPECT_NE(intersect.find("layout(local_size_x = " + std::to_string(*raysPerGroup) +
                             ", local_size_y = 1, local_size_z = 1) in;"),
              std::string::npos)
        << "sssr_prepare_args.comp's kRaysPerGroup must equal sssr_intersect.comp's local_size_x";

    // And intersect must derive its lane from the workgroup size rather than
    // re-spelling it, so there is one place the two can disagree instead of two.
    EXPECT_NE(intersect.find("* gl_WorkGroupSize.x + gl_LocalInvocationID.x"), std::string::npos)
        << "the ray index must scale by gl_WorkGroupSize.x, not a hard-coded 64";
}
