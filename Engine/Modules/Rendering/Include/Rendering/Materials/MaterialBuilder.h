#pragma once

#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <functional>

namespace GameEngine { namespace Rendering {

class MaterialBuilder {
public:
    // The ONE reflection-meta -> descriptor-set-layout conversion. A pipeline
    // layout and the bind group bound into it must describe a set identically:
    // WebGPU rejects the bind outright when they differ, and a second
    // hand-rolled converter drifts silently (image dimension, arrayedness,
    // depth-ness and read-only storage were all missing from the binder's copy,
    // which Vulkan tolerated and Dawn did not).
    static DescriptorSetLayoutDesc BuildSetLayout(const DescriptorSetMeta& setMeta);

    // Build DescriptorSetLayoutDesc array from ShaderMeta
    static std::vector<DescriptorSetLayoutDesc> BuildSetLayouts(const ShaderMeta& meta);

    // Enforce push constant policy (<=128 bytes) and return total size; 0 if none
    static uint32_t ComputeTotalPushConstantSizeOrThrow(const ShaderMeta& meta);

    // Fill PipelineDesc descriptor set layouts from meta and enforce policies
    // Compute deduplicated layout inputs from ShaderMeta (set layouts + push constants)
    static void BuildPipelineLayoutInputs(const ShaderMeta& meta, std::vector<DescriptorSetLayoutDesc>& outSetLayouts,
                                          uint32_t& outPushConstantSize, uint32_t& outPushConstantStagesMask);

    // Strict builder: fills all relevant fields from meta; throws on conflicts or policy violations
    static void BuildPipelineDescFromMeta(const ShaderMeta& meta, PipelineDesc& desc);

    enum class MergeMode { Auto, Off, Require };
    struct PushConstantPolicy { bool Enforce = true; uint32_t Limit = 128; };

    struct FormatsHint {
        std::vector<uint32_t> ColorFormats; // Engine-level numeric format identifiers.
        uint32_t DepthFormat = 0;          // 0 = undefined; backend interprets numeric value in its own format space.
    };

    // Preferred non-throwing helper: fills only missing fields when mode=Auto, validates conflicts, enforces policy
    // Returns true on success; false + outError on conflict/policy errors
    static bool BuildPipelineDescFromMeta(const ShaderMeta& meta,
                                          PipelineDesc& desc,
                                          MergeMode mode,
                                          const PushConstantPolicy& policy,
                                          std::string* outError = nullptr);

    // Overloads with rendering format hints for dynamic rendering pipelines
    static void BuildPipelineDescFromMeta(const ShaderMeta& meta,
                                          PipelineDesc& desc,
                                          const FormatsHint& formatsHint);

    static bool BuildPipelineDescFromMeta(const ShaderMeta& meta,
                                          PipelineDesc& desc,
                                          const FormatsHint& formatsHint,
                                          MergeMode mode,
                                          const PushConstantPolicy& policy,
                                          std::string* outError = nullptr);

    // ---- Typed-desc overloads ------------------------------------------
    //
    // Populate `GraphicsPipelineDesc` / `ComputePipelineDesc` directly from
    // ShaderMeta, including interning the descriptor-set layouts via the
    // device's pipeline cache. The optional `patchLayout` callback runs on
    // each `DescriptorSetLayoutDesc` after build-from-meta but BEFORE
    // interning, so callers can swap layouts per site (e.g. the bindless
    // set-1 patch) without losing the intern dedup invariant. The
    // `setIndex` passed to the callback is the canonical SPIR-V set index
    // from `ShaderMeta` (not the array slot).
    //
    // Failure semantics: on `false` return, `outDesc` is left unchanged.
    // The build is computed in locals and applied to `outDesc` only after
    // all checks pass.

    using LayoutPatchFn = std::function<void(uint32_t setIndex, DescriptorSetLayoutDesc& dsl)>;

    static bool BuildGraphicsPipelineDescFromMeta(
        IDevice& device,
        const ShaderMeta& meta,
        GraphicsPipelineDesc& outDesc,
        MergeMode mode,
        const PushConstantPolicy& policy,
        const LayoutPatchFn& patchLayout = {},
        std::string* outError = nullptr);

    static bool BuildComputePipelineDescFromMeta(
        IDevice& device,
        const ShaderMeta& meta,
        ComputePipelineDesc& outDesc,
        MergeMode mode,
        const PushConstantPolicy& policy,
        const LayoutPatchFn& patchLayout = {},
        std::string* outError = nullptr);

};



}} // namespace

