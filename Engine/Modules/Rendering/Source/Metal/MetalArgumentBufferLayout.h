#pragma once

// Deterministic descriptor-set -> Metal argument buffer slot assignment.
//
// Both the SPIRV-Cross translation (explicit [[id(n)]] bindings) and
// MetalDevice::CreateDescriptorSet/UpdateDescriptorSet derive slot ids from
// the same DescriptorSetLayoutDesc through this function, so the CPU writes
// and the shader reads agree without consulting each other.
//
// Layout rule: walk bindings ordered by binding index; each resource takes
// `count` consecutive 8-byte slots; combined image samplers take a texture
// run followed by a sampler run. This matches SPIRV-Cross MSL with
// pad_argument_buffer_resources, where the argument buffer is read through a
// generated struct — member POSITION (declaration order) is the GPU-visible
// offset, and padding members keep position == [[id(n)]] for stage-unused
// bindings. Descriptor sets must therefore be created from the same layout
// desc the pipeline was translated with (the engine's MaterialBinder keys its
// set caches by interned layout id, which guarantees this); the bind-time
// validator in MetalCommandList (GE_METAL_VALIDATE_BINDINGS=1) flags any
// divergence.
//
// SPIRV-Cross's buffer-size-constants member (runtime-array .length()) lives
// at SizeConstantsSlot, after the resource slots.

#include "Rendering/Core/Device.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace GameEngine::Rendering
{

struct MetalArgumentSlot
{
    uint32_t Binding = 0;
    DescriptorType Type = DescriptorType::UniformBuffer;
    uint32_t Count = 1;
    static constexpr uint32_t kUnused = ~0u;
    uint32_t BufferId = kUnused;  // first slot id for buffer entries
    uint32_t TextureId = kUnused; // first slot id for texture entries
    uint32_t SamplerId = kUnused; // first slot id for sampler entries
    // First slot id for acceleration-structure entries. Kept apart from
    // BufferId even though SPIRV-Cross resolves both through MSLResourceBinding
    // ::msl_buffer: the slot holds an MTLResourceID, not a gpuAddress, so the
    // update path must never treat it as a buffer.
    uint32_t AccelerationStructureId = kUnused;
};

struct MetalArgumentBufferLayout
{
    std::vector<MetalArgumentSlot> Slots; // sorted by Binding
    uint32_t TotalSlotCount = 0;          // resource slots (excludes size constants)
    uint32_t SizeConstantsSlot = 0;       // id of the buffer-size-constants entry

    const MetalArgumentSlot* FindBinding(uint32_t binding) const
    {
        for (const MetalArgumentSlot& slot : Slots)
        {
            if (slot.Binding == binding)
            {
                return &slot;
            }
        }
        return nullptr;
    }
};

inline MetalArgumentBufferLayout ComputeMetalArgumentBufferLayout(const DescriptorSetLayoutDesc& desc)
{
    MetalArgumentBufferLayout layout;
    layout.Slots.reserve(desc.bindings.size());
    for (const DescriptorBinding& binding : desc.bindings)
    {
        MetalArgumentSlot slot{};
        slot.Binding = binding.binding;
        slot.Type = binding.type;
        slot.Count = std::max(1u, binding.count);
        layout.Slots.push_back(slot);
    }
    std::sort(layout.Slots.begin(), layout.Slots.end(),
              [](const MetalArgumentSlot& a, const MetalArgumentSlot& b) { return a.Binding < b.Binding; });

    uint32_t next = 0;
    for (MetalArgumentSlot& slot : layout.Slots)
    {
        switch (slot.Type)
        {
        case DescriptorType::UniformBuffer:
        case DescriptorType::StorageBuffer:
            slot.BufferId = next;
            next += slot.Count;
            break;
        case DescriptorType::Texture:
        case DescriptorType::StorageImage:
            slot.TextureId = next;
            next += slot.Count;
            break;
        case DescriptorType::Sampler:
            slot.SamplerId = next;
            next += slot.Count;
            break;
        case DescriptorType::CombinedImageSampler:
            slot.TextureId = next;
            next += slot.Count;
            slot.SamplerId = next;
            next += slot.Count;
            break;
        case DescriptorType::AccelerationStructure:
            slot.AccelerationStructureId = next;
            next += slot.Count;
            break;
        }
    }
    layout.TotalSlotCount = next;
    layout.SizeConstantsSlot = next;
    return layout;
}

// Reserved Metal buffer-table indices shared across the backend.
// [0 .. N) argument buffers (descriptor set index == buffer index),
// 29 downwards: vertex buffers (engine slot s -> 29 - s),
// 30: push constants.
inline constexpr uint32_t kMetalPushConstantBufferIndex = 30;
inline constexpr uint32_t kMetalVertexBufferBaseIndex = 29;

inline uint32_t MetalVertexBufferIndex(uint32_t engineSlot)
{
    return kMetalVertexBufferBaseIndex - engineSlot;
}

} // namespace GameEngine::Rendering
