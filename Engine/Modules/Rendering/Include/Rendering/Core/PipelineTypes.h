// PipelineTypes.h — pipeline-descriptor types separated from format/sample state.
//
// Canonical pipeline authoring surface. The format-baked concrete pipeline is
// opaque (`PipelineHandle`); attachment formats and sample count travel
// alongside as a small `PipelineFormatKey` resolved at lookup time.
//
// Two desc types instead of one tagged union: `GraphicsPipelineDesc` covers
// graphics + mesh-shading pipelines (which both write to attachments and are
// keyed by `PipelineFormatKey`); `ComputePipelineDesc` has no attachments and
// no format key. Distinct id types prevent crossing them at the API boundary.
//
// Naming follows `CodingStyle.md`: public POD struct fields are PascalCase
// (no `m_` prefix); methods PascalCase; constants `kPrefix`.

#pragma once

#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/SpecializationConstants.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine::Rendering
{

// =====================================================================
// GraphicsPipelineDesc — graphics + mesh shading
// =====================================================================

// What kind of pipeline this is — picked by which shader-stage byte vectors
// the caller populates. Default is classic vertex-shader graphics.
enum class GraphicsPipelineKind : uint8_t
{
    VertexFragment = 0, // VertexShader + PixelShader (most common)
    MeshFragment   = 1, // MeshShader (+ optional AmplificationShader) + PixelShader
};

struct GraphicsPipelineDesc
{
    GraphicsPipelineKind Kind = GraphicsPipelineKind::VertexFragment;

    // Shader bytes — shared_ptr lets the desc share storage with the variant
    // cache without duplication. Null pointers are valid where a stage is
    // unused (depth-only pipelines have no PixelShader; vertex-fragment
    // pipelines have no MeshShader/AmplificationShader).
    std::shared_ptr<const std::vector<uint8_t>> VertexShader;
    std::shared_ptr<const std::vector<uint8_t>> PixelShader;
    std::shared_ptr<const std::vector<uint8_t>> MeshShader;
    std::shared_ptr<const std::vector<uint8_t>> AmplificationShader;

    // Set layouts are interned individually so the desc carries small ids
    // that participate in the content hash. The bindless set-1 patch is
    // applied at material build time so the patched layout is what's
    // interned (callers building the same material variant land on the
    // same id).
    std::vector<DescriptorSetLayoutId> DescriptorSetLayouts;

    RasterizationState Rasterization;
    DepthStencilState  DepthStencil;
    ColorBlendState    ColorBlend;

    // Owned by value because the interned desc lives on the device and
    // outlives any caller stack frame; a pointer would dangle.
    std::optional<DynamicStateInfo> DynamicState;

    PushConstantRange PushConstants;
    std::vector<NamedPushConstantRange> NamedPushConstantRanges;

    std::vector<VertexInputBinding>    VertexBindings;
    std::vector<VertexInputAttribute>  VertexAttributes;
    PrimitiveTopology                  Topology = PrimitiveTopology::TriangleList;

    std::string DebugName; // diagnostic only — never hashed, never compared

    // Owned by value (see DynamicState above).
    std::optional<SpecializationConstants> Specialization;

    // Deterministic content hash used by `PipelineCache::InternGraphicsPipeline`
    // for dedup. `DebugName` is NOT included.
    uint64_t ContentHash() const noexcept;
};

// =====================================================================
// ComputePipelineDesc
// =====================================================================

struct ComputePipelineDesc
{
    std::shared_ptr<const std::vector<uint8_t>> ComputeShader;

    std::vector<DescriptorSetLayoutId> DescriptorSetLayouts;

    PushConstantRange PushConstants;
    std::vector<NamedPushConstantRange> NamedPushConstantRanges;

    std::string DebugName;
    std::optional<SpecializationConstants> Specialization;

    uint64_t ContentHash() const noexcept;
};

// =====================================================================
// Free hash for DescriptorSetLayoutDesc (lives outside that struct because
// it's defined in Device.h; we hash members observed externally so we don't
// have to modify Device.h yet).
// =====================================================================

uint64_t HashDescriptorSetLayoutDesc(const DescriptorSetLayoutDesc& desc) noexcept;

} // namespace GameEngine::Rendering

// (Hash specializations for the small types live in PipelineIdentifiers.h.)
