// Camera + view registries and persistent per-view state, extracted from
// RenderServices (A1.2). Owns bookkeeping only: anything that blends view
// state with device writes, world lights, draw-builder queries, or spine
// values stays on RenderServices and reads through this object. RenderServices
// owns it BY VALUE (rs.Views()) — lifetime is identical to today's, and the
// world-pass execute lambdas read spans into this storage AFTER declaration
// returns, so the storage must never move/reallocate mid-frame.

#pragma once

#include "Rendering/CameraTypes.h"                // CameraData/Info, ViewDesc, ViewClearConfig, ViewTargets
#include "Rendering/Core/Device.h"                // IDevice, BufferHandle, kMaxSupportedFramesInFlight
#include "Engine/Rendering/AntiAliasing.h"        // AntiAliasingMode, jitter primitives
#include "Rendering/Core/Handle.h"                // TextureHandle, SamplerHandle
#include "Rendering/Core/RenderGraph/RGFrame.h"   // RGTexture, RGFrameStamp
#include "Rendering/Materials/ShaderVariantKey.h" // MaterialKeyword
#include "Engine/Rendering/CameraAspectRatio.h"   // ViewLetterbox, PixelPerfectViewState
#include "Engine/Rendering/DrawCommand.h"         // DrawCommand
#include "Engine/Rendering/DepthPassTypes.h"      // DepthPassType
#include "Components/Rendering/PostProcessVolume.h" // kDefaultManualExposureEv
#include "Engine/Rendering/PostProcessSettings.h" // PostProcessSettings
#include "Engine/Rendering/VolumetricFogTypes.h"  // VolumetricFogLocalVolume
#include "Types/Types.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine::Engine::Renderer
{

// Floor for any render scale, per-view or engine default. Below this the
// internal extent is too small for the post chain's fixed-radius kernels to
// stay meaningful and the upscale stops resembling the native image.
inline constexpr float kMinRenderScale = 0.25f;

// Ceiling: above 1.0 the split renders the world SUPERSAMPLED (SSAA) and the
// crossing filters back down. 2.0 = 4x the fill/shading cost — the practical
// limit before memory and bandwidth make it a footgun. Dynamic resolution
// stays <= 1.0 (its controller clamps its own MaxScale); only the fixed path
// supersamples.
inline constexpr float kMaxRenderScale = 2.0f;

// A render scale within this of 1.0 is treated as native: the split is skipped
// entirely so scale 1.0 is byte-identical to the pre-split path.
inline constexpr float kRenderScaleNativeEpsilon = 0.005f;

class ViewRegistry
{
public:
    // Per-camera exposure (the camera's sensor). Published from the rendered-camera sites (the editor
    // Game tab via GameViewController, and the Player) using Engine::Renderer::ToCameraExposure, and
    // consumed by the per-view post-process resolve so a camera-driven view exposes from its own camera
    // rather than the spatial volume. Mode is Components::ExposureMode as int32.
    struct CameraExposure
    {
        int32 Mode = 3; // 3 = Auto
        float32 Exposure = 1.0f;
        float32 ManualExposureEV = Components::kDefaultManualExposureEv;
        float32 ExposureCompensation = 0.0f;
        float32 Aperture = 16.0f;
        float32 ShutterTime = 0.01f;
        float32 Iso = 100.0f;
        float32 AutoExposureMinEv = 4.0f; // keep in lockstep with Components::Camera::AutoExposureMinEv
        float32 AutoExposureMaxEv = 18.0f;
        float32 AutoExposureSpeedUp = 1.0f;
        float32 AutoExposureSpeedDown = 3.0f;
        // Lens focus for physical DoF: focus-plane distance in world units, the
        // focal length ToCameraExposure derives from the camera's FovY, and the
        // sensor/gate height the derivation (and CoC-to-pixel scale) used.
        float32 FocusDistance = 10.0f;
        float32 FocalLengthMm = 20.78f;
        float32 SensorHeightMm = 24.0f;
        int32 ApertureBladeCount = 7;
        float32 ApertureRoundness = 1.0f;
        float32 ApertureRotation = 0.0f;
        float32 AnamorphicSqueeze = 1.0f;
        int32 FocusDebugMode = 0;
        float32 FocusDebugAlpha = 0.5f;
    };

    // Per-view shadow state set by ShadowMapNode during pipeline build. Only the
    // comparison sampler lives here: it is engine-lifetime state that the world
    // pass and the ocean god-ray overlay both bind. The cascade ARRAY is
    // deliberately absent — that texture is pool-owned and frame-local, so a
    // persistent copy of it would be a handle that outlives its image on any
    // frame the cascade arm does not import. Consumers take the array from this
    // frame's render-graph import or from the frame-stamped feature cache.
    struct ViewShadowResources
    {
        Rendering::SamplerHandle shadowSampler;
    };

    using PerFrameLightBuffers =
        std::array<Rendering::BufferHandle, Rendering::IDevice::kMaxSupportedFramesInFlight>;

    // Persistent per-view state, created on first touch and destroyed AS A UNIT
    // by ReleaseView — one erase point instead of scattered per-map erases (the
    // scattered form let m_WorldColorResolveOverrideByView leak: it had no
    // view-removal erase at all). The frame half lives in RenderServices'
    // ViewFrameRG: those values are RGFrame-local ids that die with their frame.
    struct PerViewResources
    {
        // Forward DrawCommand stream. Producers append during the emit step;
        // the world pass execute lambda walks it and records draws. Contents
        // are frame-scoped (cleared in BeginWorldDrawFrame); capacity persists.
        std::vector<DrawCommand> ForwardCommands;
        // Alpha-blended contributor draws that must execute after late surface
        // features (ocean/refraction) instead of being captured into them.
        std::vector<DrawCommand> LateForwardCommands;
        // Render-graph textures the ForwardCommands sample descriptor-direct
        // (EmitForwardSampledRead). AddWorldPassForView declares each as a
        // SampledVertex read so the writing sim orders before the draw and the
        // image returns to ShaderReadOnly. RGTexture ids are frame-local, so
        // the lifetime contract is strict: cleared with the command stream,
        // and consumed only when the frame identity stamped at emit time
        // matches the declaring frame (a foreign frame must not read them).
        std::vector<Rendering::RenderGraph::RGTexture> ForwardSampledRG;
        // Render-graph buffers the view's world draws consume as indirect args /
        // index / vertex-stage SSBOs (EmitForwardSampledBufferRead). A GPU-driven
        // node (CBT terrain, terrain grass) whose draw reads its own compute
        // pass's outputs registers them here with the access scope the draw
        // needs (Indirect for the arg buffer, Storage for the vertex/index
        // streams); AddWorldPassForView declares each so the producing compute
        // pass orders before the draw and lands the buffer barrier. Cleared with
        // the command stream. Each entry carries the frame it was imported into:
        // nodes emit from their own declaring frame, which need not be the frame
        // ForwardSampledFor stamps. An entry whose draws carry prepass heads
        // (ForwardBufferReaders::WorldPassAndPrepass) is declared by the view's
        // phase-A camera prepass too.
        struct ForwardSampledBuffer
        {
            Rendering::RenderGraph::RGBuffer Buffer{};
            Rendering::RenderGraph::RGBufferRead Access =
                Rendering::RenderGraph::RGBufferRead::Storage;
            Rendering::RenderGraph::RGFrameStamp For;
            ForwardBufferReaders Readers = ForwardBufferReaders::WorldPass;
        };
        std::vector<ForwardSampledBuffer> ForwardSampledBufferRG;
        Rendering::RenderGraph::RGFrameStamp ForwardSampledFor;
        // Forward draws of this frame that write their own depth
        // (ForwardDrawDepth::ColourPass). Non-zero keeps the view's world depth
        // writable under a prepass. Cleared with the command stream.
        uint32_t ForwardDrawsWritingDepth = 0;
        // Depth DrawCommand streams indexed by static_cast<size_t>(DepthPassType) —
        // sized by DepthPassType::Count so adding a pass type can't overflow it.
        // TransmittanceCascade has no contributor commands (glass draws come from
        // the entity bucketer), so its slot stays empty. Same lifetime as above.
        std::array<std::vector<DrawCommand>, static_cast<size_t>(DepthPassType::Count)>
            DepthCommands;
        // Prepass heads of ForwardDrawDepth::PrepassNonOccluding draws. The non-occluding prepass
        // draws them after DepthResolve; when it was not declared for the view this frame
        // (NonOccludingPrepassDeclared false at execution), the camera prepass draws them.
        // Same lifetime as the forward command stream.
        std::vector<DrawCommand> NonOccludingPrepassHeads;
        bool NonOccludingPrepassDeclared = false;

        ViewLetterbox Letterbox{}; // .active == false => no letterboxing
        // World-pass winding flip (planar reflections). false = normal.
        bool WorldPassFlipY = false;
        // The Parallax steps debug view: the world pass draws height-mapped materials as the number
        // of height samples their relief march took (MaterialKeyword::ParallaxStepsView).
        bool ParallaxStepsView = false;
        PixelPerfectViewState PixelPerfect{}; // .Active == false => inactive
        // Internal-resolution render scale for this view, unset to follow the
        // engine default (RenderServices::GetDefaultRenderScale). Per-view
        // because the editor scales one viewport while its siblings stay
        // native, and a Game View tab and a Scene View pane share the frame.
        std::optional<float> RenderScale;

        // Keywords in effect for the view's world pass, updated every frame by
        // AddWorldPassForView. The execute lambda reads this at runtime so keyword
        // changes (e.g. pipeline load after pass creation) take effect immediately.
        // nullopt => no world pass has recorded keywords yet.
        std::optional<Rendering::MaterialKeyword> WorldPassKeywords;

        // Render-target height of the view's world pass, snapshotted at pass
        // declaration each frame. The SSE-budget LOD scale reads it at the
        // (earlier) scatter-registration point, so it is one frame stale
        // across a resize — imperceptible for LOD switch points. 0 = no world
        // pass yet; readers choose their own answer for it rather than sharing
        // one — the SSE-budget scale substitutes kLodSseFallbackViewportH, while
        // grass placement treats 0 as "size unknown" and applies no width floor.
        uint32_t WorldViewportHeight = 0;

        // Sample count of the world pass's color attachment, snapshotted at the
        // same point as WorldViewportHeight (and one frame stale across an MSAA
        // toggle the same way). Grass reads it at the earlier forward-emit point
        // to pick alpha-to-coverage vs screen-door dither. 0 = no world pass yet
        // (readers treat that as single-sample).
        uint32_t WorldColorSampleCount = 0;

        // Attachment formats of the view's camera prepass (its depth target's format and sample
        // count), snapshotted when the prepass is declared and one frame stale the same way. A
        // forward producer pins its prepass head's pipeline at the earlier emit point and emits the
        // head only once the device has built that pipeline for these formats
        // (PipelineVariantCache::EnsureConcreteWarm). DepthFormat undefined = no prepass yet.
        Rendering::PipelineFormatKey PrepassFormatKey{};

        // Light UBO ring, one slot per device frame in flight: WriteViewLightBuffer
        // fills the slot GetFrameIndex() names inside an acquired frame while older
        // frames still read theirs. GPU-owned: device-destroyed by
        // DestroyPerViewGpuResources.
        PerFrameLightBuffers LightBuffers{};
        // HasTransmissionInView memo, stamped with the device frame index so it
        // self-invalidates next frame without manual clearing. {frameIndex, result}.
        std::pair<uint64_t, bool> TransmissionMemo{~0ull, false};
    };

    // — camera registry —
    Rendering::CameraId AllocateCamera(const char* debugName);
    bool ReleaseCamera(Rendering::CameraId id); // refuses while a view references it
    void SetCameraData(Rendering::CameraId id, const Rendering::CameraData& data);
    const Rendering::CameraData* FindCameraData(Rendering::CameraId id) const;
    const std::vector<Rendering::CameraInfo>& GetCameras() const { return m_Cameras; }

    void SetViewpointCamera(Rendering::CameraId id) { m_ViewpointCamera = id; }
    Rendering::CameraId GetViewpointCamera() const { return m_ViewpointCamera; }

    void SetCameraPostProcessMask(Rendering::CameraId id, uint32 mask);
    uint32 GetCameraPostProcessMask(Rendering::CameraId id) const;
    bool HasCameraPostProcessMask(Rendering::CameraId id) const;
    void SetCameraExposure(Rendering::CameraId id, const CameraExposure& exposure);
    const CameraExposure* FindCameraExposure(Rendering::CameraId id) const;

    // — view registry (names unchanged from RenderServices) —
    Rendering::ViewId AllocateView(const char* debugName, Rendering::CameraId cameraId,
                                   Rendering::ViewPurpose purpose = Rendering::ViewPurpose::Game,
                                   Rendering::ViewParticipation participation =
                                       Rendering::ViewParticipation::Always);
    void RequestViewFrame(Rendering::ViewId id);
    void MarkViewExtracted(Rendering::ViewId id, uint64 frameIndex);
    void MarkWorldExtracted(uint64 worldId, uint64 frameIndex);
    // True when the view was included in its world's LATEST extraction pass.
    // World-relative on purpose: the editor renders on demand, so the global
    // frame counter advances past the last extraction on idle frames — a view
    // is stale only when extraction ran without it (activation raced past this
    // frame's extraction), never merely because the frame is quiet.
    bool IsViewExtractionCurrent(Rendering::ViewId id) const;
    // Unit-destroys PerViewResources (GPU+CPU) + erases forcedLOD/pp/fog/warned
    // (A1), THEN fires the view-released callback — once per successful release.
    bool ReleaseView(Rendering::ViewId id);
    const Rendering::ViewDesc* FindViewDesc(Rendering::ViewId id) const;
    const std::vector<Rendering::ViewDesc>& GetViews() const { return m_Views; }
    Rendering::CameraData ResolveCameraData(Rendering::ViewId viewId) const;

    // — per-view anti-aliasing (two-domain rule) —
    // Registered for the modes that need per-view AA state: the jittered ones
    // (TAA, FXAA) plus SMAA, which is spatial and leaves the offsets zero but
    // whose resolve node gates on the mode recorded here.
    // The registry's stored CameraData is the LOGIC domain and stays unjittered
    // forever: culling, LOD, cascade fitting, the CascadeShadowCache key
    // (exact-memcmp on viewProj), volumetric-fog reprojection, and picking all
    // read it via FindCameraData/ResolveCameraData and are correct by
    // construction. Jitter exists only in the RASTER domain: the per-view GPU
    // upload seams (depth prepass, world color, transmissive, ViewParams)
    // resolve through ResolveJitteredCameraData instead.
    struct ViewAntiAliasing
    {
        bool Enabled = false;
        // Which AA path owns this view, and what the resolve nodes gate on.
        // TAA rotates a Halton(2,3) sequence; TemporalFXAA rotates the
        // two-phase axis-alternating pair (TemporalFxaaJitterOffset); FXAA and SMAA
        // are unjittered and leave every offset below at zero.
        AntiAliasingMode Mode = AntiAliasingMode::TAA;
        uint32 SequenceLength = 8; // Halton cycle (TAA); TemporalFXAA uses its 2-phase pair
        uint32 Phase = 0;
        float JitterX = 0.0f; // texels, [-0.5, 0.5)
        float JitterY = 0.0f;
        float PrevJitterX = 0.0f;
        float PrevJitterY = 0.0f;
        // NDC-space offsets, frozen at the FIRST jittered resolve of each
        // frame and reused verbatim by every later seam. This is what makes
        // the depth prepass and the world pass rasterize with bit-identical
        // clip positions (the world depth-tests GreaterOrEqual against the
        // prepass) even if seams disagree about viewport extent sources
        // (letterbox sub-rect vs texture size).
        float NdcJitterX = 0.0f;
        float NdcJitterY = 0.0f;
        float PrevNdcJitterX = 0.0f;
        float PrevNdcJitterY = 0.0f;
        bool NdcFrozen = false;
        // Unjittered camera snapshots for the resolve's reprojection: Curr is
        // the frame being declared, Prev the previous rendered frame's.
        Rendering::CameraData CurrCamera{};
        Rendering::CameraData PrevCamera{};
        uint64 FrameStamp = ~0ull;
        // True once Prev* hold a real rendered frame's snapshot — the last
        // frame this view advanced, however long ago. A frame gap (OnDemand
        // lapse, hidden panel) does not invalidate reprojection: PrevCamera is
        // exactly the camera that rendered the surviving history. Whether that
        // history's physical still holds those texels is the render-graph
        // pool's freshness arm, not the camera pair's.
        bool PrevValid = false;
    };
    // `mode` selects the jitter generator (TAA Halton vs TemporalFXAA edge
    // walk, none for FXAA and SMAA) and is what the TemporalAA / TemporalFxaa
    // / Smaa nodes gate on. `sequenceLength` is only consulted for TAA;
    // TemporalFXAA forces its two-phase axis-alternating cycle, the spatial modes a
    // single phase. `enabled` false drops the
    // state entirely — the modes with no per-view state (Off, MSAA) register
    // nothing (UsesPerViewAntiAliasingState).
    void SetViewAntiAliasing(Rendering::ViewId id, bool enabled, AntiAliasingMode mode,
                             uint32 sequenceLength);
    const ViewAntiAliasing* FindViewAntiAliasing(Rendering::ViewId id) const;
    // Rotate the per-view jitter/camera-snapshot state to `frameIndex` (first
    // caller of a frame rotates; later callers are no-ops) and return it.
    // Null when the view registered no AA state.
    const ViewAntiAliasing* AdvanceViewAntiAliasing(Rendering::ViewId viewId, uint64 frameIndex);
    // Raster-domain resolve: ResolveCameraData + this frame's sub-pixel jitter
    // applied to proj/viewProj/viewProjRel (exact NDC offset; depth untouched —
    // see ApplyNdcJitter). Identical to ResolveCameraData when TAA is off.
    Rendering::CameraData ResolveJitteredCameraData(Rendering::ViewId viewId, uint64 frameIndex,
                                                    uint32 renderWidth, uint32 renderHeight);

    void SetViewCamera(Rendering::ViewId id, Rendering::CameraId cameraId);
    void SetViewRenderLayerMask(Rendering::ViewId id, uint32 mask);
    void SetViewActiveRenderPipeline(Rendering::ViewId id, bool enabled);

    void SetViewWorldId(Rendering::ViewId id, uint64 worldId);
    void SetViewCullingStrategy(Rendering::ViewId id,
                                std::shared_ptr<Rendering::ICullingStrategy> strategy);

    void SetViewTargets(Rendering::ViewId id, Rendering::ViewTextureHandle color,
                        Rendering::ViewTextureHandle depth, Rendering::ViewTextureHandle resolve = 0,
                        Rendering::ViewClearConfig clear = Rendering::ViewClearConfig{});
    void SetViewClearConfig(Rendering::ViewId id, Rendering::ViewClearConfig clear);
    void ClearViewTargets(Rendering::ViewId id);

    void SetViewLetterbox(Rendering::ViewId id, ViewLetterbox letterbox);
    ViewLetterbox GetViewLetterbox(Rendering::ViewId id) const;
    void SetViewWorldPassFlipY(Rendering::ViewId id, bool enabled);
    bool GetViewWorldPassFlipY(Rendering::ViewId id) const;
    // PerViewResources::ParallaxStepsView: whether this view's world pass draws the Parallax steps
    // debug view instead of lit parallax materials.
    void SetViewParallaxStepsView(Rendering::ViewId id, bool shown);
    bool GetViewParallaxStepsView(Rendering::ViewId id) const;
    // PerViewResources::WorldColorSampleCount (0 until the view's first world pass).
    uint32 GetViewWorldColorSampleCount(Rendering::ViewId id) const;
    // PerViewResources::PrepassFormatKey (DepthFormat undefined until the view's first prepass).
    Rendering::PipelineFormatKey GetViewPrepassFormatKey(Rendering::ViewId id) const;
    // PerViewResources::WorldViewportHeight in pixels, after render scale (0 until the view's
    // first world pass). Callers converting a world size to a screen size need this paired with
    // the projection's Y scale; 0 means "not known yet" and must not be divided by.
    uint32 GetViewWorldViewportHeight(Rendering::ViewId id) const;
    void SetViewPixelPerfect(Rendering::ViewId id, const PixelPerfectViewState& state);
    PixelPerfectViewState GetViewPixelPerfect(Rendering::ViewId id) const;
    // Per-view internal render scale. Pass std::nullopt to follow the engine
    // default; a value is clamped to [kMinRenderScale, kMaxRenderScale]
    // (above 1.0 = supersampling). Read the resolved
    // value through RenderServices::ResolveViewRenderScale, which folds in the
    // default — this pair is storage only.
    void SetViewRenderScale(Rendering::ViewId id, std::optional<float> scale);
    std::optional<float> GetViewRenderScale(Rendering::ViewId id) const;

    void SetViewPostProcessOverride(Rendering::ViewId viewId, const PostProcessSettings& settings);
    void ClearViewPostProcessOverride(Rendering::ViewId viewId);
    void SetViewVolumetricFogVolumesOverride(Rendering::ViewId viewId,
                                             std::vector<VolumetricFogLocalVolume> volumes);
    void ClearViewVolumetricFogVolumesOverride(Rendering::ViewId viewId);

    // Signal that a pipeline pass has already cleared/written a view's color target.
    // The world pass will use LoadOp::Load instead of Clear for that view.
    void MarkViewColorInitialized(Rendering::ViewId viewId)
    {
        if (viewId != 0)
            m_PipelineViewsWithColorInit.insert(viewId);
    }
    bool IsViewColorInitialized(Rendering::ViewId viewId) const
    {
        return m_PipelineViewsWithColorInit.find(viewId) != m_PipelineViewsWithColorInit.end();
    }
    // Pipeline sky pass will draw the backdrop for this view before the world pass.
    void MarkViewSkyBackdropScheduled(Rendering::ViewId viewId)
    {
        if (viewId != 0)
            m_PipelineViewsWithSkyBackdrop.insert(viewId);
    }
    bool IsViewSkyBackdropScheduled(Rendering::ViewId viewId) const
    {
        return m_PipelineViewsWithSkyBackdrop.find(viewId) != m_PipelineViewsWithSkyBackdrop.end();
    }

    void SetViewShadowSampler(Rendering::ViewId viewId, Rendering::SamplerHandle shadowSampler)
    {
        m_ViewShadowResources[viewId] = {shadowSampler};
    }
    const ViewShadowResources* GetViewShadowResources(Rendering::ViewId viewId) const
    {
        const auto it = m_ViewShadowResources.find(viewId);
        return it != m_ViewShadowResources.end() ? &it->second : nullptr;
    }

    // Per-view LOD force override (setter only; ResolveViewForceLOD stays on
    // RenderServices, blending this against the global force level).
    void SetViewForcedLOD(Rendering::ViewId viewId, uint32_t level)
    {
        if (level == 0xFFFFFFFFu)
            m_ViewForcedLOD.erase(viewId);
        else
            m_ViewForcedLOD[viewId] = level;
    }

    // — per-view resource storage (RenderServices internals + draw streams reach in here) —
    PerViewResources& PerView(Rendering::ViewId id) { return m_PerView[id]; }
    PerViewResources* FindPerView(Rendering::ViewId id)
    {
        const auto it = m_PerView.find(id);
        return it == m_PerView.end() ? nullptr : &it->second;
    }
    const PerViewResources* FindPerView(Rendering::ViewId id) const
    {
        const auto it = m_PerView.find(id);
        return it == m_PerView.end() ? nullptr : &it->second;
    }
    template <typename Fn>
    void ForEachPerView(Fn&& fn)
    {
        for (auto& [id, pv] : m_PerView)
        {
            (void)id;
            fn(pv);
        }
    }
    template <typename Fn>
    void ForEachPerView(Fn&& fn) const
    {
        for (const auto& [id, pv] : m_PerView)
        {
            (void)id;
            fn(pv);
        }
    }

    // Per-view override lookups for the effective-settings blenders and the LOD
    // resolver, all of which stay on RenderServices (they combine a registry
    // override with a world/global fallback). Null => no override for this view.
    const uint32_t* FindViewForcedLOD(Rendering::ViewId id) const
    {
        const auto it = m_ViewForcedLOD.find(id);
        return it == m_ViewForcedLOD.end() ? nullptr : &it->second;
    }
    const PostProcessSettings* FindViewPostProcessOverride(Rendering::ViewId id) const
    {
        const auto it = m_ViewPostProcessOverrides.find(id);
        return it == m_ViewPostProcessOverrides.end() ? nullptr : &it->second;
    }
    const std::vector<VolumetricFogLocalVolume>*
    FindViewVolumetricFogVolumesOverride(Rendering::ViewId id) const
    {
        const auto it = m_ViewVolumetricFogVolumesOverrides.find(id);
        return it == m_ViewVolumetricFogVolumesOverrides.end() ? nullptr : &it->second;
    }
    // One-shot per-view worldless-culling diagnostic: true the first time a view
    // is marked (the caller then logs), false thereafter.
    bool MarkWorldlessViewWarned(Rendering::ViewId id)
    {
        return m_WarnedWorldlessViews.insert(id).second;
    }

    // Device-destroys the GPU-owned members of one entry and resets them to
    // invalid; CPU-side fields are untouched (device teardown must not lose
    // view state). Asserts a live LightBuffers handle implies a live device.
    void DestroyPerViewGpuResources(PerViewResources& pv);

    // — frame + device hooks (called by RenderServices only) —
    // Per-frame per-view resets: OnDemand participation aging, the pipeline-view
    // flag sets, per-view WorldPassKeywords, and the per-view shadow-resource
    // map. Called from BeginWorldDrawFrame ONLY (exactly once per app frame,
    // sec 0a-A3-i) — never from the per-window BuildFrameGraph.
    void BeginFrame();
    // Set at RenderServices::Initialize; nulled WITH RenderServices::m_Device in
    // Shutdown (sec 0a-A2), not at DestroyAllPerViewGpuResources. The destructor
    // never touches the device.
    void SetDevice(Rendering::IDevice* device) { m_Device = device; }
    void DestroyAllPerViewGpuResources(); // device teardown; CPU state survives
    /// Drop every per-view GPU handle WITHOUT destroying it, for the in-place
    /// device rebuild: the buffers died with the old device, so destroying them
    /// would hand old handles to the new one. Zeroing is what the lazy create in
    /// WriteViewLightBuffer needs — its `!IsValid()` guard cannot fire while a
    /// handle from a dead device still reads valid. The per-view draw streams are
    /// emptied too: their recorded draws name the dead device's buffers and sets.
    void ForgetPerViewGpuResourcesAfterDeviceRebuild();
    void SetViewReleasedCallback(std::function<void(Rendering::ViewId)> fn)
    {
        m_OnViewReleased = std::move(fn);
    }

private:
    // Logical camera registry (shared across ECS and editor callers).
    std::vector<Rendering::CameraInfo> m_Cameras;
    Rendering::CameraId m_NextCameraId{1};
    // Viewpoint camera for camera-following detail systems (ocean LOD cascades).
    // 0 = follow each view's own camera. See SetViewpointCamera.
    Rendering::CameraId m_ViewpointCamera{0};

    // Logical view registry. Stays a std::vector — iteration order (= allocation
    // order) drives the per-view node loop and therefore pass declaration order.
    std::vector<Rendering::ViewDesc> m_Views;
    Rendering::ViewId m_NextViewId{1};
    std::unordered_map<Rendering::ViewId, uint64> m_ViewExtractionFrames;
    std::unordered_map<uint64, uint64> m_WorldExtractionFrames;

    // Per-camera post-process layer mask (defaults to 0xFFFFFFFFu when absent).
    std::unordered_map<Rendering::CameraId, uint32> m_CameraPostProcessMasks;
    // Per-camera exposure (absent => the view falls back to volume/world exposure).
    std::unordered_map<Rendering::CameraId, CameraExposure> m_CameraExposures;

    std::unordered_map<Rendering::ViewId, PerViewResources> m_PerView;
    // Per-view shadow resources (set by ShadowMapNode, cleared per frame).
    std::unordered_map<Rendering::ViewId, ViewShadowResources> m_ViewShadowResources;
    // Per-view force overrides (absent => use the global). Erased on ReleaseView.
    std::unordered_map<Rendering::ViewId, uint32_t> m_ViewForcedLOD;
    // Per-view override halves (world-fallback halves + the GetEffective* blenders
    // stay on RenderServices). Erased on ReleaseView.
    std::unordered_map<Rendering::ViewId, PostProcessSettings> m_ViewPostProcessOverrides;
    std::unordered_map<Rendering::ViewId, std::vector<VolumetricFogLocalVolume>>
        m_ViewVolumetricFogVolumesOverrides;
    // Per-view temporal-AA jitter + reprojection snapshots. Entry present only
    // while TAA is enabled for the view. Erased on ReleaseView.
    std::unordered_map<Rendering::ViewId, ViewAntiAliasing> m_ViewAntiAliasing;
    // Per-view per-frame flags; frame-cleared in BeginFrame, so no release-erase.
    std::unordered_set<Rendering::ViewId> m_PipelineViewsWithColorInit;
    std::unordered_set<Rendering::ViewId> m_PipelineViewsWithSkyBackdrop;
    // One-shot per-view diagnostic set. Erased on ReleaseView.
    std::unordered_set<Rendering::ViewId> m_WarnedWorldlessViews;

    Rendering::IDevice* m_Device = nullptr; // never touched by the destructor (A2)
    std::function<void(Rendering::ViewId)> m_OnViewReleased;
};

} // namespace GameEngine::Engine::Renderer
