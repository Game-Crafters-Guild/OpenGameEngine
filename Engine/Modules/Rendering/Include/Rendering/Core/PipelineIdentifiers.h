// PipelineIdentifiers.h — small, self-contained pipeline-cache types.
//
// Split out from PipelineTypes.h so `Device.h` can declare the new IDevice
// surface (`InternGraphicsPipeline`, `GetOrCreateGraphicsPipeline`, …)
// without pulling in the full `GraphicsPipelineDesc` / `ComputePipelineDesc`
// definitions — those types reference state structs defined in `Device.h`,
// which would create a cyclic include.
//
// Forward-declares `enum class TextureFormat : uint32_t;` so `PipelineFormatKey`
// can store it by value without including Device.h. The same forward
// declaration appears in Device.h itself; both reach the full definition
// through Device.h-the-canonical-source.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <type_traits>

namespace GameEngine::Rendering
{

// Forward declaration. Full definition in Device.h.
enum class TextureFormat : uint32_t;

// =====================================================================
// Strongly-typed ids
// =====================================================================

struct GraphicsPipelineId
{
    uint32_t Value = 0;
    bool IsValid() const noexcept { return Value != 0; }
    auto operator<=>(const GraphicsPipelineId&) const = default;
};

struct ComputePipelineId
{
    uint32_t Value = 0;
    bool IsValid() const noexcept { return Value != 0; }
    auto operator<=>(const ComputePipelineId&) const = default;
};

// Interned id for a deduplicated descriptor-set-layout. Pipeline descs hold
// these instead of full layout state so hashing the desc walks small ids
// rather than every binding.
struct DescriptorSetLayoutId
{
    uint32_t Value = 0;
    bool IsValid() const noexcept { return Value != 0; }
    auto operator<=>(const DescriptorSetLayoutId&) const = default;
};

// =====================================================================
// Push constants
// =====================================================================

// Primary push-constant range. `Size == 0` and `StageMask == 0` together mean
// "use backend policy" (Vulkan defaults to 128 bytes / all stages).
struct PushConstantRange
{
    uint32_t Offset = 0;
    uint32_t Size = 0;
    uint32_t StageMask = 0;

    auto operator<=>(const PushConstantRange&) const = default;
};

// Additional named push-constant range (for shaders with multiple PC blocks).
// `Name` is diagnostic — does NOT participate in equality.
struct NamedPushConstantRange
{
    std::string Name;
    uint32_t Offset = 0;
    uint32_t Size = 0;
    uint32_t StageMask = 0;

    bool operator==(const NamedPushConstantRange& other) const noexcept
    {
        return Offset == other.Offset && Size == other.Size && StageMask == other.StageMask;
    }
    bool operator!=(const NamedPushConstantRange& other) const noexcept { return !(*this == other); }
};

// =====================================================================
// PipelineFormatKey
// =====================================================================

// Small, hashable, value-typed key carrying everything that varies per
// render-target binding: color/depth/stencil attachment formats and the
// rasterization sample count.
//
// 8 color attachments = Vulkan's max color attachment count; struct fits in
// a single 64-byte cache line so per-call hashing reads one line.
//
// Note: slots `[ColorCount, kMaxColors)` are undefined and must NOT be
// inspected by hashers, memcmp comparators, or backends; only `==` and
// `Hash()` (which walk only [0, ColorCount)) are authoritative.
struct PipelineFormatKey
{
    static constexpr size_t kMaxColors = 8;

    std::array<TextureFormat, kMaxColors> ColorFormats{};
    uint8_t ColorCount = 0;
    TextureFormat DepthFormat{};   // value-zero == undefined
    TextureFormat StencilFormat{}; // value-zero == undefined
    uint8_t RasterizationSamples = 1;

    uint64_t Hash() const noexcept;
    bool operator==(const PipelineFormatKey&) const noexcept;
    bool operator!=(const PipelineFormatKey& other) const noexcept { return !(*this == other); }
};
static_assert(sizeof(PipelineFormatKey) <= 64,
              "PipelineFormatKey must fit in one cache line for hot-path hashing");
static_assert(std::is_trivially_copyable_v<PipelineFormatKey>,
              "PipelineFormatKey must be trivially copyable");

// =====================================================================
// PipelineCacheStats
// =====================================================================

struct PipelineCacheStats
{
    // Concrete cache layer
    size_t   ConcreteSize = 0;
    size_t   ConcreteCapacity = 0; // 0 == unlimited
    uint64_t Hits = 0;
    uint64_t Misses = 0;
    uint64_t Inserts = 0;
    uint64_t Evictions = 0;
    uint64_t Clears = 0;

    // Intern tables
    uint32_t GraphicsPipelineCount = 0;
    uint32_t ComputePipelineCount = 0;
    uint32_t DescriptorSetLayoutCount = 0;
    uint32_t PinnedCount = 0;

    // L1 telemetry (populated when TLS L1 is enabled)
    uint64_t L1Hits = 0;
    uint64_t L1Misses = 0;
};

// What IDevice::Request{Graphics,Compute}Pipeline reports for one pipeline.
// Warm: a live concrete pipeline is cached. Pending: a build is queued or
// running. Failed: the last build produced nothing; it is not requested again
// until the pipeline cache is invalidated or the device is rebuilt.
enum class PipelineBuildState : uint8_t
{
    Warm,
    Pending,
    Failed,
};

} // namespace GameEngine::Rendering

// Hash specializations for use in std/abseil maps.
namespace std
{
template<>
struct hash<GameEngine::Rendering::PipelineFormatKey>
{
    size_t operator()(const GameEngine::Rendering::PipelineFormatKey& k) const noexcept
    {
        return static_cast<size_t>(k.Hash());
    }
};

template<>
struct hash<GameEngine::Rendering::GraphicsPipelineId>
{
    size_t operator()(GameEngine::Rendering::GraphicsPipelineId id) const noexcept
    {
        return std::hash<uint32_t>{}(id.Value);
    }
};

template<>
struct hash<GameEngine::Rendering::ComputePipelineId>
{
    size_t operator()(GameEngine::Rendering::ComputePipelineId id) const noexcept
    {
        return std::hash<uint32_t>{}(id.Value);
    }
};

template<>
struct hash<GameEngine::Rendering::DescriptorSetLayoutId>
{
    size_t operator()(GameEngine::Rendering::DescriptorSetLayoutId id) const noexcept
    {
        return std::hash<uint32_t>{}(id.Value);
    }
};
} // namespace std
