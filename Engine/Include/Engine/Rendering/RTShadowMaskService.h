#pragma once

#include "Components/Rendering/PostProcessEffects/ShadowSettingsEffect.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "Engine/Rendering/SceneAccelerationStructureService.h"
#include "Rendering/Core/AccelerationStructure.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/ShaderVariantKey.h"

namespace GameEngine::Rendering
{
class GPUScene;
class IDevice;
struct CameraData;
struct ShaderMeta;
namespace RenderGraph
{
class RGFrame;
}
}  // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{
struct TerrainShadowMap;
class RenderServices;

// Ray-query shadow-mask prototype (DirectionalShadowMode::RayTraced, default
// Cascades): a client of the shared SceneAccelerationStructureService,
// holding its own TlasSlotHandle (a cast-shadow-only, sector-filtered
// instance set) over the service's shared BLAS pool. Rebuilt per frame from
// GPUScene's CPU instance mirror. While every world's mode is Cascades this
// service is never created and no pass is declared — the shipping path is
// untouched. It claims the shared pool (holds a TLAS channel) only on frames
// some world asks for RayTraced; the first frame none does, TickInactive
// releases the channel and its TLAS. The BLAS pool stays with the shared
// service, which keeps it through a quick re-enable (only the TLAS rebuilds)
// and purges it once no consumer has claimed it for a while — see
// SceneAccelerationStructureService::BeginFrame.
//
// Prototype scope (deliberate, per the shadow-future investigation):
//  - skinned meshes and instances excluded (no post-skin vertex buffer);
//    under the flag their sun shadow disappears — an enumerated A/B diff
//  - CBT terrain absent from the TLAS by construction (procedural, never in
//    GPUScene); its shadow comes from its clearance map (DeclareMaskPass)
//  - instances in a nonzero render-origin sector are skipped (counted,
//    warned once) until the mask pass picks its working space
//  - TLAS instance array is CPU-written v0; the compute writer over the
//    instance SSBO arrives with mover epochs (G2)
//  - BLAS compaction not yet implemented (tracked follow-up)
class RTShadowMaskService
{
  public:
    // `sceneAS` is owned by the caller (RenderServices) and must outlive
    // this service; it is shared with every other ray-query consumer.
    RTShadowMaskService(SceneAccelerationStructureService* sceneAS,
                        Rendering::GPUScene* gpuScene);
    ~RTShadowMaskService();
    RTShadowMaskService(const RTShadowMaskService&)            = delete;
    RTShadowMaskService& operator=(const RTShadowMaskService&) = delete;

    // Per-frame entry (owner spine, once per app frame, after the shared
    // pool's BeginFrame). Claims pending BLAS builds and, when caster content
    // changed, refreshes the TLAS instance array and declares the AS build
    // pass.
    //
    // Epoch gating (G1): `casterEpoch` is the combined
    // RenderServices::ShadowCasterContentVersion signal — the same input the
    // cascade static cache keys on. An unchanged epoch with no newly-ready
    // BLAS declares no pass at all (idle ASBuild -> 0). The signal
    // over-invalidates (any vertex-mod shadow caster bumps it every frame —
    // wind foliage), so bump frames additionally hash the filtered instance
    // records and skip the GPU build when the TLAS content would be
    // byte-identical; only a real caster transform/membership change rebuilds
    // (v1 granularity: any such change = full rebuild).
    void Schedule(Rendering::RenderGraph::RGFrame& frame, uint64_t casterEpoch);

    // Called once per app frame INSTEAD of Schedule while no world is in
    // RayTraced mode. The first such frame releases this service's TLAS
    // channel (its claim on the shared pool) and TLAS; after that it only
    // drains the retired instance-staging buffers, then is a two-branch no-op.
    // The next Schedule re-claims and rebuilds the TLAS through the normal
    // first-enable path.
    void TickInactive();

    // After an in-place device rebuild: the mask and terrain samplers and the
    // retired instance-staging buffers died with the old device, and the TLAS
    // slot the backend kept is empty. Forgets the handles without destroying
    // them and invalidates the TLAS content, so the next Schedule rebuilds it
    // and the next mask pass recreates the samplers. The channel stays claimed.
    void OnDeviceRebuilt();

    // True when the mask pass may consume the TLAS this frame: the CURRENT
    // TLAS object holds built content and has a device address. False during
    // bring-up, after a capacity recreate that has not rebuilt yet, and in
    // the blas-only fault-bisection lane.
    bool CanDeclareMaskPass() const;

    // Declare the per-view ray-query mask pass (G1): full-res R8 (1 = lit),
    // traced from the single-sample `depth` toward `lightDirTowardLightWS`
    // (normalized, surface->light). Returns the mask texture (persistent pool
    // import keyed on viewId) or invalid when the pass cannot run — callers
    // must then keep the cascade path (fail-visible fallback, never a dropped
    // shadow term). `cam` supplies proj/view for the reverse-Z LH
    // reconstruction; `maxShadowDistance` mirrors the cascade early-out.
    //
    // `lightAngularDiameterDegrees` is the authored Light::ShadowAngularDiameter
    // (full disc, degrees): the ray is cone-jittered inside its half-angle for
    // soft shadows, 0 = hard. `accumulatesHistory` is true only for a view whose
    // AA resolve integrates many frames (TAA); the per-frame cone jitter then
    // advances so TAA resolves it. Any other view keeps a frame-stable dither
    // that the spatial denoise resolves, so it never shimmers. `quality` picks
    // the ray budget: Performance = 1 ray + the full denoise footprint, Quality =
    // 4 rays on a per-pixel R2 sequence + half the footprint.
    //
    // `terrainShadow` (nullable) is the terrain's clearance map published for this view this
    // frame: the terrain is not in the TLAS, so each pixel also reads the map, and is lit only
    // where both see the sun. The map holds the sun's central direction, so the terrain's edge is
    // hard in this mode.
    Rendering::RenderGraph::RGTexture DeclareMaskPass(
        Rendering::RenderGraph::RGFrame& frame, uint32_t viewId,
        Rendering::RenderGraph::RGTexture depth, const Rendering::CameraData& cam,
        const float* lightDirTowardLightWS, float maxShadowDistance, float distanceFadeFraction,
        float lightAngularDiameterDegrees, bool accumulatesHistory,
        Components::RayTracedShadowQuality quality, const TerrainShadowMap* terrainShadow);

    // Resolve this view's authored settings and contribute the ray-traced term to its world pass.
    Rendering::MaterialKeyword ContributeWorldPass(
        RenderServices& services, Rendering::RenderGraph::RGFrame& frame, uint32_t viewId,
        Rendering::RenderGraph::RGTexture depth, const Rendering::CameraData* camera,
        bool canDeclare, bool transmissiveOnly, Rendering::RenderGraph::RGTexture& mask,
        Rendering::MaterialKeyword keywords);

    // Diagnostics for tests / status queries.
    uint32_t GetBlasCount() const;
    uint32_t GetLastTlasInstanceCount() const { return m_LastInstanceCount; }

  private:
    void LoadMaskShader();
    void LoadDenoiseShader();
    // Declare the plane-aware spatial denoise over `rawMask`, returning the
    // denoised R8 (per-view persistent import). `kernelRadius` is the half-width
    // in texels, bounded to the penumbra scale by the caller so a small-sun
    // penumbra is never fattened. Depth is the SAME single-sample depth the mask
    // pass traced from. Returns invalid on any setup failure (caller then keeps
    // the raw mask — never a dropped shadow term).
    Rendering::RenderGraph::RGTexture DeclareDenoisePass(
        Rendering::RenderGraph::RGFrame& frame, uint32_t viewId,
        Rendering::RenderGraph::RGTexture rawMask, Rendering::RenderGraph::RGTexture depth,
        const Rendering::CameraData& cam, int kernelRadius);
    // Advance this service's retired instance-staging drain. Exactly one call
    // per app frame, from Schedule or TickInactive (never both). The shared
    // backend's clock is the frame spine's, not this consumer's.
    void TickDeferredReclaim();
    void ReleaseAccelerationStructures();

    SceneAccelerationStructureService* m_SceneAS = nullptr;
    Rendering::GPUScene* m_GpuScene              = nullptr;
    Rendering::IAccelerationStructureBackend* m_Backend = nullptr;
    Rendering::IDevice* m_Device                 = nullptr;  // this consumer's OWN
                                                              // buffers/shaders/pipelines;
                                                              // see SceneAS::GetDevice
    Rendering::TlasSlotHandle m_TlasSlot;  // this consumer's cast-shadow-only TLAS

    // TLAS instance staging: a FRESH host-visible buffer every frame, retired
    // after the frames-in-flight margin. Never reused, so a hitching frame
    // can never observe a torn CPU rewrite (the fault class behind the
    // 2026-07-20 device loss during streaming load); DestroyBuffer's timeline
    // watermark is safely in the past by the time an entry retires.
    struct RetiredInstanceBuffer
    {
        Rendering::BufferHandle Buffer{};
        uint64_t FrameStamp = 0;
    };
    std::vector<RetiredInstanceBuffer> m_RetiredInstanceBuffers;
    uint64_t m_FrameClock = 0;

    uint32_t m_LastInstanceCount = 0;
    // Skip tripwires for the summary log (fail-visible, never silent) — this
    // consumer's own instance filter: nonzero render-origin sector, and
    // non-finite/astronomically-scaled instance TRANSFORMS (distinct from
    // the shared service's own BLAS triangle-geometry degenerate tripwire —
    // that one guards mesh index/vertex counts, this one guards per-instance
    // matrices before they become TLAS instance records).
    uint64_t m_SkippedSector     = 0;
    uint64_t m_SkippedDegenerate = 0;
    bool m_WarnedSector          = false;
    bool m_WarnedDegenerate      = false;
    bool m_LoggedSummary         = false;

    // ── Epoch gating (G1) ──
    // CPU-side instance records staged here every refresh frame; hashed so a
    // spurious epoch bump (vertex-mod caster wind) skips the GPU build when
    // nothing TLAS-relevant actually changed.
    std::vector<Rendering::TlasInstanceData> m_InstanceScratch;
    uint64_t m_LastCasterEpoch = 0;
    bool m_HaveCasterEpoch     = false;
    // Hash of the instance records the CURRENT TLAS was built from; valid only
    // while m_TlasContentValid (exec-confirmed — a declared build whose exec
    // never ran must rebuild, mirroring SceneAccelerationStructureService's
    // BuildConfirmToken discipline for BLAS).
    uint64_t m_BuiltInstanceHash = 0;
    bool m_TlasContentValid      = false;
    std::shared_ptr<std::atomic<bool>> m_TlasBuildExecuted;
    uint64_t m_PendingTlasHash = 0;

    // ── Mask pass (G1) ──
    bool m_MaskLoadAttempted = false;
    Rendering::ComputePipelineId m_MaskPipelineId{};
    std::unique_ptr<Rendering::ShaderMeta> m_MaskMeta;
    Rendering::DescriptorSetLayoutDesc m_MaskSet0Layout{};
    Rendering::SamplerHandle m_MaskSampler{};
    // Bilinear reads of the terrain's clearance map and height texture.
    Rendering::SamplerHandle m_TerrainSampler{};

    // ── Denoise pass (soft-shadow completion) ──
    // Plane-aware spatial blur that resolves the mask's per-frame stipple into a
    // smooth penumbra WITHOUT temporal accumulation (the Scene View has no TAA).
    // Reuses m_MaskSampler (point-clamp: the bilateral weights the taps itself).
    bool m_DenoiseLoadAttempted = false;
    Rendering::ComputePipelineId m_DenoisePipelineId{};
    std::unique_ptr<Rendering::ShaderMeta> m_DenoiseMeta;
    Rendering::DescriptorSetLayoutDesc m_DenoiseSet0Layout{};
};

}  // namespace GameEngine::Engine::Renderer
