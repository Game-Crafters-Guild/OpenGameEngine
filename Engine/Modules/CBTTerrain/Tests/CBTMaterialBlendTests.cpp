// Behavioural tests for the per-fragment terrain material selection — the weight floor, the
// duplicate-role fold and the top-K rank + renormalize. Pure weight arithmetic, no Vulkan device
// and no samplers.
//
// These tests execute the SHIPPED shader code. The selection block of
// Includes/terrain_blend_resolve.glsl is extracted verbatim at build time (ExtractShaderBlock.cmake)
// and compiled here through GlslShim.h, so an edit to the shader is an edit to what these tests
// measure. A hand-written C++ mirror would pass forever while the shader drifted underneath it,
// which is exactly the failure this arrangement exists to prevent.
//
// That include is the ONE resolve both terrain surfaces run — the CBT ground and the grass standing
// on it — so these cases cover the ground and the blade tint together.
//
// CBT_MAX_BLEND_MATERIALS is the one thing the block reads but does not define, which is what lets
// each width below be a separate instantiation of the same shipped source. The value that actually
// SHIPS is pinned separately by CBTLayout.SurfaceBlendsThreeMaterialsPerFragment; nothing here can
// stand in for that, because every width compiles by construction.

#include <gtest/gtest.h>

#include "GlslShim.h"

#include <array>
#include <cmath>

namespace
{
using GameEngine::GlslShim::uint;
using GameEngine::GlslShim::vec4;
// The block calls max() on two floats, which ADL cannot resolve into the shim's namespace.
using GameEngine::GlslShim::max;

// One instantiation of the shipped block per blend width. The block is a set of free functions, so
// each namespace gets its own copy compiled against its own CBT_MAX_BLEND_MATERIALS.
namespace K2
{
#define CBT_MAX_BLEND_MATERIALS 2
#include "CBTBlendSelectionExtracted.h"
#undef CBT_MAX_BLEND_MATERIALS
} // namespace K2

namespace K3
{
#define CBT_MAX_BLEND_MATERIALS 3
#include "CBTBlendSelectionExtracted.h"
#undef CBT_MAX_BLEND_MATERIALS
} // namespace K3

namespace K4
{
#define CBT_MAX_BLEND_MATERIALS 4
#include "CBTBlendSelectionExtracted.h"
#undef CBT_MAX_BLEND_MATERIALS
} // namespace K4

// The tolerance is loose relative to fp32 error on four adds and a divide, and tight relative to
// the thing under test: a lost or double-counted contributor moves the sum by O(0.1), not O(1e-6).
constexpr float kSumTolerance = 1e-6f;

float Sum(const vec4& v)
{
    return v[0] + v[1] + v[2] + v[3];
}

int NonZeroCount(const vec4& v)
{
    int n = 0;
    for (int i = 0; i < 4; ++i)
    {
        if (v[i] != 0.0f)
            ++n;
    }
    return n;
}

// Four channels each naming a different material — the case where the fold is a no-op and ranking
// alone decides.
std::array<uint, 4> DistinctRoles()
{
    return {0u, 1u, 2u, 3u};
}

} // namespace

// (1) The renormalization: survivors sum to 1, so cutting a contributor cannot darken the surface.
// The 0.5/0.3/0.2 case is the transition band the change was made for — three materials well clear
// of the floor, one of which K=2 must drop.
TEST(CBTMaterialBlend, SurvivorsSumToOneAfterSelection)
{
    const std::array<vec4, 5> cases = {
        vec4(0.5f, 0.3f, 0.2f, 0.0f), // the band case
        vec4(0.34f, 0.33f, 0.33f, 0.0f),
        vec4(0.7f, 0.2f, 0.05f, 0.05f),
        vec4(0.25f, 0.25f, 0.25f, 0.25f),
        vec4(1.0f, 0.0f, 0.0f, 0.0f), // single contributor: already normalized
    };

    auto roles = DistinctRoles();
    for (const vec4& in : cases)
    {
        const vec4 out = K2::CBT_ResolveBlendWeights(in, roles.data());
        EXPECT_NEAR(Sum(out), 1.0f, kSumTolerance)
            << "input (" << in[0] << ", " << in[1] << ", " << in[2] << ", " << in[3] << ")";
    }
}

// (2) Selection keeps the heaviest and zeroes the rest EXACTLY — the blend loop skips on `<= 0.0`,
// so a dropped channel that merely became small would still be fetched.
//
// The surviving weights are the SOFT ones: each survivor is reduced by the heaviest dropped weight
// (0.2 here) before renormalizing, which is what makes the cut continuous. That pulls the blend
// toward the dominant material relative to a hard cut — 0.75/0.25 rather than 0.625/0.375 — and
// that shift is the visible price of having no seam.
TEST(CBTMaterialBlend, KeepsTheTwoHeaviestAndZeroesTheRest)
{
    auto roles = DistinctRoles();
    const vec4 in(0.5f, 0.3f, 0.2f, 0.0f);
    const vec4 out = K2::CBT_ResolveBlendWeights(in, roles.data());

    EXPECT_EQ(NonZeroCount(out), 2);
    EXPECT_EQ(out[2], 0.0f) << "the third-heaviest must be exactly zero, not merely small";
    EXPECT_EQ(out[3], 0.0f);
    // (0.5 - 0.2) and (0.3 - 0.2), renormalized over their own sum of 0.4.
    EXPECT_NEAR(out[0], 0.3f / 0.4f, kSumTolerance);
    EXPECT_NEAR(out[1], 0.1f / 0.4f, kSumTolerance);
}

// Continuity across a rank-swap contour — the property the soft cut exists for, and the one a hard
// top-K fails. Channels 1 and 2 cross at 0.25: on one side channel 1 takes the second slot, on the
// other channel 2 does. A hard cut steps the blend by the shared weight's share of it; the soft cut
// sends BOTH candidates to exactly zero on the contour, so the two sides meet there.
//
// This is not a corner case on a painted terrain. Wherever three materials are brushed across each
// other the contour is a long line through the middle of the band, which is what made the first
// implementation's seam a visible edge rather than a stray pixel.
TEST(CBTMaterialBlend, BlendIsContinuousAcrossARankSwap)
{
    auto roles = DistinctRoles();
    constexpr float kEps = 1e-4f;

    const vec4 below(0.5f, 0.25f + kEps, 0.25f - kEps, 0.0f); // channel 1 takes the second slot
    const vec4 above(0.5f, 0.25f - kEps, 0.25f + kEps, 0.0f); // channel 2 takes it
    const vec4 tie(0.5f, 0.25f, 0.25f, 0.0f);                 // exactly on the contour

    const vec4 a = K2::CBT_ResolveBlendWeights(below, roles.data());
    const vec4 b = K2::CBT_ResolveBlendWeights(above, roles.data());
    const vec4 t = K2::CBT_ResolveBlendWeights(tie, roles.data());

    // A hard cut steps ~0.33 here (the shared 0.25 over the surviving sum), so this bound separates
    // "continuous" from "seam" by more than two orders of magnitude — it cannot pass by luck.
    const float kContinuityBound = 20.0f * kEps;
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_NEAR(a[i], b[i], kContinuityBound) << "component " << i << " steps across the swap";
        EXPECT_NEAR(a[i], t[i], kContinuityBound) << "component " << i << " vs the exact tie";
        EXPECT_NEAR(b[i], t[i], kContinuityBound) << "component " << i << " vs the exact tie";
    }
    EXPECT_NEAR(Sum(t), 1.0f, kSumTolerance) << "the tie itself must still be a full-strength blend";
}

// (3) Fold before rank. Channels 1 and 2 name ONE material at 0.3 + 0.3; channel 0 is a different
// material at 0.4. Ranked per channel, the 0.4 and one 0.3 would survive and the shared material
// would be under-represented at 0.3. Folded first, the shared material leads at 0.6.
//
// This is the case the ordering comment in the shader claims, and the reason the fold cannot simply
// be a fetch-side optimization applied after selection.
TEST(CBTMaterialBlend, FoldedDuplicatesCompeteOnTheirCombinedWeight)
{
    std::array<uint, 4> sharedRoles = {7u, 9u, 9u, 0u};
    const vec4 in(0.4f, 0.3f, 0.3f, 0.0f);

    const vec4 folded = K2::CBT_FoldDuplicateRoles(in, sharedRoles.data());
    EXPECT_NEAR(folded[1], 0.6f, kSumTolerance) << "both channels of material 9 fold into the first";
    EXPECT_EQ(folded[2], 0.0f) << "the duplicate channel must be consumed, not left to be fetched";
    EXPECT_NEAR(Sum(folded), 1.0f, kSumTolerance) << "folding conserves total weight";

    // Through the SHIPPED composition, not a hand-written call order — the ordering is the thing
    // under test, so the test must not be free to choose it. Inverting the two steps inside
    // CBT_ResolveBlendWeights is what has to turn this red.
    const vec4 out = K2::CBT_ResolveBlendWeights(in, sharedRoles.data());
    EXPECT_NEAR(out[1], 0.6f, kSumTolerance) << "the shared material leads the blend";
    EXPECT_NEAR(out[0], 0.4f, kSumTolerance);
    EXPECT_NEAR(Sum(out), 1.0f, kSumTolerance);

    // The pre-fold ranking this ordering exists to avoid: material 9 enters at 0.3 per channel, so
    // it ties the contender it should have beaten — and the soft cut then fades it to nothing. The
    // material that should LEAD the blend disappears from it entirely.
    const vec4 rankedFirst = K2::CBT_SelectTopWeights(in);
    EXPECT_NEAR(rankedFirst[0], 1.0f, kSumTolerance) << "the lesser material takes the whole pixel";
    EXPECT_EQ(rankedFirst[1], 0.0f) << "ranking before folding erases the shared material";
}

// (4) K = 4 keeps every distinct contributor, so nothing is dropped, so the softening subtracts
// zero. The result must therefore be EXACTLY the plain renormalization — the soft cut is required
// to cost nothing at all on the path where it has no work to do.
//
// Asserted bitwise rather than approximately: with dropMax == 0 the shader computes
// max(w - 0, 0) / sum, which is the same arithmetic in the same order as the plain w / sum below.
// A tolerance here would hide exactly the kind of drift this is meant to catch.
TEST(CBTMaterialBlend, WidthFourIsExactlyThePlainRenormalization)
{
    auto roles = DistinctRoles();
    const vec4 in(0.4f, 0.3f, 0.2f, 0.1f);
    const vec4 out = K4::CBT_ResolveBlendWeights(in, roles.data());

    EXPECT_EQ(NonZeroCount(out), 4);
    const float renormSum = ((in[0] + in[1]) + in[2]) + in[3];
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(out[i], in[i] / renormSum) << "component " << i << " is not the plain renorm";
    EXPECT_NEAR(Sum(out), 1.0f, kSumTolerance);

    // K = 3 on the same input drops the lightest, so the softening DOES engage: the survivors are
    // each reduced by 0.1 before renormalizing over their reduced sum of 0.3.
    const vec4 three = K3::CBT_ResolveBlendWeights(in, roles.data());
    EXPECT_EQ(NonZeroCount(three), 3);
    EXPECT_EQ(three[3], 0.0f) << "the lightest contributor is the one cut";
    EXPECT_NEAR(three[0], 0.3f / 0.6f, kSumTolerance); // (0.4 - 0.1) over the reduced sum
    EXPECT_NEAR(three[1], 0.2f / 0.6f, kSumTolerance);
    EXPECT_NEAR(three[2], 0.1f / 0.6f, kSumTolerance);
    EXPECT_NEAR(Sum(three), 1.0f, kSumTolerance);
}

// The shipped width's reference claim: at three or fewer active materials K = 3 drops nothing, so
// the softening subtracts zero and the fragment is BITWISE the full blend. This is what makes the
// shipped default an exact result over everything except a four-material corner, and it is the one
// property that would silently stop holding if the subtraction ever gained a floor or an epsilon.
//
// Bitwise for the same reason as the K = 4 case above: a tolerance would hide the drift.
TEST(CBTMaterialBlend, ShippedWidthIsExactWhereNothingIsDropped)
{
    auto roles = DistinctRoles();
    const std::array<vec4, 3> withinBudget = {
        vec4(0.5f, 0.3f, 0.2f, 0.0f), // the three-material band
        vec4(0.6f, 0.4f, 0.0f, 0.0f), // an ordinary two-material transition
        vec4(1.0f, 0.0f, 0.0f, 0.0f), // one material
    };

    for (const vec4& in : withinBudget)
    {
        const vec4 out = K3::CBT_ResolveBlendWeights(in, roles.data());
        const float renormSum = ((in[0] + in[1]) + in[2]) + in[3];
        for (int i = 0; i < 4; ++i)
        {
            EXPECT_EQ(out[i], in[i] / renormSum)
                << "component " << i << " of (" << in[0] << ", " << in[1] << ", " << in[2] << ", "
                << in[3] << ") is not bitwise the full blend";
        }
    }
}

// (5) Degenerate inputs. All-equal weights make the max scan ambiguous, and an all-zero fragment
// makes the renormalization a division by zero — the branch that guards it is the only thing
// between this and a NaN written to the G-buffer.
TEST(CBTMaterialBlend, DegenerateWeightSetsStayFinite)
{
    auto roles = DistinctRoles();

    // All four exactly equal is the one input the soft cut cannot reduce: every survivor ties the
    // heaviest dropped weight, so the subtraction takes the entire blend to zero. The fallback must
    // catch that and renormalize the UNREDUCED survivors — otherwise this pixel divides by zero and
    // shades black or NaN.
    const vec4 equal =
        K2::CBT_ResolveBlendWeights(vec4(0.25f, 0.25f, 0.25f, 0.25f), roles.data());
    EXPECT_EQ(NonZeroCount(equal), 2) << "ties resolve to exactly K survivors, not more";
    EXPECT_NEAR(Sum(equal), 1.0f, kSumTolerance) << "the degenerate fragment must not darken";
    EXPECT_NEAR(equal[0], 0.5f, kSumTolerance) << "survivors share the pixel evenly";
    EXPECT_NEAR(equal[1], 0.5f, kSumTolerance);
    for (int i = 0; i < 4; ++i)
        EXPECT_TRUE(std::isfinite(equal[i])) << "component " << i << " is not finite";

    // Every channel below the floor: the fold zeroes them all and selection has nothing to keep.
    // The result must be finite and must not be renormalized into existence.
    const vec4 tiny =
        K2::CBT_ResolveBlendWeights(vec4(0.0001f, 0.0001f, 0.0f, 0.0f), roles.data());
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_TRUE(std::isfinite(tiny[i])) << "component " << i << " is not finite";
        EXPECT_EQ(tiny[i], 0.0f);
    }

    const vec4 zero =
        K2::CBT_ResolveBlendWeights(vec4(0.0f), roles.data());
    for (int i = 0; i < 4; ++i)
        EXPECT_TRUE(std::isfinite(zero[i])) << "component " << i << " is not finite";
}

// The floor is what makes "zero" mean "not fetched". A channel just under it is dropped; a channel
// just over it survives — and the fold runs after, so two sub-floor channels of one material cannot
// sum their way back into the blend.
TEST(CBTMaterialBlend, WeightFloorCutsBeforeTheFoldCanRestore)
{
    std::array<uint, 4> sharedRoles = {5u, 5u, 1u, 0u};
    // Two sub-floor channels of material 5. Folded first they would total 0.0018 and survive.
    const vec4 out = K2::CBT_FoldDuplicateRoles(vec4(0.0009f, 0.0009f, 0.998f, 0.0f),
                                                sharedRoles.data());
    EXPECT_EQ(out[0], 0.0f) << "a sub-floor channel must not be revived by folding";
    EXPECT_EQ(out[1], 0.0f);
    EXPECT_NEAR(out[2], 0.998f, kSumTolerance);
}
