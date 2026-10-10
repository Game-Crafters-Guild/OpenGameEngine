#include "Platform/Toolbar.h"
#include "Platform/Window.h"
#include "MenuIconBitmap_Win32.h"

#if defined(_WIN32)
#include <windows.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <unordered_map>
#include <vector>

namespace GameEngine
{

static std::wstring ToWide(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], len);
    return w;
}

class Win32Toolbar : public INativeToolbar
{
  public:
    bool Install(Platform::Window* window) override
    {
        if (!window)
            return false;
        GLFWwindow* gw = window->GetGLFWHandle();
        if (!gw)
            return false;
        m_Hwnd = glfwGetWin32Window(gw);
        if (!m_Hwnd)
            return false;

        // Create empty menu bar
        m_MenuBar = CreateMenu();
        SetMenu(m_Hwnd, m_MenuBar);
        DrawMenuBar(m_Hwnd);

        // Subclass window proc
        m_PrevProc = reinterpret_cast<WNDPROC>(GetWindowLongPtr(m_Hwnd, GWLP_WNDPROC));
        s_Instances[m_Hwnd] = this;
        SetWindowLongPtr(m_Hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Win32Toolbar::WndProcThunk));

        // Preserve client height (compensate for menu bar)
        RECT rcWin{}, rcClientBefore{}, rcClientAfter{};
        GetWindowRect(m_Hwnd, &rcWin);
        GetClientRect(m_Hwnd, &rcClientBefore);
        GetClientRect(m_Hwnd, &rcClientAfter);
        int beforeH = rcClientBefore.bottom - rcClientBefore.top;
        int afterH = rcClientAfter.bottom - rcClientAfter.top;
        if (afterH < beforeH)
        {
            int delta = beforeH - afterH;
            SetWindowPos(m_Hwnd, nullptr, rcWin.left, rcWin.top,
                         (rcWin.right - rcWin.left), (rcWin.bottom - rcWin.top) + delta,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        return true;
    }

    void Uninstall() override
    {
        if (!m_Hwnd)
            return;
        if (m_PrevProc)
        {
            SetWindowLongPtr(m_Hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_PrevProc));
        }
        if (m_MenuBar)
        {
            SetMenu(m_Hwnd, nullptr);
            DestroyMenu(m_MenuBar);
            m_MenuBar = nullptr;
        }
        DrawMenuBar(m_Hwnd);
        s_Instances.erase(m_Hwnd);
        m_Hwnd = nullptr;
        m_Menus.clear();
        m_Items.clear();
        ReleaseItemBitmaps();
        m_NextMenuId = 1;
        m_Callback = nullptr;
    }

    void Clear() override
    {
        if (!m_Hwnd || !m_MenuBar)
            return;

        // Clear menu contents in-place without ever detaching the menu bar
        // from the window. Toggling the menu bar on/off with SetMenu(hwnd,
        // nullptr) changes the client area height and causes framebuffer resize
        // callbacks, which in turn trigger swapchain recreation. By mutating
        // the existing HMENU instead, we keep the non-client metrics stable.

        // Destroy and remove all top-level menus and their submenus.
        int count = GetMenuItemCount(m_MenuBar);
        for (int i = count - 1; i >= 0; --i)
        {
            HMENU sub = GetSubMenu(m_MenuBar, i);
            // Remove the item from the bar first.
            RemoveMenu(m_MenuBar, static_cast<UINT>(i), MF_BYPOSITION);
            if (sub)
            {
                // Destroy the submenu hierarchy to avoid leaks.
                DestroyMenu(sub);
            }
        }

        DrawMenuBar(m_Hwnd);

        m_Menus.clear();
        m_Items.clear();
        ReleaseItemBitmaps();
        m_NextMenuId = 1;
    }

    void SetCommandHandler(CommandCallback cb) override { m_Callback = std::move(cb); }

    uint32_t AddMenu(const std::string& title) override
    {
        if (!m_MenuBar)
            return 0;
        HMENU hMenu = CreatePopupMenu();
        AppendMenuW(m_MenuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(hMenu), ToWide(title).c_str());
        DrawMenuBar(m_Hwnd);
        uint32_t id = m_NextMenuId++;
        m_Menus[id] = hMenu;
        return id;
    }

    uint32_t AddSubMenu(uint32_t parentMenuId, const std::string& title) override
    {
        HMENU parent = GetMenuById(parentMenuId);
        if (!parent)
            return 0;
        HMENU child = CreatePopupMenu();
        AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(child), ToWide(title).c_str());
        DrawMenuBar(m_Hwnd);
        uint32_t id = m_NextMenuId++;
        m_Menus[id] = child;
        return id;
    }

    void AddItem(uint32_t parentMenuId, const std::string& title, uint32_t commandId) override
    {
        HMENU parent = GetMenuById(parentMenuId);
        if (!parent)
            return;
        // Append as the last item and remember its position so we can
        // efficiently update the label in-place later without tearing down
        // the entire menu bar.
        int position = GetMenuItemCount(parent);
        AppendMenuW(parent, MF_STRING, commandId, ToWide(title).c_str());
        m_Items[commandId] = {parent, static_cast<UINT>(position)};
        DrawMenuBar(m_Hwnd);
    }

    void SetItemIcon(uint32_t commandId, const std::string& imagePath) override
    {
        auto item = m_Items.find(commandId);
        if (item == m_Items.end() || imagePath.empty())
            return;

        const int size = Platform::MenuIconPixelSize();
        HBITMAP bitmap = Platform::CreateMenuIconBitmap(imagePath, size, size);
        if (!bitmap)
            return;

        auto old = m_ItemBitmaps.find(commandId);
        if (old != m_ItemBitmaps.end() && old->second)
            DeleteObject(old->second);
        m_ItemBitmaps[commandId] = bitmap;

        MENUITEMINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_BITMAP;
        info.hbmpItem = bitmap;
        SetMenuItemInfoW(item->second.parent, item->second.position, TRUE, &info);
        DrawMenuBar(m_Hwnd);
    }

    void UpdateItemTitle(uint32_t commandId, const std::string& title) override
    {
        if (!m_Hwnd || !m_MenuBar)
            return;
        auto it = m_Items.find(commandId);
        if (it == m_Items.end())
            return;
        HMENU parent = it->second.parent;
        UINT position = it->second.position;
        if (!parent)
            return;

        std::wstring w = ToWide(title);
        if (w.empty())
            return;

        MENUITEMINFOW info{};
        info.cbSize = sizeof(MENUITEMINFOW);
        info.fMask = MIIM_STRING;
        info.dwTypeData = const_cast<LPWSTR>(w.c_str());
        info.cch = static_cast<UINT>(w.size());
        if (!SetMenuItemInfoW(parent, position, TRUE, &info))
        {
            // Fallback for older systems: modify the item by position.
            ModifyMenuW(parent, position, MF_BYPOSITION | MF_STRING, commandId, w.c_str());
        }

        DrawMenuBar(m_Hwnd);
    }

  private:
    struct ItemBinding
    {
        HMENU parent;
        UINT position;
    };

    HMENU GetMenuById(uint32_t id) const
    {
        auto it = m_Menus.find(id);
        return it == m_Menus.end() ? nullptr : it->second;
    }

    void ReleaseItemBitmaps()
    {
        for (const auto& [_, bitmap] : m_ItemBitmaps)
            if (bitmap)
                DeleteObject(bitmap);
        m_ItemBitmaps.clear();
    }

    static LRESULT CALLBACK WndProcThunk(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto it = s_Instances.find(hWnd);
        Win32Toolbar* self = (it != s_Instances.end()) ? it->second : nullptr;
        if (self)
            return self->WndProc(hWnd, msg, wParam, lParam);
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    LRESULT WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_COMMAND)
        {
            uint16_t cmd = LOWORD(wParam);
            if (m_Callback)
            {
                m_Callback(static_cast<uint32_t>(cmd));
                return 0;
            }
        }
        return m_PrevProc ? CallWindowProc(m_PrevProc, hWnd, msg, wParam, lParam) : DefWindowProc(hWnd, msg, wParam, lParam);
    }

    HWND m_Hwnd = nullptr;
    HMENU m_MenuBar = nullptr;
    WNDPROC m_PrevProc = nullptr;
    std::unordered_map<uint32_t, HMENU> m_Menus;
    std::unordered_map<uint32_t, ItemBinding> m_Items;
    std::unordered_map<uint32_t, HBITMAP> m_ItemBitmaps;
    uint32_t m_NextMenuId = 1;
    CommandCallback m_Callback;

    static inline std::unordered_map<HWND, Win32Toolbar*> s_Instances;
};

std::unique_ptr<INativeToolbar> CreateNativeToolbar()
{
    return std::make_unique<Win32Toolbar>();
}

} // namespace GameEngine

#endif // _WIN32
