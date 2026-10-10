#pragma once

#include "Editor/Settings/ProjectUIScaleBinding.h"
#include "Engine/Rendering/CameraAspectRatio.h"
#include "Engine/Rendering/Camera.h"
#include "GameViewMovieCapture.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrameStamp.h"
#include "Rendering/Core/RenderGraph/RGTypes.h"
#include "Rendering/CameraTypes.h"
#include "SceneView/ViewPresentationSnapshot.h"
#include "UI/UITargetSpace.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace GameEngine
{
namespace ECS
{
class World;
}

namespace Rendering
{
class IDevice;
namespace RenderGraph
{
class RGFrame;
struct RGTexture;
}
} // namespace Rendering

namespace Engine::Renderer
{
class RenderServices;
namespace Pipeline
{
struct ViewTargetsRG;
}
}

class GameUIHost;

// Minimal Game View renderer for the Editor:
// - Renders the first ECS Camera in the world (with WorldTransform or Transform)
// - If none exists: clears to a neutral gray (no world/sky pipeline — avoids identity-matrix artifacts)
// - Produces an RenderGraph texture sampled by the UI as engine("game_main")
class GameViewController
{
  public:
    GameViewController(Engine::Renderer::RenderServices* renderServices,
                       std::filesystem::path editorAssetsDirectory);
    ~GameViewController();

    GameViewController(const GameViewController&) = delete;
    GameViewController& operator=(const GameViewController&) = delete;

    void DeactivateRenderView();
    void PrepareForActivation(ECS::World* world, uint32_t width, uint32_t height);

    // ── RenderGraph arm (slice 7f) ──
    // Post-spine declares (8e-5 PP upscale; 8e-6 movie encode/readback).
    // Consumes rs->GetPipelineOutputRG, therefore MUST run after the
    // driver's BuildFrameGraph and before BindGameViewRG.
    // `presentedFormat` is the owning window's swapchain format, resolved by
    // the caller from its own window-target handle
    // (IDevice::GetWindowTargetSwapchainFormat) once per frame — the view's
    // finalize and the movie encode both size to it, so a recording keeps
    // matching the screen by construction.
    void DeclarePostPipelinePassesRG(Rendering::RenderGraph::RGFrame& frame, uint64_t windowId,
                                     Rendering::TextureFormat presentedFormat);
    // The pixel-perfect upscale output for THIS frame incarnation —
    // (frame, frameIndex)-stamped like GetNoCameraColorRG. False when PP is
    // inactive or the stash is stale; the caller falls back to the
    // pipeline output.
    bool GetPixelPerfectOutputRG(const Rendering::RenderGraph::RGFrame& frame,
                                 Rendering::RenderGraph::RGTexture& outTex,
                                 Rendering::TextureHandle& outPhysical,
                                 uint32_t& outW, uint32_t& outH,
                                 UI::UITextureSpace& outSpace) const;
    // The image the post-pipeline chain finished this view with, HUD included:
    // the view's own finalize when it ran (sRGB code values, SdrFinalized), else
    // the pipeline output it started from (linear). Overwrites outTex/outSpace
    // and returns true only for THIS frame incarnation, same (frame, frameIndex)
    // stamp as GetPixelPerfectOutputRG; a stale stash leaves both untouched.
    //
    // No physical handle comes back: the finalize writes a TRANSIENT, which has
    // none until Execute realizes it. Callers needing one keep the pipeline's.
    bool GetFinishedViewRG(const Rendering::RenderGraph::RGFrame& frame,
                           Rendering::RenderGraph::RGTexture& outTex,
                           UI::UITextureSpace& outSpace) const;
    // Refresh view/camera registry state + import this view's pool targets
    // into the frame. Returns false when nothing was declared. The no-camera
    // case declares a neutral-grey clear pass and returns true with
    // outTargets.View == 0 (the view joins no span; the UI binds the stashed
    // physical). The WINDOW DRIVER makes the single spine call.
    bool DeclareTargetsRG(Rendering::RenderGraph::RGFrame& frame, uint32_t width, uint32_t height,
                           uint64_t windowId, ECS::World* world,
                           Engine::Renderer::Pipeline::ViewTargetsRG& outTargets);
    Rendering::TextureHandle GetNoCameraPhysicalRG() const { return m_NoCameraPhysicalRG; }
    uint32_t GetNoCameraWidthRG() const { return m_NoCameraW; }
    uint32_t GetNoCameraHeightRG() const { return m_NoCameraH; }
    // The no-camera clear's frame-local RGTexture, for the pure-frame UI
    // publish. Valid ONLY when (frame, frameIndex) match the declaring
    // incarnation — invalid otherwise.
    Rendering::RenderGraph::RGTexture GetNoCameraColorRG(const Rendering::RenderGraph::RGFrame& frame) const;

    bool HadCameraThisFrame() const { return m_HadCameraThisFrame; }
    bool IsWaitingForExtraction() const { return m_WaitingForExtractionRG; }
    // A device rebuild invalidates the frozen copy, so it also puts this view
    // back into warmup — hence the device read rather than a bare handle test.
    bool NeedsPresentationWarmup() const;
    bool UpdatePresentedSnapshotRG(Rendering::RenderGraph::RGFrame& frame,
                                   Rendering::RenderGraph::RGTexture source,
                                   UI::UITextureSpace sourceSpace);
    // Invalid once `device` has been rebuilt since the copy was written — the
    // handle would otherwise be a freed id, and ImportExternalTexture takes it
    // at face value.
    Rendering::TextureHandle GetLastPresentedTexture(const Rendering::IDevice& device) const
    {
        return m_PresentationSnapshot.Texture(device);
    }
    uint32_t GetLastPresentedWidth() const { return m_PresentationSnapshot.Width(); }
    uint32_t GetLastPresentedHeight() const { return m_PresentationSnapshot.Height(); }
    // The space the last-presented copy was rendered in (#767) — what the
    // waiting-for-extraction re-publish must carry, not the current mode.
    UI::UITextureSpace GetLastPresentedSpace() const { return m_PresentationSnapshot.Space(); }
    // The GPU capture half of movie recording; the Game View owns it because
    // capture rides this view's render output and readbacks.
    Editor::GameViewMovieCapture& GetMovieCapture() { return m_MovieCapture; }
    const Editor::GameViewMovieCapture& GetMovieCapture() const { return m_MovieCapture; }

    // The camera used by the latest successful target declaration. This is not
    // a GPU-presentation completion signal. It survives extraction waits and is
    // cleared when the view has no camera or is deactivated.
    struct DeclaredCamera
    {
        Rendering::CameraData Data{};
        Components::ExposureMode ExposureMode = Components::ExposureMode::Auto;
        uint64_t FrameIndex = 0;
        uint32_t Width = 0;
        uint32_t Height = 0;
    };
    const std::optional<DeclaredCamera>& GetDeclaredCamera() const { return m_DeclaredCamera; }

    Rendering::ViewId GetViewId() const { return m_ViewId; }

    // Input-phase viewport events, normalized to the displayed rect. Use the
    // last UI extent even when scene extraction/compositing skips this frame.
    void HandleGameUiPointer(bool over, float x, float y, bool down, int mods);

    // The HUD host this view composites, for the input chain's play-surface
    // stage. Null until the view has been rendered once (the host is created
    // lazily with the device).
    GameUIHost* GetGameUI() const { return m_GameUI.get(); }

  private:
    friend struct GameViewControllerTestAccess;
    std::optional<Engine::Renderer::Camera> PrepareView(ECS::World* world,
                                                        uint32_t width,
                                                        uint32_t height);
    void EnsureGameUI();
    void UpdateExposureReadback(bool enabled);

    Engine::Renderer::RenderServices* m_RenderServices = nullptr; // non-owning
    std::filesystem::path m_EditorAssetsDirectory;

    // Composite the game UI (UIDocument entities) onto `target` in `targetSpace` —
    // ensuring the host exists, re-asserting it as the active one, and feeding the
    // viewport pointer. Shared by the camera path (the finished view) and the
    // camera-less path (the clear-only grey), so a pure-UI scene renders in the Game
    // View just as it does in the Player. False when there is no target or no host.
    bool DeclareGameUICompositeRG(Rendering::RenderGraph::RGFrame& frame,
                                  Rendering::RenderGraph::RGTexture target,
                                  UI::UITargetSpace targetSpace);

    // Game UI host: composites UIDocument entities over the game FinalColor so the
    // Game View is WYSIWYG vs the Player. Warmed during editor setup so opening the
    // Game View does not pay the UI/font initialization cost on its first frame.
    std::unique_ptr<GameUIHost> m_GameUI;
    // The project's authored UI scale policy, re-applied every composite.
    Editor::ProjectUIScaleBinding m_GameUiScale;
    uint32_t m_GameUiWidth = 0;
    uint32_t m_GameUiHeight = 0;
    uint64_t m_GameUiWorldId = 0;
    uint64_t m_GameUiWorldGeneration = 0;

    Rendering::CameraId m_CameraId{0};
    Rendering::ViewId m_ViewId{0};

    std::optional<DeclaredCamera> m_DeclaredCamera;
    bool m_HadCameraThisFrame = false;
    bool m_WaitingForExtractionRG = false;
    Editor::ViewPresentationSnapshot m_PresentationSnapshot;
    uint32_t m_PresentationWarmupFramesRemaining = 4;

    // ── RenderGraph-arm state ──
    // No-camera frames: the clear pass's pooled physical, stashed at
    // declaration for the same-frame UI bind (within-frame stash only —
    // never cached across frames).
    Rendering::TextureHandle m_NoCameraPhysicalRG{};
    uint32_t m_NoCameraW = 0;
    uint32_t m_NoCameraH = 0;
    // The frame-local id behind m_NoCameraPhysicalRG, stamped with its
    // declaring incarnation so a stale read can never validate. Raw id, not the
    // typed handle — keeps this header off the heavy RGFrame include; the stamp
    // carries the (frame, frameIndex) rule itself and costs only a forward
    // declaration.
    Rendering::RenderGraph::RGResourceId m_NoCameraColorId = Rendering::RenderGraph::kInvalidId;
    Rendering::RenderGraph::RGFrameStamp m_NoCameraFor;
    // Pixel-perfect RenderGraph state (8e-5). The view state is re-read after
    // ApplyActiveCameraAspect each declare (the published frac, not the
    // estimate); the output stash follows the (frame, frameIndex) rule.
    Engine::Renderer::PixelPerfectViewState m_PixelPerfectStateRG{};
    uint32_t m_PanelWRG = 0;
    uint32_t m_PanelHRG = 0;
    Rendering::RenderGraph::RGResourceId m_PixelPerfectOutId = Rendering::RenderGraph::kInvalidId;
    Rendering::RenderGraph::RGFrameStamp m_PixelPerfectFor;
    Rendering::TextureHandle m_PixelPerfectPhysicalRG{};
    uint32_t m_PixelPerfectW = 0;
    uint32_t m_PixelPerfectH = 0;
    // Space of the upscale output, carried from the image it sampled — the
    // upscale changes no values in either arm (PassthroughLinear on the linear
    // one, byte passthrough on the encoded one). Follows the same
    // (frame, frameIndex) validity as the rest of the stash.
    UI::UITextureSpace m_PixelPerfectSpaceRG = UI::UITextureSpace::DisplayLinearSdr();

    // The finished view handed to the UI bind: the finalize's output when it
    // ran, else the pipeline output. Same (frame, frameIndex) stash discipline.
    Rendering::RenderGraph::RGResourceId m_FinishedViewId = Rendering::RenderGraph::kInvalidId;
    Rendering::RenderGraph::RGFrameStamp m_FinishedViewFor;
    UI::UITextureSpace m_FinishedViewSpaceRG = UI::UITextureSpace::DisplayLinearSdr();

    Editor::GameViewMovieCapture m_MovieCapture;
};

} // namespace GameEngine
