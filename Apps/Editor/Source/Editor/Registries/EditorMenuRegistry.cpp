#include "Editor/Registries/EditorMenuRegistry.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <string>
#include <utility>

namespace GameEngine::Editor
{

namespace
{
// Win32 menu commands carry a 16-bit id (LOWORD(wParam)); this range must
// stay clear of the built-in editor commands (0x1000/0x2000 utility items,
// 0x3000-0x30FF panel openers), the context-menu/UI-replay ids
// (0x4000-0x5Bxx), and the script items (0xA000-0xEFFF).
constexpr uint32_t kNativeMenuCmdFirst = 0x6000u;
constexpr uint32_t kNativeMenuCmdLast = 0x6FFFu;
// Folder context-menu items: next to the native range, clear of the same users.
constexpr uint32_t kDirectoryMenuCmdFirst = 0x7000u;
constexpr uint32_t kDirectoryMenuCmdLast = 0x70FFu;
} // namespace

EditorMenuRegistry& EditorMenuRegistry::Get()
{
    static EditorMenuRegistry s_Instance;
    return s_Instance;
}

void EditorMenuRegistry::RegisterMenuItem(EditorMenuItemDescriptor descriptor)
{
    if (descriptor.Path.empty() || !descriptor.Action)
    {
        Logger::Log::Error("EditorMenuRegistry: rejecting menu item registration without {}",
                           descriptor.Path.empty() ? "a Path" : ("an Action ('" + descriptor.Path + "')"));
        return;
    }
    if (descriptor.Path.find('/') == std::string::npos)
    {
        Logger::Log::Error("EditorMenuRegistry: menu item '{}' needs at least \"Menu/Item\"; ignored",
                           descriptor.Path);
        return;
    }

    const auto samePath = [&](const Entry& existing) {
        return existing.Descriptor.Path == descriptor.Path;
    };
    const auto existing = std::find_if(m_Items.begin(), m_Items.end(), samePath);
    if (existing != m_Items.end())
    {
        // Replace-forward: the previous action's module DLL stays mapped for
        // the process lifetime, so swapping the descriptor is safe and the
        // stale action stops being dispatched. The command id stays stable —
        // a rebuilt toolbar and an un-rebuilt one dispatch the same item.
        Logger::Log::Info("EditorMenuRegistry: menu item '{}' re-registered (module reload); "
                          "replacing forward",
                          descriptor.Path);
        existing->Descriptor = std::move(descriptor);
        existing->Module = ECS::GetActiveRegistrationModule();
    }
    else
    {
        const uint32_t id = kNativeMenuCmdFirst + static_cast<uint32_t>(m_Items.size());
        if (id > kNativeMenuCmdLast)
        {
            Logger::Log::Error("EditorMenuRegistry: native menu command range exhausted ({} items); "
                               "'{}' not registered",
                               m_Items.size(), descriptor.Path);
            return;
        }
        m_Items.push_back(Entry{std::move(descriptor), id, ECS::GetActiveRegistrationModule()});
    }
    m_Dirty = true;
}

std::vector<EditorMenuItemSnapshot> EditorMenuRegistry::Snapshot() const
{
    std::vector<EditorMenuItemSnapshot> out;
    out.reserve(m_Items.size());
    for (const Entry& entry : m_Items)
        out.push_back(EditorMenuItemSnapshot{entry.CommandId, entry.Descriptor.Path,
                                             entry.Descriptor.Priority});
    std::sort(out.begin(), out.end(),
              [](const EditorMenuItemSnapshot& a, const EditorMenuItemSnapshot& b) {
                  if (a.Priority != b.Priority)
                      return a.Priority < b.Priority;
                  return a.Path < b.Path;
              });
    return out;
}

bool EditorMenuRegistry::TryInvoke(uint32_t commandId) const
{
    const auto it = std::find_if(m_Items.begin(), m_Items.end(),
                                 [&](const Entry& e) { return e.CommandId == commandId; });
    if (it == m_Items.end())
        return false;
    it->Descriptor.Action();
    return true;
}

void EditorMenuRegistry::RegisterDirectoryMenuItem(EditorDirectoryMenuItemDescriptor descriptor)
{
    if (descriptor.Path.empty() || !descriptor.Action)
    {
        Logger::Log::Error("EditorMenuRegistry: rejecting folder menu item registration without {}",
                           descriptor.Path.empty() ? "a Path" : ("an Action ('" + descriptor.Path + "')"));
        return;
    }
    const auto existing = std::find_if(m_DirectoryItems.begin(), m_DirectoryItems.end(),
                                       [&](const DirectoryEntry& e) { return e.Descriptor.Path == descriptor.Path; });
    if (existing != m_DirectoryItems.end())
    {
        existing->Descriptor = std::move(descriptor);
        existing->Module = ECS::GetActiveRegistrationModule();
        return;
    }
    const uint32_t id = kDirectoryMenuCmdFirst + static_cast<uint32_t>(m_DirectoryItems.size());
    if (id > kDirectoryMenuCmdLast)
    {
        Logger::Log::Error("EditorMenuRegistry: folder menu command range exhausted ({} items); '{}' not registered",
                           m_DirectoryItems.size(), descriptor.Path);
        return;
    }
    m_DirectoryItems.push_back(DirectoryEntry{std::move(descriptor), id, ECS::GetActiveRegistrationModule()});
}

std::vector<EditorDirectoryMenuItemSnapshot> EditorMenuRegistry::DirectoryMenuSnapshot() const
{
    std::vector<EditorDirectoryMenuItemSnapshot> out;
    out.reserve(m_DirectoryItems.size());
    for (const DirectoryEntry& entry : m_DirectoryItems)
        out.push_back(EditorDirectoryMenuItemSnapshot{entry.CommandId, entry.Descriptor.Path,
                                                      entry.Descriptor.Priority, entry.Descriptor.Icon});
    return out;
}

bool EditorMenuRegistry::TryInvokeDirectoryItem(uint32_t commandId, const std::filesystem::path& folder) const
{
    const auto it = std::find_if(m_DirectoryItems.begin(), m_DirectoryItems.end(),
                                 [&](const DirectoryEntry& e) { return e.CommandId == commandId; });
    if (it == m_DirectoryItems.end())
        return false;
    it->Descriptor.Action(folder);
    return true;
}

void EditorMenuRegistry::RegisterCommand(EditorCommandDescriptor descriptor)
{
    if (descriptor.CommandId == 0u || !descriptor.Invoke)
    {
        Logger::Log::Error("EditorMenuRegistry: rejecting command registration without {} (id {:#x})",
                           descriptor.CommandId == 0u ? "a CommandId" : "an Invoke",
                           descriptor.CommandId);
        return;
    }
    if ((descriptor.CommandId >= kNativeMenuCmdFirst && descriptor.CommandId <= kNativeMenuCmdLast) ||
        (descriptor.CommandId >= kDirectoryMenuCmdFirst && descriptor.CommandId <= kDirectoryMenuCmdLast))
    {
        Logger::Log::Error("EditorMenuRegistry: command id {:#x} lies in a range the registry assigns "
                           "({:#x}-{:#x} menu items, {:#x}-{:#x} folder menu items); pick an id outside them",
                           descriptor.CommandId, kNativeMenuCmdFirst, kNativeMenuCmdLast, kDirectoryMenuCmdFirst,
                           kDirectoryMenuCmdLast);
        return;
    }

    const auto existing = std::find_if(m_Commands.begin(), m_Commands.end(), [&](const CommandEntry& e) {
        return e.Descriptor.CommandId == descriptor.CommandId;
    });
    if (existing != m_Commands.end())
    {
        existing->Descriptor = std::move(descriptor);
        existing->Module = ECS::GetActiveRegistrationModule();
        return;
    }
    m_Commands.push_back(CommandEntry{std::move(descriptor), ECS::GetActiveRegistrationModule()});
}

EditorCommandResult EditorMenuRegistry::InvokeCommand(uint32_t commandId, std::string* outError) const
{
    const auto it = std::find_if(m_Commands.begin(), m_Commands.end(), [&](const CommandEntry& e) {
        return e.Descriptor.CommandId == commandId;
    });
    if (it == m_Commands.end())
        return EditorCommandResult::NotFound;
    return it->Descriptor.Invoke(outError) ? EditorCommandResult::Succeeded : EditorCommandResult::Failed;
}

bool EditorMenuRegistry::ConsumeDirty()
{
    return std::exchange(m_Dirty, false);
}

void EditorMenuRegistry::AppendModulePins(std::string_view moduleId,
                                          std::vector<std::string>& outPins) const
{
    if (moduleId.empty())
        return;
    for (const Entry& entry : m_Items)
    {
        if (entry.Module.ModuleId == moduleId)
            outPins.push_back("menu item '" + entry.Descriptor.Path + "'");
    }
    for (const DirectoryEntry& entry : m_DirectoryItems)
    {
        if (entry.Module.ModuleId == moduleId)
            outPins.push_back("folder menu item '" + entry.Descriptor.Path + "'");
    }
    for (const CommandEntry& entry : m_Commands)
    {
        if (entry.Module.ModuleId == moduleId)
            outPins.push_back("command " + std::to_string(entry.Descriptor.CommandId));
    }
}

} // namespace GameEngine::Editor
