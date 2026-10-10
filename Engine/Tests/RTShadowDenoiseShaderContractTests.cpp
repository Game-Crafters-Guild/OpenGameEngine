// Spatial-denoise contract on rt_shadow_denoise.comp. The RT shadow mask is a
// per-frame binary stipple; in a view without TAA history no jitter phase
// advances, so nothing resolves it temporally. This pass resolves it spatially
// instead. Three
// invariants are load-bearing and invisible to a pixel comparison:
//
//   * The blur is a JOINT BILATERAL keyed on the receiver's tangent PLANE, not a
//     plain box — otherwise it leaks light across occluder/receiver depth
//     discontinuities (contact shadows) and smears silhouettes. The plane
//     distance is dot(tap - center, normal).
//   * Sky taps (reverse-Z raw depth <= 0, staged with a non-positive view z) are
//     excluded, so a shadowed silhouette edge never averages in fully-lit sky.
//   * The output stays FRACTIONAL: the mask is a 0..1 occlusion, so the pass
//     writes the weighted mean directly with no threshold to {0,1}.
//
// Reads the repo shader source via GE_RENDERER_REPO_ROOT (dev-only anchor): a
// shader-only edit does not rebuild the staged copy, so the repo file is the
// artifact and is never stale. Follows RTShadowMaskShaderContractTests.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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

// Strip //-comments so prose describing the contract never satisfies it.
std::string StripLineComments(const std::string& source)
{
    std::string out;
    out.reserve(source.size());
    std::size_t pos = 0;
    while (pos < source.size())
    {
        const std::size_t comment = source.find("//", pos);
        const std::size_t lineEnd = source.find('\n', pos);
        if (comment == std::string::npos || (lineEnd != std::string::npos && comment > lineEnd))
        {
            if (lineEnd == std::string::npos)
            {
                out.append(source, pos, std::string::npos);
                break;
            }
            out.append(source, pos, lineEnd + 1 - pos);
            pos = lineEnd + 1;
            continue;
        }
        out.append(source, pos, comment - pos);
        if (lineEnd == std::string::npos)
            break;
        out.push_back('\n');
        pos = lineEnd + 1;
    }
    return out;
}

std::string RTShadowDenoiseSource()
{
#ifndef GE_RENDERER_REPO_ROOT
    return {};
#else
    const std::filesystem::path path = std::filesystem::path(GE_RENDERER_REPO_ROOT) /
                                       "Engine/Modules/Rendering/Shaders/rt_shadow_denoise.comp";
    return StripLineComments(ReadTextFile(path));
#endif
}

} // namespace

// The denoise must be a plane-aware joint bilateral: the tap weight falls off with
// distance from the receiver's tangent plane (dot(tap - center, normal)), which is
// what keeps the blur off occluder/receiver discontinuities.
TEST(RTShadowDenoiseShaderContract, PlaneAwareBilateralWeight)
{
    const std::string src = RTShadowDenoiseSource();
    ASSERT_FALSE(src.empty()) << "rt_shadow_denoise.comp not found via GE_RENDERER_REPO_ROOT";

    EXPECT_NE(src.find("dot(tapVP - centerVP, nVS)"), std::string::npos)
        << "the bilateral weight must measure off-tangent-plane distance (normal-aware)";
    EXPECT_NE(src.find("uParams.x"), std::string::npos)
        << "the kernel radius must come from the caller (penumbra-bounded footprint)";
}

// Sky taps (reverse-Z far, staged with view z <= 0) must be dropped so a
// receiver edge never averages in fully-lit sky.
TEST(RTShadowDenoiseShaderContract, SkyTapsExcluded)
{
    const std::string src = RTShadowDenoiseSource();
    ASSERT_FALSE(src.empty());

    const std::size_t skyTest = src.find("if (tapVP.z <= 0.0)");
    ASSERT_NE(skyTest, std::string::npos) << "sky taps must be excluded from the bilateral sum";
    const std::size_t skip = src.find("continue;", skyTest);
    ASSERT_NE(skip, std::string::npos) << "a sky tap must be skipped, not weighted";
    EXPECT_LT(skip - skyTest, std::size_t{64})
        << "the skip must be the sky test's own statement, not a later one";
}

// The mask carries a fractional 0..1 occlusion; the denoise writes the weighted
// mean directly and must never threshold it back to a binary value.
TEST(RTShadowDenoiseShaderContract, OutputStaysFractional)
{
    const std::string src = RTShadowDenoiseSource();
    ASSERT_FALSE(src.empty());

    EXPECT_NE(src.find("sum / wsum"), std::string::npos)
        << "the result must be the weighted mean (fractional), not a thresholded step";
    EXPECT_NE(src.find("imageStore(uMaskOut, pix, vec4(result))"), std::string::npos)
        << "the fractional mean must be stored unmodified";
    EXPECT_EQ(src.find("step("), std::string::npos)
        << "the denoise must not threshold the mask to {0,1}";
    EXPECT_EQ(src.find("result > 0.5"), std::string::npos)
        << "the denoise must not threshold the mask to {0,1}";
}
