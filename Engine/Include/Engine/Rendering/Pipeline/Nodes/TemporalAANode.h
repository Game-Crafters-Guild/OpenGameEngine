#pragma once

#include "Engine/Rendering/Pipeline/DepthUpsamplePass.h"
#include "Engine/Rendering/Pipeline/ViewMotionVectors.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::ComputePipelineId;
using ::GameEngine::Rendering::DescriptorSetLayoutDesc;
using ::GameEngine::Rendering::GraphicsPipelineId;
using ::GameEngine::Rendering::SamplerHandle;
using ::GameEngine::Rendering::ShaderMeta;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Temporal AA (P0: native resolution). Two passes per TAA-enabled view:
//   taa_motion_vectors (raster) — draws ONLY this frame's movers
//     (RenderServices::GetFrameMovers) into a shared RGBA16F target with a read-only
//     GreaterOrEqual depth test against the raster depth, writing exact
//     per-instance UV-space motion from GPUInstance.prevTransform. Static
//     world needs no MV texture: the resolve reprojects it analytically from
//     depth (the volumetric-fog scheme).
//   taa_resolve (compute) — Catmull-Rom history + YCoCg variance clip +
//     closest-depth MV dilation + depth-disocclusion / velocity rejection +
//     Karis-weighted exponential blend. Writes the new history (ping-pong
//     persistent pool textures, fog idiom) and the pass output.
//
// The node is the TAA gate for the whole post chain: when the view has no
// ViewAntiAliasing state (AA mode != TAA) it stitches its input through under
// its output name, so downstream consumers (BloomThreshold, BloomCombine)
// bind one name unconditionally.
//
// JSON shape: { "id": "TemporalAA", "type": "TemporalAA",
//               "input": "HDRHeatDistortion", "output": "HDRTemporalAA" }
class TemporalAANode final : public IRenderPipelineNode
{
  public:
    ~TemporalAANode() override;

    const char* GetTypeName() const override { return "TemporalAA"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx) override;
    void DeclareForView(ViewDeclare& d) override;

    // Whether the previous frame's history may be blended this frame.
    //
    // INVARIANT: the extent terms here must mirror the history texture's desc
    // EXACTLY — no more, no less. The history/output textures are created at
    // the DISPLAY extent (outW/outH), so that pair alone decides whether the
    // persistent pool kept the physical or reallocated it to uninitialized
    // memory. Outside TAAU the display extent IS the render extent, so this
    // one rule covers both shapes.
    //
    // The internal (raster) extent is deliberately absent: nothing the resolve
    // reads across frames lives in internal space. History is Catmull-Rom
    // sampled at normalized display UV, reprojection motion is a dimensionless
    // UV delta, and the stored disocclusion depth is NDC. Gating on it would
    // throw away perfectly valid history on every render-scale change, which
    // is what dynamic resolution does by design.
    // historyFresh comes from ImportPersistentTexture: true means the pool
    // handed back a recycled/undefined physical, so nothing this view wrote
    // survives in it.
    static bool ComputeHistoryValid(uint64_t lastWrittenFrame, uint64_t frameIndex,
                                    uint32_t prevOutWidth, uint32_t prevOutHeight,
                                    uint32_t outWidth, uint32_t outHeight, bool prevCameraValid,
                                    bool historyFresh);

    // Whether this view's history may hold content that moved this frame: an
    // instance of the view's world whose transform or pose changed, or content
    // the view draws that moves without an instance or an epoch (moving
    // particles, RenderServices::HasUnversionedMotion). The motion-vector pass
    // filters the instance set down to exactly drawable movers; history
    // validation wants the conservative superset.
    static bool ViewHasMovers(const RenderServices& rs, Rendering::ViewId viewId,
                              uint64_t worldId);

    // The resolve's history mode (taa_resolve.comp, uTaa.params2.w): +1 clips
    // stale coverage (movers or changed content, including deleted objects),
    // -1 certifies unchanged content and camera (unclipped stationary history),
    // 0 uses per-pixel validation.
    static float HistoryClipMode(bool hasMovers, bool contentChanged, bool stationaryScene);

  private:
    void LoadShaders(IDevice* device);
    void LoadTaauShaders(IDevice* device); // lazy, first TAAU frame

    std::string m_InputKey = "HDRHeatDistortion";
    std::string m_OutputKey = "HDRTemporalAA";
    std::string m_Id;

    bool m_ShadersLoaded = false; // latches on SUCCESS; failed loads retry next frame
    bool m_WarnedUnresolvedInput = false;
    // Warn-once: a descriptor name that stops matching the shader reflection
    // (an unwritten descriptor = black output at best, undefined access at
    // worst — this exact silent no-op shipped a black first cut).
    bool m_WarnedBindingMismatch = false;

    // taa_resolve.comp
    ComputePipelineId m_ResolvePipelineId{};
    std::unique_ptr<ShaderMeta> m_ResolveMeta;
    DescriptorSetLayoutDesc m_ResolveSet0Layout{};

    // TAAU (render scale < 1): taa_resolve_upscale.comp — a deliberately
    // separate shader so the native path stays byte-identical at scale 1.0 —
    // plus taau_depth_upsample (fullscreen raster) reconstituting the caller's
    // display-res depth for overlay depth testing. Loaded lazily on the first
    // TAAU frame; while unstaged the node passes the internal-res input
    // through (downstream display-res consumers bilinear-upscale implicitly).
    ComputePipelineId m_UpscalePipelineId{};
    std::unique_ptr<ShaderMeta> m_UpscaleMeta;
    DescriptorSetLayoutDesc m_UpscaleSet0Layout{};
    // Shared with the non-temporal crossing (RenderScaleUpscaleNode) so the two
    // paths cannot drift on how display-res depth is reconstituted.
    DepthUpsamplePass m_DepthUpsample;
    bool m_TaauShadersLoaded = false; // latches on SUCCESS, like m_ShadersLoaded
    bool m_WarnedTaauUnstaged = false;

    ViewMotionVectors m_MotionVectors;

    SamplerHandle m_LinearClampSampler{};
    // The device the sampler above was created on, so the destructor can
    // return it; null until LoadShaders succeeds.
    IDevice* m_Device = nullptr;

    // Per-view history bookkeeping (the fog idiom): parity selects the
    // ping-pong pool texture; validity requires a previously rendered frame at
    // the same HISTORY extent whose physical the pool still holds (see
    // ComputeHistoryValid).
    struct ViewHistory
    {
        uint64_t HistoryFrame = 0;      // parity counter (advances once per resolved frame)
        uint64_t LastWrittenFrame = ~0ull;
        uint32_t OutWidth = 0; // display extent — the history texture's own extent
        uint32_t OutHeight = 0;
        uint32_t RenderWidth = 0;
        uint32_t RenderHeight = 0;
        uint64_t WorldId = 0;
        uint64_t RenderContentVersion = 0;
        uint64_t ShadowCasterContentVersion = 0;
        uint64_t LightListVersion = 0;
        uint64_t DepthDynamicEpoch = 0;
    };
    std::unordered_map<uint32_t, ViewHistory> m_ViewHistory;

};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
