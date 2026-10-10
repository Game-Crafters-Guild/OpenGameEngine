#if defined(_WIN32)

#include "Platform/WebView.h"
#include "Platform/Window.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <windows.h>
#include <shlwapi.h>
#include <string>

// WebView2 headers - these will be available if WebView2 SDK is installed
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996) // 'GetVersionExA': was declared deprecated
#endif

// Try to include WebView2 headers if available
#if __has_include(<WebView2.h>)
#include <WebView2.h>
#include <wrl/client.h>
using namespace Microsoft::WRL;
#else
// WebView2 not available - provide stub implementation
#endif

namespace GameEngine {
namespace Platform {

#if __has_include(<WebView2.h>)

class WebView_Win32 : public IWebView {
public:
    WebView_Win32(Window* parentWindow) : m_ParentWindow(parentWindow) {
        if (!parentWindow) {
            return;
        }

        GLFWwindow* glfwWindow = parentWindow->GetGLFWHandle();
        if (!glfwWindow) {
            return;
        }

        HWND hwnd = glfwGetWin32Window(glfwWindow);
        if (!hwnd) {
            return;
        }

        m_Hwnd = hwnd;
        // WebView2 initialization is asynchronous, so we'll need to handle that
        // For now, mark as not initialized until we can properly create the WebView2
        m_Initialized = false;
    }

    ~WebView_Win32() override {
        Destroy();
    }

    bool LoadURL(const std::string& url) override {
        if (!m_Initialized || !m_WebView) {
            return false;
        }

        std::wstring wurl(url.begin(), url.end());
        HRESULT hr = m_WebView->Navigate(wurl.c_str());
        return SUCCEEDED(hr);
    }

    bool LoadHTML(const std::string& html, const std::string& baseURL) override {
        if (!m_Initialized || !m_WebView) {
            return false;
        }

        std::wstring whtml(html.begin(), html.end());
        std::wstring wbaseURL(baseURL.begin(), baseURL.end());
        HRESULT hr = m_WebView->NavigateToString(whtml.c_str());
        return SUCCEEDED(hr);
    }

    void* GetNativeView() const override {
        return m_Controller ? m_Controller->get_HWND() : nullptr;
    }

    void Resize(int width, int height) override {
        if (!m_Initialized || !m_Controller) {
            return;
        }

        RECT bounds = { 0, 0, width, height };
        m_Controller->put_Bounds(bounds);
    }

    void SetPosition(int x, int y) override {
        if (!m_Initialized || !m_Controller) {
            return;
        }

        RECT bounds;
        m_Controller->get_Bounds(&bounds);
        bounds.left = x;
        bounds.top = y;
        m_Controller->put_Bounds(bounds);
    }

    void SetVisible(bool visible) override {
        if (!m_Controller) {
            return;
        }

        BOOL isVisible = visible ? TRUE : FALSE;
        m_Controller->put_IsVisible(isVisible);
    }

    bool IsVisible() const override {
        if (!m_Controller) {
            return false;
        }

        BOOL isVisible = FALSE;
        m_Controller->get_IsVisible(&isVisible);
        return isVisible == TRUE;
    }

    void SetBackgroundColor(unsigned char r, unsigned char g, unsigned char b) override {
        if (!m_Controller) {
            return;
        }
        // Set WebView2 background color to match dark theme
        // WebView2 uses COREWEBVIEW2_COLOR with values 0-255
        COREWEBVIEW2_COLOR color;
        color.A = 255;
        color.R = r;
        color.G = g;
        color.B = b;
        m_Controller->put_DefaultBackgroundColor(color);
    }

    bool BeginFileDrag(const std::filesystem::path& /*path*/) override {
        return false;
    }

    // Mouse-down callback not wired on this platform; see WebView.h.
    void SetOnMouseDown(std::function<void()> /*callback*/) override {}

    void Destroy() override {
        if (m_Controller) {
            m_Controller->Close();
            m_Controller = nullptr;
        }
        if (m_WebView) {
            m_WebView = nullptr;
        }
        m_Initialized = false;
    }

private:
    Window* m_ParentWindow = nullptr;
    HWND m_Hwnd = nullptr;
    ComPtr<ICoreWebView2Controller> m_Controller;
    ComPtr<ICoreWebView2> m_WebView;
    bool m_Initialized = false;
};

#else // WebView2 not available

class WebView_Win32 : public IWebView {
public:
    WebView_Win32(Window* /*parentWindow*/) {}
    ~WebView_Win32() override = default;
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

#endif // __has_include(<WebView2.h>)

std::unique_ptr<IWebView> CreateWebView(Window* parentWindow) {
    if (!parentWindow) {
        return nullptr;
    }

    auto webView = std::make_unique<WebView_Win32>(parentWindow);
    if (!webView->GetNativeView()) {
        return nullptr;
    }

    return webView;
}

} // namespace Platform
} // namespace GameEngine

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif // _WIN32
