#include "UI/UICursorHelper.h"
#include "UI/UIStyle.h"

namespace GameEngine
{

void SetupUICursorCallback(UIManager* ui, Platform::Window* window)
{
    if (!ui || !window)
        return;

    ui->SetCursorCallback([window](CursorStyle cursor)
    {
        Platform::Window::CursorType winCursor = Platform::Window::CursorType::Arrow;
        switch (cursor)
        {
        case CursorStyle::Pointer:
            winCursor = Platform::Window::CursorType::Hand;
            break;
        case CursorStyle::Grab:
            winCursor = Platform::Window::CursorType::Grab;
            break;
        case CursorStyle::Grabbing:
            winCursor = Platform::Window::CursorType::Grabbing;
            break;
        case CursorStyle::Text:
            winCursor = Platform::Window::CursorType::IBeam;
            break;
        case CursorStyle::Crosshair:
            winCursor = Platform::Window::CursorType::Crosshair;
            break;
        case CursorStyle::ColResize:
            winCursor = Platform::Window::CursorType::HResize;
            break;
        case CursorStyle::RowResize:
            winCursor = Platform::Window::CursorType::VResize;
            break;
        case CursorStyle::NorthwestSoutheastResize:
            winCursor = Platform::Window::CursorType::NorthwestSoutheastResize;
            break;
        case CursorStyle::Move:
            winCursor = Platform::Window::CursorType::Move;
            break;
        case CursorStyle::NotAllowed:
            winCursor = Platform::Window::CursorType::NotAllowed;
            break;
        case CursorStyle::Auto:
        default:
            winCursor = Platform::Window::CursorType::Arrow;
            break;
        }
        window->SetCursor(winCursor);
    });
}

} // namespace GameEngine
