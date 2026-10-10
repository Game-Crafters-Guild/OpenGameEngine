#if !defined(_WIN32) && !defined(__APPLE__)

#include "Platform/WebView.h"
#include "Platform/Window.h"

#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

// Try to include WebKitGTK headers if available
#if __has_include(<webkit2/webkit2.h>)
#include <webkit2/webkit2.h>
#include <gtk/gtk.h>
#else
// WebKitGTK not available - provide stub implementation
#endif

namespace GameEngine {
namespace Platform {

#if __has_include(<webkit2/webkit2.h>)

class WebView_Linux : public IWebView {
public:
    WebView_Linux(Window* parentWindow) : m_ParentWindow(parentWindow) {
        if (!parentWindow) {
            return;
        }

        GLFWwindow* glfwWindow = parentWindow->GetGLFWHandle();
        if (!glfwWindow) {
            return;
        }

        // Get X11 window
        Window x11Window = glfwGetX11Window(glfwWindow);
        if (!x11Window) {
            return;
        }

        // Create WebKitWebView
        m_WebView = WEBKIT_WEB_VIEW(webkit_web_view_new());
        if (!m_WebView) {
            return;
        }

        // Get GtkWidget from WebView
        GtkWidget* widget = GTK_WIDGET(m_WebView);
        gtk_widget_set_size_request(widget, 640, 480);
        gtk_widget_show(widget);

        // Note: Properly embedding this into the GLFW window requires
        // additional GTK integration. For now, this is a basic implementation.
        m_Initialized = true;
    }

    ~WebView_Linux() override {
        Destroy();
    }

    bool LoadURL(const std::string& url) override {
        if (!m_Initialized || !m_WebView) {
            return false;
        }

        webkit_web_view_load_uri(m_WebView, url.c_str());
        return true;
    }

    bool LoadHTML(const std::string& html, const std::string& baseURL) override {
        if (!m_Initialized || !m_WebView) {
            return false;
        }

        webkit_web_view_load_html(m_WebView, html.c_str(), baseURL.c_str());
        return true;
    }

    void* GetNativeView() const override {
        return m_WebView ? GTK_WIDGET(m_WebView) : nullptr;
    }

    void Resize(int width, int height) override {
        if (!m_Initialized || !m_WebView) {
            return;
        }

        GtkWidget* widget = GTK_WIDGET(m_WebView);
        gtk_widget_set_size_request(widget, width, height);
    }

    void SetPosition(int x, int y) override {
        if (!m_Initialized || !m_WebView) {
            return;
        }

        // Position is typically handled by the parent container
        // This may need additional GTK container integration
    }

    void SetVisible(bool visible) override {
        if (!m_Initialized || !m_WebView) {
            return;
        }

        GtkWidget* widget = GTK_WIDGET(m_WebView);
        if (visible) {
            gtk_widget_show(widget);
        } else {
            gtk_widget_hide(widget);
        }
    }

    bool IsVisible() const override {
        if (!m_Initialized || !m_WebView) {
            return false;
        }

        GtkWidget* widget = GTK_WIDGET(m_WebView);
        return gtk_widget_get_visible(widget);
    }

    void SetBackgroundColor(unsigned char r, unsigned char g, unsigned char b) override {
        if (!m_Initialized || !m_WebView) {
            return;
        }
        // Set WebKitGTK background color to match dark theme
        GdkRGBA color;
        color.red = static_cast<double>(r) / 255.0;
        color.green = static_cast<double>(g) / 255.0;
        color.blue = static_cast<double>(b) / 255.0;
        color.alpha = 1.0;
        webkit_web_view_set_background_color(m_WebView, &color);
    }

    bool BeginFileDrag(const std::filesystem::path& /*path*/) override {
        return false;
    }

    // Mouse-down callback not wired on this platform; see WebView.h.
    void SetOnMouseDown(std::function<void()> /*callback*/) override {}

    void Destroy() override {
        if (m_WebView) {
            GtkWidget* widget = GTK_WIDGET(m_WebView);
            gtk_widget_destroy(widget);
            m_WebView = nullptr;
            m_Initialized = false;
        }
    }

private:
    Window* m_ParentWindow = nullptr;
    WebKitWebView* m_WebView = nullptr;
    bool m_Initialized = false;
};

#else // WebKitGTK not available

class WebView_Linux : public IWebView {
public:
    WebView_Linux(Window* /*parentWindow*/) {}
    ~WebView_Linux() override = default;
    bool LoadURL(const std::string& /*url*/) override { return false; }
    bool LoadHTML(const std::string& /*html*/, const std::string& /*baseURL*/) override { return false; }
    void* GetNativeView() const override { return nullptr; }
    void Resize(int /*width*/, int /*height*/) override {}
    void SetPosition(int /*x*/, int /*y*/) override {}
    void SetVisible(bool /*visible*/) override {}
    bool IsVisible() const override { return false; }
    void SetBackgroundColor(unsigned char /*r*/, unsigned char /*g*/, unsigned char /*b*/) override {}
    bool BeginFileDrag(const std::filesystem::path& /*path*/) override { return false; }
    void SetOnMouseDown(std::function<void()> /*callback*/) override {}
    void Destroy() override {}
};

#endif // __has_include(<webkit2/webkit2.h>)

std::unique_ptr<IWebView> CreateWebView(Window* parentWindow) {
    if (!parentWindow) {
        return nullptr;
    }

    auto webView = std::make_unique<WebView_Linux>(parentWindow);
    if (!webView->GetNativeView()) {
        return nullptr;
    }

    return webView;
}

} // namespace Platform
} // namespace GameEngine

#endif // !_WIN32 && !__APPLE__
