// WebView on web: a DOM <iframe> laid over the canvas.
//
// Every other backend composites an OS webview above the GPU surface. A canvas
// has no such layer, but the page it lives in does: the iframe is a sibling of
// the canvas, positioned in page coordinates over the rectangle the panel
// occupies. That is the whole design — the engine keeps drawing the panel's
// chrome, and the browser draws the page inside the hole.
//
// Coordinate space: the panel hands these calls CLIENT pixels, and emscripten's
// GLFW reports the canvas's CSS size as the window size (the backing store,
// canvas.width, is the framebuffer). Client pixels are therefore CSS pixels
// already, and no devicePixelRatio math belongs here — the one conversion is
// the canvas's own page offset, read per-apply because layout moves it.
//
// What a browser cannot give back: no cross-origin page will report its clicks,
// so SetOnMouseDown never fires (its contract already says macOS only), and
// there is no OS drag source, so BeginFileDrag stays false.

#include "Platform/WebView.h"

#include <emscripten.h>

#include <cstdint>
#include <string>

namespace GameEngine
{
namespace Platform
{
namespace
{

// Elements live in a JS-side registry keyed by this id rather than being
// addressed by a DOM id string, so two panels cannot collide on a name and
// GetNativeView has a stable non-null value to hand back.
std::uint32_t NextWebViewId()
{
    static std::uint32_t s_Next = 0;
    return ++s_Next;
}

class WebViewWeb final : public IWebView
{
  public:
    explicit WebViewWeb(std::uint32_t id) : m_Id(id)
    {
        EM_ASM({
            var id = $0;
            Module.__geWebViews = Module.__geWebViews || {};
            var frame = document.createElement('iframe');
            frame.style.position = 'absolute';
            frame.style.border = '0';
            frame.style.margin = '0';
            frame.style.padding = '0';
            // Above the canvas, below anything the shell puts on top of both.
            frame.style.zIndex = '10';
            frame.style.display = 'none';
            // The editor page is cross-origin isolated (COOP+COEP) because the
            // threaded build needs SharedArrayBuffer. Under COEP a cross-origin
            // iframe must either send its own COEP header — no ordinary website
            // does — or be marked credentialless, or the browser refuses the
            // frame outright and shows "refused to connect". The parent's own
            // COEP: credentialless does NOT cover nested documents; only this
            // attribute does. It costs the frame its credentials, so embedded
            // pages load logged out.
            frame.setAttribute('credentialless', '');
            frame.setAttribute('allow', 'autoplay; encrypted-media; picture-in-picture');
            document.body.appendChild(frame);
            Module.__geWebViews[id] = frame;
        }, m_Id);
    }

    ~WebViewWeb() override { Destroy(); }

    bool LoadURL(const std::string& url) override
    {
        if (m_Destroyed)
            return false;
        EM_ASM({
            var frame = (Module.__geWebViews || {})[$0];
            if (!frame)
                return;
            // srcdoc outranks src whenever both are present, so a frame that
            // ever showed inline HTML would keep showing it and every later
            // navigation would silently do nothing. Callers do exactly that:
            // WebPanel seeds a dark placeholder through LoadHTML, then calls
            // LoadURL.
            frame.removeAttribute('srcdoc');
            frame.src = UTF8ToString($1);
        }, m_Id, url.c_str());
        return true;
    }

    bool LoadHTML(const std::string& html, const std::string& /*baseURL*/) override
    {
        if (m_Destroyed)
            return false;
        // srcdoc rather than a blob URL: no object URL to leak, and the frame
        // is replaced atomically. The baseURL a caller passes cannot be honoured
        // — srcdoc content resolves relative URLs against this page — which only
        // affects relative links, and callers embed absolute ones.
        EM_ASM({
            var frame = (Module.__geWebViews || {})[$0];
            if (!frame)
                return;
            // Dropping src keeps the pair unambiguous in the other direction:
            // a frame told to show inline HTML must not race a still-loading
            // navigation from an earlier LoadURL.
            frame.removeAttribute('src');
            frame.srcdoc = UTF8ToString($1);
        }, m_Id, html.c_str());
        return true;
    }

    void* GetNativeView() const override
    {
        // No native view pointer exists, but callers use this as a liveness
        // check, so it answers with something stable and non-null while the
        // element is alive.
        return m_Destroyed ? nullptr
                           : reinterpret_cast<void*>(static_cast<std::uintptr_t>(m_Id));
    }

    void Resize(int width, int height) override
    {
        m_Width = width;
        m_Height = height;
        Apply();
    }

    void SetPosition(int x, int y) override
    {
        m_X = x;
        m_Y = y;
        Apply();
    }

    void SetVisible(bool visible) override
    {
        m_Visible = visible;
        Apply();
    }

    bool IsVisible() const override { return m_Visible; }

    void SetBackgroundColor(unsigned char r, unsigned char g, unsigned char b) override
    {
        if (m_Destroyed)
            return;
        EM_ASM({
            var frame = (Module.__geWebViews || {})[$0];
            if (frame)
                frame.style.backgroundColor = 'rgb(' + $1 + ',' + $2 + ',' + $3 + ')';
        }, m_Id, (int)r, (int)g, (int)b);
    }

    bool BeginFileDrag(const std::filesystem::path& /*path*/) override { return false; }

    void SetOnMouseDown(std::function<void()> /*callback*/) override {}

    void Destroy() override
    {
        if (m_Destroyed)
            return;
        m_Destroyed = true;
        EM_ASM({
            var views = Module.__geWebViews || {};
            var frame = views[$0];
            if (frame && frame.parentNode)
                frame.parentNode.removeChild(frame);
            delete views[$0];
        }, m_Id);
    }

  private:
    void Apply() const
    {
        if (m_Destroyed)
            return;
        EM_ASM({
            var frame = (Module.__geWebViews || {})[$0];
            if (!frame)
                return;
            // The canvas's page offset is read on every apply: the shell is free
            // to lay the canvas out however it likes, and a cached offset would
            // leave the frame behind the first time the page reflows.
            var canvas = Module['canvas'] || document.querySelector('canvas');
            // One declaration per statement: EM_ASM's body is a macro argument,
            // so a comma outside parentheses splits it and the C preprocessor
            // hands the compiler half a statement.
            var originX = 0;
            var originY = 0;
            if (canvas)
            {
                var rect = canvas.getBoundingClientRect();
                originX = rect.left + window.scrollX;
                originY = rect.top + window.scrollY;
            }
            frame.style.left = (originX + $1) + 'px';
            frame.style.top = (originY + $2) + 'px';
            frame.style.width = $3 + 'px';
            frame.style.height = $4 + 'px';
            frame.style.display = $5 ? 'block' : 'none';
        }, m_Id, m_X, m_Y, m_Width, m_Height, m_Visible ? 1 : 0);
    }

    std::uint32_t m_Id = 0;
    int m_X = 0;
    int m_Y = 0;
    int m_Width = 0;
    int m_Height = 0;
    bool m_Visible = false;
    bool m_Destroyed = false;
};

} // namespace

std::unique_ptr<IWebView> CreateWebView(Window* /*parentWindow*/)
{
    return std::make_unique<WebViewWeb>(NextWebViewId());
}

} // namespace Platform
} // namespace GameEngine
