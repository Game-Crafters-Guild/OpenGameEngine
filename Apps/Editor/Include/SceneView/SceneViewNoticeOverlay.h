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
} // namespace GameEngine

namespace GameEngine::Editor
{

/// Shows `text` along the top of the Scene View for a few seconds, replacing a notice shown now:
/// why an action the user just took did nothing, and what to do instead. Main thread only.
void ShowSceneViewNotice(std::string text);

/// Draws the notice ShowSceneViewNotice raised, a banner in the Scene View's overlay layer,
/// hidden once its time is up.
class SceneViewNoticeOverlay final : public ViewOverlay
{
  public:
    void Present(ViewOverlayView view, UIElement& layer, const ViewOverlayCamera& camera) override;
    void Update() override;

  private:
    std::vector<UIElement::WeakRef<Label>> m_Banners;
    uint64_t m_ShownSerial = 0;
    bool m_Visible = false;
};

} // namespace GameEngine::Editor
