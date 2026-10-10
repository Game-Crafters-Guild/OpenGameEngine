#include "UI/ViewOverlayHost.h"

#include <algorithm>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kLayerClass = "view-overlay-layer";
constexpr const char* kLayerStyleAssetPath = "UI/controls/ViewOverlay/ViewOverlayLayer.css";

UIElement* FindLayer(const UIElement& viewport)
{
    for (const auto& child : viewport.GetChildren())
    {
        if (child && child->HasClass(kLayerClass))
            return child.get();
    }
    return nullptr;
}
} // namespace

ViewOverlayHost& ViewOverlayHost::Get()
{
    static ViewOverlayHost s_Host;
    return s_Host;
}

void ViewOverlayHost::Register(std::unique_ptr<ViewOverlay> overlay)
{
    if (!overlay)
        return;
    ForgetDestroyedLayers();
    for (const PublishedLayer& published : m_Layers)
    {
        if (UIElement* layer = published.Layer.Get())
            overlay->Present(published.View, *layer, published.Camera);
    }
    m_Overlays.push_back(std::move(overlay));
}

UIElement* ViewOverlayHost::PublishViewport(ViewOverlayView view, UIElement& viewport, ViewOverlayCamera camera)
{
    if (UIElement* existing = FindLayer(viewport))
        return existing;
    ForgetDestroyedLayers();
    auto owned = std::make_unique<UIElement>();
    owned->AddClass(kLayerClass);
    owned->RequestSubtreeStyleAssetPath(kLayerStyleAssetPath, "editor");
    UIElement* layer = owned.get();
    viewport.AddChild(std::move(owned));
    m_Layers.push_back({view, UIElement::MakeWeakRef(layer), std::move(camera)});
    for (const std::unique_ptr<ViewOverlay>& overlay : m_Overlays)
        overlay->Present(view, *layer, m_Layers.back().Camera);
    return layer;
}

void ViewOverlayHost::Update()
{
    for (const std::unique_ptr<ViewOverlay>& overlay : m_Overlays)
        overlay->Update();
}

void ViewOverlayHost::Reset()
{
    m_Overlays.clear();
    m_Layers.clear();
}

void ViewOverlayHost::ForgetDestroyedLayers()
{
    m_Layers.erase(std::remove_if(m_Layers.begin(), m_Layers.end(),
                                  [](const PublishedLayer& published) { return published.Layer.Get() == nullptr; }),
                   m_Layers.end());
}

} // namespace GameEngine::Editor
