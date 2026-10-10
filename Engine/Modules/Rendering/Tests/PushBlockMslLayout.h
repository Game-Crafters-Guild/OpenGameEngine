#pragma once

// The push-constant size trap between SPIR-V and MSL. A SPIR-V block's size
// is where its last member ends; the MSL struct SPIRV-Cross emits for it has
// that size rounded up to the largest member alignment (int2/float2: 8,
// int3/int4/float4 and 3- or 4-row matrix columns: 16). The engine pushes the
// SPIR-V size, so a block that does not end on its largest alignment is
// smaller than the struct Metal binds and the API validation layer aborts the
// draw or dispatch. A block is safe on every backend when the two agree.

#include "Rendering/Materials/ShaderMeta.h"

#include <algorithm>
#include <cstdint>

namespace GameEngine::Rendering::PushBlockMslLayout
{

// Bytes in one component of `member`. BaseType names only 32-bit scalars, so
// an unnamed base with a known size is a 64- or 16-bit one.
inline uint32_t ComponentBytes(const Member& member)
{
    if (member.Type.Base != BaseType::Unknown || !member.Type.ArrayDims.empty() ||
        member.Type.Kind == TypeKind::Struct)
        return 4;
    const uint32_t components = member.Type.Kind == TypeKind::Vector   ? member.Type.VecSize
                                : member.Type.Kind == TypeKind::Matrix ? member.Type.Rows * member.Type.Cols
                                                                       : 1u;
    return components != 0 && member.Size != 0 ? member.Size / components : 4u;
}

inline uint32_t AlignmentOf(const TypeDesc& type, uint32_t componentBytes)
{
    switch (type.Kind)
    {
    case TypeKind::Scalar:
        return componentBytes;
    case TypeKind::Vector:
        return componentBytes * (type.VecSize == 2 ? 2u : 4u);
    case TypeKind::Matrix:
        return componentBytes * (type.Rows == 2 ? 2u : 4u); // one column vector
    case TypeKind::Struct:
    {
        uint32_t alignment = 4;
        for (const Member& member : type.StructMembers)
            alignment = std::max(alignment, AlignmentOf(member.Type, 4));
        return alignment;
    }
    }
    return 4;
}

// Where the block's last member ends: the size SPIR-V gives it and the
// number of bytes the engine pushes.
inline uint32_t SpirvEnd(const BlockLayout& block)
{
    uint32_t end = 0;
    for (const Member& member : block.Members)
        end = std::max(end, member.Offset + member.Size);
    return end;
}

inline uint32_t LargestAlignment(const BlockLayout& block)
{
    uint32_t alignment = 4;
    for (const Member& member : block.Members)
        alignment = std::max(alignment, AlignmentOf(member.Type, ComponentBytes(member)));
    return alignment;
}

// The size of the MSL struct SPIRV-Cross emits for `block`.
inline uint32_t MslSize(const BlockLayout& block)
{
    const uint32_t alignment = LargestAlignment(block);
    return (SpirvEnd(block) + alignment - 1) / alignment * alignment;
}

} // namespace GameEngine::Rendering::PushBlockMslLayout
