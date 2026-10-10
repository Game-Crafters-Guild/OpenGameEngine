#include "SceneView/SceneViewRenderCoordinator.h"

#include "Core/Engine.h"
#include "EditorPanelIds.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "Engine/Rendering/Pipeline/PipelineFrameResources.h"
#include "Engine/Rendering/RenderDeviceContext.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewFinalize.h"
#include "Logger/Logger.h"
#include "Panels/SceneViewPanel.h"
#include "Scene/SceneThumbnailCapture.h"
#include "SceneViewController.h"
#include "UI/Controls/Mount.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace GameEngine::Editor
{
namespace
{
// Round a float pixel extent to the nearest whole pixel, floored at 2 (the floor
// avoids degenerate 1x1 allocations when a viewport collapses).
//
// 1:1 INVARIANT: the extent must equal the physical rect the UI consumes it at,
// or the panel-background sampler resolves a `cover` fit to a non-unit scale and
// every destination pixel becomes a weighted blend of source texels. The
// viewport element's rect is snapped to whole device pixels
// (UIElement::SetSnapRectToDevicePixels), so the value arriving here is already
// integral and this rounding changes nothing.
//
// Render scale is NOT applied here. It is per-view pipeline state
// (ViewRegistry::SetViewRenderScale): the pipeline rasterizes the world half at
// the reduced extent and crosses back to this extent before the overlays, so
// what the UI receives is always 1:1 with the rect however the scale is set.
inline std::uint32_t SnapToWholePx(float v)
{
    if (!(v >= 2.0f))
        return 2u;
    return static_cast<std::uint32_t>(std::lround(v));
}

constexpr SceneViewPanel::ViewportSlot kSceneViewSlots[] = {
    SceneViewPanel::ViewportSlot::Perspective,
    SceneViewPanel::ViewportSlot::Top,
    SceneViewPanel::ViewportSlot::Front,
    SceneViewPanel::ViewportSlot::Side,
};

// UI external-texture names for the RenderGraph arm, keyed by CONTROLLER (not slot):
// slots can reassign their view kind, and a name that followed the slot would
// cross-wire pane textures mid-reassignment. "scene_main" is the CSS-visible
// main scene name (theme/views.css, theme/scene-view.css).
constexpr const char* kSceneQuadExternalNames[3] = {"scene_view_top", "scene_view_front",
                                                    "scene_view_side"};

const char* ExternalSceneNameForController(SceneViewRenderCoordinator::EditorWindowContext* ctx,
                                           const SceneViewController* controller)
{
    if (!ctx || !controller)
        return nullptr;
    if (controller == ctx->scene.get())
        return "scene_main";
    for (size_t i = 0; i < ctx->sceneQuadViews.size(); ++i)
    {
        if (ctx->sceneQuadViews[i].get() == controller)
            return kSceneQuadExternalNames[i];
    }
    return nullptr;
}

template <typename TPanel>
TPanel* ResolveMountedPanelForWindow(SceneViewRenderCoordinator::EditorWindowContext* ctx, const char* mountId)
{
    if (!ctx || !ctx->ui || !mountId)
        return nullptr;

    UIElement* root = ctx->ui->GetRootElement();
    if (!root)
        return nullptr;

    if (auto* mount = dynamic_cast<Mount*>(root->FindById(mountId)))
        return dynamic_cast<TPanel*>(mount->GetTarget());
    return nullptr;
}

UIElement* FindHdrTestPatternOverlay(UIElement* viewport)
{
    if (!viewport)
        return nullptr;
    for (const auto& child : viewport->GetChildren())
    {
        if (child && child->HasClass("scene-view-hdr-test-pattern-overlay"))
            return child.get();
    }
    return nullptr;
}

void HideHdrTestPatternOverlay(UIElement* viewport)
{
    UIElement* overlay = FindHdrTestPatternOverlay(viewport);
    if (!overlay)
        return;
    overlay->Overrides()
        .Set(Style::Display, DisplayMode::None)
        .Set(Style::PointerEvents, false);
    overlay->Styles()
        .ResetBackgroundImage()
        .ResetBackgroundSize()
        .ResetBackgroundPosition()
        .ResetBackgroundRepeat()
        .ResetBackgroundTint();
}
} // namespace

SceneViewPanel* SceneViewRenderCoordinator::SyncMountedPanelControllers(EditorWindowContext* ctx)
{
    if (!ctx)
        return nullptr;

    SceneViewPanel* svPanel = ResolveMountedPanelForWindow<SceneViewPanel>(ctx, EditorPanelIds::MountSceneView);
    if (!svPanel)
        return nullptr;

    svPanel->SetSceneController(ctx->scene.get());
    if (ctx->sceneQuadViews[0])
        svPanel->SetSceneControllerForSlot(SceneViewPanel::ViewportSlot::Top, ctx->sceneQuadViews[0].get());
    if (ctx->sceneQuadViews[1])
        svPanel->SetSceneControllerForSlot(SceneViewPanel::ViewportSlot::Front, ctx->sceneQuadViews[1].get());
    if (ctx->sceneQuadViews[2])
        svPanel->SetSceneControllerForSlot(SceneViewPanel::ViewportSlot::Side, ctx->sceneQuadViews[2].get());

    return svPanel;
}

void SceneViewRenderCoordinator::ForEachController(EditorWindowContext* ctx,
                                                   const std::function<void(SceneViewController*)>& fn)
{
    if (!ctx || !fn)
        return;
    if (ctx->scene)
        fn(ctx->scene.get());
    for (auto& quadScene : ctx->sceneQuadViews)
    {
        if (quadScene)
            fn(quadScene.get());
    }
}

void SceneViewRenderCoordinator::SyncSelectionAcrossControllers(EditorWindowContext* ctx,
                                                                SceneViewController* source)
{
    if (!ctx || !source)
        return;

    const auto& selected = source->GetSelectedEntities();
    if (ctx->scene && ctx->scene.get() != source)
        ctx->scene->SilentlySetSelection(selected);
    for (auto& quadScene : ctx->sceneQuadViews)
    {
        if (quadScene && quadScene.get() != source)
            quadScene->SilentlySetSelection(selected);
    }
}

void SceneViewRenderCoordinator::SyncViewportStateAcrossControllers(EditorWindowContext* ctx,
                                                                    SceneViewController* source)
{
    if (!ctx || !source)
        return;

    const SceneViewController::ToolKind activeTool = source->GetActiveToolKind();
    const auto transformMode = source->GetTransformMode();
    const bool localTransformSpace = source->IsTransformSpaceLocal();
    const bool showGizmos = source->AreGizmosVisible();
    const bool showTransformGizmos = source->AreTransformGizmosVisible();
    const bool showLightGizmos = source->AreLightGizmosVisible();
    const bool showMarkupGizmos = source->AreMarkupGizmosVisible();
    const bool showGrid = source->IsGridVisible();
    const bool gridSnapEnabled = source->IsGridSnapEnabled();
    const float gridSnapSize = source->GetGridSnapSize();
    const float gridOpacity = source->GetGridOpacity();
    const bool postProcessingEnabled = source->IsPostProcessingEnabled();

    ForEachController(ctx, [&](SceneViewController* controller)
    {
        if (!controller || controller == source)
            return;

        controller->SetGizmosVisible(showGizmos);
        controller->SetTransformGizmosVisible(showTransformGizmos);
        controller->SetLightGizmosVisible(showLightGizmos);
        controller->SetMarkupGizmosVisible(showMarkupGizmos);
        controller->SetGridVisible(showGrid);
        controller->SetGridSnapEnabled(gridSnapEnabled);
        controller->SetGridSnapSize(gridSnapSize);
        controller->SetGridOpacity(gridOpacity);
        controller->SetPostProcessingEnabled(postProcessingEnabled);

        if (controller->IsTransformSpaceLocal() != localTransformSpace)
            controller->ToggleTransformSpace();

        if (activeTool == SceneViewController::ToolKind::Transform &&
            (controller->GetActiveToolKind() != activeTool || controller->GetTransformMode() != transformMode))
        {
            controller->SetTransformMode(transformMode);
        }
        if (controller->GetActiveToolKind() != activeTool)
            controller->SetActiveTool(activeTool);
    });
}

bool SceneViewRenderCoordinator::TryResolveRenderableViewportSize(EditorWindowContext* ctx,
                                                                  UIElement* viewport,
                                                                  std::uint32_t& outW,
                                                                  std::uint32_t& outH)
{
    outW = 1;
    outH = 1;
    if (!ctx || !viewport)
        return false;
    if (viewport->HasClass("hidden") || viewport->HasClass("quad-collapsed"))
        return false;

    const float vpW = viewport->GetLayoutWidth();
    const float vpH = viewport->GetLayoutHeight();
    if (vpW <= 1.0f || vpH <= 1.0f)
        return false;

    const float cs = ctx->ui ? ctx->ui->GetContentScale() : 1.0f;
    outW = SnapToWholePx(vpW * cs);
    outH = SnapToWholePx(vpH * cs);
    return outW > 1u && outH > 1u;
}

// Quad layout on the multi-view spine (8e-3): every renderable pane declares
// its own full-pipeline view into the RenderGraph frame (the old arm's lightweight
// non-pipeline panes are gone). ORDER CONTRACT: ctx->scene's entry is pushed
// FIRST when renderable — targets[0] stays the main-scene entry.
static bool CollectQuadRG(SceneViewRenderCoordinator::EditorWindowContext* ctx,
                           Rendering::RenderGraph::RGFrame& frame,
                           SceneViewPanel* svPanel,
                           SceneViewController* activeController,
                           std::vector<Engine::Renderer::Pipeline::ViewTargetsRG>& outTargets)
{
    struct PaneDeclare
    {
        SceneViewController* Controller = nullptr;
        UIElement* Viewport = nullptr;
        std::uint32_t Width = 1;
        std::uint32_t Height = 1;
    };
    std::array<PaneDeclare, static_cast<size_t>(SceneViewPanel::ViewportSlot::Count)> panes{};
    size_t paneCount = 0;
    float fullQuadTop = std::numeric_limits<float>::infinity();
    float fullQuadBottom = 0.0f;
    for (SceneViewPanel::ViewportSlot slot : kSceneViewSlots)
    {
        if (paneCount >= panes.size())
            break;
        SceneViewController* controller = svPanel->GetSceneControllerForSlot(slot);
        UIElement* viewport = svPanel->GetViewportElementForSlot(slot);
        std::uint32_t svW = 1;
        std::uint32_t svH = 1;
        if (!controller || !viewport ||
            !SceneViewRenderCoordinator::TryResolveRenderableViewportSize(ctx, viewport, svW, svH))
            continue;
        panes[paneCount++] = PaneDeclare{controller, viewport, svW, svH};
        fullQuadTop = std::min(fullQuadTop, viewport->GetLayoutY());
        fullQuadBottom = std::max(fullQuadBottom, viewport->GetLayoutY() + viewport->GetLayoutHeight());
    }


    const float fullQuadLogicalHeight =
        std::isfinite(fullQuadTop) ? std::max(0.0f, fullQuadBottom - fullQuadTop) : 0.0f;
    const auto declarePane = [&](const PaneDeclare& pane) -> bool
    {
        // A pane renders at its own extent but the measure overlay's labels
        // must read as if drawn at full quad height (old-arm parity).
        const float paneHeight = pane.Viewport->GetLayoutHeight();
        const float measureScaleCompensation =
            (paneHeight > 1.0f && fullQuadLogicalHeight > paneHeight)
                ? std::clamp(fullQuadLogicalHeight / paneHeight, 1.0f, 4.0f)
                : 1.0f;
        pane.Controller->SetMeasureViewportScaleCompensation(measureScaleCompensation);
        Engine::Renderer::Pipeline::ViewTargetsRG vt{};
        if (!pane.Controller->DeclareTargetsRG(frame, pane.Width, pane.Height, ctx->windowId, vt))
            return false;
        if (pane.Controller == activeController)
        {
            if (auto* rs = pane.Controller->GetRenderServices())
                rs->Views().SetViewpointCamera(pane.Controller->GetCameraId());
        }
        if (vt.View != 0)
            outTargets.push_back(vt);
        return true;
    };

    bool anyDeclared = false;
    for (size_t i = 0; i < paneCount; ++i)
    {
        if (panes[i].Controller == ctx->scene.get())
            anyDeclared = declarePane(panes[i]) || anyDeclared;
    }
    for (size_t i = 0; i < paneCount; ++i)
    {
        if (panes[i].Controller != ctx->scene.get())
            anyDeclared = declarePane(panes[i]) || anyDeclared;
    }

    // Bookmark preview rides the same frame, active-controller-only (8c-3).
    if (anyDeclared)
    {
        Engine::Renderer::Pipeline::ViewTargetsRG previewVt{};
        if (activeController->DeclarePreviewTargetsRG(frame, ctx->windowId, previewVt))
            outTargets.push_back(previewVt);
    }

    return anyDeclared;
}

bool SceneViewRenderCoordinator::CollectRG(
    EditorWindowContext* ctx, Rendering::RenderGraph::RGFrame& frame,
    std::vector<Engine::Renderer::Pipeline::ViewTargetsRG>& outTargets)
{
    if (!ctx || !ctx->ui || !ctx->capabilities.worldRender || !ctx->scene)
        return false;

    SceneViewPanel* svPanel = SyncMountedPanelControllers(ctx);
    if (!svPanel)
    {
        // No mounted scene panel: nothing declares. The scene view stops being
        // re-armed (RequestViewFrame runs only from DeclareTargetsRG), so its
        // OnDemand participation lapses to zero within two frames — extraction,
        // batch-key build, culling, and shadow dispatches gate off at no
        // per-frame cost — while the viewId + HZB visibility history survive for
        // an instant warm reopen (D9/A1.6). The view is released only when the
        // controller is destroyed. Retain the last complete registration so a
        // reactivated tab can present it while extraction catches up.
        return false;
    }

    SceneViewController* activeController =
        svPanel->GetSceneControllerForSlot(svPanel->GetActiveViewportSlot());
    if (!activeController)
        activeController = ctx->scene.get();
    SyncViewportStateAcrossControllers(ctx, activeController);
    // Camera input drives only the ACTIVE pane's controller (old-arm parity).
    activeController->SetCameraAnglesDeg(svPanel->GetYawDeg(), svPanel->GetPitchDeg());
    activeController->UpdateOrbit(svPanel->IsOrbiting());

    if (svPanel->IsQuadViewEnabled())
    {
        const bool declared = CollectQuadRG(ctx, frame, svPanel, activeController,
                                             outTargets);
        const auto pose = activeController->GetCameraPose();
        svPanel->SetYawPitch(pose.YawDeg, pose.PitchDeg);
        return declared;
    }

    activeController->SetMeasureViewportScaleCompensation(1.0f);


    UIElement* viewport = svPanel->GetViewportElementForSlot(svPanel->GetActiveViewportSlot());
    std::uint32_t svW = 1;
    std::uint32_t svH = 1;
    if (!TryResolveRenderableViewportSize(ctx, viewport, svW, svH))
        return false;

    Engine::Renderer::Pipeline::ViewTargetsRG vt{};
    if (!activeController->DeclareTargetsRG(frame, svW, svH, ctx->windowId, vt))
        return false;
    if (auto* rs = activeController->GetRenderServices())
        rs->Views().SetViewpointCamera(activeController->GetCameraId());
    if (vt.View != 0)
        outTargets.push_back(vt);

    // Bookmark preview rides the same frame as an extra span entry (8c-3).
    // Scene stays targets[0] (DeclareOverlaysRG indexes it); per-entry
    // ViewId keying makes order among the other entries irrelevant.
    Engine::Renderer::Pipeline::ViewTargetsRG previewVt{};
    if (activeController->DeclarePreviewTargetsRG(frame, ctx->windowId, previewVt))
        outTargets.push_back(previewVt);

    const auto pose = activeController->GetCameraPose();
    svPanel->SetYawPitch(pose.YawDeg, pose.PitchDeg);
    return true;
}

void SceneViewRenderCoordinator::DeclareOverlaysRG(
    EditorWindowContext* ctx, Rendering::RenderGraph::RGFrame& frame,
    std::span<const Engine::Renderer::Pipeline::ViewTargetsRG> targets)
{
    if (!ctx || !ctx->ui || !ctx->scene || targets.empty())
        return;
    // MAIN RS (8e): a floating window's private RS never saw the pipeline
    // declares — the controllers were constructed on the main RS.
    auto* rs = EngineCore::GetInstance().GetRenderServices();
    if (!rs)
        return;

    SceneViewPanel* svPanel =
        ResolveMountedPanelForWindow<SceneViewPanel>(ctx, EditorPanelIds::MountSceneView);
    if (!svPanel)
        return;
    SceneViewController* activeController =
        svPanel->GetSceneControllerForSlot(svPanel->GetActiveViewportSlot());
    if (!activeController)
        activeController = ctx->scene.get();

    // Post-spine bookmark-preview half (8c-3): snapshot copy on the close
    // frame + widget screenshot tickets. Runs regardless of the panes'
    // output state below — the preview has its own pipeline output.
    activeController->FinalizePreviewRG(frame);

    // Per pane (8e-3): every scene controller with a span entry declares its
    // overlays onto ITS pipeline output, depth-testing against ITS imported
    // Depth from the entry. out.Out is the pipeline's ACTUAL FinalColor —
    // never the caller-imported Color/Resolve. The bookmark-preview entry
    // matches no controller's view id and is skipped naturally.
    ForEachController(ctx, [&](SceneViewController* controller)
                      {
                          for (const auto& vt : targets)
                          {
                              if (vt.View != controller->GetViewId())
                                  continue;
                              const auto out = rs->GetPipelineOutputRG(frame, controller->GetViewId());
                              if (!out.IsValid())
                                  return; // BindRG owns the warn-once for this state
                              if (frame.Graph().ResourceDesc(out.Out.Id).SampleCount > 1)
                                  return; // multisampled output: BindRG skips the bind too
                              controller->DeclareOverlaysRG(frame, out.Out, vt.Depth, rs);
                              return;
                          }
                      });
}

void SceneViewRenderCoordinator::BindRG(EditorWindowContext* ctx, Rendering::RenderGraph::RGFrame& frame,
                                         bool pureRGFrame)
{
    if (!ctx || !ctx->ui || !ctx->scene)
        return;
    // MAIN RS (8e) — see DeclareOverlaysRG.
    auto* rs = EngineCore::GetInstance().GetRenderServices();
    if (!rs)
        return;
    auto* device = rs->GetDevice();
    if (!device)
        return;

    SceneViewPanel* svPanel =
        ResolveMountedPanelForWindow<SceneViewPanel>(ctx, EditorPanelIds::MountSceneView);
    if (!svPanel)
        return;

    // The presented format every pane's finalize sizes to, resolved from THIS
    // window's own target handle once per frame — never from the device's
    // active-target ambient and never cached across frames (swapchain
    // recreates happen between frames).
    Rendering::IDevice* dev = ctx->renderCtx ? ctx->renderCtx->GetDevice() : nullptr;
    const Rendering::TextureFormat presentedFormat =
        dev ? dev->GetWindowTargetSwapchainFormat(ctx->renderCtx->GetWindowTarget())
            : Rendering::TextureFormat::Unknown;

    // Pass 1 — registry state, per CONTROLLER name. Re-point EVERY frame:
    // under FinalCopy elision the physical varies with PP toggles; caching it
    // across frames is the stale-image bug class. Controllers whose pane did
    // not declare this frame (quad off, hidden/collapsed pane) drop their
    // registry entry so nothing samples a stale physical.
    ForEachController(ctx, [&](SceneViewController* controller)
                      {
                          const char* name = ExternalSceneNameForController(ctx, controller);
                          if (!name)
                              return;
                          const bool isMainScene = controller == ctx->scene.get();
                          if (controller->IsWaitingForExtraction())
                          {
                              // Invalid across a device rebuild: this arm never
                              // reaches the snapshot's Update, so the accessor's
                              // generation compare is the only thing between a
                              // freed id and ImportExternalTexture.
                              const Rendering::TextureHandle cached =
                                  controller->GetLastPresentedTexture(*device);
                              if (!cached.IsValid())
                                  return;
                              const std::string importName = std::string(name) + ".LastPresented";
                              const Rendering::TextureFormat cachedFormat =
                                  device->GetTextureFormat(cached);
                              const Rendering::RenderGraph::RGTexture cachedRG =
                                  frame.ImportExternalTexture(
                                      importName.c_str(), cached,
                                      Rendering::ResourceState::ShaderResource, cachedFormat);
                              if (!cachedRG.IsValid())
                                  return;
                              // The cached copy keeps the space of the frame
                              // that rendered it — never the current mode.
                              ctx->ui->SetExternalTextureRG(
                                  name, controller->GetLastPresentedWidth(),
                                  controller->GetLastPresentedHeight(),
                                  controller->GetLastPresentedSpace(), cachedFormat);
                              ctx->ui->PublishExternalTextureRG(name, frame, cachedRG);
                              return;
                          }
                          const auto out = rs->GetPipelineOutputRG(frame, controller->GetViewId());
                          if (!out.IsValid())
                          {
                              ctx->ui->RemoveExternalTexture(name);
                              return;
                          }
                          // UI samples single-sample only (the MSAA guard the
                          // old arm ran through ValidateUiSampleTextureRef).
                          const auto& desc = frame.Graph().ResourceDesc(out.Out.Id);
                          if (desc.SampleCount > 1)
                          {
                              static bool sWarnedMsaaOutput = false;
                              if (!sWarnedMsaaOutput && isMainScene)
                              {
                                  Logger::Log::Warning(
                                      "Editor RenderGraph: pipeline output for the scene view is multisampled — "
                                      "'scene_main' skipped (no resolve in the chain?)");
                                  sWarnedMsaaOutput = true;
                              }
                              ctx->ui->RemoveExternalTexture(name);
                              return;
                          }
                          if (pureRGFrame)
                          {
                              // The view's own resolve: everything that composes
                              // this pane — the world, the post chain, and the
                              // overlay and gizmo passes DeclareOverlaysRG just
                              // wrote into the same image — is behind us, so the
                              // finalize's filters see world pixels and nothing
                              // else. Pure frames only: the finalize writes a
                              // TRANSIENT, which has no physical handle until
                              // Execute realizes it (RGFrame::PhysicalOf), and the
                              // hybrid arm below needs one that outlives
                              // declaration.
                              const Engine::Renderer::ViewFinalizeResult finalized =
                                  Engine::Renderer::DeclareViewFinalize(
                                      frame, out.Out, rs->GetPipelineOutputSpaceRG(), name,
                                      Engine::Renderer::ResolveViewDebandThresholdLsb(rs),
                                      presentedFormat, /*hostRefusal=*/false);
                              const auto& finalDesc =
                                  frame.Graph().ResourceDesc(finalized.Image.Id);
                              controller->UpdatePresentedSnapshotRG(frame, finalized.Image,
                                                                    finalized.Space);
                              // The UI pass lives in the SAME graph — register the
                              // name and publish THIS frame's id; RenderRG declares
                              // the sampled read (the barrier the hybrid era got
                              // from executing RenderGraph first).
                              ctx->ui->SetExternalTextureRG(
                                  name, finalDesc.Width, finalDesc.Height, finalized.Space,
                                  static_cast<Rendering::TextureFormat>(finalDesc.Format));
                              ctx->ui->PublishExternalTextureRG(name, frame, finalized.Image);
                          }
                          else
                          {
                              // RECORDED REGRESSION, landing 4 (2026-08-05): this
                              // arm reaches the screen with NO dither owner on a
                              // hardware-sRGB swapchain. The finalize above cannot
                              // run here (it writes a frame transient with no
                              // physical handle until Execute, while this arm binds
                              // out.Physical through the non-RG SetExternalTexture),
                              // and the terminal answers None because the bytes
                              // already sit on the presented step. No other pass
                              // dithers: the step's owner is the only filter site.
                              //
                              // Shipped because routing.pure is assigned true
                              // unconditionally, so no editor frame reaches this
                              // branch. Re-enabling the hybrid arm means landing R7
                              // first — a persistent-handle finalize target — or
                              // knowingly shipping an un-dithered viewport on an
                              // 8-bit sRGB display. This log is the trip-wire: if
                              // it ever appears, that decision is live again.
                              static bool s_WarnedHybridUnfiltered = false;
                              if (!s_WarnedHybridUnfiltered)
                              {
                                  s_WarnedHybridUnfiltered = true;
                                  Logger::Log::Warning(
                                      "Scene view '{}': hybrid (non-pure) frame — the view "
                                      "cannot finalize, so it presents with no dither and no "
                                      "deband. Land R7 (persistent-handle finalize target) "
                                      "before relying on this arm",
                                      name);
                              }
                              controller->UpdatePresentedSnapshotRG(
                                  frame, out.Out, rs->GetPipelineOutputSpaceRG());
                              ctx->ui->SetExternalTexture(name, out.Physical, desc.Width,
                                                          desc.Height,
                                                          rs->GetPipelineOutputSpaceRG());
                          }
                      });

    // Pass 2 — viewport elements, per SLOT: bind each slot's CURRENT
    // controller's name every frame so slot kind reassignment re-points the
    // element with it.
    bool boundAnyViewport = false;
    for (SceneViewPanel::ViewportSlot slot : kSceneViewSlots)
    {
        UIElement* viewport = svPanel->GetViewportElementForSlot(slot);
        if (!viewport)
            continue;
        SceneViewController* controller = svPanel->GetSceneControllerForSlot(slot);
        if (!controller && slot == svPanel->GetActiveViewportSlot())
            controller = ctx->scene.get();
        const char* name = controller ? ExternalSceneNameForController(ctx, controller) : nullptr;
        bool bound = false;
        if (controller && name)
        {
            if (controller->IsWaitingForExtraction())
            {
                bound = controller->GetLastPresentedTexture(*device).IsValid();
            }
            else
            {
                const auto out = rs->GetPipelineOutputRG(frame, controller->GetViewId());
                bound = out.IsValid() && frame.Graph().ResourceDesc(out.Out.Id).SampleCount <= 1;
            }
        }
        if (!bound)
        {
            ctx->ui->ClearElementBackgroundTexture(*viewport);
            continue;
        }
        HideHdrTestPatternOverlay(viewport);
        // Rebind only when the binding actually changes. The unconditional
        // Reset/Set cycle here dirtied the viewport element every rendered
        // frame (StyleOverrides::Reset erases a present override), which —
        // via the sibling-combinator conservative invalidation in
        // BuildYogaRecursive — kept the whole UI tree perpetually dirty:
        // idle frames never converged and pointer-only frames could never
        // gate. Slot kind reassignment still re-points the element the frame
        // the name changes.
        const auto currentBg = viewport->Overrides().Get(Style::BackgroundImage);
        const bool alreadyBound = currentBg.has_value() &&
                                  currentBg->Kind == BackgroundImageSource::SourceKind::ResourceName &&
                                  currentBg->Value == name &&
                                  !viewport->HasBackgroundImageTextureOverride();
        if (!alreadyBound)
        {
            ctx->ui->ClearElementBackgroundTexture(*viewport);
            viewport->Styles()
                .SetBackgroundResourceName(name)
                .SetBackgroundRepeat(BackgroundRepeat::NoRepeat)
                .SetBackgroundSizeCover()
                .SetBackgroundPositionPercent(50.0f, 50.0f)
                .ResetBackgroundTint();
        }
        boundAnyViewport = true;
    }

    if (!boundAnyViewport)
    {
        static bool sWarnedNoOutput = false;
        if (!sWarnedNoOutput)
        {
            Logger::Log::Warning(
                "Editor RenderGraph: no scene pane produced a pipeline output this frame — "
                "the scene viewport will not render");
            sWarnedNoOutput = true;
        }
    }
}

} // namespace GameEngine::Editor
