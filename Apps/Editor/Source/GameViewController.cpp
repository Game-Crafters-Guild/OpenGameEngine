#include "GameViewController.h"

#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/Entity.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "Engine/GameUI/GameUIHost.h"
#include "Engine/GameUI/GameplayUI.h"
#include "Engine/Rendering/Camera.h"
#include "Engine/Rendering/CameraAntiAliasing.h"
#include "Engine/Rendering/CameraAspectRatio.h"
#include "Engine/Rendering/CameraExposureHelpers.h"
#include "Engine/Rendering/ExposureReadbackFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewFinalize.h"
#include "Logger/Logger.h"

#include "Components/Rendering/Camera.h"
#include "Components/Transform.h"
#include "Engine/Rendering/Pipeline/PipelineFrameResources.h"
#include "Rendering/Common/Math.h"
#include "Rendering/Core/HzbCullingStrategy.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Passes/PixelPerfectUpscalePass.h"
#include "Rendering/Passes/SRGBEncodePass.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>

namespace GameEngine
{

namespace
{
// Dark neutral gray when no ECS camera drives Game View (~#161616).
constexpr float kGameViewNoCameraClearRgb = 0.09f;

} // namespace

using GameEngine::Rendering::TextureFormat;

GameViewController::GameViewController(Engine::Renderer::RenderServices* renderServices,
                                       std::filesystem::path editorAssetsDirectory)
    : m_RenderServices(renderServices),
      m_EditorAssetsDirectory(std::move(editorAssetsDirectory)),
      m_MovieCapture(renderServices)
{
    EnsureGameUI();
}

GameViewController::~GameViewController()
{
    try
    {
        m_MovieCapture.StopForShutdown();
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("GameView: exception while stopping movie capture during shutdown: {}", e.what());
    }
    catch (...)
    {
        Logger::Log::Error("GameView: unknown exception while stopping movie capture during shutdown");
    }
    m_PresentationSnapshot.Destroy(m_RenderServices ? m_RenderServices->GetDevice() : nullptr);
    DeactivateRenderView();
}

bool GameViewController::NeedsPresentationWarmup() const
{
    auto* device = m_RenderServices ? m_RenderServices->GetDevice() : nullptr;
    // No device: nothing has presented, so the view still needs warming.
    if (!device)
        return true;
    return !m_PresentationSnapshot.Texture(*device).IsValid() ||
           m_PresentationWarmupFramesRemaining > 0;
}

bool GameViewController::UpdatePresentedSnapshotRG(Rendering::RenderGraph::RGFrame& frame,
                                                   Rendering::RenderGraph::RGTexture source,
                                                   UI::UITextureSpace sourceSpace)
{
    auto* device = m_RenderServices ? m_RenderServices->GetDevice() : nullptr;
    const bool updated = device && m_PresentationSnapshot.Update(
                                      frame, *device, source, sourceSpace,
                                      "Editor.GameView.PresentationSnapshot");
    if (updated && m_PresentationWarmupFramesRemaining > 0)
        --m_PresentationWarmupFramesRemaining;
    return updated;
}

void GameViewController::DeactivateRenderView()
{
    m_DeclaredCamera.reset();
    UpdateExposureReadback(false);
    if (m_RenderServices)
    {
        if (m_ViewId != 0)
        {
            m_RenderServices->Views().ReleaseView(m_ViewId);
            m_ViewId = 0;
        }
        if (m_CameraId != 0)
        {
            m_RenderServices->Views().ReleaseCamera(m_CameraId);
            m_CameraId = 0;
        }
    }
}

void GameViewController::UpdateExposureReadback(bool enabled)
{
    if (!m_RenderServices || m_ViewId == 0)
        return;
    auto* readback = m_RenderServices->GetFeature<Engine::Renderer::ExposureReadbackFeature>();
    if (!readback && enabled)
        readback = &m_RenderServices->EnsureFeature<Engine::Renderer::ExposureReadbackFeature>();
    if (readback)
        readback->SetReadbackEnabled(m_ViewId, enabled);
}

void GameViewController::EnsureGameUI()
{
    if (m_GameUI || !m_RenderServices || !m_RenderServices->GetDevice())
        return;

    m_GameUI = GameUIHost::CreateForHost(
        m_RenderServices->GetDevice(), &EngineCore::GetInstance().GetAssetManager(),
        &EngineCore::GetInstance().GetJobSystem(), m_EditorAssetsDirectory,
        /*neverClearTarget=*/false);
    m_GameUiScale.Apply(*m_GameUI->GetUIManager());
    GameUI::SetHost(m_GameUI.get());
}

void GameViewController::HandleGameUiPointer(bool over, float x, float y, bool down, int mods)
{
    if (!m_GameUI)
        return;
    // Input may precede this window's next composite. Script callbacks must
    // resolve gameplay UI against the receiving host, not the last rendered one.
    GameUI::SetHost(m_GameUI.get());
    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    if (!over || !world || !m_GameUiWidth || !m_GameUiHeight ||
        world->GetWorldId() != m_GameUiWorldId ||
        world->GetLifecycleResetGeneration() != m_GameUiWorldGeneration)
    {
        m_GameUI->CancelPointer(down ? GameUIHost::PrimaryButtonState::Held
                                     : GameUIHost::PrimaryButtonState::Released);
        return;
    }
    GameUIHost::PointerState pointer{x * static_cast<float>(m_GameUiWidth),
                                     y * static_cast<float>(m_GameUiHeight), down, mods};
    m_GameUI->UpdatePointer(&pointer);
}

std::optional<Engine::Renderer::Camera> GameViewController::PrepareView(
    ECS::World* world, uint32_t width, uint32_t height)
{
    Engine::Renderer::RenderServices* rs = m_RenderServices;
    if (!rs || !rs->GetDevice())
        return std::nullopt;

    const uint32_t w = std::max(1u, width);
    const uint32_t h = std::max(1u, height);
    if (m_CameraId == 0)
        m_CameraId = rs->Views().AllocateCamera("GameView.Camera");
    if (m_ViewId == 0)
        m_ViewId = rs->Views().AllocateView("Game View (Editor)", m_CameraId,
                                            Rendering::ViewPurpose::Game,
                                            Rendering::ViewParticipation::OnDemand);
    else
        rs->Views().SetViewCamera(m_ViewId, m_CameraId);

    // OnDemand views participate in the next extraction frame when armed on
    // tab press, then are armed again by the normal declaration on mouse-up.
    // A hidden view still lapses within two frames when no activation is armed.
    rs->Views().RequestViewFrame(m_ViewId);
    rs->Views().SetCameraPostProcessMask(m_CameraId, 0xFFFFFFFFu);
    rs->Views().SetViewRenderLayerMask(m_ViewId, 1u);
    rs->Views().SetViewActiveRenderPipeline(m_ViewId, true);
    rs->Views().SetViewCullingStrategy(m_ViewId, Rendering::MakeDefaultOcclusionStrategyOrNull());
    // Render scale is per-view pipeline state: the render targets stay 1:1 with
    // the panel rect and the pipeline rasterizes the world half at scale x that
    // extent, crossing back before the overlays. Republished every frame so the
    // settings sliders stay live. Priority: the camera's explicit RenderScale
    // (set again below once the active camera resolves), else the
    // SceneViewSettings editor-side override (its default 1.0 means none),
    // else the engine default — so the project Render Scale (and the SSAA
    // mode driving it) reaches editor viewports.
    {
        const float svScale = Editor::SceneViewSettings::Get().GetRenderScale();
        rs->Views().SetViewRenderScale(m_ViewId, svScale == 1.0f
                                                     ? std::nullopt
                                                     : std::optional<float>(svScale));
    }
    if (world)
        rs->Views().SetViewWorldId(m_ViewId, world->GetWorldId());

    std::optional<Engine::Renderer::Camera> activeCam;
    if (world)
        activeCam = Engine::Renderer::FindActiveCamera(*world);
    m_HadCameraThisFrame = activeCam.has_value();
    UpdateExposureReadback(activeCam && activeCam->params.ExposureControl == Components::ExposureMode::Auto);
    if (!activeCam)
    {
        m_DeclaredCamera.reset();
        rs->Views().SetViewActiveRenderPipeline(m_ViewId, false);
        rs->Views().SetViewRenderLayerMask(m_ViewId, 0u);
        return std::nullopt;
    }

    Engine::Renderer::ApplyActiveCameraAspect(*rs, m_ViewId, m_CameraId, *activeCam, w, h);
    m_PixelPerfectStateRG = rs->Views().GetViewPixelPerfect(m_ViewId);
    m_PanelWRG = w;
    m_PanelHRG = h;
    rs->Views().SetCameraPostProcessMask(m_CameraId, activeCam->params.PostProcessMask);
    rs->Views().SetCameraExposure(m_CameraId, Engine::Renderer::ToCameraExposure(activeCam->params));

    Rendering::ViewClearConfig clear{};
    clear.clearColor = true;
    const Engine::Renderer::CameraAspectResolution aspectResolution =
        Engine::Renderer::ResolveCameraAspect(activeCam->params, w, h);
    const float bg = aspectResolution.letterbox.active ? 0.0f : 0.02f;
    clear.clearColorValue[0] = bg;
    clear.clearColorValue[1] = bg;
    clear.clearColorValue[2] = bg;
    clear.clearColorValue[3] = 1.0f;
    clear.clearDepth = true;
    clear.clearDepthValue = 0.0f;
    rs->Views().SetViewClearConfig(m_ViewId, clear);
    return activeCam;
}

void GameViewController::PrepareForActivation(ECS::World* world, uint32_t width, uint32_t height)
{
    (void)PrepareView(world, width, height);
}

bool GameViewController::DeclareTargetsRG(Rendering::RenderGraph::RGFrame& frame, uint32_t width,
                                           uint32_t height, uint64_t windowId, ECS::World* world,
                                           Engine::Renderer::Pipeline::ViewTargetsRG& outTargets)
{
    namespace RenderGraph = Rendering::RenderGraph;
    outTargets = {};
    m_NoCameraPhysicalRG = {};
    m_NoCameraColorId = RenderGraph::kInvalidId;
    m_NoCameraFor = {};
    m_PixelPerfectStateRG = {};
    m_PixelPerfectOutId = RenderGraph::kInvalidId;
    m_PixelPerfectFor = {};
    m_PixelPerfectPhysicalRG = {};
    m_PixelPerfectW = 0;
    m_PixelPerfectH = 0;
    m_WaitingForExtractionRG = false;

    Engine::Renderer::RenderServices* rs = m_RenderServices;
    if (!rs || !rs->GetDevice())
        return false;
    const uint32_t w = width > 0 ? width : 1u;
    const uint32_t h = height > 0 ? height : 1u;

    std::optional<Engine::Renderer::Camera> activeCam = PrepareView(world, w, h);

    const std::string baseName = "Editor.W" + std::to_string(windowId) + ".GameView";

    if (!m_HadCameraThisFrame)
    {
        // Declare a neutral-grey clear of a pooled color and export it for UI sampling.
        // The view joins no span (outTargets.View stays 0) so the spine declares nothing
        // for it. This uses its OWN pool key (".ClearOnly", fixed at (w,h,1)) so the camera
        // path's ".Color" key is never re-imported with a changing desc (MSAA/pixel-perfect)
        // across the grey<->camera transition; GetNoCameraColorRG samples it by stashed id.
        Rendering::TextureDesc colorDesc{};
        colorDesc.width = w;
        colorDesc.height = h;
        colorDesc.mipLevels = 1;
        colorDesc.arrayLayers = 1;
        colorDesc.sampleCount = 1;
        colorDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
        colorDesc.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
                          static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
        colorDesc.debugName = "Editor.GameView.ClearOnly";
        const RenderGraph::RGTexture color =
            frame.ImportPersistentTexture((baseName + ".ClearOnly").c_str(), colorDesc);
        if (!color.IsValid())
            return false;

        frame.AddPass(
            "GameView.NoCameraClearRG", static_cast<int32_t>(Rendering::PassPhase::kWorldRender),
            [&](RenderGraph::RGPassBuilder& p)
            {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Clear;
                ops.Store = RenderGraph::RGStoreOp::Store;
                ops.Clear.Color[0] = kGameViewNoCameraClearRgb;
                ops.Clear.Color[1] = kGameViewNoCameraClearRgb;
                ops.Clear.Color[2] = kGameViewNoCameraClearRgb;
                ops.Clear.Color[3] = 1.0f;
                p.AttachColor(0, color, ops);
            },
            [](RenderGraph::RGContext&) {});
        // The export (not PreventCulling) anchors the clear pass; the trailing
        // ShaderReadOnly transition is what lets the UI sample the physical.
        frame.MarkOutput(color, RenderGraph::RGImageLayout::ShaderReadOnly);
        m_NoCameraPhysicalRG = frame.PhysicalTexture(color);
        m_NoCameraColorId = color.Id;
        m_NoCameraFor.Stamp(frame);
        m_NoCameraW = w;
        m_NoCameraH = h;
        return true;
    }

    if (!rs->Views().IsViewExtractionCurrent(m_ViewId))
    {
        // Tab activation can arrive after this app frame's extraction phase.
        // Keep presenting the last complete Game View image while the request
        // feeds the next extraction instead of rendering a sky-only draw list.
        m_WaitingForExtractionRG = true;
        return true;
    }

    const Engine::Renderer::CameraAspectResolution aspectResolution =
        Engine::Renderer::ResolveCameraAspect(activeCam->params, w, h);
    // Per-camera AA policy, and the per-view AA state it publishes
    // (Engine/Rendering/CameraAntiAliasing.h owns it; the Player applies the
    // same one).
    const Engine::Renderer::ResolvedAntiAliasing aa = Engine::Renderer::ApplyCameraAntiAliasing(
        *rs, m_ViewId, *activeCam, aspectResolution);
    const uint32_t samples = aa.SampleCount;

    // Publishes CameraData + aspect/letterbox state onto the view registry.
    Engine::Renderer::ApplyActiveCameraAspect(*rs, m_ViewId, m_CameraId, *activeCam, w, h);
    // ApplyActiveCameraAspect publishes the full pixel-perfect state
    // (including the snapped sub-pixel remainder); re-read it so the 8e-5
    // upscale declare gets FracX/FracY, not the frac=0 estimate.
    m_PixelPerfectStateRG = rs->Views().GetViewPixelPerfect(m_ViewId);
    m_PanelWRG = w;
    m_PanelHRG = h;
    rs->Views().SetCameraPostProcessMask(m_CameraId, activeCam->params.PostProcessMask);
    rs->Views().SetCameraExposure(m_CameraId, Engine::Renderer::ToCameraExposure(activeCam->params));
    // The camera's explicit render scale outranks the editor-side override set
    // earlier this frame (0 = inherit; see Components::Camera::RenderScale).
    if (activeCam->params.RenderScale > 0.0f)
        rs->Views().SetViewRenderScale(m_ViewId, activeCam->params.RenderScale);

    // Clear config — letterbox bars are black, otherwise near-black.
    Rendering::ViewClearConfig clear{};
    clear.clearColor = true;
    const float bg = aspectResolution.letterbox.active ? 0.0f : 0.02f;
    clear.clearColorValue[0] = bg;
    clear.clearColorValue[1] = bg;
    clear.clearColorValue[2] = bg;
    clear.clearColorValue[3] = 1.0f;
    clear.clearDepth = true;
    clear.clearDepthValue = 0.0f;
    rs->Views().SetViewClearConfig(m_ViewId, clear);
    rs->WriteViewLightBuffer(m_ViewId);

    // Pool imports — window-namespaced (global pool keyspace). A smooth
    // pixel-perfect camera renders at the padded reference size; the 8e-5
    // upscale pass writes the full-panel output the UI samples.
    const uint32_t renderW =
        m_PixelPerfectStateRG.Active ? std::max(1u, m_PixelPerfectStateRG.PaddedWidth) : w;
    const uint32_t renderH =
        m_PixelPerfectStateRG.Active ? std::max(1u, m_PixelPerfectStateRG.PaddedHeight) : h;
    Rendering::TextureDesc colorDesc{};
    colorDesc.width = renderW;
    colorDesc.height = renderH;
    colorDesc.mipLevels = 1;
    colorDesc.arrayLayers = 1;
    colorDesc.sampleCount = samples;
    colorDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    // TransferSrc: the presentation snapshot copies whichever colour target is
    // single-sampled — this one when MSAA is off, the resolve (which inherits
    // this desc) when it is on. Pool-backed, so the desc is the only declaration.
    colorDesc.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
                      static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource) |
                      static_cast<uint32_t>(Rendering::TextureUsage::TransferSrc);
    colorDesc.debugName = "Editor.GameView.Color";

    Rendering::TextureDesc depthDesc = colorDesc;
    depthDesc.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    depthDesc.usage = static_cast<uint32_t>(Rendering::TextureUsage::DepthStencil) |
                      static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    depthDesc.debugName = "Editor.GameView.Depth";

    const RenderGraph::RGTexture color =
        frame.ImportPersistentTexture((baseName + ".Color").c_str(), colorDesc);
    const RenderGraph::RGTexture depth =
        frame.ImportPersistentTexture((baseName + ".Depth").c_str(), depthDesc);
    RenderGraph::RGTexture resolve{};
    if (samples > 1)
    {
        Rendering::TextureDesc resolveDesc = colorDesc;
        resolveDesc.sampleCount = 1;
        resolveDesc.debugName = "Editor.GameView.Resolve";
        resolve = frame.ImportPersistentTexture((baseName + ".Resolve").c_str(), resolveDesc);
    }
    if (!color.IsValid() || !depth.IsValid())
        return false;

    const auto& letterbox = rs->Views().GetViewLetterbox(m_ViewId);
    m_DeclaredCamera = DeclaredCamera{rs->Views().ResolveCameraData(m_ViewId),
                                     activeCam->params.ExposureControl, frame.FrameIndex(),
                                     letterbox.active ? letterbox.width : renderW,
                                     letterbox.active ? letterbox.height : renderH};
    outTargets.View = m_ViewId;
    outTargets.Color = color;
    outTargets.Depth = depth;
    outTargets.Resolve = resolve;
    return true;
}

bool GameViewController::DeclareGameUICompositeRG(Rendering::RenderGraph::RGFrame& frame,
                                                  Rendering::RenderGraph::RGTexture target,
                                                  UI::UITargetSpace targetSpace)
{
    if (!target.IsValid())
        return false;
    EnsureGameUI();
    if (!m_GameUI)
        return false;
    // Re-assert every composite: a tear-off/redock can construct a second
    // Game View after the first host was cleared, leaving GetHost() null.
    GameUI::SetHost(m_GameUI.get());
    const auto& outDesc = frame.Graph().ResourceDesc(target.Id);
    // Input is delivered at the viewport event, independently of this composite.
    // Remember the layout extent for subsequent normalized pointer coordinates.
    // NOTE: exact for the normal path where the viewport displays the target 1:1.
    // Under a pixel-perfect 2D camera the target is the padded reference RT shown
    // via an integer-zoom/black-bar upscale, so the mapping is offset there —
    // a follow-up would invert that upscale (Zoom/bars/frac) before scaling.
    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    const uint64_t worldId = world ? world->GetWorldId() : 0;
    const uint64_t generation = world ? world->GetLifecycleResetGeneration() : 0;
    if (worldId != m_GameUiWorldId || generation != m_GameUiWorldGeneration)
        m_GameUI->UpdatePointer(nullptr);
    m_GameUI->SyncDocuments(world);
    m_GameUiWorldId = worldId;
    m_GameUiWorldGeneration = generation;
    m_GameUiWidth = outDesc.Width;
    m_GameUiHeight = outDesc.Height;
    m_GameUiScale.Apply(*m_GameUI->GetUIManager());
    m_GameUI->UpdateAndRender(frame, target, targetSpace, outDesc.Width, outDesc.Height);
    return true;
}

Rendering::RenderGraph::RGTexture GameViewController::GetNoCameraColorRG(
    const Rendering::RenderGraph::RGFrame& frame) const
{
    if (!m_NoCameraFor.IsFor(frame))
        return {};
    return Rendering::RenderGraph::RGTexture{m_NoCameraColorId};
}

void GameViewController::DeclarePostPipelinePassesRG(Rendering::RenderGraph::RGFrame& frame,
                                                      uint64_t windowId,
                                                      Rendering::TextureFormat presentedFormat)
{
    namespace RenderGraph = Rendering::RenderGraph;

    // The RenderGraph poll site: drains finished movie frames to the encoder. The old
    // arm polls at the top of Record; RenderGraph frames never reach it.
    m_MovieCapture.Poll();

    if (!m_RenderServices)
        return;
    if (!m_HadCameraThisFrame)
    {
        // No ECS camera, but a camera-less scene can still own game UI (a Fullscreen
        // main menu, a pure-UI scene). The Player composites its UIDocuments regardless
        // of camera, so mirror that here instead of showing a bare grey view. The target
        // is the clear-only grey DeclareTargetsRG already stashed: a ready Fullscreen
        // menu's own Clear paints its background, an Overlay HUD blends over the grey.
        // SyncAndRender declares nothing when the scene has no documents, so an empty
        // camera-less scene still just shows grey.
        //
        // LinearSdr: the clear writes a display-referred grey constant into a linear F16
        // target that no finalize has touched, and LinearSdr reads back DisplayLinearSdr
        // — the exact stamp the editor's bind applies to this texture.
        (void)DeclareGameUICompositeRG(frame, GetNoCameraColorRG(frame),
                                       UI::UITargetSpace::LinearSdr());
        return;
    }
    const auto out = m_RenderServices->GetPipelineOutputRG(frame, m_ViewId);
    if (!out.IsValid())
    {
        static bool sWarnedNoOutput = false;
        if (!sWarnedNoOutput)
        {
            sWarnedNoOutput = true;
            Logger::Log::Warning(
                "GameView RenderGraph: no pipeline output for post-pipeline declares this frame");
        }
        return;
    }
    if (frame.Graph().ResourceDesc(out.Out.Id).SampleCount > 1)
    {
        static bool sWarnedMsaa = false;
        if (!sWarnedMsaa)
        {
            sWarnedMsaa = true;
            Logger::Log::Warning(
                "GameView RenderGraph: pipeline output is multisampled — post-pipeline passes skipped");
        }
        return;
    }

    // The view finalizes itself BEFORE the HUD composites, so the HUD blends on
    // sRGB code values and the finalize's filters see world pixels and nothing
    // else — the Player's order (#767 slice iii). The dither therefore lands
    // strictly UNDER the HUD: a flat chrome fill reaches the screen through one
    // ROP rounding instead of being grained by a filter that cannot tell chrome
    // from world.
    //
    // Two arms keep the linear chain, both decided inside the call — the first
    // by the policy's own eligibility, the second stated to it as this host's
    // refusal operand:
    //   - an active HDR output mode (the encoded blend space is SDR-only);
    //   - a frame recording an HDR movie, because that capture's encode carries
    //     a PQ/HLG `encodeOverride`, and an already-encoded source feeding an
    //     HDR encode is a Finalize contract violation the pass refuses outright
    //     (FinalizeContract.h) — the frame would lose its movie frame, not just
    //     its flip.
    //
    // That second arm is not free on the SCREEN: declining the finalize hands
    // the terminal encode a linear image, so for the whole duration of an HDR
    // recording the game view loses its per-view deband and dither and shows
    // whatever the terminal pass does full-frame. Trading the viewport's
    // filters for the recording's existence is the right way round, but it is a
    // trade, and it lasts as long as the recording does.
    //
    // Read before the finalize decision below and not inside the movie block:
    // the finalize is what this gates. The capture owns the recording state, so
    // it answers — non-recording frames stop at its first `has_value()` test.
    const bool hdrMovieThisFrame = m_MovieCapture.IsCapturingHdr();
    // One resolution for the whole frame: the finalize and the movie encode must
    // gate on the same value or a recording stops matching the screen.
    const float debandLsb = Engine::Renderer::ResolveViewDebandThresholdLsb(m_RenderServices);
    const Engine::Renderer::ViewFinalizeResult view = Engine::Renderer::DeclareViewFinalize(
        frame, out.Out, m_RenderServices->GetPipelineOutputSpaceRG(), "game_main", debandLsb,
        presentedFormat, /*hostRefusal=*/hdrMovieThisFrame);
    const bool viewEncoded = view.Space == UI::UITextureSpace::SdrFinalized();
    const Rendering::Passes::FinalizeInputSpace viewInputSpace =
        viewEncoded ? Rendering::Passes::FinalizeInputSpace::EncodedSrgb
                    : Rendering::Passes::FinalizeInputSpace::Linear;
    // Published before the HUD's early-out below: the bind consumes whichever
    // image this chain finished with, and the finalize pass is already declared.
    m_FinishedViewId = view.Image.Id;
    m_FinishedViewSpaceRG = view.Space;
    m_FinishedViewFor.Stamp(frame);

    // Composite the game UI (UIDocument entities) onto the finished view BEFORE
    // the pixel-perfect upscale samples it — so the UI is included whether or not
    // PP is active, and the upscale's point sampling carries HUD and world alike.
    //
    // The HUD composites onto the finished view: its declared space is that image's
    // PRODUCER stamp (#784), not the editor display's — EncodedSrgb once the finalize
    // has run, the pipeline's own space when it declined.
    if (!DeclareGameUICompositeRG(frame, view.Image,
                                  UI::UITargetSpace::ForPipelineOutput(
                                      view.Space, frame.Device()->GetActiveHdrOutputMode())))
        return;

    // Smooth pixel-perfect (8e-5): point-upscale the padded reference RT into
    // a full-panel-size target the UI samples. Every output pixel is a copy of
    // exactly one source texel, so the pass moves values without filtering them
    // in either arm — PassthroughLinear leaves the linear chain's OETF to the
    // terminal encode, and an encoded source passes through byte-for-byte. Pool
    // import + MarkOutput, never a transient: hybrid frames' old-graph UI
    // samples the physical.
    //
    // THE ONE EXEMPTION to "dither lands at final resolution, never below a
    // resample" (per-view-finalize-design.html, Invariants). The finalize above
    // runs BEFORE this upscale deliberately: a nearest-integer replication is
    // not a resample. It rounds nothing and mixes nothing, so every finalized
    // byte — dither included — survives verbatim; amplitude and the hash's
    // decorrelation are preserved at SOURCE-texel scale, and the resulting NxN
    // grain blocks ARE the pixel-art look this mode exists to produce, at the
    // scale the art is authored at. The red line is aimed at FILTERING
    // resamples, which low-pass the noise and then re-round the recovered
    // detail undithered — one of those after the finalize stays forbidden, on
    // this arm and every other.
    if (m_PixelPerfectStateRG.Active)
    {
        const std::string ppName =
            "Editor.W" + std::to_string(windowId) + ".GameView.PixelPerfectOutput";
        Rendering::TextureDesc ppDesc{};
        ppDesc.width = std::max(1u, m_PanelWRG);
        ppDesc.height = std::max(1u, m_PanelHRG);
        ppDesc.mipLevels = 1;
        ppDesc.arrayLayers = 1;
        ppDesc.sampleCount = 1;
        ppDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
        ppDesc.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
                       static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
        ppDesc.debugName = "Editor.GameView.PixelPerfectOutput";
        const RenderGraph::RGTexture pp = frame.ImportPersistentTexture(ppName.c_str(), ppDesc);
        if (pp.IsValid())
        {
            Rendering::Passes::PixelPerfectUpscaleParamsRG upscale{};
            upscale.Src = view.Image;
            upscale.Dst = pp;
            upscale.PassthroughLinear = true;
            upscale.ReferenceWidth = m_PixelPerfectStateRG.ReferenceWidth;
            upscale.ReferenceHeight = m_PixelPerfectStateRG.ReferenceHeight;
            upscale.SourceWidth = m_PixelPerfectStateRG.PaddedWidth;
            upscale.SourceHeight = m_PixelPerfectStateRG.PaddedHeight;
            upscale.Zoom = m_PixelPerfectStateRG.Zoom;
            upscale.FracX = m_PixelPerfectStateRG.FracX;
            upscale.FracY = m_PixelPerfectStateRG.FracY;
            if (Rendering::Passes::AddPixelPerfectUpscalePassRG(frame, upscale, viewInputSpace,
                                                                "GameView.PixelPerfectUpscale")
                    .IsValid())
            {
                frame.MarkOutput(pp, RenderGraph::RGImageLayout::ShaderReadOnly);
                m_PixelPerfectOutId = pp.Id;
                m_PixelPerfectFor.Stamp(frame);
                m_PixelPerfectPhysicalRG = frame.PhysicalTexture(pp);
                m_PixelPerfectW = ppDesc.width;
                m_PixelPerfectH = ppDesc.height;
                m_PixelPerfectSpaceRG = view.Space;
            }
        }
    }

    // Movie capture (8e-6): recorded PP movies capture the upscaled output
    // (old-arm parity). The capture helper owns the encode/readback declares
    // and backpressure.
    if (m_MovieCapture.IsCapturing())
    {
        // Through the accessor, not the raw id: an RG id only means anything in
        // the frame that produced it, and the accessor is the one place that
        // checks the frame identity as well as the id.
        RenderGraph::RGTexture ppTex{};
        Rendering::TextureHandle ppPhysical{};
        uint32_t ppW = 0;
        uint32_t ppH = 0;
        UI::UITextureSpace ppSpace = view.Space;
        const RenderGraph::RGTexture movieSrc =
            GetPixelPerfectOutputRG(frame, ppTex, ppPhysical, ppW, ppH, ppSpace) ? ppTex
                                                                                 : view.Image;
        // `viewInputSpace` covers both arms of that choice — the upscale carries
        // its source's values unchanged — and is what tells the capture which
        // pass owns the OETF: on the linear chain its own encode applies it, on
        // the encoded one the view's finalize already did. `debandLsb` and
        // `presentedFormat` are the frame's single resolutions, shared with the
        // finalize above so a recording keeps matching the screen.
        m_MovieCapture.DeclareCaptureRG(frame, movieSrc, m_PanelWRG, m_PanelHRG, viewInputSpace,
                                        debandLsb, presentedFormat);
    }
}

bool GameViewController::GetPixelPerfectOutputRG(const Rendering::RenderGraph::RGFrame& frame,
                                                 Rendering::RenderGraph::RGTexture& outTex,
                                                 Rendering::TextureHandle& outPhysical,
                                                 uint32_t& outW, uint32_t& outH,
                                                 UI::UITextureSpace& outSpace) const
{
    if (!m_PixelPerfectFor.IsFor(frame) ||
        m_PixelPerfectOutId == Rendering::RenderGraph::kInvalidId)
        return false;
    outTex = Rendering::RenderGraph::RGTexture{m_PixelPerfectOutId};
    outPhysical = m_PixelPerfectPhysicalRG;
    outW = m_PixelPerfectW;
    outH = m_PixelPerfectH;
    outSpace = m_PixelPerfectSpaceRG;
    return true;
}

bool GameViewController::GetFinishedViewRG(const Rendering::RenderGraph::RGFrame& frame,
                                           Rendering::RenderGraph::RGTexture& outTex,
                                           UI::UITextureSpace& outSpace) const
{
    if (!m_FinishedViewFor.IsFor(frame) ||
        m_FinishedViewId == Rendering::RenderGraph::kInvalidId)
        return false;
    outTex = Rendering::RenderGraph::RGTexture{m_FinishedViewId};
    outSpace = m_FinishedViewSpaceRG;
    return true;
}

} // namespace GameEngine
