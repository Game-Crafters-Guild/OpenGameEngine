#pragma once

#include "UI/UIElement.h"
#include "UI/ViewOverlay.h"

#include <string>
#include <vector>

namespace GameEngine
{
class Label;
} // namespace GameEngine

namespace GameEngine::Editor
{

/// Drives the asset loads the editor started for the user (RunWhenAssetLoaded) and
/// says on the Scene View what is still loading: "Loading terrain.glb..." while a
/// dropped model imports, hidden when nothing is pending. Polls the loads once per
/// frame, so their continuations run on the main thread; drops the continuations
/// that never ran when the editor tears the overlays down.
class PendingAssetLoadsOverlay final : public ViewOverlay
{
  public:
    ~PendingAssetLoadsOverlay() override;

    void Present(ViewOverlayView view, UIElement& layer, const ViewOverlayCamera& camera) override;
    void Update() override;

  private:
    std::vector<UIElement::WeakRef<Label>> m_Banners;
    std::string m_ShownText;
};

} // namespace GameEngine::Editor
