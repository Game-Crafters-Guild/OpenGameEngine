#include "Rendering/Core/PipelineTypes.h"

#include "Rendering/Core/HashUtils.h"

#include <cstring>

namespace GameEngine::Rendering
{

// FNV-1a 64-bit offset basis as the hash seed across this file.
static constexpr uint64_t kHashSeed = 0xcbf29ce484222325ULL;

uint64_t PipelineFormatKey::Hash() const noexcept
{
    using HashUtils::HashCombine;
    uint64_t h = kHashSeed;
    h = HashCombine(h, static_cast<uint64_t>(ColorCount));
    for (uint8_t i = 0; i < ColorCount && i < kMaxColors; ++i)
    {
        h = HashCombine(h, static_cast<uint64_t>(ColorFormats[i]));
    }
    h = HashCombine(h, static_cast<uint64_t>(DepthFormat));
    h = HashCombine(h, static_cast<uint64_t>(StencilFormat));
    h = HashCombine(h, static_cast<uint64_t>(RasterizationSamples));
    return h;
}

// =====================================================================
// DescriptorSetLayoutDesc content hash
// =====================================================================

uint64_t HashDescriptorSetLayoutDesc(const DescriptorSetLayoutDesc& desc) noexcept
{
    using HashUtils::HashCombine;
    using HashUtils::HashValue;
    uint64_t h = kHashSeed;
    h = HashValue(h, desc.bindings.size());
    for (const auto& b : desc.bindings)
    {
        h = HashValue(h, b.binding);
        h = HashValue(h, b.type);
        h = HashValue(h, b.count);
        h = HashValue(h, b.shaderStages);
        h = HashValue(h, b.flags);
        h = HashValue(h, b.imageDim);
        h = HashValue(h, b.imageArrayed);
        h = HashValue(h, b.imageIsDepth);
        h = HashValue(h, b.imageIsUnsignedInteger);
        h = HashValue(h, b.storageTexelFormat);
        // WebGPU bakes these into the bind group layout, so two descs that
        // differ only here must intern to two layouts.
        h = HashValue(h, b.storageReadOnly);
        h = HashValue(h, b.imageFilterableFloat);
        h = HashValue(h, b.imageUsageReflected);
        h = HashValue(h, b.imageMultisample);
        // debugName intentionally NOT hashed
    }
    return h;
}

// =====================================================================
// Helpers for desc hashing (used by GraphicsPipelineDesc / ComputePipelineDesc)
// =====================================================================

namespace
{

// Hash a `shared_ptr<const vector<uint8_t>>` shader-byte ingredient.
// Null pointer hashes the same as an empty vector — semantically equivalent.
uint64_t HashShader(uint64_t h, const std::shared_ptr<const std::vector<uint8_t>>& bytes) noexcept
{
    using HashUtils::HashBytes;
    using HashUtils::HashValue;
    if (!bytes)
    {
        h = HashValue(h, uint64_t{0}); // size 0
        return h;
    }
    return HashBytes(h, *bytes);
}

uint64_t HashRasterizationState(uint64_t h, const RasterizationState& s) noexcept
{
    using HashUtils::HashValue;
    h = HashValue(h, s.depthClampEnable);
    h = HashValue(h, s.rasterizerDiscardEnable);
    h = HashValue(h, s.polygonMode);
    h = HashValue(h, s.cullMode);
    h = HashValue(h, s.frontFace);
    h = HashValue(h, s.depthBiasEnable);
    h = HashValue(h, s.depthBiasConstantFactor);
    h = HashValue(h, s.depthBiasClamp);
    h = HashValue(h, s.depthBiasSlopeFactor);
    h = HashValue(h, s.lineWidth);
    return h;
}

uint64_t HashDepthStencilState(uint64_t h, const DepthStencilState& s) noexcept
{
    using HashUtils::HashValue;
    h = HashValue(h, s.depthTestEnable);
    h = HashValue(h, s.depthWriteEnable);
    h = HashValue(h, s.depthCompareOp);
    h = HashValue(h, s.depthBoundsTestEnable);
    h = HashValue(h, s.stencilTestEnable);
    h = HashValue(h, s.minDepthBounds);
    h = HashValue(h, s.maxDepthBounds);
    return h;
}

uint64_t HashColorBlendAttachment(uint64_t h, const ColorBlendAttachmentState& s) noexcept
{
    using HashUtils::HashValue;
    h = HashValue(h, s.blendEnable);
    h = HashValue(h, s.srcColorBlendFactor);
    h = HashValue(h, s.dstColorBlendFactor);
    h = HashValue(h, s.colorBlendOp);
    h = HashValue(h, s.srcAlphaBlendFactor);
    h = HashValue(h, s.dstAlphaBlendFactor);
    h = HashValue(h, s.alphaBlendOp);
    h = HashValue(h, s.colorWriteMask);
    return h;
}

uint64_t HashColorBlendState(uint64_t h, const ColorBlendState& s) noexcept
{
    using HashUtils::HashValue;
    h = HashValue(h, s.logicOpEnable);
    h = HashValue(h, s.alphaToCoverageEnable);
    h = HashValue(h, s.attachments.size());
    for (const auto& a : s.attachments)
        h = HashColorBlendAttachment(h, a);
    h = HashValue(h, s.blendConstants[0]);
    h = HashValue(h, s.blendConstants[1]);
    h = HashValue(h, s.blendConstants[2]);
    h = HashValue(h, s.blendConstants[3]);
    return h;
}

uint64_t HashPushConstants(uint64_t h, const PushConstantRange& r) noexcept
{
    using HashUtils::HashValue;
    h = HashValue(h, r.Offset);
    h = HashValue(h, r.Size);
    h = HashValue(h, r.StageMask);
    return h;
}

uint64_t HashNamedPushConstantRanges(uint64_t h, const std::vector<NamedPushConstantRange>& v) noexcept
{
    using HashUtils::HashValue;
    h = HashValue(h, v.size());
    for (const auto& r : v)
    {
        // Name intentionally NOT hashed (diagnostic only).
        h = HashValue(h, r.Offset);
        h = HashValue(h, r.Size);
        h = HashValue(h, r.StageMask);
    }
    return h;
}

uint64_t HashVertexBindings(uint64_t h, const std::vector<VertexInputBinding>& v) noexcept
{
    using HashUtils::HashValue;
    h = HashValue(h, v.size());
    for (const auto& b : v)
    {
        h = HashValue(h, b.binding);
        h = HashValue(h, b.stride);
        h = HashValue(h, b.inputRate);
    }
    return h;
}

uint64_t HashVertexAttributes(uint64_t h, const std::vector<VertexInputAttribute>& v) noexcept
{
    using HashUtils::HashValue;
    h = HashValue(h, v.size());
    for (const auto& a : v)
    {
        h = HashValue(h, a.location);
        h = HashValue(h, a.binding);
        h = HashValue(h, a.format);
        h = HashValue(h, a.offset);
    }
    return h;
}

uint64_t HashDescriptorSetLayoutIds(uint64_t h, const std::vector<DescriptorSetLayoutId>& v) noexcept
{
    using HashUtils::HashValue;
    h = HashValue(h, v.size());
    for (const auto& id : v)
        h = HashValue(h, id.Value);
    return h;
}

uint64_t HashDynamicState(uint64_t h, const std::optional<DynamicStateInfo>& s) noexcept
{
    using HashUtils::HashValue;
    h = HashValue(h, s.has_value());
    if (s.has_value())
    {
        h = HashValue(h, s->states.size());
        for (auto state : s->states)
            h = HashValue(h, state);
    }
    return h;
}

uint64_t HashSpecializationConstants(uint64_t h, const std::optional<SpecializationConstants>& s) noexcept
{
    using HashUtils::HashCombine;
    using HashUtils::HashValue;
    h = HashValue(h, s.has_value());
    if (s.has_value())
    {
        // Hash the actual content (constant ids + byte values, sorted by id)
        // so two callers that build the same specialization constants intern
        // to the same pipeline id regardless of insertion order. Previously
        // this used pointer identity, which fragmented the cache per call
        // site once specialization constants were adopted.
        h = HashCombine(h, s.value().GetHash());
    }
    return h;
}

} // anonymous namespace

// =====================================================================
// GraphicsPipelineDesc::ContentHash
// =====================================================================

uint64_t GraphicsPipelineDesc::ContentHash() const noexcept
{
    using HashUtils::HashValue;
    uint64_t h = kHashSeed;
    h = HashValue(h, Kind);
    h = HashShader(h, VertexShader);
    h = HashShader(h, PixelShader);
    h = HashShader(h, MeshShader);
    h = HashShader(h, AmplificationShader);
    h = HashDescriptorSetLayoutIds(h, DescriptorSetLayouts);
    h = HashRasterizationState(h, Rasterization);
    h = HashDepthStencilState(h, DepthStencil);
    h = HashColorBlendState(h, ColorBlend);
    h = HashDynamicState(h, DynamicState);
    h = HashPushConstants(h, PushConstants);
    h = HashNamedPushConstantRanges(h, NamedPushConstantRanges);
    h = HashVertexBindings(h, VertexBindings);
    h = HashVertexAttributes(h, VertexAttributes);
    h = HashValue(h, Topology);
    h = HashSpecializationConstants(h, Specialization);
    // DebugName intentionally NOT hashed.
    return h;
}

// =====================================================================
// ComputePipelineDesc::ContentHash
// =====================================================================

uint64_t ComputePipelineDesc::ContentHash() const noexcept
{
    using HashUtils::HashValue;
    uint64_t h = kHashSeed;
    h = HashShader(h, ComputeShader);
    h = HashDescriptorSetLayoutIds(h, DescriptorSetLayouts);
    h = HashPushConstants(h, PushConstants);
    h = HashNamedPushConstantRanges(h, NamedPushConstantRanges);
    h = HashSpecializationConstants(h, Specialization);
    // DebugName intentionally NOT hashed.
    return h;
}

bool PipelineFormatKey::operator==(const PipelineFormatKey& other) const noexcept
{
    if (ColorCount != other.ColorCount) return false;
    if (DepthFormat != other.DepthFormat) return false;
    if (StencilFormat != other.StencilFormat) return false;
    if (RasterizationSamples != other.RasterizationSamples) return false;
    // Only compare in-use slots — slots beyond ColorCount may be zero-padded
    // or stale and must not affect equality.
    for (uint8_t i = 0; i < ColorCount; ++i)
    {
        if (ColorFormats[i] != other.ColorFormats[i]) return false;
    }
    return true;
}

} // namespace GameEngine::Rendering
