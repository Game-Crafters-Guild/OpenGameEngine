#include "Engine/Rendering/Pipeline/Nodes/ViewParamsUploadNode.h"

#include "Engine/Rendering/CameraUtils.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"

#include "Core/Time.h"
#include "Rendering/Common/Math.h"
#include "Rendering/Core/ViewParamsLayout.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <nlohmann/json.hpp>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

float ViewParamsUploadNode::ComputeTaauMipBias(uint32_t renderWidth, uint32_t outputWidth)
{
    if (renderWidth == 0 || outputWidth == 0 || renderWidth >= outputWidth)
        return 0.0f;
    const float bias =
        std::log2(static_cast<float>(renderWidth) / static_cast<float>(outputWidth));
    // Two mip levels is the floor, and the pre-pass does reach it: render scale
    // clamps to [kMinRenderScale, 1.0] with kMinRenderScale = 0.25, whose exact
    // ratio already lands on -2.0, and the internal extent is an even-snapped
    // FLOOR of output*scale, so the realised ratio can sit below the requested
    // scale and the clamp engages. Dynamic resolution cannot get here — its own
    // MinScale is separately clamped to >= 0.5.
    return std::max(bias, -2.0f);
}

bool ViewParamsUploadNode::Initialize(std::string nodeId, std::string nodeJson, std::string* outError)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);

    try
    {
        auto j = nlohmann::json::parse(m_Json);
        if (j.is_object())
        {
            if (j.contains("buffer") && j["buffer"].is_string())
            {
                m_BufferRef = j["buffer"].get<std::string>();
            }
        }
    }
    catch (const std::exception& e)
    {
        if (outError)
            *outError = std::string("JSON parse failed: ") + e.what();
        return false;
    }

    return true;
}

void ViewParamsUploadNode::DeclareForView(ViewDeclare& d)
{
    // Dissolved into the upload ring: payload computed and written at
    // declaration — no pass, no graph declaration (host-coherent). Sized
    // from the STRUCT (sizeof(ViewParamsUBO), asserted in ViewParamsLayout.h); the
    // blueprint's size entry is not consulted on this Upload path.
    const auto* vd = d.Services.Views().FindViewDesc(d.View.id);
    if (!vd || vd->cameraId == 0)
        return;
    const auto* stored = d.Services.Views().FindCameraData(vd->cameraId);
    if (!stored)
        return;
    auto alloc = d.Frame.AllocUpload<ViewParamsUBO>();
    if (!alloc.Valid())
        return;

    // Raster domain: ViewParams describes the raster that produced the depth
    // its consumers (GTAO, fog apply, clouds, DoF) reconstruct against, so a
    // TAA view uploads the jittered proj/viewProj here (no-op when TAA is
    // off). The registry's stored CameraData stays unjittered (logic domain).
    const auto* tempAA = d.Services.Views().FindViewAntiAliasing(d.View.id);
    Rendering::CameraData jittered = *stored;
    if (tempAA != nullptr)
    {
        jittered = d.Services.Views().ResolveJitteredCameraData(
            d.View.id, d.Frame.FrameIndex(), d.RenderWidth, d.RenderHeight);
    }
    const Rendering::CameraData* cam = &jittered;

    GameEngine::Rendering::Matrix4x4 projM;
    std::memcpy(projM.Data(), cam->proj, sizeof(cam->proj));
    GameEngine::Rendering::Matrix4x4 invProjM = GameEngine::Mathematics::Inverse(projM);

    ViewParamsUBO ubo{};
    std::memcpy(ubo.ge_invProj, invProjM.Data(), sizeof(ubo.ge_invProj));
    std::memcpy(ubo.ge_view, cam->view, sizeof(ubo.ge_view));
    float zn = 0.1f, zf = 1000.0f;
    ExtractNearFarLH_ZO(cam->proj, zn, zf);
    ubo.ge_nearFar[0] = zn;
    ubo.ge_nearFar[1] = zf;
    const float logRatio = std::log(zf / std::max(zn, 1e-4f));
    ubo.ge_nearFar[2] = (logRatio > 1e-6f) ? (1.0f / logRatio) : 0.0f;
    const float shaderAnimationTime = d.Services.GetShaderAnimationTimeSeconds();
    ubo.ge_nearFar[3] = shaderAnimationTime;

    GameEngine::Rendering::Matrix4x4 viewM;
    std::memcpy(viewM.Data(), cam->view, sizeof(cam->view));
    GameEngine::Rendering::Matrix4x4 invViewM = GameEngine::Mathematics::Inverse(viewM);
    const float* iv = invViewM.Data();
    ubo.ge_cameraPosWS[0] = iv[12];
    ubo.ge_cameraPosWS[1] = iv[13];
    ubo.ge_cameraPosWS[2] = iv[14];
    ubo.ge_cameraPosWS[3] = 0.0f;
    // view->world, so consumers reconstruct a world position from a view-space
    // point without a per-pixel matrix inverse (SSR reprojection).
    std::memcpy(ubo.ge_invView, invViewM.Data(), sizeof(ubo.ge_invView));

    // THE view extent — the same numbers buffer-size expressions and
    // dispatch sizing use.
    const float sw = std::max(static_cast<float>(d.RenderWidth), 1.0f);
    const float sh = std::max(static_cast<float>(d.RenderHeight), 1.0f);
    ubo.ge_screenSize[0] = sw;
    ubo.ge_screenSize[1] = sh;
    ubo.ge_screenSize[2] = 1.0f / sw;
    ubo.ge_screenSize[3] = 1.0f / sh;
    std::memcpy(ubo.ge_viewProj, cam->viewProj, sizeof(ubo.ge_viewProj));
    std::memcpy(ubo.ge_proj, cam->proj, sizeof(ubo.ge_proj)); // ge_proj == the projection ge_invProj inverts

    // TAAU texture-sharpness compensation: material sampling adds this bias per
    // sample (adapter_forward's GE_MaterialTexture texture() overload) so texture
    // detail at the reduced internal raster matches display resolution. Derived
    // from the ACTUAL extent ratio (even-snap included), so it tracks the slider
    // live; exactly 0.0 when the view is not upscaling — the scale-1.0
    // byte-neutrality gate.
    ubo.ge_mipBiasParams[0] = ComputeTaauMipBias(d.RenderWidth, d.OutputWidth);

    // Previous frame's UNJITTERED world->clip. This history is independent of
    // the active AA mode, so SSSR's temporal lane also works with AA off/MSAA.
    bool previousValid = false;
    const ViewDeformationClock clock =
        d.Services.TemporalHistory().ResolveDeformationClock(Time::GetCumulativeSeconds());
    const ViewTemporalSample currentSample{*stored, clock.TimeSeconds,
                                           d.Services.GetScrollAnimationTimeSeconds(), clock.Origin};
    // A declare path: the rotation takes the frame stream's submitted-frame
    // count so a declared-and-abandoned frame never becomes a later previous.
    const ViewTemporalSample* previous = d.Services.TemporalHistory().Advance(
        d.View.id, d.Frame.FrameIndex(), currentSample, d.Frame.SubmittedFrameCount(),
        &previousValid);
    const float* prevViewProj =
        (previousValid && previous) ? previous->Camera.viewProj : stored->viewProj;
    std::memcpy(ubo.ge_prevViewProj, prevViewProj, sizeof(ubo.ge_prevViewProj));

    // Sub-pixel NDC jitter, current (xy) and previous (zw) frame. A consumer
    // reprojecting through the UNJITTERED ge_prevViewProj adds zw to land on the
    // previous frame's actual raster grid (ApplyNdcJitter is additive in NDC).
    // Left zero when no jittered AA drives this view.
    if (tempAA != nullptr && tempAA->Enabled)
    {
        ubo.ge_taaJitter[0] = tempAA->NdcJitterX;
        ubo.ge_taaJitter[1] = tempAA->NdcJitterY;
        ubo.ge_taaJitter[2] = tempAA->PrevNdcJitterX;
        ubo.ge_taaJitter[3] = tempAA->PrevNdcJitterY;
    }

    // The exposure the tonemap applies to this view, from the same effective settings the
    // AutoExposure and Tonemap nodes read. While auto exposure meters the view, forward shading
    // takes the metered scale from ExposureHistory instead (GE_ViewExposureScale).
    const PostProcessSettings& post = d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId);
    ubo.ge_exposureParams[0] = post.Exposure;
    ubo.ge_exposureParams[1] = post.IsAutoExposureActive() ? 1.0f : 0.0f;

    *alloc.Ptr = ubo;
    d.PublishBuffer(m_BufferRef,
                    {alloc.Buffer, alloc.Offset, sizeof(ViewParamsUBO), /*Graph*/ {}});
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
