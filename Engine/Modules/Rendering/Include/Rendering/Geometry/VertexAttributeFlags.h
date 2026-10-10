#pragma once

#include <cstdint>

namespace GameEngine
{
namespace Rendering
{

// Bitmask describing which vertex attributes a mesh provides.
// Used by the shader variant system to select the correct adapter vertex shader,
// by the pipeline builder to configure vertex input state, and by the draw
// bucketing system to determine geometry draw compatibility.
//
// Flags are intentionally small (uint32_t) so they can be stored inline in
// GPU-facing structs (MeshGPUEntry) without padding concerns.
enum class VertexAttributeFlags : uint32_t
{
    None = 0,

    // Core attributes (binding 0 interleaved)
    HasPosition = 1u << 0, // vec3 position (always present)
    HasNormal   = 1u << 1, // vec3 normal
    HasUV0      = 1u << 2, // vec2 primary UV

    // Optional attributes (separate bindings)
    HasTangent  = 1u << 3, // vec4 tangent (xyz + w=handedness)
    HasUV1      = 1u << 4, // vec2 secondary UV
    HasColor    = 1u << 5, // vec4 vertex color

    // Skinning streams (separate bindings)
    HasJoints   = 1u << 6, // uvec4 joint indices (uint16x4)
    HasWeights  = 1u << 7, // vec4 bone weights
    HasJoints1  = 1u << 8, // optional second uvec4 joint indices
    HasWeights1 = 1u << 9, // optional second vec4 bone weights

    // Extra UV streams (separate bindings). UV0 is core, UV1 is the common
    // lightmap/detail stream above; these cover imported FBX UV2..UV7.
    HasUV2      = 1u << 10,
    HasUV3      = 1u << 11,
    HasUV4      = 1u << 12,
    HasUV5      = 1u << 13,
    HasUV6      = 1u << 14,
    HasUV7      = 1u << 15,

    // Convenience combos
    Skinned = HasJoints | HasWeights,
    Skinned8 = Skinned | HasJoints1 | HasWeights1,

    // Common import configurations
    StandardMesh = HasPosition | HasNormal | HasUV0,
    StandardMeshWithTangent = StandardMesh | HasTangent,
    SkinnedMesh = StandardMesh | Skinned,
    SkinnedMeshWithTangent = StandardMeshWithTangent | Skinned,
};

// Bitwise operators for VertexAttributeFlags
inline constexpr VertexAttributeFlags operator|(VertexAttributeFlags a, VertexAttributeFlags b)
{
    return static_cast<VertexAttributeFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline constexpr VertexAttributeFlags operator&(VertexAttributeFlags a, VertexAttributeFlags b)
{
    return static_cast<VertexAttributeFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline constexpr VertexAttributeFlags operator~(VertexAttributeFlags a)
{
    return static_cast<VertexAttributeFlags>(~static_cast<uint32_t>(a));
}

inline constexpr VertexAttributeFlags& operator|=(VertexAttributeFlags& a, VertexAttributeFlags b)
{
    a = a | b;
    return a;
}

inline constexpr VertexAttributeFlags& operator&=(VertexAttributeFlags& a, VertexAttributeFlags b)
{
    a = a & b;
    return a;
}

inline constexpr bool HasFlag(VertexAttributeFlags flags, VertexAttributeFlags test)
{
    return (flags & test) == test;
}

inline constexpr bool IsSkinned(VertexAttributeFlags flags)
{
    return HasFlag(flags, VertexAttributeFlags::Skinned);
}

// Compute the stride (in bytes) of the core interleaved vertex buffer
// (position + optional normal + optional UV0). This determines binding 0 stride.
inline constexpr uint32_t CoreVertexStride(VertexAttributeFlags flags)
{
    uint32_t stride = 0;
    if (HasFlag(flags, VertexAttributeFlags::HasPosition))
        stride += 3 * sizeof(float); // vec3
    if (HasFlag(flags, VertexAttributeFlags::HasNormal))
        stride += 3 * sizeof(float); // vec3
    if (HasFlag(flags, VertexAttributeFlags::HasUV0))
        stride += 2 * sizeof(float); // vec2
    return stride;
}

} // namespace Rendering
} // namespace GameEngine
