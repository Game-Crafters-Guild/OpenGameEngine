#pragma once

#include <cstddef>
#include <cstdint>

// C++ mirror of the GPU material parameter block. The field list lives in
// Shaders/Includes/material_params.glsl and is shared verbatim with the GLSL
// `MaterialData` (the shared MaterialParams SSBO row) so the layout is defined
// exactly once. `sizeof(MaterialGpuParams)` is the authoritative param-block
// byte size — the SSBO stride math derives from it.

namespace GameEngine
{

// 16-byte GPU vec4 (matches std140/std430 vec4 alignment). Layout-only mirror type:
// the param block is vec4-granular, so every field maps to this regardless of the
// glsl token in the field list.
struct alignas(16) GpuVec4
{
    float x, y, z, w;
};
static_assert(sizeof(GpuVec4) == 16, "GpuVec4 must be a tight 16-byte vec4");

struct MaterialGpuParams
{
#define GE_FIELD(glslType, name) GpuVec4 name;
#include "../../../Shaders/Includes/material_params.glsl"
#undef GE_FIELD
};

// vec4 lanes in the block — the budget a program's declared properties pack into.
inline constexpr uint32_t kMaterialParamLaneCount = GE_MATERIAL_PARAM_LANE_COUNT;
#undef GE_MATERIAL_PARAM_LANE_COUNT
inline constexpr uint32_t kMaterialParamLaneBytes = static_cast<uint32_t>(sizeof(GpuVec4));

// The block's size is a GPU ABI shared with every composed shader (it sets the
// 784-byte MaterialData row stride), so it is pinned directly.
static_assert(sizeof(MaterialGpuParams) % 16 == 0,
              "Material param block must be 16-byte-granular (all-vec4 fields)");
static_assert(sizeof(MaterialGpuParams) == kMaterialParamLaneCount * kMaterialParamLaneBytes,
              "Material param block is exactly its lane array");
static_assert(sizeof(MaterialGpuParams) == 480, "Material param block size changed — the SSBO row stride is an ABI");

inline constexpr uint32_t kMaterialParamBlockBytes =
    static_cast<uint32_t>(sizeof(MaterialGpuParams));

// Byte offset of lane `lane` within the block.
constexpr uint32_t MaterialParamLaneOffset(uint32_t lane)
{
    return lane * kMaterialParamLaneBytes;
}

} // namespace GameEngine
