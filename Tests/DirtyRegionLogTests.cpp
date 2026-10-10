#include <gtest/gtest.h>

#include "TerrainECS/DirtyRegionLog.h"
#include "TerrainECS/TerrainService.h"

namespace
{
using namespace GameEngine;
using namespace GameEngine::TerrainECS;

using Region = DirtyRegionLog::Region;

Region MakeRegion(int32 minX, int32 minZ, int32 maxX, int32 maxZ)
{
    Region r;
    r.MinX = minX;
    r.MinZ = minZ;
    r.MaxX = maxX;
    r.MaxZ = maxZ;
    return r;
}

TEST(DirtyRegionLogTests, EmptyLogHasNothingToCollect)
{
    DirtyRegionLog log;
    Region out;
    EXPECT_FALSE(log.CollectSince(0, out));
    EXPECT_EQ(log.LatestVersion(), 0u);
}

TEST(DirtyRegionLogTests, CursorSeesOnlyNewerEntries)
{
    DirtyRegionLog log;
    log.Append(1, MakeRegion(0, 0, 100, 100));
    log.Append(2, MakeRegion(10, 20, 30, 40));

    Region out;
    ASSERT_TRUE(log.CollectSince(1, out));
    EXPECT_EQ(out.MinX, 10);
    EXPECT_EQ(out.MinZ, 20);
    EXPECT_EQ(out.MaxX, 30);
    EXPECT_EQ(out.MaxZ, 40);

    ASSERT_TRUE(log.CollectSince(0, out));
    EXPECT_EQ(out.MinX, 0);
    EXPECT_EQ(out.MaxX, 100);

    EXPECT_FALSE(log.CollectSince(2, out));
    EXPECT_EQ(log.LatestVersion(), 2u);
}

TEST(DirtyRegionLogTests, TwoConsumersDoNotStarveEachOther)
{
    // The bug this class exists to fix: with a single clearable rect, the
    // first consumer's clear left the second reading nothing.
    DirtyRegionLog log;
    log.Append(1, MakeRegion(5, 5, 15, 15));

    uint64 cursorA = 0;
    uint64 cursorB = 0;

    Region outA;
    ASSERT_TRUE(log.CollectSince(cursorA, outA));
    cursorA = log.LatestVersion();

    // Consumer B still sees the same region after A consumed.
    Region outB;
    ASSERT_TRUE(log.CollectSince(cursorB, outB));
    EXPECT_EQ(outB.MinX, 5);
    EXPECT_EQ(outB.MaxX, 15);
    cursorB = log.LatestVersion();

    EXPECT_FALSE(log.CollectSince(cursorA, outA));
    EXPECT_FALSE(log.CollectSince(cursorB, outB));

    // A new edit reaches both consumers independently again.
    log.Append(2, MakeRegion(50, 50, 60, 60));
    ASSERT_TRUE(log.CollectSince(cursorA, outA));
    ASSERT_TRUE(log.CollectSince(cursorB, outB));
}

TEST(DirtyRegionLogTests, OverflowCoalescesConservatively)
{
    DirtyRegionLog log;

    // Append far past capacity with small disjoint regions at ascending versions.
    constexpr uint32 kAppends = 64;
    for (uint32 i = 0; i < kAppends; ++i)
    {
        const int32 base = static_cast<int32>(i) * 10;
        log.Append(i + 1, MakeRegion(base, base, base + 5, base + 5));
    }

    // A fresh cursor gets exactly the newest entry.
    Region out;
    ASSERT_TRUE(log.CollectSince(kAppends - 1, out));
    const int32 lastBase = static_cast<int32>(kAppends - 1) * 10;
    EXPECT_EQ(out.MinX, lastBase);
    EXPECT_EQ(out.MaxX, lastBase + 5);

    // A stale cursor must get a region that CONTAINS everything appended
    // after it — coalescing may widen the answer, never narrow it.
    const uint64 staleCursor = 3;
    ASSERT_TRUE(log.CollectSince(staleCursor, out));
    for (uint32 i = static_cast<uint32>(staleCursor); i < kAppends; ++i)
    {
        const int32 base = static_cast<int32>(i) * 10;
        EXPECT_TRUE(out.Contains(MakeRegion(base, base, base + 5, base + 5)))
            << "coalesced region lost the edit at version " << (i + 1);
    }
}

TEST(DirtyRegionLogTests, CreateTerrainSeedsFullDirtyEntry)
{
    if (TerrainService::IsInitialized())
        TerrainService::Shutdown();
    TerrainService::Initialize();

    Terrain::TerrainConfig cfg{};
    cfg.HeightmapWidth = 65;
    cfg.HeightmapHeight = 65;
    cfg.WorldSizeX = 128.0f;
    cfg.WorldSizeZ = 128.0f;
    cfg.HeightScale = 32.0f;
    cfg.LODLevels = 3;

    auto& svc = TerrainService::Get();
    const TerrainHandle handle = svc.CreateTerrain(cfg);
    auto* data = svc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    // A brand-new consumer (cursor 0) sees the initial full rect.
    Region out;
    ASSERT_TRUE(data->HeightfieldDirtyLog.CollectSince(0, out));
    EXPECT_EQ(out.MinX, 0);
    EXPECT_EQ(out.MinZ, 0);
    EXPECT_EQ(out.MaxX, 65);
    EXPECT_EQ(out.MaxZ, 65);
    EXPECT_EQ(data->HeightfieldVersion, 1u);
    EXPECT_EQ(data->HeightfieldDirtyLog.LatestVersion(), 1u);

    // MarkRegionDirty appends without disturbing older consumers' view.
    data->MarkRegionDirty(4, 8, 12, 16);
    ASSERT_TRUE(data->HeightfieldDirtyLog.CollectSince(1, out));
    EXPECT_EQ(out.MinX, 4);
    EXPECT_EQ(out.MinZ, 8);
    EXPECT_EQ(out.MaxX, 12);
    EXPECT_EQ(out.MaxZ, 16);

    TerrainService::Shutdown();
}

} // namespace
