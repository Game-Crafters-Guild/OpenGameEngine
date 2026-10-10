#include "Components/Spline/SplineFence.h"
#include "Components/Spline/SplinePoolSelection.h"
#include "Placement/FenceLayout.h"

#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <vector>

using namespace GameEngine;
using Components::ActivePoolCount;
using Components::kSplineFencePoolCapacity;
using Components::MaterialRef;
using Components::ModelRef;
using Components::PoolHasHole;
using Components::SplineFence;
using Components::SplinePoolRole;
using Components::SplinePoolSelect;
using Editor::SanitizeFenceRecipe;

namespace
{

// A distinct, non-null GUID per ordinal for pool contents.
ModelRef TestRef(uint8 ordinal)
{
    GUID::Data data{};
    data[0] = ordinal;
    data[15] = 0x7E;
    return ModelRef(GUID(data));
}

// Same shape for the override-material slot, in a disjoint GUID space so a
// material can never compare equal to a pool mesh.
MaterialRef MaterialTestRef(uint8 ordinal)
{
    GUID::Data data{};
    data[0] = ordinal;
    data[15] = 0xD2;
    return MaterialRef(GUID(data));
}

} // namespace

// The pool helpers are templated on the array bound precisely so the two
// recipes can size their pools from their own kits. Capacity 7 comes from the
// castle battlement family (6) plus one.
TEST(SplineFenceRecipe, PoolHelpersWorkAtFenceCapacity)
{
    EXPECT_EQ(kSplineFencePoolCapacity, 7u);

    ModelRef pool[kSplineFencePoolCapacity] = {};
    EXPECT_EQ(ActivePoolCount(pool), 0u);
    EXPECT_FALSE(PoolHasHole(pool));

    for (uint32 i = 0; i < kSplineFencePoolCapacity; ++i)
        pool[i] = TestRef(static_cast<uint8>(i + 1));
    EXPECT_EQ(ActivePoolCount(pool), kSplineFencePoolCapacity);
    EXPECT_FALSE(PoolHasHole(pool));

    // (valid, empty, valid) selects from the leading prefix only, and reports.
    ModelRef holed[kSplineFencePoolCapacity] = {};
    holed[0] = TestRef(1);
    holed[2] = TestRef(3);
    EXPECT_EQ(ActivePoolCount(holed), 1u);
    EXPECT_TRUE(PoolHasHole(holed));
}

// Role salts are an on-disk-adjacent contract: renumbering one reshuffles every
// authored scene's picks. Post/Span/Gate must draw independently of each other
// AND of the tile roles that share the enum.
TEST(SplineFenceRecipe, FenceRolesDrawIndependently)
{
    std::vector<uint32> post, span, gate, straight;
    for (uint32 i = 0; i < 32; ++i)
    {
        post.push_back(SplinePoolSelect(9u, SplinePoolRole::Post, i, 5u));
        span.push_back(SplinePoolSelect(9u, SplinePoolRole::Span, i, 5u));
        gate.push_back(SplinePoolSelect(9u, SplinePoolRole::Gate, i, 5u));
        straight.push_back(SplinePoolSelect(9u, SplinePoolRole::Straight, i, 5u));
    }
    EXPECT_NE(post, span);
    EXPECT_NE(post, gate);
    EXPECT_NE(span, gate);
    EXPECT_NE(post, straight);
    EXPECT_NE(span, straight);
}

// Golden picks for the fence roles: a deliberate hash or salt change must edit
// these values consciously.
TEST(SplineFenceRecipe, FenceRoleGoldenPicks)
{
    constexpr uint32 kPostSeed0Count3[8] = {2, 2, 2, 2, 0, 0, 2, 0};
    constexpr uint32 kSpanSeed0Count3[8] = {0, 0, 0, 0, 2, 2, 2, 1};
    for (uint32 s = 0; s < 8; ++s)
    {
        EXPECT_EQ(SplinePoolSelect(0u, SplinePoolRole::Post, s, 3u), kPostSeed0Count3[s])
            << "post station " << s;
        EXPECT_EQ(SplinePoolSelect(0u, SplinePoolRole::Span, s, 3u), kSpanSeed0Count3[s])
            << "span ordinal " << s;
    }
}

// Chunk-resident authored data: five fixed pools plus the override table must
// not balloon the component. 5 * 7 * 16 (pools) + 8 * 4 (scalars/enums) +
// 16 * 8 (overrides) + 4 (Seed) + 16 (override material) = 740 bytes, no
// padding. The pin is exact, so every field added to the recipe edits it
// deliberately; the budget is 768 bytes, which holds one more scalar family and
// no sixth pool.
TEST(SplineFenceRecipe, ComponentSizeStaysEcsFriendly)
{
    RecordProperty("sizeof_SplineFence", static_cast<int>(sizeof(SplineFence)));
    EXPECT_EQ(sizeof(SplineFence), 740u);
}

// The rebuild scheduler compares whole recipes; flipping any single field must
// break equality, or edits to that field silently stop scheduling rebuilds.
TEST(SplineFenceRecipe, EveryFieldParticipatesInEquality)
{
    const SplineFence base{};
    EXPECT_TRUE(base == SplineFence{});

    struct Case
    {
        const char* Name;
        void (*Flip)(SplineFence&);
    };
    const Case cases[] = {
        {"PostPool[0]", [](SplineFence& f) { f.PostPool[0] = TestRef(1); }},
        {"PostPool[last]",
         [](SplineFence& f) { f.PostPool[kSplineFencePoolCapacity - 1] = TestRef(2); }},
        {"SpanPool[0]", [](SplineFence& f) { f.SpanPool[0] = TestRef(3); }},
        {"SpanPool[last]",
         [](SplineFence& f) { f.SpanPool[kSplineFencePoolCapacity - 1] = TestRef(4); }},
        {"GatePool[0]", [](SplineFence& f) { f.GatePool[0] = TestRef(5); }},
        {"CrestPool[0]", [](SplineFence& f) { f.CrestPool[0] = TestRef(6); }},
        {"CrestPool[last]",
         [](SplineFence& f) { f.CrestPool[kSplineFencePoolCapacity - 1] = TestRef(7); }},
        {"CapPool[0]", [](SplineFence& f) { f.CapPool[0] = TestRef(8); }},
        {"CapPool[last]",
         [](SplineFence& f) { f.CapPool[kSplineFencePoolCapacity - 1] = TestRef(9); }},
        {"PostPitch", [](SplineFence& f) { f.PostPitch = 3.75f; }},
        {"CrestPitch", [](SplineFence& f) { f.CrestPitch = 1.5f; }},
        {"ConformMode",
         [](SplineFence& f) { f.ConformMode = Components::SplinePlacementConform::None; }},
        {"ConformTarget",
         [](SplineFence& f)
         { f.ConformTarget = Components::SplineConformTarget::TerrainOnly; }},
        {"SlopeBlend", [](SplineFence& f) { f.SlopeBlend = 0.5f; }},
        {"SpanMaxStretch", [](SplineFence& f) { f.SpanMaxStretch = 1.6f; }},
        {"PlantMode",
         [](SplineFence& f) { f.PlantMode = Components::SplinePlantMode::BoundsMin; }},
        {"SpanGrade", [](SplineFence& f) { f.SpanGrade = Components::SplineSpanGrade::Stepped; }},
        {"Overrides[0].PointIndex", [](SplineFence& f) { f.Overrides[0].PointIndex = 3u; }},
        {"Overrides[0].SpanOrdinal", [](SplineFence& f) { f.Overrides[0].SpanOrdinal = 2u; }},
        {"Overrides[0].Kind",
         [](SplineFence& f)
         { f.Overrides[0].Kind = Components::SplineSpanOverrideKind::ExplicitPiece; }},
        {"Overrides[0].PoolSlot", [](SplineFence& f) { f.Overrides[0].PoolSlot = 4u; }},
        {"Overrides[last].PointIndex",
         [](SplineFence& f)
         { f.Overrides[Components::kSplineFenceMaxSpanOverrides - 1].PointIndex = 9u; }},
        {"Seed", [](SplineFence& f) { f.Seed = 99u; }},
        {"OverrideMaterial", [](SplineFence& f) { f.OverrideMaterial = MaterialTestRef(7); }},
    };

    for (const Case& c : cases)
    {
        SplineFence flipped = base;
        c.Flip(flipped);
        EXPECT_FALSE(flipped == base) << c.Name << " does not participate in operator==";
    }
}

// The trap the defaulted operator== creates: a NaN field makes a recipe unequal
// to ITSELF, and the settle-timer rebuild would then re-arm every frame for as
// long as the field stayed poisoned. Sanitizing on read is the single gate.
TEST(SplineFenceRecipe, NaNRecipeIsSelfUnequalUntilSanitized)
{
    SplineFence poisoned{};
    poisoned.PostPitch = std::numeric_limits<float32>::quiet_NaN();
    // The hazard, stated so it cannot be argued away: this is what would loop.
    EXPECT_FALSE(poisoned == poisoned);

    const SplineFence clean = SanitizeFenceRecipe(poisoned);
    EXPECT_TRUE(clean == clean);
    EXPECT_TRUE(clean == SanitizeFenceRecipe(poisoned));
    EXPECT_FLOAT_EQ(clean.PostPitch, SplineFence{}.PostPitch);
}

TEST(SplineFenceRecipe, SanitizeRepairsEveryFloatAndKeepsTheRest)
{
    const float32 nan = std::numeric_limits<float32>::quiet_NaN();
    const float32 inf = std::numeric_limits<float32>::infinity();
    const SplineFence defaults{};

    SplineFence poisoned{};
    poisoned.PostPitch = nan;
    poisoned.SlopeBlend = inf;
    poisoned.SpanMaxStretch = -nan;
    poisoned.CrestPitch = nan;
    poisoned.Seed = 1234u;
    poisoned.SpanGrade = Components::SplineSpanGrade::Stepped;
    poisoned.ConformTarget = Components::SplineConformTarget::TerrainOnly;
    poisoned.PostPool[0] = TestRef(1);

    const SplineFence clean = SanitizeFenceRecipe(poisoned);
    // A non-finite value carries no authored intent, so it falls back to the
    // default rather than being clamped into range — clamping +inf to 1.0
    // would invent a decision the author never made.
    EXPECT_FLOAT_EQ(clean.PostPitch, defaults.PostPitch);
    EXPECT_FLOAT_EQ(clean.SlopeBlend, defaults.SlopeBlend);
    EXPECT_FLOAT_EQ(clean.SpanMaxStretch, defaults.SpanMaxStretch);
    EXPECT_FLOAT_EQ(clean.CrestPitch, defaults.CrestPitch);
    // Untouched fields survive verbatim.
    EXPECT_EQ(clean.Seed, 1234u);
    EXPECT_EQ(clean.SpanGrade, Components::SplineSpanGrade::Stepped);
    EXPECT_EQ(clean.ConformTarget, Components::SplineConformTarget::TerrainOnly)
        << "sanitization dropped the conform target, so a terrain-only fence would silently "
           "re-place itself against whatever scenery overhangs it";
    EXPECT_EQ(clean.PostPool[0], poisoned.PostPool[0]);

    // Out-of-domain but finite values are clamped into the domain the layout
    // can act on, so a hand-edited scene cannot divide by zero downstream.
    SplineFence silly{};
    silly.PostPitch = -5.0f;
    silly.SpanMaxStretch = 0.25f;
    silly.SlopeBlend = 7.0f;
    silly.CrestPitch = -2.0f;
    const SplineFence fixed = SanitizeFenceRecipe(silly);
    EXPECT_GT(fixed.PostPitch, 0.0f);
    EXPECT_GE(fixed.SpanMaxStretch, 1.0f);
    EXPECT_FLOAT_EQ(fixed.SlopeBlend, 1.0f);
    // A negative pitch spaces nothing, so it reads as 0 — the longest crest
    // piece's own length — rather than as a distance to lay cells at.
    EXPECT_FLOAT_EQ(fixed.CrestPitch, 0.0f);
}

// The controller schedules rebuilds by comparing SANITIZED recipes, so the
// override material only re-places the fence if it survives sanitization
// distinguishably. Equality on the raw recipe (the flip test above) does not
// prove that; this closes the chain the scheduler actually walks.
TEST(SplineFenceRecipe, SanitizePreservesOverrideMaterialAndKeepsItRebuildTriggering)
{
    SplineFence authored{};
    authored.PostPool[0] = TestRef(1);

    SplineFence retextured = authored;
    retextured.OverrideMaterial = MaterialTestRef(3);

    const SplineFence cleanBase = SanitizeFenceRecipe(authored);
    const SplineFence cleanRetextured = SanitizeFenceRecipe(retextured);

    EXPECT_EQ(cleanRetextured.OverrideMaterial, retextured.OverrideMaterial);
    EXPECT_TRUE(cleanBase.OverrideMaterial.IsNull());
    EXPECT_FALSE(cleanRetextured == cleanBase);

    // Clearing it back is equally visible, so removing an override re-places
    // the fence on the embedded materials instead of stranding the old look.
    SplineFence cleared = retextured;
    cleared.OverrideMaterial.Clear();
    EXPECT_TRUE(SanitizeFenceRecipe(cleared) == cleanBase);
}
