#pragma once

#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <cstdint>
#include <span>

namespace GameEngine::Rendering::RenderGraph
{

// Shared fullscreen-pass spine. Every fullscreen post pass in the engine
// records the same body — resolve the pipeline variant, create a transient
// descriptor set, write N sampled-texture + M buffer bindings, set
// viewport/scissor, Draw(3,1) — and re-rolled it by hand (~40 lines per pass
// across OceanUnderwater/OceanLightShafts/OceanSceneGrab/SceneColorGrab).
// AddFullscreenPass owns that body plus the graph declaration; pipeline
// INTERNING stays with the caller (shaders and blend state are the pass's
// identity), seeded from MakeFullscreenPipelineDesc's canonical state.

// One sampled-texture input. Resolution order at record time:
//   1. Texture (graph) — declared Read(Sampled) at build; if its physical
//      resolves invalid: skip the draw when Required, else bind Fallback.
//   2. RawTexture — feature-owned physical bound descriptor-direct (its
//      layout contract is the owner's problem, e.g. via MarkOutput).
//   3. Fallback (+ FallbackSampler when set) — dummy so the binding is
//      always valid; if nothing resolves, the draw is skipped.
struct RGFullscreenTextureInput
{
    uint32_t Binding = 0;
    RGTexture Texture{};
    TextureHandle RawTexture{};
    TextureHandle Fallback{};
    SamplerHandle Sampler{};
    SamplerHandle FallbackSampler{}; // invalid => Sampler
    bool Required = false;
};

// One UBO/SSBO input at an offset — RGFrame::AllocUpload allocations bind
// directly. `Declare` (optional) adds a graph Read so a graph-produced buffer
// orders/barriers correctly; the binding itself always uses {Buffer, Offset}.
struct RGFullscreenBufferInput
{
    uint32_t Binding = 0;
    BufferHandle Buffer{};
    uint64_t Offset = 0;
    uint64_t Size = 0;
    bool IsStorage = false;
    RGBuffer Declare{};
};

struct RGFullscreenDesc
{
    const char* Name = nullptr;
    int32_t Phase = 0;
    // Pre-interned by the caller (variant-resolved via GetOrCreatePipelineVariant
    // at record). The caller picks the MSAA-resolve fork where a source can be
    // multisampled — it already knows at declare.
    GraphicsPipelineId Pipeline{};
    DescriptorSetLayoutDesc Layout{};
    std::span<const RGFullscreenTextureInput> Textures{};
    std::span<const RGFullscreenBufferInput> Buffers{};
    RGTexture Target{};
    RGAttachmentOps Ops{}; // Load for blend-composite, DontCare for overwrite
    // Viewport/scissor. 0 => the Target's RGResourceDesc dimensions; pass the
    // view's render size explicitly when the physical target is larger (view
    // rects, letterboxing).
    uint32_t Width = 0;
    uint32_t Height = 0;
};

// Declares the reads/attachment and records the canonical fullscreen body.
// Capacity: at most kMaxFullscreenTextureInputs/kMaxFullscreenBufferInputs
// inputs (asserted) — the exec lambda copies the spans into fixed arrays.
inline constexpr uint32_t kMaxFullscreenTextureInputs = 20;
inline constexpr uint32_t kMaxFullscreenBufferInputs = 4;
RGPass AddFullscreenPass(RGFrame& frame, const RGFullscreenDesc& desc);

// The self-read snapshot: copy `source` into `dest` so a composite pass can
// read the scene while writing it. Picks `pipeline` vs `msaaPipeline` (the
// sampler2DMS resolve fork) off source's SampleCount. Layouts must match the
// picked pipeline's set 0: one combined-image-sampler at binding 0.
RGPass AddCopyPass(RGFrame& frame, RGTexture source, RGTexture dest, const char* name,
                   int32_t phase, GraphicsPipelineId pipeline, GraphicsPipelineId msaaPipeline,
                   const DescriptorSetLayoutDesc& layout, const DescriptorSetLayoutDesc& msaaLayout,
                   SamplerHandle sampler, uint32_t width = 0, uint32_t height = 0);

// The non-MSAA pipeline for AddCopyPass: the stock copy program
// (Shaders/copy.shaderpkg) on canonical fullscreen state, with its set 0 layout
// (one combined-image-sampler at binding 0) written to `outLayout`. Returns
// false and leaves both outputs untouched when the package is missing. The
// names are borrowed, not copied, so they must outlive the descs.
bool LoadCopyPipelineDesc(ShaderSourceKind kind, const char* pipelineName, const char* layoutName,
                          DescriptorSetLayoutDesc& outLayout, PipelineDesc& outPipeline);

// Canonical fullscreen pipeline state: fullscreen-triangle vertex fetch (no
// vertex buffers), depth off, cull none, dynamic viewport/scissor, 1 sample.
// The caller fills shaders/layout/blend, then interns via
// PipelineDescTranslator::InternGraphics.
PipelineDesc MakeFullscreenPipelineDesc(const char* debugName);

} // namespace GameEngine::Rendering::RenderGraph
