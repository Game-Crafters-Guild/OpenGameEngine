#include "Editor/Assets/PendingAssetLoadsOverlay.h"

#include "Editor/Assets/AsyncAssetHelpers.h"
#include "UI/Controls/Label.h"

#include <algorithm>
#include <memory>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kHiddenClass = "hidden";

std::string PendingText(const std::vector<std::string>& labels)
{
    if (labels.empty())
        return {};
    if (labels.size() == 1)
        return "Loading " + labels.front() + "...";
    return "Loading " + labels.front() + " and " + std::to_string(labels.size() - 1) + " more...";
}
} // namespace

PendingAssetLoadsOverlay::~PendingAssetLoadsOverlay()
{
    ClearPendingAssetLoads();
}

void PendingAssetLoadsOverlay::Present(ViewOverlayView view, UIElement& layer, const ViewOverlayCamera& /*camera*/)
{
    if (view != ViewOverlayView::Scene)
        return;
    auto banner = std::make_unique<Label>();
    banner->AddClass("view-overlay-banner");
    banner->AddClass("pending-asset-loads");
    banner->AddClass(kHiddenClass);
    m_Banners.push_back(UIElement::MakeWeakRef(banner.get()));
    layer.AddChild(std::move(banner));
    m_ShownText.clear();
}

void PendingAssetLoadsOverlay::Update()
{
    PollPendingAssetLoads();

    m_Banners.erase(std::remove_if(m_Banners.begin(), m_Banners.end(),
                                   [](const UIElement::WeakRef<Label>& banner) { return banner.Get() == nullptr; }),
                    m_Banners.end());
    const std::string text = PendingText(PendingAssetLoadLabels());
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
