// ServedGenerationTable: the per-frame (material, pass class) -> served
// generation record the variant cache writes from its serve path. These pin
// the table's own semantics — frame scoping, per-class independence, the
// conflict verdict, out-of-range tolerance — so the recompile-window
// comparison a later pass makes rests on a table whose rules are stated.

#include <gtest/gtest.h>

#include "Engine/Rendering/ServedGenerationTable.h"

#include <optional>

using GameEngine::Engine::Renderer::ServedGenerationTable;
using GameEngine::Engine::Renderer::ServedPassClass;

TEST(ServedGenerationTableTest, UnservedReadsAsUnset)
{
    ServedGenerationTable table;
    table.BeginFrame(4u);
    EXPECT_EQ(table.Get(0u, ServedPassClass::Color), std::nullopt);
    EXPECT_EQ(table.Get(3u, ServedPassClass::Depth), std::nullopt);
}

TEST(ServedGenerationTableTest, RecordsPerMaterialAndPassClassIndependently)
{
    ServedGenerationTable table;
    table.BeginFrame(4u);
    table.Record(1u, ServedPassClass::Color, 7u);
    table.Record(1u, ServedPassClass::Depth, 6u);
    table.Record(2u, ServedPassClass::Color, 9u);

    EXPECT_EQ(table.Get(1u, ServedPassClass::Color), std::optional<uint32_t>(7u));
    EXPECT_EQ(table.Get(1u, ServedPassClass::Depth), std::optional<uint32_t>(6u));
    EXPECT_EQ(table.Get(2u, ServedPassClass::Color), std::optional<uint32_t>(9u));
    EXPECT_EQ(table.Get(2u, ServedPassClass::Depth), std::nullopt);
    EXPECT_EQ(table.Get(0u, ServedPassClass::Color), std::nullopt);
}

// The recompile window: the colour draws served the stale generation while
// the depth draws already serve the fresh one. The table reports both so the
// comparison a consumer makes is between two recorded facts, not a guess.
TEST(ServedGenerationTableTest, DisagreeingPassClassesAreBothObservable)
{
    ServedGenerationTable table;
    table.BeginFrame(2u);
    table.Record(0u, ServedPassClass::Color, 3u);
    table.Record(0u, ServedPassClass::Depth, 4u);
    EXPECT_NE(table.Get(0u, ServedPassClass::Color), table.Get(0u, ServedPassClass::Depth));
}

TEST(ServedGenerationTableTest, SameGenerationTwiceIsNotAConflict)
{
    ServedGenerationTable table;
    table.BeginFrame(1u);
    table.Record(0u, ServedPassClass::Color, 5u);
    table.Record(0u, ServedPassClass::Color, 5u);
    EXPECT_EQ(table.Get(0u, ServedPassClass::Color), std::optional<uint32_t>(5u));
}

TEST(ServedGenerationTableTest, TwoGenerationsInOneFrameReadAsConflict)
{
    ServedGenerationTable table;
    table.BeginFrame(1u);
    table.Record(0u, ServedPassClass::Color, 5u);
    table.Record(0u, ServedPassClass::Color, 6u);
    EXPECT_EQ(table.Get(0u, ServedPassClass::Color),
              std::optional<uint32_t>(ServedGenerationTable::kConflict));
    // A conflict is sticky for the frame: a later agreeing record cannot hide it.
    table.Record(0u, ServedPassClass::Color, 5u);
    EXPECT_EQ(table.Get(0u, ServedPassClass::Color),
              std::optional<uint32_t>(ServedGenerationTable::kConflict));
}

TEST(ServedGenerationTableTest, NewFrameForgetsLastFramesRecords)
{
    ServedGenerationTable table;
    table.BeginFrame(2u);
    table.Record(1u, ServedPassClass::Color, 8u);
    table.Record(1u, ServedPassClass::Depth, 8u);

    table.BeginFrame(2u);
    EXPECT_EQ(table.Get(1u, ServedPassClass::Color), std::nullopt);
    EXPECT_EQ(table.Get(1u, ServedPassClass::Depth), std::nullopt);

    // A record in the new frame replaces the stale one rather than conflicting
    // with it: the previous frame's generation is not part of this frame.
    table.Record(1u, ServedPassClass::Color, 9u);
    EXPECT_EQ(table.Get(1u, ServedPassClass::Color), std::optional<uint32_t>(9u));
}

TEST(ServedGenerationTableTest, IndicesPastTheSizingAreIgnored)
{
    ServedGenerationTable table;
    table.BeginFrame(2u);
    table.Record(2u, ServedPassClass::Color, 1u); // one past the sizing
    EXPECT_EQ(table.Get(2u, ServedPassClass::Color), std::nullopt);
    EXPECT_EQ(table.Get(1u, ServedPassClass::Color), std::nullopt);

    // Growing on a later frame admits the index.
    table.BeginFrame(3u);
    table.Record(2u, ServedPassClass::Color, 1u);
    EXPECT_EQ(table.Get(2u, ServedPassClass::Color), std::optional<uint32_t>(1u));
}

TEST(ServedGenerationTableTest, GrowthKeepsEarlierIndicesAddressable)
{
    ServedGenerationTable table;
    table.BeginFrame(1u);
    table.BeginFrame(64u);
    table.Record(0u, ServedPassClass::Depth, 2u);
    table.Record(63u, ServedPassClass::Depth, 3u);
    EXPECT_EQ(table.Get(0u, ServedPassClass::Depth), std::optional<uint32_t>(2u));
    EXPECT_EQ(table.Get(63u, ServedPassClass::Depth), std::optional<uint32_t>(3u));
}
