#pragma once

#include "UI/UIElement.h"

namespace GameEngine
{

/// Base for an editor modal or popup whose window is moved by dragging a
/// title bar. Owns the gesture — press on the handle, move, release or
/// cancel, capture on the handle — and nothing else: the window's origin
/// lives with the subclass, in its own space (margin offsets from a
/// flex-centred rest, or absolute px clamped to the host), read once when a
/// drag starts and placed on every move. Reading it at press rather than
/// accumulating per move is what keeps a long drag from creeping away from
/// the cursor, and what lets a clamped placement start the next drag from
/// where the window actually is.
class DraggableModal : public UIElement
{
  public:
    bool IsWindowDragging() const { return m_Dragging; }

  protected:
    DraggableModal() = default;

    /// Wire the drag gesture onto `handle`. The handle takes capture so move
    /// and release reach it, and the events are stopped once consumed.
    void BindDragHandle(UIElement* handle);

    /// End a drag in progress without moving the window. Hide paths call it.
    void CancelWindowDrag();

    /// A disabled handle ignores presses; a fullscreen window is not dragged.
    void SetWindowDragEnabled(bool enabled);

    /// The window's current origin, read when a drag starts.
    virtual void GetWindowOrigin(float& x, float& y) const = 0;
    /// Place the window at an origin in the same space, once per drag move.
    virtual void PlaceWindow(float x, float y) = 0;

  private:
    void BeginDrag(UIEvent& e, UIElement* handle);
    void UpdateDrag(UIEvent& e);
    void EndDrag(UIEvent& e);

    bool m_Dragging = false;
    bool m_DragEnabled = true;
    float m_DragMouseStartX = 0.0f;
    float m_DragMouseStartY = 0.0f;
    float m_DragWindowStartX = 0.0f;
    float m_DragWindowStartY = 0.0f;
};

} // namespace GameEngine
