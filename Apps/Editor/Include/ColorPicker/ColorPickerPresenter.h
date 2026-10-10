#pragma once

#include "EditorApplication.h" // EditorApplication::EditorWindowContext
#include "InspectorRegistry.h" // ColorPickerCallbacks, OpenColorPickerWindowFn
#include "UI/UIElement.h"      // UIElement::WeakRef

#include <cstdint>
#include <functional>
#include <memory>

namespace GameEngine
{
class ColorPickerPopup;

namespace Editor
{

/// Presents the colour picker a panel swatch asks for. Where the platform has
/// more than one window it is a native tool window beside the main one; on a
/// single-surface host it is the in-engine ColorPickerPopup over the main
/// window's UI root. Panels never learn which: they hold the callback from
/// AsOpenCallback() and the decision is made here on each open. A picker opened
/// with ColorPickerCallbacks::scope closes when that scope closes.
class ColorPickerPresenter
{
  public:
    using EditorWindowContext = EditorApplication::EditorWindowContext;

    struct Dependencies
    {
        // The window that owns a native picker and hosts the in-engine one.
        // Resolved per open: the window list is rebuilt during Initialize.
        std::function<EditorWindowContext*()> GetMainWindow;
        // EditorApplication::QueueNativeToolWindow — attaches a built tool
        // window on the next pre-UI safe point.
        std::function<void(std::unique_ptr<EditorWindowContext>)> QueueNativeToolWindow;
        // True while a UI replay scenario is loaded. A native picker joins the
        // rule every editor window follows: it hands its UIManager to no router
        // while a replay owns the input stream.
        std::function<bool()> IsUiReplayActive;
    };

    void Initialize(Dependencies deps);

    void Open(uint32_t initialArgb, float initialIntensity, ColorPickerCallbacks callbacks);

    /// The callback a panel takes through SetOpenColorPickerWindow, bound to
    /// this presenter. Valid for the presenter's lifetime.
    OpenColorPickerWindowFn AsOpenCallback();

  private:
    // One open of the in-engine modal. Only the latest open has a live session.
    struct ModalSession
    {
        bool closed = false;
    };

    // Returns the picker window's id, or 0 when no window was built.
    uint64_t OpenNativeWindow(EditorWindowContext& mainWindow, uint32_t initialArgb, float initialIntensity,
                              ColorPickerCallbacks callbacks);
    std::weak_ptr<ModalSession> OpenModal(EditorWindowContext& mainWindow, uint32_t initialArgb,
                                          float initialIntensity, ColorPickerCallbacks callbacks);
    void CloseModal(ModalSession& session);
    ColorPickerPopup* EnsureModal(EditorWindowContext& mainWindow);

    Dependencies m_Deps;
    // Built on first use and owned by the main window's UI root.
    UIElement::WeakRef<ColorPickerPopup> m_Modal;
    std::shared_ptr<ModalSession> m_ModalSession;
};

} // namespace Editor
} // namespace GameEngine
