// Near-square packing of the DDGI glossy reflection atlas, and the C++/GLSL
// constant contract that packing depends on.
//
// Two failure modes are worth pinning here, and neither is visible in a
// screenshot as anything but "reflections look wrong somewhere":
//
//   * The CPU sizes the atlas and its history buffers while the GPU addresses
//     tiles inside them. If the two disagree about TilesX by one, every probe
//     past the first row reads a neighbour's tile — a smooth, plausible-looking
//     wrongness. A sqrt() on either side would be the obvious way to introduce
//     that (rounding at the perfect squares differs between targets), which is
//     why the production helper is integer-only and why the perfect squares are
//     tested explicitly below.
//
//   * The tile size is written twice, once in C++ (DDGIGlossyAtlasLayout.h) and
//     once in GLSL (Includes/ddgi_common.glsl). The kernels index their state
//     buffers with the GLSL value while the allocation uses the C++ one, so a
//     divergence is an out-of-bounds write rather than an artifact. The shader
//     source is read as the artifact under test, following
//     ShadowSamplingShaderContractTests/IblShaderContractTests.

#include <gtest/gtest.h>

#include "Engine/Rendering/DDGIGlossyAtlasLayout.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using GameEngine::Engine::Renderer::ComputeDDGIGlossyAtlasLayout;
using GameEngine::Engine::Renderer::DDGIGlossyAtlasLayout;
using GameEngine::Engine::Renderer::kDDGIGlossyBorder;
using GameEngine::Engine::Renderer::kDDGIGlossyOctRes;
using GameEngine::Engine::Renderer::kDDGIGlossyTile;

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

}  // namespace

TEST(DDGIGlossyAtlasLayoutTests, PacksEveryProbeWithinBounds)
{
    // The property that actually matters: every probe index maps to a tile
    // fully inside the allocated texture, at every count — not just the tidy
    // ones. Mirrors GE_DDGIGlossyTileOrigin's arithmetic exactly.
    for (int32_t probeTotal = 1; probeTotal <= 2000; ++probeTotal)
    {
        const DDGIGlossyAtlasLayout layout = ComputeDDGIGlossyAtlasLayout(probeTotal);
        ASSERT_GE(layout.TilesX * layout.TilesY, probeTotal)
            << "layout cannot hold all tiles at probeTotal=" << probeTotal;

        const int32_t lastIdx = probeTotal - 1;
        const int32_t col = lastIdx % layout.TilesX;
        const int32_t row = lastIdx / layout.TilesX;
        EXPECT_LE((col + 1) * kDDGIGlossyTile, layout.WidthTexels) << "probeTotal=" << probeTotal;
        EXPECT_LE((row + 1) * kDDGIGlossyTile, layout.HeightTexels) << "probeTotal=" << probeTotal;
    }
}

TEST(DDGIGlossyAtlasLayoutTests, IsNearSquareAndMinimal)
{
    // Near-square is the whole reason this layout exists (the z-major one
    // overflows the texture limit at 18x18 tiles), so pin that it stays near
    // square and does not waste a whole extra row.
    for (int32_t probeTotal = 1; probeTotal <= 2000; ++probeTotal)
    {
        const DDGIGlossyAtlasLayout layout = ComputeDDGIGlossyAtlasLayout(probeTotal);
        EXPECT_EQ(layout.TilesY, (probeTotal + layout.TilesX - 1) / layout.TilesX)
            << "row count is not minimal at probeTotal=" << probeTotal;
        // TilesX is the smallest n with n*n >= probeTotal, so one column
        // narrower must NOT have been enough.
        EXPECT_GE(layout.TilesX * layout.TilesX, probeTotal);
        if (layout.TilesX > 1)
            EXPECT_LT((layout.TilesX - 1) * (layout.TilesX - 1), probeTotal);
    }
}

TEST(DDGIGlossyAtlasLayoutTests, PerfectSquaresDoNotOverAllocate)
{
    // The rounding-sensitive cases: a float sqrt() that lands a hair above the
    // integer would add an entire unused column and row here, and (worse) shift
    // every tile origin relative to a shader that rounded the other way.
    for (int32_t n = 1; n <= 64; ++n)
    {
        const DDGIGlossyAtlasLayout layout = ComputeDDGIGlossyAtlasLayout(n * n);
        EXPECT_EQ(layout.TilesX, n) << "at probeTotal=" << (n * n);
        EXPECT_EQ(layout.TilesY, n) << "at probeTotal=" << (n * n);
        EXPECT_EQ(layout.WidthTexels, n * kDDGIGlossyTile);
        EXPECT_EQ(layout.HeightTexels, n * kDDGIGlossyTile);
    }
}

TEST(DDGIGlossyAtlasLayoutTests, DegenerateProbeCountStillAllocatesBindableTexture)
{
    // A zero/negative count must not produce a zero-sized texture: the binding
    // still has to be satisfiable (see the world pass's fallback contract).
    for (const int32_t probeTotal : {0, -1, -1000})
    {
        const DDGIGlossyAtlasLayout layout = ComputeDDGIGlossyAtlasLayout(probeTotal);
        EXPECT_EQ(layout.TilesX, 1);
        EXPECT_EQ(layout.TilesY, 1);
        EXPECT_EQ(layout.WidthTexels, kDDGIGlossyTile);
        EXPECT_EQ(layout.HeightTexels, kDDGIGlossyTile);
    }
}

TEST(DDGIGlossyAtlasLayoutTests, DefaultVolumeStaysWellInsideTextureLimits)
{
    // The sizing claim the design rests on: near-square keeps even a 32^3 grid
    // inside the 8192 limit that the z-major layout (32*32*18 = 18432) blows
    // through. If this ever fails, the packing choice needs revisiting, not the
    // constant.
    constexpr int32_t kMaxAtlasDimension = 8192;
    const DDGIGlossyAtlasLayout largest = ComputeDDGIGlossyAtlasLayout(32 * 32 * 32);
    EXPECT_LE(largest.WidthTexels, kMaxAtlasDimension);
    EXPECT_LE(largest.HeightTexels, kMaxAtlasDimension);
    EXPECT_GT(32 * 32 * kDDGIGlossyTile, kMaxAtlasDimension)
        << "z-major would fit after all — the near-square packing's rationale needs rechecking";
}

TEST(DDGIGlossyAtlasLayoutTests, GlslConstantsMatchTheCppMirror)
{
#ifndef GE_RENDERER_REPO_ROOT
    GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined (dev-only shader-source anchor)";
#else
    const std::filesystem::path path = std::filesystem::path(GE_RENDERER_REPO_ROOT) /
                                       "Engine/Modules/Rendering/Shaders/Includes/ddgi_common.glsl";
    const std::string source = ReadTextFile(path);
    ASSERT_FALSE(source.empty()) << "could not read " << path.string();

    const std::string octRes =
        "const int GE_DDGI_GLOSSY_OCT_RES = " + std::to_string(kDDGIGlossyOctRes) + ";";
    EXPECT_NE(source.find(octRes), std::string::npos)
        << "GLSL glossy interior resolution no longer matches kDDGIGlossyOctRes ("
        << kDDGIGlossyOctRes << ") — the kernels would index state buffers the C++ side sized "
        << "differently, which is an out-of-bounds write, not an artifact";

    const std::string tile =
        "const int GE_DDGI_GLOSSY_TILE = GE_DDGI_GLOSSY_OCT_RES + 2 * GE_DDGI_BORDER;";
    EXPECT_NE(source.find(tile), std::string::npos)
        << "GLSL glossy tile size is no longer octRes + 2*border";

    const std::string border = "const int GE_DDGI_BORDER = " + std::to_string(kDDGIGlossyBorder) + ";";
    EXPECT_NE(source.find(border), std::string::npos)
        << "GLSL gutter width no longer matches kDDGIGlossyBorder (" << kDDGIGlossyBorder << ")";

    // Guards the derived value the two assertions above only imply.
    EXPECT_EQ(kDDGIGlossyTile, kDDGIGlossyOctRes + 2 * kDDGIGlossyBorder);
#endif
}
