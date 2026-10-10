// Blocker-search contract on shadow_sampling.glsl. PCSS derives its search
// radius from the light's angular size — searchWorld = tan(halfAngle) * a
// fraction of the cascade's depth span — and bounds it in TEXELS, because what
// the bounds govern is tap density, not physical size.
//
// The defect this pins shipped for a long time and was invisible to every other
// kind of test. The parameter is a dimensionless tangent, but it was clamped
// against world-space constants (0.01/0.08 m) as though it were a length. Two
// consequences, both measured on the 100 m / 4-cascade / 2048 default fit:
//   * on the shipped 0.53 degree sun, clamp(0.005, 0.01, 0.08) pinned the radius
//     at the floor, so the authored value never reached the search at all;
//   * the radius came out SUB-TEXEL in every cascade — 0.73 texels in cascade 0
//     down to 0.09 in cascade 3 — so every Vogel tap resolved into one texel.
//     avgBlocker was a point sample rather than an average, and a point sample
//     that misses returns "fully lit". The far cascades rendered no penumbra at
//     all, not a thin one.
//
// A pixel comparison is poor evidence here (the failure looks like a plausible
// hard shadow), and the derivation lives in GLSL where a C++ arithmetic mirror
// would drift from the code it claims to pin. So the shape of the source is the
// artifact under test, following IblShaderContractTests.
//
// Reads the repo shader source via GE_RENDERER_REPO_ROOT (dev-only anchor): a
// staged copy only refreshes when its staging target rebuilds, which a
// shader-only edit does not trigger, so the repo file is the artifact and is
// never stale.

#include <gtest/gtest.h>

#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/ShadowMinMaxPyramid.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

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

// Strip //-comments so prose describing the contract never satisfies it. Without
// this, a commented-out derivation would pass the presence assertions and a
// comment naming a deleted constant would fail the absence ones.
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

std::string ShadowSamplingSource()
{
#ifndef GE_RENDERER_REPO_ROOT
    return {};
#else
    const std::filesystem::path path = std::filesystem::path(GE_RENDERER_REPO_ROOT) /
                                       "Engine/Modules/Rendering/Shaders/Includes/shadow_sampling.glsl";
    return StripLineComments(ReadTextFile(path));
#endif
}

// The PCSS decisions shadow_sampling.glsl includes (penumbra clamp, search
// bound, lit proof).
std::string ShadowPcssDecisionsSource()
{
#ifndef GE_RENDERER_REPO_ROOT
    return {};
#else
    const std::filesystem::path path = std::filesystem::path(GE_RENDERER_REPO_ROOT) /
                                       "Engine/Modules/Rendering/Shaders/Includes/shadow_pcss_decisions.glsl";
    return StripLineComments(ReadTextFile(path));
#endif
}

// The cross-cascade blend band every cascade lookup with a blend shares.
std::string ShadowCascadeBlendSource()
{
#ifndef GE_RENDERER_REPO_ROOT
    return {};
#else
    const std::filesystem::path path = std::filesystem::path(GE_RENDERER_REPO_ROOT) /
                                       "Engine/Modules/Rendering/Shaders/Includes/shadow_cascade_blend.glsl";
    return StripLineComments(ReadTextFile(path));
#endif
}

// The local (area / spot / point) families live in a different file and use a
// different filter entirely — see the LocalShadowWorldRadiusContract tests.
std::string ClusteredLightingSource()
{
#ifndef GE_RENDERER_REPO_ROOT
    return {};
#else
    const std::filesystem::path path =
        std::filesystem::path(GE_RENDERER_REPO_ROOT) /
        "Engine/Modules/Rendering/Shaders/Includes/clustered_lighting.glsl";
    return StripLineComments(ReadTextFile(path));
#endif
}

} // namespace

TEST(PhysicalPenumbraShaderContract, SearchRadiusScalesWithAngularDiameter)
{
    const std::string src = ShadowSamplingSource();
    ASSERT_FALSE(src.empty()) << "shadow_sampling.glsl not found via GE_RENDERER_REPO_ROOT";

    // The radius is the light's tangent times a searched depth range, so a
    // larger light searches proportionally wider. Matched on tokens rather than
    // whole formatted lines so reformatting the shader does not fail this.
    EXPECT_NE(src.find("tanHalfAngle * searchDepthWorld"), std::string::npos)
        << "search radius must be derived from the light's angular size";
    EXPECT_NE(src.find("depthSpanWorld * kPcssSearchDepthFraction"), std::string::npos)
        << "the searched depth range must scale with the cascade's depth span";

    // The world-space clamp constants are what made the tangent unreachable.
    // Their return would silently restore the sub-texel search.
    EXPECT_EQ(src.find("kPcssMinBlockerSearchWorld"), std::string::npos)
        << "the world-space search clamp must not return — it clamped a tangent as metres";
    EXPECT_EQ(src.find("kPcssMaxBlockerSearchWorld"), std::string::npos)
        << "the world-space search clamp must not return — it clamped a tangent as metres";
}

TEST(PhysicalPenumbraShaderContract, SearchRadiusIsBoundedInTexelsNotWorld)
{
    const std::string src = ShadowSamplingSource();
    ASSERT_FALSE(src.empty()) << "shadow_sampling.glsl not found via GE_RENDERER_REPO_ROOT";

    // Bounds govern tap density, so they apply to the texel count. Because the
    // searched depth is a fraction of the cascade's depth span, and depth span
    // and worldPerTexel both scale with cascade extent, the physical radius then
    // lands at a near-constant texel count on every cascade — the invariance the
    // old world-space clamp claimed and could not deliver.
    EXPECT_NE(src.find("kPcssMinSearchTexels"), std::string::npos);
    EXPECT_NE(src.find("kPcssSearchTexelsPerRootTap"), std::string::npos);
    // The clamp() wrapper is the load-bearing token. The bare division existed
    // before this change too, so matching only "searchWorld / max(worldPerTexel"
    // would pass against the defective source and prove nothing.
    EXPECT_NE(src.find("clamp(searchWorld / max(worldPerTexel"), std::string::npos)
        << "the clamp must be applied to the texel count, not to the world radius";

    // Below 2 texels every Vogel tap can resolve into one texel under bilinear
    // snapping, which is the point-sample failure this whole change removes.
    EXPECT_NE(src.find("kPcssMinSearchTexels = 2.0"), std::string::npos)
        << "the floor must keep avgBlocker an average rather than a point sample";

    // Resolution independence: the sampling math must come from textureSize()
    // and worldPerTexel, never a baked extent. Slice C (dynamic-resolution
    // shadow maps) drops in underneath this only while it holds.
    //
    // Unlike the assertions above, this one already held before the change — it
    // is a forward guard on slice C's precondition, not a driver of this one.
    EXPECT_EQ(src.find("2048"), std::string::npos)
        << "no literal shadow resolution may appear in the sampling math";
}

// ── Dither basis (ShadowDitherBasis) ──
//
// The rotation the Vogel disk is spun by can be keyed on screen position (the
// shipped behaviour — the noise field is nailed to the screen, so geometry
// slides under it and the grain swims) or on the shadow coordinate (welded to
// the shadow lattice). Neither wins outright, which is why the shader carries
// both and the choice is a uniform rather than a define.

TEST(ShadowDitherBasisShaderContract, RotationSelectsOnTheBasisUniform)
{
    const std::string src = ShadowSamplingSource();
    ASSERT_FALSE(src.empty()) << "shadow_sampling.glsl not found via GE_RENDERER_REPO_ROOT";

    // A uniform, not a #define: the basis is an A/B knob answered live at a
    // given camera range, and a compile-time switch cannot be A/B'd by eye.
    EXPECT_NE(src.find("ge_shadowFilterParams"), std::string::npos)
        << "the basis must come from the uniform block, not a compile-time define";

    // All three inputs always passed, so no call site decides the basis for
    // itself. Two call sites feed this (the blocker search and the PCF loop) and
    // they must agree — a fragment whose search and filter disagree on the
    // rotation samples two different kernels.
    EXPECT_NE(src.find("GE_PoissonRotation(vec2 screenPos, vec2 shadowUV, vec2 texSize)"),
              std::string::npos)
        << "call sites must not select the basis themselves";
}

TEST(ShadowDitherBasisShaderContract, ShadowSpaceKeyingIsContinuousNotFloored)
{
    const std::string src = ShadowSamplingSource();
    ASSERT_FALSE(src.empty());

    // Flooring collapses every screen pixel inside one shadow texel onto a
    // single rotation. Under magnification — which is exactly the far-range case
    // this slice exists for — that trades fine grain for texel-sized blocks, so
    // the continuous product is the whole value of the shadow-space basis.
    // clustered_lighting.glsl's local families DO floor; this must not copy them.
    EXPECT_NE(src.find("shadowUV * texSize"), std::string::npos)
        << "shadow-space keying must use the continuous product";
    EXPECT_EQ(src.find("floor(shadowUV"), std::string::npos)
        << "flooring the shadow coordinate blocks the noise under magnification";
}

// ── Penumbra width in world units, not texels ──
//
// The defect these pin: every bound on the penumbra was a multiple of
// worldPerTexel, so halving shadow resolution DOUBLED the permitted penumbra in
// metres while the tap count stayed put. Measured on ShadowStress with the
// cascade fit held identical (half-extents 15/24/40/80 in both arms): median
// edge-width ratio 2.12 across 12 paired samples, 2048 vs 1024.
//
// The raw penumbra was always correct — depthDelta * tan(halfAngle) has no
// texel term. The disease was entirely in the clamps.

TEST(PenumbraWorldClampContract, CeilingIsNotProportionalToWorldPerTexel)
{
    const std::string src = ShadowSamplingSource();
    ASSERT_FALSE(src.empty()) << "shadow_sampling.glsl not found via GE_RENDERER_REPO_ROOT";

    // This exact product IS the defect. Its return would silently restore a
    // penumbra that changes size with the resolution slider.
    EXPECT_EQ(src.find("sqrt(float(GE_PcssTapCount())) * worldPerTexel"), std::string::npos)
        << "the penumbra ceiling must not scale with texel size";

    EXPECT_NE(src.find("kPcssMaxPenumbraWorldAuto"), std::string::npos)
        << "the auto ceiling must be a world constant, uniform across cascades";
}

// The floor stays at one texel and that is deliberate: a penumbra narrower than
// a texel IS a hard edge, which is a sampling limit rather than a bug. So the
// invariance this slice buys holds only ABOVE one texel of the coarsest family
// involved, and it degrades earlier at lower resolution. Pinned so the
// guarantee is not later read as stronger than it is.
TEST(PenumbraWorldClampContract, FloorRemainsOneTexel)
{
    const std::string src = ShadowPcssDecisionsSource();
    ASSERT_FALSE(src.empty());

    EXPECT_NE(src.find("clamp(penumbraWorld, worldPerTexel"), std::string::npos)
        << "the one-texel floor is a sampling limit and must stay";
}

TEST(ShadowDitherBasisShaderContract, ScreenBasisExpressionIsPreserved)
{
    const std::string src = ShadowSamplingSource();
    ASSERT_FALSE(src.empty());

    // Screen is the default, so an unconfigured project must render exactly what
    // it rendered before this slice. These are the interleaved-gradient-noise
    // constants (Jimenez 2014) that define that field; losing them would change
    // every existing scene silently.
    //
    // This one passes before the change as well as after — it is the dark-ship
    // guard, not a driver.
    EXPECT_NE(src.find("vec2(0.06711056, 0.00583715)"), std::string::npos)
        << "the IGN constants are the shipped screen-space field";
    EXPECT_NE(src.find("52.9829189"), std::string::npos);
}

// ── Local families: filter radius in resolution-free UV ──
//
// Area, spot and point do NOT share one filter, and the fix differs per family.
// Measured on the point atlas (T4): pinning a light's tier Low(256) vs
// High(1024) moved its shadow edge width 3.3x against a control that moved <1%,
// because every radius was written as a multiple of texelSize = 1/resolution.
//
// UV is resolution-independent by construction, so expressing the radius as a
// constant in UV removes the resolution dependence while KEEPING the
// distance-dependent behaviour these perspective lights want (worldPerTexel
// grows with receiver distance, which reads as a soft-with-distance edge).

TEST(LocalShadowWorldRadiusContract, SpotAndPointRadiusIsNotATexelMultiple)
{
    const std::string src = ClusteredLightingSource();
    ASSERT_FALSE(src.empty()) << "clustered_lighting.glsl not found via GE_RENDERER_REPO_ROOT";

    // These two products ARE the defect. Spot and point have no blocker search
    // and no penumbra — just a fixed 12-tap PCF whose radius was 1.5 texels, so
    // halving the map doubled the blur in world units.
    EXPECT_EQ(src.find("1.5 * texelSize"), std::string::npos)
        << "spot filter radius must not be a texel multiple";
    EXPECT_EQ(src.find("1.5 * tileTexel"), std::string::npos)
        << "point filter radius must not be a tile-texel multiple";

    EXPECT_NE(src.find("kLocalFilterRadiusUV"), std::string::npos)
        << "the radius must be a constant in UV, which is resolution-free";
}

TEST(LocalShadowWorldRadiusContract, AreaClampsAreNotTexelMultiples)
{
    const std::string src = ClusteredLightingSource();
    ASSERT_FALSE(src.empty());

    // Area DOES have a penumbra (blocker search + variable radius); only its
    // bounds were texel multiples. Deliberately NOT asserted for spot/point:
    // they never had these clamps, so such an assertion would pass today and
    // prove nothing.
    EXPECT_EQ(src.find("32.0 * texelSize"), std::string::npos)
        << "the area penumbra ceiling must not scale with texel size";
    EXPECT_EQ(src.find("1.25 * texelSize"), std::string::npos)
        << "the area penumbra floor must not scale with texel size";
}

// The reference resolution is what keeps this a pure de-coupling rather than a
// look change: the UV constants are chosen to equal the old texel-derived
// values at that resolution, so the default tier renders as it did and only
// the DEPENDENCE on resolution is removed.
TEST(LocalShadowWorldRadiusContract, ReferenceResolutionIsStated)
{
    const std::string src = ClusteredLightingSource();
    ASSERT_FALSE(src.empty());

    EXPECT_NE(src.find("kLocalReferenceRes"), std::string::npos)
        << "the UV constants must be derived from a named reference resolution";
}

// ── DPCF (Treyarch, Cold War) ──
//
// Contact hardening from ONE tap set. The occluder count and the occluder
// DISTANCE SUM come from the same gathered depths, so there is no
// blocker-search pass and no dependent texture read — that is the property the
// whole technique exists for, and the reason it fit a gen8 60 Hz budget where
// PCSS did not.
//
// The curve folds the occluded percentage about 0.5, applies a power there, and
// unfolds — pushing values AWAY from the midpoint by how close the average
// occluder is. Below 0.5 that reduces shadowing (which is what "naturally
// reduces acne" means: a surface self-shadowing at ~25% comes out at ~6%).

TEST(DpcfShaderContract, OccluderStatsComeFromTheSameTapsAsThePcf)
{
    const std::string src = ShadowSamplingSource();
    ASSERT_FALSE(src.empty()) << "shadow_sampling.glsl not found via GE_RENDERER_REPO_ROOT";

    // Both accumulators must exist, and they must be fed by one loop. A separate
    // GE_BlockerSearch call inside the DPCF path would reintroduce exactly the
    // dependent read the technique removes.
    EXPECT_NE(src.find("occluderDistSum"), std::string::npos)
        << "DPCF needs the occluder distance sum, not just the occluded count";
    EXPECT_NE(src.find("GE_DpcfCurve"), std::string::npos);
}

// The pyramid is currently gated on effective == PCSS, so it is OFF under DPCF
// and this property is not load-bearing today — it is the precondition for ever
// turning it on. The early-outs return exactly 1.0 for "no blockers in range"
// and 0.0 for "everything in range is a blocker", which DPCF reproduces only
// while its curve maps 0 -> 0 and 1 -> 1. Pinned now so that enabling the
// pyramid for DPCF later is a gate change rather than a silent correctness bug;
// the conservativeness of the fully-shadowed test would still need re-deriving,
// because DPCF does not use the PCF loop's per-tap reference bias.
TEST(DpcfShaderContract, CurvePreservesEndpointsSoThePyramidStaysExact)
{
    const std::string src = ShadowSamplingSource();
    ASSERT_FALSE(src.empty());

    // The fold is what guarantees it: 1 - |2p-1| is 0 at BOTH ends, any power of
    // 0 is 0, and the unfold puts it back on the endpoint it came from.
    EXPECT_NE(src.find("float GE_DpcfCurve("), std::string::npos);
    EXPECT_NE(src.find("sign("), std::string::npos)
        << "the fold about 0.5 needs the sign to unfold onto the correct side";
}

TEST(DpcfShaderContract, IsAdditionalToPcssRatherThanAReplacement)
{
    const std::string src = ShadowSamplingSource();
    ASSERT_FALSE(src.empty());

    // DPCF approximates contact hardening; PCSS derives a physical penumbra from
    // the light's angular size. They are different points on the cost/quality
    // curve, so the PCSS path must survive DPCF landing.
    EXPECT_NE(src.find("GE_BlockerSearch"), std::string::npos)
        << "PCSS keeps its blocker search — DPCF is an additional quality, not a replacement";
}

TEST(ShadowReceiverPlaneContract, CapturesDerivativesBeforeDivergentReceiverBranches)
{
    const std::string src = ShadowSamplingSource();
    ASSERT_FALSE(src.empty());
    const auto sampler = src.find("float GE_SampleShadow(");
    const auto derivativeX = src.find("GE_DFDX(posWS)", sampler);
    const auto derivativeY = src.find("GE_DFDY(posWS)", sampler);
    const auto receiveGate = src.find("if (ge_ReceiveShadows", sampler);
    ASSERT_NE(derivativeX, std::string::npos);
    ASSERT_NE(derivativeY, std::string::npos);
    ASSERT_NE(receiveGate, std::string::npos);
    EXPECT_LT(derivativeX, receiveGate);
    EXPECT_LT(derivativeY, receiveGate);
    const auto cascade = src.find("float GE_SampleCascade(");
    ASSERT_NE(cascade, std::string::npos);
    EXPECT_EQ(src.substr(cascade, sampler - cascade).find("dFdx("), std::string::npos);
    EXPECT_EQ(src.substr(cascade, sampler - cascade).find("dFdy("), std::string::npos);
    EXPECT_EQ(src.substr(cascade, sampler - cascade).find("GE_DFDX("), std::string::npos);
    EXPECT_EQ(src.substr(cascade, sampler - cascade).find("GE_DFDY("), std::string::npos);
}

// PCSS leaves as lit only through the pyramid's proof that no tap of any kernel
// up to the cap is occluded. A blocker search that finds nothing sizes the
// minimum kernel and the filter decides; it is not a way out as lit.
TEST(PcssLitExitContract, LitOnlyFromTheKernelProof)
{
    const std::string src = ShadowSamplingSource();
    ASSERT_FALSE(src.empty()) << "shadow_sampling.glsl not found via GE_RENDERER_REPO_ROOT";
    const auto pcss = src.find("if (quality == 3 && int(ge_shadowDebug.y) != 0)");
    ASSERT_NE(pcss, std::string::npos);
    const auto poisson = src.find("if (quality == 2)", pcss);
    ASSERT_NE(poisson, std::string::npos);
    const std::string block = src.substr(pcss, poisson - pcss);
    const auto proof = block.find("GE_PcssKernelProvesLit(");
    ASSERT_NE(proof, std::string::npos);
    // Any spelling of a constant-one return: 1, 1.0, 1.0f, with any spacing.
    const std::regex litReturn(R"(return\s+1(\.0*)?f?\s*;)");
    std::vector<size_t> litReturns;
    for (auto it = std::sregex_iterator(block.begin(), block.end(), litReturn);
         it != std::sregex_iterator(); ++it)
        litReturns.push_back(static_cast<size_t>(it->position()));
    ASSERT_EQ(litReturns.size(), 1u) << "exactly one lit return in the PCSS path";
    EXPECT_GT(litReturns[0], proof) << "the lit return follows the kernel proof";
    const auto search = block.find("GE_BlockerSearch(");
    ASSERT_NE(search, std::string::npos);
    EXPECT_GT(search, litReturns[0]) << "the search runs only once the proof failed";
}

// The lit proof is safe only over the widest kernel the filter can pick, so the
// pyramid is queried at the kernel cap, not at the (possibly narrower) bounded
// search radius.
TEST(PcssLitExitContract, PyramidIsQueriedAtTheKernelCap)
{
    const std::string src = ShadowSamplingSource();
    ASSERT_FALSE(src.empty());
    const std::string flat = std::regex_replace(src, std::regex(R"(\s+)"), " ");
    EXPECT_NE(flat.find("GE_PcssPyramidMinMax(shadowUV, shadowSize, pyramidIdx, pyramidLevels, "
                        "pyramidBaseShift, pcfCapTexels)"),
              std::string::npos);
    EXPECT_EQ(flat.find("pyramidBaseShift, searchTexels)"), std::string::npos)
        << "the pyramid queried at the search radius proves nothing about the kernel";
}

// The pyramid's level count is sized on the CPU from the kernel cap and the
// query margin, so the shader's constants and the CPU's must agree or the lit
// proof's query can outgrow the pyramid.
TEST(PcssLitExitContract, CpuMirrorsOfTheQueryConstantsMatchTheShader)
{
    const std::string src = ShadowSamplingSource();
    ASSERT_FALSE(src.empty());
    const auto constant = [&src](const char* name) -> double
    {
        std::smatch m;
        const std::regex re(std::string("const float ") + name + R"(\s*=\s*([0-9.]+)\s*;)");
        if (!std::regex_search(src, m, re))
            return -1.0;
        return std::stod(m[1].str());
    };
    EXPECT_DOUBLE_EQ(constant("kPcssPyramidQueryMargin"),
                     static_cast<double>(GameEngine::Engine::Renderer::ShadowMinMaxPyramid::kQueryMarginTexels));
    EXPECT_DOUBLE_EQ(constant("kPcssMaxPenumbraWorldAuto"),
                     static_cast<double>(GameEngine::Engine::Renderer::kPcssMaxPenumbraWorldAuto));
}

// ── Distance fade (MaxShadowDistance) ──
//
// ShadowSettingsEffect promises that directional shadows fade out at
// MaxShadowDistance. Every directional term goes through one band,
// GE_ShadowDistanceFade, whose falloff
// ShadowFilterComputeTest.DistanceFadeEndsShadowsSmoothlyAtMaxShadowDistance
// pins on the GPU. A cascade term that skips it returns its full value up to the
// distance and fully lit past it: coverage then ends on a hard line that moves
// with the camera, and only the contact term fades.

TEST(ShadowDistanceFadeContract, CascadeTermFadesBeforeMaxShadowDistance)
{
    const std::string src = ShadowSamplingSource();
    ASSERT_FALSE(src.empty()) << "shadow_sampling.glsl not found via GE_RENDERER_REPO_ROOT";
    const auto sampler = src.find("float GE_SampleShadow(");
    ASSERT_NE(sampler, std::string::npos);
    const auto cascadePath = src.find("int numCascades = int(ge_shadowParams.z);", sampler);
    ASSERT_NE(cascadePath, std::string::npos);
    const auto contact =
        src.find("shadow = GE_ScreenSpaceShadow(shadow, linearDepth, screenPos);", cascadePath);
    ASSERT_NE(contact, std::string::npos);
    const std::string cascade = src.substr(cascadePath, contact - cascadePath);

    const auto fade = cascade.find("GE_ShadowDistanceFade(linearDepth, ge_shadowParams.w, ge_shadowFilterParams.y)");
    EXPECT_NE(fade, std::string::npos) << "the cascade term must fade before MaxShadowDistance";
    // The cascade factor reaches the fade through the join with the terrain's
    // term (GE_JoinTerrainAfterFade, pinned on the GPU by
    // ShadowFilterComputeTest.DistanceFadeEndsTheCascadeTermAndLeavesTheTerrainTerm).
    const auto join = cascade.find(
        "shadow = GE_JoinTerrainAfterFade(shadow, GE_TerrainShadow(posWS, maxPenumbraWorld), linearDepth, ge_shadowParams.w, ge_shadowFilterParams.y)");
    EXPECT_NE(join, std::string::npos) << "the fade must reach the cascade shadow factor";
    // After the cross-cascade blend, so the fade applies to the blended value
    // when the band reaches back into the previous cascade.
    const auto blend = cascade.find("mix(nextShadow, shadow, cascadeWeight)");
    ASSERT_NE(blend, std::string::npos);
    EXPECT_LT(blend, fade);
    EXPECT_LT(blend, join);
    // Glass transmittance is directional shadowing too; it must not outlive the
    // occlusion it tints.
    EXPECT_NE(cascade.find("mix(tintSample.rgb, vec3(1.0), distanceFade)"), std::string::npos)
        << "the glass tint must fade with the cascade term";
    EXPECT_NE(cascade.find("tintSample.a * (1.0 - distanceFade)"), std::string::npos)
        << "the caustic dapple's glass presence must fade with the cascade term";
}

TEST(ShadowDistanceFadeContract, ContactTermSharesTheCascadeBand)
{
    const std::string src = ShadowSamplingSource();
    ASSERT_FALSE(src.empty()) << "shadow_sampling.glsl not found via GE_RENDERER_REPO_ROOT";
    const auto contact = src.find("float GE_ScreenSpaceShadow(");
    const auto sampler = src.find("float GE_SampleShadow(");
    ASSERT_NE(contact, std::string::npos);
    ASSERT_NE(sampler, std::string::npos);
    ASSERT_LT(contact, sampler);
    const std::string contactTerm = src.substr(contact, sampler - contact);
    EXPECT_NE(contactTerm.find("GE_ShadowDistanceFade(linearDepth, ge_shadowParams.w, ge_shadowFilterParams.y)"),
              std::string::npos)
        << "the contact term must fade over the same band as the cascades";
    // The fade must reach the stored result, not only be computed.
    EXPECT_NE(contactTerm.find("ge_lastContactShadowResult = mix(visibility, 1.0, fade)"),
              std::string::npos)
        << "the contact term's stored result must fade with the cascades";
    // One definition of the band: a second literal drifts from the first.
    EXPECT_EQ(src.find("ge_shadowParams.w * 0.9"), std::string::npos)
        << "the fade band must come from GE_ShadowDistanceFade only";
}

// ── Cross-cascade blend band ──
//
// A receiver this close before its cascade's far split also samples the next
// cascade. A band with a world-unit floor (3 m) outgrew every cascade at viewer
// scale, where SDSM fits cascades a few tenths of a metre deep: the finer
// cascade never won and most of the screen evaluated the filter twice. The
// band is a fraction of each cascade's own range, on the GPU and in the
// receiver fit's CPU mirror alike.

TEST(CascadeBlendBandContract, BandIsTheShaderFractionOfEachCascadeRangeWithNoFloor)
{
    const std::string blend = ShadowCascadeBlendSource();
    ASSERT_FALSE(blend.empty()) << "shadow_cascade_blend.glsl not found via GE_RENDERER_REPO_ROOT";
    std::smatch m;
    ASSERT_TRUE(std::regex_search(blend, m, std::regex(R"(const float kCascadeBlendFraction\s*=\s*([0-9.]+)\s*;)")));
    const float fraction = std::stof(m[1].str());
    ASSERT_GT(fraction, 0.0f);
    ASSERT_LT(fraction, 1.0f) << "a band as long as the cascade leaves the finer cascade nothing";
    EXPECT_NE(blend.find("return cascadeRange * kCascadeBlendFraction;"), std::string::npos);
    const std::string sampling = ShadowSamplingSource();
    EXPECT_NE(sampling.find("float blendBand = GE_CascadeBlendBand(cascadeRange);"), std::string::npos)
        << "the cascade lookup must take its band from GE_CascadeBlendBand";

    // SDSM splits at a viewer-scale pose (a scene a few metres across seen
    // from about 2 m) and the fitted splits without SDSM at the same pose.
    using GameEngine::Engine::Renderer::CascadeFrameData;
    using GameEngine::Engine::Renderer::ShadowMapRenderFeature;
    for (const auto& splits : {std::vector<float>{1.231f, 1.474f, 1.751f, 2.068f},
                               std::vector<float>{0.456f, 1.026f, 2.230f, 6.650f}})
    {
        CascadeFrameData frame{};
        frame.NumCascades = 4;
        for (uint32_t c = 0; c < 4; ++c)
            frame.SplitDistances[c] = splits[c];
        for (uint32_t c = 0; c < 3; ++c)
        {
            const float range = splits[c] - (c == 0 ? 0.0f : splits[c - 1]);
            EXPECT_FLOAT_EQ(ShadowMapRenderFeature::CascadeBlendBand(frame, c), range * fraction)
                << "cascade " << c << " of " << splits[3] << " m";
        }
        EXPECT_EQ(ShadowMapRenderFeature::CascadeBlendBand(frame, 3), 0.0f) << "the last cascade blends into nothing";
    }
}
