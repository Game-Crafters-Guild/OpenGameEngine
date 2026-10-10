#include "HexGridHelpers.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Pathfinding::HexGrid
{

static constexpr float32 kSqrt3Over2 = 0.86602540378f; // sqrt(3) / 2

void GetNeighborOffsets(uint32 row, int32* outDx, int32* outDz, uint32& outCount)
{
    outCount = 6;

    if ((row & 1) == 0)
    {
        // Even row
        outDx[0] =  1; outDz[0] =  0;
        outDx[1] =  0; outDz[1] = -1;
        outDx[2] = -1; outDz[2] = -1;
        outDx[3] = -1; outDz[3] =  0;
        outDx[4] = -1; outDz[4] =  1;
        outDx[5] =  0; outDz[5] =  1;
    }
    else
    {
        // Odd row
        outDx[0] =  1; outDz[0] =  0;
        outDx[1] =  1; outDz[1] = -1;
        outDx[2] =  0; outDz[2] = -1;
        outDx[3] = -1; outDz[3] =  0;
        outDx[4] =  0; outDz[4] =  1;
        outDx[5] =  1; outDz[5] =  1;
    }
}

float32 Distance(uint32 ax, uint32 az, uint32 bx, uint32 bz)
{
    // Convert odd-r offset to cube coordinates
    // q = col - (row - (row & 1)) / 2
    // r = row
    // s = -q - r
    const int32 aq = static_cast<int32>(ax) - (static_cast<int32>(az) - static_cast<int32>(az & 1)) / 2;
    const int32 ar = static_cast<int32>(az);
    const int32 as = -aq - ar;

    const int32 bq = static_cast<int32>(bx) - (static_cast<int32>(bz) - static_cast<int32>(bz & 1)) / 2;
    const int32 br = static_cast<int32>(bz);
    const int32 bs = -bq - br;

    const int32 dq = std::abs(aq - bq);
    const int32 dr = std::abs(ar - br);
    const int32 ds = std::abs(as - bs);

    return static_cast<float32>(std::max({dq, dr, ds}));
}

void WorldToHex(float32 worldX, float32 worldZ, float32 cellSize,
                float32 originX, float32 originZ,
                int32& outCol, int32& outRow)
{
    const float32 relX = worldX - originX;
    const float32 relZ = worldZ - originZ;

    const float32 vertSpacing = cellSize * kSqrt3Over2;

    // Approximate row from Z
    const int32 row = static_cast<int32>(std::floor(relZ / vertSpacing));

    // Offset X for odd rows
    float32 adjustedX = relX;
    if (row >= 0 && (row & 1) != 0)
    {
        adjustedX -= cellSize * 0.5f;
    }

    const int32 col = static_cast<int32>(std::floor(adjustedX / cellSize));

    outCol = col;
    outRow = row;
}

void HexToWorld(uint32 col, uint32 row, float32 cellSize,
                float32 originX, float32 originZ,
                float32& outX, float32& outZ)
{
    const float32 vertSpacing = cellSize * kSqrt3Over2;

    outX = originX + static_cast<float32>(col) * cellSize + cellSize * 0.5f;
    if ((row & 1) != 0)
    {
        outX += cellSize * 0.5f;
    }

    outZ = originZ + static_cast<float32>(row) * vertSpacing + vertSpacing * 0.5f;
}

} // namespace GameEngine::Pathfinding::HexGrid
