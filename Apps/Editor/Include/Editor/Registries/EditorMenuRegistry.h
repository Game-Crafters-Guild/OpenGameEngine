#pragma once

#include "ECS/ModuleRegistration.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Editor
{

// What a package module registers to add a native toolbar menu item — the
// native sibling of the script-driven ScriptMenuRegistry items. Path is a
// '/'-separated menu path with at least "Menu/Item" (e.g. "Tools/Import
// Unity Package..."); intermediate segments become submenus, seeded with the
// editor's built-in menus so packages extend rather than duplicate them.
struct EditorMenuItemDescriptor
{
    std::string Path;
    int32_t Priority = 0; // lower sorts first among native items; ties by Path
    std::function<void()> Action;
};

// Snapshot row the toolbar build consumes: the registry owns command-id
// assignment (a range disjoint from built-in and script commands) and the
// toolbar dispatches ids back through TryInvoke.
struct EditorMenuItemSnapshot
{
    uint32_t CommandId = 0;
    std::string Path;
    int32_t Priority = 0;
};

// What a module registers to add an item to the Assets panel's folder context
// menu. Path is the item's place in that menu ("Generate Thumbnails", or
// "Submenu/Item"); Action receives the folder the menu was opened on. Icon is
// an editor icon path (EditorIcons), empty for none.
struct EditorDirectoryMenuItemDescriptor
{
    std::string Path;
    int32_t Priority = 0; // the folder menu sorts by (priority, path); built-in rows sit at 0
    std::string Icon;
    std::function<void(const std::filesystem::path& folder)> Action;
};

// Snapshot row the folder context menu consumes; the registry assigns the ids.
struct EditorDirectoryMenuItemSnapshot
{
    uint32_t CommandId = 0;
    std::string Path;
    int32_t Priority = 0;
    std::string Icon;
};

// A command reached by a fixed id instead of a menu path: UI replay and the
// debug server's execute_command drive it (the ids live in
// Automation/UiReplayCommandIds.h). The owner of the feature registers it, so
// the dispatch site never names the feature. Invoke returns false and fills
// outError (when non-null) on failure.
struct EditorCommandDescriptor
{
    uint32_t CommandId = 0;
    std::function<bool(std::string* outError)> Invoke;
};

enum class EditorCommandResult
{
    NotFound,
    Succeeded,
    Failed,
};

// Registration is main-thread (module loads + editor startup), matching the
// other editor registries. Module lifetime is loud no-unload: a module
// rebuild re-registers under the same Path and the NEW descriptor wins
// (replace-forward, command id kept stable); nothing is ever unregistered.
class EditorMenuRegistry
{
public:
    static EditorMenuRegistry& Get();

    void RegisterMenuItem(EditorMenuItemDescriptor descriptor);

    std::vector<EditorMenuItemSnapshot> Snapshot() const;

    // Toolbar command dispatch. Returns false when the id is not a native
    // menu command (the caller falls through to the script registry).
    bool TryInvoke(uint32_t commandId) const;

    // Folder context-menu items. A same-Path registration replaces forward and
    // keeps its command id, like a toolbar item.
    void RegisterDirectoryMenuItem(EditorDirectoryMenuItemDescriptor descriptor);
    std::vector<EditorDirectoryMenuItemSnapshot> DirectoryMenuSnapshot() const;
    // Runs the folder item registered under commandId on `folder`. Returns
    // false when the id is not a registered folder item.
    bool TryInvokeDirectoryItem(uint32_t commandId, const std::filesystem::path& folder) const;

    // Fixed-id commands. A same-id registration replaces forward, like a
    // same-Path menu item. Ids inside the native menu range are rejected
    // loudly: the registry assigns those.
    void RegisterCommand(EditorCommandDescriptor descriptor);

    // Runs the fixed-id command registered under commandId. NotFound leaves
    // outError untouched so the caller can try its other dispatchers.
    EditorCommandResult InvokeCommand(uint32_t commandId, std::string* outError) const;

    // True once after any registration since the last consume; the editor
    // polls this to rebuild the native toolbar (packages load at project
    // open, long after the toolbar first builds).
    bool ConsumeDirty();

    // C12 editor-kind unload refusal diagnostics: menu actions hold module
    // code and pin the module's images mapped.
    void AppendModulePins(std::string_view moduleId, std::vector<std::string>& outPins) const;

private:
    EditorMenuRegistry() = default;

    struct Entry
    {
        EditorMenuItemDescriptor Descriptor;
        uint32_t CommandId = 0;
        ECS::ModuleRegistrationStamp Module;
    };

    // Command ids are kNativeMenuCmdFirst + index: entries are never removed
    // (loud no-unload; a module rebuild replaces in place and keeps its id),
    // so the index is a stable, collision-free assignment.
    std::vector<Entry> m_Items;

    struct DirectoryEntry
    {
        EditorDirectoryMenuItemDescriptor Descriptor;
        uint32_t CommandId = 0;
        ECS::ModuleRegistrationStamp Module;
    };
    std::vector<DirectoryEntry> m_DirectoryItems;

    struct CommandEntry
    {
        EditorCommandDescriptor Descriptor;
        ECS::ModuleRegistrationStamp Module;
    };
    std::vector<CommandEntry> m_Commands;

    bool m_Dirty = false;
};

} // namespace GameEngine::Editor
