#pragma once

#include "Rendering/Core/Device.h"
#include <cassert>
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

// Lightweight builder to batch UpdateDescriptorSet calls
class DescriptorWriter
{
  public:
    explicit DescriptorWriter(IDevice* device, DescriptorSetHandle set)
        : m_Device(device), m_Set(set) {}

    // Single-element helpers
    DescriptorWriter& AddUniformBuffer(uint32_t binding, BufferHandle buf, size_t offset, size_t size)
    {
        DescriptorSetUpdate u{};
        u.binding = binding;
        u.type = DescriptorType::UniformBuffer;
        u.buffers = {buf};
        u.bufferOffsets = {offset};
        u.bufferRanges = {size};
        m_Updates.push_back(std::move(u));
        return *this;
    }
    DescriptorWriter& AddStorageBuffer(uint32_t binding, BufferHandle buf, size_t offset, size_t size)
    {
        DescriptorSetUpdate u{};
        u.binding = binding;
        u.type = DescriptorType::StorageBuffer;
        u.buffers = {buf};
        u.bufferOffsets = {offset};
        u.bufferRanges = {size};
        m_Updates.push_back(std::move(u));
        return *this;
    }
    DescriptorWriter& AddTexture(uint32_t binding, TextureHandle tex, uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate u{};
        u.binding = binding;
        u.arrayElement = arrayElement;
        u.type = DescriptorType::Texture;
        u.textures = {tex};
        m_Updates.push_back(std::move(u));
        return *this;
    }
    DescriptorWriter& AddSampler(uint32_t binding, SamplerHandle samp, uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate u{};
        u.binding = binding;
        u.arrayElement = arrayElement;
        u.type = DescriptorType::Sampler;
        u.samplers = {samp};
        m_Updates.push_back(std::move(u));
        return *this;
    }
    DescriptorWriter& AddCombinedImageSampler(uint32_t binding, TextureHandle tex, SamplerHandle samp, uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate u{};
        u.binding = binding;
        u.arrayElement = arrayElement;
        u.type = DescriptorType::CombinedImageSampler;
        u.textures = {tex};
        u.samplers = {samp};
        m_Updates.push_back(std::move(u));
        return *this;
    }

    DescriptorWriter& AddAccelerationStructure(uint32_t binding, TlasSlotHandle tlas)
    {
        DescriptorSetUpdate u{};
        u.binding = binding;
        u.type = DescriptorType::AccelerationStructure;
        u.accelerationStructures = {tlas};
        m_Updates.push_back(std::move(u));
        return *this;
    }

    // Array helpers
    DescriptorWriter& AddUniformBuffers(uint32_t binding, const std::vector<BufferHandle>& bufs, const std::vector<size_t>& offsets, const std::vector<size_t>& sizes)
    {
        DescriptorSetUpdate u{};
        u.binding = binding;
        u.type = DescriptorType::UniformBuffer;
        u.buffers = bufs;
        u.bufferOffsets = offsets;
        u.bufferRanges = sizes;
        m_Updates.push_back(std::move(u));
        return *this;
    }
    DescriptorWriter& AddStorageBuffers(uint32_t binding, const std::vector<BufferHandle>& bufs, const std::vector<size_t>& offsets, const std::vector<size_t>& sizes)
    {
        DescriptorSetUpdate u{};
        u.binding = binding;
        u.type = DescriptorType::StorageBuffer;
        u.buffers = bufs;
        u.bufferOffsets = offsets;
        u.bufferRanges = sizes;
        m_Updates.push_back(std::move(u));
        return *this;
    }
    DescriptorWriter& AddTextures(uint32_t binding, const std::vector<TextureHandle>& texs, uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate u{};
        u.binding = binding;
        u.arrayElement = arrayElement;
        u.type = DescriptorType::Texture;
        u.textures = texs;
        m_Updates.push_back(std::move(u));
        return *this;
    }
    DescriptorWriter& AddSamplers(uint32_t binding, const std::vector<SamplerHandle>& samps, uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate u{};
        u.binding = binding;
        u.arrayElement = arrayElement;
        u.type = DescriptorType::Sampler;
        u.samplers = samps;
        m_Updates.push_back(std::move(u));
        return *this;
    }
    DescriptorWriter& AddCombinedImageSamplers(uint32_t binding, const std::vector<TextureHandle>& texs, const std::vector<SamplerHandle>& samps, uint32_t arrayElement = 0)
    {
        DescriptorSetUpdate u{};
        u.binding = binding;
        u.arrayElement = arrayElement;
        u.type = DescriptorType::CombinedImageSampler;
        u.textures = texs;
        u.samplers = samps;
        m_Updates.push_back(std::move(u));
        return *this;
    }

    bool HasPendingUpdates() const { return !m_Updates.empty(); }

    void Flush()
    {
        if (!m_Device || !m_Set.IsValid())
            return;
        if (m_Updates.empty())
            return; // no-op guard
        for (const auto& u : m_Updates)
        {
            Validate(u);
            m_Device->UpdateDescriptorSet(m_Set, u);
        }
        m_Updates.clear();
    }

    // Merge updates by (binding,type,arrayElement) to minimize driver calls
    void FlushCoalesced()
    {
        if (!m_Device || !m_Set.IsValid())
            return;
        struct Key
        {
            uint32_t b;
            uint32_t t;
            uint32_t a;
            bool operator==(const Key& o) const { return b == o.b && t == o.t && a == o.a; }
        };
        struct KeyHash
        {
            size_t operator()(const Key& k) const { return (size_t)k.b ^ ((size_t)k.t << 8) ^ ((size_t)k.a << 16); }
        };
        std::unordered_map<Key, DescriptorSetUpdate, KeyHash> merged;
        for (const auto& u : m_Updates)
        {
            Validate(u);
            Key k{u.binding, (uint32_t)u.type, u.arrayElement};
            auto it = merged.find(k);
            if (it == merged.end())
            {
                merged.emplace(k, u);
            }
            else
            {
                Append(it->second, u);
            }
        }
        for (const auto& kv : merged)
        {
            m_Device->UpdateDescriptorSet(m_Set, kv.second);
        }
        m_Updates.clear();
    }

  private:
    static void Validate(const DescriptorSetUpdate& u)
    {
        if (!u.buffers.empty())
        {
            assert(u.bufferOffsets.size() == u.buffers.size() && "Offsets must match buffers length");
            assert(u.bufferRanges.size() == u.buffers.size() && "Ranges must match buffers length");
        }
        if (!u.textures.empty())
        {
            if (!u.samplers.empty())
            {
                assert(u.samplers.size() == u.textures.size() && "Samplers must match textures when using CombinedImageSampler");
            }
        }
    }
    static void Append(DescriptorSetUpdate& dst, const DescriptorSetUpdate& src)
    {
        dst.buffers.insert(dst.buffers.end(), src.buffers.begin(), src.buffers.end());
        dst.bufferOffsets.insert(dst.bufferOffsets.end(), src.bufferOffsets.begin(), src.bufferOffsets.end());
        dst.bufferRanges.insert(dst.bufferRanges.end(), src.bufferRanges.begin(), src.bufferRanges.end());
        dst.textures.insert(dst.textures.end(), src.textures.begin(), src.textures.end());
        dst.samplers.insert(dst.samplers.end(), src.samplers.begin(), src.samplers.end());
        dst.accelerationStructures.insert(dst.accelerationStructures.end(),
                                          src.accelerationStructures.begin(),
                                          src.accelerationStructures.end());
    }

    IDevice* m_Device = nullptr;
    DescriptorSetHandle m_Set = INVALID_HANDLE;
    std::vector<DescriptorSetUpdate> m_Updates{};
};

} // namespace Rendering
} // namespace GameEngine
