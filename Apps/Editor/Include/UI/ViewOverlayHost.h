#pragma once

#include "UI/UIElement.h"
#include "UI/ViewOverlay.h"

#include <memory>
#include <vector>

namespace GameEngine::Editor
{

/// Where the editor's view overlays meet the views. A view panel publishes each viewport it
/// builds; the host adds an overlay layer to it (the `view-overlay-layer` element, styled by
/// UI/controls/ViewOverlay/ViewOverlayLayer.css: a column centered along the viewport's top edge
/// that takes no pointer input) and presents every registered overlay on that layer. An overlay
/// registered after a viewport was published is presented on it at registration.
///
/// The panels know the host, never the overlays; the overlays are registered once at startup
/// (RegisterBuiltInViewOverlays). Main thread only.
class ViewOverlayHost
{
  public:
    static ViewOverlayHost& Get();

    /// Takes `overlay` and presents it on every published viewport.
    void Register(std::unique_ptr<ViewOverlay> overlay);

    /// Adds the overlay layer to `viewport`, which `camera` draws, and presents every overlay on
    /// it. A viewport that already carries its layer is left as it is, so a panel may publish on
    /// every rebind. Returns the layer.
    UIElement* PublishViewport(ViewOverlayView view, UIElement& viewport, ViewOverlayCamera camera);

    /// Updates every overlay, once per editor frame.
    void Update();

    /// Destroys every overlay and forgets every viewport. The editor calls it before the state
    /// the overlays read goes away; tests call it between cases.
    void Reset();

  private:
    struct PublishedLayer
    {
        ViewOverlayView View = ViewOverlayView::Scene;
        UIElement::WeakRef<UIElement> Layer;
        ViewOverlayCamera Camera;
    };

    void ForgetDestroyedLayers();

    std::vector<std::unique_ptr<ViewOverlay>> m_Overlays;
    std::vector<PublishedLayer> m_Layers;
};

} // namespace GameEngine::Editor
