#include "Components/Spline/SplinePlacement.h"
#include "Components/Spline/SplinePoolSelection.h"
#include "Components/Spline/SplineStationJitter.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/Components.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <span>
#include <string_view>
#include <vector>

using namespace GameEngine;
using Components::ActivePoolCount;
using Components::kSplinePoolCapacity;
using Components::MaterialRef;
using Components::ModelRef;
using Components::PoolHasHole;
using Components::SplinePlacement;
using Components::SplineConformTarget;
using Components::SplineJitterChannel;
using Components::SplineJitterSigned;
using Components::SplineJitterUnit;
using Components::SplinePlantMode;
using Components::SplinePoolRole;
using Components::SplinePoolSelect;
using Components::SplineSelectionMix;

namespace
{

// A distinct, non-null GUID per ordinal for pool contents.
ModelRef TestRef(uint8 ordinal)
{
    GUID::Data data{};
    data[0] = ordinal;
    data[15] = 0xA5;
    return ModelRef(GUID(data));
}

// Same shape for the override-material slot, in a disjoint GUID space so a
// material can never compare equal to a pool mesh.
MaterialRef MaterialTestRef(uint8 ordinal)
{
    GUID::Data data{};
    data[0] = ordinal;
    data[15] = 0xC3;
    return MaterialRef(GUID(data));
}

} // namespace

// The mix is pinned to reference splitmix64: these are the published first
// outputs for states 0 and 1. If this test fails, the selection hash changed
// and every authored scene's picks reshuffle — that must be a deliberate edit.
TEST(SplinePoolSelection, MixMatchesReferenceSplitmix64)
{
    EXPECT_EQ(SplineSelectionMix(0ull), 0xE220A8397B1DCDAFull);
    EXPECT_EQ(SplineSelectionMix(1ull), 0x910A2DEC89025CC1ull);
}

// Golden picks: a deliberate hash change must edit these values consciously.
TEST(SplinePoolSelection, GoldenPicks)
{
    constexpr uint32 kSeed0Count3[8] = {0, 1, 1, 2, 1, 1, 0, 1};
    constexpr uint32 kSeed77Count5[8] = {3, 4, 0, 1, 2, 1, 1, 3};
    constexpr uint32 kSeed77ScatterCount5[8] = {4, 3, 0, 2, 1, 4, 2, 4};
    for (uint32 s = 0; s < 8; ++s)
    {
        EXPECT_EQ(SplinePoolSelect(0u, SplinePoolRole::Straight, s, 3u), kSeed0Count3[s])
            << "station " << s;
        EXPECT_EQ(SplinePoolSelect(77u, SplinePoolRole::Straight, s, 5u), kSeed77Count5[s])
            << "station " << s;
        EXPECT_EQ(SplinePoolSelect(77u, SplinePoolRole::Scatter, s, 5u), kSeed77ScatterCount5[s])
            << "station " << s;
    }
}

// Selection is a pure function of (Seed, role, station): recomputing yields
// identical picks (rebuild idempotence), and the picks for stations 0..N-1 do
// not depend on how many stations follow — the pure-function form of "nudging
// knot 9 leaves stations 0..8 byte-identical".
TEST(SplinePoolSelection, RebuildIdempotentAndPrefixStable)
{
    constexpr uint32 kSeed = 12345u;
    constexpr uint32 kCount = 4u;

    std::vector<uint32> firstPass;
    for (uint32 s = 0; s < 32; ++s)
        firstPass.push_back(SplinePoolSelect(kSeed, SplinePoolRole::Straight, s, kCount));

    std::vector<uint32> secondPass;
    for (uint32 s = 0; s < 32; ++s)
        secondPass.push_back(SplinePoolSelect(kSeed, SplinePoolRole::Straight, s, kCount));
    EXPECT_EQ(firstPass, secondPass);

    // A longer spline (more stations) reproduces the shorter one's prefix.
    for (uint32 s = 0; s < 16; ++s)
        EXPECT_EQ(SplinePoolSelect(kSeed, SplinePoolRole::Straight, s, kCount), firstPass[s])
            << "station " << s;
}

TEST(SplinePoolSelection, PicksStayInRange)
{
    for (uint32 seed : {0u, 1u, 77u, 0xFFFFFFFFu})
    {
        for (uint32 count = 1; count <= kSplinePoolCapacity; ++count)
        {
            for (uint32 s = 0; s < 64; ++s)
            {
                const uint32 pick = SplinePoolSelect(seed, SplinePoolRole::Straight, s, count);
                ASSERT_LT(pick, count) << "seed " << seed << " count " << count << " station " << s;
            }
        }
    }
}

// Roles draw independently: same seed and stations, different salt, different
// pick sequence (not element-wise guaranteed, so compare the sequences).
TEST(SplinePoolSelection, RoleSaltDecorrelatesPicks)
{
    std::vector<uint32> straight, scatter;
    for (uint32 s = 0; s < 32; ++s)
    {
        straight.push_back(SplinePoolSelect(9u, SplinePoolRole::Straight, s, 5u));
        scatter.push_back(SplinePoolSelect(9u, SplinePoolRole::Scatter, s, 5u));
    }
    EXPECT_NE(straight, scatter);
}

TEST(SplinePoolSelection, ActivePoolCountIsLeadingNonEmptyPrefix)
{
    ModelRef pool[kSplinePoolCapacity] = {};
    EXPECT_EQ(ActivePoolCount(pool), 0u);
    EXPECT_FALSE(PoolHasHole(pool));

    pool[0] = TestRef(1);
    EXPECT_EQ(ActivePoolCount(pool), 1u);
    pool[1] = TestRef(2);
    pool[2] = TestRef(3);
    EXPECT_EQ(ActivePoolCount(pool), 3u);
    EXPECT_FALSE(PoolHasHole(pool));

    for (uint32 i = 0; i < kSplinePoolCapacity; ++i)
        pool[i] = TestRef(static_cast<uint8>(i + 1));
    EXPECT_EQ(ActivePoolCount(pool), kSplinePoolCapacity);
    EXPECT_FALSE(PoolHasHole(pool));
}

// The pinned hole contract: (valid, empty, valid) selects from the leading
// prefix only, and the hole is reported — never silently reshuffled.
TEST(SplinePoolSelection, HoleTerminatesActivePrefixAndIsReported)
{
    ModelRef pool[kSplinePoolCapacity] = {};
    pool[0] = TestRef(1);
    pool[2] = TestRef(3);
    EXPECT_EQ(ActivePoolCount(pool), 1u);
    EXPECT_TRUE(PoolHasHole(pool));
}

// Chunk-resident authored data: three fixed pools must not balloon the
// component. 3 * 6 * 16 (pools) + 15 * 4 (scalars) + 16 (override material)
// = 364 bytes, no padding.
//
// If this fires you added a field: add it to the equality flip table below and
// to the SceneIO round-trip's authored block, then re-baseline the size here.
TEST(SplinePlacementRecipe, ComponentSizeStaysEcsFriendly)
{
    RecordProperty("sizeof_SplinePlacement", static_cast<int>(sizeof(SplinePlacement)));
    EXPECT_EQ(sizeof(SplinePlacement), 364u);
    EXPECT_LE(sizeof(SplinePlacement), 512u);
}

// The rebuild scheduler compares whole recipes; flipping any single field must
// break equality, or edits to that field silently stop scheduling rebuilds.
// operator== is defaulted (memberwise), so this list guards against it ever
// becoming a hand-written comparison; a NEW field missing from this list is
// caught by the sizeof pin above, which forces both to be revisited together.
TEST(SplinePlacementRecipe, EveryFieldParticipatesInEquality)
{
    const SplinePlacement base{};
    EXPECT_TRUE(base == SplinePlacement{});

    struct Case
    {
        const char* Name;
        void (*Flip)(SplinePlacement&);
    };
    const Case cases[] = {
        {"StraightPool[0]", [](SplinePlacement& p) { p.StraightPool[0] = TestRef(1); }},
        {"StraightPool[5]", [](SplinePlacement& p)
         { p.StraightPool[kSplinePoolCapacity - 1] = TestRef(2); }},
        {"CurvePool[0]", [](SplinePlacement& p) { p.CurvePool[0] = TestRef(3); }},
        {"ScatterPool[0]", [](SplinePlacement& p) { p.ScatterPool[0] = TestRef(4); }},
        {"Spacing", [](SplinePlacement& p) { p.Spacing = 3.75f; }},
        {"Fit", [](SplinePlacement& p) { p.Fit = Components::SplinePlacementFit::FixedPitch; }},
        {"ConformMode",
         [](SplinePlacement& p) { p.ConformMode = Components::SplinePlacementConform::None; }},
        {"LateralOffset", [](SplinePlacement& p) { p.LateralOffset = -0.5f; }},
        {"SlopeBlend", [](SplinePlacement& p) { p.SlopeBlend = 0.25f; }},
        {"MaxTiltDegrees", [](SplinePlacement& p) { p.MaxTiltDegrees = 12.5f; }},
        {"Seed", [](SplinePlacement& p) { p.Seed = 99u; }},
        {"SeamShearMaxDegrees", [](SplinePlacement& p) { p.SeamShearMaxDegrees = 0.0f; }},
        {"ConformTarget",
         [](SplinePlacement& p)
         { p.ConformTarget = Components::SplineConformTarget::TerrainOnly; }},
        {"PlantMode",
         [](SplinePlacement& p) { p.PlantMode = Components::SplinePlantMode::PivotPlane; }},
        {"SpacingJitterMetres", [](SplinePlacement& p) { p.SpacingJitterMetres = 0.4f; }},
        {"YawJitterDegrees", [](SplinePlacement& p) { p.YawJitterDegrees = 7.5f; }},
        {"LateralJitterMetres", [](SplinePlacement& p) { p.LateralJitterMetres = 0.2f; }},
        {"DropoutChance", [](SplinePlacement& p) { p.DropoutChance = 0.15f; }},
        {"EndTaperMetres", [](SplinePlacement& p) { p.EndTaperMetres = 3.0f; }},
        {"OverrideMaterial", [](SplinePlacement& p) { p.OverrideMaterial = MaterialTestRef(7); }},
    };

    for (const Case& c : cases)
    {
        SplinePlacement flipped = base;
        c.Flip(flipped);
        EXPECT_FALSE(flipped == base) << c.Name << " does not participate in operator==";
    }

    // Every slot of every pool participates, not just the sampled ones above.
    for (uint32 slot = 0; slot < kSplinePoolCapacity; ++slot)
    {
        SplinePlacement s = base;
        s.StraightPool[slot] = TestRef(static_cast<uint8>(slot + 1));
        EXPECT_FALSE(s == base) << "StraightPool[" << slot << "]";

        SplinePlacement cv = base;
        cv.CurvePool[slot] = TestRef(static_cast<uint8>(slot + 1));
        EXPECT_FALSE(cv == base) << "CurvePool[" << slot << "]";

        SplinePlacement sc = base;
        sc.ScatterPool[slot] = TestRef(static_cast<uint8>(slot + 1));
        EXPECT_FALSE(sc == base) << "ScatterPool[" << slot << "]";
    }
}

// ---- Per-station variation: the determinism contract ----

// The zero-behaviour-change promise, stated where a reviewer will look for it:
// a recipe that has never been touched must ask for no variation at all, and
// must conform the way every recipe authored before the knobs existed did.
TEST(SplinePlacementRecipe, VariationKnobsDefaultToNoOps)
{
    const SplinePlacement fresh{};
    EXPECT_FLOAT_EQ(fresh.SpacingJitterMetres, 0.0f);
    EXPECT_FLOAT_EQ(fresh.YawJitterDegrees, 0.0f);
    EXPECT_FLOAT_EQ(fresh.LateralJitterMetres, 0.0f);
    EXPECT_FLOAT_EQ(fresh.DropoutChance, 0.0f);
    EXPECT_FLOAT_EQ(fresh.EndTaperMetres, 0.0f);
    EXPECT_EQ(fresh.ConformTarget, SplineConformTarget::Scene)
        << "the default conform must stay whole-scene, or every existing recipe re-places";
    EXPECT_EQ(fresh.PlantMode, SplinePlantMode::BoundsMin)
        << "the default plant mode must stay base-anchored: PivotPlane sinks every tile by its "
           "pivot depth, so flipping the default silently re-beds every authored path on load";
}

// SplinePlacement DOES have a bespoke inspector, so its rows do not come from
// DefaultComponentInspector's field walk. The row is still fed from reflection:
// AddReflectedEnumRow (SplinePlacementInspector.cpp) reads the field's EnumNames
// AND its Tooltip from ComponentFieldRegistry, and PlantMode is the one row that
// passes no literal at all — the "// @ge-tooltip" marker in SplinePlacement.h is
// its only source. That marker, the component scanner that harvests it into a
// GE_REFLECT_FIELD_TOOLTIP line, and the registry that stores it are three
// separate moving parts and none of them fails loudly, so this asserts the END
// of the chain: what the registry holds at runtime.
//
// What it CANNOT see is the inspector itself — SplinePlacementInspector.cpp is
// not compiled into this target — so a row that stopped asking for the tooltip
// would still pass here.
TEST(SplinePlacementRecipe, PlantModeCarriesItsAuthorTooltipThroughReflection)
{
    const ECS::ComponentTypeId typeId = ECS::GetComponentTypeId<SplinePlacement>();
    const std::span<const ECS::FieldInfo> fields = ECS::ComponentFieldRegistry::Get(typeId);
    ASSERT_FALSE(fields.empty())
        << "SplinePlacement is not reflected at all, so no inspector row of any kind exists";

    const auto field = std::find_if(fields.begin(), fields.end(),
                                    [](const ECS::FieldInfo& f) { return f.Name == "PlantMode"; });
    ASSERT_NE(field, fields.end()) << "PlantMode is not a reflected field";

    EXPECT_FALSE(field->Tooltip.empty())
        << "PlantMode reaches the author as a bare row: the @ge-tooltip marker in "
           "SplinePlacement.h is no longer reaching FieldInfo::Tooltip, and the inspector passes "
           "no literal to fall back on";

    // The clause that earns the tooltip is the consequence of choosing wrong —
    // which plane meets the ground is invisible in the label. A tooltip trimmed
    // back to naming the two modes has stopped doing its job.
    EXPECT_NE(field->Tooltip.find("lowest vertex"), std::string_view::npos)
        << "the tooltip no longer says what Bounds Min anchors on; it reads: " << field->Tooltip;
    EXPECT_NE(field->Tooltip.find("y=0"), std::string_view::npos)
        << "the tooltip no longer says what Pivot Plane anchors on; it reads: " << field->Tooltip;

    // The enum table the same lookup feeds: without it the row reports "enum
    // table missing from reflection" instead of rendering.
    EXPECT_EQ(field->EnumNames.size(), 2u)
        << "the SplinePlantMode enumerator table is not bound to the field";

    // Positive control: an unmarked sibling must still be bare, or the assertion
    // above would pass on a registry that hands every field a string.
    const auto unmarked = std::find_if(
        fields.begin(), fields.end(), [](const ECS::FieldInfo& f) { return f.Name == "Spacing"; });
    ASSERT_NE(unmarked, fields.end());
    EXPECT_TRUE(unmarked->Tooltip.empty())
        << "an unmarked field carries tooltip text, so this test cannot tell a harvested marker "
           "from a default";
}

// Golden draws: a deliberate hash change must edit these values consciously,
// because every authored scene's wobble and survivors move when it does.
TEST(SplineStationJitter, GoldenDraws)
{
    constexpr float32 kSeed0Spacing[8] = {0.93938220f, 0.72043836f, 0.06946343f, 0.80953228f,
                                          0.09822047f, 0.92894351f, 0.98994750f, 0.76695818f};
    constexpr float32 kSeed77Yaw[8] = {0.09440619f, 0.20027035f, 0.37959468f, 0.55619431f,
                                       0.30719084f, 0.49877447f, 0.52622157f, 0.31560338f};
    constexpr float32 kSeed77Dropout[8] = {0.74052316f, 0.22227138f, 0.50580567f, 0.39997739f,
                                           0.74394768f, 0.50233871f, 0.80533963f, 0.04013216f};
    for (uint32 s = 0; s < 8; ++s)
    {
        EXPECT_FLOAT_EQ(SplineJitterUnit(0u, SplineJitterChannel::Spacing, s), kSeed0Spacing[s])
            << "station " << s;
        EXPECT_FLOAT_EQ(SplineJitterUnit(77u, SplineJitterChannel::Yaw, s), kSeed77Yaw[s])
            << "station " << s;
        EXPECT_FLOAT_EQ(SplineJitterUnit(77u, SplineJitterChannel::Dropout, s), kSeed77Dropout[s])
            << "station " << s;
    }
}

TEST(SplineStationJitter, DrawsStayInUnitRangeAndSignedRangeAcrossSeeds)
{
    for (uint32 seed : {0u, 1u, 77u, 0xFFFFFFFFu})
    {
        for (SplineJitterChannel ch : {SplineJitterChannel::Spacing, SplineJitterChannel::Yaw,
                                       SplineJitterChannel::Lateral, SplineJitterChannel::Dropout})
        {
            for (uint32 s = 0; s < 256; ++s)
            {
                const float32 u = SplineJitterUnit(seed, ch, s);
                ASSERT_GE(u, 0.0f) << "seed " << seed << " station " << s;
                ASSERT_LT(u, 1.0f) << "seed " << seed << " station " << s;
                const float32 g = SplineJitterSigned(seed, ch, s);
                ASSERT_GE(g, -1.0f);
                ASSERT_LT(g, 1.0f);
            }
        }
    }
}

// The prefix-stability contract the pool pick already carries, restated for the
// wobble: a station's draw depends on its own ordinal and nothing else, so
// lengthening a spline (or dropping a station elsewhere) cannot move it.
TEST(SplineStationJitter, DrawsArePureInTheirThreeInputs)
{
    std::vector<float32> first;
    for (uint32 s = 0; s < 64; ++s)
        first.push_back(SplineJitterUnit(12345u, SplineJitterChannel::Lateral, s));

    std::vector<float32> second;
    for (uint32 s = 0; s < 64; ++s)
        second.push_back(SplineJitterUnit(12345u, SplineJitterChannel::Lateral, s));
    EXPECT_EQ(first, second);

    // A different seed must actually change the layout, or Seed is decorative.
    bool anyDiffer = false;
    for (uint32 s = 0; s < 64; ++s)
        anyDiffer |= SplineJitterUnit(999u, SplineJitterChannel::Lateral, s) != first[s];
    EXPECT_TRUE(anyDiffer);
}

// Channels must not move together: if yaw tracked spacing, every piece that
// slid forward would also turn the same way and the result would read as one
// coherent slither rather than as noise.
TEST(SplineStationJitter, ChannelsDrawIndependently)
{
    std::vector<float32> spacing, yaw, lateral, dropout;
    for (uint32 s = 0; s < 64; ++s)
    {
        spacing.push_back(SplineJitterUnit(5u, SplineJitterChannel::Spacing, s));
        yaw.push_back(SplineJitterUnit(5u, SplineJitterChannel::Yaw, s));
        lateral.push_back(SplineJitterUnit(5u, SplineJitterChannel::Lateral, s));
        dropout.push_back(SplineJitterUnit(5u, SplineJitterChannel::Dropout, s));
    }
    EXPECT_NE(spacing, yaw);
    EXPECT_NE(spacing, lateral);
    EXPECT_NE(spacing, dropout);
    EXPECT_NE(yaw, lateral);
    EXPECT_NE(yaw, dropout);
    EXPECT_NE(lateral, dropout);
}

// The salt is the whole reason a station's mesh pick and its spacing wobble are
// independent: both hash small integers through the same mix, and channel 0
// would otherwise BE role 0. Reconstruct the unsalted hash and require the real
// draw to differ from it — a dropped or zeroed salt fails here immediately.
TEST(SplineStationJitter, ChannelSaltLiftsDrawsClearOfPoolRoles)
{
    for (uint32 s = 0; s < 32; ++s)
    {
        uint64 h = SplineSelectionMix(3u);
        h = SplineSelectionMix(h ^ static_cast<uint64>(SplinePoolRole::Straight));
        h = SplineSelectionMix(h ^ static_cast<uint64>(s));
        const float32 unsalted = static_cast<float32>(h >> 40) * (1.0f / 16777216.0f);
        EXPECT_NE(SplineJitterUnit(3u, SplineJitterChannel::Spacing, s), unsalted)
            << "station " << s << ": jitter channel 0 collided with pool role 0";
    }
}
