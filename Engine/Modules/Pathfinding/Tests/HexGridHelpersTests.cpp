#include "HexGridHelpers.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Pathfinding;

TEST(HexGridHelpersTest, GetNeighborOffsetsEvenRowValues)
{
    int32 dx[6], dz[6];
    uint32 count = 0;

    HexGrid::GetNeighborOffsets(0, dx, dz, count);

    EXPECT_EQ(count, 6u);
    // Even row offsets (odd-r offset hex grid)
    EXPECT_EQ(dx[0],  1); EXPECT_EQ(dz[0],  0);
    EXPECT_EQ(dx[1],  0); EXPECT_EQ(dz[1], -1);
    EXPECT_EQ(dx[2], -1); EXPECT_EQ(dz[2], -1);
    EXPECT_EQ(dx[3], -1); EXPECT_EQ(dz[3],  0);
    EXPECT_EQ(dx[4], -1); EXPECT_EQ(dz[4],  1);
    EXPECT_EQ(dx[5],  0); EXPECT_EQ(dz[5],  1);
}

TEST(HexGridHelpersTest, GetNeighborOffsetsOddRowValues)
{
    int32 dx[6], dz[6];
    uint32 count = 0;

    HexGrid::GetNeighborOffsets(1, dx, dz, count);

    EXPECT_EQ(count, 6u);
    // Odd row offsets (odd-r offset hex grid)
    EXPECT_EQ(dx[0],  1); EXPECT_EQ(dz[0],  0);
    EXPECT_EQ(dx[1],  1); EXPECT_EQ(dz[1], -1);
    EXPECT_EQ(dx[2],  0); EXPECT_EQ(dz[2], -1);
    EXPECT_EQ(dx[3], -1); EXPECT_EQ(dz[3],  0);
    EXPECT_EQ(dx[4],  0); EXPECT_EQ(dz[4],  1);
    EXPECT_EQ(dx[5],  1); EXPECT_EQ(dz[5],  1);
}

TEST(HexGridHelpersTest, DistanceReturnsZeroForSameCell)
{
    float32 dist = HexGrid::Distance(3, 3, 3, 3);
    EXPECT_FLOAT_EQ(dist, 0.0f);
}

TEST(HexGridHelpersTest, DistanceReturnsOneForAdjacentCells)
{
    float32 dist = HexGrid::Distance(3, 0, 4, 0);
    EXPECT_FLOAT_EQ(dist, 1.0f);

    float32 dist2 = HexGrid::Distance(3, 0, 2, 1);
    EXPECT_FLOAT_EQ(dist2, 1.0f);
}

TEST(HexGridHelpersTest, WorldToHexAndHexToWorldRoundTrip)
{
    const float32 cellSize = 1.0f;
    const float32 originX = 0.0f;
    const float32 originZ = 0.0f;

    float32 worldX = 0.0f, worldZ = 0.0f;
    HexGrid::HexToWorld(3, 2, cellSize, originX, originZ, worldX, worldZ);

    int32 col = 0, row = 0;
    HexGrid::WorldToHex(worldX, worldZ, cellSize, originX, originZ, col, row);

    EXPECT_EQ(col, 3);
    EXPECT_EQ(row, 2);
}

TEST(HexGridHelpersTest, WorldToHexAndHexToWorldRoundTripOddRow)
{
    const float32 cellSize = 1.0f;
    const float32 originX = 0.0f;
    const float32 originZ = 0.0f;

    float32 worldX = 0.0f, worldZ = 0.0f;
    HexGrid::HexToWorld(4, 3, cellSize, originX, originZ, worldX, worldZ);

    int32 col = 0, row = 0;
    HexGrid::WorldToHex(worldX, worldZ, cellSize, originX, originZ, col, row);

    EXPECT_EQ(col, 4);
    EXPECT_EQ(row, 3);
}

TEST(HexGridHelpersTest, DistanceIsSymmetric)
{
    float32 distAB = HexGrid::Distance(1, 2, 5, 7);
    float32 distBA = HexGrid::Distance(5, 7, 1, 2);

    EXPECT_FLOAT_EQ(distAB, distBA);
}

TEST(HexGridHelpersTest, DistanceMultiStep)
{
    // Two steps away in hex grid
    float32 dist = HexGrid::Distance(0, 0, 2, 0);
    EXPECT_FLOAT_EQ(dist, 2.0f);

    // Farther away
    float32 dist2 = HexGrid::Distance(0, 0, 3, 3);
    EXPECT_GT(dist2, 2.0f);
}

TEST(HexGridHelpersTest, WorldToHexWithNonZeroOrigin)
{
    const float32 cellSize = 2.0f;
    const float32 originX = 10.0f;
    const float32 originZ = 20.0f;

    float32 worldX = 0.0f, worldZ = 0.0f;
    HexGrid::HexToWorld(2, 4, cellSize, originX, originZ, worldX, worldZ);

    int32 col = 0, row = 0;
    HexGrid::WorldToHex(worldX, worldZ, cellSize, originX, originZ, col, row);

    EXPECT_EQ(col, 2);
    EXPECT_EQ(row, 4);
}
