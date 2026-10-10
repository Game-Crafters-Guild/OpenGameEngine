// Headless tests for the point-shadow atlas planner (arc slice M1). Three
// concerns, all exercised without a device or RenderServices:
//   1. Tier hysteresis — a boundary-hovering light does NOT re-tier; a decisive
//      coverage crossing does; a cooldown freezes the tier after a change.
//   2. Slot allocation / eviction — the budget caps admission; a light that
//      leaves the candidate set frees its slot and re-admits fresh; an incumbent
//      is only evicted by a challenger that out-covers it by the hysteresis
//      factor; slot indices are sticky across frames.
//   3. Tile-local sampling math — the pure tile→atlas UV helpers that mirror the
//      receiver GLSL (resolution/scale/clamp), so the §4.5 math is pinned here.

#include "Engine/Rendering/PointShadowAtlasPlanner.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace
{
using GameEngine::Engine::Renderer::kMaxPointShadowSlots;
using GameEngine::Engine::Renderer::kPointAtlasTileResolution;
using GameEngine::Engine::Renderer::PointShadowAtlasLayerCount;
using GameEngine::Engine::Renderer::PointShadowAtlasPlanner;
using GameEngine::Engine::Renderer::PointShadowDirtyCause;
using GameEngine::Engine::Renderer::PointShadowLodKey;
using GameEngine::Engine::Renderer::PointShadowSlotFaceLayer;
using GameEngine::Engine::Renderer::PointShadowTileScale;
using GameEngine::Engine::Renderer::PointShadowTileToAtlasUV;
using GameEngine::Engine::Renderer::ResolvePointShadowTileResolution;

using Candidate = PointShadowAtlasPlanner::Candidate;
using CacheInputs = PointShadowAtlasPlanner::CacheInputs;
using PlanResult = PointShadowAtlasPlanner::PlanResult;
using SlotAssignment = PointShadowAtlasPlanner::SlotAssignment;

constexpr uint32_t kTierLow = 1u;
constexpr uint32_t kTierMedium = 2u;
constexpr uint32_t kTierHigh = 3u;

Candidate MakeCandidate(uint32_t sortId, float coveragePx, uint32_t explicitTier = 0u,
                        uint32_t inheritRes = 1024u, uint32_t clusterIndex = 0u,
                        uint64_t contentHash = 0u)
{
    Candidate c{};
    c.SortId = sortId;
    c.ClusterIndex = clusterIndex == 0u ? sortId : clusterIndex;
    c.CoverageRadiusPx = coveragePx;
    c.ExplicitTier = explicitTier;
    c.InheritResolution = inheritRes;
    c.ContentHash = contentHash;
    return c;
}

// L1a caching input with sensible defaults for the dirty-trigger tests.
CacheInputs Cache(uint64_t epoch, bool enabled = true,
                  uint32_t refreshBudget = kMaxPointShadowSlots)
{
    CacheInputs c{};
    c.CasterEpoch = epoch;
    c.RefreshBudget = refreshBudget;
    c.Enabled = enabled;
    return c;
}

// ViewLODParams::forceLod "auto-select by screen coverage".
constexpr uint32_t kLodAuto = 0xFFFFFFFFu;
// A representative in-use key: every field away from its default, so a test that
// moves ONE field is not also crossing a default boundary.
constexpr float kLodBudgetPx = 10.0f;
constexpr float kLodSkinnedScale = 0.75f;

PointShadowLodKey LodKey(float bias = 0.0f, uint32_t forceLevel = kLodAuto,
                         uint32_t selectionMode = 1u, float errorBudgetPx = kLodBudgetPx,
                         float skinnedBudgetScale = kLodSkinnedScale)
{
    PointShadowLodKey k{};
    k.Bias = bias;
    k.ForceLevel = forceLevel;
    k.SelectionMode = selectionMode;
    k.ErrorBudgetPx = errorBudgetPx;
    k.SkinnedBudgetScale = skinnedBudgetScale;
    return k;
}

// L1a caching input carrying an explicit LOD selection key — the knobs the
// point-face depth slices are registered with.
CacheInputs CacheLod(uint64_t epoch, PointShadowLodKey lod)
{
    CacheInputs c = Cache(epoch);
    c.Lod = lod;
    return c;
}

// The committed slot for a light, or nullptr if unadmitted.
const SlotAssignment* AssignOf(const PlanResult& r, uint32_t sortId)
{
    for (uint32_t s = 0; s < r.SlotCount; ++s)
        if (r.Slots[s].TileResolution != 0u && r.Slots[s].SortId == sortId)
            return &r.Slots[s];
    return nullptr;
}

// Find the slot holding `sortId`, or -1.
int SlotOf(const PointShadowAtlasPlanner::PlanResult& r, uint32_t sortId)
{
    for (uint32_t s = 0; s < r.SlotCount; ++s)
        if (r.Slots[s].TileResolution != 0u && r.Slots[s].SortId == sortId)
            return static_cast<int>(s);
    return -1;
}

uint32_t OccupiedCount(const PointShadowAtlasPlanner::PlanResult& r)
{
    uint32_t n = 0;
    for (uint32_t s = 0; s < r.SlotCount; ++s)
        if (r.Slots[s].TileResolution != 0u)
            ++n;
    return n;
}
} // namespace

// ───────────────────────── Tier hysteresis ─────────────────────────

TEST(PointShadowAtlasPlanner, BoundaryHoveringLightDoesNotFlipTier)
{
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    // Initialize at Medium (128 < cov <= 384).
    planner.Plan(std::vector<Candidate>{MakeCandidate(1, 250.0f)}, /*budget*/ 4u,
                 /*cooldown*/ 0u, frame++);
    ASSERT_EQ(planner.CommittedTier(1), kTierMedium);

    // Oscillate within the Medium dead-band [96, 448] (promote needs >448, demote
    // <96). Cooldown 0, so ONLY the asymmetric thresholds hold the tier.
    const std::array<float, 8> hover = {130.0f, 420.0f, 100.0f, 447.0f,
                                        97.0f,  440.0f, 200.0f, 300.0f};
    for (int i = 0; i < 40; ++i)
    {
        planner.Plan(std::vector<Candidate>{MakeCandidate(1, hover[i % hover.size()])}, 4u, 0u,
                     frame++);
        EXPECT_EQ(planner.CommittedTier(1), kTierMedium) << "flipped at frame " << frame;
    }
}

TEST(PointShadowAtlasPlanner, DecisiveCoverageCrossingRetiers)
{
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    planner.Plan(std::vector<Candidate>{MakeCandidate(1, 250.0f)}, 4u, 0u, frame++);
    ASSERT_EQ(planner.CommittedTier(1), kTierMedium);

    // Clearly above the Medium→High promote threshold (>448).
    planner.Plan(std::vector<Candidate>{MakeCandidate(1, 600.0f)}, 4u, 0u, frame++);
    EXPECT_EQ(planner.CommittedTier(1), kTierHigh);

    // Clearly below the High→Medium demote threshold (<320).
    planner.Plan(std::vector<Candidate>{MakeCandidate(1, 200.0f)}, 4u, 0u, frame++);
    EXPECT_EQ(planner.CommittedTier(1), kTierMedium);

    // Clearly below the Medium→Low demote threshold (<96).
    planner.Plan(std::vector<Candidate>{MakeCandidate(1, 40.0f)}, 4u, 0u, frame++);
    EXPECT_EQ(planner.CommittedTier(1), kTierLow);
}

TEST(PointShadowAtlasPlanner, CooldownFreezesTierAfterChange)
{
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const uint32_t kCooldown = 5u;
    planner.Plan(std::vector<Candidate>{MakeCandidate(1, 250.0f)}, 4u, kCooldown, frame++);
    ASSERT_EQ(planner.CommittedTier(1), kTierMedium);

    // Promote to High — arms the cooldown.
    planner.Plan(std::vector<Candidate>{MakeCandidate(1, 600.0f)}, 4u, kCooldown, frame++);
    ASSERT_EQ(planner.CommittedTier(1), kTierHigh);

    // Demote-worthy coverage during the freeze window must NOT change the tier.
    for (uint32_t i = 0; i < kCooldown - 1u; ++i)
    {
        planner.Plan(std::vector<Candidate>{MakeCandidate(1, 40.0f)}, 4u, kCooldown, frame++);
        EXPECT_EQ(planner.CommittedTier(1), kTierHigh) << "unfroze early at " << i;
    }
    // Once the cooldown elapses the demote applies.
    bool demoted = false;
    for (uint32_t i = 0; i < 4u; ++i)
    {
        planner.Plan(std::vector<Candidate>{MakeCandidate(1, 40.0f)}, 4u, kCooldown, frame++);
        if (planner.CommittedTier(1) != kTierHigh)
        {
            demoted = true;
            break;
        }
    }
    EXPECT_TRUE(demoted);
}

TEST(PointShadowAtlasPlanner, ExplicitTierPinsAndIgnoresCoverage)
{
    PointShadowAtlasPlanner planner;
    // Tiny coverage, but an explicit High tier pins to 1024.
    auto r = planner.Plan(std::vector<Candidate>{MakeCandidate(1, 5.0f, kTierHigh)}, 4u, 8u, 0);
    ASSERT_EQ(planner.CommittedTier(1), kTierHigh);
    ASSERT_EQ(SlotOf(r, 1), 0);
    EXPECT_EQ(r.Slots[0].TileResolution, 1024u);

    // Huge coverage, but an explicit Low tier pins to 256.
    PointShadowAtlasPlanner planner2;
    auto r2 = planner2.Plan(std::vector<Candidate>{MakeCandidate(2, 5000.0f, kTierLow)}, 4u, 8u, 0);
    EXPECT_EQ(planner2.CommittedTier(2), kTierLow);
    EXPECT_EQ(r2.Slots[SlotOf(r2, 2)].TileResolution, 256u);
}

// ───────────────────────── Slot allocation / eviction ─────────────────────────

TEST(PointShadowAtlasPlanner, BudgetCapsAdmissionToHighestCoverage)
{
    PointShadowAtlasPlanner planner;
    const std::vector<Candidate> cands = {
        MakeCandidate(1, 400.0f), MakeCandidate(2, 300.0f), MakeCandidate(3, 200.0f),
        MakeCandidate(4, 100.0f)};
    auto r = planner.Plan(cands, /*budget*/ 2u, /*cooldown*/ 0u, 0);
    EXPECT_EQ(OccupiedCount(r), 2u);
    // The two strongest are admitted; the weaker two are unshadowed.
    EXPECT_GE(SlotOf(r, 1), 0);
    EXPECT_GE(SlotOf(r, 2), 0);
    EXPECT_EQ(SlotOf(r, 3), -1);
    EXPECT_EQ(SlotOf(r, 4), -1);
}

TEST(PointShadowAtlasPlanner, DropoutFreesSlotAndReadmitsFresh)
{
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    auto r1 = planner.Plan(
        std::vector<Candidate>{MakeCandidate(1, 400.0f), MakeCandidate(2, 300.0f)}, 2u, 0u, frame++);
    const int slotA = SlotOf(r1, 1);
    const int slotB = SlotOf(r1, 2);
    ASSERT_GE(slotA, 0);
    ASSERT_GE(slotB, 0);

    // B drops out; C appears. A keeps its slot, C takes B's freed slot.
    auto r2 = planner.Plan(
        std::vector<Candidate>{MakeCandidate(1, 400.0f), MakeCandidate(3, 350.0f)}, 2u, 0u, frame++);
    EXPECT_EQ(SlotOf(r2, 1), slotA); // sticky
    EXPECT_EQ(SlotOf(r2, 3), slotB); // reused freed slot
    EXPECT_EQ(planner.CommittedTier(2), 0u); // B's state was reaped
}

TEST(PointShadowAtlasPlanner, IncumbentSurvivesMarginalChallenger)
{
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    auto r1 = planner.Plan(std::vector<Candidate>{MakeCandidate(1, 400.0f)}, /*budget*/ 1u, 0u,
                           frame++);
    ASSERT_EQ(SlotOf(r1, 1), 0);

    // Challenger only marginally stronger (420 < 400*1.25=500): incumbent stays.
    auto r2 = planner.Plan(
        std::vector<Candidate>{MakeCandidate(1, 400.0f), MakeCandidate(2, 420.0f)}, 1u, 0u, frame++);
    EXPECT_EQ(SlotOf(r2, 1), 0);
    EXPECT_EQ(SlotOf(r2, 2), -1);

    // Challenger clearly stronger (600 > 500): it evicts the incumbent.
    auto r3 = planner.Plan(
        std::vector<Candidate>{MakeCandidate(1, 400.0f), MakeCandidate(2, 600.0f)}, 1u, 0u, frame++);
    EXPECT_EQ(SlotOf(r3, 2), 0);
    EXPECT_EQ(SlotOf(r3, 1), -1);
}

TEST(PointShadowAtlasPlanner, SlotIndicesAreStickyAcrossFrames)
{
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    auto r1 = planner.Plan(std::vector<Candidate>{MakeCandidate(1, 100.0f), MakeCandidate(2, 200.0f),
                                                  MakeCandidate(3, 300.0f)},
                           3u, 0u, frame++);
    const int s1 = SlotOf(r1, 1);
    const int s2 = SlotOf(r1, 2);
    const int s3 = SlotOf(r1, 3);
    ASSERT_GE(s1, 0);
    ASSERT_GE(s2, 0);
    ASSERT_GE(s3, 0);

    // Same lights next frame keep their exact slot indices regardless of rank.
    auto r2 = planner.Plan(std::vector<Candidate>{MakeCandidate(1, 100.0f), MakeCandidate(2, 200.0f),
                                                  MakeCandidate(3, 300.0f)},
                           3u, 0u, frame++);
    EXPECT_EQ(SlotOf(r2, 1), s1);
    EXPECT_EQ(SlotOf(r2, 2), s2);
    EXPECT_EQ(SlotOf(r2, 3), s3);
}

TEST(PointShadowAtlasPlanner, EmptyCandidateSetYieldsNoSlots)
{
    PointShadowAtlasPlanner planner;
    auto r = planner.Plan(std::vector<Candidate>{}, 4u, 0u, 0);
    EXPECT_EQ(r.SlotCount, 0u);
    EXPECT_EQ(OccupiedCount(r), 0u);
}

// ───────────────────────── Tile-local sampling math ─────────────────────────

TEST(PointShadowAtlasPlanner, TileResolutionResolvesAndClampsToAtlasMax)
{
    EXPECT_EQ(ResolvePointShadowTileResolution(kTierLow, 1024u), 256u);
    EXPECT_EQ(ResolvePointShadowTileResolution(kTierMedium, 1024u), 512u);
    EXPECT_EQ(ResolvePointShadowTileResolution(kTierHigh, 1024u), 1024u);
    // Inherit (tier 0) resolves to the node resolution, then clamps to the atlas.
    EXPECT_EQ(ResolvePointShadowTileResolution(0u, 512u), 512u);
    EXPECT_EQ(ResolvePointShadowTileResolution(0u, 2048u), kPointAtlasTileResolution); // clamped
}

TEST(PointShadowAtlasPlanner, TileScaleMatchesResolutionRatio)
{
    EXPECT_FLOAT_EQ(PointShadowTileScale(1024u), 1.0f);
    EXPECT_FLOAT_EQ(PointShadowTileScale(512u), 0.5f);
    EXPECT_FLOAT_EQ(PointShadowTileScale(256u), 0.25f);
}

TEST(PointShadowAtlasPlanner, FullTileMapsUvIdentity)
{
    // tileScale == 1: an interior UV maps to itself (within the half-texel gutter),
    // so a High/Inherit slot samples exactly like the pre-M1 full-layer path.
    auto uv = PointShadowTileToAtlasUV(0.5f, 0.5f, 1024u);
    EXPECT_NEAR(uv.U, 0.5f, 1e-4f);
    EXPECT_NEAR(uv.V, 0.5f, 1e-4f);
}

TEST(PointShadowAtlasPlanner, LowTileMapsIntoSubRect)
{
    // tileRes 512 in a 1024 atlas: scale 0.5, so the tile occupies [0, 0.5]^2.
    const float scale = 0.5f;
    const float half = 0.5f / static_cast<float>(kPointAtlasTileResolution);

    auto center = PointShadowTileToAtlasUV(0.5f, 0.5f, 512u);
    EXPECT_NEAR(center.U, 0.25f, 1e-4f);
    EXPECT_NEAR(center.V, 0.25f, 1e-4f);

    // Corner UVs clamp to the sub-rect minus the gutter (never into the border).
    auto hi = PointShadowTileToAtlasUV(1.0f, 1.0f, 512u);
    EXPECT_NEAR(hi.U, scale - half, 1e-5f);
    EXPECT_NEAR(hi.V, scale - half, 1e-5f);

    auto lo = PointShadowTileToAtlasUV(0.0f, 0.0f, 512u);
    EXPECT_NEAR(lo.U, half, 1e-5f);
    EXPECT_NEAR(lo.V, half, 1e-5f);
}

TEST(PointShadowAtlasPlanner, ClampKeepsOvershootingTapInsideTile)
{
    const float scale = 0.5f;
    const float half = 0.5f / static_cast<float>(kPointAtlasTileResolution);
    // A PCF tap that overshoots past the tile edge is pulled back inside.
    auto over = PointShadowTileToAtlasUV(1.2f, -0.1f, 512u);
    EXPECT_LE(over.U, scale - half + 1e-6f);
    EXPECT_GE(over.U, half - 1e-6f);
    EXPECT_LE(over.V, scale - half + 1e-6f);
    EXPECT_GE(over.V, half - 1e-6f);
}

TEST(PointShadowAtlasPlanner, LayerAndAtlasSizeMath)
{
    EXPECT_EQ(PointShadowSlotFaceLayer(0u, 0u), 0u);
    EXPECT_EQ(PointShadowSlotFaceLayer(0u, 5u), 5u);
    EXPECT_EQ(PointShadowSlotFaceLayer(1u, 0u), 6u);
    EXPECT_EQ(PointShadowSlotFaceLayer(2u, 3u), 15u);
    EXPECT_EQ(PointShadowAtlasLayerCount(4u), 24u);
}

// ───────────────────── L1a on-dirty render caching ─────────────────────
// The dirty-trigger matrix: a slot re-renders only for the enumerated causes
// and otherwise stays cached (declares no passes). ContentHash stands in for
// the caller's world-position + range + face-mask signature; CasterEpoch stands
// in for the extraction's global shadow-caster content version.

TEST(PointShadowAtlasPlannerL1a, FirstRenderThenCaches)
{
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const std::vector<Candidate> c = {MakeCandidate(1, 300.0f, 0u, 1024u, 0u, 0xAAAA)};

    auto r1 = planner.Plan(c, 4u, 0u, frame++, Cache(5));
    const auto* a1 = AssignOf(r1, 1);
    ASSERT_NE(a1, nullptr);
    EXPECT_TRUE(a1->NeedsRender);
    EXPECT_EQ(a1->DirtyCause, PointShadowDirtyCause::FirstRender);

    // Identical content, tier and caster epoch => no passes.
    auto r2 = planner.Plan(c, 4u, 0u, frame++, Cache(5));
    const auto* a2 = AssignOf(r2, 1);
    ASSERT_NE(a2, nullptr);
    EXPECT_FALSE(a2->NeedsRender);
    EXPECT_EQ(a2->DirtyCause, PointShadowDirtyCause::Cached);
}

TEST(PointShadowAtlasPlannerL1a, CasterEpochBumpReRendersThenReCaches)
{
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const std::vector<Candidate> c = {MakeCandidate(1, 300.0f, 0u, 1024u, 0u, 0xAAAA)};
    planner.Plan(c, 4u, 0u, frame++, Cache(5)); // FirstRender
    ASSERT_FALSE(AssignOf(planner.Plan(c, 4u, 0u, frame++, Cache(5)), 1)->NeedsRender);

    // A caster moved: the global epoch advanced -> re-render every cached light.
    auto r = planner.Plan(c, 4u, 0u, frame++, Cache(6));
    ASSERT_NE(AssignOf(r, 1), nullptr);
    EXPECT_TRUE(AssignOf(r, 1)->NeedsRender);
    EXPECT_EQ(AssignOf(r, 1)->DirtyCause, PointShadowDirtyCause::CasterMoved);

    // Re-cached at the new epoch.
    EXPECT_FALSE(AssignOf(planner.Plan(c, 4u, 0u, frame++, Cache(6)), 1)->NeedsRender);
}

TEST(PointShadowAtlasPlannerL1a, ContentChangeReRenders)
{
    // Light moved / range changed / a face became camera-visible — all fold into
    // ContentHash, so any of them re-renders the light.
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    planner.Plan(std::vector<Candidate>{MakeCandidate(1, 300.0f, 0u, 1024u, 0u, 0xAA)}, 4u, 0u, frame++, Cache(5));
    planner.Plan(std::vector<Candidate>{MakeCandidate(1, 300.0f, 0u, 1024u, 0u, 0xAA)}, 4u, 0u, frame++, Cache(5));

    auto r = planner.Plan(std::vector<Candidate>{MakeCandidate(1, 300.0f, 0u, 1024u, 0u, 0xBB)}, 4u, 0u, frame++, Cache(5));
    ASSERT_NE(AssignOf(r, 1), nullptr);
    EXPECT_TRUE(AssignOf(r, 1)->NeedsRender);
    EXPECT_EQ(AssignOf(r, 1)->DirtyCause, PointShadowDirtyCause::ContentChanged);
}

TEST(PointShadowAtlasPlannerL1a, TierChangeReRenders)
{
    // Same content and epoch, only the committed resolution tier moves.
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    planner.Plan(std::vector<Candidate>{MakeCandidate(1, 300.0f, kTierHigh, 1024u, 0u, 0xAA)}, 4u, 0u, frame++, Cache(5));
    planner.Plan(std::vector<Candidate>{MakeCandidate(1, 300.0f, kTierHigh, 1024u, 0u, 0xAA)}, 4u, 0u, frame++, Cache(5));

    auto r = planner.Plan(std::vector<Candidate>{MakeCandidate(1, 300.0f, kTierLow, 1024u, 0u, 0xAA)}, 4u, 0u, frame++,
                          Cache(5));
    ASSERT_NE(AssignOf(r, 1), nullptr);
    EXPECT_TRUE(AssignOf(r, 1)->NeedsRender);
    EXPECT_EQ(AssignOf(r, 1)->DirtyCause, PointShadowDirtyCause::TierChanged);
}

TEST(PointShadowAtlasPlannerL1a, SlotReTenantReRenders)
{
    // Budget 1: a fresh tenant of a slot always renders, even if the departed
    // tenant's content hash and the caster epoch match.
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    auto r1 = planner.Plan(std::vector<Candidate>{MakeCandidate(1, 300.0f, 0u, 1024u, 0u, 0xAA)}, 1u, 0u, frame++, Cache(5));
    ASSERT_EQ(SlotOf(r1, 1), 0);

    auto r2 = planner.Plan(std::vector<Candidate>{MakeCandidate(2, 300.0f, 0u, 1024u, 0u, 0xAA)}, 1u, 0u, frame++, Cache(5));
    ASSERT_EQ(SlotOf(r2, 2), 0); // B took A's freed slot
    EXPECT_TRUE(AssignOf(r2, 2)->NeedsRender);
    EXPECT_EQ(AssignOf(r2, 2)->DirtyCause, PointShadowDirtyCause::SlotReTenant);
}

TEST(PointShadowAtlasPlannerL1a, LodBiasChangeReRendersThenReCaches)
{
    // A retained slot holds depth rasterized at the LOD levels the knobs in
    // force at its last render selected. Move the bias and the levels a fresh
    // render would pick differ, so the slot must not report Cached.
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const std::vector<Candidate> c = {MakeCandidate(1, 300.0f, 0u, 1024u, 0u, 0xAA)};
    planner.Plan(c, 4u, 0u, frame++, CacheLod(5, LodKey(0.0f))); // FirstRender
    ASSERT_FALSE(
        AssignOf(planner.Plan(c, 4u, 0u, frame++, CacheLod(5, LodKey(0.0f))), 1)->NeedsRender);

    auto r = planner.Plan(c, 4u, 0u, frame++, CacheLod(5, LodKey(-1.0f)));
    ASSERT_NE(AssignOf(r, 1), nullptr);
    EXPECT_TRUE(AssignOf(r, 1)->NeedsRender);
    EXPECT_EQ(AssignOf(r, 1)->DirtyCause, PointShadowDirtyCause::LodSelectionChanged);

    // Re-cached under the new knobs.
    EXPECT_FALSE(
        AssignOf(planner.Plan(c, 4u, 0u, frame++, CacheLod(5, LodKey(-1.0f))), 1)->NeedsRender);
}

TEST(PointShadowAtlasPlannerL1a, LodForceLevelChangeReRenders)
{
    // The force level pins every instance to one level, so flipping it changes
    // the rasterized silhouettes even though the bias did not move.
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const std::vector<Candidate> c = {MakeCandidate(1, 300.0f, 0u, 1024u, 0u, 0xAA)};
    planner.Plan(c, 4u, 0u, frame++, CacheLod(5, LodKey(0.0f, kLodAuto)));
    ASSERT_FALSE(AssignOf(planner.Plan(c, 4u, 0u, frame++, CacheLod(5, LodKey(0.0f, kLodAuto))), 1)
                     ->NeedsRender);

    auto r = planner.Plan(c, 4u, 0u, frame++, CacheLod(5, LodKey(0.0f, 3u)));
    ASSERT_NE(AssignOf(r, 1), nullptr);
    EXPECT_TRUE(AssignOf(r, 1)->NeedsRender);
    EXPECT_EQ(AssignOf(r, 1)->DirtyCause, PointShadowDirtyCause::LodSelectionChanged);
}

TEST(PointShadowAtlasPlannerL1a, EveryLodKnobInvalidatesIndependently)
{
    // One leg per keyed field. A field present in the struct but dropped from the
    // comparison is exactly the failure mode the INVARIANT guards against, and it
    // is invisible to a test that only ever moves the bias.
    const std::vector<Candidate> c = {MakeCandidate(1, 300.0f, 0u, 1024u, 0u, 0xAA)};
    const PointShadowLodKey base = LodKey();
    const PointShadowLodKey moved[] = {
        LodKey(-2.0f),                                          // Bias
        LodKey(0.0f, 1u),                                       // ForceLevel
        LodKey(0.0f, kLodAuto, 2u),                             // SelectionMode
        LodKey(0.0f, kLodAuto, 1u, kLodBudgetPx * 2.0f),        // ErrorBudgetPx
        LodKey(0.0f, kLodAuto, 1u, kLodBudgetPx, 0.5f),         // SkinnedBudgetScale
    };
    for (const PointShadowLodKey& next : moved)
    {
        PointShadowAtlasPlanner planner;
        uint64_t frame = 0;
        planner.Plan(c, 4u, 0u, frame++, CacheLod(5, base)); // FirstRender
        ASSERT_FALSE(AssignOf(planner.Plan(c, 4u, 0u, frame++, CacheLod(5, base)), 1)->NeedsRender);

        auto r = planner.Plan(c, 4u, 0u, frame++, CacheLod(5, next));
        ASSERT_NE(AssignOf(r, 1), nullptr);
        EXPECT_TRUE(AssignOf(r, 1)->NeedsRender);
        EXPECT_EQ(AssignOf(r, 1)->DirtyCause, PointShadowDirtyCause::LodSelectionChanged);
    }
}

TEST(PointShadowAtlasPlannerL1a, StableNonDefaultLodKeyStaysCached)
{
    // Control for the two above: a key that is merely non-default must not
    // dirty anything. Only a CHANGE re-renders — otherwise the tests above
    // would pass against a planner that never caches at all.
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const std::vector<Candidate> c = {MakeCandidate(1, 300.0f, 0u, 1024u, 0u, 0xAA)};
    planner.Plan(c, 4u, 0u, frame++, CacheLod(5, LodKey(-1.5f, 2u))); // FirstRender
    for (int i = 0; i < 3; ++i)
    {
        auto r = planner.Plan(c, 4u, 0u, frame++, CacheLod(5, LodKey(-1.5f, 2u)));
        ASSERT_NE(AssignOf(r, 1), nullptr);
        EXPECT_FALSE(AssignOf(r, 1)->NeedsRender);
        EXPECT_EQ(AssignOf(r, 1)->DirtyCause, PointShadowDirtyCause::Cached);
    }
}

TEST(PointShadowAtlasPlannerL1a, ReturnToDefaultLodKeyReRenders)
{
    // Coming BACK to the default key is a change like any other. Nothing may
    // treat the default as a wildcard that matches whatever was recorded — the
    // slot's depth was rasterized under the non-default knobs.
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const std::vector<Candidate> c = {MakeCandidate(1, 300.0f, 0u, 1024u, 0u, 0xAA)};
    planner.Plan(c, 4u, 0u, frame++, CacheLod(5, LodKey(-1.5f, 2u))); // FirstRender
    ASSERT_FALSE(
        AssignOf(planner.Plan(c, 4u, 0u, frame++, CacheLod(5, LodKey(-1.5f, 2u))), 1)->NeedsRender);

    auto r = planner.Plan(c, 4u, 0u, frame++, CacheLod(5, PointShadowLodKey{}));
    ASSERT_NE(AssignOf(r, 1), nullptr);
    EXPECT_TRUE(AssignOf(r, 1)->NeedsRender);
    EXPECT_EQ(AssignOf(r, 1)->DirtyCause, PointShadowDirtyCause::LodSelectionChanged);
    EXPECT_FALSE(AssignOf(planner.Plan(c, 4u, 0u, frame++, CacheLod(5, PointShadowLodKey{})), 1)
                     ->NeedsRender);
}

TEST(PointShadowAtlasPlannerL1a, LodChangeDirtiesEveryAdmittedSlot)
{
    // The knobs are per-view, not per-light: one change makes every retained
    // slot in the view stale at once (unlike a caster change, which L1b scopes
    // to the lights whose influence sphere it intersects).
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const std::vector<Candidate> c = {MakeCandidate(1, 400.0f, 0u, 1024u, 1u, 0xA1),
                                      MakeCandidate(2, 300.0f, 0u, 1024u, 2u, 0xA2),
                                      MakeCandidate(3, 200.0f, 0u, 1024u, 3u, 0xA3)};
    planner.Plan(c, 4u, 0u, frame++, CacheLod(5, LodKey(0.0f)));
    auto cached = planner.Plan(c, 4u, 0u, frame++, CacheLod(5, LodKey(0.0f)));
    for (uint32_t sortId = 1; sortId <= 3; ++sortId)
        ASSERT_FALSE(AssignOf(cached, sortId)->NeedsRender) << "sortId " << sortId;

    auto r = planner.Plan(c, 4u, 0u, frame++, CacheLod(5, LodKey(1.0f)));
    for (uint32_t sortId = 1; sortId <= 3; ++sortId)
    {
        ASSERT_NE(AssignOf(r, sortId), nullptr) << "sortId " << sortId;
        EXPECT_TRUE(AssignOf(r, sortId)->NeedsRender) << "sortId " << sortId;
        EXPECT_EQ(AssignOf(r, sortId)->DirtyCause, PointShadowDirtyCause::LodSelectionChanged)
            << "sortId " << sortId;
    }
}

TEST(PointShadowAtlasPlannerL1a, RefreshBudgetDefersLowerImportance)
{
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const std::vector<Candidate> c = {MakeCandidate(1, 400.0f, 0u, 1024u, 0u, 0xA1),
                                      MakeCandidate(2, 200.0f, 0u, 1024u, 0u, 0xB2)};

    // Two dirty slots, refresh budget 1: only the higher-coverage light renders.
    auto r1 = planner.Plan(c, 4u, 0u, frame++, Cache(5, true, 1u));
    ASSERT_NE(AssignOf(r1, 1), nullptr);
    ASSERT_NE(AssignOf(r1, 2), nullptr);
    EXPECT_TRUE(AssignOf(r1, 1)->NeedsRender);
    EXPECT_FALSE(AssignOf(r1, 2)->NeedsRender);
    EXPECT_EQ(AssignOf(r1, 2)->DirtyCause, PointShadowDirtyCause::BudgetDeferred);

    // Next frame light 1 is cached (frees the budget), so the deferred light 2 renders.
    auto r2 = planner.Plan(c, 4u, 0u, frame++, Cache(5, true, 1u));
    EXPECT_FALSE(AssignOf(r2, 1)->NeedsRender);
    EXPECT_TRUE(AssignOf(r2, 2)->NeedsRender);
    EXPECT_EQ(AssignOf(r2, 2)->DirtyCause, PointShadowDirtyCause::FirstRender);
}

TEST(PointShadowAtlasPlannerL1a, CacheDisabledAlwaysRenders)
{
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const std::vector<Candidate> c = {MakeCandidate(1, 300.0f, 0u, 1024u, 0u, 0xAA)};
    EXPECT_TRUE(AssignOf(planner.Plan(c, 4u, 0u, frame++, Cache(5, false)), 1)->NeedsRender);
    // Nothing changed, but with caching off the light still renders every frame.
    EXPECT_TRUE(AssignOf(planner.Plan(c, 4u, 0u, frame++, Cache(5, false)), 1)->NeedsRender);
}

TEST(PointShadowAtlasPlannerL1a, ResetClearsRenderCache)
{
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const std::vector<Candidate> c = {MakeCandidate(1, 300.0f, 0u, 1024u, 0u, 0xAA)};
    planner.Plan(c, 4u, 0u, frame++, Cache(5));
    ASSERT_FALSE(AssignOf(planner.Plan(c, 4u, 0u, frame++, Cache(5)), 1)->NeedsRender);

    // A device rebuild recreates the atlas -> nothing may be reported cached.
    planner.Reset();
    auto r = planner.Plan(c, 4u, 0u, frame++, Cache(5));
    ASSERT_NE(AssignOf(r, 1), nullptr);
    EXPECT_TRUE(AssignOf(r, 1)->NeedsRender);
    EXPECT_EQ(AssignOf(r, 1)->DirtyCause, PointShadowDirtyCause::FirstRender);
}

TEST(PointShadowAtlasPlannerL1a, InvalidateRenderCacheForcesReRenderDespiteMatchingKey)
{
    // The render key tracks CONTENT identity, not the atlas texture's GPU lifetime.
    // When the persistent atlas is recreated fresh underneath a cached slot (pool
    // idle-eviction after the view went un-imported, or a budget realloc), the
    // caller (RenderServices::EnsurePointShadowAssignment on an import gap /
    // OnDeviceRebuilt) drops the render cache via InvalidateRenderCache() so a slot
    // whose key still matches re-renders instead of sampling the Undefined physical.
    // This pins that guard: a matching key is NOT sufficient once the cache is dropped.
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const Candidate a = MakeCandidate(1, 300.0f, 0u, 1024u, 0u, 0xAA);
    planner.Plan(std::vector<Candidate>{a}, 4u, 0u, frame++, Cache(5)); // FirstRender
    ASSERT_FALSE(AssignOf(planner.Plan(std::vector<Candidate>{a}, 4u, 0u, frame++, Cache(5)), 1)
                     ->NeedsRender); // Cached (key matches)

    // The atlas physical was recreated underneath the slot: force the cache invalid.
    planner.InvalidateRenderCache();

    // Identical key, but the fresh physical holds no depth => must re-render.
    auto r = planner.Plan(std::vector<Candidate>{a}, 4u, 0u, frame++, Cache(5));
    ASSERT_NE(AssignOf(r, 1), nullptr);
    EXPECT_TRUE(AssignOf(r, 1)->NeedsRender);
    EXPECT_EQ(AssignOf(r, 1)->DirtyCause, PointShadowDirtyCause::FirstRender);
}

TEST(PointShadowAtlasPlannerL1a, ReadmitAfterReTenantReRenders)
{
    // If another tenant used the slot in the interim, its layers were overwritten,
    // so the returning light must re-render — the per-slot SortId guard catches it.
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    planner.Plan(std::vector<Candidate>{MakeCandidate(1, 400.0f, 0u, 1024u, 0u, 0xAA)}, 1u, 0u, frame++, Cache(5));
    planner.Plan(std::vector<Candidate>{MakeCandidate(2, 400.0f, 0u, 1024u, 0u, 0xBB)}, 1u, 0u, frame++, Cache(5)); // B owns slot 0

    auto r = planner.Plan(std::vector<Candidate>{MakeCandidate(1, 400.0f, 0u, 1024u, 0u, 0xAA)}, 1u, 0u, frame++, Cache(5));
    ASSERT_EQ(SlotOf(r, 1), 0);
    EXPECT_TRUE(AssignOf(r, 1)->NeedsRender);
    EXPECT_EQ(AssignOf(r, 1)->DirtyCause, PointShadowDirtyCause::SlotReTenant);
}

TEST(PointShadowAtlasPlannerL1a, StaticMultiLightSceneCachesAfterFirstFrame)
{
    // The core L1a promise at the planner level: a static multi-light scene renders
    // once, then declares zero passes every subsequent frame.
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const std::vector<Candidate> c = {MakeCandidate(1, 300.0f, 0u, 1024u, 0u, 0x11),
                                      MakeCandidate(2, 250.0f, 0u, 1024u, 0u, 0x22),
                                      MakeCandidate(3, 200.0f, 0u, 1024u, 0u, 0x33)};
    auto r1 = planner.Plan(c, 4u, 0u, frame++, Cache(7));
    for (uint32_t id : {1u, 2u, 3u})
        EXPECT_TRUE(AssignOf(r1, id)->NeedsRender) << "light " << id << " should first-render";

    for (int i = 0; i < 10; ++i)
    {
        auto r = planner.Plan(c, 4u, 0u, frame++, Cache(7));
        for (uint32_t id : {1u, 2u, 3u})
        {
            const auto* a = AssignOf(r, id);
            ASSERT_NE(a, nullptr);
            EXPECT_FALSE(a->NeedsRender) << "light " << id << " re-rendered at frame " << frame;
        }
    }
}

// ───────────────────── L1b caster-proximity keying ─────────────────────

namespace
{
using GameEngine::Engine::Renderer::ShadowCasterChangeSphere;

Candidate CandidateAt(uint32_t sortId, float x, float y, float z, float range,
                      uint64_t contentHash)
{
    Candidate c = MakeCandidate(sortId, 300.0f, 0u, 1024u, 0u, contentHash);
    c.InfluenceSphere[0] = x;
    c.InfluenceSphere[1] = y;
    c.InfluenceSphere[2] = z;
    c.InfluenceSphere[3] = range;
    return c;
}

// Attributed epoch advance: the given spheres explain the step to `epoch`.
CacheInputs CacheAttributed(uint64_t epoch, std::span<const ShadowCasterChangeSphere> spheres)
{
    CacheInputs c = Cache(epoch);
    c.ChangedCasters = spheres;
    c.Unattributed = false;
    return c;
}

// Two lights far apart: A at the origin, B at x=100, both range 5.
std::vector<Candidate> TwoFarLights()
{
    return {CandidateAt(1, 0.0f, 0.0f, 0.0f, 5.0f, 0xA1),
            CandidateAt(2, 100.0f, 0.0f, 0.0f, 5.0f, 0xB2)};
}
} // namespace

TEST(PointShadowAtlasPlannerL1b, FarCasterChangeKeepsUnaffectedLightCached)
{
    // The headline L1b promise: a mover near light A re-renders A only; light B,
    // which cannot see the mover, keeps its cached map.
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const auto c = TwoFarLights();
    planner.Plan(c, 4u, 0u, frame++, Cache(5)); // FirstRender both
    planner.Plan(c, 4u, 0u, frame++, Cache(5)); // cached both

    const ShadowCasterChangeSphere nearA{2.0f, 0.0f, 0.0f, 1.0f}; // intersects A only
    auto r = planner.Plan(c, 4u, 0u, frame++, CacheAttributed(6, {&nearA, 1}));
    ASSERT_NE(AssignOf(r, 1), nullptr);
    ASSERT_NE(AssignOf(r, 2), nullptr);
    EXPECT_TRUE(AssignOf(r, 1)->NeedsRender);
    EXPECT_EQ(AssignOf(r, 1)->DirtyCause, PointShadowDirtyCause::CasterMoved);
    EXPECT_FALSE(AssignOf(r, 2)->NeedsRender) << "far light re-rendered by an unrelated caster";

    // Both settle cached at the new epoch.
    auto r2 = planner.Plan(c, 4u, 0u, frame++, Cache(6));
    EXPECT_FALSE(AssignOf(r2, 1)->NeedsRender);
    EXPECT_FALSE(AssignOf(r2, 2)->NeedsRender);
}

TEST(PointShadowAtlasPlannerL1b, CasterEnteringRangeDirtiesLight)
{
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const auto c = TwoFarLights();
    planner.Plan(c, 4u, 0u, frame++, Cache(5));

    // Approaching but still outside B's range (dist 10 > 5 + 1): B stays cached.
    const ShadowCasterChangeSphere outside{90.0f, 0.0f, 0.0f, 1.0f};
    auto r1 = planner.Plan(c, 4u, 0u, frame++, CacheAttributed(6, {&outside, 1}));
    EXPECT_FALSE(AssignOf(r1, 2)->NeedsRender);

    // Crosses into range (dist 3 < 5 + 1): B re-renders to pick up the new shadow.
    const ShadowCasterChangeSphere inside{103.0f, 0.0f, 0.0f, 1.0f};
    auto r2 = planner.Plan(c, 4u, 0u, frame++, CacheAttributed(7, {&inside, 1}));
    EXPECT_TRUE(AssignOf(r2, 2)->NeedsRender);
    EXPECT_EQ(AssignOf(r2, 2)->DirtyCause, PointShadowDirtyCause::CasterMoved);
    EXPECT_FALSE(AssignOf(r2, 1)->NeedsRender);
}

TEST(PointShadowAtlasPlannerL1b, CasterLeavingRangeTriggersOneFinalRenderThenCaches)
{
    // The caller publishes the sphere enclosing old ∪ new bounds, so the exit
    // crossing still intersects (old pose inside) — one final re-render removes
    // the departed caster's shadow; subsequent far movement leaves B cached.
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const auto c = TwoFarLights();
    planner.Plan(c, 4u, 0u, frame++, Cache(5));

    // Exit frame: old pose (103,0,0) inside, new (110,0,0) outside — enclosing
    // sphere center (106.5, 0, 0), radius 3.5 + 1. Intersects B (dist 6.5 <= 5+4.5).
    const ShadowCasterChangeSphere exitCrossing{106.5f, 0.0f, 0.0f, 4.5f};
    auto r1 = planner.Plan(c, 4u, 0u, frame++, CacheAttributed(6, {&exitCrossing, 1}));
    EXPECT_TRUE(AssignOf(r1, 2)->NeedsRender) << "exit crossing must remove the stale shadow";
    EXPECT_EQ(AssignOf(r1, 2)->DirtyCause, PointShadowDirtyCause::CasterMoved);

    // Fully outside now (dist 15 > 5 + 1): B stays cached.
    const ShadowCasterChangeSphere farAway{115.0f, 0.0f, 0.0f, 1.0f};
    auto r2 = planner.Plan(c, 4u, 0u, frame++, CacheAttributed(7, {&farAway, 1}));
    EXPECT_FALSE(AssignOf(r2, 2)->NeedsRender);
}

TEST(PointShadowAtlasPlannerL1b, UnattributedChangeDirtiesEveryLight)
{
    // Full-lane frames / vertex-mod casters / overflow publish no spheres: the
    // pre-L1b world-scoped behavior is the explicit fallback.
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const auto c = TwoFarLights();
    planner.Plan(c, 4u, 0u, frame++, Cache(5));

    auto r = planner.Plan(c, 4u, 0u, frame++, Cache(6)); // Unattributed defaults true
    EXPECT_TRUE(AssignOf(r, 1)->NeedsRender);
    EXPECT_TRUE(AssignOf(r, 2)->NeedsRender);
}

TEST(PointShadowAtlasPlannerL1b, EpochJumpGlobalizesDespiteAttribution)
{
    // The sphere list explains exactly ONE epoch step. A planner that missed an
    // epoch (inactive view) cannot trust the list for the steps it never saw.
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const auto c = TwoFarLights();
    planner.Plan(c, 4u, 0u, frame++, Cache(5));

    const ShadowCasterChangeSphere farFromBoth{50.0f, 0.0f, 0.0f, 1.0f};
    auto r = planner.Plan(c, 4u, 0u, frame++, CacheAttributed(7, {&farFromBoth, 1}));
    EXPECT_TRUE(AssignOf(r, 1)->NeedsRender);
    EXPECT_TRUE(AssignOf(r, 2)->NeedsRender);
}

TEST(PointShadowAtlasPlannerL1b, AttributedEmptyListKeepsAllCached)
{
    // A bump whose only instance changes were non-casters publishes an attributed
    // EMPTY list: no light's shadow depth can have changed, so none re-render.
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const auto c = TwoFarLights();
    planner.Plan(c, 4u, 0u, frame++, Cache(5));

    auto r = planner.Plan(c, 4u, 0u, frame++, CacheAttributed(6, {}));
    EXPECT_FALSE(AssignOf(r, 1)->NeedsRender);
    EXPECT_FALSE(AssignOf(r, 2)->NeedsRender);
}

TEST(PointShadowAtlasPlannerL1b, LightOwnMovementStillReRenders)
{
    // L1b must not weaken the L1a content key: the light itself moving re-renders
    // its faces via ContentChanged even when no caster sphere is anywhere near.
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    planner.Plan(std::vector<Candidate>{CandidateAt(1, 0.0f, 0.0f, 0.0f, 5.0f, 0xA1)}, 4u, 0u,
                 frame++, Cache(5));

    auto r = planner.Plan(std::vector<Candidate>{CandidateAt(1, 1.0f, 0.0f, 0.0f, 5.0f, 0xA2)},
                          4u, 0u, frame++, Cache(5));
    EXPECT_TRUE(AssignOf(r, 1)->NeedsRender);
    EXPECT_EQ(AssignOf(r, 1)->DirtyCause, PointShadowDirtyCause::ContentChanged);
}

TEST(PointShadowAtlasPlannerL1b, ReturningLightReRendersWhenEpochAdvancedWhileAbsent)
{
    // The reap + reseed couple: Plan() erases state for lights absent from the
    // candidate list, and a returning light reseeds its relevant epoch at the
    // CURRENT value — so a change that landed while it was away must surface as
    // a re-render, never as a stale cached map. This is the invariant a future
    // "keep LightState for absent lights" optimization would silently break.
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const auto both = TwoFarLights();
    planner.Plan(both, 4u, 0u, frame++, Cache(5)); // FirstRender both
    planner.Plan(both, 4u, 0u, frame++, Cache(5)); // cached both

    // B drops out of candidacy for a frame during which the epoch advances,
    // attributed to a change inside B's range (which B never saw).
    const std::vector<Candidate> onlyA{both[0]};
    const ShadowCasterChangeSphere nearB{102.0f, 0.0f, 0.0f, 1.0f};
    planner.Plan(onlyA, 4u, 0u, frame++, CacheAttributed(6, {&nearB, 1}));

    // B returns: whatever the surfaced cause, it must NOT serve the stale map.
    auto r = planner.Plan(both, 4u, 0u, frame++, Cache(6));
    ASSERT_NE(AssignOf(r, 2), nullptr);
    EXPECT_TRUE(AssignOf(r, 2)->NeedsRender)
        << "light returned after an epoch advance while absent and served a stale shadow map";
}

TEST(PointShadowAtlasPlannerL1b, ReturningLightStaysCachedWhenNothingChangedWhileAbsent)
{
    // Twin of the above: absence with NO epoch advance must not cost a re-render
    // on return — that reuse is the designed win of the render cache.
    PointShadowAtlasPlanner planner;
    uint64_t frame = 0;
    const auto both = TwoFarLights();
    planner.Plan(both, 4u, 0u, frame++, Cache(5));
    planner.Plan(both, 4u, 0u, frame++, Cache(5));

    const std::vector<Candidate> onlyA{both[0]};
    planner.Plan(onlyA, 4u, 0u, frame++, Cache(5));

    auto r = planner.Plan(both, 4u, 0u, frame++, Cache(5));
    ASSERT_NE(AssignOf(r, 2), nullptr);
    EXPECT_FALSE(AssignOf(r, 2)->NeedsRender)
        << "unchanged world: a light re-admitted to its old slot should reuse its cached map";
}
