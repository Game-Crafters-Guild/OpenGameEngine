#pragma once

#include "Components/Rendering/DDGIVolume.h"  // DDGIProbePlacement — one definition, shared with the component
#include "Engine/Rendering/DDGICameraIdleGate.h"
#include "Engine/Rendering/DDGISolveBudget.h"
#include "Engine/Rendering/DDGIProbeWindow.h"
#include "Engine/Rendering/DDGIAtlasUploadState.h"
#include "Engine/Rendering/DDGIEmitterAreas.h"
#include "Mathematics/Vector3.h"
#include "Engine/Rendering/DDGIGlossyAtlasLayout.h"
#include "Engine/Rendering/IRenderFeature.h"
#include "Engine/Rendering/SceneAccelerationStructureService.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/RenderGraph/RGReadbackRing.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine::Rendering
{
class IDevice;
class GPUScene;
class MeshGPURegistry;
struct GPUInstance;
struct ShaderMeta;
namespace RenderGraph
{
class RGFrame;
}
}  // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{

class RenderServices;
class MaterialSystem;
class DDGISceneService;
class DDGIMaterialMapAtlas;
class DDGISkinnedGeometry;

// Snapshot of the single active DDGIVolume component, pushed once per frame
// by DDGIVolumeSystem (component-only authoring: this feature does nothing —
// declares no passes, costs nothing — while no world has an enabled
// DDGIVolume). v1 limit: one volume, engine-wide (not per-world), axis-
// aligned; matches ImageBasedLightingFeature's own single-environment shape.
// A second concrete multi-world/rotated-volume need generalizes this later.
struct DDGIVolumeDesc
{
    bool Enabled = false;
    uint64_t WorldId = 0;  // whose RenderServices::GetWorldLights() feeds NEE
    float GridMinWS[3] = {0.0f, 0.0f, 0.0f};
    float GridSizeWS[3] = {1.0f, 1.0f, 1.0f};
    // Adaptive (default) = uniform lattice topology plus the per-probe
    // relocation/classification pass. Grid = the bare lattice, no classify
    // pass, probes never relocate or deactivate. See Components::DDGIVolume.
    // This default only applies to a desc nobody filled; the live value always
    // comes from the component via DDGIVolumeSystem::ResolveActiveVolume.
    Components::DDGIProbePlacement ProbePlacement = Components::DDGIProbePlacement::Adaptive;
    // Manual = GridMinWS is authored and final. FollowCamera = the feature
    // re-centres the grid on the active view each frame; GridSizeWS is
    // authored in BOTH modes.
    Components::DDGIVolumeFit Fit = Components::DDGIVolumeFit::Manual;
    // Only meaningful when Fit == FollowCamera, where they REPLACE the
    // transform as the source of the grid's size.
    float FollowRange = 16.0f;
    float FollowHeightFraction = 0.75f;
    float ClassifyStrength = 1.0f;  // clamped 0..1; 0 also skips the classify dispatch
    // Diagnostic overlay; evaluated in the resolve pass only (Components::DDGIDebugView).
    Components::DDGIDebugView DebugView = Components::DDGIDebugView::None;
    int32_t ProbesLongAxis = 12;   // clamped 2..32; per-axis counts derive from this + aspect
    int32_t RaysPerProbe = 64;     // clamped 32..256
    float Intensity = 1.0f;
    // Every field below mirrors the identically named Components::DDGIVolume
    // field, which carries the documentation; this struct is the extraction
    // snapshot, not a second source of truth.
    float BounceIntensity = 1.0f;
    float SkyIntensity = 1.0f;
    float RadianceClamp = 8.0f;
    // Gated holds the spherical-Fibonacci basis (shared.RayEpoch stays 0,
    // so classify/trace/blend all see rotation 0 — a converged field is a
    // fixed point and shows no temporal noise). MonteCarlo advances the basis
    // every tick for temporal decorrelation. See Components::DDGIJitterMode.
    Components::DDGIJitterMode JitterMode = Components::DDGIJitterMode::Gated;
    // Pairs with JitterMode, and the two are not independent: the reference
    // pairs a HELD basis with 0.60 and a rotating one with 0.90
    // (JITTER_HYSTERESIS_DEFAULTS, gi_probes.js:147). Running a rotating
    // basis at 0.60 retains too little history for the per-solve variance,
    // which surfaces as probe-scale irradiance blobs drifting over surfaces.
    float Hysteresis = 0.6f;
    float FireflyClamp = 6.0f;
    float ChangeThreshold = 2.5f;
    float SnapAmount = 0.30f;
    float NormalBiasScale = 1.75f;
    float ChebyshevStrength = 0.8f;
    float DepthSharpness = 1.0f;  // clamped 0.01..200
    // Shared = depth moments share the irradiance tile (one fused blend and
    // upload); Fine = their own 14x14 tile, two extra dispatches per cascade
    // per tick. See Components::DDGIDepthResolution.
    Components::DDGIDepthResolution DepthResolution = Components::DDGIDepthResolution::Shared;
    float FilterStrength = 1.0f;
    float FilterSmoothness = 0.5f;
    // False = hold the whole solve while any scene/game camera is moving.
    // Default true, mirroring Components::DDGIVolume — see its doc.
    bool ContinuousSolve = true;
    // Continuous = the full adaptive budget every tick; Throttle = a patrol
    // budget once the readback says the field has converged. See
    // Components::DDGIConvergedSolve.
    Components::DDGIConvergedSolve ConvergedSolve = Components::DDGIConvergedSolve::Continuous;
    // M5: optional second, finer cascade (C1) covering a smaller sub-volume
    // centered on the same point as the coarse grid (C0) — closer-range
    // indirect detail than C0's spacing alone gives. Disabled costs exactly
    // what M2-M4 already cost (C1 simply never allocates or dispatches).
    // Default true, mirroring Components::DDGIVolume — see its doc.
    bool EnableFineCascade = true;
    float FineCascadeExtentFraction = 0.35f;  // clamped 0.05..0.9 of C0's extents
    // Optional specular-lobe probe reflections (BOTH lobes — rough and
    // glossy). Each cascade bakes from its own traced rays when this is set
    // — see DDGIProbeFeature::EnsureReflectionResources. Disabled costs
    // exactly what the diffuse field alone costs (no reflection state, weight
    // or atlas is allocated, and no pass declared).
    bool EnableGlossy = false;
    float ReflectionIntensity = 1.0f;
    // Resolution of the per-view glossy-reflection resolve pass, as a
    // fraction of the render extent (Components::DDGIGlossyResolveScale's
    // doc). Full (the default, mirroring the component) keeps the inline
    // per-fragment gather.
    Components::DDGIGlossyResolveScale GlossyResolveScale =
        Components::DDGIGlossyResolveScale::Half;
};

// Engine-shared (world-space, view-independent — like ImageBasedLightingFeature,
// unlike the per-view ShadowMapRenderFeature) DDGI probe field: owns the ray/
// irradiance-state/depth-state/probe-state SSBOs, the irradiance + depth
// atlases, the shared-BLAS-pool TLAS channel it traces against, and the five
// compute kernels (classify/trace/blend/upload/clear). Classify/trace/blend
// cover a round-robin window of the grid each tick (M4 budget); the shared
// ray set is held fixed (DDGIJitterMode::Gated, flicker-free) or rotated each
// tick for temporal decorrelation (MonteCarlo). Upload updates that window,
// preserving untouched atlas tiles; allocation, scrolling and filter edits
// refresh the diffuse/depth atlas in full. The TLAS instance list only rebuilds
// on a GPUScene content-epoch change or a newly-ready BLAS (M4 reactivity).
//
// M5 adds an optional second cascade (C1, finer/smaller) alongside the
// original grid (C0, coarse/full-volume) — see CascadeGrid's doc. Both
// cascades trace against the SAME scene TLAS (one geometry, one epoch/hash/
// exec-confirm state — see the TLAS members below, which stay single/shared,
// not per-cascade) and share the atlas sampler; only the per-cascade probe
// grid (buffers, atlases, idle-gate, round-robin cursor) is duplicated.
//
// C0's rays additionally bake two specular reflection lobes (rough and
// glossy) when EnableGlossy is set, and C1 does the same from its own rays
// when the fine cascade is on — see EnsureReflectionResources. See
// Includes/ddgi_common.glsl for the shared GPU-side grid math this mirrors.
class DDGIProbeFeature final : public IRenderFeature
{
  public:
    DDGIProbeFeature();
    ~DDGIProbeFeature() override;
    DDGIProbeFeature(const DDGIProbeFeature&) = delete;
    DDGIProbeFeature& operator=(const DDGIProbeFeature&) = delete;

    // `sceneAS` is null on a non-ray-query device (DDGINode's own doc on why
    // it still calls Initialize rather than declining outright — M7 gives
    // DDGI a fallback the shadow-mask lane doesn't have). `meshRegistry`/
    // `materials` feed the software lane's own DDGISceneService and are
    // required regardless of which lane ends up active, since the active
    // lane is a per-tick device-capability check (DeclareProbePasses), not a
    // per-initialize one — a hot-pluggable GPU or a forced A/B toggle must be
    // able to switch lanes without a full feature re-Initialize.
    bool Initialize(Rendering::IDevice* device, SceneAccelerationStructureService* sceneAS,
                    Rendering::GPUScene* gpuScene, Rendering::MeshGPURegistry* meshRegistry,
                    MaterialSystem* materials);
    bool IsInitialized() const { return m_Device != nullptr; }

    void OnDeviceRebuilt(Rendering::IDevice* device) override;

    // Convergence readback (DDGIConvergedSolve::Throttle) rides the generic
    // RGReadbackRing completion contract: the frame's submission token stamps
    // this tick's reduction result, and a dying frame stream purges its
    // unstamped pendings.
    void OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                            const Rendering::IDevice::GpuSyncToken& token) override;
    void OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame) override;

    // Pushed once per frame by DDGIVolumeSystem, BEFORE Declare (DDGINode's
    // frame-scope Declare runs after the Camera-phase ECS systems — see
    // RegisterRenderingSystems.cpp's phase ordering). Resizing/re-enabling
    // arms a clear on the NEXT Declare.
    void SetActiveVolume(const DDGIVolumeDesc& desc);
    const DDGIVolumeDesc& GetActiveVolume() const { return m_Volume; }
    // GpuGridValid alone means the buffers were ALLOCATED, not that the
    // atlas has ever actually been written — see CascadeGrid::AtlasEverUploaded's
    // doc for the M7 gap this closes (the software lane can leave a grid
    // allocated-but-never-cleared for an arbitrary number of ticks while its
    // own scene data is still resolving).
    bool HasConvergedVolume() const
    {
        return m_Volume.Enabled && m_C0.GpuGridValid && m_C0.AtlasEverUploaded;
    }
    // C1 converges independently of C0 (its own allocation/clear cycle) —
    // the consumer must gate its C1 sample on this, not just EnableFineCascade,
    // since a just-(re)sized C1 grid is momentarily invalid the same way C0 is.
    bool HasConvergedFineCascade() const
    {
        return m_Volume.Enabled && m_Volume.EnableFineCascade && m_C1.GpuGridValid &&
              m_C1.AtlasEverUploaded;
    }

    // Emits this frame's compute passes (clear-if-needed, TLAS rebuild-if-
    // needed, classify, trace, blend, upload — for C0, and for C1 too when
    // EnableFineCascade) via `frame`. No-ops (declares nothing) while no
    // world has an enabled DDGIVolume — component-only authoring means zero
    // cost, not just zero visual effect, when absent. Needs `services` (not
    // just the device/GPUScene from Initialize) because the light buffer
    // this feature's NEE reads is published PER-VIEW by LightUploadNode
    // (perView=true) — a frame-scope DDGI declaration has no blackboard
    // access to it, so DDGI packs its OWN compact light buffer directly from
    // RenderServices::GetWorldLights(m_Volume.WorldId), same CPU source
    // LightUploadNode itself packs from (see the .cpp for why the two
    // packers must byte-match Includes/light_packed_fields.glsl). Also
    // supplies the MaterialParamsSSBO (Materials().PackedMaterialParams())
    // and the sky irradiance cube (GetFeature<ImageBasedLightingFeature>()).
    //
    // Named DeclareProbePasses rather than Declare on purpose: its signature
    // does not match IRenderFeature::Declare's (this needs RenderServices&
    // directly, not a FeatureDeclareContext built for the per-view shadow
    // case), so reusing the name would silently HIDE the virtual overload
    // instead of overriding it (-Woverloaded-virtual) — the same bespoke-
    // named-method shape ImageBasedLightingFeature already uses (UploadEnvData,
    // ScheduleBrdfLutBake, ...) instead of forcing everything through the
    // generic virtual.
    // `anyBlasBecameReady`: SceneAccelerationStructureService::
    // BlasBecameReadyThisFrame(), so a just-readied mesh's instances join the
    // TLAS this frame without waiting for an unrelated GPUScene content-epoch
    // bump. The hardware lane claims the shared pool (a TLAS channel) only on
    // frames it traces: a disabled volume or the software lane releases it.
    void DeclareProbePasses(Rendering::RenderGraph::RGFrame& frame, RenderServices& services,
                            float deltaTimeSeconds, bool anyBlasBecameReady);

    // Resources the world pass binds by reflected name under the DDGI
    // keyword (Includes/ddgi_probes.glsl: ge_ddgiIrradianceAtlas b29,
    // DDGIVolumeData b30, ge_ddgiDepthAtlas b31, DDGIProbeStateRO b32).
    // Uploads this frame's volume-data UBO element and returns the handle to
    // bind — mirrors ImageBasedLightingFeature::UploadEnvData.
    Rendering::TextureHandle GetIrradianceAtlas() const { return m_C0.IrradianceAtlas; }
    Rendering::SamplerHandle GetAtlasSampler() const { return m_AtlasSampler; }
    Rendering::BufferHandle UploadVolumeData(Rendering::IDevice* device);
    Rendering::TextureHandle GetDepthAtlas() const { return m_C0.DepthAtlas; }

    // One cascade's probe-state SSBO and the byte range to bind, resolved
    // together so a caller can never pair a fallback buffer with the real
    // grid's size. Buffer is ALWAYS valid once initialized — see
    // GetProbeStateBinding.
    struct ProbeStateBinding
    {
        Rendering::BufferHandle Buffer;
        uint64_t Bytes;
    };

    // The DDGI keyword's descriptor layout declares b32/b36 unconditionally
    // (Includes/ddgi_probes.glsl), and MaterialBinder DROPS any draw whose
    // storage buffer resolves to no provider — so an unbound probe-state SSBO
    // is a world pass that renders nothing at all, not a dimmer frame. These
    // therefore never return an invalid handle: a cascade with no grid
    // allocated (C1 while EnableFineCascade is off, either cascade mid-resize)
    // resolves to a shared all-zero stand-in. Its contents are never sampled —
    // the same GpuGridValid that leaves the grid unallocated also drives the
    // matching UBO's uParams1.x to 0, which is the gate GE_DDGISampleC0/C1
    // check before indexing — and all-zero reads as "probe inactive" anyway,
    // so a future shader that dropped that gate degrades to black rather than
    // to garbage.
    ProbeStateBinding GetProbeStateBinding();

    // M5 fine-cascade bindings (b33 atlas, b34 UBO, b35 depth atlas, b36
    // probe state — see Includes/ddgi_probes.glsl). Only meaningful under
    // HasConvergedFineCascade(); the world pass binds the same
    // black-texture/zero-weighted-UBO/zero-probe-state fallbacks C0 uses when
    // not converged.
    Rendering::TextureHandle GetIrradianceAtlasFine() const { return m_C1.IrradianceAtlas; }
    Rendering::TextureHandle GetDepthAtlasFine() const { return m_C1.DepthAtlas; }
    ProbeStateBinding GetProbeStateBindingFine();
    Rendering::BufferHandle UploadVolumeDataFine(Rendering::IDevice* device);

    // Reflection-lobe bindings (b37/b38 C0, b39/b40 C1 — see
    // Includes/ddgi_probes.glsl). Only meaningful when the matching cascade's
    // UBO uParams1.z > 0.5 (HasConvergedVolume/Fine && EnableGlossy). The two
    // atlases are packed DIFFERENTLY: rough shares the probe-XYZ z-major tile
    // layout, glossy is near-square. A consumer must address the glossy one
    // through GetGlossyAtlasLayout() / GetGlossyAtlasLayoutFine(), never the
    // grid's probe counts.
    Rendering::TextureHandle GetRoughAtlas() const { return m_C0Refl.RoughAtlas; }
    Rendering::TextureHandle GetGlossyAtlas() const { return m_C0Refl.GlossyAtlas; }
    DDGIGlossyAtlasLayout GetGlossyAtlasLayout() const { return m_C0Refl.GlossyLayout; }
    Rendering::TextureHandle GetRoughAtlasFine() const { return m_C1Refl.RoughAtlas; }
    Rendering::TextureHandle GetGlossyAtlasFine() const { return m_C1Refl.GlossyAtlas; }
    DDGIGlossyAtlasLayout GetGlossyAtlasLayoutFine() const { return m_C1Refl.GlossyLayout; }

    // The atlases and probe-state buffers the DDGI gather (Includes/ddgi_probes.glsl)
    // samples, imported into `frame` for a consumer pass to declare as reads: DDGIGen's
    // passes write them earlier in the frame, and only a declared read orders the
    // consumer after them. Imports dedup by physical, so each entry is the resource its
    // writer declared. A resource that is not allocated stays an invalid entry: the
    // black texture or zero probe-state stand-in bound in its place has no writer.
    struct GatherReadsRG
    {
        Rendering::RenderGraph::RGTexture Atlases[8];
        Rendering::RenderGraph::RGBuffer ProbeStates[2];
    };
    GatherReadsRG ImportGatherReads(Rendering::RenderGraph::RGFrame& frame) const;

    // Per-view scaled glossy-reflection resolve (Shaders/ddgi_glossy_resolve.comp,
    // consumed by Includes/ibl.glsl): declares one compute pass that evaluates
    // the probe reflection gather at GlossyResolveScale x the view's render
    // extent from the 1-sample view depth, writing one RGBA16F texture per
    // baked lobe (coverage-premultiplied radiance in rgb, coverage in a).
    // Returns true and fills outRough/outGlossy when the pass was declared;
    // false (and declares nothing) when the resolve is inactive for this
    // frame (GlossyResolveConsumeActive()) or the view cannot feed it (no
    // 1-sample depth, zero extent). Callers bind the outputs as
    // ge_ddgiResolveRough/ge_ddgiResolveGlossy (set 0, b41/b42) on the same
    // view's DDGI-keyword world passes.
    //
    // The reflection direction takes the SHADING normal from the previous
    // frame's forward-MRT normal slice (Adapters/adapter_forward.glsl under
    // MaterialKeyword::SSSRNormalRoughness), reprojected; this pass runs
    // before the world pass that would write this frame's. outShadingNormal
    // is the per-view persistent render target the caller must hand to the
    // world pass as that slice (ReflectionsProvider::ContributeWorldPassTargets)
    // and, when the pass actually attached it, report back through
    // MarkGlossyResolveNormalWritten — the kernel reads the history only for
    // a frame that follows a reported write. Implemented in DDGIGlossyResolve.cpp.
    bool DeclareGlossyResolveForView(Rendering::RenderGraph::RGFrame& frame,
                                     RenderServices& services,
                                     Rendering::BufferHandle viewParams, uint64_t viewParamsOffset,
                                     uint64_t viewParamsSize,
                                     Rendering::RenderGraph::RGTexture viewDepth,
                                     uint32_t renderWidth, uint32_t renderHeight,
                                     uint32_t viewId, float viewExposureScale,
                                     const char* passName,
                                     Rendering::RenderGraph::RGTexture& outRough,
                                     Rendering::RenderGraph::RGTexture& outGlossy,
                                     Rendering::RenderGraph::RGTexture& outIrradiance,
                                     Rendering::RenderGraph::RGTexture& outShadingNormal);

    // The view's world pass attached this frame's outShadingNormal as its
    // normal MRT slice, so next frame's resolve may read it as history.
    void MarkGlossyResolveNormalWritten(uint32_t viewId, uint64_t frameIndex);

    // Whether this frame's forward consume reads the scaled resolve textures
    // instead of running the inline gather — the value UploadVolumeData
    // writes into the C0 UBO's uParams2.w. Frame-coherent by construction
    // (volume state + kernel availability only, nothing per view or per
    // call): every UBO ring slot written in a frame carries the same value,
    // so ring-slot reuse across the frame's world/recover/transmissive/
    // resolve uploads can never bind a pass to the wrong flag.
    // Kernel availability only — NOT EnableGlossy. The resolve carries the
    // diffuse irradiance as well as the specular lobes, and ibl.glsl reads
    // diffuse from it whenever this flag is set, so tying the flag to the
    // specular toggle silently removed probe diffuse from every volume that
    // left EnableGlossy at its default.
    bool GlossyResolveConsumeActive() const
    {
        // Consumed whenever the resolve pipelines are valid. The pass is no longer
        // EnableGlossy-gated (it also resolves diffuse irradiance, the only DDGI
        // diffuse path when the forward shader carries no inline gather), so this
        // read follows the pass rather than the specular toggle.
        return m_GlossyResolve.PipelineId.IsValid() && m_GlossyResolveBlur.PipelineId.IsValid();
    }

    // Whether the resolve's rough/glossy targets carry reflection lobes this
    // frame. Inactive bindings alias irradiance on native devices or use
    // declared placeholders where aliases are restricted; neither is a reflection.
    bool GlossyLobesActive() const { return m_Volume.EnableGlossy && m_C0Refl.GpuValid; }

    // Per-axis probe counts a grid with world-space size `gridSizeWS` and
    // `probesLongAxis` probes along its longest axis resolves to (longest
    // axis gets probesLongAxis, the other two scale by aspect ratio, each
    // clamped 2..32) — the same derivation EnsureCascadeGpuResources uses to
    // size the GPU grid. Exposed statically so editor-side gizmos can
    // preview the exact probe layout without a live feature instance.
    static void ComputeProbeGridLayout(const float gridSizeWS[3], int32_t probesLongAxis,
                                       int32_t outCounts[3]);

    // World-space half-extents of a DDGIVolume whose entity has the given
    // column-major WorldTransform. The volume is a unit cube scaled by the
    // transform (DDGIVolume.h's convention, shared with PostProcessVolume),
    // so each half-extent is half the length of the corresponding basis
    // column — which extracts scale correctly whether or not the transform
    // also carries rotation, and yields the world-axis-aligned box v1
    // samples. Exposed statically for the same reason as
    // ComputeProbeGridLayout: the editor gizmo must draw exactly the box the
    // extraction system feeds the GPU, and one derivation cannot disagree
    // with itself. Degenerate (zero/negative) scale clamps to a small
    // positive extent so a collapsed transform cannot produce an empty or
    // inverted grid.
    static void ComputeVolumeHalfExtents(const float worldMatrix[16], float outHalfExtents[3]);

    // Centre for a DDGIVolumeFit::FollowCamera volume: `cameraWS` quantised to
    // whole probe cells of the grid `gridSizeWS`/`probesLongAxis` describes.
    //
    // Snapping is what makes a following grid affordable. An unsnapped centre
    // moves every probe every frame, so no probe's history is ever valid and
    // the field can never converge; snapped, sub-cell camera motion moves no
    // probe at all and a cell-crossing moves them by exactly one cell, which
    // is the step the scroll logic can carry history across.
    //
    // Exposed statically for the same reason as the two above: the editor
    // gizmo must draw the box the feature actually samples, and a followed
    // volume's box is not the one on the entity's transform.
    // `gridSizeWS` stays a raw array because it is read straight off
    // DDGIVolumeDesc, whose layout mirrors the GPU-side UBO.
    static Mathematics::Vector3 SnapCentreToProbeGrid(const Mathematics::Vector3& cameraWS,
                                                      const float gridSizeWS[3],
                                                      int32_t probesLongAxis);

  private:
    // Both cascade UBOs share the layout owned by this feature.
    static size_t GetVolumeDataSize();

    struct KernelPipeline
    {
        Rendering::ComputePipelineId PipelineId{};
        std::unique_ptr<Rendering::ShaderMeta> Meta;
        Rendering::DescriptorSetLayoutDesc Set0Layout{};
        bool LoadAttempted = false;
    };
    bool LoadKernel(const char* shaderPkgPath, KernelPipeline& out, const char* debugName);
    void LoadKernelsIfNeeded();

    // One probe grid's full GPU-side state — everything M2-M4 originally
    // kept as flat DDGIProbeFeature members, now duplicated per cascade (M5:
    // m_C0 = coarse/full-volume, m_C1 = optional fine/sub-volume). What is
    // NOT here: the TLAS/BLAS/scene-AS state (single, shared — both cascades
    // trace the same scene) and the atlas sampler (one sampler serves any
    // atlas). See EnsureCascadeGpuResources/ReleaseCascadeGpuResources/
    // DeclareCascadeGridPasses, the parameterized versions of what M2-M4
    // wrote directly against `this`.
    struct CascadeGrid
    {
        int32_t ProbeCount[3] = {0, 0, 0};
        int32_t ProbeTotal = 0;
        int32_t AllocatedRaysPerProbe = 0;
        // Depth-moment tile edge in texels, border included: kTile (8) under
        // DDGIDepthResolution::Shared, kDepthTileFine (16) under Fine. Sizes
        // DepthState and DepthAtlas; the kernels receive the interior
        // resolution (tile - 2) and address the atlas through
        // GE_DDGIDepthTexelUV.
        int32_t DepthTile = 0;
        float MinCellWS = 0.0f;
        int32_t AtlasWidth = 0;
        int32_t AtlasHeight = 0;

        // Structural-resize idle gate (per cascade — C0 and C1 can be
        // dragged/resized independently in principle, though v1 derives
        // both from the same DDGIVolumeDesc edit).
        int32_t LastRequestedProbeCount[3] = {-1, -1, -1};
        int32_t LastRequestedRaysPerProbe = -1;
        int32_t LastRequestedDepthTile = -1;
        float StructuralIdleTimerMs = 0.0f;

        Rendering::BufferHandle RayBuffer{};
        Rendering::BufferHandle IrradianceState{};
        Rendering::TextureHandle IrradianceAtlas{};
        Rendering::BufferHandle DepthState{};
        Rendering::TextureHandle DepthAtlas{};
        // One float per irradiance texel: the blend kernel's steady-tick
        // count (its running-mean history), reduced by ddgi_variability.comp
        // into the convergence readback.
        Rendering::BufferHandle TemporalState{};
        Rendering::BufferHandle ProbeStateBuffer{};
        // One vec4 per probe: xyz = the world cell the slot's history belongs
        // to (Includes/ddgi_common.glsl's GE_DDGIProbeCellId). A scrolled
        // followed grid hands a slot to a new cell; the kernels compare this
        // record to reset the inherited history, and the blend re-stamps it.
        Rendering::BufferHandle ProbeCellBuffer{};

        bool NeedsClear = true;
        bool GpuGridValid = false;  // false until the first successful clear+resource alloc
        // M7: false until this cascade's Upload dispatch has actually been
        // DECLARED at least once. GpuGridValid alone is not enough to trust
        // the atlas texture — it only means the buffers were ALLOCATED, not
        // that anything has ever been written into them. On the hardware
        // lane this was a non-issue (clear+upload always declared in the
        // SAME tick GpuGridValid went true, before M7 existed). The software
        // lane's own scene-readiness gate (DeclareProbePasses) can return
        // before DeclareCascadeGridPasses ever runs, for an arbitrary number
        // of ticks, while GpuGridValid stays true — HasConvergedVolume() (and
        // HasConvergedFineCascade()) must gate on this too, or the world pass
        // starts sampling the atlas texture while it is still whatever the
        // GPU allocator happened to leave in that memory, not the safe
        // all-zero state Clear would have produced.
        bool AtlasEverUploaded = false;
        DDGIAtlasUploadState AtlasUploadState;
        uint32_t ProbeCursor = 0;   // M4 round-robin budget cursor

        // Volume-data UBO ring (dynamic scalars — Intensity/Hysteresis
        // live-edit without a grid rebuild), mirrors
        // ImageBasedLightingFeature's EnvData ring.
        static constexpr uint32_t kVolumeDataFrameSlots = 3;
        Rendering::BufferHandle VolumeDataBuffer[kVolumeDataFrameSlots]{};
        uint32_t VolumeDataCursor = 0;
    };

    static constexpr float kStructuralIdleGateMs = 200.0f;
    static uint64_t CascadeProbeStateBytes(const CascadeGrid& grid)
    {
        return static_cast<uint64_t>(grid.ProbeTotal) * sizeof(float) * 4;
    }

    // One probe's worth of state (vec4) — the whole size of the stand-in
    // buffer m_ProbeStateFallback, which exists to be bindable, not to be read.
    static constexpr uint64_t kFallbackProbeStateBytes = sizeof(float) * 4;

    // Shared by both cascades: only one can ever be sampled at a time, and
    // neither samples the fallback. See GetProbeStateBinding's doc.
    ProbeStateBinding ResolveProbeStateBinding(const CascadeGrid& grid);

    // Static-literal debug names for one cascade's GPU resources/passes.
    // Plain `const char*` fields deliberately: RGPassDesc::Name and
    // BufferDesc/TextureDesc::debugName are all raw `const char*` with no
    // copy/ownership guarantee — a dynamically-built std::string's c_str()
    // would dangle once its backing storage is destroyed, which for a pass
    // name happens well before the render graph actually executes that pass.
    // kC0DebugNames/kC1DebugNames below are the only two instances; every
    // field must be a string literal (static storage), never computed.
    struct CascadeDebugNames
    {
        const char* Prefix;
        const char* RayBuffer;
        const char* IrradianceState;
        const char* DepthState;
        const char* TemporalState;
        const char* ProbeState;
        const char* ProbeCell;
        const char* IrradianceAtlas;
        const char* DepthAtlas;
        const char* ClearIrradiance;
        const char* ClearDepth;
        const char* ClearTemporal;
        const char* ClearProbeState;
        const char* ClearProbeCell;
        const char* ClearRayBuffer;
        const char* Classify;
        const char* ClassifySW;
        const char* TraceHW;
        const char* TraceSW;
        const char* Blend;
        const char* DepthBlend;
        const char* Upload;
        const char* DepthUpload;
        const char* Variability;
        const char* RoughBlend;
        const char* RoughUpload;
        const char* GlossyBlend;
        const char* GlossyUpload;
        const char* ClearRough;
        const char* ClearGlossyNum;
        const char* ClearGlossyWeight;
        const char* RoughState;
        const char* GlossyNumerator;
        const char* GlossyWeight;
        const char* RoughAtlas;
        const char* GlossyAtlas;
    };
    static constexpr CascadeDebugNames kC0DebugNames{
        "DDGI.C0",           "DDGI.C0.RayBuffer",        "DDGI.C0.IrradianceState",
        "DDGI.C0.DepthState", "DDGI.C0.TemporalState",   "DDGI.C0.ProbeState",
        "DDGI.C0.ProbeCell",  "DDGI.C0.IrradianceAtlas",
        "DDGI.C0.DepthAtlas", "DDGI.C0.Clear.Irradiance", "DDGI.C0.Clear.Depth",
        "DDGI.C0.Clear.Temporal",
        "DDGI.C0.Clear.ProbeState", "DDGI.C0.Clear.ProbeCell", "DDGI.C0.Clear.RayBuffer", "DDGI.C0.Classify",
        "DDGI.C0.ClassifySW", "DDGI.C0.TraceHW",   "DDGI.C0.TraceSW",          "DDGI.C0.Blend",
        "DDGI.C0.DepthBlend", "DDGI.C0.Upload", "DDGI.C0.DepthUpload", "DDGI.C0.Variability",
        "DDGI.C0.RoughBlend", "DDGI.C0.RoughUpload",
        "DDGI.C0.GlossyBlend", "DDGI.C0.GlossyUpload", "DDGI.C0.Clear.Rough",
        "DDGI.C0.Clear.GlossyNum", "DDGI.C0.Clear.GlossyWeight",
        "DDGI.C0.RoughState", "DDGI.C0.GlossyNumerator", "DDGI.C0.GlossyWeight",
        "DDGI.C0.RoughAtlas", "DDGI.C0.GlossyAtlas"};
    static constexpr CascadeDebugNames kC1DebugNames{
        "DDGI.C1",           "DDGI.C1.RayBuffer",        "DDGI.C1.IrradianceState",
        "DDGI.C1.DepthState", "DDGI.C1.TemporalState",   "DDGI.C1.ProbeState",
        "DDGI.C1.ProbeCell",  "DDGI.C1.IrradianceAtlas",
        "DDGI.C1.DepthAtlas", "DDGI.C1.Clear.Irradiance", "DDGI.C1.Clear.Depth",
        "DDGI.C1.Clear.Temporal",
        "DDGI.C1.Clear.ProbeState", "DDGI.C1.Clear.ProbeCell", "DDGI.C1.Clear.RayBuffer", "DDGI.C1.Classify",
        "DDGI.C1.ClassifySW", "DDGI.C1.TraceHW",   "DDGI.C1.TraceSW",          "DDGI.C1.Blend",
        "DDGI.C1.DepthBlend", "DDGI.C1.Upload", "DDGI.C1.DepthUpload", "DDGI.C1.Variability",
        "DDGI.C1.RoughBlend", "DDGI.C1.RoughUpload",
        "DDGI.C1.GlossyBlend", "DDGI.C1.GlossyUpload", "DDGI.C1.Clear.Rough",
        "DDGI.C1.Clear.GlossyNum", "DDGI.C1.Clear.GlossyWeight",
        "DDGI.C1.RoughState", "DDGI.C1.GlossyNumerator", "DDGI.C1.GlossyWeight",
        "DDGI.C1.RoughAtlas", "DDGI.C1.GlossyAtlas"};

    // Recomputes probe counts/grid for ONE cascade from the given world-
    // space size/longest-axis-probe-count/ray-count and (re)allocates its
    // GPU buffers/atlases when they changed AND the request has been stable
    // (idle) for kStructuralIdleGateMs — a continuous inspector drag on
    // ProbesLongAxis/RaysPerProbe/Extents re-requests a new layout every
    // frame, but the actual (expensive) buffer/texture teardown+recreate
    // only fires once dragging stops. `names` distinguishes C0 vs C1
    // resources in GPU debuggers — pass kC0DebugNames or kC1DebugNames.
    // `depthTile` is the depth-moment tile edge the volume's DepthResolution
    // resolves to this tick (kTile or kDepthTileFine); a change reallocates
    // through the same idle gate a probe-count change does.
    bool EnsureCascadeGpuResources(CascadeGrid& grid, const float gridSizeWS[3], int32_t probesLongAxis,
                                   int32_t rawRaysPerProbe, int32_t depthTile, float deltaTimeSeconds,
                                   const CascadeDebugNames& names);
    void ReleaseCascadeGpuResources(CascadeGrid& grid);

    // Dispatches ddgi_clear.comp against `target`, filling every vec4
    // element with `fillValue`. Shared by the irradiance-state, depth-state,
    // and probe-state clears — see ddgi_clear.comp's doc on why one generic
    // kernel serves all three.
    void DispatchClear(Rendering::RenderGraph::RGFrame& frame, Rendering::BufferHandle target,
                       uint32_t elementCount, const float fillValue[4], const char* passName);

    // Shared implementation behind UploadVolumeData/UploadVolumeDataFine —
    // uploads ONE cascade's DDGIVolumeData UBO element (Includes/ddgi_probes.glsl)
    // from `grid`'s resolved probe count/cell size plus the given world-space
    // placement. A member function (not an anonymous-namespace free function
    // in the .cpp) because CascadeGrid is private.
    Rendering::BufferHandle UploadCascadeVolumeData(CascadeGrid& grid, const float gridMinWS[3],
                                                     const float gridSizeWS[3], float normalBiasScale,
                                                     float intensity, bool convergedEnabled,
                                                     bool glossyEnabled, float reflectionIntensity,
                                                     const DDGIGlossyAtlasLayout& glossyLayout,
                                                     const char* debugName);

    // Everything shared across BOTH cascades this tick: the light buffer
    // (packed once, read by both cascades' trace), material params, sky
    // cube and mesh-geometry/instance buffers — computed once in
    // DeclareProbePasses and passed to DeclareCascadeGridPasses for each
    // cascade so the light-packing loop does not run twice per frame. The
    // TLAS is not here: both hardware kernels bind m_TlasSlot as a descriptor
    // (DescriptorType::AccelerationStructure), so nothing per-tick is derived
    // from it.
    struct SharedTickInputs
    {
        Rendering::BufferHandle LightBuffer{};
        uint64_t LightOffset = 0;
        uint64_t LightBytes = 0;
        uint32_t LightCount = 0;
        Rendering::BufferHandle MaterialParamsBuffer{};
        uint64_t MaterialParamsOffset = 0;
        uint64_t MaterialParamsSize = 0;
        Rendering::TextureHandle SkyIrradiance{};
        Rendering::SamplerHandle SkySampler{};
        // What a miss ray's sky sample is multiplied by: the volume's own
        // SkyIntensity times the scene environment's global IblIntensity.
        // The cube itself is unscaled (ibl.glsl folds the global intensity at
        // sample time, not into the convolution), and DDGI's sample REPLACES
        // the forward path's GE_SampleEnvironmentIrradiance, so folding it
        // here is what keeps the global sky slider live for GI at all.
        float SkyIntensityScale = 1.0f;
        // Shared by BOTH lanes: the material map atlas both trace kernels
        // shade their flat base-colour and emissive factors with. MapTable is
        // the hardware lane's per-material (layers, uv transform) lookup; the
        // software lane carries the same values inside its packed uber-material
        // records.
        Rendering::TextureHandle MapAtlas{};
        Rendering::SamplerHandle MapSampler{};
        Rendering::BufferHandle MapTable{};
        uint64_t MapTableBytes = 0;
        Rendering::BufferHandle GpuInstanceBuffer{};
        uint64_t GpuInstanceBytes = 0;
        Rendering::BufferHandle MeshGeomBuffer{};
        uint64_t MeshGeomBytes = 0;
        // Sampling epoch feeding the classify/trace/blend ray-set rotation
        // (GE_DDGIRayBasis). Held at 0 under DDGIJitterMode::Gated —
        // the fixed ray set — and advances once per tick under MonteCarlo.
        uint64_t RayEpoch = 0;
        float TickTimeMs = 0.0f;
        // This tick's adaptive ray budget (DDGISolveBudget::RaysPerTick),
        // resolved once per feature tick so both cascades derive their
        // round-robin windows from the same number.
        uint32_t RaysPerTickBudget = 0;
        // Skinned per-instance geometry-row override for the trace kernel
        // (DDGISkinnedGeometry's row map + posed rows; a 1-element
        // no-override dummy when no skinned instance is in the TLAS).
        Rendering::BufferHandle SkinnedRowMapBuffer{};
        uint64_t SkinnedRowMapOffset = 0;
        uint64_t SkinnedRowMapBytes = 0;
        Rendering::BufferHandle SkinnedGeomRowsBuffer{};
        uint64_t SkinnedGeomRowsOffset = 0;
        uint64_t SkinnedGeomRowsBytes = 0;
        // Software lane only: RG imports of SwNodes/SwVertexData, valid on a
        // tick that declared the skinned BVH refit — the classify/trace
        // passes declare Reads on them so the graph orders refit writes
        // before every traversal of the pooled tree.
        Rendering::RenderGraph::RGBuffer SwNodesRG{};
        Rendering::RenderGraph::RGBuffer SwPackedSceneRG{};
        Rendering::RenderGraph::RGBuffer SwVertexDataRG{};

        // M7: which trace kernel/bindings DeclareCascadeGridPasses should
        // use this tick — decided once in DeclareProbePasses (device
        // capability + force-software override), not per cascade, so C0/C1
        // never disagree within one tick. false selects every field above
        // (GpuInstanceBuffer/MeshGeomBuffer, the hardware lane's
        // ddgi_trace_hw.comp bindings); true selects the five below instead
        // (the software lane's ddgi_trace_sw.comp bindings, which the
        // software classify kernel also reads). Both lanes classify when the
        // volume asks for adaptive placement — the lane only picks WHICH
        // classify kernel (ddgi_classify.comp vs ddgi_classify_sw.comp).
        bool UseSoftwareLane = false;
        Rendering::BufferHandle SwPackedScene{};
        Rendering::BufferHandle SwNodes{};
        Rendering::BufferHandle SwTriangleIndices{};
        Rendering::BufferHandle SwTriangleMaterials{};
        Rendering::BufferHandle SwVertexData{};
        uint64_t SwPackedSceneBytes = 0;
        uint64_t SwNodesBytes = 0;
        uint64_t SwTriangleIndicesBytes = 0;
        uint64_t SwTriangleMaterialsBytes = 0;
        uint64_t SwVertexDataBytes = 0;
        uint32_t SwTlasNodeCount = 0;
        uint32_t SwInstanceBase = 0;
        uint32_t SwTlasBase = 0;
    };

    // Emits classify+trace+blend+upload for ONE cascade's grid, budget-
    // windowed per M4's round-robin policy. `gridMinWS`/`gridSizeWS` are
    // THIS cascade's placement (C0: the volume's own; C1: a smaller
    // sub-region — see DeclareProbePasses). Declares whatever passes this
    // cascade's tick needs, or none if `grid` isn't GPU-valid yet.
    // `outProbeBaseThisTick`/`outProbesThisTick` (both nullable) report the
    // round-robin window this tick actually used, so a caller needing to
    // piggyback another gather on the SAME just-traced ray buffer (reflection
    // blend, per cascade) can reuse it instead of re-deriving its own
    // window against a cascade's cursor that DeclareCascadeGridPasses has
    // already advanced.
    void DeclareCascadeGridPasses(Rendering::RenderGraph::RGFrame& frame, CascadeGrid& grid,
                                  const float gridMinWS[3], const float gridSizeWS[3],
                                  const SharedTickInputs& shared, const CascadeDebugNames& names,
                                  uint32_t* outProbeBaseThisTick = nullptr,
                                  uint32_t* outProbesThisTick = nullptr);

    // Convergence readback (DDGIConvergedSolve::Throttle). Each tick's result
    // is one slot of partial sums per cascade (kVariabilityGroups uints
    // each); the CPU finishes the sum and compares the mean steady-tick
    // count against the blend kernel's history cap.
    struct VariabilityPayload
    {
        uint32_t Texels[2] = {0, 0};  // irradiance texels reduced per cascade (0 = cascade absent)
    };
    static constexpr uint32_t kVariabilityGroups = 64;
    // Reduces one cascade's TemporalState into `slot` at uint offset
    // `slotOffset`; declared after that cascade's blend so it reads this
    // tick's counts.
    void DeclareVariabilityPass(Rendering::RenderGraph::RGFrame& frame, CascadeGrid& grid,
                                Rendering::BufferHandle slot, uint32_t slotOffset,
                                const CascadeDebugNames& names);
    // Maps the newest finished reduction and moves the budget's throttle.
    // Hysteretic (kConvergedEnter / kConvergedExit) so a mean sitting on the
    // threshold does not flap the budget every tick.
    void ReadbackConvergence();
    static constexpr float kConvergedEnter = 0.95f;
    static constexpr float kConvergedExit = 0.90f;

    // Hardware lane's per-tick TLAS lifecycle (M4's epoch/hash/exec-confirm
    // gate + the shared BLAS pool's pending-build collection + the AS build
    // pass) — extracted unchanged from before M7 added the software lane
    // alongside it, so useHardware==true stays byte-identical to M2-M6.
    // Returns false when there is nothing confirmed or in-flight to trace
    // against yet this tick (the caller should skip the rest of its work).
    // Only ever called when m_SceneAS/m_TlasSlot are valid (see
    // ClaimHardwareLane in DeclareProbePasses).
    // Refreshes the material map atlas's layer assignment and declares one blit
    // dispatch per layer that still needs filling. Runs on both lanes, before
    // any trace pass is declared.
    void DeclareMapAtlasPasses(Rendering::RenderGraph::RGFrame& frame);

    // Software lane: declares this tick's skin-compute + BVH-refit passes for
    // every skinned instance the service planned a per-instance range for,
    // and imports SwNodes/SwVertexData so downstream traversals order after
    // the refit's writes (stashed in m_TickSwNodesRG / m_TickSwVertexDataRG).
    void DeclareSwSkinnedRefit(Rendering::RenderGraph::RGFrame& frame, RenderServices& services,
                               const std::vector<Rendering::GPUInstance>& instances,
                               bool solveWillRun);

    // The hardware lane's claim on the shared pool. Claim acquires the TLAS
    // channel on the first frame the lane traces; Release drops it — and every
    // TLAS and skinned-BLAS reference that depended on it — on the first frame
    // it does not. Both are no-ops when already in that state.
    bool ClaimHardwareLane();
    void ReleaseHardwareLane();

    bool TickHardwareLane(Rendering::RenderGraph::RGFrame& frame, bool anyBlasBecameReady,
                          const std::vector<Rendering::GPUInstance>& instances,
                          Rendering::BufferHandle skinPaletteBuffer, uint64_t skinPaletteBytes,
                          bool solveWillRun);

    // Per-cascade reflection-lobe GPU state. Kept SEPARATE from CascadeGrid
    // because a cascade can refuse the sharp lobe (history cap) while still
    // running the rough one, and because EnableGlossy=false must leave both
    // cascades' diffuse path untouched. C0 and C1 each bake from their own
    // traced ray buffer.
    struct ReflectionCascade
    {
        Rendering::BufferHandle RoughState{};
        Rendering::TextureHandle RoughAtlas{};
        Rendering::BufferHandle GlossyNumerator{};
        Rendering::BufferHandle GlossyWeight{};
        Rendering::TextureHandle GlossyAtlas{};
        DDGIGlossyAtlasLayout GlossyLayout{};
        bool GpuValid = false;       // at least the rough lobe is allocated
        bool GlossyAllocated = false;
        bool RoughAtlasUploaded = false;
        bool GlossyAtlasUploaded = false;
        bool NeedsClear = true;
        int32_t AllocatedProbeTotal = -1;
        int32_t AllocatedAtlasWidth = -1;
        int32_t AllocatedAtlasHeight = -1;
    };

    bool EnsureReflectionResources(float deltaTimeSeconds);
    void ReleaseReflectionResources();
    void ReleaseReflectionCascade(ReflectionCascade& refl);
    bool AllocateReflectionCascade(const CascadeGrid& grid, ReflectionCascade& refl,
                                   const char* debugPrefix, bool allocateGlossy);
    void DeclareReflectionPasses(Rendering::RenderGraph::RGFrame& frame, CascadeGrid& grid,
                                 ReflectionCascade& refl, const float gridMinWS[3],
                                 const float gridSizeWS[3], uint32_t probeBaseThisTick,
                                 uint32_t probesThisTick, const SharedTickInputs& shared,
                                 const CascadeDebugNames& names);

    Rendering::IDevice* m_Device = nullptr;
    SceneAccelerationStructureService* m_SceneAS = nullptr;  // null on a non-ray-query device
    Rendering::GPUScene* m_GpuScene = nullptr;
    Rendering::TlasSlotHandle m_TlasSlot;  // held only while the hardware lane traces
    Rendering::MeshGPURegistry* m_MeshRegistry = nullptr;  // retained to rebuild m_SceneService on device rebuild
    DDGIEmitterAreas m_EmitterAreas;
    MaterialSystem* m_Materials = nullptr;                 // ditto

    // M7/M9 sticky fallback: `supportsRayQuery` is a HARDWARE capability
    // flag (this GPU family can do ray-query) — it says nothing about
    // whether THIS BUILD's shader pipeline can actually translate the
    // ray-query GLSL for the active backend today (e.g., Metal's
    // ConvertUToAccelerationStructure has no MSL equivalent until the
    // descriptor-AS-type work lands — see the M9 plan entry). Pipeline
    // variant compilation is lazy (RGContext::GetOrCreatePipelineVariant,
    // called at command-recording time, not Declare time) and its failure
    // is NOT cached by the device's own variant cache, so retrying every
    // tick would both spam the log and silently do nothing forever instead
    // of falling back to the working software lane. Set (from the classify/
    // trace-HW dispatch lambdas, which may run off the declaring thread) the
    // first time a hardware pipeline variant comes back invalid; checked by
    // useHardware on every subsequent tick. Never cleared — a translation
    // failure is a build/backend-capability fact, not a transient one.
    std::atomic<bool> m_HardwareLaneFailed{false};

    // M7 software lane (Shaders/ddgi_trace_sw.comp). Owned unconditionally —
    // constructed in Initialize whenever meshRegistry/materials are given,
    // regardless of whether this device has ray-query, since the active
    // lane is a per-tick check (DeclareProbePasses), not a construction-time
    // one. See DDGISceneService.h's doc for what it actually does.
    std::unique_ptr<DDGISceneService> m_SceneService;
    // Hardware lane only: per-instance skinned BLASes (see its class doc).
    std::unique_ptr<DDGISkinnedGeometry> m_SkinnedGeometry;
    // This tick's skinned geometry-row override uploads, produced by
    // TickHardwareLane and consumed by the shared-inputs population below it.
    Rendering::BufferHandle m_TickSkinnedRowMapBuffer{};
    uint64_t m_TickSkinnedRowMapOffset = 0;
    uint64_t m_TickSkinnedRowMapBytes = 0;
    Rendering::BufferHandle m_TickSkinnedGeomRowsBuffer{};
    uint64_t m_TickSkinnedGeomRowsOffset = 0;
    uint64_t m_TickSkinnedGeomRowsBytes = 0;
    Rendering::RenderGraph::RGBuffer m_TickSwNodesRG{};
    Rendering::RenderGraph::RGBuffer m_TickSwVertexDataRG{};

    // Base-colour + emissive map atlas, shared by both lanes. Owned here (not
    // by DDGISceneService) because the hardware lane needs it too and never
    // constructs that service's scene sweep; DDGISceneService only reads it,
    // to bake each layer index into its packed uber-material records.
    std::unique_ptr<DDGIMaterialMapAtlas> m_MapAtlas;

    DDGIVolumeDesc m_Volume;

    CascadeGrid m_C0;  // coarse, full-volume
    CascadeGrid m_C1;  // M5: optional fine, sub-volume

    Rendering::SamplerHandle m_AtlasSampler{};  // shared by any cascade's atlases

    // All-zero stand-in bound for a cascade whose probe-state SSBO is not
    // allocated, so the DDGI keyword's b32/b36 always have a provider.
    // Created on first use — a world that never enables DDGI never allocates it.
    Rendering::BufferHandle m_ProbeStateFallback{};

    ReflectionCascade m_C0Refl;
    ReflectionCascade m_C1Refl;

    // Instance staging for the TLAS this feature's channel builds (unfiltered
    // — every opaque, non-skinned, zero-sector instance the shared BLAS pool
    // has ready; DDGI has no shadow-caster-style filter). A fresh
    // host-visible buffer per rebuild, retired the same way
    // RTShadowMaskService retires its own (frame-margin safe). Single/shared
    // across cascades — one scene, one TLAS, one epoch/hash/exec-confirm
    // state (M4's discipline; see DeclareProbePasses).
    struct RetiredBuffer
    {
        Rendering::BufferHandle Buffer{};
        uint64_t FrameStamp = 0;
    };
    std::vector<RetiredBuffer> m_RetiredInstanceBuffers;
    // The live TLAS instance staging buffer and the FNV hash of the bytes it
    // holds. A pose-only rebuild (skinned BLAS content moved under stable
    // addresses, instance list byte-identical) reuses it with no allocation
    // and no host write — the GPU re-reads the same immutable bytes — so a
    // scene with an animating skinned instance stops paying one upload-buffer
    // allocation per tick. Replaced (old one retired) only when the hash
    // moves.
    Rendering::BufferHandle m_TlasInstanceStaging{};
    uint64_t m_StagedInstanceHash = 0;
    uint64_t m_FrameClock = 0;
    uint64_t m_BuiltInstanceHash = 0;
    bool m_TlasContentValid = false;

    // M4 epoch/hash TLAS-rebuild gate (RTShadowMaskService::Schedule's
    // discipline, mirrored): a declared TLAS build only becomes trusted
    // content after its execution is confirmed via this atomic token —
    // checked at the top of the NEXT DeclareProbePasses call, matching the
    // shared BLAS pool's own BuildConfirmToken pattern. m_PendingTlasHash is
    // the hash the in-flight build will confirm to once executed.
    std::shared_ptr<std::atomic<bool>> m_TlasBuildExecuted;
    uint64_t m_PendingTlasHash = 0;
    bool m_HaveContentEpoch = false;
    uint64_t m_LastContentEpoch = 0;

    // M4 round-robin probe budget: classify/trace/blend cover `probesThisTick`
    // probes starting at a cascade's own ProbeCursor, not the whole grid
    // every tick. Upload preserves the untouched tiles between updates.
    // The per-tick ray budget is adaptive (DDGISolveBudget,
    // the reference's cadence controller), fed only on accepted solve ticks
    // and shared by both cascades' budgets via SharedTickInputs.
    DDGISolveBudget m_SolveBudget;
    // Per-tick reduction readback feeding m_SolveBudget's converged throttle.
    // Lazily initialized on the first Throttle tick; a device rebuild drops
    // it (its slots are persistently mapped Readback buffers).
    Rendering::RenderGraph::RGReadbackRing<VariabilityPayload> m_VariabilityRing;
    // True while the idle gate held the solve; the first accepted tick after
    // a hold clamps the budget (DDGISolveBudget::OnRestResume) so resuming
    // never lands at a stale pre-interaction maximum.
    bool m_SolveWasHeld = false;

    KernelPipeline m_Classify;
    KernelPipeline m_ClassifySw;  // software twin of m_Classify (adaptive placement, non-ray-query lane)
    KernelPipeline m_TraceHw;
    KernelPipeline m_TraceSw;  // M7 software lane
    KernelPipeline m_SwRefit;  // software-lane skinned BVH refit (ddgi_bvh_refit.comp)
    KernelPipeline m_Blend;
    KernelPipeline m_DepthBlend;   // DDGIDepthResolution::Fine only
    KernelPipeline m_Upload;
    KernelPipeline m_DepthUpload;  // DDGIDepthResolution::Fine only
    KernelPipeline m_Variability;  // DDGIConvergedSolve::Throttle only
    KernelPipeline m_Clear;
    KernelPipeline m_RoughBlend;
    KernelPipeline m_RoughUpload;
    KernelPipeline m_GlossyBlend;
    KernelPipeline m_GlossyUpload;
    KernelPipeline m_MapBlit;
    // Per-view scaled reflection resolve (DeclareGlossyResolveForView /
    // DDGIGlossyResolve.cpp). Loaded lazily on the first declaring view;
    // the point-clamp depth sampler avoids bilinear-mixed depths at
    // silhouettes feeding the position reconstruction. The blur kernel is
    // the depth-aware tent that smooths the lobe textures at resolve
    // resolution before the forward pass magnifies them.
    KernelPipeline m_GlossyResolve;
    KernelPipeline m_GlossyResolveBlur;
    Rendering::SamplerHandle m_GlossyResolveDepthSampler{};
    // Last frame each view's world pass wrote the resolve's shading-normal
    // history (MarkGlossyResolveNormalWritten). The kernel trusts the
    // history only on the frame right after a write: a view whose pass
    // withheld the slice (MSAA colour) or that skipped a frame falls back
    // to the depth-derived normal instead of reading a stale surface.
    struct GlossyResolveViewState
    {
        uint32_t ViewId = 0;
        uint64_t NormalWrittenFrame = 0;
    };
    std::vector<GlossyResolveViewState> m_GlossyResolveViews;
    uint64_t GlossyResolveNormalWrittenFrame(uint32_t viewId) const;

    // Sampled EVERY tick, whether or not DDGIVolumeDesc::ContinuousSolve is
    // off: the gate's rest timer must already be current the moment the toggle
    // is flipped, or a mid-motion flip would read a stale window.
    DDGICameraIdleGate m_CameraIdleGate;

    float m_LastTickTimeMs = -1.0f;  // for the hysteresis dt; -1 = first tick this session
};

}  // namespace GameEngine::Engine::Renderer
