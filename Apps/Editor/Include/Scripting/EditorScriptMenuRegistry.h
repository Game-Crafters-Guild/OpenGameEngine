#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
class CoreCLRHost;

namespace Editor
{
// Mirror of managed EditorMenuBridge.MenuKind
enum class ScriptMenuKind : uint32_t
{
    Toolbar = 0,
    Context = 1,
};

// Mirror of GameEngine.Editor.Scripting.EditorContextMenuTarget (managed)
enum class EditorContextMenuTarget : uint32_t
{
    None = 0,
    AssetsItem = 1u << 0,
    AssetsEmpty = 1u << 1,
    HierarchyItem = 1u << 2,
    HierarchyEmpty = 1u << 3,
    SceneView = 1u << 4,
};

// Blittable snapshot item layout sent from managed EditorMenuBridge.
// IMPORTANT: Keep in sync with Managed/Editor.Managed/EditorMenuBridge.cs (EditorMenuItemNative).
struct EditorMenuItemNative
{
    uint64_t domainId;
    uint32_t kind;        // ScriptMenuKind
    uint32_t targetsMask; // EditorContextMenuTarget bitmask (0 for toolbar)
    int32_t  priority;

    const uint8_t* pathUtf8;
    uint32_t       pathLen;

    const uint8_t* methodUtf8;
    uint32_t       methodLen;
};

struct ScriptToolbarMenuItem
{
    uint64_t    domainId = 0;
    uint32_t    commandId = 0;
    std::string path;
    std::string method;
    int32_t     priority = 0;
};

struct ScriptContextMenuItem
{
    uint64_t    domainId = 0;
    uint32_t    commandId = 0;
    uint32_t    targetsMask = 0;
    std::string path;
    std::string method;
    int32_t     priority = 0;
};

// Editor-side registry of script-driven menu items.
class ScriptMenuRegistry
{
  public:
    static ScriptMenuRegistry& Get();

    // Called by native callback invoked from managed.
    void ReplaceFromSnapshot(const EditorMenuItemNative* items, uint32_t count);

    std::vector<ScriptToolbarMenuItem> GetToolbarItems() const;
    std::vector<ScriptContextMenuItem> GetContextItems(uint32_t requiredTargetsMask) const;

    // Resolve a script command id to (domainId, methodFqn). Returns false if not found.
    bool TryResolveCommand(uint32_t commandId, uint64_t& outDomainId, std::string& outMethodFqn) const;

    // Dirty flag used by EditorApplication to refresh native toolbar on the main thread.
    bool ConsumeToolbarDirty();

    uint64_t GetCurrentDomainId() const;

    // Register the native snapshot callback with the Editor-managed bridge.
    // Returns true when registration succeeds (idempotent).
    bool TryRegisterManagedBridge(CoreCLRHost& clrHost);

  private:
    ScriptMenuRegistry() = default;

    mutable std::mutex m_Mutex;
    uint64_t m_CurrentDomainId = 0;
    std::vector<ScriptToolbarMenuItem> m_ToolbarItems;
    std::vector<ScriptContextMenuItem> m_ContextItems;
    std::unordered_map<uint32_t, std::pair<uint64_t, std::string>> m_CommandBindings;
    std::atomic<bool> m_ToolbarDirty{false};
    // Fingerprints of the last snapshot (used to avoid redundant toolbar rebuilds).
    // Initialize to an impossible sentinel so the first snapshot always applies.
    uint64_t m_LastToolbarSnapshotFingerprint = 0xFFFFFFFFFFFFFFFFull; // guarded by m_Mutex
    uint64_t m_LastContextSnapshotFingerprint = 0xFFFFFFFFFFFFFFFFull; // guarded by m_Mutex
    bool m_BridgeRegistered = false; // guarded by m_Mutex
};

} // namespace Editor
} // namespace GameEngine


