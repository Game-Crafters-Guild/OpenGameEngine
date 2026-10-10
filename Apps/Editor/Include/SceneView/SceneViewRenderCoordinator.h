#pragma once

#include "EditorApplication.h"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace GameEngine
{
class SceneViewController;
class SceneViewPanel;
class UIElement;

namespace Rendering
{
namespace RenderGraph
{
class RGFrame;
}
}

namespace Engine::Renderer::Pipeline
{
struct ViewTargetsRG;
}

namespace Editor
{
class SceneThumbnailCapture;

class SceneViewRenderCoordinator final
{
public:
    using EditorWindowContext = EditorApplication::EditorWindowContext;

    SceneViewRenderCoordinator() = delete;

    static SceneViewPanel* SyncMountedPanelControllers(EditorWindowContext* ctx);
    static void ForEachController(EditorWindowContext* ctx,
                                  const std::function<void(SceneViewController*)>& fn);
    static void SyncSelectionAcrossControllers(EditorWindowContext* ctx, SceneViewController* source);
    static void SyncViewportStateAcrossControllers(EditorWindowContext* ctx, SceneViewController* source);

    // ── RenderGraph arm (slice 7d single-view; 8e-3 quad) ──
    // Phase 1 (pre-spine): sync controllers, resolve viewport render sizes,
    // suspend the OLD graph's scene passes, declare each renderable pane's
    // targets into the frame (one pane single-view; up to four in quad).
    // CONTRACT: ctx->scene's entry is pushed FIRST when it declares —
    // targets[0] is the main-scene entry (the driver's scene-save thumbnail
    // readback keys on it). Returns false when nothing declared (caller
    // skips the bind).
    static bool CollectRG(EditorWindowContext* ctx,
                           Rendering::RenderGraph::RGFrame& frame,
                           std::vector<Engine::Renderer::Pipeline::ViewTargetsRG>& outTargets);
    // Phase 1.5 (post-spine, pre-bind): declare the editor overlay passes
    // (gizmos / selection mask / outline / composite) onto the pipeline's
    // ACTUAL FinalColor, per pane with a span entry. Needs the same targets
    // span the spine consumed (each pane's imported Depth rides in it).
    static void DeclareOverlaysRG(EditorWindowContext* ctx,
                                   Rendering::RenderGraph::RGFrame& frame,
                                   std::span<const Engine::Renderer::Pipeline::ViewTargetsRG> targets);
    // Phase 2 (post-spine): re-point the UI at each declared pane's ACTUAL
    // pipeline output — every frame, never cached (FinalCopy elision changes
    // the physical with PP toggles). Names are per CONTROLLER ("scene_main"
    // for ctx->scene, kSceneQuadExternalNames for the quad panes) so slot
    // kind reassignment can't cross-wire textures. On HYBRID frames the bind
    // is the device-texture overload (old graph samples the executed
    // physical); on PURE frames it is register+publish — the RenderGraph UI pass
    // declares the read in the same graph.
    static void BindRG(EditorWindowContext* ctx, Rendering::RenderGraph::RGFrame& frame,
                        bool pureRGFrame);

    // Failable viewport-size resolve for RenderGraph arms: false when the element is
    // unmounted/degenerate (the caller declares nothing — never the old
    // resolver's backbuffer-size fallback). Shared with the 7f game arm.
    static bool TryResolveRenderableViewportSize(EditorWindowContext* ctx,
                                                 UIElement* viewport,
                                                 std::uint32_t& outW,
                                                 std::uint32_t& outH);
};

} // namespace Editor
} // namespace GameEngine
