#pragma once

// Builds PipelineDesc vertex input state from VertexAttributeFlags.
//
// This replaces manual vertex layout configuration with a single call that
// configures bindings and attributes based on what the mesh actually provides.
// The resulting layout matches the adapter vertex shader #ifdef conventions.
//
// Binding layout convention:
//   Binding 0: Core interleaved (position + normal + UV0)
//     location 0: vec3 position   (offset 0)
//     location 1: vec3 normal     (offset 12, if HasNormal)
//     location 2: vec2 uv0        (offset 24, if HasUV0; 12 if no normal)
//   Binding 1: vec4 tangent       (if HasTangent)
//   Binding 2: vec4 vertex color  (if HasColor)
//   Binding 3: vec2 uv1           (if HasUV1)
//   Binding 4: uvec4 joints       (if HasJoints, R16G16B16A16_UINT)
//   Binding 5: vec4 weights       (if HasWeights, R32G32B32A32_FLOAT)
//   Binding 6: uvec4 joints1      (if HasJoints1, R16G16B16A16_UINT)
//   Binding 7: vec4 weights1      (if HasWeights1, R32G32B32A32_FLOAT)
//   Binding 8..13: vec2 uv2..uv7  (if HasUV2..HasUV7)

#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"

#include <cstdint>

namespace GameEngine
{
namespace Rendering
{

// Shader input locations matching adapter_vertex.glsl conventions.
// These are stable across all variants; unused locations are simply not bound.
struct VertexLocation
{
    static constexpr uint32_t Position = 0;
    static constexpr uint32_t Normal   = 1;
    static constexpr uint32_t UV0      = 2;
    static constexpr uint32_t Tangent  = 3;
    static constexpr uint32_t Color    = 4;
    static constexpr uint32_t UV1      = 5;
    static constexpr uint32_t Joints   = 6;
    static constexpr uint32_t Weights  = 7;
    static constexpr uint32_t Joints1  = 8;
    static constexpr uint32_t Weights1 = 9;
    static constexpr uint32_t UV2      = 10;
    static constexpr uint32_t UV3      = 11;
    static constexpr uint32_t UV4      = 12;
    static constexpr uint32_t UV5      = 13;
    static constexpr uint32_t UV6      = 14;
    static constexpr uint32_t UV7      = 15;
};

// Vertex buffer binding indices matching the convention above.
struct VertexBinding
{
    static constexpr uint32_t Core     = 0; // pos + normal + uv0 interleaved
    static constexpr uint32_t Tangent  = 1;
    static constexpr uint32_t Color    = 2;
    static constexpr uint32_t UV1      = 3;
    static constexpr uint32_t Joints   = 4;
    static constexpr uint32_t Weights  = 5;
    static constexpr uint32_t Joints1  = 6;
    static constexpr uint32_t Weights1 = 7;
    static constexpr uint32_t UV2      = 8;
    static constexpr uint32_t UV3      = 9;
    static constexpr uint32_t UV4      = 10;
    static constexpr uint32_t UV5      = 11;
    static constexpr uint32_t UV6      = 12;
    static constexpr uint32_t UV7      = 13;
};

// Configure a `GraphicsPipelineDesc`'s vertex input state from
// `VertexAttributeFlags`. Clears existing bindings/attributes first and
// returns the number of bindings written.
inline uint32_t BuildVertexLayoutFromFlags(VertexAttributeFlags flags, GraphicsPipelineDesc& pd)
{
    pd.VertexBindings.clear();
    pd.VertexAttributes.clear();

    // Fully-procedural geometry (customVertexShader materials): no vertex buffer
    // exists, so declare zero vertex input. Without this short-circuit the Core
    // binding below is emitted with a zero stride and no attributes — a phantom
    // binding the pipeline would still expect a bound vertex buffer for.
    if (flags == VertexAttributeFlags::None)
        return 0;

    uint32_t bindingCount = 0;

    // --- Binding 0: Core interleaved (position + normal + uv0) ---
    {
        const uint32_t stride = CoreVertexStride(flags);
        pd.VertexBindings.push_back({VertexBinding::Core, stride, /*inputRate=*/0});
        ++bindingCount;

        uint32_t offset = 0;

        if (HasFlag(flags, VertexAttributeFlags::HasPosition))
        {
            pd.VertexAttributes.push_back(
                {VertexLocation::Position, VertexBinding::Core, Format::R32G32B32_FLOAT, offset});
            offset += 3 * sizeof(float);
        }

        if (HasFlag(flags, VertexAttributeFlags::HasNormal))
        {
            pd.VertexAttributes.push_back(
                {VertexLocation::Normal, VertexBinding::Core, Format::R32G32B32_FLOAT, offset});
            offset += 3 * sizeof(float);
        }

        if (HasFlag(flags, VertexAttributeFlags::HasUV0))
        {
            pd.VertexAttributes.push_back(
                {VertexLocation::UV0, VertexBinding::Core, Format::R32G32_FLOAT, offset});
            offset += 2 * sizeof(float);
        }
    }

    if (HasFlag(flags, VertexAttributeFlags::HasTangent))
    {
        constexpr uint32_t tangentStride = 4 * sizeof(float);
        pd.VertexBindings.push_back({VertexBinding::Tangent, tangentStride, /*inputRate=*/0});
        pd.VertexAttributes.push_back(
            {VertexLocation::Tangent, VertexBinding::Tangent, Format::R32G32B32A32_FLOAT, 0});
        ++bindingCount;
    }

    if (HasFlag(flags, VertexAttributeFlags::HasColor))
    {
        constexpr uint32_t colorStride = 4 * sizeof(float);
        pd.VertexBindings.push_back({VertexBinding::Color, colorStride, /*inputRate=*/0});
        pd.VertexAttributes.push_back(
            {VertexLocation::Color, VertexBinding::Color, Format::R32G32B32A32_FLOAT, 0});
        ++bindingCount;
    }

    if (HasFlag(flags, VertexAttributeFlags::HasUV1))
    {
        constexpr uint32_t uv1Stride = 2 * sizeof(float);
        pd.VertexBindings.push_back({VertexBinding::UV1, uv1Stride, /*inputRate=*/0});
        pd.VertexAttributes.push_back(
            {VertexLocation::UV1, VertexBinding::UV1, Format::R32G32_FLOAT, 0});
        ++bindingCount;
    }

    if (HasFlag(flags, VertexAttributeFlags::HasJoints))
    {
        constexpr uint32_t jointsStride = 4 * sizeof(uint16_t);
        pd.VertexBindings.push_back({VertexBinding::Joints, jointsStride, /*inputRate=*/0});
        pd.VertexAttributes.push_back(
            {VertexLocation::Joints, VertexBinding::Joints, Format::R16G16B16A16_UINT, 0});
        ++bindingCount;
    }

    if (HasFlag(flags, VertexAttributeFlags::HasWeights))
    {
        constexpr uint32_t weightsStride = 4 * sizeof(float);
        pd.VertexBindings.push_back({VertexBinding::Weights, weightsStride, /*inputRate=*/0});
        pd.VertexAttributes.push_back(
            {VertexLocation::Weights, VertexBinding::Weights, Format::R32G32B32A32_FLOAT, 0});
        ++bindingCount;
    }

    if (HasFlag(flags, VertexAttributeFlags::HasJoints1))
    {
        constexpr uint32_t jointsStride = 4 * sizeof(uint16_t);
        pd.VertexBindings.push_back({VertexBinding::Joints1, jointsStride, /*inputRate=*/0});
        pd.VertexAttributes.push_back(
            {VertexLocation::Joints1, VertexBinding::Joints1, Format::R16G16B16A16_UINT, 0});
        ++bindingCount;
    }

    if (HasFlag(flags, VertexAttributeFlags::HasWeights1))
    {
        constexpr uint32_t weightsStride = 4 * sizeof(float);
        pd.VertexBindings.push_back({VertexBinding::Weights1, weightsStride, /*inputRate=*/0});
        pd.VertexAttributes.push_back(
            {VertexLocation::Weights1, VertexBinding::Weights1, Format::R32G32B32A32_FLOAT, 0});
        ++bindingCount;
    }

    auto addUv = [&](VertexAttributeFlags flag, uint32_t location, uint32_t binding)
    {
        if (!HasFlag(flags, flag))
            return;
        constexpr uint32_t uvStride = 2 * sizeof(float);
        pd.VertexBindings.push_back({binding, uvStride, /*inputRate=*/0});
        pd.VertexAttributes.push_back({location, binding, Format::R32G32_FLOAT, 0});
        ++bindingCount;
    };
    addUv(VertexAttributeFlags::HasUV2, VertexLocation::UV2, VertexBinding::UV2);
    addUv(VertexAttributeFlags::HasUV3, VertexLocation::UV3, VertexBinding::UV3);
    addUv(VertexAttributeFlags::HasUV4, VertexLocation::UV4, VertexBinding::UV4);
    addUv(VertexAttributeFlags::HasUV5, VertexLocation::UV5, VertexBinding::UV5);
    addUv(VertexAttributeFlags::HasUV6, VertexLocation::UV6, VertexBinding::UV6);
    addUv(VertexAttributeFlags::HasUV7, VertexLocation::UV7, VertexBinding::UV7);

    return bindingCount;
}

} // namespace Rendering
} // namespace GameEngine
