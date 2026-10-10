#include "Scene/SceneLoadProgressOverlay.h"

#include "EditorContext.h"
#include "UI/Controls/Label.h"

#include <algorithm>
#include <memory>
#include <string>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kHiddenClass = "hidden";
} // namespace

SceneLoadProgressOverlay::SceneLoadProgressOverlay(const EditorContext& context)
    : m_Context(context)
{
}

void SceneLoadProgressOverlay::Present(ViewOverlayView view, UIElement& layer, const ViewOverlayCamera& /*camera*/)
{
    if (view != ViewOverlayView::Scene)
        return;
    auto banner = std::make_unique<Label>();
    banner->AddClass("view-overlay-banner");
    banner->AddClass("scene-load-progress");
    banner->AddClass(kHiddenClass);
    m_Banners.push_back(UIElement::MakeWeakRef(banner.get()));
    layer.AddChild(std::move(banner));
    // A banner presented mid-load shows the progress from the next update.
    m_ShownProcessed = 0;
    m_ShownTotal = 0;
}

void SceneLoadProgressOverlay::Update()
{
    m_Banners.erase(std::remove_if(m_Banners.begin(), m_Banners.end(),
                                   [](const UIElement::WeakRef<Label>& banner) { return banner.Get() == nullptr; }),
                    m_Banners.end());
    uint64_t processed = 0;
    uint64_t total = 0;
    const bool loading = m_Context.SceneBuildProgress && m_Context.SceneBuildProgress(processed, total) && total > 0;
    if (!loading)
    {
        processed = 0;
        total = 0;
    }
    if (processed == m_ShownProcessed && total == m_ShownTotal)
        return;
    m_ShownProcessed = processed;
    m_ShownTotal = total;

    const std::string text =
        loading ? "Loading scene... " + std::to_string(processed) + " / " + std::to_string(total) : std::string();
    for (const UIElement::WeakRef<Label>& weak : m_Banners)
    {
        Label* banner = weak.Get();
        banner->SetText(text);
        if (loading)
            banner->RemoveClass(kHiddenClass);
        else
            banner->AddClass(kHiddenClass);
    }
}

} // namespace GameEngine::Editor
