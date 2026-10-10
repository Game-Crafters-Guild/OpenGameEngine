#include "Platform/ContextMenu.h"
#include "Platform/SystemTheme.h"
#include "Platform/Window.h"

#if defined(_WIN32)
#include <windows.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include "MenuIconBitmap_Win32.h"
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstdlib>
#include <cctype>

namespace GameEngine {

static int ParseHexChar(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static std::wstring ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), len);
    return result;
}

class Win32ContextMenu : public INativeContextMenu {
public:
    Win32ContextMenu() {
        Platform::ApplySystemNativeAppTheme();
        m_Menu = CreatePopupMenu();
    }

    ~Win32ContextMenu() override {
        if (m_Menu) {
            DestroyMenu(m_Menu);
            m_Menu = nullptr;
        }
        // DestroyMenu does not touch bitmaps assigned to hbmpItem; they stay
        // the application's to free.
        ReleaseBitmaps();
    }

    void Clear() override {
        if (!m_Menu) return;
        while (GetMenuItemCount(m_Menu) > 0) {
            RemoveMenu(m_Menu, 0, MF_BYPOSITION);
        }
        m_SubMenus.clear();
        m_CommandIds.clear();
        m_StaticFlags.clear();
        ReleaseBitmaps();
    }

    uint32_t AddSubMenu(uint32_t parentId, const std::string& title) override {
        HMENU parent = GetNativeMenu(parentId);
        if (!parent) return 0;
        HMENU sub = CreatePopupMenu();
        AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), ToWide(title).c_str());
        uint32_t id = m_NextSubMenuId++;
        m_SubMenus[id] = sub;
        return id;
    }

    void AddItem(uint32_t parentId, const std::string& title,
                 uint32_t commandId, uint32_t ItemFlags) override {
        HMENU parent = GetNativeMenu(parentId);
        if (!parent) return;
        UINT state = MF_STRING;
        if (ItemFlags & MenuItemFlag_Disabled) state |= MF_GRAYED | MF_DISABLED;
        AppendMenuW(parent, state, commandId, ToWide(title).c_str());
        m_CommandIds.insert(commandId);
        if (ItemFlags != MenuItemFlag_None) {
            m_StaticFlags[commandId] = ItemFlags;
        }
        if (ItemFlags & MenuItemFlag_Checked) {
            CheckMenuItem(m_Menu, commandId, MF_BYCOMMAND | MF_CHECKED);
        }
    }

    void AddSeparator(uint32_t parentId) override {
        HMENU parent = GetNativeMenu(parentId);
        if (!parent) return;
        AppendMenuW(parent, MF_SEPARATOR, 0, nullptr);
    }

    void SetCommandHandler(CommandCallback cb) override {
        m_CommandCallback = std::move(cb);
    }

    void SetStateProvider(StateProviderCallback cb) override {
        m_StateProvider = std::move(cb);
    }

    void SetItemEnabled(uint32_t commandId, bool enabled) override {
        uint32_t& flags = m_StaticFlags[commandId];
        if (enabled) flags &= ~MenuItemFlag_Disabled;
        else         flags |=  MenuItemFlag_Disabled;
    }

    void SetItemChecked(uint32_t commandId, bool checked) override {
        uint32_t& flags = m_StaticFlags[commandId];
        if (checked) flags |=  MenuItemFlag_Checked;
        else         flags &= ~MenuItemFlag_Checked;
    }

    void SetItemColor(uint32_t commandId, const std::string& hexColor) override {
        if (hexColor.empty() || hexColor[0] != '#') return;
        HBITMAP bmp = CreateColorDotBitmap(hexColor);
        if (!bmp) return;
        HBITMAP old = m_ItemBitmaps[commandId];
        if (old) DeleteObject(old);
        m_ItemBitmaps[commandId] = bmp;
        SetMenuItemBitmapRecursive(m_Menu, commandId, bmp);
    }

    void SetItemIcon(uint32_t commandId, const std::string& imagePath) override {
        if (imagePath.empty()) return;
        // Keyed on the URI, not the command: rows commonly share an image, and
        // a menu is rebuilt from scratch on every open.
        auto it = m_IconBitmaps.find(imagePath);
        if (it == m_IconBitmaps.end()) {
            const int size = Platform::MenuIconPixelSize();
            it = m_IconBitmaps.emplace(imagePath,
                                       Platform::CreateMenuIconBitmap(imagePath, size, size)).first;
        }
        if (!it->second) return;
        SetMenuItemBitmapRecursive(m_Menu, commandId, it->second);
    }

    void Show(Platform::Window* window, int x, int y) override {
        if (!window || !m_Menu) return;
        Platform::ApplySystemNativeAppTheme();
        GLFWwindow* glfwWin = window->GetGLFWHandle();
        if (!glfwWin) return;
        HWND hwnd = glfwGetWin32Window(glfwWin);
        if (!hwnd) return;

        ApplyItemStates();

        // Callers pass UI-logical coords (UIElement::GetLayoutX/Y). Win32 client
        // coords are physical pixels (GLFW 3.4+). Convert via UIContentScale /
        // nativeContentScale ratio (usually 1.0 unless the user applied an
        // additional HiDPI multiplier).
        float nsx = 1.0f, nsy = 1.0f;
        window->GetContentScale(nsx, nsy);
        const float nativeScale = std::max(0.01f, 0.5f * (nsx + nsy));
        const float uiScale = window->GetUiContentScale();
        const float ratio = (uiScale > 0.01f) ? (uiScale / nativeScale) : 1.0f;
        POINT pt{ static_cast<LONG>(x * ratio), static_cast<LONG>(y * ratio) };
        ClientToScreen(hwnd, &pt);

        UINT flags = TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD;
        UINT cmd = TrackPopupMenuEx(m_Menu, flags, pt.x, pt.y, hwnd, nullptr);
        if (cmd != 0 && m_CommandCallback) {
            m_CommandCallback(static_cast<uint32_t>(cmd));
        }
    }

private:
    void ReleaseBitmaps() {
        for (auto& p : m_ItemBitmaps) {
            if (p.second) DeleteObject(p.second);
        }
        m_ItemBitmaps.clear();
        for (auto& p : m_IconBitmaps) {
            if (p.second) DeleteObject(p.second);
        }
        m_IconBitmaps.clear();
    }

    HMENU GetNativeMenu(uint32_t id) const {
        if (id == 0) return m_Menu;
        auto it = m_SubMenus.find(id);
        return it == m_SubMenus.end() ? nullptr : it->second;
    }

    static HBITMAP CreateColorDotBitmap(const std::string& hexColor) {
        int r = 0, g = 0, b = 0;
        if (hexColor.size() == 7) {
            r = ParseHexChar(hexColor[1]) * 16 + ParseHexChar(hexColor[2]);
            g = ParseHexChar(hexColor[3]) * 16 + ParseHexChar(hexColor[4]);
            b = ParseHexChar(hexColor[5]) * 16 + ParseHexChar(hexColor[6]);
        } else if (hexColor.size() == 4) {
            r = ParseHexChar(hexColor[1]) * 17;
            g = ParseHexChar(hexColor[2]) * 17;
            b = ParseHexChar(hexColor[3]) * 17;
        } else {
            return nullptr;
        }
        const int size = 12;
        HDC hdc = GetDC(nullptr);
        HDC memdc = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, size, size);
        ReleaseDC(nullptr, hdc);
        if (!bmp || !memdc) { if (memdc) DeleteDC(memdc); if (bmp) DeleteObject(bmp); return nullptr; }
        HGDIOBJ old = SelectObject(memdc, bmp);
        HBRUSH brush = CreateSolidBrush(RGB(r, g, b));
        HBRUSH oldBrush = (HBRUSH)SelectObject(memdc, brush);
        Ellipse(memdc, 0, 0, size, size);
        SelectObject(memdc, oldBrush);
        DeleteObject(brush);
        SelectObject(memdc, old);
        DeleteDC(memdc);
        return bmp;
    }

    static bool SetMenuItemBitmapRecursive(HMENU menu, UINT commandId, HBITMAP bmp) {
        MENUITEMINFOW mii = {};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_ID | MIIM_SUBMENU;
        const int n = GetMenuItemCount(menu);
        for (int i = 0; i < n; ++i) {
            if (!GetMenuItemInfoW(menu, i, TRUE, &mii)) continue;
            if (mii.hSubMenu) {
                if (SetMenuItemBitmapRecursive(mii.hSubMenu, commandId, bmp)) return true;
            } else if (mii.wID == commandId) {
                mii.fMask = MIIM_BITMAP;
                mii.hbmpItem = bmp;
                SetMenuItemInfoW(menu, i, TRUE, &mii);
                return true;
            }
        }
        return false;
    }

    void ApplyItemStates() {
        for (uint32_t cmd : m_CommandIds) {
            uint32_t baseFlags = 0;
            auto it = m_StaticFlags.find(cmd);
            if (it != m_StaticFlags.end()) baseFlags = it->second;

            bool enabled = (baseFlags & MenuItemFlag_Disabled) == 0;
            bool checked = (baseFlags & MenuItemFlag_Checked) != 0;

            if (m_StateProvider) {
                MenuItemState state = m_StateProvider(cmd);
                enabled = enabled && state.Enabled;
                checked = state.Checked;
            }

            UINT enableFlags = MF_BYCOMMAND | (enabled ? MF_ENABLED : (MF_GRAYED | MF_DISABLED));
            EnableMenuItem(m_Menu, cmd, enableFlags);

            UINT checkFlags = MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED);
            CheckMenuItem(m_Menu, cmd, checkFlags);
        }
    }

    HMENU m_Menu = nullptr;
    std::unordered_map<uint32_t, HMENU> m_SubMenus;
    std::unordered_set<uint32_t> m_CommandIds;
    std::unordered_map<uint32_t, uint32_t> m_StaticFlags;
    std::unordered_map<uint32_t, HBITMAP> m_ItemBitmaps;
    std::unordered_map<std::string, HBITMAP> m_IconBitmaps;
    uint32_t m_NextSubMenuId = 1;
    CommandCallback m_CommandCallback;
    StateProviderCallback m_StateProvider;
};

std::unique_ptr<INativeContextMenu> CreateNativeContextMenu() {
    return std::make_unique<Win32ContextMenu>();
}

} // namespace GameEngine

#endif // _WIN32
