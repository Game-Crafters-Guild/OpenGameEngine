#include "UI/Controls/DraggableModal.h"

#include "UI/UIEvents.h"

namespace GameEngine
{

void DraggableModal::BindDragHandle(UIElement* handle)
{
    if (!handle)
        return;
    handle->RegisterEventHandler(kEventMouseDown, [this, handle](UIEvent& e) { BeginDrag(e, handle); });
    handle->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e) { UpdateDrag(e); });
    handle->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) { EndDrag(e); });
    handle->RegisterEventHandler(kEventMouseCancel, [this](UIEvent& e) { EndDrag(e); });
}

void DraggableModal::CancelWindowDrag()
{
    m_Dragging = false;
}

void DraggableModal::SetWindowDragEnabled(bool enabled)
{
    m_DragEnabled = enabled;
    if (!enabled)
        m_Dragging = false;
}

void DraggableModal::BeginDrag(UIEvent& e, UIElement* handle)
{
    if (e.Button != 0 || !m_DragEnabled)
        return;
    m_Dragging = true;
    m_DragMouseStartX = e.X;
    m_DragMouseStartY = e.Y;
    GetWindowOrigin(m_DragWindowStartX, m_DragWindowStartY);
    e.Capture(handle);
    e.Stop();
}

void DraggableModal::UpdateDrag(UIEvent& e)
{
    if (!m_Dragging)
        return;
    PlaceWindow(m_DragWindowStartX + (e.X - m_DragMouseStartX), m_DragWindowStartY + (e.Y - m_DragMouseStartY));
    e.Stop();
}

void DraggableModal::EndDrag(UIEvent& e)
{
    if (!m_Dragging)
        return;
    m_Dragging = false;
    e.Stop();
}

} // namespace GameEngine
