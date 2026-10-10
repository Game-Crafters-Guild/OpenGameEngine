// Pass-scope state owned by MaterialBinder. Returned from BeginPass and
// threaded through BindMaterialForDraw calls; tracks the resolved per-view
// world-shared resource table plus the sticky pipeline / per-set state that
// lets the binder skip redundant vkCmdBindDescriptorSets calls.
//
// The binder is fully name-driven: it resolves each reflected binding by
// StringId against multiple sources (per-draw, per-pass, per-material). No
// hardcoded "set 0 is special / set 1 is material" policy lives in the
// binder — the shader's reflected layout is the single source of truth.

#pragma once

#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Types/StringId.h"

#include <array>
#include <cstdint>
#include <vector>

namespace GameEngine::Rendering
{
class CommandList;
namespace RenderGraph
{
class RGContext;
}
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{

// Maximum descriptor sets per pipeline. Vulkan guarantees at least 4 in core;
// the binder tracks one sticky-state slot per set up to this bound.
inline constexpr uint32_t kMaxDescriptorSets = 8;

// Resolved per-(view, pass) resource table. Populated once per pass by
// MaterialBinder::BeginPass via RenderServices::ResolvePassResources; reused
// across every draw in the pass. The name was not bound to a set index — these
// are world-shared resources that the binder resolves against whichever set
// the shader puts them in.
struct ResolvedPassResources
{
    // Field names match DrawBindings::BufferEntry so MaterialBinder lookup
    // helpers can use the same accessor shape against either source.
    struct BufferEntry
    {
        StringId Name{};
        ::GameEngine::Rendering::BufferHandle Buffer{};
        uint64_t Offset = 0;
        // 0 == whole buffer.
        uint64_t Range  = 0;
    };

    // Texture/sampler bindings resolved from pass keywords (shadow map, MSM
    // moments). Keyed by StringId so the binder can resolve them via
    // reflection against whichever binding the shader declared.
    struct TextureEntry
    {
        StringId Name{};
        ::GameEngine::Rendering::TextureHandle Texture{};
        ::GameEngine::Rendering::SamplerHandle Sampler{};
    };

    std::vector<BufferEntry>  Buffers;
    std::vector<TextureEntry> Textures;
};

struct PassBindingContext
{
    // Identity — set by BeginPass, immutable afterwards.
    ::GameEngine::Rendering::CommandList*               Cmd       = nullptr;
    ::GameEngine::Rendering::ViewId                     View      = 0;
    const ::GameEngine::Rendering::RenderGraph::RGContext*      PassRG    = nullptr;
    ::GameEngine::Rendering::MaterialKeyword            PassKeywords{};
    uint32_t                                            FrameSlot = 0;
    uint32_t                                            Samples   = 1;
    // Disambiguates multiple instances of the same logical pass that share
    // (view, keywords, layout) but bind distinct per-pass resources. Shadow
    // cascades use their cascade index; depth prepass and forward pass pass 0.
    // Threaded into the per-pass descriptor-set cache key so cascade N's
    // descriptor writes don't poison cascade M's cached set.
    uint32_t                                            PassInstanceIndex = 0;

    // Pre-resolved per-(view, pass) world-shared resources. The binder
    // resolves each reflected binding by name; this table is one of the
    // sources it consults.
    ResolvedPassResources PassResources{};

    // Dirty state mutated by BindMaterialForDraw. Used to skip redundant
    // pipeline + descriptor-set binds when consecutive draws share state.
    ::GameEngine::Rendering::PipelineHandle      CurrentPipeline{};

    // Last descriptor-set handle bound at each set index. The binder skips
    // BindDescriptorSet when the resolved handle for a set matches the
    // currently-bound handle — the standard pipeline-layout-compatibility
    // sticky-binding optimisation.
    std::array<::GameEngine::Rendering::DescriptorSetHandle, kMaxDescriptorSets> CurrentSets{};

    // Layout id of the currently-bound pipeline's descriptor set at each
    // index. Used by MaterialBinder on SetPipeline to find the FIRST set
    // whose layout differs from the prior pipeline's. Vulkan disturbs all
    // descriptor set bindings from that point upward; sets below stay
    // sticky. The previous "fill({}) on every pipeline change" approach
    // over-cleared and forced redundant rebinds even when set-0 layouts
    // were identical across the transition.
    std::array<::GameEngine::Rendering::DescriptorSetLayoutId, kMaxDescriptorSets> CurrentSetLayouts{};

    // Draw-attribution instrumentation: the count of pipeline / descriptor-set
    // binds the binder actually recorded for this pass (redundant binds skipped
    // by the sticky state do NOT count). A consumer reads these after its draw
    // loop to attribute per-pass CPU submission cost. Always incremented (a
    // bare ++), reported only under GE_DRAW_ATTRIBUTION.
    uint32_t PipelineBinds   = 0;
    uint32_t DescriptorBinds = 0;
};

} // namespace GameEngine::Engine::Renderer
