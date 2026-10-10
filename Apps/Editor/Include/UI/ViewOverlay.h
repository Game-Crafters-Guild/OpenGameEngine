#pragma once

#include <cstdint>
#include <functional>

namespace GameEngine
{
class SceneViewController;
class UIElement;
} // namespace GameEngine

namespace GameEngine::Editor
{

/// The kind of view a viewport belongs to.
enum class ViewOverlayView : uint8_t
{
    Scene,
    Game,
};

/// The Scene View controller whose camera draws the viewport a layer sits on, read when asked: a
/// quad view pane can switch which view it shows. Empty for a viewport no Scene View camera draws
/// (the Game View); null while the viewport has no controller.
using ViewOverlayCamera = std::function<SceneViewController*()>;

/// Something drawn over the editor's views (a notice, a progress banner), registered with the
/// ViewOverlayHost. The host gives it the overlay layer of every viewport a view panel publishes;
/// the overlay builds its elements there and keeps them current from Update. The layer owns the
/// elements: an overlay holds them only through UIElement::WeakRef, since a panel's layout reload
/// destroys a viewport with everything on it, and a reloaded viewport is published again.
class ViewOverlay
{
  public:
    virtual ~ViewOverlay() = default;

    /// Builds this overlay's elements into `layer`, the overlay layer of a newly published
    /// viewport of `view`, which `camera` draws. An overlay that does not draw on that kind of
    /// view adds nothing. `camera` may be called while `layer` lives, never after.
    virtual void Present(ViewOverlayView view, UIElement& layer, const ViewOverlayCamera& camera) = 0;

    /// Once per editor frame on the main thread, outside UI event dispatch.
    virtual void Update() = 0;
};

} // namespace GameEngine::Editor
