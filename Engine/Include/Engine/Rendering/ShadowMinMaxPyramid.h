#pragma once

#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <cstdint>
#include <memory>
#include <unordered_map>

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope
// using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::ComputePipelineId;
using ::GameEngine::Rendering::DescriptorSetLayoutDesc;
using ::GameEngine::Rendering::IDevice;
using ::GameEngine::Rendering::SamplerHandle;
using ::GameEngine::Rendering::ShaderMeta;

class RenderServices;

// Min/max depth pyramid over the directional cascade shadow depth, the
// acceleration structure behind the PCSS early-outs.
//
// One RG32_SFLOAT 2D-array texture per view: R = min depth, G = max depth, one
// array layer per cascade, `LevelCount` mip levels starting at shadow
// resolution >> kBaseDownshift. shadow_minmax_reduce.comp fills it — level 0
// reduces a 4x4 footprint of the D32 cascade depth, each later level a 2x2
// footprint of its predecessor.
//
// Reverse-Z: near -> 1.0, far -> 0.0, so G (max) is the NEAREST occluder in a
// footprint and R (min) the farthest. A receiver behind every occluder in its
// footprint (max <= ref) is fully lit; one in front of all of them (min > ref)
// is fully shadowed. Either verdict skips the blocker search and the PCF loop.
//
// The cascade depth is never mipped directly: Vulkan makes linear filtering
// optional for depth formats and comparison samplers cannot read raw values,
// which is why this separate pyramid exists.
class ShadowMinMaxPyramid
{
  public:
    // Base level is the shadow resolution shifted down by this much (2 =>
    // quarter resolution => a level-0 texel covers a 4x4 shadow-texel
    // footprint, which is the reduction shadow_minmax_reduce.comp's mode 0
    // performs).
    static constexpr int kBaseDownshift = 2;

    // How far a PCSS query reaches past its kernel radius, in full-resolution
    // shadow texels: every PCF tap is a hardware 2x2 bilinear compare, reading
    // up to 1.5 texels past its centre. Mirror of kPcssPyramidQueryMargin in
    // shadow_sampling.glsl.
    static constexpr float kQueryMarginTexels = 1.5f;

    // C++ mirror of shadow_minmax_reduce.comp's `Push` block. Field order,
    // types and offsets match it exactly, and so does the size: the block ends
    // on a multiple of its largest member's alignment (ivec2: 8), the size an
    // MSL struct rounds up to, so every backend reads the 24 bytes pushed.
    struct PushConstants
    {
        int32_t DstSize[2];
        int32_t LayerCount;
        int32_t SrcLevel;
        int32_t Mode;
        int32_t Pad0;
    };

    // Levels a pyramid over a `baseExtent`-texel base level needs so its top
    // level's footprint covers a query of `queryTexels` full-resolution texels:
    // the base footprint is 1 << kBaseDownshift texels and doubles per level.
    // Bounded by the base level's own mip chain; at least one level.
    static uint32_t LevelsForQuery(uint32_t baseExtent, float queryTexels);

    // Declare one compute dispatch per level, covering all cascade layers in
    // its Z workgroups, and return the pyramid texture, or an invalid handle
    // when the pyramid is off or its inputs are unusable. Gating is structural:
    // `enabled == false` declares NO passes at
    // all, so the disabled arm costs exactly zero GPU time — a pass that ran
    // and early-returned would still show up in an A/B measurement.
    //
    // `cascadeDepth` is the pooled D32 cascade depth array; `shadowResolution`
    // its per-layer extent. `widestKernelTexels` is the widest PCF kernel any
    // cascade can pick, in that cascade's texels: the levels follow it
    // (LevelsForQuery), so the PCSS lit proof always finds a level covering its
    // query. The render graph derives the level-to-level ordering from the
    // declared read/write ranges — no manual barriers.
    Rendering::RenderGraph::RGTexture Declare(Rendering::RenderGraph::RGFrame& frame,
                                              RenderServices& rs, Rendering::ViewId viewId,
                                              Rendering::RenderGraph::RGTexture cascadeDepth,
                                              uint32_t shadowResolution, uint32_t cascadeCount,
                                              float widestKernelTexels, bool enabled);

    // The pyramid `viewId` has THIS frame. Everything a later phase needs to
    // publish it, answered through one lookup so the three facts can never come
    // from different declarations.
    struct FrameState
    {
        // Pooled physical the levels were reduced into. Resolved at Declare
        // time rather than handing out the RGTexture: that id is graph-local
        // and every consumer needs the physical anyway, so the id never leaves
        // the frame that owns it.
        Rendering::TextureHandle Physical;
        // 0 means the view has NO pyramid — publish the "no pyramid" sentinel.
        // Never inferred from PCSS being active: PCSS is the effective quality
        // on frames that have no pyramid at all.
        int Levels = 0;
        // Cascade layers the pyramid actually has, which is not always the
        // view's cascade count. A per-layer view built past this layer fails to
        // create, and a failed view falls back to the whole-array view — an
        // image2DArray bound where a sampler2D is declared.
        uint32_t Layers = 0;
    };

    // `viewId`'s pyramid as of `frame`, or a zeroed state when it has none.
    //
    // Keyed by view because the UBO carrying this is built in a later phase
    // (BuildShadowDataGPU), by which point a per-object value would answer with
    // whichever view declared most recently. Stamped with the frame because a
    // view can stop reaching Declare entirely — its light stops casting, or the
    // node returns before the feature runs — and a decline that never executes
    // cannot clear its own entry. Both are the same failure: reading state that
    // belonged to a different declaration.
    //
    // Takes the frame, never a frame index. BuildShadowDataGPU carries an
    // unrelated `uint32_t frameIndex` of its own (a per-node debug counter), so
    // an index parameter would bind it silently and report "no pyramid" for
    // every frame — an early-out that never engages, reported as an
    // optimisation that does nothing. RGFrameStamp also pins frame IDENTITY, so
    // a second window's frame cannot alias this one by index alone (RGFrame.h's
    // caching rule).
    FrameState StateFor(Rendering::ViewId viewId,
                        const Rendering::RenderGraph::RGFrame& frame) const;

    // Drop the device-derived state a rebuild destroyed, WITHOUT destroying it —
    // the rebuild already freed the pipeline, the sampler and every pooled
    // physical. Without this the next Declare would bind a dead pipeline and a
    // dead sampler, because the load is one-shot.
    void OnDeviceRebuilt();

  private:
    void LoadShader(IDevice* device);

    // One dispatch: reduce all `layers` into mip `level` of `pyramid`.
    // Level 0 reads `cascadeDepth`, later levels read `pyramid` at level - 1.
    void DeclareLevelPass(Rendering::RenderGraph::RGFrame& frame,
                          Rendering::RenderGraph::RGTexture cascadeDepth,
                          Rendering::RenderGraph::RGTexture pyramid, Rendering::ViewId viewId,
                          uint32_t layers, uint32_t level, uint32_t dstWidth, uint32_t dstHeight);

    bool m_LoadAttempted = false;
    ComputePipelineId m_PipelineId{};
    std::unique_ptr<ShaderMeta> m_Meta;
    DescriptorSetLayoutDesc m_Set0Layout{};
    // uDst's set-0 index, resolved from reflection at load so a layout edit in
    // shadow_minmax_reduce.comp stays in lockstep with the bind. The two
    // sampled sources go through NamedDescriptorWriter, which resolves its own.
    uint32_t m_DstBinding = 0;
    SamplerHandle m_Sampler{};

    // A view's pyramid as of one frame. The stamp is what makes a stale entry
    // detectable: an entry only answers for the frame incarnation that wrote it.
    struct ViewPyramid
    {
        Rendering::RenderGraph::RGFrameStamp Stamp;
        FrameState State;
    };
    std::unordered_map<Rendering::ViewId, ViewPyramid> m_PyramidByView;
};
} // namespace GameEngine::Engine::Renderer
