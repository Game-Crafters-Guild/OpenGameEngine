#include "Ocean/OceanRenderNode.h"
#include "Ocean/OceanLightShafts.h"
#include "Ocean/OceanRenderFeature.h"
#include "Ocean/OceanShapeSampleInputs.h"

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace GameEngine::Ocean
{

namespace
{

::GameEngine::Rendering::CameraId ResolveOceanSnapCamera(
    const Engine::Renderer::RenderServices& rs,
    const ::GameEngine::Rendering::ViewDesc& currentView)
{
    // The ocean sim cascades are shared once per frame. If this declaration is
    // for the editor scene viewport, snap directly to that view's freshly-
    // declared camera so a stale global viewpoint (or a live Game View) cannot
    // make foam/flow appear to trail while navigating the editor camera.
    if (currentView.purpose == ::GameEngine::Rendering::ViewPurpose::EditorScene &&
        currentView.cameraId != 0 && rs.Views().FindCameraData(currentView.cameraId))
    {
        return currentView.cameraId;
    }

    const auto viewpointCamera = rs.Views().GetViewpointCamera();
    if (viewpointCamera != 0 && rs.Views().FindCameraData(viewpointCamera))
        return viewpointCamera;

    for (const auto& view : rs.Views().GetViews())
    {
        if (!view.activeRenderPipeline || view.ActiveRenderLayerMask() == 0u)
            continue;
        if (view.purpose == ::GameEngine::Rendering::ViewPurpose::EditorScene &&
            view.cameraId != 0 && rs.Views().FindCameraData(view.cameraId))
        {
            return view.cameraId;
        }
    }

    return currentView.cameraId;
}

// Reflected caustics light receivers up to `bandHeight` above the displaced
// surface, traced back to the water along a ray that rises at least
// kReflectedCausticsMinRise, so the lit band hugs the water bodies: the pass is
// declared only when that band, around any water footprint, can be on screen.
bool IsReflectedCausticsBandInView(const Engine::Renderer::RenderServices& rs,
                                   const OceanRenderFeature& feature,
                                   const ::GameEngine::Rendering::ViewDesc& view,
                                   const OceanParamsGPU& params, float bandHeight,
                                   float displacement)
{
    const auto* camera = rs.Views().FindCameraData(view.cameraId);
    if (!camera)
        return true;
    const float maxHeightAboveSurface = bandHeight + 2.0f * displacement;
    return feature.IsWaterFootprintInView(camera->viewProj, params.SeaLevel - displacement,
                                          params.SeaLevel + displacement + bandHeight,
                                          maxHeightAboveSurface / kReflectedCausticsMinRise);
}

// Whether any water surface can be on this view's screen: the band the displaced
// surface can occupy, over the water footprint widened by how far waves move the
// surface sideways, intersects the view frustum. Conservative: the bound is the
// displacement clamp, not a measurement.
bool IsWaterSurfaceInView(const Engine::Renderer::RenderServices& rs,
                          const OceanRenderFeature& feature,
                          const ::GameEngine::Rendering::ViewDesc& view, const OceanParamsGPU& params)
{
    const auto* camera = rs.Views().FindCameraData(view.cameraId);
    if (!camera)
        return true;
    const float displacement = ReflectedCausticsSurfaceDisplacementBound(params, nullptr);
    return feature.IsWaterFootprintInView(camera->viewProj, params.SeaLevel - displacement,
                                          params.SeaLevel + displacement,
                                          std::max(params.MaxHorizontalDisplacement, 0.0f));
}

// The reflected-caustics surface bound for this frame. The height-field
// readback measures the global FFT surface; local FFT streams are not in it, so
// with any of them ready the bound falls back to the clamp.
float ResolveReflectedCausticsDisplacement(OceanRenderFeature& feature,
                                           const OceanParamsGPU& params,
                                           uint32 localFFTReadyMask)
{
    float measured = 0.0f;
    const bool hasMeasurement = localFFTReadyMask == 0u && feature.IsHeightFieldReady() &&
                                feature.GetHeightField().GetRecentMaxVerticalDisplacement(measured);
    return ReflectedCausticsSurfaceDisplacementBound(params, hasMeasurement ? &measured : nullptr);
}

} // namespace

OceanRenderNode::~OceanRenderNode() = default;

bool OceanRenderNode::Initialize(std::string nodeId, std::string nodeJson, std::string* /*outError*/)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);
    // The post-world overlay instance is declared explicitly in the node JSON
    // ("mode": "underwater") — the node id is a label, not a switch.
    m_UnderwaterMode = false;
    try
    {
        const auto j = nlohmann::json::parse(m_Json);
        if (j.contains("mode") && j["mode"].is_string())
            m_UnderwaterMode = j["mode"].get<std::string>() == "underwater";
    }
    catch (...)
    {
    }
    if (m_UnderwaterMode)
    {
        namespace Names = Engine::Renderer::Pipeline::Names;
        m_PostOceanDepthResolve.Initialize(
            m_Id + ".PostOceanDepthResolve",
            std::string(R"({"output":")") + Names::View::DepthResolvedPostOcean +
                R"(","poolName":"Pipeline.DepthResolvedPostOcean.View"})",
            nullptr);
    }
    // Migration tripwire, not a fallback: project .rendergraph copies predating
    // the explicit mode field would silently lose the ocean surface + underwater
    // overlay. Fail loudly with the one-line fix instead.
    if (!m_UnderwaterMode && m_Id.find("Underwater") != std::string::npos)
    {
        Logger::Log::Error(
            "OceanRenderNode: node '{}' looks like the post-world overlay instance but has no "
            "\"mode\": \"underwater\" field — add it to this pipeline's .rendergraph (or re-copy "
            "the default), or the ocean surface and underwater overlay will not render.",
            m_Id);
    }
    return true;
}

void OceanRenderNode::Declare(Engine::Renderer::Pipeline::RenderPipelineInstance& instance,
                              const Engine::Renderer::Pipeline::PipelineDeclareContext& ctx)
{
    if (m_UnderwaterMode)
        m_PostOceanDepthResolve.Declare(instance, ctx);
}

void OceanRenderNode::DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d)
{
    namespace RenderGraph = ::GameEngine::Rendering::RenderGraph;
    auto& rs = d.Services;
    auto* device = rs.GetDevice();
    if (!device)
        return;

    auto& feature = rs.EnsureFeature<OceanRenderFeature>();
    if (!feature.IsInitialized())
    {
        if (!feature.Initialize(device))
            return;
    }
    const uint32 frameIndex = device->GetFrameIndex();

    // Post-world overlay instance: schedule ONLY the submerged fullscreen overlay,
    // using the submersion state the pre-world instance stamped this frame. It must
    // be a separate node placed after World so it reads the fully-drawn scene colour
    // (the pre-world instance would tint sky-only colour that World then overwrites).
    if (m_UnderwaterMode)
    {
        if (feature.HasOcean())
        {
            const OceanParamsGPU viewParams = feature.GetParamsForView(d.View.id);
            feature.GetShadows().DeclareForView(d, viewParams);
            // The surface and the scene grab it refracts only when this view can
            // see water: from below (the overlay is active) or within the frustum.
            const bool waterInView =
                viewParams.Underwater != 0u || IsWaterSurfaceInView(rs, feature, d.View, viewParams);
            if (waterInView)
            {
                feature.NoteWaterInView();
                feature.GetSceneGrab().DeclareForView(d, frameIndex);
            }

            Engine::Renderer::DrawCommand surfaceCommand{};
            std::span<const RenderGraph::RGTexture> sampledCascades{};
            if (waterInView &&
                feature.BuildSurfaceCommandForView(rs, d.Frame, d.View.id, frameIndex,
                                                   surfaceCommand, &sampledCascades))
            {
                Engine::Renderer::RenderServices::WorldPassTargetsRG targets{};
                targets.Color = d.ViewColor;
                targets.Depth = d.ViewDepth;
                targets.Resolve = d.ViewResolve;
                targets.DepthResolved = d.ViewDepthResolved;
                targets.ResolveFromPipeline = d.ResolveFromPipeline;

                auto passKeywords = ::GameEngine::Rendering::MaterialKeyword::None;
                if (auto kw = rs.GetWorldPassKeywords(d.View.id))
                    passKeywords = *kw;

                // Declared sampled reads for every dynamic texture the surface
                // draw binds descriptor-direct (grab, planar reflection, and all
                // sim cascades — collected by BuildForwardCommand at the bind
                // sites, so a texture is declared iff it is bound). The declared
                // reads are what order this pass after the producers and land
                // the layout transitions; the bind alone forms no edge.
                std::array<Engine::Renderer::DrawCommand, 1> commands{surfaceCommand};
                rs.AddForwardCommandPassForView(d.Frame, d.View.id, targets, passKeywords,
                                                commands, sampledCascades, "OceanSurface");
                m_PostOceanDepthResolve.DeclareForView(d);

            }

            feature.GetSprayGPU().DeclareForView(d, feature);

            const OceanParamsGPU uwParams = feature.GetParamsForView(d.View.id);
            const OceanUnderwaterSettings uwSettings = feature.GetUnderwaterSettings();
            // The wave-shape inputs the fullscreen passes sample: raw handles for
            // the binds + RG imports for the declared reads (which order those
            // passes after the sims even when the surface pass declined).
            OceanShapeSampleInputs shape{};
            shape.Displacement = feature.GetDisplacementTexture();
            shape.DisplacementSampler = feature.GetDisplacementSampler();
            shape.DisplacementRG = feature.GetFFT().ImportDisplacementRG(d.Frame);
            if (feature.IsWaveMaskReady() && feature.GetWaveMask().HasSources())
            {
                shape.LocalFFTReadyMask = feature.GetLocalFFTReadyMaskForFrame(frameIndex);
                for (uint32 stream = 0u; stream < kMaxOceanLocalFFTStreams; ++stream)
                {
                    if ((shape.LocalFFTReadyMask & (1u << stream)) == 0u)
                        continue;
                    shape.LocalDisplacements[stream] = feature.GetLocalDisplacementTexture(stream);
                    shape.LocalDisplacementSamplers[stream] =
                        feature.GetLocalDisplacementSampler(stream);
                    shape.LocalDisplacementsRG[stream] =
                        feature.GetLocalFFT(stream).ImportDisplacementRG(d.Frame);
                }
                for (uint32 page = 0u; page < kOceanLocalFFTMaskPages; ++page)
                {
                    shape.LocalFFTMasks[page] = feature.GetWaveMask().GetLocalFFTMaskTexture(page);
                    shape.LocalFFTMaskSamplers[page] = feature.GetWaveMaskSampler();
                    shape.LocalFFTMasksRG[page] =
                        feature.GetWaveMask().ImportLocalFFTMaskRG(d.Frame, page);
                }
                shape.LocalFFTMaskLayout = &feature.GetWaveMask().GetLayout();
            }
            const float submergedDepth = feature.GetSubmergedDepthForView(d.View.id);
            const bool overlayActive = uwParams.Underwater != 0u;
            const bool cameraIsBelowSurface = submergedDepth > 0.0f;

            // The underwater overlay can be activated slightly above sea level to
            // keep the wavy waterline stable at grazing views. That headroom must
            // not suppress above-water reflected caustics while the camera is
            // still actually above the displaced surface.
            if (!cameraIsBelowSurface)
            {
                const bool reflectedCausticsEnabled =
                    uwSettings.CausticsOnGeometry &&
                    uwSettings.ReflectedCaustics_Strength > 0.0f &&
                    uwSettings.ReflectedCaustics_Height > 0.0f &&
                    (feature.HasUserCausticsTexture() || feature.IsCausticsReady());
                const float causticsDisplacement =
                    reflectedCausticsEnabled
                        ? ResolveReflectedCausticsDisplacement(feature, uwParams,
                                                               shape.LocalFFTReadyMask)
                        : 0.0f;
                if (reflectedCausticsEnabled &&
                    IsReflectedCausticsBandInView(rs, feature, d.View, uwParams,
                                                  uwSettings.ReflectedCaustics_Height,
                                                  causticsDisplacement))
                {
                    const auto userCaustics = feature.GetUserCausticsTexture();
                    const auto causticsTex =
                        userCaustics.IsValid() ? userCaustics : feature.GetCausticsTexture();
                    const auto causticsSampler = userCaustics.IsValid()
                        ? feature.GetUserCausticsSampler()
                        : feature.GetCausticsSampler();
                    // Above water: reflected caustics from the water.
                    feature.GetReflectedCaustics().DeclareForView(d, uwParams, uwSettings, shape,
                                                                  causticsTex, causticsSampler,
                                                                  causticsDisplacement);
                }
            }

            feature.FillUnderwaterPortalData(m_PortalData, kMaxOceanUnderwaterPortalVolumes,
                                             kMaxOceanUnderwaterPortalExclusions,
                                             kMaxOceanUnderwaterPortalPolygons,
                                             kMaxOceanUnderwaterPortalOccluders);
            const OceanUnderwaterPortalData &portalData = m_PortalData;
            const OceanUnderwaterPortalData *portalDataPtr =
                (portalData.Enabled || !portalData.Materials.empty()) ? &portalData : nullptr;

            const auto underwaterCaustics = feature.HasUserCausticsTexture()
                ? feature.GetUserCausticsTexture() : feature.GetCausticsTexture();
            const auto underwaterCausticsSampler = feature.HasUserCausticsTexture()
                ? feature.GetUserCausticsSampler() : feature.GetCausticsSampler();

            if (overlayActive)
            {
                feature.GetUnderwater().DeclareForView(d, uwParams, submergedDepth, shape,
                                                       underwaterCaustics,
                                                       underwaterCausticsSampler, uwSettings,
                                                       portalDataPtr);
            }
            else
            {
                // Dry camera: the composite serves only the portal volumes, so
                // a frame that cannot see one declares no pass at all.
                const auto* viewCamera = rs.Views().FindCameraData(d.View.cameraId);
                if (portalData.Enabled && viewCamera &&
                    IsUnderwaterPortalInView(portalData, viewCamera->viewProj))
                    feature.GetUnderwater().DeclareForView(d, uwParams, 0.0f, shape,
                                                           underwaterCaustics,
                                                           underwaterCausticsSampler, uwSettings,
                                                           &portalData);
            }
        }
        return;
    }

    // Lazily create the surface contributor. The post-world OceanUnderwater node
    // submits the actual surface draw after it can grab the current opaque scene.
    feature.EnsureForwardContributor(rs);

    if (!feature.HasOcean())
        return;

    // The sim cascades (foam/flow/dyn-waves/seabed/clip/albedo) are one shared set
    // dispatched once per frame, so they can only center on one camera. Snap them to
    // the "viewpoint" camera (the editor's active scene view, set via
    // SetViewpointCamera) so the ocean detail follows the camera being navigated even
    // when a Game view is also live — mirroring the reference's single
    // OceanRenderer.Viewpoint. Falls back to this view's own camera when no viewpoint
    // is set (the runtime/Player default, where there's a single game view).
    float cameraX = 0.0f, cameraY = 0.0f, cameraZ = 0.0f;
    bool haveCamera = false;
    auto snapCameraId = ResolveOceanSnapCamera(rs, d.View);
    if (const auto* cam = rs.Views().FindCameraData(snapCameraId))
    {
        cameraX = cam->cameraPos[0];
        cameraY = cam->cameraPos[1];
        cameraZ = cam->cameraPos[2];
        haveCamera = true;
    }

    // Apply the authored cascade resolution controls (OceanRenderer.MinScale /
    // LodDataResolution / geometry density) before the sims snap + dispatch this
    // frame: base scale feeds the next snap, a resolution change resizes the
    // cascade textures, and a density change rebuilds the grid. Idempotent across
    // views — steady frames are no-ops.
    feature.ApplyCascadeConfig(device);

    const OceanRasterDepthCaptureSettings rasterDepthSettings =
        feature.GetRasterDepthCaptureSettings();
    auto& rasterDepthCapture = feature.GetRasterDepthCapture();
    // Set when the seabed bake will bind the raster capture this frame — the
    // bake pass declares the read (below). Without it the capture's readback
    // CopySrc read is the texture's only declared access, its first-touch
    // SRV->TransferSrc transition hoists to the submission front, and the
    // kEarlySetup bake samples TransferSrc through an SRV descriptor.
    RenderGraph::RGTexture rasterDepthRG{};
    if (rasterDepthSettings.Enabled && haveCamera)
    {
        const OceanParamsGPU rasterParams = feature.GetParams();
        const bool declared = rasterDepthCapture.DeclareForView(
            d, cameraX, cameraZ, rasterParams.SeaLevel, rasterDepthSettings);
        if (declared && rasterDepthCapture.HasSampleableCapture(frameIndex))
        {
            feature.GetSeabedDepth().SetRasterDepthCapture(
                rasterDepthCapture.GetTexture(), rasterDepthCapture.GetSampler(),
                rasterDepthCapture.GetOriginX(), rasterDepthCapture.GetOriginZ(),
                rasterDepthCapture.GetSizeX(), rasterDepthCapture.GetSizeZ(),
                rasterDepthCapture.GetDeepWaterDepth());
            rasterDepthRG = rasterDepthCapture.ImportRG(d.Frame);
        }
        else
        {
            feature.GetSeabedDepth().ClearRasterDepthCapture();
            if (!declared)
                rasterDepthCapture.InvalidateReadback();
        }
    }
    else
    {
        feature.GetSeabedDepth().ClearRasterDepthCapture();
        rasterDepthCapture.InvalidateReadback();
    }

    float viewCameraX = cameraX, viewCameraY = cameraY, viewCameraZ = cameraZ;
    bool haveViewCamera = haveCamera;
    bool isOrthographicSceneView = false;
    if (const auto* viewCam = rs.Views().FindCameraData(d.View.cameraId))
    {
        viewCameraX = viewCam->cameraPos[0];
        viewCameraY = viewCam->cameraPos[1];
        viewCameraZ = viewCam->cameraPos[2];
        haveViewCamera = true;
        // cameraPos.w == 1 is the editor 2D-ortho marker (CameraTypes.h) — the
        // explicit signal the old name+projection sniffing approximated.
        isOrthographicSceneView = viewCam->cameraPos[3] == 1.0f;
    }

    // Underwater gate: this view's camera is submerged when it sits below the
    // displaced surface at its XZ (vs flat SeaLevel — robust to waves).
    // SampleSurfaceForRendering uses the FFT-backed query path, so this tracks the
    // actual wave height the surface renders. Stamp the result onto the params (ANDed with the authored
    // toggle) so the surface draw
    // shades two-sided + flips back-face normals this frame; then schedule the
    // fullscreen overlay when genuinely below. Degrades gracefully — no camera,
    // toggle off, or stripped post phase leaves the ocean above-water. This must use
    // d.View.cameraId, not the shared ocean snap/viewpoint camera, or quad view lets
    // one viewport's submersion state tint every other viewport.
    float submergedDepth = 0.0f;
    bool submerged = false;
    if (haveViewCamera && !isOrthographicSceneView)
    {
        if (feature.IsUnderwaterExcluded(viewCameraX, viewCameraY, viewCameraZ))
        {
            submerged = false;
        }
        else if (feature.HasUnderwaterVolumes())
        {
            // Volume / fly-through mode: submerged only when the camera sits inside
            // an authored water volume; the overlay fog depth is measured down from
            // that box's top. Confines the underwater look to a bounded body.
            submerged = feature.TestUnderwaterVolume(viewCameraX, viewCameraY, viewCameraZ, submergedDepth);
        }
        else
        {
            // Infinite SeaLevel plane. Enable the overlay + two-sided surface across
            // the whole near-surface zone using the FLAT sea level plus a wave-crest
            // headroom, NOT the per-XZ wave height: comparing the camera to the wavy
            // surface at its exact XZ flickers in troughs (the surface dips a metre+
            // below the camera), which briefly disables the overlay and exposes the
            // un-fogged, back-face-culled surface underside as a grey void at the
            // waterline. The fullscreen frag masks above/below per pixel, so activating
            // a couple of metres above the surface is a no-op above the line.
            // submergedDepth keeps the true (wavy) depth so the overlay fog strength
            // still tracks how deep the camera actually is.
            constexpr float kOverlayActivationHeadroom = 2.0f; // metres above flat sea level
            const OceanParamsGPU params = feature.GetParams();
            const float surfaceY = feature.SampleSurfaceForRendering(viewCameraX, viewCameraZ).Height;
            submergedDepth = surfaceY - viewCameraY;
            submerged = viewCameraY < params.SeaLevel + kOverlayActivationHeadroom;
        }
    }
    // Stamp the submersion state + depth: the surface draw (this same world pass)
    // reads the Underwater flag to shade two-sided / flip back-face normals, and
    // the post-world OceanUnderwater node reads GetSubmergedDepth() to schedule the
    // fullscreen overlay AFTER the scene is drawn.
    feature.SetUnderwaterActive(d.View.id, submerged, submergedDepth);

    // The sims below serve only what a view can see, except the wave simulation,
    // which gameplay queries also read. A view that sees no water declares none of
    // the rest; the foam catches up the skipped time in its next step, and every
    // other sim is a function of the ocean clock and its sources, so it resumes
    // where an uninterrupted run would be.
    feature.BeginSimulationFrame();
    const bool waterInView =
        submerged || IsWaterSurfaceInView(rs, feature, d.View, feature.GetParams());
    if (!waterInView)
        feature.ForgetSimulationClaimsBefore(frameIndex);
    const bool simulateWaves =
        waterInView || feature.HasRecentSurfaceQueries() ||
        (feature.IsGPUQueryReady() && feature.GetGPUQuery().HasPending());

    // Planar reflection capture: re-render the opaque world mirrored across the
    // sea plane into a scaled target the surface samples for mirror reflections.
    // Pre-world (the surface binds it on the world draw), once per frame (a full
    // world pass; views share the single reflection target). Gated by the authored
    // PlanarReflections toggle and by this view seeing water. Degrades to the
    // procedural sky dome when it declines (no camera, the GPU-driven spine isn't
    // stamped this frame). The
    // reflection view is fed by extraction starting the frame after it is first
    // allocated, so the very first frame is sky-only.
    if (haveCamera && waterInView)
    {
        const OceanParamsGPU reflParams = feature.GetParams();
        if (reflParams.PlanarReflections != 0u && feature.TryClaimReflectionDispatch(frameIndex))
            feature.GetPlanarReflection().DeclareForView(
                d, reflParams.SeaLevel, feature.GetPlanarReflectionScale(), frameIndex);
    }


    // Sea-floor depth bake: independent of the FFT dispatch (it drives the shallow-
    // water colour + shoreline foam), so it has its own once-per-frame claim.
    // Scheduled at kEarlySetup BEFORE the foam sim (the foam
    // shoreline term reads this cascade) and before the world pass that samples it.
    // Snaps to the camera at schedule time so the bound layout matches the
    // dispatch. The seabed list is stashed by OceanExtractionSystem. Degrades
    // gracefully — no seabed tagged leaves the cascade deep, so the
    // shallow/shoreline terms stay off.
    auto& seabedDepth = feature.GetSeabedDepth();
    if (waterInView && feature.IsSeabedDepthReady() && seabedDepth.HasSeabeds() &&
        seabedDepth.BeginFrame(cameraX, cameraZ) &&
        feature.TryClaimSeabedDispatch(frameIndex))
    {
        auto* featurePtrDepth = &feature;
        const auto seabedParams = d.Frame.AllocUpload<OceanSeabedDepthParamsGPU>();
        if (seabedParams.Valid())
            seabedDepth.FillParams(*seabedParams.Ptr);
        const RenderGraph::RGTexture seabedRG = seabedDepth.ImportRG(d.Frame);
        d.Frame.AddPass(
            "OceanSeabedDepth", Rendering::PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& p)
            {
                p.Write(seabedRG, RenderGraph::RGTextureWrite::Storage);
                // Raster capture sampled in the bake dispatch (binding 3):
                // the declared read keeps its layout ShaderReadOnly here and
                // stops the readback's CopySrc transition from hoisting past
                // this dispatch.
                if (rasterDepthRG.IsValid())
                    p.Read(rasterDepthRG, RenderGraph::RGTextureRead::SampledCompute);
            },
            [featurePtrDepth, buf = seabedParams.Buffer,
             off = seabedParams.Offset](RenderGraph::RGContext& ctx)
            {
                if (ctx.Cmd && ctx.GetDevice())
                    featurePtrDepth->GetSeabedDepth().RecordDispatch(ctx.GetDevice(), ctx.Cmd, buf,
                                                                     off);
            });
        d.Frame.MarkOutput(seabedRG, RenderGraph::RGImageLayout::ShaderReadOnly);
        feature.GetSplineRaster().Declare(d.Frame, device, 0u, seabedRG, seabedDepth.GetLayout(),
                                          seabedDepth.GetResolution());
    }

    // Flow bake: independent of the FFT dispatch (it advects the foam + scrolls the
    // surface detail UVs), so it has its own once-per-frame claim. Scheduled at
    // kEarlySetup BEFORE the foam sim (the foam advection
    // term reads this cascade) and before the world pass that samples it. Snaps to
    // the camera at schedule time so the bound layout matches the dispatch. The
    // flow-source list is stashed by OceanExtractionSystem. Degrades gracefully —
    // no flow source leaves the cascade at zero, so advection / UV scroll stay off.
    auto& flow = feature.GetFlow();
    if (waterInView && feature.IsFlowReady() && flow.HasSources() &&
        flow.BeginFrame(cameraX, cameraZ) &&
        feature.TryClaimFlowDispatch(frameIndex))
    {
        auto* featurePtrFlow = &feature;
        const auto flowParams = d.Frame.AllocUpload<OceanFlowParamsGPU>();
        if (flowParams.Valid())
            flow.FillParams(*flowParams.Ptr);
        // Declared write: the graph owns the ShaderResource->General transition
        // (imported textures are cull sinks, so no PreventCulling) and every
        // declared reader (foam, the surface span) forms a real RAW edge.
        const RenderGraph::RGTexture flowRG = flow.ImportRG(d.Frame);
        d.Frame.AddPass(
            "OceanFlowSim", Rendering::PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& p)
            { p.Write(flowRG, RenderGraph::RGTextureWrite::Storage); },
            [featurePtrFlow, buf = flowParams.Buffer,
             off = flowParams.Offset](RenderGraph::RGContext& ctx)
            {
                if (ctx.Cmd && ctx.GetDevice())
                    featurePtrFlow->GetFlow().RecordDispatch(ctx.GetDevice(), ctx.Cmd, buf, off);
            });
        // Restores the resting ShaderReadOnly at end of frame even when no
        // declared reader ran (surface declined), keeping the import claim and
        // cross-frame descriptor-direct consumers (other windows) valid. Probe
        // captures declare their reads (EmitForwardSampledRead), so they don't
        // ride this export.
        d.Frame.MarkOutput(flowRG, RenderGraph::RGImageLayout::ShaderReadOnly);
        feature.GetSplineRaster().Declare(d.Frame, device, 1u, flowRG, flow.GetLayout(),
                                          flow.GetResolution());
    }

    // Dynamic (interactive) wave sim: independent of the FFT (spreading ripples
    // from impulses layer on top of any spectrum). Its own once-per-frame claim.
    // Scheduled at kEarlySetup before the world pass (the surface vertex modifier
    // samples its height). Flips the ping-pong + snaps to the camera at schedule
    // time so the bound texture is the one the dispatch writes. The impulse list +
    // sim scalars are stashed by OceanExtractionSystem. Degrades gracefully — no
    // impulse leaves the cascade calm, so the surface reads spectrum waves only.
    if (feature.IsDynWavesReady() && feature.IsDynWavesEnabled() &&
        !feature.GetDynWaves().IsQuiescent() &&
        feature.GetDynWaves().GetPendingSubstepCount() > 0u &&
        feature.TryClaimDynWavesDispatch(frameIndex))
    {
        auto* featurePtrDyn = &feature;
        RenderGraph::RGTexture dynWriteRG{};
        const uint32 substepCount = feature.GetDynWaves().GetPendingSubstepCount();
        for (uint32 substep = 0u; substep < substepCount; ++substep)
        {
            // Each fixed step flips the ping-pong target. Capturing writeIndex in
            // the execute lambda is essential: all steps are declared before any
            // of them execute, while the feature's final index already points at
            // the last scheduled output.
            const uint32 writeIndex = feature.GetDynWaves().BeginSubstep(cameraX, cameraZ);
            dynWriteRG = feature.GetDynWaves().ImportWriteRG(d.Frame, writeIndex);
            const RenderGraph::RGTexture dynPrevRG =
                feature.GetDynWaves().ImportPrevRG(d.Frame, writeIndex);
            const RenderGraph::RGTexture dynSeabedRG =
                feature.IsSeabedDepthReady() ? feature.GetSeabedDepth().ImportRG(d.Frame)
                                             : RenderGraph::RGTexture{};

            const auto dynParams = d.Frame.AllocUpload<OceanDynWavesParamsGPU>();
            if (dynParams.Valid())
                feature.GetDynWaves().FillParams(*dynParams.Ptr, writeIndex, substep);
            d.Frame.AddPass(
                "OceanDynWavesSim", Rendering::PassPhase::kEarlySetup,
                [&](RenderGraph::RGPassBuilder& p)
                {
                    if (dynPrevRG.IsValid())
                        p.Read(dynPrevRG, RenderGraph::RGTextureRead::SampledCompute);
                    if (dynSeabedRG.IsValid())
                        p.Read(dynSeabedRG, RenderGraph::RGTextureRead::SampledCompute);
                    p.Write(dynWriteRG, RenderGraph::RGTextureWrite::Storage);
                },
                [featurePtrDyn, buf = dynParams.Buffer, off = dynParams.Offset,
                 writeIndex](RenderGraph::RGContext& ctx)
                {
                    if (ctx.Cmd && ctx.GetDevice())
                        featurePtrDyn->GetDynWaves().RecordDispatch(
                            ctx.GetDevice(), ctx.Cmd, buf, off, writeIndex);
                });
            d.Frame.MarkOutput(dynWriteRG, RenderGraph::RGImageLayout::ShaderReadOnly);
        }
        feature.GetDynWaves().FinishSubsteps();

        // Height-query readback: its OWN pass so its sampled read of the freshly
        // written cascade is a real RAW edge (it used to ride the same exec
        // lambda, ordered only by the sim's trailing manual barrier). The sink is
        // a CPU readback ring — off-graph, hence PreventCulling.
        if (feature.IsDynWavesReadbackReady())
        {
            OceanCascadeLayoutGPU dynLayout = feature.GetDynWaves().GetLayout();
            const uint32 lodLimit = feature.GetLodCountLimit();
            if (lodLimit != 0u && dynLayout.LodCount > lodLimit)
                dynLayout.LodCount = lodLimit;

            auto& dynReadback = feature.GetDynWavesReadback();
            dynReadback.BeginFrame(cameraX, cameraZ);
            dynReadback.SetFrameInputs(feature.GetDynWavesTexture(), feature.GetDynWavesSampler(),
                                       dynLayout);

            const auto rbParams = d.Frame.AllocUpload<OceanDynWavesQueryParamsGPU>();
            if (rbParams.Valid())
            {
                dynReadback.FillParams(*rbParams.Ptr);
                d.Frame.AddPass(
                    "OceanDynWavesQuery", Rendering::PassPhase::kEarlySetup,
                    [&](RenderGraph::RGPassBuilder& p)
                    {
                        p.Read(dynWriteRG, RenderGraph::RGTextureRead::SampledCompute);
                        p.PreventCulling();
                    },
                    [featurePtrDyn, framePtr = &d.Frame, rbBuf = rbParams.Buffer,
                     rbOff = rbParams.Offset](RenderGraph::RGContext& ctx)
                    {
                        if (ctx.Cmd && ctx.GetDevice())
                            featurePtrDyn->GetDynWavesReadback().RecordDispatch(
                                ctx.GetDevice(), ctx.Cmd, *framePtr, rbBuf, rbOff);
                    });
            }
        }
    }

    // Local wave-mask bake: per-body wave/chop multipliers. Independent of the
    // FFT and scheduled before the world pass so the surface samples this frame's
    // mask while applying spectrum and dynamic-wave displacement.
    if (waterInView && feature.IsWaveMaskReady() && feature.GetWaveMask().HasSources() &&
        feature.TryClaimWaveMaskDispatch(frameIndex))
    {
        feature.GetWaveMask().BeginFrame(cameraX, cameraZ);

        auto* featurePtrWaveMask = &feature;
        const auto maskParams = d.Frame.AllocUpload<OceanWaveMaskParamsGPU>();
        if (maskParams.Valid())
            feature.GetWaveMask().FillParams(*maskParams.Ptr);
        const RenderGraph::RGTexture maskRG = feature.GetWaveMask().ImportMaskRG(d.Frame);
        std::array<RenderGraph::RGTexture, kOceanLocalFFTMaskPages> maskPagesRG{};
        for (uint32 page = 0u; page < kOceanLocalFFTMaskPages; ++page)
            maskPagesRG[page] = feature.GetWaveMask().ImportLocalFFTMaskRG(d.Frame, page);
        d.Frame.AddPass(
            "OceanWaveMaskSim", Rendering::PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& p)
            {
                p.Write(maskRG, RenderGraph::RGTextureWrite::Storage);
                for (const RenderGraph::RGTexture pageRG : maskPagesRG)
                    if (pageRG.IsValid())
                        p.Write(pageRG, RenderGraph::RGTextureWrite::Storage);
            },
            [featurePtrWaveMask, buf = maskParams.Buffer,
             off = maskParams.Offset](RenderGraph::RGContext& ctx)
            {
                if (ctx.Cmd && ctx.GetDevice())
                    featurePtrWaveMask->GetWaveMask().RecordDispatch(ctx.GetDevice(), ctx.Cmd, buf,
                                                                     off);
            });
        d.Frame.MarkOutput(maskRG, RenderGraph::RGImageLayout::ShaderReadOnly);
        for (const RenderGraph::RGTexture pageRG : maskPagesRG)
            if (pageRG.IsValid())
                d.Frame.MarkOutput(pageRG, RenderGraph::RGImageLayout::ShaderReadOnly);
    }

    // Clip bake: independent of the FFT dispatch, so it has its own once-per-frame
    // claim. Scheduled at kEarlySetup before the world pass that samples it. Snaps to the camera at
    // schedule time so the bound layout matches the dispatch. The clip-source list
    // is stashed by OceanExtractionSystem. Degrades gracefully — no clip source
    // leaves the cascade at the default state, so the surface stays solid.
    auto& clip = feature.GetClip();
    if (waterInView && feature.IsClipReady() && clip.HasSources() &&
        clip.BeginFrame(cameraX, cameraZ) &&
        feature.TryClaimClipDispatch(frameIndex))
    {
        auto* featurePtrClip = &feature;
        const auto clipParams = d.Frame.AllocUpload<OceanClipParamsGPU>();
        if (clipParams.Valid())
            clip.FillParams(*clipParams.Ptr);
        const RenderGraph::RGTexture clipRG = clip.ImportRG(d.Frame);
        d.Frame.AddPass(
            "OceanClipSim", Rendering::PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& p)
            { p.Write(clipRG, RenderGraph::RGTextureWrite::Storage); },
            [featurePtrClip, buf = clipParams.Buffer,
             off = clipParams.Offset](RenderGraph::RGContext& ctx)
            {
                if (ctx.Cmd && ctx.GetDevice())
                    featurePtrClip->GetClip().RecordDispatch(ctx.GetDevice(), ctx.Cmd, buf, off);
            });
        d.Frame.MarkOutput(clipRG, RenderGraph::RGImageLayout::ShaderReadOnly);
        feature.GetSplineRaster().Declare(d.Frame, device, 2u, clipRG, clip.GetLayout(),
                                          clip.GetResolution());
    }

    // Albedo bake: independent of the FFT dispatch, so it has its own once-per-frame
    // claim. Scheduled at kEarlySetup before the world pass that samples it. Snaps to the camera at
    // schedule time so the bound layout matches the dispatch. The albedo-source list
    // is stashed by OceanExtractionSystem. Degrades gracefully — no albedo source
    // leaves the cascade transparent, so the base water colour is unchanged.
    auto& albedo = feature.GetAlbedo();
    if (waterInView && feature.IsAlbedoReady() && albedo.HasSources() &&
        albedo.BeginFrame(cameraX, cameraZ) &&
        feature.TryClaimAlbedoDispatch(frameIndex))
    {
        auto* featurePtrAlbedo = &feature;
        const auto albedoParams = d.Frame.AllocUpload<OceanAlbedoParamsGPU>();
        if (albedoParams.Valid())
            albedo.FillParams(*albedoParams.Ptr);
        const RenderGraph::RGTexture albedoRG = albedo.ImportRG(d.Frame);
        d.Frame.AddPass(
            "OceanAlbedoSim", Rendering::PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& p)
            { p.Write(albedoRG, RenderGraph::RGTextureWrite::Storage); },
            [featurePtrAlbedo, buf = albedoParams.Buffer,
             off = albedoParams.Offset](RenderGraph::RGContext& ctx)
            {
                if (ctx.Cmd && ctx.GetDevice())
                    featurePtrAlbedo->GetAlbedo().RecordDispatch(ctx.GetDevice(), ctx.Cmd, buf, off);
            });
        d.Frame.MarkOutput(albedoRG, RenderGraph::RGImageLayout::ShaderReadOnly);
        feature.GetSplineRaster().Declare(d.Frame, device, 3u, albedoRG, albedo.GetLayout(),
                                          albedo.GetResolution());
    }

    // Schedule the FFT wave simulation once per frame (the sim is global; views
    // share it). The declared Write on the displacement is what orders every
    // declared reader (foam, height-field bakes, the surface span) after it;
    // the FFT's INTERNAL chain textures stay manually barriered inside
    // RecordDispatch — they never leave the pass, so the graph cannot see them.
    if (simulateWaves && feature.IsFFTReady() && feature.TryClaimFFTDispatch(frameIndex))
    {
        auto* featurePtr = &feature;
        const auto fftParams = d.Frame.AllocUpload<OceanFFTParamsGPU>();
        if (fftParams.Valid())
            feature.GetFFT().FillParams(*fftParams.Ptr);
        const RenderGraph::RGTexture fftDispRG = feature.GetFFT().ImportDisplacementRG(d.Frame);
        d.Frame.AddPass(
            "OceanFFTGenerate", Rendering::PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& p)
            { p.Write(fftDispRG, RenderGraph::RGTextureWrite::Storage); },
            [featurePtr, buf = fftParams.Buffer, off = fftParams.Offset](RenderGraph::RGContext& ctx)
            {
                if (ctx.Cmd && ctx.GetDevice())
                    featurePtr->GetFFT().RecordDispatch(ctx.GetDevice(), ctx.Cmd, buf, off);
            });
        d.Frame.MarkOutput(fftDispRG, RenderGraph::RGImageLayout::ShaderReadOnly);

        for (uint32 stream = 0u; stream < kMaxOceanLocalFFTStreams; ++stream)
        {
            if (!feature.IsLocalFFTReady(stream) ||
                !feature.TryClaimLocalFFTDispatch(stream, frameIndex))
            {
                continue;
            }

            auto* featurePtrLocal = &feature;
            const uint32 localStream = stream;
            const auto localFFTParams = d.Frame.AllocUpload<OceanFFTParamsGPU>();
            if (localFFTParams.Valid())
                feature.GetLocalFFT(stream).FillParams(*localFFTParams.Ptr);
            const RenderGraph::RGTexture localDispWriteRG =
                feature.GetLocalFFT(stream).ImportDisplacementRG(d.Frame);
            d.Frame.AddPass(
                ("OceanLocalFFTGenerate" + std::to_string(stream)).c_str(),
                Rendering::PassPhase::kEarlySetup,
                [&](RenderGraph::RGPassBuilder& p)
                { p.Write(localDispWriteRG, RenderGraph::RGTextureWrite::Storage); },
                [featurePtrLocal, localStream, buf = localFFTParams.Buffer,
                 off = localFFTParams.Offset](RenderGraph::RGContext& ctx)
                {
                    if (ctx.Cmd && ctx.GetDevice())
                        featurePtrLocal->GetLocalFFT(localStream).RecordDispatch(
                            ctx.GetDevice(), ctx.Cmd, buf, off);
                });
            d.Frame.MarkOutput(localDispWriteRG, RenderGraph::RGImageLayout::ShaderReadOnly);

            if (feature.IsLocalHeightFieldReady(stream))
            {
                const OceanParamsGPU hp = feature.GetParams();
                auto& localHF = feature.GetLocalHeightField(stream);
                localHF.BeginFrame(cameraX, cameraZ);
                localHF.SetFrameInputs(feature.GetLocalDisplacementTexture(stream),
                                       feature.GetLocalDisplacementSampler(stream),
                                       feature.GetLocalFFT(stream).GetCascadeCount(), hp.SeaLevel,
                                       hp.Weight, hp.MaxHorizontalDisplacement,
                                       hp.MaxVerticalDisplacement,
                                       hp.WaveOriginOffsetX, hp.WaveOriginOffsetZ);

                auto* featurePtrLocalHF = &feature;
                const auto localHFParams = d.Frame.AllocUpload<OceanHeightQueryParamsGPU>();
                if (localHFParams.Valid())
                    localHF.FillParams(*localHFParams.Ptr);
                // Declared read of the per-stream displacement: the RAW edge
                // orders this bake after its local FFT (previously unordered —
                // it relied on the scheduler tiebreak). PreventCulling stays:
                // the bake's sink is a CPU readback ring, invisible to the graph.
                const RenderGraph::RGTexture localDispRG =
                    feature.GetLocalFFT(stream).ImportDisplacementRG(d.Frame);
                d.Frame.AddPass(
                    ("OceanLocalHeightFieldBake" + std::to_string(stream)).c_str(),
                    Rendering::PassPhase::kEarlySetup,
                    [&](RenderGraph::RGPassBuilder& p)
                    {
                        if (localDispRG.IsValid())
                            p.Read(localDispRG, RenderGraph::RGTextureRead::SampledCompute);
                        p.PreventCulling();
                    },
                    [featurePtrLocalHF, framePtr = &d.Frame, localStream,
                     buf = localHFParams.Buffer,
                     off = localHFParams.Offset](RenderGraph::RGContext& ctx)
                    {
                        if (ctx.Cmd && ctx.GetDevice())
                            featurePtrLocalHF->GetLocalHeightField(localStream).RecordDispatch(
                                ctx.GetDevice(), ctx.Cmd, *framePtr, buf, off);
                    });
            }
        }

        // Buoyancy height-field bake: also reads this frame's FFT displacement, so
        // it runs in the same once-per-frame FFT claim, right after it. Snaps the
        // query tile to the same viewpoint camera as the sims, stashes the surface
        // shape controls, then records the bake (which reads back to the CPU).
        if (feature.IsHeightFieldReady())
        {
            const OceanParamsGPU hp = feature.GetParams();
            auto& hf = feature.GetHeightField();
            hf.BeginFrame(cameraX, cameraZ);
            hf.SetFrameInputs(feature.GetDisplacementTexture(), feature.GetDisplacementSampler(),
                              feature.GetFFT().GetCascadeCount(), hp.SeaLevel, hp.Weight,
                              hp.MaxHorizontalDisplacement, hp.MaxVerticalDisplacement,
                              hp.WaveOriginOffsetX, hp.WaveOriginOffsetZ);

            auto* featurePtrHF = &feature;
            const auto hfParams = d.Frame.AllocUpload<OceanHeightQueryParamsGPU>();
            if (hfParams.Valid())
                hf.FillParams(*hfParams.Ptr);
            // Declared read of this frame's displacement (RAW edge to the FFT
            // pass); PreventCulling stays — the sink is a CPU readback ring.
            const RenderGraph::RGTexture hfDispRG = feature.GetFFT().ImportDisplacementRG(d.Frame);
            d.Frame.AddPass(
                "OceanHeightFieldBake", Rendering::PassPhase::kEarlySetup,
                [&](RenderGraph::RGPassBuilder& p)
                {
                    if (hfDispRG.IsValid())
                        p.Read(hfDispRG, RenderGraph::RGTextureRead::SampledCompute);
                    p.PreventCulling();
                },
                [featurePtrHF, framePtr = &d.Frame, buf = hfParams.Buffer,
                 off = hfParams.Offset](RenderGraph::RGContext& ctx)
                {
                    if (ctx.Cmd && ctx.GetDevice())
                        featurePtrHF->GetHeightField().RecordDispatch(ctx.GetDevice(), ctx.Cmd,
                                                                      *framePtr, buf, off);
                });
        }

    }

    // Compose the tileable FFT bands into a world-coordinate cascade. The backing
    // tile snaps to the camera, but every output texel is evaluated from absolute
    // world XZ, so camera motion does not alter phase. The explicit RG dependency
    // orders this after FFT and before all combined-surface consumers.
    bool combineAvailable = false;
    RenderGraph::RGTexture combineRG{};
    if (simulateWaves && feature.IsCombineReady() && feature.IsCombineEnabled() && feature.IsFFTReady() &&
        feature.TryClaimCombineDispatch(frameIndex))
    {
        feature.GetCombineSim().BeginFrame(cameraX, cameraZ);
        auto* featurePtrCombine = &feature;
        const auto combineParams = d.Frame.AllocUpload<OceanCombineParamsGPU>();
        if (combineParams.Valid())
            feature.GetCombineSim().FillParams(*combineParams.Ptr);
        const RenderGraph::RGTexture fftReadRG = feature.GetFFT().ImportDisplacementRG(d.Frame);
        combineRG = feature.GetCombineSim().ImportRG(d.Frame);
        d.Frame.AddPass(
            "OceanCombineWaves", Rendering::PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& p)
            {
                if (fftReadRG.IsValid())
                    p.Read(fftReadRG, RenderGraph::RGTextureRead::SampledCompute);
                p.Write(combineRG, RenderGraph::RGTextureWrite::Storage);
            },
            [featurePtrCombine, buf = combineParams.Buffer,
             off = combineParams.Offset](RenderGraph::RGContext& ctx)
            {
                if (ctx.Cmd && ctx.GetDevice())
                    featurePtrCombine->GetCombineSim().RecordDispatch(ctx.GetDevice(), ctx.Cmd,
                                                                      buf, off);
            });
        d.Frame.MarkOutput(combineRG, RenderGraph::RGImageLayout::ShaderReadOnly);
        combineAvailable = true;
    }
    else if (feature.IsCombineReadyForFrame(frameIndex))
    {
        combineRG = feature.GetCombineSim().ImportRG(d.Frame);
        combineAvailable = combineRG.IsValid();
    }

    // Owner-keyed arbitrary collision queries. Unlike the legacy camera tile,
    // this dispatch evaluates only the requested world points and therefore
    // covers off-screen/headless physics registrations without expanding a
    // fixed readback texture. Results land through a token-signaled ring and are
    // consumed by the provider on a later extraction tick.
    if (feature.IsGPUQueryReady())
    {
        feature.GetGPUQuery().PollCompleted(device);
        if (feature.GetGPUQuery().HasPending() && feature.IsFFTReady() &&
            feature.TryClaimGPUQueryDispatch(frameIndex))
        {
            feature.GetGPUQuery().SetFrameInputs(
                feature.GetDisplacementTexture(), feature.GetDisplacementSampler(),
                combineAvailable ? feature.GetCombineSim().GetTexture()
                                 : Rendering::TextureHandle{},
                combineAvailable ? feature.GetCombineSim().GetSampler()
                                 : Rendering::SamplerHandle{});
            const OceanCascadeLayoutGPU combinedLayout =
                combineAvailable ? feature.GetCombineSim().GetLayout()
                                 : OceanCascadeLayoutGPU{};
            if (feature.GetGPUQuery().PrepareNext(feature.GetParams(), combinedLayout,
                                                  combineAvailable))
            {
                const auto queryParams = d.Frame.AllocUpload<OceanGPUQueryParamsGPU>();
                if (queryParams.Valid())
                    feature.GetGPUQuery().FillParams(*queryParams.Ptr);
                const RenderGraph::RGTexture queryFFTRG =
                    feature.GetFFT().ImportDisplacementRG(d.Frame);
                auto* featurePtrQuery = &feature;
                d.Frame.AddPass(
                    "OceanGPUQueries", Rendering::PassPhase::kEarlySetup,
                    [&](RenderGraph::RGPassBuilder& p)
                    {
                        if (queryFFTRG.IsValid())
                            p.Read(queryFFTRG, RenderGraph::RGTextureRead::SampledCompute);
                        if (combineAvailable && combineRG.IsValid())
                            p.Read(combineRG, RenderGraph::RGTextureRead::SampledCompute);
                        p.PreventCulling();
                    },
                    [featurePtrQuery, framePtr = &d.Frame, buf = queryParams.Buffer,
                     off = queryParams.Offset](RenderGraph::RGContext& ctx)
                    {
                        if (ctx.Cmd && ctx.GetDevice())
                            featurePtrQuery->GetGPUQuery().RecordDispatch(
                                ctx.GetDevice(), ctx.Cmd, *framePtr, buf, off);
                    });
            }
        }
    }

    if (feature.IsFoamReady())
        feature.GetFoamSim().SetCombinedInputs(
            combineAvailable ? feature.GetCombineSim().GetTexture()
                             : Rendering::TextureHandle{},
            combineAvailable ? feature.GetCombineSim().GetSampler()
                             : Rendering::SamplerHandle{},
            combineAvailable ? feature.GetCombineSim().GetLayout()
                             : OceanCascadeLayoutGPU{},
            combineAvailable);

    // Foam simulation: runs once per frame after any scheduled FFT pass in source
    // order. The foam controls + dt are stashed by OceanExtractionSystem via
    // SetFrameInputs; here we flip the ping-pong, snap the write target to the
    // active viewpoint camera, then schedule the dispatch. Keeping this on its own
    // claim prevents the sampled foam layout from trailing the camera when the FFT
    // claim path is skipped by another view/path.
    if (waterInView && feature.IsFoamReady() && feature.GetFoamSim().GetPendingSubstepCount() > 0u &&
        feature.TryClaimFoamDispatch(frameIndex))
    {
        auto* featurePtrFoam = &feature;
        const RenderGraph::RGTexture foamDispRG = combineAvailable
                                                     ? combineRG
                                                     : (feature.IsFFTReady()
                                                            ? feature.GetFFT().ImportDisplacementRG(d.Frame)
                                                            : RenderGraph::RGTexture{});
        const RenderGraph::RGTexture foamSeabedRG =
            feature.IsSeabedDepthReady() ? feature.GetSeabedDepth().ImportRG(d.Frame)
                                         : RenderGraph::RGTexture{};
        const RenderGraph::RGTexture foamFlowRG =
            feature.IsFlowReady() ? feature.GetFlow().ImportRG(d.Frame)
                                  : RenderGraph::RGTexture{};
        const uint32 substepCount = feature.GetFoamSim().GetPendingSubstepCount();
        for (uint32 substep = 0u; substep < substepCount; ++substep)
        {
            const uint32 writeIndex = feature.GetFoamSim().BeginSubstep(cameraX, cameraZ);
            const RenderGraph::RGTexture foamWriteRG =
                feature.GetFoamSim().ImportWriteRG(d.Frame, writeIndex);
            const RenderGraph::RGTexture foamPrevRG =
                feature.GetFoamSim().ImportPrevRG(d.Frame, writeIndex);
            const auto foamParams = d.Frame.AllocUpload<OceanFoamParamsGPU>();
            if (foamParams.Valid())
                feature.GetFoamSim().FillParams(*foamParams.Ptr, writeIndex, substep);
            d.Frame.AddPass(
                "OceanFoamSim", Rendering::PassPhase::kEarlySetup,
                [&](RenderGraph::RGPassBuilder& p)
                {
                    if (foamPrevRG.IsValid())
                        p.Read(foamPrevRG, RenderGraph::RGTextureRead::SampledCompute);
                    if (foamDispRG.IsValid())
                        p.Read(foamDispRG, RenderGraph::RGTextureRead::SampledCompute);
                    if (foamSeabedRG.IsValid())
                        p.Read(foamSeabedRG, RenderGraph::RGTextureRead::SampledCompute);
                    if (foamFlowRG.IsValid())
                        p.Read(foamFlowRG, RenderGraph::RGTextureRead::SampledCompute);
                    p.Write(foamWriteRG, RenderGraph::RGTextureWrite::Storage);
                },
                [featurePtrFoam, buf = foamParams.Buffer, off = foamParams.Offset,
                 writeIndex](RenderGraph::RGContext& ctx)
                {
                    if (ctx.Cmd && ctx.GetDevice())
                        featurePtrFoam->GetFoamSim().RecordDispatch(
                            ctx.GetDevice(), ctx.Cmd, buf, off, writeIndex);
                });
            d.Frame.MarkOutput(foamWriteRG, RenderGraph::RGImageLayout::ShaderReadOnly);
        }
        feature.GetFoamSim().FinishSubsteps();
    }

}

} // namespace GameEngine::Ocean
