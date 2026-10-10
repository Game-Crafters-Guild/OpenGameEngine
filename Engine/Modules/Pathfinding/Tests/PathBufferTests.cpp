#include "Pathfinding/PathBuffer.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Pathfinding;

namespace
{

PathPoint MakePathPoint(float32 x, float32 y, float32 z)
{
    return PathPoint{x, y, z};
}

} // namespace

class PathBufferTest : public ::testing::Test
{
protected:
    PathBuffer m_Buffer;
};

TEST_F(PathBufferTest, AllocatePathReturnsValidHandle)
{
    PathPoint points[] = {MakePathPoint(1.0f, 0.0f, 2.0f), MakePathPoint(3.0f, 0.0f, 4.0f)};
    PathHandle handle = m_Buffer.AllocatePath(points, 2);

    EXPECT_TRUE(handle.IsValid());
    EXPECT_EQ(m_Buffer.GetPointCount(handle), 2u);
}

TEST_F(PathBufferTest, FreePathInvalidatesHandle)
{
    PathPoint points[] = {MakePathPoint(1.0f, 0.0f, 2.0f)};
    PathHandle handle = m_Buffer.AllocatePath(points, 1);

    m_Buffer.FreePath(handle);

    EXPECT_EQ(m_Buffer.GetPointCount(handle), 0u);
}

TEST_F(PathBufferTest, GetPointReturnsCorrectData)
{
    PathPoint points[] = {MakePathPoint(1.0f, 2.0f, 3.0f), MakePathPoint(4.0f, 5.0f, 6.0f)};
    PathHandle handle = m_Buffer.AllocatePath(points, 2);

    PathPoint p0 = m_Buffer.GetPoint(handle, 0);
    PathPoint p1 = m_Buffer.GetPoint(handle, 1);

    EXPECT_FLOAT_EQ(p0.X, 1.0f);
    EXPECT_FLOAT_EQ(p0.Y, 2.0f);
    EXPECT_FLOAT_EQ(p0.Z, 3.0f);
    EXPECT_FLOAT_EQ(p1.X, 4.0f);
    EXPECT_FLOAT_EQ(p1.Y, 5.0f);
    EXPECT_FLOAT_EQ(p1.Z, 6.0f);
}

TEST_F(PathBufferTest, GetPointOutOfBoundsReturnsZero)
{
    PathPoint points[] = {MakePathPoint(1.0f, 2.0f, 3.0f)};
    PathHandle handle = m_Buffer.AllocatePath(points, 1);

    PathPoint p = m_Buffer.GetPoint(handle, 99);

    EXPECT_FLOAT_EQ(p.X, 0.0f);
    EXPECT_FLOAT_EQ(p.Y, 0.0f);
    EXPECT_FLOAT_EQ(p.Z, 0.0f);
}

TEST_F(PathBufferTest, CopyPointsCopiesCorrectly)
{
    PathPoint points[] = {MakePathPoint(1.0f, 0.0f, 0.0f), MakePathPoint(2.0f, 0.0f, 0.0f), MakePathPoint(3.0f, 0.0f, 0.0f)};
    PathHandle handle = m_Buffer.AllocatePath(points, 3);

    PathPoint outBuffer[3] = {};
    uint32 copied = m_Buffer.CopyPoints(handle, outBuffer, 3);

    EXPECT_EQ(copied, 3u);
    EXPECT_FLOAT_EQ(outBuffer[0].X, 1.0f);
    EXPECT_FLOAT_EQ(outBuffer[1].X, 2.0f);
    EXPECT_FLOAT_EQ(outBuffer[2].X, 3.0f);
}

TEST_F(PathBufferTest, CopyPointsWithSmallerBufferTruncates)
{
    PathPoint points[] = {MakePathPoint(1.0f, 0.0f, 0.0f), MakePathPoint(2.0f, 0.0f, 0.0f), MakePathPoint(3.0f, 0.0f, 0.0f)};
    PathHandle handle = m_Buffer.AllocatePath(points, 3);

    PathPoint outBuffer[2] = {};
    uint32 copied = m_Buffer.CopyPoints(handle, outBuffer, 2);

    EXPECT_EQ(copied, 2u);
    EXPECT_FLOAT_EQ(outBuffer[0].X, 1.0f);
    EXPECT_FLOAT_EQ(outBuffer[1].X, 2.0f);
}

TEST_F(PathBufferTest, MultipleAllocationsAndFreeListReuse)
{
    PathPoint points[] = {MakePathPoint(1.0f, 0.0f, 0.0f)};

    PathHandle h1 = m_Buffer.AllocatePath(points, 1);
    PathHandle h2 = m_Buffer.AllocatePath(points, 1);

    m_Buffer.FreePath(h1);

    PathPoint newPoints[] = {MakePathPoint(9.0f, 0.0f, 0.0f)};
    PathHandle h3 = m_Buffer.AllocatePath(newPoints, 1);

    EXPECT_EQ(h3.Index, h1.Index);
    EXPECT_NE(h3.Generation, h1.Generation);

    EXPECT_EQ(m_Buffer.GetPointCount(h2), 1u);

    PathPoint p = m_Buffer.GetPoint(h3, 0);
    EXPECT_FLOAT_EQ(p.X, 9.0f);
}

TEST_F(PathBufferTest, GenerationPreventsUseAfterFree)
{
    PathPoint points[] = {MakePathPoint(1.0f, 0.0f, 0.0f)};
    PathHandle h1 = m_Buffer.AllocatePath(points, 1);

    m_Buffer.FreePath(h1);

    PathPoint newPoints[] = {MakePathPoint(5.0f, 0.0f, 0.0f)};
    PathHandle h2 = m_Buffer.AllocatePath(newPoints, 1);

    EXPECT_EQ(h2.Index, h1.Index);

    EXPECT_EQ(m_Buffer.GetPointCount(h1), 0u);
    PathPoint stale = m_Buffer.GetPoint(h1, 0);
    EXPECT_FLOAT_EQ(stale.X, 0.0f);

    EXPECT_EQ(m_Buffer.GetPointCount(h2), 1u);
    PathPoint fresh = m_Buffer.GetPoint(h2, 0);
    EXPECT_FLOAT_EQ(fresh.X, 5.0f);
}

TEST_F(PathBufferTest, ClearRemovesAllEntries)
{
    PathPoint points[] = {MakePathPoint(1.0f, 0.0f, 0.0f)};
    PathHandle h1 = m_Buffer.AllocatePath(points, 1);
    PathHandle h2 = m_Buffer.AllocatePath(points, 1);

    m_Buffer.Clear();

    EXPECT_EQ(m_Buffer.GetPointCount(h1), 0u);
    EXPECT_EQ(m_Buffer.GetPointCount(h2), 0u);
}

TEST_F(PathBufferTest, CopyPointsZeroCapacityReturnsZero)
{
    PathPoint points[] = {MakePathPoint(1.0f, 2.0f, 3.0f)};
    PathHandle h = m_Buffer.AllocatePath(points, 1);

    uint32 copied = m_Buffer.CopyPoints(h, nullptr, 0);
    EXPECT_EQ(copied, 0u);
}

TEST_F(PathBufferTest, CopyPointsInvalidHandleReturnsZero)
{
    PathHandle invalid{999, 999};
    PathPoint outBuffer[4];
    uint32 copied = m_Buffer.CopyPoints(invalid, outBuffer, 4);
    EXPECT_EQ(copied, 0u);
}
