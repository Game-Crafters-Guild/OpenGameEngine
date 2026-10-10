#pragma once

#include "UI/UIElement.h"
#include "UI/ViewOverlay.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine
{
namespace EditorUI
{
class InspectorNotice;
} // namespace EditorUI
struct EditorContext;
namespace Engine::Renderer
{
struct PipelineStandIn;
} // namespace Engine::Renderer
} // namespace GameEngine

namespace GameEngine::Editor
{

/// The notices on the Scene View and the Game View while the project's render pipeline is not the
/// one drawing (FrameOrchestrator::ActivePipelineStandIn), both the shared notice control's opaque
/// look. A refusal is a warning along the top of the view, titled "Render pipeline refused": it
/// names the requested pipeline, says that the engine's default pipeline draws instead (or that
/// nothing draws, and why), and gives the reason with its fix. While the request waits for the
/// project's scripts to build, the rest of the requested pipeline draws (or the engine's default
/// pipeline, when the rest is refused too) and a neutral note titled "Waiting for scripts" names the
/// passes that join once the scripts are built and counts the seconds. Both sit in the overlay layer over the
/// drawn view; hidden while the requested pipeline draws. Their places in the layer are declared in
/// UI/controls/ViewOverlay/RenderPipelineStandInNotice.css.
class RenderPipelineStandInNotice final : public ViewOverlay
{
  public:
    explicit RenderPipelineStandInNotice(const EditorContext& context);

    void Present(ViewOverlayView view, UIElement& layer, const ViewOverlayCamera& camera) override;
    void Update() override;

    /// The notice's text for an active `standIn`: the requested pipeline, what draws instead, and
    /// the reason with its fix.
    static std::string Describe(const Engine::Renderer::PipelineStandIn& standIn);

    /// The waiting note's text for a `standIn` that waits for scripts, `seconds` into the wait: what
    /// draws meanwhile, and that the project's passes join once its scripts are built.
    static std::string DescribeWait(const Engine::Renderer::PipelineStandIn& standIn, int64_t seconds);

  private:
    void ShowRefusal(const std::string& text);
    void ShowWait(const std::string& text);

    const EditorContext& m_Context;
    std::vector<UIElement::WeakRef<EditorUI::InspectorNotice>> m_Notices;
    std::vector<UIElement::WeakRef<EditorUI::InspectorNotice>> m_WaitNotes;
    // What the notices show now: the render services, the stand-in generation and the seconds of the
    // wait they were drawn from, so an unchanged frame writes nothing.
    const void* m_ShownServices = nullptr;
    uint64_t m_ShownGeneration = 0;
    int64_t m_ShownWaitSeconds = -1;
    bool m_ShownOnce = false;
    // When the current wait began; reset when a stand-in episode starts that does not wait.
    std::chrono::steady_clock::time_point m_WaitStarted{};
};

} // namespace GameEngine::Editor
