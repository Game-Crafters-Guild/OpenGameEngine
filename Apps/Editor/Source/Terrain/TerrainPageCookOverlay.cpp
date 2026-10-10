#include "Terrain/TerrainPageCookOverlay.h"

#include "TerrainECS/TerrainService.h"
#include "UI/Controls/Label.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kHiddenClass = "hidden";

// The banner's text for `cooks`: the first cook's percentage and terrain, then how many more.
std::string CookText(const std::vector<TerrainECS::HeightPageStoreLoader::CookStatus>& cooks)
{
    if (cooks.empty())
        return {};
    const TerrainECS::HeightPageStoreLoader::CookStatus& first = cooks.front();
    std::string text = "Preparing terrain pages " +
                       std::to_string(static_cast<int>(std::floor(first.Fraction * 100.0f))) + "%: " +
                       (first.Terrain.empty() ? std::string("a terrain") : first.Terrain);
    if (cooks.size() > 1)
        text += " and " + std::to_string(cooks.size() - 1) + " more";
    return text;
}
} // namespace

void TerrainPageCookOverlay::Present(ViewOverlayView view, UIElement& layer, const ViewOverlayCamera& /*camera*/)
{
    if (view != ViewOverlayView::Scene)
        return;
    auto banner = std::make_unique<Label>();
    banner->AddClass("view-overlay-banner");
    banner->AddClass("terrain-page-cook");
    banner->AddClass(kHiddenClass);
    m_Banners.push_back(UIElement::MakeWeakRef(banner.get()));
    layer.AddChild(std::move(banner));
    m_ShownText.clear();
}

void TerrainPageCookOverlay::Update()
{
    m_Banners.erase(std::remove_if(m_Banners.begin(), m_Banners.end(),
                                   [](const UIElement::WeakRef<Label>& banner) { return banner.Get() == nullptr; }),
                    m_Banners.end());
    m_Cooks.clear();
    if (auto* terrains = TerrainECS::TerrainService::TryGet())
        terrains->GetHeightPageStores().CookStatuses(m_Cooks);
    const std::string text = CookText(m_Cooks);
    if (text == m_ShownText)
        return;
    m_ShownText = text;
    for (const UIElement::WeakRef<Label>& weak : m_Banners)
    {
        Label* banner = weak.Get();
        banner->SetText(text);
        if (text.empty())
            banner->AddClass(kHiddenClass);
        else
            banner->RemoveClass(kHiddenClass);
    }
}

} // namespace GameEngine::Editor
