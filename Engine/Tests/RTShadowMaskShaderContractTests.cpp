// Soft-shadow contract on rt_shadow_mask.comp. The RT directional mask jitters
// its one ray inside the sun's angular cone (uRayParams.w = tan half-angle) and
// lets TAA resolve the per-frame binary result into a fractional penumbra. Two
// invariants are load-bearing and invisible to a pixel comparison:
//
//   * At angular radius 0 the traced direction must be BYTE-IDENTICAL to the
//     hard ray. The guarantee is structural: the jitter block is gated on
//     `tanHalfAngle > 0.0`, so at 0 the direction stays exactly uLightDirWS.xyz
//     (no normalize, no basis, no float drift). A pixel test cannot prove
//     byte-identity; the shape of the source can.
//   * The ray-query is initialized with the JITTERED `rayDir`, not
//     uLightDirWS.xyz — otherwise the cone would be computed and discarded and
//     every shadow would stay hard.
//
// Reads the repo shader source via GE_RENDERER_REPO_ROOT (dev-only anchor): a
// staged copy only refreshes when its staging target rebuilds, which a
// shader-only edit does not trigger, so the repo file is the artifact and is
// never stale. Follows ShadowSamplingShaderContractTests.

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

std::string RepoShaderSource(const char* relativePath)
{
#ifndef GE_RENDERER_REPO_ROOT
    return {};
#else
    return StripLineComments(ReadTextFile(std::filesystem::path(GE_RENDERER_REPO_ROOT) / relativePath));
#endif
}

std::string RTShadowMaskSource()
{
#ifndef GE_RENDERER_REPO_ROOT
    return {};
#else
    const std::filesystem::path path = std::filesystem::path(GE_RENDERER_REPO_ROOT) /
                                       "Engine/Modules/Rendering/Shaders/rt_shadow_mask.comp";
    return StripLineComments(ReadTextFile(path));
#endif
}

} // namespace

// The soft path must be gated on a positive tangent, so tangent 0 leaves the
// ray direction exactly uLightDirWS.xyz — byte-identical to the hard ray.
TEST(RTShadowMaskShaderContract, SoftJitterGatedOnPositiveTangent)
{
    const std::string src = RTShadowMaskSource();
    ASSERT_FALSE(src.empty()) << "rt_shadow_mask.comp not found via GE_RENDERER_REPO_ROOT";

    EXPECT_NE(src.find("tanHalfAngle = uRayParams.w"), std::string::npos)
        << "the light's tan(half-angle) must be read from uRayParams.w";
    EXPECT_NE(src.find("rayDir = uLightDirWS.xyz"), std::string::npos)
        << "the direction must default to the exact hard ray before any jitter";
    EXPECT_NE(src.find("if (tanHalfAngle > 0.0)"), std::string::npos)
        << "the cone jitter must be gated so tangent 0 is byte-identical to the hard ray";
    EXPECT_EQ(src.find("tan("), std::string::npos)
        << "the tangent arrives precomputed (ResolveShadowTanHalfAngle); no per-pixel tan()";
}

// The ray-query must trace the JITTERED direction, or the cone is computed and
// thrown away and every shadow stays hard.
TEST(RTShadowMaskShaderContract, RayQueryTracesJitteredDirection)
{
    const std::string src = RTShadowMaskSource();
    ASSERT_FALSE(src.empty());

    EXPECT_NE(src.find("origin, uRayParams.x, rayDir, uRayParams.y"), std::string::npos)
        << "rayQueryInitializeEXT must trace the jittered rayDir, not uLightDirWS.xyz";
    EXPECT_EQ(src.find("origin, uRayParams.x, uLightDirWS.xyz, uRayParams.y"), std::string::npos)
        << "the trace must no longer hard-code the un-jittered light direction";
}

// The cone sample is a uniform concentric disk, and its per-pixel field advances
// with the jitter phase (uTemporal.x) so TAA integrates it into a smooth
// penumbra.
TEST(RTShadowMaskShaderContract, ConcentricDiskSampleAdvancesWithTemporalPhase)
{
    const std::string src = RTShadowMaskSource();
    ASSERT_FALSE(src.empty());

    EXPECT_NE(src.find("ConcentricDisk"), std::string::npos)
        << "the cone sample must use the uniform concentric disk map";
    EXPECT_NE(src.find("uTemporal.x"), std::string::npos)
        << "the per-pixel jitter must advance with the temporal phase for TAA to resolve it";
    EXPECT_NE(src.find("fract(h + float(i) * kR2)"), std::string::npos)
        << "rays of one pixel must step an R2 sequence from the pixel's hash (Quality = 4 rays)";
    EXPECT_NE(src.find("litSum / float(rayCount)"), std::string::npos)
        << "the mask must store the lit fraction over the traced rays";
}

// Beyond MaxShadowDistance the mask writes lit and traces nothing; inside it the
// traced visibility fades over the band the cascades use, so switching the
// directional mode does not move where or how shadows end.
TEST(RTShadowMaskShaderContract, MaskFadesOverTheCascadeBand)
{
    const std::string src = RTShadowMaskSource();
    ASSERT_FALSE(src.empty()) << "rt_shadow_mask.comp not found via GE_RENDERER_REPO_ROOT";
    EXPECT_NE(src.find("#include \"Includes/shadow_distance_fade.glsl\""), std::string::npos);
    const auto fade = src.find("GE_JoinTerrainAfterFade(visibility, TerrainLit(posWS), posVS.z, uLightDirWS.w, uTemporal.z)");
    EXPECT_NE(fade, std::string::npos) << "the traced visibility must fade before MaxShadowDistance";
    // Applied to the stored result, after every ray has been counted.
    const auto lastRay = src.rfind("litSum +=");
    ASSERT_NE(lastRay, std::string::npos);
    EXPECT_LT(lastRay, fade);
}

// The terrain is not in the TLAS, so both directional-shadow modes take its shadow from one map
// through one lookup (terrain_shadow.glsl GE_TerrainShadowLit): the raster term (GE_SampleShadow,
// inside and beyond the cascades' range) and the ray-traced mask (every pixel, and beyond the range
// where no ray is traced). A caller that marched or sampled on its own would let the modes disagree.
TEST(RTShadowMaskShaderContract, BothShadowModesReadTheTerrainsClearanceMap)
{
    const std::string mask = RTShadowMaskSource();
    const std::string raster = RepoShaderSource("Engine/Modules/Rendering/Shaders/Includes/shadow_sampling.glsl");
    ASSERT_FALSE(mask.empty());
    ASSERT_FALSE(raster.empty()) << "shadow_sampling.glsl not found via GE_RENDERER_REPO_ROOT";

    EXPECT_NE(mask.find("#include \"Includes/terrain_shadow.glsl\""), std::string::npos);
    EXPECT_NE(mask.find("GE_TerrainShadowLit(g, p.x, p.y - uTerrainGrid3.z, p.z, 0.0,"), std::string::npos)
        << "the mask reads the shared lookup, every receiver at its own height";
    EXPECT_NE(mask.find("GE_JoinTerrainAfterFade(visibility, TerrainLit(posWS),"), std::string::npos)
        << "a traced pixel is lit only where the meshes and the terrain both let the sun through";
    EXPECT_NE(mask.find("vec4(TerrainLit((uInvView * vec4(posVS, 1.0)).xyz))"), std::string::npos)
        << "beyond the max shadow distance the terrain alone decides, as in the raster path";

    EXPECT_NE(raster.find("#include \"terrain_shadow.glsl\""), std::string::npos);
    EXPECT_NE(raster.find("return GE_TerrainShadowLit(g,"), std::string::npos);
    EXPECT_NE(raster.find("float farTerrain = GE_TerrainShadow(posWS, maxPenumbraWorld);"), std::string::npos)
        << "beyond the cascades the terrain's term still reaches";
    EXPECT_NE(raster.find("GE_JoinTerrainAfterFade(shadow, GE_TerrainShadow(posWS, maxPenumbraWorld),"), std::string::npos)
        << "inside the cascades the terrain's term joins the meshes' by a minimum";
    EXPECT_NE(raster.find("if (ge_terrainShadowSource.z == 0u)\n        return 1.0;"), std::string::npos)
        << "with no map published (Terrain.CastShadows off, no sun) every receiver stays lit";
}

// The distance fade ends the terms that have a range (the cascades, the traced rays) and leaves
// the terrain's, which has none: both modes join the terrain term after the fade through one
// function (GE_JoinTerrainAfterFade, whose result ShadowFilterComputeTest.
// DistanceFadeEndsTheCascadeTermAndLeavesTheTerrainTerm pins on the GPU), and past the distance
// both return the terrain term alone. A terrain term passed through the fade, or faded on its own,
// would let a hill's shadow vanish at MaxShadowDistance.
TEST(RTShadowMaskShaderContract, BothModesFadeTheRangedTermAndNotTheTerrainTerm)
{
    const std::string mask = RTShadowMaskSource();
    const std::string raster = RepoShaderSource("Engine/Modules/Rendering/Shaders/Includes/shadow_sampling.glsl");
    ASSERT_FALSE(mask.empty());
    ASSERT_FALSE(raster.empty()) << "shadow_sampling.glsl not found via GE_RENDERER_REPO_ROOT";

    // The ray-traced mask: the rays' visibility is the ranged term, TerrainLit the terrain term.
    const auto maskJoin =
        mask.find("GE_JoinTerrainAfterFade(visibility, TerrainLit(posWS), posVS.z, uLightDirWS.w, uTemporal.z)");
    EXPECT_NE(maskJoin, std::string::npos) << "the mask joins the terrain term after the fade";
    EXPECT_NE(mask.find("vec4(TerrainLit((uInvView * vec4(posVS, 1.0)).xyz))"), std::string::npos)
        << "past the distance the mask stores the terrain term alone, unfaded";
    EXPECT_EQ(mask.find("GE_ShadowDistanceFade("), std::string::npos)
        << "the mask fades only through the join, so no term is faded on its own";

    // The raster path: the blended cascade factor is the ranged term, GE_TerrainShadow the terrain's.
    const auto sampler = raster.find("float GE_SampleShadow(");
    ASSERT_NE(sampler, std::string::npos);
    const auto rasterJoin = raster.find(
        "GE_JoinTerrainAfterFade(shadow, GE_TerrainShadow(posWS, maxPenumbraWorld), linearDepth, ge_shadowParams.w, ge_shadowFilterParams.y)", sampler);
    EXPECT_NE(rasterJoin, std::string::npos) << "the raster path joins the terrain term after the fade";
    const auto far = raster.find("float farTerrain = GE_TerrainShadow(posWS, maxPenumbraWorld);", sampler);
    ASSERT_NE(far, std::string::npos);
    EXPECT_NE(raster.find("return farTerrain;", far), std::string::npos)
        << "past the distance the raster path returns the terrain term alone, unfaded";
    // The join's skip may drop the terrain read only where the result is already 0: a fully
    // shadowed cascade term before the band.
    EXPECT_NE(raster.find("if (shadow > 0.0 || distanceFade > 0.0)", sampler), std::string::npos)
        << "inside the band a fully shadowed cascade term still fades, so the terrain must be read";
    EXPECT_EQ(raster.find("mix(GE_TerrainShadow"), std::string::npos);
    EXPECT_EQ(raster.find("GE_TerrainShadow(posWS, maxPenumbraWorld) * (1.0 - "), std::string::npos);
}

// The terrain's penumbra follows each mode's mesh shadows: both read the occluder distance with the
// clearance (one tap of the RG map); the ray-traced mask softens it with the cone's tangent and, like
// the cone, no bound; the raster term with the light's tangent under the filters that read it (PCSS,
// DPCF) and the radius bound the PCSS kernel shares.
TEST(RTShadowMaskShaderContract, BothShadowModesSoftenTheTerrainsEdgeAsTheirMeshShadowsDo)
{
    const std::string mask = RTShadowMaskSource();
    const std::string raster = RepoShaderSource("Engine/Modules/Rendering/Shaders/Includes/shadow_sampling.glsl");
    ASSERT_FALSE(mask.empty());
    ASSERT_FALSE(raster.empty()) << "shadow_sampling.glsl not found via GE_RENDERER_REPO_ROOT";

    EXPECT_NE(mask.find("(gridCoord + 0.5) / float(uTerrainSource.y), 0.0).rg;"), std::string::npos)
        << "the mask reads the clearance and the occluder distance in one tap";
    EXPECT_NE(mask.find("p.z, 0.0, uRayParams.w,\n                               kGE_TerrainShadowUncappedPenumbra);"),
              std::string::npos)
        << "the mask softens the terrain's edge with the cone's tangent, unbounded like the cone";

    EXPECT_NE(raster.find("(gridCoord + 0.5) / side, 0.0).rg;"), std::string::npos)
        << "the raster term reads the clearance and the occluder distance in one tap";
    EXPECT_NE(raster.find("(quality == 3 || quality == 5) ? ge_shadowPcssCascades[0].w : 0.0;"), std::string::npos)
        << "the raster term takes the light's tangent under PCSS and DPCF";
    EXPECT_NE(raster.find("ge_ShadowReceiverOnTerrain, tanHalfAngle, maxPenumbraRadius);"), std::string::npos);
    EXPECT_NE(raster.find("float maxPenumbraWorld = ge_shadowPcss.z > 0.0 ? ge_shadowPcss.z : kPcssMaxPenumbraWorldAuto;"),
              std::string::npos)
        << "the terrain's radius bound is the PCSS kernel's (Max Penumbra, 0 = auto)";
}
