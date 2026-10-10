#include "Platform/OverlayWindow.h"

#if defined(_WIN32)
#include <windows.h>
#include <memory>
#include <algorithm>

namespace GameEngine { namespace Platform {

namespace {
    inline uint8_t ClampU8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

    // Convert 0xAARRGGBB to premultiplied BGRA bytes
    inline void ARGBtoPremulBGRA(uint32_t argb, uint8_t& b, uint8_t& g, uint8_t& r, uint8_t& a) {
        a = static_cast<uint8_t>((argb >> 24) & 0xFF);
        uint8_t R = static_cast<uint8_t>((argb >> 16) & 0xFF);
        uint8_t G = static_cast<uint8_t>((argb >>  8) & 0xFF);
        uint8_t B = static_cast<uint8_t>((argb      ) & 0xFF);
        // premultiply
        r = ClampU8((int)R * a / 255);
        g = ClampU8((int)G * a / 255);
        b = ClampU8((int)B * a / 255);
    }

    ATOM EnsureClassRegistered() {
        static ATOM s_Atom = 0;
        if (s_Atom) return s_Atom;
        WNDCLASSW wc{};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = DefWindowProcW; // no input
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"GE_OverlayLayeredWindow";
        s_Atom = RegisterClassW(&wc);
        return s_Atom;
    }
}

class OverlayWindowWin32 final : public OverlayWindow {
public:
    static std::unique_ptr<OverlayWindowWin32> CreateImpl() {
        if (!EnsureClassRegistered()) return nullptr;
        auto ptr = std::unique_ptr<OverlayWindowWin32>(new OverlayWindowWin32());
        if (!ptr->Init()) return nullptr;
        return ptr;
    }

    ~OverlayWindowWin32() override {
        DestroyBitmap();
        if (m_Hwnd) DestroyWindow(m_Hwnd);
    }

    void Show() override {
        if (!m_Hwnd) return;
        ShowWindow(m_Hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(m_Hwnd, HWND_TOPMOST, m_X, m_Y, m_W, m_H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        m_Visible = true;
        Present();
    }

    void Hide() override {
        if (!m_Hwnd) return;
        ShowWindow(m_Hwnd, SW_HIDE);
        m_Visible = false;
    }

    void SetBounds(int x, int y, int w, int h) override {
        if (w <= 0 || h <= 0) { w = 1; h = 1; }
        m_X = x; m_Y = y; m_W = w; m_H = h;
        if (ResizeBitmapIfNeeded(w, h)) {
            // Redraw content after bitmap resize
            RedrawGhost();
        }
        if (m_Visible) Present();
    }

    void SetStyle(uint32_t fillARGB, uint32_t outlineARGB, int outlinePx) override {
        m_FillARGB = fillARGB; m_OutlineARGB = outlineARGB; m_OutlinePx = std::max(0, outlinePx);
        RedrawGhost();
        if (m_Visible) Present();
    }

private:
    bool Init() {
        DWORD ex = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
        m_Hwnd = CreateWindowExW(ex, L"GE_OverlayLayeredWindow", L"",
                                 WS_POPUP, m_X, m_Y, m_W, m_H, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!m_Hwnd) return false;
        m_MemDC = CreateCompatibleDC(nullptr);
        return m_MemDC != nullptr;
    }

    void DestroyBitmap() {
        if (m_OldBmp) { SelectObject(m_MemDC, m_OldBmp); m_OldBmp = nullptr; }
        if (m_Bitmap) { DeleteObject(m_Bitmap); m_Bitmap = nullptr; }
        if (m_MemDC) { DeleteDC(m_MemDC); m_MemDC = nullptr; }
    }

    bool ResizeBitmapIfNeeded(int w, int h) {
        if (w == m_BmpW && h == m_BmpH && m_Bitmap) return false;
        // Recreate 32-bit DIB section (top-down)
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32; // BGRA
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP newBmp = CreateDIBSection(m_MemDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!newBmp || !bits) return false;
        DestroyBitmap(); // free old DC/bitmap
        // Recreate DC and select bitmap
        m_MemDC = CreateCompatibleDC(nullptr);
        m_Bitmap = newBmp;
        m_OldBmp = (HBITMAP)SelectObject(m_MemDC, m_Bitmap);
        m_Bits = static_cast<uint8_t*>(bits);
        m_BmpW = w; m_BmpH = h;
        return true;
    }

    void RedrawGhost() {
        if (!m_Bitmap || !m_Bits) return;
        // Fill transparent
        memset(m_Bits, 0, (size_t)m_BmpW * (size_t)m_BmpH * 4);
        // Prepare colors
        uint8_t fb, fg, fr, fa; ARGBtoPremulBGRA(m_FillARGB, fb, fg, fr, fa);
        uint8_t ob, og, or_, oa; ARGBtoPremulBGRA(m_OutlineARGB, ob, og, or_, oa);
        const int t = m_OutlinePx;
        // Draw fill (inside border)
        int fx0 = std::max(0, t), fy0 = std::max(0, t);
        int fx1 = std::max(0, m_BmpW - t), fy1 = std::max(0, m_BmpH - t);
        for (int y = fy0; y < fy1; ++y) {
            uint8_t* row = m_Bits + (size_t)y * (size_t)m_BmpW * 4;
            for (int x = fx0; x < fx1; ++x) {
                uint8_t* px = row + (size_t)x * 4;
                px[0] = fb; px[1] = fg; px[2] = fr; px[3] = fa; // BGRA
            }
        }
        // Draw outline rectangles (top/bottom/left/right)
        if (t > 0) {
            // top
            for (int y = 0; y < std::min(t, m_BmpH); ++y) {
                uint8_t* row = m_Bits + (size_t)y * (size_t)m_BmpW * 4;
                for (int x = 0; x < m_BmpW; ++x) { uint8_t* p = row + (size_t)x*4; p[0]=ob;p[1]=og;p[2]=or_;p[3]=oa; }
            }
            // bottom
            for (int y = std::max(0, m_BmpH - t); y < m_BmpH; ++y) {
                uint8_t* row = m_Bits + (size_t)y * (size_t)m_BmpW * 4;
                for (int x = 0; x < m_BmpW; ++x) { uint8_t* p = row + (size_t)x*4; p[0]=ob;p[1]=og;p[2]=or_;p[3]=oa; }
            }
            // left/right
            for (int y = t; y < std::max(0, m_BmpH - t); ++y) {
                uint8_t* row = m_Bits + (size_t)y * (size_t)m_BmpW * 4;
                for (int x = 0; x < std::min(t, m_BmpW); ++x) { uint8_t* p = row + (size_t)x*4; p[0]=ob;p[1]=og;p[2]=or_;p[3]=oa; }
                for (int x = std::max(0, m_BmpW - t); x < m_BmpW; ++x) { uint8_t* p = row + (size_t)x*4; p[0]=ob;p[1]=og;p[2]=or_;p[3]=oa; }
            }
        }
    }

    void Present() {
        if (!m_Hwnd || !m_Bitmap || !m_MemDC) return;
        POINT ptPos{ m_X, m_Y };
        SIZE  size{ m_W, m_H };
        POINT ptSrc{ 0, 0 };
        BLENDFUNCTION bf{}; bf.BlendOp = AC_SRC_OVER; bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
        UpdateLayeredWindow(m_Hwnd, nullptr, &ptPos, &size, m_MemDC, &ptSrc, 0, &bf, ULW_ALPHA);
    }

private:
    HWND   m_Hwnd = nullptr;
    HDC    m_MemDC = nullptr;
    HBITMAP m_Bitmap = nullptr;
    HBITMAP m_OldBmp = nullptr;
    uint8_t* m_Bits = nullptr;
    int m_BmpW = 0, m_BmpH = 0;

    int m_X = 0, m_Y = 0, m_W = 1, m_H = 1;
    bool m_Visible = false;

    uint32_t m_FillARGB = 0x6633A0FF;     // default semi-transparent blue fill
    uint32_t m_OutlineARGB = 0xCC2080FF;  // default more opaque outline
    int m_OutlinePx = 2;
};

std::unique_ptr<OverlayWindow> OverlayWindow::Create() {
    auto impl = OverlayWindowWin32::CreateImpl();
    return std::unique_ptr<OverlayWindow>(impl.release());
}

}} // namespace GameEngine::Platform

#else // !_WIN32

namespace GameEngine { namespace Platform {

class OverlayWindowNull final : public OverlayWindow {
public:
    static std::unique_ptr<OverlayWindowNull> CreateImpl() { return std::unique_ptr<OverlayWindowNull>(new OverlayWindowNull()); }
    void Show() override {}
    void Hide() override {}
    void SetBounds(int, int, int, int) override {}
    void SetStyle(uint32_t, uint32_t, int) override {}
};

std::unique_ptr<OverlayWindow> OverlayWindow::Create() {
    return std::unique_ptr<OverlayWindow>(OverlayWindowNull::CreateImpl().release());
}

}} // namespace GameEngine::Platform

#endif

