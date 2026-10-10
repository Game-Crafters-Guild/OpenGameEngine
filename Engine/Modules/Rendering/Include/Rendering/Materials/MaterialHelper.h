#pragma once
#include <string>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/ReflectionCache.h"
#include "Rendering/Materials/MaterialBuilder.h"

namespace GameEngine { namespace Rendering {

class MaterialHelper {
public:
        // Meta-first: caller already has ShaderMeta (e.g. from .shaderpkg).
        // Legacy `PipelineDesc` shape — kept for tests and any caller not yet
        // migrated to typed descs. New code should prefer the
        // `ApplyShaderMetaToGraphicsDesc` / `ApplyShaderMetaToComputeDesc`
        // overloads below.
        static bool ApplyShaderMetaToPipelineDesc(const ShaderMeta& meta,
                                                  PipelineDesc& inOutDesc,
                                                  MaterialBuilder::MergeMode mergeMode = MaterialBuilder::MergeMode::Auto,
                                                  const MaterialBuilder::PushConstantPolicy& policy = {true, 128},
                                                  std::string* OutError = nullptr)
        {
            return MaterialBuilder::BuildPipelineDescFromMeta(meta, inOutDesc, mergeMode, policy, OutError);
        }

        // Typed-desc overloads: populate `GraphicsPipelineDesc` /
        // `ComputePipelineDesc` directly from meta, including interning the
        // descriptor-set layouts through the device's pipeline cache.
        //
        // The optional `patchLayout` callback runs on each
        // `DescriptorSetLayoutDesc` after build-from-meta but BEFORE
        // interning. Callers use it to swap layouts per site (e.g. the
        // bindless set-1 patch) without losing the intern dedup invariant.
        // The set index passed to the callback is the canonical SPIR-V set
        // index from `ShaderMeta` (not the array slot).
        //
        // Failure semantics: on `false` return, `inOutDesc` is left
        // unchanged.
        static bool ApplyShaderMetaToGraphicsDesc(IDevice& device,
                                                   const ShaderMeta& meta,
                                                   GraphicsPipelineDesc& inOutDesc,
                                                   MaterialBuilder::MergeMode mergeMode = MaterialBuilder::MergeMode::Auto,
                                                   const MaterialBuilder::PushConstantPolicy& policy = {true, 128},
                                                   const MaterialBuilder::LayoutPatchFn& patchLayout = {},
                                                   std::string* outError = nullptr)
        {
            return MaterialBuilder::BuildGraphicsPipelineDescFromMeta(
                device, meta, inOutDesc, mergeMode, policy, patchLayout, outError);
        }

        static bool ApplyShaderMetaToComputeDesc(IDevice& device,
                                                  const ShaderMeta& meta,
                                                  ComputePipelineDesc& inOutDesc,
                                                  MaterialBuilder::MergeMode mergeMode = MaterialBuilder::MergeMode::Auto,
                                                  const MaterialBuilder::PushConstantPolicy& policy = {true, 128},
                                                  const MaterialBuilder::LayoutPatchFn& patchLayout = {},
                                                  std::string* outError = nullptr)
        {
            return MaterialBuilder::BuildComputePipelineDescFromMeta(
                device, meta, inOutDesc, mergeMode, policy, patchLayout, outError);
        }

};

}} // namespace

