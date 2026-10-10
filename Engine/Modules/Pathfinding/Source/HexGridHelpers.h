#pragma once

#include "Types/Types.h"

namespace GameEngine::Pathfinding::HexGrid
{

// 6 neighbor offsets for odd-r offset hex grid
void GetNeighborOffsets(uint32 row, int32* outDx, int32* outDz, uint32& outCount);

// Hex distance between two offset coordinates (convert to cube coords internally)
float32 Distance(uint32 ax, uint32 az, uint32 bx, uint32 bz);

// World position to offset hex coordinate
void WorldToHex(float32 worldX, float32 worldZ, float32 cellSize,
                float32 originX, float32 originZ,
                int32& outCol, int32& outRow);

// Offset hex coordinate to world center
void HexToWorld(uint32 col, uint32 row, float32 cellSize,
                float32 originX, float32 originZ,
                float32& outX, float32& outZ);

} // namespace GameEngine::Pathfinding::HexGrid
