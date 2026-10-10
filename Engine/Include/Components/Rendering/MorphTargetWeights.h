#pragma once

#include "Types/Types.h"

#include <type_traits>

namespace GameEngine
{
namespace Components
{

// @ge-no-add  data helper, not user-addable in the editor
struct MorphTargetWeights
{
    static constexpr uint32 kMaxWeights = 64u;

    uint32 weightCount = 0;
    float weights[kMaxWeights] = {};
    float appliedWeights[kMaxWeights] = {};

    // Bump this when authoring/runtime code changes any weight.
    uint32 version = 1;
    uint32 appliedVersion = 0;

    uint32 sourceMeshId = 0;
    uint64 sourceMeshGpuHandleId = 0;
    uint64 runtimeMeshGpuHandleId = 0;

    uint8 sourceModelGuid[16] = {0};
    uint8 runtimeModelGuid[16] = {0};
};

static_assert(std::is_trivially_copyable_v<MorphTargetWeights>, "MorphTargetWeights must be trivially copyable for ECS");
static_assert(std::is_standard_layout_v<MorphTargetWeights>, "MorphTargetWeights must be standard layout for ECS");

} // namespace Components
} // namespace GameEngine
