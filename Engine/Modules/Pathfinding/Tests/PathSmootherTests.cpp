#include "PathSmoother.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/PathBuffer.h"

#include <gtest/gtest.h>

#include <memory>

using namespace GameEngine;
using namespace GameEngine::Pathfinding;

namespace
{

GridSettings MakeSmootherSettings(uint32 width = 16, uint32 depth = 16, float32 cellSize = 1.0f)
{
    GridSettings settings;
    settings.Type = GridType::Square;
    settings.CellSize = cellSize;
    settings.OriginX = 0.0f;
    settings.OriginY = 0.0f;
    settings.OriginZ = 0.0f;
    settings.Width = width;
    settings.Depth = depth;
    return settings;
}

PathPoint MakePoint(float32 x, float32 y, float32 z)
{
    PathPoint p;
    p.X = x;
    p.Y = y;
    p.Z = z;
    return p;
}

} // namespace

class PathSmootherTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Map = std::make_unique<GridMap>(MakeSmootherSettings());
    }

    std::unique_ptr<GridMap> m_Map;
};

TEST_F(PathSmootherTest, SmoothStraightLineRemovesIntermediatePoints)
{
    // 10 collinear points along a straight horizontal line (all in same row)
    std::vector<PathPoint> points;
    for (uint32 i = 0; i < 10; ++i)
    {
        float32 worldX = 0.0f, worldZ = 0.0f;
        m_Map->CellToWorld(i, 4, worldX, worldZ);
        points.push_back(MakePoint(worldX, 0.0f, worldZ));
    }

    uint32 count = PathSmoother::Smooth(*m_Map, points);

    // Should reduce to just start and end
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(points.size(), 2u);
}

TEST_F(PathSmootherTest, SmoothPreservesCornerPoints)
{
    // L-shaped path: right along row 2, then down along column 8
    std::vector<PathPoint> points;

    // Horizontal segment (cells 2,2 to 8,2)
    for (uint32 x = 2; x <= 8; ++x)
    {
        float32 worldX = 0.0f, worldZ = 0.0f;
        m_Map->CellToWorld(x, 2, worldX, worldZ);
        points.push_back(MakePoint(worldX, 0.0f, worldZ));
    }

    // Block cells that would allow a diagonal shortcut past the corner
    // Block a wall along the diagonal so the corner can't be cut
    m_Map->SetCellBlocked(7, 3, true);
    m_Map->SetCellBlocked(8, 3, true);
    m_Map->SetCellBlocked(9, 3, true);
    m_Map->SetCellBlocked(9, 4, true);
    m_Map->SetCellBlocked(9, 5, true);

    // Vertical segment (cells 8,3 to 8,8)
    for (uint32 z = 3; z <= 8; ++z)
    {
        float32 worldX = 0.0f, worldZ = 0.0f;
        m_Map->CellToWorld(8, z, worldX, worldZ);
        points.push_back(MakePoint(worldX, 0.0f, worldZ));
    }

    uint32 count = PathSmoother::Smooth(*m_Map, points);

    // Should be fewer than original but keep at least 3 points (start, corner area, end)
    EXPECT_GE(count, 3u);
    EXPECT_LT(count, static_cast<uint32>(13)); // less than original 13

    // First and last points must be preserved
    float32 startX = 0.0f, startZ = 0.0f;
    m_Map->CellToWorld(2, 2, startX, startZ);
    EXPECT_FLOAT_EQ(points.front().X, startX);
    EXPECT_FLOAT_EQ(points.front().Z, startZ);

    float32 endX = 0.0f, endZ = 0.0f;
    m_Map->CellToWorld(8, 8, endX, endZ);
    EXPECT_FLOAT_EQ(points.back().X, endX);
    EXPECT_FLOAT_EQ(points.back().Z, endZ);
}

TEST_F(PathSmootherTest, SmoothPathWithTwoPointsUnchanged)
{
    std::vector<PathPoint> points;
    points.push_back(MakePoint(0.5f, 0.0f, 0.5f));
    points.push_back(MakePoint(5.5f, 0.0f, 5.5f));

    uint32 count = PathSmoother::Smooth(*m_Map, points);

    EXPECT_EQ(count, 2u);
    EXPECT_EQ(points.size(), 2u);
}

TEST_F(PathSmootherTest, SmoothPathWithOnePointUnchanged)
{
    std::vector<PathPoint> points;
    points.push_back(MakePoint(0.5f, 0.0f, 0.5f));

    uint32 count = PathSmoother::Smooth(*m_Map, points);

    EXPECT_EQ(count, 1u);
    EXPECT_EQ(points.size(), 1u);
}

TEST_F(PathSmootherTest, SmoothAroundObstacle)
{
    // Create a wall of blocked cells across the middle of the grid
    // Block cells (3..12, 8) to force a path around
    for (uint32 x = 3; x <= 12; ++x)
    {
        m_Map->SetCellBlocked(x, 8, true);
    }

    // Build a path that goes around the obstacle: start at (5,5), go right to (13,5),
    // then down to (13,10), then left to (5,10)
    // This simulates what A* would produce to go from (5,5) to (5,10) around the wall
    std::vector<PathPoint> points;

    // Go right along row 5
    for (uint32 x = 5; x <= 13; ++x)
    {
        float32 worldX = 0.0f, worldZ = 0.0f;
        m_Map->CellToWorld(x, 5, worldX, worldZ);
        points.push_back(MakePoint(worldX, 0.0f, worldZ));
    }

    // Go down along column 13
    for (uint32 z = 6; z <= 10; ++z)
    {
        float32 worldX = 0.0f, worldZ = 0.0f;
        m_Map->CellToWorld(13, z, worldX, worldZ);
        points.push_back(MakePoint(worldX, 0.0f, worldZ));
    }

    // Go left along row 10
    for (uint32 x = 12; x >= 5; --x)
    {
        float32 worldX = 0.0f, worldZ = 0.0f;
        m_Map->CellToWorld(x, 10, worldX, worldZ);
        points.push_back(MakePoint(worldX, 0.0f, worldZ));
    }

    const auto originalSize = points.size();
    uint32 count = PathSmoother::Smooth(*m_Map, points);

    // Should reduce the path
    EXPECT_LT(count, static_cast<uint32>(originalSize));

    // All remaining points must be on navigable cells
    for (const auto& point : points)
    {
        uint32 cellX = 0, cellZ = 0;
        ASSERT_TRUE(m_Map->WorldToCell(point.X, point.Z, cellX, cellZ));
        EXPECT_FALSE(m_Map->IsCellBlocked(cellX, cellZ))
            << "Smoothed path point at cell (" << cellX << ", " << cellZ << ") is blocked";
    }

    // Start and end preserved
    float32 startX = 0.0f, startZ = 0.0f;
    m_Map->CellToWorld(5, 5, startX, startZ);
    EXPECT_FLOAT_EQ(points.front().X, startX);
    EXPECT_FLOAT_EQ(points.front().Z, startZ);

    float32 endX = 0.0f, endZ = 0.0f;
    m_Map->CellToWorld(5, 10, endX, endZ);
    EXPECT_FLOAT_EQ(points.back().X, endX);
    EXPECT_FLOAT_EQ(points.back().Z, endZ);
}
