#pragma once

#include "UI/UIElement.h"
#include "UI/ViewOverlay.h"

#include <cstdint>
#include <vector>

namespace GameEngine
{
class Label;
struct EditorContext;
} // namespace GameEngine

namespace GameEngine::Editor
{

/// "Loading scene... N / M" on the Scene View while an opened scene's entities resolve across
/// frames (EditorContext::SceneBuildProgress), hidden otherwise. The shader compile banner
/// (ShaderCompileProgressOverlay) stays hidden meanwhile, so the two never show at once.
class SceneLoadProgressOverlay final : public ViewOverlay
{
  public:
    explicit SceneLoadProgressOverlay(const EditorContext& context);

    void Present(ViewOverlayView view, UIElement& layer, const ViewOverlayCamera& camera) override;
    void Update() override;

  private:
    const EditorContext& m_Context;
    std::vector<UIElement::WeakRef<Label>> m_Banners;
    // The last progress shown, so an unchanged frame writes nothing.
    uint64_t m_ShownProcessed = 0;
    uint64_t m_ShownTotal = 0;
};

} // namespace GameEngine::Editor
