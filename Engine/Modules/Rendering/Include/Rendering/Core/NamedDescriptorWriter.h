#pragma once

#include "Rendering/Core/DescriptorWriter.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/ShaderMeta.h"
#include <string>
#include <unordered_map>

namespace GameEngine
{
namespace Rendering
{

// Name-based descriptor writer built on top of DescriptorWriter.
// Resolves binding indices from ShaderMeta for a given set index.
class NamedDescriptorWriter
{
  public:
    NamedDescriptorWriter(IDevice* device,
                          DescriptorSetHandle set,
                          const ShaderMeta& meta,
                          uint32_t setIndex)
        : m_Writer(device, set)
    {
        // Build name -> binding map for the requested set
        for (const auto& s : meta.Sets)
        {
            if (s.Set == setIndex)
            {
                for (const auto& b : s.Bindings)
                {
                    if (!b.Name.empty())
                        m_NameToBinding[b.Name] = b.Binding;
                }
            }
        }
    }

    bool Has(const std::string& name) const { return m_NameToBinding.find(name) != m_NameToBinding.end(); }

    // Optional guard: allow caller to check if any writes are pending before Flush
    bool HasPendingUpdates() const { return m_Writer.HasPendingUpdates(); }

    // Single element helpers
    NamedDescriptorWriter& AddUniformBuffer(const std::string& name, BufferHandle buf, size_t offset, size_t size)
    {
        auto it = m_NameToBinding.find(name);
        if (it == m_NameToBinding.end())
            return *this;
        m_Writer.AddUniformBuffer(it->second, buf, offset, size);
        return *this;
    }
    NamedDescriptorWriter& AddStorageBuffer(const std::string& name, BufferHandle buf, size_t offset, size_t size)
    {
        auto it = m_NameToBinding.find(name);
        if (it == m_NameToBinding.end())
            return *this;
        m_Writer.AddStorageBuffer(it->second, buf, offset, size);
        return *this;
    }
    NamedDescriptorWriter& AddTexture(const std::string& name, TextureHandle tex, uint32_t arrayElement = 0)
    {
        auto it = m_NameToBinding.find(name);
        if (it == m_NameToBinding.end())
            return *this;
        m_Writer.AddTexture(it->second, tex, arrayElement);
        return *this;
    }
    NamedDescriptorWriter& AddSampler(const std::string& name, SamplerHandle samp, uint32_t arrayElement = 0)
    {
        auto it = m_NameToBinding.find(name);
        if (it == m_NameToBinding.end())
            return *this;
        m_Writer.AddSampler(it->second, samp, arrayElement);
        return *this;
    }
    NamedDescriptorWriter& AddCombinedImageSampler(const std::string& name, TextureHandle tex, SamplerHandle samp, uint32_t arrayElement = 0)
    {
        auto it = m_NameToBinding.find(name);
        if (it == m_NameToBinding.end())
            return *this;
        m_Writer.AddCombinedImageSampler(it->second, tex, samp, arrayElement);
        return *this;
    }

    // Array helpers
    NamedDescriptorWriter& AddUniformBuffers(const std::string& name,
                                             const std::vector<BufferHandle>& bufs,
                                             const std::vector<size_t>& offs,
                                             const std::vector<size_t>& sizes)
    {
        auto it = m_NameToBinding.find(name);
        if (it == m_NameToBinding.end())
            return *this;
        m_Writer.AddUniformBuffers(it->second, bufs, offs, sizes);
        return *this;
    }
    NamedDescriptorWriter& AddStorageBuffers(const std::string& name,
                                             const std::vector<BufferHandle>& bufs,
                                             const std::vector<size_t>& offs,
                                             const std::vector<size_t>& sizes)
    {
        auto it = m_NameToBinding.find(name);
        if (it == m_NameToBinding.end())
            return *this;
        m_Writer.AddStorageBuffers(it->second, bufs, offs, sizes);
        return *this;
    }
    NamedDescriptorWriter& AddTextures(const std::string& name, const std::vector<TextureHandle>& texs, uint32_t arrayElement = 0)
    {
        auto it = m_NameToBinding.find(name);
        if (it == m_NameToBinding.end())
            return *this;
        m_Writer.AddTextures(it->second, texs, arrayElement);
        return *this;
    }

    // TryAdd variants that return false when the binding name is missing
    bool TryAddUniformBuffer(const std::string& name, BufferHandle buf, size_t offset, size_t size)
    {
        auto it = m_NameToBinding.find(name);
        if (it == m_NameToBinding.end())
            return false;
        m_Writer.AddUniformBuffer(it->second, buf, offset, size);
        return true;
    }
    bool TryAddStorageBuffer(const std::string& name, BufferHandle buf, size_t offset, size_t size)
    {
        auto it = m_NameToBinding.find(name);
        if (it == m_NameToBinding.end())
            return false;
        m_Writer.AddStorageBuffer(it->second, buf, offset, size);
        return true;
    }
    bool TryAddTexture(const std::string& name, TextureHandle tex, uint32_t arrayElement = 0)
    {
        auto it = m_NameToBinding.find(name);
        if (it == m_NameToBinding.end())
            return false;
        m_Writer.AddTexture(it->second, tex, arrayElement);
        return true;
    }
    // No silent Add* twin on purpose: an unbound acceleration structure is not
    // a dimmer frame, it is a ray query reading an unwritten descriptor slot.
    // Callers must handle the miss.
    bool TryAddAccelerationStructure(const std::string& name, TlasSlotHandle tlas)
    {
        auto it = m_NameToBinding.find(name);
        if (it == m_NameToBinding.end())
            return false;
        m_Writer.AddAccelerationStructure(it->second, tlas);
        return true;
    }
    bool TryAddSampler(const std::string& name, SamplerHandle samp, uint32_t arrayElement = 0)
    {
        auto it = m_NameToBinding.find(name);
        if (it == m_NameToBinding.end())
            return false;
        m_Writer.AddSampler(it->second, samp, arrayElement);
        return true;
    }
    bool TryAddCombinedImageSampler(const std::string& name, TextureHandle tex, SamplerHandle samp, uint32_t arrayElement = 0)
    {
        auto it = m_NameToBinding.find(name);
        if (it == m_NameToBinding.end())
            return false;
        m_Writer.AddCombinedImageSampler(it->second, tex, samp, arrayElement);
        return true;
    }
    bool TryAddTextures(const std::string& name, const std::vector<TextureHandle>& texs, uint32_t arrayElement = 0)
    {
        auto it = m_NameToBinding.find(name);
        if (it == m_NameToBinding.end())
            return false;
        m_Writer.AddTextures(it->second, texs, arrayElement);
        return true;
    }
    bool TryAddSamplers(const std::string& name, const std::vector<SamplerHandle>& samps, uint32_t arrayElement = 0)
    {
        auto it = m_NameToBinding.find(name);
        if (it == m_NameToBinding.end())
            return false;
        m_Writer.AddSamplers(it->second, samps, arrayElement);
        return true;
    }
    bool TryAddUniformBuffers(const std::string& name, const std::vector<BufferHandle>& bufs, const std::vector<size_t>& offs, const std::vector<size_t>& sizes)
    {
        auto it = m_NameToBinding.find(name);
        if (it == m_NameToBinding.end())
            return false;
        m_Writer.AddUniformBuffers(it->second, bufs, offs, sizes);
        return true;
    }
    bool TryAddStorageBuffers(const std::string& name, const std::vector<BufferHandle>& bufs, const std::vector<size_t>& offs, const std::vector<size_t>& sizes)
    {
        auto it = m_NameToBinding.find(name);
        if (it == m_NameToBinding.end())
            return false;
        m_Writer.AddStorageBuffers(it->second, bufs, offs, sizes);
        return true;
    }

    void Flush() { m_Writer.Flush(); }

  private:
    DescriptorWriter m_Writer;
    std::unordered_map<std::string, uint32_t> m_NameToBinding;
};

} // namespace Rendering
} // namespace GameEngine
