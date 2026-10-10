#pragma once

#include "Rendering/Core/Device.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Shared string->enum tables for the render-pipeline blueprint schema. The
// runtime materializers (RenderPipeline.cpp) and FullscreenShaderNode read
// through these; the validating compiler front-stops invalid strings against
// the same tables so the runtime lenient defaults become unreachable through
// the compile path. One authority per enum keeps the parse and the validation
// list from drifting.
namespace GameEngine::Engine::Renderer::Pipeline
{
// Blend mode for fullscreen post passes. Lives here (not private to
// FullscreenShaderNode) so the compiler and the node validate/parse the same
// "blendMode" string set.
enum class BlendMode : uint8_t
{
    Disabled = 0,
    Alpha,
    Additive
};

// One string->enum row. Names are stored lowercase; Lookup lowercases input.
template <typename T>
struct EnumTableEntry
{
    std::string_view Name;
    T Value;
};

// Read-only string->enum table with a fail-loud Lookup and a valid-names list
// for compile-time validation + nearest-name suggestions. Backed by a static
// entry array owned by the accessor in PipelineEnumTables.cpp.
template <typename T>
class EnumStringTable
{
  public:
    constexpr explicit EnumStringTable(std::span<const EnumTableEntry<T>> entries)
        : m_Entries(entries)
    {
    }

    std::optional<T> Lookup(std::string_view name) const
    {
        for (const auto& e : m_Entries)
            if (CaseInsensitiveEquals(e.Name, name))
                return e.Value;
        return std::nullopt;
    }

    std::span<const EnumTableEntry<T>> Entries() const { return m_Entries; }

    std::vector<std::string_view> ValidNames() const
    {
        std::vector<std::string_view> out;
        out.reserve(m_Entries.size());
        for (const auto& e : m_Entries)
            out.push_back(e.Name);
        return out;
    }

  private:
    static bool CaseInsensitiveEquals(std::string_view lowerKey, std::string_view input)
    {
        if (lowerKey.size() != input.size())
            return false;
        for (size_t i = 0; i < lowerKey.size(); ++i)
        {
            char c = input[i];
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
            if (lowerKey[i] != c)
                return false;
        }
        return true;
    }

    std::span<const EnumTableEntry<T>> m_Entries;
};

const EnumStringTable<::GameEngine::Rendering::TextureFormat>& TextureFormatTable();
const EnumStringTable<::GameEngine::Rendering::TextureUsage>& TextureUsageTable();
const EnumStringTable<::GameEngine::Rendering::BufferUsage>& BufferUsageTable();
const EnumStringTable<::GameEngine::Rendering::BufferMemoryUsage>& BufferMemoryUsageTable();
const EnumStringTable<BlendMode>& BlendModeTable();

// Runtime lenient-default diagnostic: the validating compiler front-stops
// invalid enum strings, so this only fires for in-code/direct blueprints that
// bypass Compile. Warn-once per (category,value) keeps the per-frame
// materializers quiet.
void WarnUnknownEnumOnce(std::string_view category, std::string_view value,
                         std::span<const std::string_view> validNames);

} // namespace GameEngine::Engine::Renderer::Pipeline
