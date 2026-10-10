#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace GameEngine {
namespace Platform {

class Window;

// Cross-platform web view interface
// Provides a unified API for embedding web content (YouTube, etc.) in the editor
class IWebView {
public:
    virtual ~IWebView() = default;

    // Load a URL in the web view
    virtual bool LoadURL(const std::string& url) = 0;

    // Load HTML content directly (for better control over iframe embeds)
    virtual bool LoadHTML(const std::string& html, const std::string& baseURL = "https://www.youtube.com") = 0;

    // Get the native view handle (platform-specific)
    // macOS: returns NSView*
    // Windows: returns HWND
    // Linux: returns GtkWidget*
    virtual void* GetNativeView() const = 0;

    // Resize the web view to the given dimensions (in pixels)
    virtual void Resize(int width, int height) = 0;

    // Set the position of the web view relative to its parent (in pixels)
    virtual void SetPosition(int x, int y) = 0;

    // Show or hide the web view
    virtual void SetVisible(bool visible) = 0;

    // Check if the web view is visible
    virtual bool IsVisible() const = 0;

    // Set the background color (RGB values 0-255)
    virtual void SetBackgroundColor(unsigned char r, unsigned char g, unsigned char b) = 0;

    // Start a native OS file drag from the host window so embedded web content can
    // receive a real HTML5 file drop. Returns false when unsupported on the platform.
    virtual bool BeginFileDrag(const std::filesystem::path& path) = 0;

    // Invoked on mouse-down inside the web view. Native web-view clicks never reach
    // the engine input path, so the host UI uses this to clear its own focus state
    // (e.g. a focused URL field). Currently fired on macOS only.
    virtual void SetOnMouseDown(std::function<void()> callback) = 0;

    // Destroy the web view and clean up resources
    virtual void Destroy() = 0;
};

// Factory function to create a platform-specific web view
// parentWindow: The GLFW window that will host the web view
// Returns nullptr if web view creation fails or is not supported on this platform
std::unique_ptr<IWebView> CreateWebView(Window* parentWindow);

} // namespace Platform
} // namespace GameEngine
