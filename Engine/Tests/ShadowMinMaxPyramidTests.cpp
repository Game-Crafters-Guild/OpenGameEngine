#include <gtest/gtest.h>

#include "Engine/Rendering/ShadowMinMaxPyramid.h"

#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

using namespace GameEngine::Engine::Renderer;

namespace
{
// Full-resolution texels one texel of `level` covers.
float Footprint(uint32_t level)
{
    return static_cast<float>(1u << (ShadowMinMaxPyramid::kBaseDownshift + level));
}

// Offset of the block member called `name`, or ~0u when it has none.
uint32_t MemberOffset(const GameEngine::Rendering::BlockLayout& block, const char* name)
{
    for (const GameEngine::Rendering::Member& member : block.Members)
    {
        if (member.Name == name)
            return member.Offset;
    }
    return ~0u;
}
} // namespace

// The pyramid's top level must cover the PCSS lit proof's query (the widest
// kernel plus the query margin), or the proof answers "no bound" and the lit exit
// never fires. A 60-texel kernel (a wide Max Penumbra over a fine cascade) still
// gets a level that covers it.
TEST(ShadowMinMaxPyramid, LevelsCoverTheWidestKernelQuery)
{
    constexpr uint32_t kBase = 512u; // a 2048 shadow map
    for (const float kernel : {1.0f, 10.0f, 30.0f, 60.0f, 120.0f})
    {
        SCOPED_TRACE(::testing::Message() << "kernel " << kernel << " texels");
        const float query = kernel + ShadowMinMaxPyramid::kQueryMarginTexels;
        const uint32_t levels = ShadowMinMaxPyramid::LevelsForQuery(kBase, query);
        ASSERT_GE(levels, 1u);
        EXPECT_GE(Footprint(levels - 1u), query) << "the top level covers the query";
        if (levels > 1u)
            EXPECT_LT(Footprint(levels - 2u), query) << "no level beyond the first that covers it";
    }
    EXPECT_EQ(ShadowMinMaxPyramid::LevelsForQuery(kBase, 61.5f), 5u);
    // Never past the base level's own mip chain (512 -> 10 levels).
    EXPECT_EQ(ShadowMinMaxPyramid::LevelsForQuery(kBase, 1.0e6f), 10u);
    EXPECT_EQ(ShadowMinMaxPyramid::LevelsForQuery(0u, 10.0f), 0u);
}

// The C++ mirror and shadow_minmax_reduce.comp's push block agree on every
// backend: each member sits at the same offset, the block ends where the
// struct does (the engine pushes sizeof), and that end is a multiple of the
// block's largest alignment (ivec2: 8), the size the MSL struct rounds up to.
// A 20-byte push against Metal's 24-byte struct aborts the dispatch under the
// Metal API validation layer.
TEST(ShadowMinMaxPyramid, PushConstantsMatchTheShaderBlockOnEveryBackend)
{
    using PushConstants = ShadowMinMaxPyramid::PushConstants;
    namespace R = GameEngine::Rendering;
    R::ShaderPackage pkg{};
    std::string err;
    ASSERT_TRUE(R::LoadShaderPkg("Shaders/shadow_minmax_reduce.shaderpkg", R::ShaderSourceKind::SpirV, pkg, &err))
        << err;
    ASSERT_EQ(pkg.meta.PushConstants.size(), 1u);
    const R::BlockLayout& block = pkg.meta.PushConstants[0].Block;

    uint32_t end = 0;
    for (const R::Member& member : block.Members)
        end = std::max(end, member.Offset + member.Size);
    EXPECT_EQ(end, sizeof(PushConstants)) << "the engine pushes sizeof(PushConstants)";
    constexpr size_t kLargestAlignment = 2 * sizeof(int32_t); // ivec2 dstSize
    EXPECT_EQ(sizeof(PushConstants) % kLargestAlignment, 0u);

    EXPECT_EQ(MemberOffset(block, "dstSize"), offsetof(PushConstants, DstSize));
    EXPECT_EQ(MemberOffset(block, "layerCount"), offsetof(PushConstants, LayerCount));
    EXPECT_EQ(MemberOffset(block, "srcLevel"), offsetof(PushConstants, SrcLevel));
    EXPECT_EQ(MemberOffset(block, "mode"), offsetof(PushConstants, Mode));
}
