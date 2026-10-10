#pragma once

#include "TerrainECS/HeightPageStoreLoader.h"
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

/// "Preparing terrain pages 42%: Terrain Near" on the Scene View while a heightmap's height page
/// store cooks (TerrainECS::HeightPageStoreLoader::CookStatuses), hidden otherwise. A large DEM
/// cooks for minutes; the terrain draws from its height texture meanwhile.
class TerrainPageCookOverlay final : public ViewOverlay
{
  public:
    void Present(ViewOverlayView view, UIElement& layer, const ViewOverlayCamera& camera) override;
    void Update() override;

  private:
    std::vector<UIElement::WeakRef<Label>> m_Banners;
    std::vector<TerrainECS::HeightPageStoreLoader::CookStatus> m_Cooks; // reused each frame
    std::string m_ShownText;
};

} // namespace GameEngine::Editor
