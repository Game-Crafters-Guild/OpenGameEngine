// ViewRegistry.cpp — camera/view registries and persistent per-view state,
// extracted from RenderServices (A1.2 S1). Bodies here were moved verbatim from
// RenderServicesViewState.cpp / GpuDriven.cpp / DepthShadowPasses.cpp; the only
// behavior change is A1 (ReleaseView erases four historically-leaking per-view
// maps and fires the released callback once per successful release).
#include "Engine/Rendering/ViewRegistry.h"

#include "Engine/Rendering/RenderOrigin.h"
#include "Logger/Logger.h"
#include "Mathematics/Matrix4x4.h"
#include "Rendering/Core/Device.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

CameraId ViewRegistry::AllocateCamera(const char* debugName)
{
    CameraInfo info{};
    info.id = m_NextCameraId++;
    info.debugName = debugName;
    m_Cameras.push_back(info);
    return info.id;
}

bool ViewRegistry::ReleaseCamera(CameraId id)
{
    if (id == 0)
        return false;
    // Don't release if any view still references this camera.
    for (const auto& v : m_Views)
    {
        if (v.cameraId == id)
        {
            return false;
        }
    }

    const auto it = std::find_if(m_Cameras.begin(), m_Cameras.end(), [id](const CameraInfo& c)
                                 { return c.id == id; });
    if (it == m_Cameras.end())
        return false;
    m_Cameras.erase(it);
    // Drop the per-camera side tables so a recycled CameraId can't inherit a
    // previous camera's exposure or post-process mask.
    m_CameraExposures.erase(id);
    m_CameraPostProcessMasks.erase(id);
    return true;
}

void ViewRegistry::SetCameraData(CameraId id, const CameraData& data)
{
    for (auto& cam : m_Cameras)
    {
        if (cam.id == id)
        {
            cam.data = data;
            return;
        }
    }
}

const CameraData* ViewRegistry::FindCameraData(CameraId id) const
{
    for (const auto& cam : m_Cameras)
    {
        if (cam.id == id)
        {
            return &cam.data;
        }
    }
    return nullptr;
}

void ViewRegistry::SetCameraPostProcessMask(CameraId id, uint32 mask)
{
    m_CameraPostProcessMasks[id] = mask;
}

uint32 ViewRegistry::GetCameraPostProcessMask(CameraId id) const
{
    const auto it = m_CameraPostProcessMasks.find(id);
    if (it == m_CameraPostProcessMasks.end())
        return 0xFFFFFFFFu;
    return it->second;
}

bool ViewRegistry::HasCameraPostProcessMask(CameraId id) const
{
    return m_CameraPostProcessMasks.find(id) != m_CameraPostProcessMasks.end();
}

void ViewRegistry::SetCameraExposure(CameraId id, const CameraExposure& exposure)
{
    m_CameraExposures[id] = exposure;
}

const ViewRegistry::CameraExposure* ViewRegistry::FindCameraExposure(CameraId id) const
{
    const auto it = m_CameraExposures.find(id);
    return it == m_CameraExposures.end() ? nullptr : &it->second;
}

CameraData ViewRegistry::ResolveCameraData(ViewId viewId) const
{
    const Mathematics::Matrix4x4 identity = Mathematics::Matrix4x4::Identity();
    CameraData cam{};
    std::memcpy(cam.view, identity.Data(), 64);
    std::memcpy(cam.proj, identity.Data(), 64);
    std::memcpy(cam.viewProj, identity.Data(), 64);

    CameraId cameraId = 0;
    for (const auto& v : m_Views)
    {
        if (v.id == viewId)
        {
            cameraId = v.cameraId;
            break;
        }
    }
    if (cameraId != 0)
    {
        for (const auto& c : m_Cameras)
        {
            if (c.id == cameraId)
            {
                cam = c.data;
                break;
            }
        }
    }

    // Camera-relative rendering: derive the render origin from the (full-world)
    // camera position and fill the rebased view/viewProj the vertex stage uses
    // for a precise clip position. view/proj/viewProj/cameraPos stay full-world,
    // so culling, ViewParams (FindCameraData path), and every fragment consumer
    // are unaffected. Inside the activation radius the sector is (0,0,0) and the
    // rebased matrices equal the full-world ones bit-for-bit.
    ComputeRebasedView(cam.view, cam.proj, cam.viewProj, cam.cameraPos[0], cam.cameraPos[1],
                       cam.cameraPos[2], cam.viewRel, cam.viewProjRel, cam.renderOriginSector);
    return cam;
}

void ViewRegistry::SetViewAntiAliasing(ViewId id, bool enabled, AntiAliasingMode mode,
                                       uint32 sequenceLength)
{
    if (id == 0)
        return;
    if (!enabled)
    {
        m_ViewAntiAliasing.erase(id);
        return;
    }
    ViewAntiAliasing& aa = m_ViewAntiAliasing[id];
    aa.Enabled = true;
    // FXAA and SMAA are spatial-only: the state exists so their nodes can
    // gate on the view's mode, but the raster stays unjittered (sequence of
    // 1, zero offset below).
    uint32 length = sequenceLength > 0u ? sequenceLength : 8u;
    if (mode == AntiAliasingMode::TemporalFXAA)
        length = kFxaaJitterSequenceLength;
    else if (!IsJitteredAntiAliasingMode(mode))
        length = 1u;
    if (aa.Mode != mode || aa.SequenceLength != length)
    {
        aa.Mode = mode;
        aa.SequenceLength = length;
        aa.Phase = 0u;
    }
}

const ViewRegistry::ViewAntiAliasing* ViewRegistry::FindViewAntiAliasing(ViewId id) const
{
    const auto it = m_ViewAntiAliasing.find(id);
    return it == m_ViewAntiAliasing.end() ? nullptr : &it->second;
}

const ViewRegistry::ViewAntiAliasing* ViewRegistry::AdvanceViewAntiAliasing(ViewId viewId,
                                                                        uint64 frameIndex)
{
    const auto it = m_ViewAntiAliasing.find(viewId);
    if (it == m_ViewAntiAliasing.end() || !it->second.Enabled)
        return nullptr;
    ViewAntiAliasing& taa = it->second;
    if (taa.FrameStamp != frameIndex)
    {
        // First raster-domain resolve of this frame rotates; SceneViewController
        // produces CameraData twice per frame (Update + Declare), so keying on
        // the frame index — never call count — is what keeps one jitter sample
        // per rendered frame.
        // Prev* snapshot the LAST RENDERED frame, however long ago: an OnDemand
        // view that lapses (hidden tab, collapsed pane) resumes with a frame
        // gap, and its surviving history was written by exactly that last
        // rendered camera — so the pair stays valid for reprojection. Whether
        // the history PHYSICAL survived is the render-graph pool's freshness
        // arm (TemporalAANode), not the camera pair's concern.
        taa.PrevValid = (taa.FrameStamp != ~0ull);
        taa.PrevCamera = taa.CurrCamera;
        taa.PrevJitterX = taa.JitterX;
        taa.PrevJitterY = taa.JitterY;
        taa.PrevNdcJitterX = taa.NdcJitterX;
        taa.PrevNdcJitterY = taa.NdcJitterY;
        taa.NdcFrozen = false;
        taa.CurrCamera = ResolveCameraData(viewId);
        taa.Phase = (taa.Phase + 1u) % taa.SequenceLength;
        if (taa.Mode == AntiAliasingMode::TemporalFXAA)
            TemporalFxaaJitterOffset(taa.Phase, taa.JitterX, taa.JitterY);
        else if (!IsJitteredAntiAliasingMode(taa.Mode))
        {
            taa.JitterX = 0.0f;
            taa.JitterY = 0.0f;
        }
        else
            TemporalJitterOffset(taa.Phase, taa.SequenceLength, taa.JitterX, taa.JitterY);
        taa.FrameStamp = frameIndex;
    }
    return &taa;
}

CameraData ViewRegistry::ResolveJitteredCameraData(ViewId viewId, uint64 frameIndex,
                                                   uint32 renderWidth, uint32 renderHeight)
{
    CameraData cam = ResolveCameraData(viewId);
    if (AdvanceViewAntiAliasing(viewId, frameIndex) == nullptr)
        return cam;
    ViewAntiAliasing& taa = m_ViewAntiAliasing.find(viewId)->second;
    if (!taa.NdcFrozen)
    {
        if (renderWidth == 0u || renderHeight == 0u)
            return cam; // no extent yet: stay unjittered, the next seam freezes
        taa.NdcJitterX = 2.0f * taa.JitterX / static_cast<float>(renderWidth);
        taa.NdcJitterY = 2.0f * taa.JitterY / static_cast<float>(renderHeight);
        taa.NdcFrozen = true;
    }
    ApplyNdcJitter(cam.proj, taa.NdcJitterX, taa.NdcJitterY);
    ApplyNdcJitter(cam.viewProj, taa.NdcJitterX, taa.NdcJitterY);
    ApplyNdcJitter(cam.viewProjRel, taa.NdcJitterX, taa.NdcJitterY);
    return cam;
}

ViewId ViewRegistry::AllocateView(const char* debugName, CameraId cameraId,
                                  Rendering::ViewPurpose purpose,
                                  Rendering::ViewParticipation participation)
{
    ViewDesc desc{};
    desc.id = m_NextViewId++;
    desc.cameraId = cameraId;
    desc.debugName = debugName;
    desc.purpose = purpose;
    desc.participation = participation;
    // targets remain zero until SetViewTargets is called
    m_Views.push_back(desc);
    return desc.id;
}

void ViewRegistry::RequestViewFrame(ViewId id)
{
    for (auto& view : m_Views)
    {
        if (view.id == id)
        {
            // 2 = the remainder of THIS frame's declaration plus the next full
            // frame (whose extraction runs before the requester's next declare
            // and is the one the request feeds). BeginFrame decrements.
            view.participationFrames = 2;
            return;
        }
    }
}

void ViewRegistry::MarkViewExtracted(ViewId id, uint64 frameIndex)
{
    if (id != 0)
        m_ViewExtractionFrames[id] = frameIndex;
}

void ViewRegistry::MarkWorldExtracted(uint64 worldId, uint64 frameIndex)
{
    m_WorldExtractionFrames[worldId] = frameIndex;
}

bool ViewRegistry::IsViewExtractionCurrent(ViewId id) const
{
    const ViewDesc* view = FindViewDesc(id);
    if (!view)
        return false;
    const auto worldIt = m_WorldExtractionFrames.find(view->worldId);
    if (worldIt == m_WorldExtractionFrames.end())
        return false; // no extraction has run for this world yet (first frames)
    const auto viewIt = m_ViewExtractionFrames.find(id);
    return viewIt != m_ViewExtractionFrames.end() && viewIt->second == worldIt->second;
}

const ViewDesc* ViewRegistry::FindViewDesc(ViewId id) const
{
    for (const auto& v : m_Views)
    {
        if (v.id == id)
        {
            return &v;
        }
    }
    return nullptr;
}

bool ViewRegistry::ReleaseView(ViewId id)
{
    if (id == 0)
        return false;

    // Destroy per-view registry state first (idempotent — a double-release finds
    // nothing to erase). The A1 erase set (forcedLOD/pp/fog/warned) joins the
    // PerViewResources + shadow-resource teardown at this single point: those
    // four maps were insert-only and leaked across probe/thumbnail/quad-view
    // churn. ViewIds are monotonic, so this is memory hygiene, not stale-data.
    if (auto itPv = m_PerView.find(id); itPv != m_PerView.end())
    {
        DestroyPerViewGpuResources(itPv->second);
        m_PerView.erase(itPv);
    }
    m_ViewShadowResources.erase(id);
    m_ViewForcedLOD.erase(id);
    m_ViewPostProcessOverrides.erase(id);
    m_ViewVolumetricFogVolumesOverrides.erase(id);
    m_ViewAntiAliasing.erase(id);
    m_WarnedWorldlessViews.erase(id);
    m_ViewExtractionFrames.erase(id);

    const auto it = std::find_if(m_Views.begin(), m_Views.end(), [id](const ViewDesc& v)
                                 { return v.id == id; });
    if (it == m_Views.end())
        return false; // double-release / unknown id: no-op, callback does NOT fire

    m_Views.erase(it);

    // The one cross-subsystem lifecycle edge (WorldDrawBuilder::ClearView, wired
    // by RenderServices::Initialize): registry state is torn down above, THEN the
    // callback fires — once per successful release.
    if (m_OnViewReleased)
        m_OnViewReleased(id);
    return true;
}

void ViewRegistry::SetViewCamera(ViewId id, CameraId cameraId)
{
    for (auto& view : m_Views)
    {
        if (view.id == id)
        {
            view.cameraId = cameraId;
            return;
        }
    }
}

void ViewRegistry::SetViewRenderLayerMask(ViewId id, uint32 mask)
{
    for (auto& view : m_Views)
    {
        if (view.id == id)
        {
            view.renderLayerMask = mask;
            return;
        }
    }
}

void ViewRegistry::SetViewActiveRenderPipeline(ViewId id, bool enabled)
{
    for (auto& view : m_Views)
    {
        if (view.id == id)
        {
            view.activeRenderPipeline = enabled;
            return;
        }
    }
}

void ViewRegistry::SetViewWorldId(ViewId id, uint64 worldId)
{
    for (auto& view : m_Views)
    {
        if (view.id == id)
        {
            view.worldId = worldId;
            return;
        }
    }
}

void ViewRegistry::SetViewCullingStrategy(ViewId id,
                                          std::shared_ptr<Rendering::ICullingStrategy> strategy)
{
    for (auto& view : m_Views)
    {
        if (view.id == id)
        {
            view.cullingStrategy = std::move(strategy);
            return;
        }
    }
}

void ViewRegistry::SetViewLetterbox(ViewId id, ViewLetterbox letterbox)
{
    if (!letterbox.active)
    {
        if (auto* pv = FindPerView(id))
            pv->Letterbox = {};
        return;
    }
    PerView(id).Letterbox = letterbox;
}

ViewLetterbox ViewRegistry::GetViewLetterbox(ViewId id) const
{
    const auto* pv = FindPerView(id);
    return pv ? pv->Letterbox : ViewLetterbox{};
}

void ViewRegistry::SetViewWorldPassFlipY(ViewId id, bool enabled)
{
    if (!enabled)
    {
        if (auto* pv = FindPerView(id))
            pv->WorldPassFlipY = false;
        return;
    }
    PerView(id).WorldPassFlipY = true;
}

bool ViewRegistry::GetViewWorldPassFlipY(ViewId id) const
{
    const auto* pv = FindPerView(id);
    return pv && pv->WorldPassFlipY;
}

void ViewRegistry::SetViewParallaxStepsView(ViewId id, bool shown)
{
    if (!shown)
    {
        if (auto* pv = FindPerView(id))
            pv->ParallaxStepsView = false;
        return;
    }
    PerView(id).ParallaxStepsView = true;
}

bool ViewRegistry::GetViewParallaxStepsView(ViewId id) const
{
    const auto* pv = FindPerView(id);
    return pv && pv->ParallaxStepsView;
}

uint32 ViewRegistry::GetViewWorldColorSampleCount(ViewId id) const
{
    const auto* pv = FindPerView(id);
    return pv ? pv->WorldColorSampleCount : 0u;
}

Rendering::PipelineFormatKey ViewRegistry::GetViewPrepassFormatKey(ViewId id) const
{
    const auto* pv = FindPerView(id);
    return pv ? pv->PrepassFormatKey : Rendering::PipelineFormatKey{};
}

uint32 ViewRegistry::GetViewWorldViewportHeight(ViewId id) const
{
    const auto* pv = FindPerView(id);
    return pv ? pv->WorldViewportHeight : 0u;
}

void ViewRegistry::SetViewPixelPerfect(ViewId id, const PixelPerfectViewState& state)
{
    if (!state.Active)
    {
        if (auto* pv = FindPerView(id))
            pv->PixelPerfect = {};
        return;
    }
    PerView(id).PixelPerfect = state;
}

PixelPerfectViewState ViewRegistry::GetViewPixelPerfect(ViewId id) const
{
    const auto* pv = FindPerView(id);
    return pv ? pv->PixelPerfect : PixelPerfectViewState{};
}

void ViewRegistry::SetViewRenderScale(ViewId id, std::optional<float> scale)
{
    if (!scale.has_value())
    {
        if (auto* pv = FindPerView(id))
            pv->RenderScale.reset();
        return;
    }
    // NaN / non-positive collapses to native rather than to the floor: a bad
    // value must not silently halve a viewport's resolution.
    const float requested = *scale;
    PerView(id).RenderScale =
        (requested > 0.0f) ? std::clamp(requested, kMinRenderScale, kMaxRenderScale) : 1.0f;
}

std::optional<float> ViewRegistry::GetViewRenderScale(ViewId id) const
{
    const auto* pv = FindPerView(id);
    return pv ? pv->RenderScale : std::nullopt;
}

void ViewRegistry::SetViewPostProcessOverride(ViewId viewId, const PostProcessSettings& settings)
{
    m_ViewPostProcessOverrides[viewId] = settings;
}

void ViewRegistry::ClearViewPostProcessOverride(ViewId viewId)
{
    m_ViewPostProcessOverrides.erase(viewId);
}

void ViewRegistry::SetViewVolumetricFogVolumesOverride(ViewId viewId,
                                                       std::vector<VolumetricFogLocalVolume> volumes)
{
    m_ViewVolumetricFogVolumesOverrides[viewId] = std::move(volumes);
}

void ViewRegistry::ClearViewVolumetricFogVolumesOverride(ViewId viewId)
{
    m_ViewVolumetricFogVolumesOverrides.erase(viewId);
}

void ViewRegistry::SetViewTargets(ViewId id,
                                  ViewTextureHandle color,
                                  ViewTextureHandle depth,
                                  ViewTextureHandle resolve,
                                  ViewClearConfig clear)
{
    for (auto& view : m_Views)
    {
        if (view.id == id)
        {
            view.targets.color = color;
            view.targets.depth = depth;
            view.targets.resolve = resolve;
            view.targets.colorSampleCount = 0;

            // Clear configuration
            view.targets.clearColor = clear.clearColor;
            view.targets.clearDepth = clear.clearDepth;
            view.targets.clearStencil = clear.clearStencil;
            view.targets.clearColorValue[0] = clear.clearColorValue[0];
            view.targets.clearColorValue[1] = clear.clearColorValue[1];
            view.targets.clearColorValue[2] = clear.clearColorValue[2];
            view.targets.clearColorValue[3] = clear.clearColorValue[3];
            view.targets.clearDepthValue = clear.clearDepthValue;
            view.targets.clearStencilValue = clear.clearStencilValue;

#if defined(_DEBUG) || defined(DEBUG)
            // In debug builds, emit a diagnostic if a camera-backed view ends
            // up with no primary color/depth targets. This is usually a
            // configuration error (e.g. passing unresolved RG handles) and
            // will cause world/entity passes for this view to be skipped.
            if (view.cameraId != 0 && view.targets.color == 0 && view.targets.depth == 0)
            {
                Logger::Log::Error(
                    "ViewRegistry::SetViewTargets: view {} ('{}') with camera {} was given no color/depth targets; "
                    "world/entity passes for this view will be skipped. If this view is intended to be headless, clear its cameraId.",
                    static_cast<uint32_t>(id),
                    view.debugName ? view.debugName : "<no name>",
                    static_cast<uint32_t>(view.cameraId));
            }
#endif
            return;
        }
    }

    Logger::Log::Warning(
        "ViewRegistry::SetViewTargets: view {} not found; targets not updated.",
        static_cast<uint32_t>(id));
}

void ViewRegistry::SetViewClearConfig(ViewId id, ViewClearConfig clear)
{
    // RenderGraph-arm clear publish: the frame owns the attachments (ViewTargetsRG),
    // but the world/sky nodes still read the clear configuration from the
    // view registry. Updating only the clear fields keeps the debug
    // "camera-backed view with no targets" diagnostic meaningful for the
    // old arm, where null targets really do skip world passes.
    for (auto& view : m_Views)
    {
        if (view.id != id)
            continue;

        const bool changed =
            (view.targets.clearColor != clear.clearColor) ||
            (view.targets.clearDepth != clear.clearDepth) ||
            (view.targets.clearStencil != clear.clearStencil) ||
            (view.targets.clearDepthValue != clear.clearDepthValue) ||
            (view.targets.clearStencilValue != clear.clearStencilValue) ||
            (view.targets.clearColorValue[0] != clear.clearColorValue[0]) ||
            (view.targets.clearColorValue[1] != clear.clearColorValue[1]) ||
            (view.targets.clearColorValue[2] != clear.clearColorValue[2]) ||
            (view.targets.clearColorValue[3] != clear.clearColorValue[3]);
        if (!changed)
            return;

        view.targets.clearColor = clear.clearColor;
        view.targets.clearDepth = clear.clearDepth;
        view.targets.clearStencil = clear.clearStencil;
        view.targets.clearColorValue[0] = clear.clearColorValue[0];
        view.targets.clearColorValue[1] = clear.clearColorValue[1];
        view.targets.clearColorValue[2] = clear.clearColorValue[2];
        view.targets.clearColorValue[3] = clear.clearColorValue[3];
        view.targets.clearDepthValue = clear.clearDepthValue;
        view.targets.clearStencilValue = clear.clearStencilValue;
        return;
    }

    Logger::Log::Warning(
        "ViewRegistry::SetViewClearConfig: view {} not found; clear not updated.",
        static_cast<uint32_t>(id));
}

void ViewRegistry::ClearViewTargets(ViewId id)
{
    // Old-arm → RenderGraph handoff: the old graph's retained world pass activates on
    // the REGISTRY targets (hasTargets in its predicate). A view the RenderGraph arm
    // takes over after an old-arm excursion (movie/pixel-perfect frames) must
    // drop its published targets or the old graph keeps rendering it every
    // frame. Clearing carries none of SetViewTargets' null-target diagnostic —
    // a target-less RenderGraph-owned view is the intended state.
    for (auto& view : m_Views)
    {
        if (view.id != id)
            continue;

        view.targets.color = 0;
        view.targets.depth = 0;
        view.targets.resolve = 0;
        view.targets.colorSampleCount = 0;
        return;
    }
}

void ViewRegistry::DestroyPerViewGpuResources(PerViewResources& pv)
{
    for (auto& buf : pv.LightBuffers)
    {
        if (buf.IsValid())
        {
            // A2: a valid GPU handle with a null device is a teardown-ordering
            // bug — the device is nulled only WITH RenderServices::m_Device, so
            // any release before that point still has a live device to free it.
            assert(m_Device &&
                   "DestroyPerViewGpuResources: valid LightBuffers handle but null device");
            if (m_Device)
                m_Device->DestroyBuffer(buf);
        }
    }
    pv.LightBuffers = {};
}

void ViewRegistry::DestroyAllPerViewGpuResources()
{
    for (auto& [viewId, pv] : m_PerView)
    {
        (void)viewId;
        DestroyPerViewGpuResources(pv);
    }
}

void ViewRegistry::ForgetPerViewGpuResourcesAfterDeviceRebuild()
{
    // Deliberately NOT DestroyPerViewGpuResources: by the time a rebuild reaches
    // re-provisioning the old device and its buffers are already gone, so calling
    // DestroyBuffer would hand handles from the dead device to the live one.
    for (auto& [viewId, pv] : m_PerView)
    {
        (void)viewId;
        pv.LightBuffers = {};
        // The recorded draws name buffers and descriptor sets of the dead device; the producers
        // emit them again from live handles.
        pv.ForwardCommands.clear();
        pv.LateForwardCommands.clear();
        for (std::vector<DrawCommand>& depthCommands : pv.DepthCommands)
            depthCommands.clear();
        pv.NonOccludingPrepassHeads.clear();
        pv.ForwardDrawsWritingDepth = 0;
    }
}

void ViewRegistry::BeginFrame()
{
    // Called exactly once per app frame from BeginWorldDrawFrame (sec 0a-A3-i),
    // NEVER from the per-window BuildFrameGraph — moving it there would let
    // window 1 wipe window 0's per-frame view flags mid-frame.

    // Step 1: expire lapsed OnDemand view requests. Runs before extraction, so a
    // view requested during LAST frame's declaration (participationFrames = 2) is
    // fed by THIS frame's extraction/culling (now 1) and expires the frame after
    // (0) — the RequestViewFrame contract.
    for (auto& view : m_Views)
    {
        if (view.participation == Rendering::ViewParticipation::OnDemand &&
            view.participationFrames > 0)
        {
            --view.participationFrames;
        }
    }

    // Step 4: per-frame pipeline-view flags + per-view world-pass keywords. The
    // RenderGraph spine has no per-frame build step, so without these clears a
    // sky-active frame would leak m_PipelineViewsWithSkyBackdrop into the next
    // frame and the world pass would Load instead of Clear after sky turns off.
    m_PipelineViewsWithColorInit.clear();
    m_PipelineViewsWithSkyBackdrop.clear();
    for (auto& [viewId, pv] : m_PerView)
    {
        (void)viewId;
        pv.WorldPassKeywords.reset();
    }

    // Step 8: the per-view shadow comparison sampler. ShadowMapNode republishes
    // it during pipeline build, but only on frames it reaches that publish —
    // several early-returns skip it — so consumers must treat an absent entry as
    // normal and fall back rather than assume this is refilled every frame. The
    // cascade ARRAY is deliberately not stored here at all (it is pool-owned and
    // frame-local); see ViewRegistry.h's ViewShadowResources.
    m_ViewShadowResources.clear();
}

} // namespace Engine::Renderer
} // namespace GameEngine
