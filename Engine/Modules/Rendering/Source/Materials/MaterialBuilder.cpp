#include "Rendering/Materials/MaterialBuilder.h"
#include <algorithm>
#include <stdexcept>
#include "Rendering/Materials/ShaderMetaValidation.h"


namespace GameEngine { namespace Rendering {

static DescriptorType MapBindingType(uint32_t t) {
    using namespace ShaderMetaBindingType;
    switch (t) {
        case kUniformBuffer:        return DescriptorType::UniformBuffer;
        case kStorageBuffer:        return DescriptorType::StorageBuffer;
        case kSampler:              return DescriptorType::Sampler;
        case kSampledImage:         return DescriptorType::Texture;
        case kStorageImage:         return DescriptorType::StorageImage;
        case kCombinedImageSampler: return DescriptorType::CombinedImageSampler;
        case kAccelerationStructure: return DescriptorType::AccelerationStructure;
        default: return DescriptorType::UniformBuffer;
    }
}


// Map the ShaderMeta stage mask (ShaderMetaStage) to backend-native flags (Vulkan by default)
static uint32_t MapAbstractStagesToBackend(uint32_t m) {
    uint32_t out = 0;
    // These constants mirror Vulkan VkShaderStageFlagBits values; D3D12 ignores them
    if (m & ShaderMetaStage::kVertex)                 out |= 0x00000001; // VK_SHADER_STAGE_VERTEX_BIT
    if (m & ShaderMetaStage::kFragment)               out |= 0x00000010; // VK_SHADER_STAGE_FRAGMENT_BIT
    if (m & ShaderMetaStage::kCompute)                out |= 0x00000020; // VK_SHADER_STAGE_COMPUTE_BIT
    if (m & ShaderMetaStage::kGeometry)               out |= 0x00000008; // VK_SHADER_STAGE_GEOMETRY_BIT
    if (m & ShaderMetaStage::kMesh)                   out |= 0x00000080; // VK_SHADER_STAGE_MESH_BIT_EXT (value in headers)
    if (m & ShaderMetaStage::kTessellationControl)    out |= 0x00000002; // VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT
    if (m & ShaderMetaStage::kTessellationEvaluation) out |= 0x00000004; // VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
    return out;
}


DescriptorSetLayoutDesc MaterialBuilder::BuildSetLayout(const DescriptorSetMeta& setMeta) {
    DescriptorSetLayoutDesc dsl{};
    dsl.bindings.reserve(setMeta.Bindings.size());
    for (const auto& b : setMeta.Bindings) {
        DescriptorBinding db{};
        db.binding = b.Binding;
        db.type = MapBindingType(b.Type);
        db.count = b.Count;
        db.shaderStages = MapAbstractStagesToBackend(b.StagesMask);
        db.debugName = b.Name.c_str();
        if (b.ReadOnly)
            db.flags |= kDescriptorBindingReadOnlyStorage;
        if (b.Image)
        {
            // 4 = cube in DescriptorBinding's convention; meta reports
            // cube as Dim=2 + Cube for its existing consumers.
            db.imageDim = b.Image->Cube ? 4u : b.Image->Dim;
            db.imageArrayed = b.Image->Arrayed;
            db.imageIsDepth = b.Image->Depth;
            db.imageIsUnsignedInteger = b.Image->UnsignedInteger;
            db.imageFilterableFloat = b.Image->Filtered;
            db.imageUsageReflected = true;
            db.imageMultisample = b.Image->Multisample;
            db.storageTexelFormat = b.Image->Format;
        }
        dsl.bindings.push_back(db);
    }
    return dsl;
}

std::vector<DescriptorSetLayoutDesc> MaterialBuilder::BuildSetLayouts(const ShaderMeta& meta) {
    std::vector<DescriptorSetLayoutDesc> out;
    // Ensure stable ordering by set index
    std::vector<const DescriptorSetMeta*> sorted;
    sorted.reserve(meta.Sets.size());
    for (const auto& s : meta.Sets) sorted.push_back(&s);
    std::sort(sorted.begin(), sorted.end(), [](auto* a, auto* b){ return a->Set < b->Set; });

    for (auto* sm : sorted)
        out.push_back(BuildSetLayout(*sm));
    return out;
}

uint32_t MaterialBuilder::ComputeTotalPushConstantSizeOrThrow(const ShaderMeta& meta) {
    uint32_t maxSize = 0; uint32_t stages = 0;
    for (const auto& r : meta.PushConstants) { maxSize = std::max(maxSize, r.Size); stages |= r.StagesMask; }
    if (maxSize > 128) {
        throw std::runtime_error("Push constant size exceeds 128 bytes policy");
    }
    return maxSize;
}
void MaterialBuilder::BuildPipelineLayoutInputs(const ShaderMeta& meta, std::vector<DescriptorSetLayoutDesc>& outSetLayouts,
                                                 uint32_t& outPushConstantSize, uint32_t& outPushConstantStagesMask) {
    outSetLayouts = BuildSetLayouts(meta);
    outPushConstantSize = ComputeTotalPushConstantSizeOrThrow(meta);
    uint32_t stages = 0; for (const auto& r : meta.PushConstants) stages |= MapAbstractStagesToBackend(r.StagesMask);
    if (stages == 0 && outPushConstantSize > 0) stages = 0xFFFFFFFFu; // backend policy fallback
    outPushConstantStagesMask = stages;
}


void MaterialBuilder::BuildPipelineDescFromMeta(const ShaderMeta& meta, PipelineDesc& desc) {
    // Validate meta first (throws here on errors)
    {
        auto report = ValidateShaderMeta(meta, 128);
        if (report.HasErrors()) {
            throw std::runtime_error("ShaderMeta validation failed");
        }
    }
    // Set layouts only if meta contains sets; otherwise preserve existing desc value
    if (!meta.Sets.empty()) {
        desc.descriptorSetLayouts = BuildSetLayouts(meta);
    }
    // Enforce push constant cap and set optional fields
    uint32_t total = ComputeTotalPushConstantSizeOrThrow(meta);
    desc.pushConstantSize = total; // sum of ranges (packed with 4-byte alignment will be applied by backend)
    // Stages: union from meta; backend will map numeric stages appropriately
    // Ensure total respects backend maximum; MaterialBuilder should not overrun policy
    if (total > 128) total = 128; // default backend policy cap; device may clamp further

    uint32_t stages = 0; for (const auto& r : meta.PushConstants) stages |= MapAbstractStagesToBackend(r.StagesMask); if (stages == 0 && total > 0) stages = 0xFFFFFFFFu; // backend policy fallback
    desc.pushConstantStagesMask = stages;
    // Fill named ranges preserving metadata order; backend will finalize 4-byte aligned packing
    desc.pushConstantRanges.clear(); desc.pushConstantRanges.reserve(meta.PushConstants.size());
    {
        uint32_t runningOffset = 0;
        for (const auto& r : meta.PushConstants) {
            PipelineDesc::PushConstantRangeDesc pr{};
            pr.name = r.Name; pr.size = r.Size; pr.stagesMask = MapAbstractStagesToBackend(r.StagesMask); pr.offset = runningOffset;
            runningOffset += r.Size; // backend will align up
            desc.pushConstantRanges.push_back(std::move(pr));
        }
    }

}


bool MaterialBuilder::BuildPipelineDescFromMeta(const ShaderMeta& meta,
                                                PipelineDesc& desc,
                                                MergeMode mode,
                                                const PushConstantPolicy& policy,
                                                std::string* outError) {
    if (mode == MergeMode::Off) return true;
    // Basic sanity on meta
    if (mode == MergeMode::Require) {
        if (meta.Sets.empty() && meta.PushConstants.empty() && meta.EntryPoints.empty()) {
			if (outError)
			{
				*outError = "ShaderMeta missing or empty";
			}
			return false;
        }
    }

    // Validate meta early with policy.Limit
    {
        auto report = ValidateShaderMeta(meta, policy.Limit ? policy.Limit : 128);
        if (report.HasErrors()) {
            if (outError) {
                std::string msg = "Validation failed: ";
                for (const auto& i : report.Issues) if (i.Severity == IssueSeverity::Error) { msg += i.Code + ": " + i.Message + "; "; }
                *outError = msg;
            }
            return false;
        }
    }

    // Compute from meta without mutating desc first
    std::vector<DescriptorSetLayoutDesc> metaDSL = BuildSetLayouts(meta);
    uint32_t metaPCSize = 0; uint32_t metaPCStages = 0;
    // Non-throwing computation for policy-aware path
    for (const auto& r : meta.PushConstants) { metaPCSize = std::max(metaPCSize, r.Size); metaPCStages |= MapAbstractStagesToBackend(r.StagesMask); }
    if (metaPCStages == 0 && metaPCSize > 0) metaPCStages = 0xFFFFFFFFu;

    // Enforce policy if enabled
    if (policy.Enforce && metaPCSize > policy.Limit) {
		if (outError)
		{
			*outError = "Push constant size exceeds policy limit";
		}
		return false;
    }

    // Conflicts: if desc already specifies DSL or push constants, they must match meta in Auto/Require
    auto hasDSL = !desc.descriptorSetLayouts.empty();
    auto hasPC  = (desc.pushConstantSize != 0) || (desc.pushConstantStagesMask != 0);

    if (hasDSL) {
        // Shallow structural check: counts and bindings (binding index + type + count + stages)
        // This is a basic check; deeper equivalence could be added later
        if (desc.descriptorSetLayouts.size() != metaDSL.size()) {
			if (outError)
			{
				*outError = "Descriptor set layout count conflict with metadata";
			}
			return false;
        }
    }
    if (hasPC) {
        if (desc.pushConstantSize != 0 && desc.pushConstantSize != metaPCSize) {
			if (outError)
			{
				*outError = "Push constant size conflicts with metadata";
			}
			return false;
        }
        if (desc.pushConstantStagesMask != 0 && desc.pushConstantStagesMask != metaPCStages) {
			if (outError)
			{
				*outError = "Push constant stagesMask conflicts with metadata";
			}
			return false;
        }
    }

    // Fill only missing fields when Auto/Require
    if (!hasDSL) desc.descriptorSetLayouts = std::move(metaDSL);
    if (!hasPC) {
        desc.pushConstantSize = metaPCSize;
        desc.pushConstantStagesMask = metaPCStages;
    }
    // Populate named push-constant ranges if not provided, preserving metadata order
    if (desc.pushConstantRanges.empty() && !meta.PushConstants.empty()) {
        desc.pushConstantRanges.clear();
        desc.pushConstantRanges.reserve(meta.PushConstants.size());
        uint32_t runningOffset = 0;
        for (const auto& r : meta.PushConstants) {
            PipelineDesc::PushConstantRangeDesc pr{};
            pr.name = r.Name; pr.size = r.Size; pr.stagesMask = MapAbstractStagesToBackend(r.StagesMask); pr.offset = runningOffset;
            runningOffset += r.Size; // backend will align to 4 bytes when creating VkPipelineLayout
            desc.pushConstantRanges.push_back(std::move(pr));
        }
    }
    return true;
}


void MaterialBuilder::BuildPipelineDescFromMeta(const ShaderMeta& meta,
                                                 PipelineDesc& desc,
                                                 const FormatsHint& formatsHint) {
    // Reuse strict builder then apply format hints for dynamic rendering
    BuildPipelineDescFromMeta(meta, desc);
    if (desc.colorAttachmentFormats.empty() && !formatsHint.ColorFormats.empty()) {
        desc.colorAttachmentFormats = formatsHint.ColorFormats;
    }
    if (desc.depthAttachmentFormat == 0 && formatsHint.DepthFormat != 0) {
        desc.depthAttachmentFormat = formatsHint.DepthFormat;
    }
}

bool MaterialBuilder::BuildPipelineDescFromMeta(const ShaderMeta& meta,
                                                PipelineDesc& desc,
                                                const FormatsHint& formatsHint,
                                                MergeMode mode,
                                                const PushConstantPolicy& policy,
                                                std::string* outError) {
    bool ok = BuildPipelineDescFromMeta(meta, desc, mode, policy, outError);
    if (!ok || mode == MergeMode::Off) return ok;

    // Fill only if missing, similar to Auto/Require behavior
    if (desc.colorAttachmentFormats.empty() && !formatsHint.ColorFormats.empty()) {
        desc.colorAttachmentFormats = formatsHint.ColorFormats;
    }
    if (desc.depthAttachmentFormat == 0 && formatsHint.DepthFormat != 0) {
        desc.depthAttachmentFormat = formatsHint.DepthFormat;
    }
    return true;

}

// =====================================================================
// Typed-desc overloads — populate GraphicsPipelineDesc / ComputePipelineDesc
// directly from ShaderMeta and intern the set layouts via the device.
//
// Shared core: compute layouts + push-constant inputs locally, validate
// against the caller-provided outDesc, apply optional patching, intern,
// then commit to outDesc. On failure, outDesc is left untouched.
// =====================================================================

namespace {

// Build the meta-derived layouts + push-constant inputs, validate against
// the caller-provided desc, and apply the patch callback. On success, the
// caller interns the patched layouts and assigns to the typed desc.
bool BuildAndValidateMetaInputs(
    const ShaderMeta& meta,
    MaterialBuilder::MergeMode mode,
    const MaterialBuilder::PushConstantPolicy& policy,
    bool descHasDSL,
    size_t descDSLCount,
    uint32_t descPCSize,
    uint32_t descPCStages,
    const MaterialBuilder::LayoutPatchFn& patchLayout,
    std::vector<DescriptorSetLayoutDesc>& outPatchedLayouts,
    uint32_t& outPCSize,
    uint32_t& outPCStages,
    std::string* outError) {
    if (mode == MaterialBuilder::MergeMode::Off) {
        outPatchedLayouts.clear();
        outPCSize = 0;
        outPCStages = 0;
        return true;
    }

    if (mode == MaterialBuilder::MergeMode::Require) {
        if (meta.Sets.empty() && meta.PushConstants.empty() && meta.EntryPoints.empty()) {
            if (outError) *outError = "ShaderMeta missing or empty";
            return false;
        }
    }

    auto report = ValidateShaderMeta(meta, policy.Limit ? policy.Limit : 128);
    if (report.HasErrors()) {
        if (outError) {
            std::string msg = "Validation failed: ";
            for (const auto& i : report.Issues)
                if (i.Severity == IssueSeverity::Error)
                    msg += i.Code + ": " + i.Message + "; ";
            *outError = msg;
        }
        return false;
    }

    std::vector<DescriptorSetLayoutDesc> metaDSL = MaterialBuilder::BuildSetLayouts(meta);
    uint32_t metaPCSize = 0;
    uint32_t metaPCStages = 0;
    for (const auto& r : meta.PushConstants) {
        metaPCSize = std::max(metaPCSize, r.Size);
        metaPCStages |= MapAbstractStagesToBackend(r.StagesMask);
    }
    if (metaPCStages == 0 && metaPCSize > 0) metaPCStages = 0xFFFFFFFFu;

    if (policy.Enforce && metaPCSize > policy.Limit) {
        if (outError) *outError = "Push constant size exceeds policy limit";
        return false;
    }

    // Conflict checks against caller-provided desc fields. Matches the
    // legacy PipelineDesc overload's logic.
    if (descHasDSL && descDSLCount != metaDSL.size()) {
        if (outError) *outError = "Descriptor set layout count conflict with metadata";
        return false;
    }
    const bool hasPC = (descPCSize != 0) || (descPCStages != 0);
    if (hasPC) {
        if (descPCSize != 0 && descPCSize != metaPCSize) {
            if (outError) *outError = "Push constant size conflicts with metadata";
            return false;
        }
        if (descPCStages != 0 && descPCStages != metaPCStages) {
            if (outError) *outError = "Push constant stagesMask conflicts with metadata";
            return false;
        }
    }

    // Apply the per-set patch callback before interning. The set index
    // passed back is the canonical SPIR-V set index from `meta.Sets`
    // (BuildSetLayouts sorts by set), not the array slot.
    if (patchLayout) {
        std::vector<uint32_t> sortedSetIndices;
        sortedSetIndices.reserve(meta.Sets.size());
        for (const auto& s : meta.Sets) sortedSetIndices.push_back(s.Set);
        std::sort(sortedSetIndices.begin(), sortedSetIndices.end());
        for (size_t i = 0; i < metaDSL.size() && i < sortedSetIndices.size(); ++i)
            patchLayout(sortedSetIndices[i], metaDSL[i]);
    }

    outPatchedLayouts = std::move(metaDSL);
    outPCSize = metaPCSize;
    outPCStages = metaPCStages;
    return true;
}

void InternLayoutsInto(IDevice& device,
                       const std::vector<DescriptorSetLayoutDesc>& patched,
                       std::vector<DescriptorSetLayoutId>& outIds) {
    outIds.reserve(outIds.size() + patched.size());
    for (const auto& dsl : patched)
        outIds.push_back(device.InternDescriptorSetLayout(dsl));
}

void FillNamedPushConstantRanges(const ShaderMeta& meta,
                                 std::vector<NamedPushConstantRange>& out) {
    out.clear();
    out.reserve(meta.PushConstants.size());
    uint32_t runningOffset = 0;
    for (const auto& r : meta.PushConstants) {
        NamedPushConstantRange nr{};
        nr.Name = r.Name;
        nr.Size = r.Size;
        nr.StageMask = MapAbstractStagesToBackend(r.StagesMask);
        nr.Offset = runningOffset;
        runningOffset += r.Size; // backend aligns to 4 bytes when finalizing
        out.push_back(std::move(nr));
    }
}

// Grow the colour-blend array to cover every location the fragment stage
// writes. A blend attachment describes a colour target, so a shader emitting
// location N needs N+1 of them; the array is otherwise inherited from whatever
// template the caller started from, which knows nothing about this variant's
// outputs. An MRT variant that keeps a one-entry array is silently wrong —
// Vulkan ignores the surplus targets and WebGPU write-masks them to None, so
// the extra slices are simply never stored. Grow only: entry 0 carries the
// material's authored blend state, and the added slots take the default
// (blending off, full write mask), which is what a G-buffer slice wants.
void SizeColorAttachmentsToFragmentOutputs(const ShaderMeta& meta,
                                           GraphicsPipelineDesc& outDesc) {
    const auto fs = meta.Stages.find("fs");
    if (fs == meta.Stages.end()) return;

    uint32_t colorAttachments = 0;
    for (const auto& output : fs->second.Outputs) {
        if (output.Builtin) continue; // gl_FragDepth and friends own no target
        colorAttachments = std::max(colorAttachments, output.Location + 1u);
    }
    if (outDesc.ColorBlend.attachments.size() < colorAttachments)
        outDesc.ColorBlend.attachments.resize(colorAttachments);
}

} // anonymous namespace

bool MaterialBuilder::BuildGraphicsPipelineDescFromMeta(
    IDevice& device,
    const ShaderMeta& meta,
    GraphicsPipelineDesc& outDesc,
    MergeMode mode,
    const PushConstantPolicy& policy,
    const LayoutPatchFn& patchLayout,
    std::string* outError) {

    if (mode == MergeMode::Off) return true;

    std::vector<DescriptorSetLayoutDesc> patchedLayouts;
    uint32_t metaPCSize = 0;
    uint32_t metaPCStages = 0;
    const bool descHasDSL = !outDesc.DescriptorSetLayouts.empty();
    const bool ok = BuildAndValidateMetaInputs(
        meta, mode, policy,
        descHasDSL, outDesc.DescriptorSetLayouts.size(),
        outDesc.PushConstants.Size, outDesc.PushConstants.StageMask,
        patchLayout,
        patchedLayouts, metaPCSize, metaPCStages,
        outError);
    if (!ok) return false;

    // Commit to outDesc only after all validation passes.
    if (!descHasDSL) {
        InternLayoutsInto(device, patchedLayouts, outDesc.DescriptorSetLayouts);
    }
    if (outDesc.PushConstants.Size == 0) outDesc.PushConstants.Size = metaPCSize;
    if (outDesc.PushConstants.StageMask == 0) outDesc.PushConstants.StageMask = metaPCStages;
    if (outDesc.NamedPushConstantRanges.empty() && !meta.PushConstants.empty()) {
        FillNamedPushConstantRanges(meta, outDesc.NamedPushConstantRanges);
    }
    SizeColorAttachmentsToFragmentOutputs(meta, outDesc);
    return true;
}

bool MaterialBuilder::BuildComputePipelineDescFromMeta(
    IDevice& device,
    const ShaderMeta& meta,
    ComputePipelineDesc& outDesc,
    MergeMode mode,
    const PushConstantPolicy& policy,
    const LayoutPatchFn& patchLayout,
    std::string* outError) {

    if (mode == MergeMode::Off) return true;

    std::vector<DescriptorSetLayoutDesc> patchedLayouts;
    uint32_t metaPCSize = 0;
    uint32_t metaPCStages = 0;
    const bool descHasDSL = !outDesc.DescriptorSetLayouts.empty();
    const bool ok = BuildAndValidateMetaInputs(
        meta, mode, policy,
        descHasDSL, outDesc.DescriptorSetLayouts.size(),
        outDesc.PushConstants.Size, outDesc.PushConstants.StageMask,
        patchLayout,
        patchedLayouts, metaPCSize, metaPCStages,
        outError);
    if (!ok) return false;

    if (!descHasDSL) {
        InternLayoutsInto(device, patchedLayouts, outDesc.DescriptorSetLayouts);
    }
    if (outDesc.PushConstants.Size == 0) outDesc.PushConstants.Size = metaPCSize;
    if (outDesc.PushConstants.StageMask == 0) outDesc.PushConstants.StageMask = metaPCStages;
    if (outDesc.NamedPushConstantRanges.empty() && !meta.PushConstants.empty()) {
        FillNamedPushConstantRanges(meta, outDesc.NamedPushConstantRanges);
    }
    return true;
}

}} // namespace
