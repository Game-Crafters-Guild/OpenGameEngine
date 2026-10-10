#pragma once

#include <memory>

namespace GameEngine
{

/// Which implementation CreateContextMenu() wraps.
enum class ContextMenuBackend
{
    /// The editor-drawn UIContextMenu (the web editor's menu) — identical look
    /// and behavior on every platform.
    BuiltIn,
    /// The operating system's menu (NSMenu / Win32 popup). Falls back to
    /// BuiltIn on platforms with no native implementation.
    Native,
};

/// True when this platform has a real native menu implementation.
bool NativeContextMenuAvailable();

/// Default: BuiltIn on every platform (Native is opt-in via Settings).
ContextMenuBackend DefaultContextMenuBackend();

/// The backend new menus are created with. Settings apply changes through the
/// setter; a menu already built picks the change up the next time it is
/// Clear()ed for a rebuild (InterceptableContextMenu swaps its inner backend
/// there), which is every right-click for the editor's menus.
ContextMenuBackend GetContextMenuBackend();
void SetContextMenuBackend(ContextMenuBackend backend);

/// Bumped by SetContextMenuBackend when the backend changes; lets a menu
/// detect that it was built with a stale backend.
unsigned GetContextMenuBackendGeneration();

/// Whether a click on a selection row (a checkable choice in a group that
/// shows a check) keeps the menu open so several choices can be made in one
/// visit. Commands and dialog rows always close; outside click and Escape
/// close regardless. Settings > Interface owns the toggle; default on.
bool GetContextMenuKeepOpenOnToggle();
void SetContextMenuKeepOpenOnToggle(bool keepOpen);

class INativeContextMenu;

/// A fresh instance of the currently selected backend (falls back to the
/// built-in menu where no native one exists). Defined in UIContextMenu.cpp.
std::unique_ptr<INativeContextMenu> CreateSelectedContextMenuBackend();

} // namespace GameEngine
