#include <gtest/gtest.h>

#include "UI/UICompatDrawRuns.h"
#include "UI/UIPrimitive.h"

#include <vector>

using namespace GameEngine::UI;

// The compat profile has no binding arrays, so the CPU partitions the draw
// order into runs whose distinct textures fit the pipeline's fixed slots. The
// invariants that matter are: painter's order survives (runs are consecutive
// spans covering the whole list exactly once), every entry's ordinal addresses
// the slot its own texture landed in, and a run only ever closes because a slot
// family filled up.
namespace
{

UIPrimitive MakeRect()
{
    UIPrimitive p{};
    p.ModeAndFlags = static_cast<uint32_t>(PrimitiveMode::Rect);
    return p;
}

UIPrimitive MakeTextured(uint32_t textureIndex)
{
    UIPrimitive p{};
    p.ModeAndFlags = static_cast<uint32_t>(PrimitiveMode::Textured);
    p.TextureIndex = textureIndex;
    return p;
}

UIPrimitive MakeSlug(uint32_t curveIndex, uint32_t bandIndex)
{
    UIPrimitive p{};
    p.ModeAndFlags = static_cast<uint32_t>(PrimitiveMode::Slug);
    p.TextureIndex = curveIndex;
    p.GradColor0 = bandIndex;
    return p;
}

UIPrimitive MakeColorGlyph(uint32_t atlasIndex)
{
    UIPrimitive p{};
    p.ModeAndFlags = static_cast<uint32_t>(PrimitiveMode::Slug) | kPrimColorGlyphBit;
    p.TextureIndex = atlasIndex;
    return p;
}

std::vector<uint32_t> Identity(size_t count)
{
    std::vector<uint32_t> order(count);
    for (uint32_t i = 0; i < count; ++i)
        order[i] = i;
    return order;
}

uint32_t SlotOf(uint32_t word) { return word & kUiDrawOrderSlotMask; }
uint32_t OrdinalOf(uint32_t word) { return word >> kUiDrawOrderTexSlotShift; }

// Every run is a consecutive span, the spans tile [0, drawCount) in order, and
// no primitive is dropped or duplicated.
void ExpectRunsTileTheOrder(const UICompatDrawRunBuilder& builder, size_t drawCount)
{
    uint32_t expectedFirst = 0;
    for (const UICompatDrawRun& run : builder.Runs())
    {
        EXPECT_EQ(run.First, expectedFirst);
        EXPECT_GT(run.Count, 0u);
        expectedFirst += run.Count;
    }
    EXPECT_EQ(expectedFirst, drawCount);
}

} // namespace

TEST(UICompatDrawRuns, EmptyOrderProducesNoRuns)
{
    UICompatDrawRunBuilder builder;
    builder.Build({}, {});
    EXPECT_TRUE(builder.Runs().empty());
    EXPECT_TRUE(builder.Words().empty());
}

TEST(UICompatDrawRuns, UntexturedPrimitivesStayOneRun)
{
    std::vector<UIPrimitive> prims(64, MakeRect());
    UICompatDrawRunBuilder builder;
    builder.Build(prims, Identity(prims.size()));

    ASSERT_EQ(builder.Runs().size(), 1u);
    EXPECT_EQ(builder.Runs()[0].Count, 64u);
    EXPECT_EQ(builder.Runs()[0].TextureCount, 0u);
    for (uint32_t word : builder.Words())
        EXPECT_EQ(OrdinalOf(word), 0u);
}

TEST(UICompatDrawRuns, RepeatedTextureClaimsOneSlot)
{
    std::vector<UIPrimitive> prims(32, MakeTextured(7));
    UICompatDrawRunBuilder builder;
    builder.Build(prims, Identity(prims.size()));

    ASSERT_EQ(builder.Runs().size(), 1u);
    EXPECT_EQ(builder.Runs()[0].TextureCount, 1u);
    EXPECT_EQ(builder.Runs()[0].Textures[0], 7u);
    for (uint32_t word : builder.Words())
        EXPECT_EQ(OrdinalOf(word), 0u);
}

TEST(UICompatDrawRuns, DistinctTexturesFillSlotsThenSplit)
{
    // One more distinct texture than the budget: the last one must open a run.
    std::vector<UIPrimitive> prims;
    for (uint32_t i = 0; i <= kUiCompatTextureSlots; ++i)
        prims.push_back(MakeTextured(100 + i));

    UICompatDrawRunBuilder builder;
    builder.Build(prims, Identity(prims.size()));

    ASSERT_EQ(builder.Runs().size(), 2u);
    EXPECT_EQ(builder.Runs()[0].Count, kUiCompatTextureSlots);
    EXPECT_EQ(builder.Runs()[0].TextureCount, kUiCompatTextureSlots);
    EXPECT_EQ(builder.Runs()[1].First, kUiCompatTextureSlots);
    EXPECT_EQ(builder.Runs()[1].Count, 1u);
    ASSERT_EQ(builder.Runs()[1].TextureCount, 1u);
    EXPECT_EQ(builder.Runs()[1].Textures[0], 100 + kUiCompatTextureSlots);

    // The overflowing primitive addresses slot 0 of its NEW run, not slot 8.
    EXPECT_EQ(OrdinalOf(builder.Words()[kUiCompatTextureSlots]), 0u);
    ExpectRunsTileTheOrder(builder, prims.size());
}

TEST(UICompatDrawRuns, GlyphPairsShareOneSlotFamily)
{
    // Curve and band are claimed together, so N glyph pages cost N slots, not
    // N*M — and a repeat of the same PAIR reuses its slot.
    std::vector<UIPrimitive> prims;
    for (uint32_t i = 0; i < kUiCompatGlyphSlots; ++i)
    {
        prims.push_back(MakeSlug(10 + i, 20 + i));
        prims.push_back(MakeSlug(10 + i, 20 + i));
    }

    UICompatDrawRunBuilder builder;
    builder.Build(prims, Identity(prims.size()));

    ASSERT_EQ(builder.Runs().size(), 1u);
    EXPECT_EQ(builder.Runs()[0].GlyphCount, kUiCompatGlyphSlots);
    for (uint32_t i = 0; i < kUiCompatGlyphSlots; ++i)
    {
        EXPECT_EQ(builder.Runs()[0].GlyphCurves[i], 10 + i);
        EXPECT_EQ(builder.Runs()[0].GlyphBands[i], 20 + i);
        EXPECT_EQ(OrdinalOf(builder.Words()[i * 2]), i);
        EXPECT_EQ(OrdinalOf(builder.Words()[i * 2 + 1]), i);
    }
}

TEST(UICompatDrawRuns, GlyphAndTextureFamiliesDoNotCompete)
{
    // A full texture family must not close a run that still has glyph room, and
    // a colour-emoji glyph draws from the CONTENT family, not the glyph family.
    std::vector<UIPrimitive> prims;
    for (uint32_t i = 0; i < kUiCompatTextureSlots; ++i)
        prims.push_back(MakeTextured(200 + i));
    prims.push_back(MakeSlug(1, 2));
    prims.push_back(MakeColorGlyph(200)); // already-claimed content texture

    UICompatDrawRunBuilder builder;
    builder.Build(prims, Identity(prims.size()));

    ASSERT_EQ(builder.Runs().size(), 1u);
    EXPECT_EQ(builder.Runs()[0].TextureCount, kUiCompatTextureSlots);
    EXPECT_EQ(builder.Runs()[0].GlyphCount, 1u);
    EXPECT_EQ(OrdinalOf(builder.Words()[kUiCompatTextureSlots]), 0u);     // glyph slot 0
    EXPECT_EQ(OrdinalOf(builder.Words()[kUiCompatTextureSlots + 1]), 0u); // content slot 0
}

TEST(UICompatDrawRuns, WordsPreserveDrawOrderAndSlotIndices)
{
    // Draw order is painter's order and is NOT sorted by texture: the partition
    // may only decide where a draw ends, never move a primitive.
    std::vector<UIPrimitive> prims = {MakeTextured(1), MakeRect(), MakeTextured(2)};
    const std::vector<uint32_t> order = {2, 0, 1};

    UICompatDrawRunBuilder builder;
    builder.Build(prims, order);

    ASSERT_EQ(builder.Words().size(), order.size());
    for (size_t i = 0; i < order.size(); ++i)
        EXPECT_EQ(SlotOf(builder.Words()[i]), order[i]);
    ASSERT_EQ(builder.Runs().size(), 1u);
    ASSERT_EQ(builder.Runs()[0].TextureCount, 2u);
    EXPECT_EQ(builder.Runs()[0].Textures[0], 2u); // claimed first — it draws first
    EXPECT_EQ(builder.Runs()[0].Textures[1], 1u);
}

TEST(UICompatDrawRuns, DrawOrderEntryPastPrimitiveSpanDoesNotSplit)
{
    // A stale entry cannot be classified, and it cannot render either; it must
    // not cost a run.
    std::vector<UIPrimitive> prims = {MakeRect()};
    const std::vector<uint32_t> order = {0, 4242, 0};

    UICompatDrawRunBuilder builder;
    builder.Build(prims, order);

    ASSERT_EQ(builder.Runs().size(), 1u);
    EXPECT_EQ(builder.Runs()[0].Count, 3u);
    ExpectRunsTileTheOrder(builder, order.size());
}

TEST(UICompatDrawRuns, AlternatingTexturesSplitOncePerBudget)
{
    // Worst realistic shape: many distinct icons interleaved with chrome. Runs
    // must still tile the order, and each must claim at most the budget.
    std::vector<UIPrimitive> prims;
    for (uint32_t i = 0; i < 40; ++i)
    {
        prims.push_back(MakeRect());
        prims.push_back(MakeTextured(1000 + i));
    }

    UICompatDrawRunBuilder builder;
    builder.Build(prims, Identity(prims.size()));

    ExpectRunsTileTheOrder(builder, prims.size());
    for (const UICompatDrawRun& run : builder.Runs())
    {
        EXPECT_LE(run.TextureCount, kUiCompatTextureSlots);
        EXPECT_LE(run.GlyphCount, kUiCompatGlyphSlots);
    }
    EXPECT_EQ(builder.Runs().size(), 40u / kUiCompatTextureSlots);
}
