#pragma once

#include "UI/UIElement.h"
#include "UI/ViewOverlay.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine
{
class Label;
struct EditorContext;
} // namespace GameEngine

namespace GameEngine::Editor
{

/// "Compiling shaders... N / M" on the Scene View while the material prewarm queue drains (a cold
/// shader cache is a long stretch of dark fallback materials with no other signal), counted for
/// this batch rather than for the session. Holds the final "M / M" briefly once the queue drains,
/// so a fast warm burst does not strobe, and stays hidden while an opened scene resolves: the
/// "Loading scene" banner (SceneLoadProgressOverlay) takes precedence. Logs the progress as
/// "[ShaderWarmup]" lines.
class ShaderCompileProgressOverlay final : public ViewOverlay
{
  public:
    explicit ShaderCompileProgressOverlay(const EditorContext& context);

    void Present(ViewOverlayView view, UIElement& layer, const ViewOverlayCamera& camera) override;
    void Update() override;

  private:
    void Show(const std::string& text);
    void Hide();
    void LogProgress(uint64_t done, uint64_t total);

    const EditorContext& m_Context;
    std::vector<UIElement::WeakRef<Label>> m_Banners;
    bool m_Shown = false;
    // The batch: opened on the idle-to-busy edge, with the completed count then as its baseline.
    bool m_BatchActive = false;
    uint64_t m_BatchBaseCompleted = 0;
    // The post-drain hold of the final count.
    bool m_Draining = false;
    std::chrono::steady_clock::time_point m_DrainedAt{};
    // The last progress logged, so the log gets one line per change.
    uint64_t m_LoggedDone = ~0ull;
    uint64_t m_LoggedTotal = ~0ull;
};

} // namespace GameEngine::Editor
