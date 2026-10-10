#pragma once

#include "ECS/Entity.h"
#include "EZTree/EZTreeOptions.h"
#include "Types/Types.h"

#include <array>
#include <type_traits>

namespace GameEngine::Components
{

struct EZTree
{
    ::GameEngine::EZTree::TreeOptions Options{};
    uint32 SelectedBranchId = 0;
    uint32 SelectedBranchLevel = 0;
    uint32 RandomizeCounter = 0;
    uint64 RuntimeOptionsHash = 0;
    uint64 RuntimeMaterialHash = 0;
    uint64 RuntimeMeshHandleId = 0;
    uint64 RuntimeBranchMeshHandleId = 0;
    uint64 RuntimeLeafMeshHandleId = 0;
    uint64 RuntimeTrellisMeshHandleId = 0;
    uint32 RuntimeBranchInstanceIndex = 0xFFFFFFFFu;
    uint32 RuntimeLeafInstanceIndex = 0xFFFFFFFFu;
    uint32 RuntimeTrellisInstanceIndex = 0xFFFFFFFFu;
    std::array<uint8, 16> RuntimeMeshGuid{};
    uint32 RuntimeVertexCount = 0;
    uint32 RuntimeTriangleCount = 0;
    uint32 RuntimeBranchCount = 0;
};

static_assert(std::is_standard_layout_v<EZTree>, "EZTree must be standard layout for ECS");

} // namespace GameEngine::Components
