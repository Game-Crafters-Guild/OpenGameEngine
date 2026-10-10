#pragma once

#include "Engine/Rendering/IRenderFeature.h"
#include "Ocean/OceanAlbedoSim.h"
#include "Ocean/OceanCaustics.h"
#include "Ocean/OceanClipSim.h"
#include "Ocean/OceanCombineSim.h"
#include "Ocean/OceanCollisionProvider.h"
#include "Ocean/OceanCpuTexture.h"
#include "Ocean/OceanDynWaves.h"
#include "Ocean/OceanDynWavesReadback.h"
#include "Ocean/OceanFFT.h"
#include "Ocean/OceanFlowSim.h"
#include "Ocean/OceanFoamSim.h"
#include "Ocean/OceanShadowSim.h"
#include "Ocean/OceanSimulationDemand.h"
#include "Ocean/OceanSplineRaster.h"
#include "Ocean/OceanWaterMaterial.h"
#include "Ocean/OceanSprayGPU.h"
#include "Ocean/OceanGPUQuery.h"
#include "Ocean/OceanForwardContributor.h"
#include "Ocean/OceanHeightField.h"
#include "Ocean/OceanInputRegistry.h"
#include "Ocean/OceanInputDrawSource.h"
#include "Ocean/OceanLightShafts.h"
#include "Ocean/OceanPlanarReflection.h"
#include "Ocean/OceanQuery.h"
#include "Ocean/OceanRasterDepthCapture.h"
#include "Ocean/OceanSceneGrab.h"
#include "Ocean/OceanSeabedDepth.h"
#include "Ocean/OceanTypes.h"
#include "Ocean/OceanTime.h"
#include "Ocean/OceanUnderwater.h"
#include "Ocean/OceanWaveMaskSim.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Handle.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace GameEngine::Rendering
{
class IDevice;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{
class RenderServices;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Ocean
{

// Owns all GPU-side resources for the ocean: the camera-following clipmap grid
// mesh, the persistent displacement cascade Texture2DArray (sampled from the FFT
// phase onward), and the per-frame ocean parameter ring buffer bound to the
// surface material. Created via RenderServices::EnsureFeature<OceanRenderFeature>().
class OceanRenderFeature : public Engine::Renderer::IRenderFeature,
                           public IOceanOriginShiftListener
{
public:
    void SetWaterMaterials(std::vector<OceanWaterMaterialGPU> materials)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_WaterMaterials = std::move(materials);
    }
    // Uploads the water-body material table for the surface draw: a 16-byte
    // count lane, then the materials. Returns an invalid allocation on failure.
    Rendering::RenderGraph::RGUploadRing::Alloc UploadWaterMaterials(Rendering::RenderGraph::RGFrame &frame,
                                                                     uint64 &outBytes) const;
    OceanSprayGPU& GetSprayGPU() { return m_SprayGPU; }
    OceanSplineRaster& GetSplineRaster() { return m_SplineRaster; }
    OceanShadowSim& GetShadows() { return m_Shadows; }
    OceanRenderFeature();
    ~OceanRenderFeature() override;

    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsInitialized() const { return m_Initialized; }

    // Lazily creates the surface contributor. The surface is submitted by the
    // post-world ocean pass so refraction can grab the same frame's opaque scene.
    void EnsureForwardContributor(Engine::Renderer::RenderServices& rs);
    // outSampledCascades (optional): the RG imports of every dynamic sim cascade
    // the command binds, for the consuming pass's declared sampled reads. Points
    // into contributor scratch — consume before the next build for this view.
    bool BuildSurfaceCommandForView(Engine::Renderer::RenderServices& rs,
                                    Rendering::RenderGraph::RGFrame& frame,
                                    ::GameEngine::Rendering::ViewId viewId,
                                    uint32 frameIndex,
                                    Engine::Renderer::DrawCommand& outCommand,
                                    std::span<const Rendering::RenderGraph::RGTexture>*
                                        outSampledCascades = nullptr);

    // ---- Camera-snapped concentric clipmap-ring mesh (single indexed draw) ----
    ::GameEngine::Rendering::BufferHandle GetGridVB() const { return m_GridVB; }
    ::GameEngine::Rendering::BufferHandle GetGridIB() const { return m_GridIB; }
    uint32 GetGridIndexCount() const { return m_GridIndexCount; }

    // ---- FFT wave simulation (the reference parity) ----
    bool IsFFTReady() const { return m_FFTReady; }
    OceanFFT& GetFFT() { return m_FFT; }
    bool IsLocalFFTReady(uint32 streamIndex) const
    {
        return streamIndex < kMaxOceanLocalFFTStreams &&
               m_LocalFFTReady[streamIndex] &&
               m_LocalFFTEnabled[streamIndex].load(std::memory_order_acquire);
    }
    bool IsLocalFFTReady() const { return GetLocalFFTReadyMask() != 0u; }
    uint32 GetLocalFFTReadyMask() const
    {
        uint32 mask = 0u;
        for (uint32 i = 0; i < kMaxOceanLocalFFTStreams; ++i)
            if (IsLocalFFTReady(i))
                mask |= (1u << i);
        return mask;
    }
    bool IsLocalFFTReadyForFrame(uint32 streamIndex, uint32 frameIndex) const
    {
        return IsLocalFFTReady(streamIndex) &&
               m_LastLocalFFTDispatchFrame[streamIndex] == frameIndex;
    }
    uint32 GetLocalFFTReadyMaskForFrame(uint32 frameIndex) const
    {
        uint32 mask = 0u;
        for (uint32 i = 0; i < kMaxOceanLocalFFTStreams; ++i)
            if (IsLocalFFTReadyForFrame(i, frameIndex))
                mask |= (1u << i);
        return mask;
    }
    OceanFFT& GetLocalFFT(uint32 streamIndex = 0u) { return m_LocalFFT[streamIndex]; }

    // Claims the once-per-frame FFT dispatch. DeclareForView runs per view, but
    // the wave simulation is global, so only the first view each frame records it.
    bool TryClaimFFTDispatch(uint32 frameIndex)
    {
        if (m_LastFFTDispatchFrame == frameIndex)
            return false;
        m_LastFFTDispatchFrame = frameIndex;
        m_SimulationDemand.NoteWaveDispatch(OceanSimulationDemand::Clock::now());
        return true;
    }

    // Bookkeeping for the frames in which no view sees water, called by every
    // view's pre-world declare before the sims claim; repeated calls are harmless.
    // It folds gameplay surface queries into OceanSimulationDemand's hold, and when
    // no wave dispatch happened within that hold it drops the height fields' CPU
    // copies: their waves are from an earlier ocean time, so queries use the
    // analytic surface until a new bake reads back.
    void BeginSimulationFrame();
    // True while a gameplay surface query (SampleSurface, SampleSurfaces) arrived
    // within OceanSimulationDemand::kHold: the wave simulation and its height
    // field keep running for it with no water in view.
    bool HasRecentSurfaceQueries() const
    {
        return m_SimulationDemand.HasRecentSurfaceQueries(OceanSimulationDemand::Clock::now());
    }
    // A view that sees no water claims no simulation dispatch. Claims compare
    // device frame slots, which repeat every frames-in-flight, so a claim left
    // from an earlier frame is forgotten; otherwise the first frame that sees
    // water again on the same slot would find its claim taken.
    void ForgetSimulationClaimsBefore(uint32 frameIndex);
    bool TryClaimLocalFFTDispatch(uint32 frameIndex)
    {
        return TryClaimLocalFFTDispatch(0u, frameIndex);
    }
    bool TryClaimLocalFFTDispatch(uint32 streamIndex, uint32 frameIndex)
    {
        if (streamIndex >= kMaxOceanLocalFFTStreams ||
            m_LastLocalFFTDispatchFrame[streamIndex] == frameIndex)
            return false;
        m_LastLocalFFTDispatchFrame[streamIndex] = frameIndex;
        return true;
    }

    // Once-per-frame claim for the sea-floor depth bake. Separate from the FFT
    // claim because shallow-water colour + shoreline foam don't depend on FFT
    // availability.
    bool TryClaimSeabedDispatch(uint32 frameIndex)
    {
        if (m_LastSeabedDispatchFrame == frameIndex)
            return false;
        m_LastSeabedDispatchFrame = frameIndex;
        return true;
    }

    // Once-per-frame claims for the flow bake + dynamic-wave sim. Both are global
    // (views share them) and independent of the FFT dispatch, so each gets its own
    // claim.
    bool TryClaimFlowDispatch(uint32 frameIndex)
    {
        if (m_LastFlowDispatchFrame == frameIndex)
            return false;
        m_LastFlowDispatchFrame = frameIndex;
        return true;
    }
    bool TryClaimDynWavesDispatch(uint32 frameIndex)
    {
        if (m_LastDynWavesDispatchFrame == frameIndex)
            return false;
        m_LastDynWavesDispatchFrame = frameIndex;
        return true;
    }
    bool TryClaimWaveMaskDispatch(uint32 frameIndex)
    {
        if (m_LastWaveMaskDispatchFrame == frameIndex)
            return false;
        m_LastWaveMaskDispatchFrame = frameIndex;
        return true;
    }
    bool TryClaimFoamDispatch(uint32 frameIndex)
    {
        if (m_LastFoamDispatchFrame == frameIndex)
            return false;
        m_LastFoamDispatchFrame = frameIndex;
        return true;
    }

    // Once-per-frame claims for the clip + albedo bakes. Both are global (views
    // share them) and independent of the FFT dispatch, so each gets its own claim.
    bool TryClaimClipDispatch(uint32 frameIndex)
    {
        if (m_LastClipDispatchFrame == frameIndex)
            return false;
        m_LastClipDispatchFrame = frameIndex;
        return true;
    }
    bool TryClaimAlbedoDispatch(uint32 frameIndex)
    {
        if (m_LastAlbedoDispatchFrame == frameIndex)
            return false;
        m_LastAlbedoDispatchFrame = frameIndex;
        return true;
    }
    // Once-per-frame claim for the planar-reflection capture. It re-renders the
    // world (a full world pass), and all views share the single reflection
    // target, so only the first view each frame records it (using that view's
    // camera). Separate from the FFT claim because reflections are a world pass.
    bool TryClaimReflectionDispatch(uint32 frameIndex)
    {
        if (m_LastReflectionDispatchFrame == frameIndex)
            return false;
        m_LastReflectionDispatchFrame = frameIndex;
        return true;
    }
    void SetFFTParams(const OceanFFTParamsGPU& params, bool spectrumDirty)
    {
        m_LatestDepthInputs.FFT = params;
        m_FFT.SetParams(params, spectrumDirty);
    }
    void SetLocalFFTParams(uint32 streamIndex, const OceanFFTParamsGPU& params,
                           bool spectrumDirty, bool enabled);
    void SetLocalFFTParams(const OceanFFTParamsGPU& params, bool spectrumDirty, bool enabled);
    ::GameEngine::Rendering::TextureHandle GetDisplacementTexture() const
    {
        return m_FFT.GetDisplacementTexture();
    }
    ::GameEngine::Rendering::SamplerHandle GetDisplacementSampler() const
    {
        return m_FFT.GetSampler();
    }
    ::GameEngine::Rendering::TextureHandle GetLocalDisplacementTexture(uint32 streamIndex = 0u) const
    {
        return (streamIndex < kMaxOceanLocalFFTStreams)
                   ? m_LocalFFT[streamIndex].GetDisplacementTexture()
                   : ::GameEngine::Rendering::TextureHandle{};
    }
    ::GameEngine::Rendering::SamplerHandle GetLocalDisplacementSampler(uint32 streamIndex = 0u) const
    {
        return (streamIndex < kMaxOceanLocalFFTStreams)
                   ? m_LocalFFT[streamIndex].GetSampler()
                   : ::GameEngine::Rendering::SamplerHandle{};
    }

    // ---- CPU-readable baked height field (buoyancy) ----
    // A camera-snapped tile of the FFT surface height, baked by a compute pass and
    // read back to the CPU (a few frames latent). The buoyancy system queries it
    // via SampleSurfaces so bodies float on the actual FFT waves. Dispatched once per
    // frame by OceanRenderNode right after the FFT.
    bool IsHeightFieldReady() const { return m_HeightFieldReady; }
    OceanHeightField& GetHeightField() { return m_HeightField; }
    bool IsLocalHeightFieldReady(uint32 streamIndex = 0u) const
    {
        return streamIndex < kMaxOceanLocalFFTStreams &&
               m_LocalHeightFieldReady[streamIndex] &&
               IsLocalFFTReady(streamIndex);
    }
    OceanHeightField& GetLocalHeightField(uint32 streamIndex = 0u)
    {
        return m_LocalHeightField[streamIndex];
    }

    // Post-submit fan-out (RenderServices::OnFrameSubmittedRG): stamps every
    // readback ring's pendings with the frame's fence token — completion is
    // the token signaling, so any dispatch cadence (dyn-waves quiescence,
    // one-shot raster captures) is safe without resets.
    void OnFrameSubmittedRG(::GameEngine::Rendering::RenderGraph::RGFrame& frame,
                            const ::GameEngine::Rendering::IDevice::GpuSyncToken& token) override
    {
        m_HeightField.OnFrameSubmitted(frame, token);
        for (auto& localHF : m_LocalHeightField)
            localHF.OnFrameSubmitted(frame, token);
        m_DynWavesReadback.OnFrameSubmitted(frame, token);
        m_GPUQuery.OnFrameSubmitted(frame, token);
        m_RasterDepthCapture.OnFrameSubmitted(frame, token);
    }

    // Frame-stream retirement (window closed): purge unstamped pendings keyed
    // to the dying frame so a recycled-address incarnation cannot falsely
    // stamp them.
    void OnFrameStreamRetiredRG(::GameEngine::Rendering::RenderGraph::RGFrame& frame) override
    {
        m_HeightField.OnFrameStreamRetired(frame);
        for (auto& localHF : m_LocalHeightField)
            localHF.OnFrameStreamRetired(frame);
        m_DynWavesReadback.OnFrameStreamRetired(frame);
        m_GPUQuery.OnFrameStreamRetired(frame);
        m_RasterDepthCapture.OnFrameStreamRetired(frame);
    }

    // Full world-space ocean surface sample at XZ for gameplay/physics. Reads the
    // newest completed FFT height field when it covers this XZ and otherwise uses
    // the analytic Gerstner representation. Flat water is never reported as valid
    // merely because an asynchronous readback has not completed yet.
    OceanSurfaceSample SampleSurface(float worldX, float worldZ) const;
    void SampleSurfaces(const OceanSurfaceQueryPoint* points, uint32 count,
                        OceanSurfaceSample* outSamples) const;
    OceanCurrentSample SampleFlow(float worldX, float worldZ) const;
    OceanSurfaceQueryHandle EnqueueSurfaceQueries(const OceanSurfaceQueryPoint* points, uint32 count);
    void ProcessQueuedSurfaceQueries(uint32 maxBatches = 0u);
    bool IsSurfaceQueryReady(OceanSurfaceQueryHandle handle) const;
    bool CopySurfaceQueryResults(OceanSurfaceQueryHandle handle, OceanSurfaceSample* outSamples,
                                 uint32 maxCount, uint32* outCount = nullptr,
                                 bool consume = true);

    // Preferred collision API. The provider owns persistent owner-keyed query
    // batches and always exposes the newest completed result without blocking the
    // caller. The legacy helpers above remain compatibility wrappers.
    IOceanCollisionProvider& GetCollisionProvider();
    const IOceanCollisionProvider& GetCollisionProvider() const;
    bool IsGPUQueryReady() const { return m_GPUQueryReady; }
    OceanGPUQuery& GetGPUQuery() { return m_GPUQuery; }
    bool TryClaimGPUQueryDispatch(uint32 frameIndex)
    {
        if (m_LastGPUQueryDispatchFrame == frameIndex)
            return false;
        m_LastGPUQueryDispatchFrame = frameIndex;
        return true;
    }

    // SampleSurface for the renderer's own probes (the submersion test, the
    // interaction wake impulses): the same sample, but not counted as a gameplay
    // query, so it never keeps the wave simulation running with no water in view.
    OceanSurfaceSample SampleSurfaceForRendering(float worldX, float worldZ) const;

    // ---- Foam simulation (persistent advected whitecaps) ----
    // Persistent foam is snapped/written once per frame like the other viewer
    // cascades. It usually reads this frame's FFT displacement, but it owns a
    // separate dispatch claim so the camera-following foam layout cannot stall
    // behind the surface when the FFT claim is skipped by another view/path.
    bool IsFoamReady() const { return m_FoamReady; }
    OceanFoamSim& GetFoamSim() { return m_FoamSim; }
    ::GameEngine::Rendering::TextureHandle GetFoamTexture() const { return m_FoamSim.GetFoamTexture(); }
    ::GameEngine::Rendering::SamplerHandle GetFoamSampler() const { return m_FoamSim.GetFoamSampler(); }

    // ---- Displacement combine cascade (reserved perf path) ----
    // Allocated for a future world-stable combine pass. The current viewer-snapped
    // implementation is bypassed for visible waves/foam, so the surface stays on the
    // direct FFT sum.
    bool IsCombineReady() const { return m_CombineReady; }
    OceanCombineSim& GetCombineSim() { return m_CombineSim; }
    void SetCombineEnabled(bool value) { m_CombineEnabled.store(value, std::memory_order_release); }
    bool IsCombineEnabled() const { return m_CombineEnabled.load(std::memory_order_acquire); }
    bool TryClaimCombineDispatch(uint32 frameIndex)
    {
        if (m_LastCombineDispatchFrame == frameIndex)
            return false;
        m_LastCombineDispatchFrame = frameIndex;
        return true;
    }
    bool IsCombineReadyForFrame(uint32 frameIndex) const
    {
        return m_CombineReady && IsCombineEnabled() && m_LastCombineDispatchFrame == frameIndex;
    }
    // ---- Sea-floor depth cascade (shallow colour + shoreline foam) ----
    // Baked once per frame from the tagged seabeds, right before the foam sim
    // (which reads it for the shoreline term). Degrades to "deep water" when no
    // seabed is tagged or the bake never initialized.
    bool IsSeabedDepthReady() const { return m_SeabedDepthReady; }
    OceanSeabedDepth& GetSeabedDepth() { return m_SeabedDepth; }
    ::GameEngine::Rendering::TextureHandle GetSeabedDepthTexture() const
    {
        return m_SeabedDepth.GetDepthTexture();
    }
    ::GameEngine::Rendering::SamplerHandle GetSeabedDepthSampler() const
    {
        return m_SeabedDepth.GetDepthSampler();
    }
    OceanRasterDepthCapture& GetRasterDepthCapture() { return m_RasterDepthCapture; }
    void SetRasterDepthCaptureSettings(const OceanRasterDepthCaptureSettings& settings)
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_RasterDepthCaptureSettings = settings;
    }
    OceanRasterDepthCaptureSettings GetRasterDepthCaptureSettings() const
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        return m_RasterDepthCaptureSettings;
    }

    // ---- Flow cascade (foam advection + detail-UV scroll) ----
    // Baked once per frame from the tagged flow sources. Degrades to "no flow"
    // when no source is tagged or the bake never initialized.
    bool IsFlowReady() const { return m_FlowReady; }
    OceanFlowSim& GetFlow() { return m_Flow; }
    ::GameEngine::Rendering::TextureHandle GetFlowTexture() const { return m_Flow.GetFlowTexture(); }
    ::GameEngine::Rendering::SamplerHandle GetFlowSampler() const { return m_Flow.GetFlowSampler(); }

    // ---- Dynamic (interactive) waves ----
    // Ping-pong wave-equation sim driven by per-frame impulses. Sampled by the
    // surface vertex modifier (displacement) + fragment shader (normal fold).
    // Degrades to "spectrum waves only" when no impulse is present or the sim
    // never initialized.
    bool IsDynWavesReady() const { return m_DynWavesReady; }
    OceanDynWaves& GetDynWaves() { return m_DynWaves; }
    ::GameEngine::Rendering::TextureHandle GetDynWavesTexture() const { return m_DynWaves.GetTexture(); }
    ::GameEngine::Rendering::SamplerHandle GetDynWavesSampler() const { return m_DynWaves.GetSampler(); }
    bool IsDynWavesReadbackReady() const { return m_DynWavesReadbackReady; }
    OceanDynWavesReadback& GetDynWavesReadback() { return m_DynWavesReadback; }

    // ---- Local wave overrides ----
    // Baked once per frame from water-body wave override sources. Degrades to
    // (1,1) everywhere when no source is tagged or the bake never initialized.
    bool IsWaveMaskReady() const { return m_WaveMaskReady; }
    OceanWaveMaskSim& GetWaveMask() { return m_WaveMask; }
    ::GameEngine::Rendering::TextureHandle GetWaveMaskTexture() const { return m_WaveMask.GetTexture(); }
    ::GameEngine::Rendering::SamplerHandle GetWaveMaskSampler() const { return m_WaveMask.GetSampler(); }

    // ---- Clip cascade (cut holes in the surface) ----
    // Baked once per frame from the tagged clip sources. Degrades to "the default
    // clip state" when no source is tagged or the bake never initialized.
    bool IsClipReady() const { return m_ClipReady; }
    OceanClipSim& GetClip() { return m_Clip; }
    ::GameEngine::Rendering::TextureHandle GetClipTexture() const { return m_Clip.GetClipTexture(); }
    ::GameEngine::Rendering::SamplerHandle GetClipSampler() const { return m_Clip.GetClipSampler(); }

    // ---- Albedo cascade (decals / paint on the surface albedo) ----
    // Baked once per frame from the tagged albedo sources. Degrades to "no paint"
    // when no source is tagged or the bake never initialized.
    bool IsAlbedoReady() const { return m_AlbedoReady; }
    OceanAlbedoSim& GetAlbedo() { return m_Albedo; }
    ::GameEngine::Rendering::TextureHandle GetAlbedoTexture() const { return m_Albedo.GetAlbedoTexture(); }
    ::GameEngine::Rendering::SamplerHandle GetAlbedoSampler() const { return m_Albedo.GetAlbedoSampler(); }

    // Clamp a snapped cascade layout's LodCount DOWN to the authored limit
    // (SetLodCountLimit) so the quality tier / renderer LOD budget trims how many
    // cascade layers the surface samples, without resizing the (fixed-size)
    // cascade textures. The caller writes the result into the frame's upload
    // ring and binds it by name on the surface draw.
    OceanCascadeLayoutGPU ClampCascadeLayout(const OceanCascadeLayoutGPU& layout) const;

    // Which camera-snapped cascades a consumer binds this frame (each true only
    // when that sim's texture is bound and current).
    struct SampledCascadeAvailability
    {
        bool Available[static_cast<uint32>(OceanSampledCascade::Count)] = {};

        bool& operator[](OceanSampledCascade cascade) { return Available[static_cast<uint32>(cascade)]; }
        bool operator[](OceanSampledCascade cascade) const { return Available[static_cast<uint32>(cascade)]; }
    };
    // The layout block ("OceanCascadeLayout", set 2 binding 4) for a consumer of
    // the ocean sample helpers: each available cascade's own layout, clamped by
    // ClampCascadeLayout except the clip's (its coarsest layer covers all authored
    // water); LodCount 0 for the others. Each sim snaps on its own
    // schedule, so a texture sampled with another cascade's layout is drawn where
    // that cascade was snapped, not where it was written.
    OceanSampledCascadeLayoutsGPU SampledCascadeLayouts(const SampledCascadeAvailability& available) const;

    // Authored cascade LOD-count limit (from OceanRenderer.LodCount), set by the
    // extraction system. Clamps the uploaded layout's LodCount down so the surface
    // samples fewer snapped LOD layers (lower fill) — never up past what each sim
    // actually allocated. 0 = no limit (use the sim's full layer count).
    void SetLodCountLimit(uint32 value) { m_LodCountLimit.store(value, std::memory_order_release); }
    uint32 GetLodCountLimit() const { return m_LodCountLimit.load(std::memory_order_acquire); }

    // ---- Authored cascade resolution controls (OceanRenderer) ----
    // Set by the extraction system; applied once per frame by ApplyCascadeConfig.
    // 0 = leave the sim's built-in default. MinScale is the world extent of the
    // finest (LOD 0) cascade — smaller concentrates resolution nearer the camera.
    // LodDataResolution is the per-axis texel size of the snapped sim cascades
    // (foam/flow/dyn-waves/seabed/clip/albedo); a change resizes those textures.
    // GeometryGridSize is the generated surface grid cells per edge.
    void SetCascadeBaseScale(float value) { m_CascadeBaseScale.store(value, std::memory_order_release); }
    void SetCascadeResolution(uint32 value) { m_CascadeResolution.store(value, std::memory_order_release); }
    void SetGeometryGridSize(uint32 value) { m_GeometryGridSize.store(value, std::memory_order_release); }
    // Apply the authored base scale / resolution to every snapped sim and rebuild
    // the grid mesh if the requested density changed. Call once per frame BEFORE
    // the sims' BeginFrame snaps (so the new base scale takes effect this frame).
    void ApplyCascadeConfig(::GameEngine::Rendering::IDevice* device);

    // ---- Per-frame ocean parameters ----
    // Written by OceanExtractionSystem (ECS thread), read at emit/execute time.
    void SetParams(const OceanParamsGPU& params);
    OceanParamsGPU GetParams() const;
    void SetInputStats(const OceanInputFrameStats& stats);
    OceanInputFrameStats GetInputStats() const;
    void SetTypedInputPackets(std::vector<OceanInputDrawPacket> packets);
    std::vector<OceanInputDrawPacket> GetTypedInputPackets() const;

    OceanOriginShiftEvent NotifyOriginShift(float32 shiftX, float32 shiftY,
                                            float32 shiftZ, bool teleport = false)
    {
        return m_OriginShiftNotifier.Notify(shiftX, shiftY, shiftZ, teleport);
    }
    void OnOceanOriginShift(const OceanOriginShiftEvent& event) override;
    OceanOriginShiftNotifier& GetOriginShiftNotifier() { return m_OriginShiftNotifier; }
    float32 GetWaveOriginOffsetX() const
    {
        return m_WaveOriginOffsetX.load(std::memory_order_acquire);
    }
    float32 GetWaveOriginOffsetZ() const
    {
        return m_WaveOriginOffsetZ.load(std::memory_order_acquire);
    }

    // Stamp the per-view submersion result into the stored params' Underwater
    // flag (1 = camera below the displaced surface this frame, so the surface
    // shades two-sided + the overlay runs). The extraction system stored the
    // authored toggle; this ANDs the render-time submersion onto it. Called by
    // OceanRenderNode once per view after the camera snap.
    // Pre-world node stamps the submersion state + depth here; that view's post-world
    // underwater node reads GetSubmergedDepth() to drive the overlay (the overlay
    // can't run in the same node — it must be scheduled after the world pass).
    void SetUnderwaterActive(::GameEngine::Rendering::ViewId viewId, bool active, float submergedDepth);
    OceanParamsGPU GetParamsForView(::GameEngine::Rendering::ViewId viewId) const;
    float GetSubmergedDepthForView(::GameEngine::Rendering::ViewId viewId) const;
    bool IsUnderwaterEnabled() const { return m_UnderwaterEnabled.load(std::memory_order_acquire); }

    // ---- Underwater volumes (Crest volume / fly-through mode) ----
    // An axis-aligned box that confines the underwater overlay to a bounded water
    // body. Resolved from OceanUnderwaterVolume components by the extraction system.
    struct UnderwaterVolumeBox
    {
        float CenterX = 0.0f, CenterY = 0.0f, CenterZ = 0.0f;
        float HalfX = 0.0f, HalfY = 0.0f, HalfZ = 0.0f;
    };
    struct UnderwaterVolumePolygon
    {
        float SurfaceY = 0.0f;
        float Depth = 0.0f;
        uint32 PointCount = 0;
        float X[kMaxOceanClipPolygonPoints] = {};
        float Z[kMaxOceanClipPolygonPoints] = {};
    };
    void SetUnderwaterVolumes(const std::vector<UnderwaterVolumeBox>& boxes);
    void SetUnderwaterVolumePolygons(const std::vector<UnderwaterVolumePolygon>& polygons);
    void SetUnderwaterExclusionVolumes(const std::vector<UnderwaterVolumeBox>& boxes);
    void SetUnderwaterPortalOccluders(const std::vector<UnderwaterVolumeBox>& boxes);
    bool HasUnderwaterVolumes() const;
    bool IsUnderwaterExcluded(float x, float y, float z) const;
    // True when (x,y,z) is inside any volume; outDepth = that box's top minus y
    // (>= 0, the submersion depth used for the overlay fog). False when outside all.
    bool TestUnderwaterVolume(float x, float y, float z, float& outDepth) const;
    // Fills `out` for the underwater pass, reusing its storage.
    void FillUnderwaterPortalData(OceanUnderwaterPortalData &out, uint32 maxVolumeBoxes,
                                  uint32 maxExclusionBoxes, uint32 maxPolygons,
                                  uint32 maxOccluderBoxes = kMaxOceanUnderwaterPortalOccluders) const;

    // ---- Water-body bounds (query/collision filtering) ----
    // Rectangular water-body footprints resolved from OceanWaterBody components.
    // When constrainSurface is true, CPU surface queries outside every box return
    // Valid=false so gameplay/physics agree with the clipped visible surface.
    struct WaterBodyBox
    {
        float CenterX = 0.0f, CenterZ = 0.0f;
        float HalfX = 0.0f, HalfZ = 0.0f;
    };
    struct WaterBodyStamp
    {
        float CenterX = 0.0f, CenterZ = 0.0f;
        float HalfWidth = 0.0f;
    };
    struct WaterBodyPolygon
    {
        uint32 PointCount = 0;
        float X[kMaxOceanClipPolygonPoints] = {};
        float Z[kMaxOceanClipPolygonPoints] = {};
    };
    struct WaterBodyWaveBox
    {
        float CenterX = 0.0f, CenterZ = 0.0f;
        float HalfX = 0.0f, HalfZ = 0.0f;
        float Weight = 1.0f;
        float Chop = 1.0f;
        float Feather = 0.0f;
        uint32 LocalWaveCount = 1u;
        float LocalAmplitude = 0.0f;
        float LocalWavelength = 12.0f;
        float LocalDirX = 1.0f;
        float LocalDirZ = 0.0f;
        float LocalWaveExtra[kMaxOceanWaveMaskLocalWaves - 1u][4] = {};
        float LocalFFTBlend = 0.0f;
        uint32 LocalFFTStream = 0u;
    };
    struct WaterBodyWavePolygon
    {
        uint32 PointCount = 0;
        float X[kMaxOceanClipPolygonPoints] = {};
        float Z[kMaxOceanClipPolygonPoints] = {};
        float Weight = 1.0f;
        float Chop = 1.0f;
        float Feather = 0.0f;
        uint32 LocalWaveCount = 1u;
        float LocalAmplitude = 0.0f;
        float LocalWavelength = 12.0f;
        float LocalDirX = 1.0f;
        float LocalDirZ = 0.0f;
        float LocalWaveExtra[kMaxOceanWaveMaskLocalWaves - 1u][4] = {};
        float LocalFFTBlend = 0.0f;
        uint32 LocalFFTStream = 0u;
    };
    struct WaterBodyWaveTextureBox
    {
        float CenterX = 0.0f, CenterZ = 0.0f;
        float HalfX = 0.0f, HalfZ = 0.0f;
        float WeightScale = 1.0f;
        float ChopScale = 1.0f;
        float WeightBias = 0.0f;
        float ChopBias = 0.0f;
        float Coverage = 1.0f;
        float Feather = 0.0f;
        OceanCpuTextureRG Texture;
    };
    struct WaterBodyWaveOverrideSample
    {
        float Weight = 1.0f;
        float Chop = 1.0f;
        bool TextureWaveMaskContributes = false;
        uint32 TextureWaveMaskCount = 0u;
        float LocalFFTBlend = 0.0f;
        uint32 LocalFFTStream = 0u;
        float LocalHeight = 0.0f;
        float LocalSlopeX = 0.0f;
        float LocalSlopeZ = 0.0f;
    };
    void SetWaterBodies(const std::vector<WaterBodyBox>& boxes, bool constrainSurface);
    void SetWaterBodyStamps(const std::vector<WaterBodyStamp>& stamps);
    void SetWaterBodyPolygons(const std::vector<WaterBodyPolygon>& polygons);
    void SetWaterBodyWaveOverrides(const std::vector<WaterBodyWaveBox>& boxes,
                                   const std::vector<WaterBodyWavePolygon>& polygons,
                                   const std::vector<WaterBodyWaveTextureBox>& textures = {});
    bool IsSurfaceQueryAllowed(float x, float z) const;
    // True when the visible water may reach into the view-projection `viewProj`
    // (16 floats, the CameraData layout): the box spanning [minY, maxY] over any
    // water-body footprint (box, stamp, polygon or surface-ribbon bounds), widened
    // by horizontalMargin metres, intersects its frustum. When the water bodies do
    // not confine the surface the ocean is unbounded, and the band spans the world.
    bool IsWaterFootprintInView(const float* viewProj, float minY, float maxY,
                                float horizontalMargin) const;
    float SampleWaterBodyWaveWeight(float x, float z) const;
    WaterBodyWaveOverrideSample SampleWaterBodyWaveOverride(float x, float z, float time) const;

    // Map the current frame slot and upload the stored params. Returns the
    // buffer to bind on the surface draw. Lazily creates the ring on first call.
    // refractionAvailable stamps the per-draw grab flag (the contributor knows
    // grab readiness at emit; the extraction system does not), so a frame where
    // the scene grab declined reads opaque without an extra param round-trip.
    // seabedDepthAvailable likewise stamps the depth-cascade flag (1 = a seabed
    // was baked + bound this frame; 0 = deep water, shallow/shoreline off).
    // causticsAvailable stamps the caustics gate (1 = the caustics texture was
    // generated AND bound this frame; only set when refraction is also active,
    // since caustics modulate the grabbed scene colour).
    // flowAvailable stamps the flow gate (1 = the flow cascade was baked + bound
    // this frame, so the surface scrolls its detail UVs by it). dynWavesAvailable
    // stamps the dynamic-wave gate (1 = the sim ran + its cascade was bound, so the
    // surface adds the dynamic height + normal fold). Both default off when the sim
    // declined or no source/impulse is present.
    // clipAvailable stamps the clip gate (1 = the clip cascade was baked + bound
    // this frame, so the surface discards where the cascade clips). albedoAvailable
    // stamps the albedo gate (1 = the albedo cascade was baked + bound, so the
    // surface blends the painted colour into baseColor). Both default off when the
    // bake declined or no source is present.
    void FillViewParams(OceanParamsGPU& out, ::GameEngine::Rendering::ViewId viewId,
                        uint32 frameIndex, bool refractionAvailable, bool seabedDepthAvailable,
                        bool causticsAvailable, bool flowAvailable, bool dynWavesAvailable,
                        bool waveMaskAvailable, bool clipAvailable, bool albedoAvailable,
                        bool foamTextureAvailable, bool causticsTextureAvailable,
                        bool planarReflectionAvailable);

    bool HasOcean() const { return m_HasOcean.load(std::memory_order_acquire); }
    void SetHasOcean(bool value) { m_HasOcean.store(value, std::memory_order_release); }
    // The surface writes depth (Opaque material) from its own node, and that
    // depth changes only when the blocks it is drawn from change (the surface
    // parameter block and the global and local FFT spectrum blocks: the ocean
    // time, the wave, spectrum and surface settings) while some view sees the
    // water. With those blocks unchanged (ocean time fixed or paused, no edit),
    // or no water in view, the claim is false.
    bool WritesDynamicDepth() const override
    {
        return HasOcean() && m_SurfaceDepthChanging.load(std::memory_order_acquire);
    }
    // A view's post-world declare found water in view. Declare thread.
    void NoteWaterInView() { m_WaterInViewSincePublish.store(true, std::memory_order_release); }
    // Decides WritesDynamicDepth for the frame about to render: true when a view
    // saw water since the previous call and the published surface, FFT or local
    // FFT block differs from the one at that call. The extraction system calls it once
    // per update, after its last SetParams.
    void PublishDepthClaim();

    // Authored flow / dynamic-wave toggles (from OceanSurface). Stored by the
    // extraction system; read by the contributor to gate binding + availability.
    void SetFlowEnabled(bool value) { m_FlowEnabled.store(value, std::memory_order_release); }
    bool IsFlowEnabled() const { return m_FlowEnabled.load(std::memory_order_acquire); }
    void SetDynWavesEnabled(bool value) { m_DynWavesEnabled.store(value, std::memory_order_release); }
    bool IsDynWavesEnabled() const { return m_DynWavesEnabled.load(std::memory_order_acquire); }

    // Authored clip / albedo toggles (from OceanSurface.ClipSurface / .Albedo).
    // Stored by the extraction system; read by the contributor to gate binding +
    // the per-draw availability flags.
    void SetClipEnabled(bool value) { m_ClipEnabled.store(value, std::memory_order_release); }
    bool IsClipEnabled() const { return m_ClipEnabled.load(std::memory_order_acquire); }
    void SetAlbedoEnabled(bool value) { m_AlbedoEnabled.store(value, std::memory_order_release); }
    bool IsAlbedoEnabled() const { return m_AlbedoEnabled.load(std::memory_order_acquire); }

    // ---- Scene-colour grab (refraction) ----
    // Snapshots the opaque scene colour so the surface can refract it. Scheduled
    // by OceanRenderNode before the world pass; bound by name on the surface
    // draw. Degrades to opaque when the grab can't be built (no SceneColor).
    OceanSceneGrab& GetSceneGrab() { return m_SceneGrab; }

    // ---- Planar reflection capture ----
    // Re-renders the opaque world reflected across the sea plane into a scaled
    // texture the surface samples (mirror reflections). Scheduled by
    // OceanRenderNode pre-world when OceanSurface.PlanarReflections is on; bound
    // by name on the surface draw when it produced a texture this frame. Degrades
    // to the procedural sky dome when it declines.
    OceanPlanarReflection& GetPlanarReflection() { return m_PlanarReflection; }
    void SetPlanarReflectionScale(float value)
    {
        m_PlanarReflectionScale.store(value, std::memory_order_release);
    }
    float GetPlanarReflectionScale() const
    {
        return m_PlanarReflectionScale.load(std::memory_order_acquire);
    }

    // ---- Underwater overlay ----
    // Fullscreen submerged tint + fog-from-below + meniscus, scheduled by
    // OceanRenderNode at the post phase only when the camera is below the
    // displaced surface. Degrades to no overlay when SceneColor/depth can't be
    // resolved (a project rendergraph stripped the post phase).
    OceanUnderwater& GetUnderwater() { return m_Underwater; }

    // ---- Above-water reflected caustics ----
    // Fullscreen caustic projection from the ocean surface, scheduled post-world
    // by OceanRenderNode when the camera is above water.
    OceanLightShafts& GetReflectedCaustics() { return m_ReflectedCaustics; }

    // Authored underwater + reflected-caustic settings (from OceanSurface),
    // stored here so the effect knobs stay OFF the byte-exact OceanParamsGPU. Set
    // by the extraction system; read by OceanRenderNode to drive the passes.
    void SetUnderwaterSettings(const OceanUnderwaterSettings& s)
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_UnderwaterSettings = s;
    }
    OceanUnderwaterSettings GetUnderwaterSettings() const
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        return m_UnderwaterSettings;
    }

    // ---- Procedural caustics (underwater light focusing) ----
    // A small tileable RGBA8 caustics texture generated once at startup, sampled
    // by the surface to modulate the refracted scene colour. Degrades to "no
    // caustics" when generation failed (the surface's CausticsAvailable gate
    // stays 0). Bound by name on the surface draw only when refraction is active.
    bool IsCausticsReady() const { return m_CausticsReady; }
    ::GameEngine::Rendering::TextureHandle GetCausticsTexture() const
    {
        return m_Caustics.GetTexture();
    }
    ::GameEngine::Rendering::SamplerHandle GetCausticsSampler() const
    {
        return m_Caustics.GetSampler();
    }

    // ---- User texture overrides (normal detail, foam bubbles, caustics) ----
    // Optional textures resolved from OceanSurface's NormalTexture, FoamTexture and
    // CausticsTexture GUIDs by extraction. Each overrides its procedural fallback
    // and is gated by a per-draw availability flag. An empty slot clears its handle.
    // Set during extraction and read by the contributor in the same frame (the
    // extraction phase completes before the render graph executes), so plain
    // handles match the existing m_Caustics object pattern.
    void SetUserNormalTexture(::GameEngine::Rendering::TextureHandle tex) { m_UserNormalTexture = tex; }
    bool HasUserNormalTexture() const { return m_UserNormalTexture.IsValid(); }
    ::GameEngine::Rendering::TextureHandle GetUserNormalTexture() const { return m_UserNormalTexture; }
    void SetUserFoamTexture(::GameEngine::Rendering::TextureHandle tex) { m_UserFoamTexture = tex; }
    bool HasUserFoamTexture() const { return m_UserFoamTexture.IsValid(); }
    ::GameEngine::Rendering::TextureHandle GetUserFoamTexture() const { return m_UserFoamTexture; }
    ::GameEngine::Rendering::SamplerHandle GetUserFoamSampler() const { return m_UserTextureSampler; }

    void SetUserCausticsTexture(::GameEngine::Rendering::TextureHandle tex)
    {
        m_UserCausticsTexture = tex;
    }
    bool HasUserCausticsTexture() const { return m_UserCausticsTexture.IsValid(); }
    ::GameEngine::Rendering::TextureHandle GetUserCausticsTexture() const { return m_UserCausticsTexture; }
    ::GameEngine::Rendering::SamplerHandle GetUserCausticsSampler() const { return m_UserTextureSampler; }

private:
    void CreateGridMesh(::GameEngine::Rendering::IDevice* device, uint32 gridSize);
    // Free the current clipmap VB/IB and rebuild at a new vertex density.
    void RebuildGridMesh(::GameEngine::Rendering::IDevice* device, uint32 gridSize);

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    bool m_Initialized = false;
    bool m_InitDeclined = false;

    ::GameEngine::Rendering::BufferHandle m_GridVB;
    ::GameEngine::Rendering::BufferHandle m_GridIB;
    uint32 m_GridIndexCount = 0;
    uint32 m_GridVertexCount = 0;
    uint32 m_GridSize = 0;

    // FFT wave simulation (displacement cascade array + compute pipelines).
    OceanFFT m_FFT;
    bool m_FFTReady = false;
    uint32 m_LastFFTDispatchFrame = 0xFFFFFFFFu;
    // Mutable: the const SampleSurface / SampleSurfaces note gameplay demand.
    mutable OceanSimulationDemand m_SimulationDemand;
    std::array<uint32, kMaxOceanLocalFFTStreams> m_LastLocalFFTDispatchFrame = {
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    uint32 m_LastSeabedDispatchFrame = 0xFFFFFFFFu;
    uint32 m_LastFlowDispatchFrame = 0xFFFFFFFFu;
    uint32 m_LastDynWavesDispatchFrame = 0xFFFFFFFFu;
    uint32 m_LastWaveMaskDispatchFrame = 0xFFFFFFFFu;
    uint32 m_LastFoamDispatchFrame = 0xFFFFFFFFu;
    uint32 m_LastClipDispatchFrame = 0xFFFFFFFFu;
    uint32 m_LastAlbedoDispatchFrame = 0xFFFFFFFFu;
    uint32 m_LastReflectionDispatchFrame = 0xFFFFFFFFu;
    uint32 m_LastCombineDispatchFrame = 0xFFFFFFFFu;
    uint32 m_LastGPUQueryDispatchFrame = 0xFFFFFFFFu;

    // Optional per-water-body local spectrum streams.
    std::array<OceanFFT, kMaxOceanLocalFFTStreams> m_LocalFFT;
    std::array<bool, kMaxOceanLocalFFTStreams> m_LocalFFTReady = {};
    std::array<std::atomic<bool>, kMaxOceanLocalFFTStreams> m_LocalFFTEnabled{};

    // Foam simulation (persistent advected foam cascade + compute pipeline).
    std::vector<OceanWaterMaterialGPU> m_WaterMaterials; // guarded by m_Mutex
    OceanSprayGPU m_SprayGPU;
    OceanSplineRaster m_SplineRaster;
    OceanShadowSim m_Shadows;
    OceanFoamSim m_FoamSim;
    bool m_FoamReady = false;

    // World-coordinate displacement combine cascade. Its storage follows the
    // camera, but texels are evaluated from absolute world XZ, preserving phase
    // while reducing the surface/foam sample count.
    OceanCombineSim m_CombineSim;
    bool m_CombineReady = false;

    // CPU-readable baked height field (buoyancy reads the FFT surface).
    OceanHeightField m_HeightField;
    bool m_HeightFieldReady = false;
    std::array<OceanHeightField, kMaxOceanLocalFFTStreams> m_LocalHeightField;
    std::array<bool, kMaxOceanLocalFFTStreams> m_LocalHeightFieldReady = {};
    std::array<bool, kMaxOceanLocalFFTStreams> m_LocalHeightFieldInitAttempted = {};
    OceanGPUQuery m_GPUQuery;
    bool m_GPUQueryReady = false;

    // Sea-floor depth cascade (top-down seabed depth + compute pipeline).
    OceanSeabedDepth m_SeabedDepth;
    bool m_SeabedDepthReady = false;
    OceanRasterDepthCapture m_RasterDepthCapture;
    OceanRasterDepthCaptureSettings m_RasterDepthCaptureSettings;

    // Flow cascade (horizontal current bake + compute pipeline).
    OceanFlowSim m_Flow;
    bool m_FlowReady = false;

    // Dynamic (interactive) wave sim (ping-pong height/velocity + compute pipeline).
    OceanDynWaves m_DynWaves;
    bool m_DynWavesReady = false;
    OceanDynWavesReadback m_DynWavesReadback;
    bool m_DynWavesReadbackReady = false;

    // Local wave override mask (weight/chop + compute pipeline).
    OceanWaveMaskSim m_WaveMask;
    bool m_WaveMaskReady = false;

    // Clip cascade (surface clip-state bake + compute pipeline).
    OceanClipSim m_Clip;
    bool m_ClipReady = false;

    // Albedo cascade (surface paint bake + compute pipeline).
    OceanAlbedoSim m_Albedo;
    bool m_AlbedoReady = false;

    // Scene-colour grab for screen-space refraction (feature-owned texture +
    // copy pipeline). Lazily built on first DeclareForView with a SceneColor.
    OceanSceneGrab m_SceneGrab;

    // Planar reflection capture (feature-owned scaled colour target + a
    // persistent secondary view/camera). Lazily built on first DeclareForView
    // with PlanarReflections enabled.
    OceanPlanarReflection m_PlanarReflection;

    // Underwater overlay (feature-owned runtime-compiled program + UBO ring).
    // Lazily built on first submerged DeclareForView.
    OceanUnderwater m_Underwater;

    // Above-water reflected caustics (feature-owned runtime-compiled program + UBO ring).
    OceanLightShafts m_ReflectedCaustics;

    // Procedural caustics texture (feature-owned RGBA8 + mip chain), generated
    // once at Initialize. Inert (handle invalid) when generation failed.
    OceanCaustics m_Caustics;
    bool m_CausticsReady = false;

    // User texture overrides resolved each frame from the OceanSurface component
    // (NormalTexture / FoamTexture / CausticsTexture GUIDs). Invalid means no override.
    // All three use a shared Linear/Repeat sampler created at Initialize.
    ::GameEngine::Rendering::TextureHandle m_UserNormalTexture;
    ::GameEngine::Rendering::TextureHandle m_UserFoamTexture;
    ::GameEngine::Rendering::TextureHandle m_UserCausticsTexture;
    ::GameEngine::Rendering::SamplerHandle m_UserTextureSampler;

    mutable std::mutex m_Mutex;
    OceanParamsGPU m_Params;
    OceanInputFrameStats m_InputStats;
    std::vector<OceanInputDrawPacket> m_TypedInputPackets; // guarded by m_Mutex
    OceanUnderwaterSettings m_UnderwaterSettings; // guarded by m_Mutex
    struct UnderwaterViewState
    {
        uint32 Underwater = 0u;
        float SubmergedDepth = 0.0f;
    };
    std::unordered_map<::GameEngine::Rendering::ViewId, UnderwaterViewState> m_UnderwaterByView; // guarded by m_Mutex
    struct PendingSurfaceQueryBatch
    {
        OceanSurfaceQueryHandle Handle = 0;
        std::vector<OceanSurfaceQueryPoint> Points;
        std::vector<OceanSurfaceSample> Samples;
        bool Ready = false;
        bool InProgress = false;
    };
    mutable std::mutex m_SurfaceQueryMutex;
    std::vector<PendingSurfaceQueryBatch> m_SurfaceQueryBatches;
    std::atomic<OceanSurfaceQueryHandle> m_NextSurfaceQueryHandle{1};
    mutable std::unique_ptr<OceanFeatureCollisionProvider> m_CollisionProvider;
    std::atomic<bool> m_HasOcean{false};
    std::atomic<bool> m_WaterInViewSincePublish{false};
    std::atomic<bool> m_SurfaceDepthChanging{false};
    // Every block the surface depth is a function of. The FFT blocks are recorded
    // as SetFFTParams / SetLocalFFTParams receive them: a spectrum-dirty flag is
    // not enough, since Chop reaches the surface without re-baking the spectrum.
    // Extraction thread.
    struct SurfaceDepthInputs
    {
        OceanParamsGPU Params{};
        OceanFFTParamsGPU FFT{};
        OceanFFTParamsGPU LocalFFT[kMaxOceanLocalFFTStreams]{};
        uint32 LocalFFTEnabled[kMaxOceanLocalFFTStreams]{};
    };
    SurfaceDepthInputs m_LatestDepthInputs{};
    // The inputs at the previous PublishDepthClaim.
    SurfaceDepthInputs m_ClaimedDepthInputs{};
    OceanOriginShiftNotifier m_OriginShiftNotifier;
    std::atomic<float32> m_WaveOriginOffsetX{0.0f};
    std::atomic<float32> m_WaveOriginOffsetZ{0.0f};
    // Authored underwater toggle (from OceanSurface.Underwater), set by the
    // extraction system. ANDed with the render-time submersion test in
    // SetUnderwaterActive so toggling the look off disables it regardless of
    // camera depth.
    std::atomic<bool> m_UnderwaterEnabled{false};
    // Underwater volumes (Crest volume / fly-through mode). Resolved each frame
    // by the extraction system; read by the render node's submersion gate. Guarded
    // by m_Mutex (written on the ECS thread, read on the render thread).
    std::vector<UnderwaterVolumeBox> m_UnderwaterVolumes;
    std::vector<UnderwaterVolumePolygon> m_UnderwaterVolumePolygons;
    std::vector<UnderwaterVolumeBox> m_UnderwaterExclusionVolumes;
    std::vector<UnderwaterVolumeBox> m_UnderwaterPortalOccluders;
    std::vector<WaterBodyBox> m_WaterBodies;
    std::vector<WaterBodyStamp> m_WaterBodyStamps;
    std::vector<WaterBodyPolygon> m_WaterBodyPolygons;
    std::vector<WaterBodyWaveBox> m_WaterBodyWaveBoxes;
    std::vector<WaterBodyWavePolygon> m_WaterBodyWavePolygons;
    std::vector<WaterBodyWaveTextureBox> m_WaterBodyWaveTextureBoxes;
    bool m_WaterBodiesConstrainSurface = false;

    // Authored flow + dynamic-wave toggles (from OceanSurface.Flow /
    // .DynamicWaves), set by the extraction system. The contributor reads them to
    // gate binding + the per-draw availability flags, so toggling either off
    // disables the effect regardless of whether a source/impulse is present.
    std::atomic<bool> m_FlowEnabled{false};
    std::atomic<bool> m_DynWavesEnabled{false};

    // Authored combine toggle (OceanRenderer.CombineDisplacementCascade).
    std::atomic<bool> m_CombineEnabled{false};

    // Authored clip + albedo toggles (from OceanSurface.ClipSurface / .Albedo), set
    // by the extraction system. The contributor reads them to gate binding + the
    // per-draw availability flags.
    std::atomic<bool> m_ClipEnabled{false};
    std::atomic<bool> m_AlbedoEnabled{false};
    std::atomic<float> m_PlanarReflectionScale{0.5f};

    // Authored cascade LOD-count limit (from OceanRenderer.LodCount). 0 = no
    // limit. Clamps the uploaded cascade layout's LodCount down (never up).
    std::atomic<uint32> m_LodCountLimit{0};

    // Authored cascade resolution controls (OceanRenderer). 0 = sim default.
    // Applied once per frame by ApplyCascadeConfig.
    std::atomic<float> m_CascadeBaseScale{0.0f};  // MinScale (LOD0 world extent)
    std::atomic<uint32> m_CascadeResolution{0};   // LodDataResolution (texels/axis)
    std::atomic<uint32> m_GeometryGridSize{0};    // generated surface grid cells per edge

    std::unique_ptr<OceanForwardContributor> m_ForwardContributor;
};

} // namespace GameEngine::Ocean
