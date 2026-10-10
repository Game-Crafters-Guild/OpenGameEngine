#pragma once

// Native C++ port of @dgreenheck/ez-tree procedural generation.
// Portions adapted from EZ-Tree, MIT License.
// Copyright (c) 2024 Daniel Greenheck.

#include "EZTree/EZTreeOptions.h"
#include "Assets/ModelAsset.h"
#include "Mathematics/Geometry.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::EZTree
{

struct TreeMeshStats
{
    uint32 branchVertices = 0;
    uint32 branchIndices = 0;
    uint32 leafVertices = 0;
    uint32 leafIndices = 0;
    uint32 trellisVertices = 0;
    uint32 trellisIndices = 0;
    uint32 generatedBranchCount = 0;
};

struct GeneratedTree
{
    Mesh branches;
    Mesh leaves;
    Mesh trellis;
    Mesh combined;
    Mathematics::BoundingBox bounds{};
    TreeMeshStats stats{};
};

struct TrellisForceResult
{
    bool active = false;
    Mathematics::Vector3 direction{0.0f, 1.0f, 0.0f};
    float strength = 0.0f;
};

Mathematics::Vector3 GetNearestTrellisPoint(const TreeOptions& options, const Mathematics::Vector3& position);
TrellisForceResult CalculateTrellisForce(const TreeOptions& options, const Mathematics::Vector3& position, float radius);

class Generator
{
public:
    GeneratedTree Generate(const TreeOptions& options);
};

} // namespace GameEngine::EZTree
