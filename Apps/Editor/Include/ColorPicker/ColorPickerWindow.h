#pragma once

#include "AssetCore/GUID.h"
#include "EditorApplication.h"
#include "Platform/Window.h"

#include <functional>
#include <memory>
#include <optional>

namespace GameEngine
{

class AssetManager;

// Standalone native window wrapper for the Color Picker tool.
// This keeps the EditorApplication free of ColorPicker-specific API surface.
class ColorPickerWindow final
{
  public:
    struct Callbacks
    {
        // Optional: called whenever the value changes (live preview while dragging).
        // argb is clamped 0-255; intensity is the HDR multiplier from the picker.
        std::function<void(uint32_t argb, float intensity)> onValueChanging;

        // Called when user commits.
        std::function<void(uint32_t argb, float intensity)> onApply;
        std::function<void()> onCancel;
    };

    struct Params
    {
        AssetManager* assetManager = nullptr;
        GUID styleGuid;
        Platform::Window* ownerWindow = nullptr; // optional (used for owned-tool windows)
        std::optional<UIManager::RenderRuntimeConfig> uiRenderConfig;
        // When true, place the picker near the current cursor position
        // (clamped to monitor work area) instead of centering it.
        bool openNearCursor = true;
        // True while a UI replay scenario is loaded. A replay drives editor
        // UIManagers directly with a deterministic stream, so this window hands
        // its manager to no router while it answers true and real OS input
        // reaches the dialog's UI through nothing.
        std::function<bool()> isUiReplayActive;
    };

    // Create a native OS window that contains only the ColorPicker UI (picker + Apply/Cancel),
    // with a live-preview callback and HDR intensity. Returns a fully built
    // EditorWindowContext that the caller must add to their window list.
    static std::unique_ptr<EditorApplication::EditorWindowContext> CreateWindowContext(
        uint32_t initialArgb,
        float initialIntensity,
        Callbacks callbacks,
        const Params& params);

    // Asks the picker window with this id to close; the editor's window teardown
    // then runs its onCancel. Does nothing once that window has been torn down.
    static void Close(uint64_t windowId);
};

} // namespace GameEngine
