#include "Scripting/EditorScriptMenuRegistry.h"

#include "Logger/Logger.h"
#include "Scripting/CoreCLRHost.h"
#include "Scripting/PathResolver.h"

#include <cstring>
#include <filesystem>

namespace GameEngine::Editor
{
namespace
{
static std::string Utf8ToString(const uint8_t* data, uint32_t len)
{
    if (!data || len == 0)
        return {};
    return std::string(reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(data) + len);
}

// Native callback invoked from managed EditorMenuBridge.
extern "C" void CORECLR_DELEGATE_CALLTYPE EditorMenu_OnSnapshot(const EditorMenuItemNative* items, uint32_t count, void* userData)
{
    (void)userData;
    try
    {
        try
        {
            Logger::Log::Info("ScriptMenuRegistry: received menu snapshot count={}", count);
            if (items && count > 0)
            {
                auto p0 = Utf8ToString(items[0].pathUtf8, items[0].pathLen);
                auto m0 = Utf8ToString(items[0].methodUtf8, items[0].methodLen);
                Logger::Log::Info("ScriptMenuRegistry: first item kind={} targets=0x{:X} path='{}' method='{}'",
                                  items[0].kind, items[0].targetsMask, p0, m0);
            }
        }
        catch (...)
        {
        }
        ScriptMenuRegistry::Get().ReplaceFromSnapshot(items, count);
    }
    catch (...)
    {
        // Never allow exceptions to cross the managed->native boundary.
    }
}
} // namespace

ScriptMenuRegistry& ScriptMenuRegistry::Get()
{
    static ScriptMenuRegistry s;
    return s;
}

void ScriptMenuRegistry::ReplaceFromSnapshot(const EditorMenuItemNative* items, uint32_t count)
{
    auto fnv1a64_append = [](uint64_t h, const void* data, size_t len) -> uint64_t
    {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        constexpr uint64_t kPrime = 1099511628211ull;
        for (size_t i = 0; i < len; ++i)
        {
            h ^= (uint64_t)p[i];
            h *= kPrime;
        }
        return h;
    };
    auto fnv1a64_u32 = [&](uint64_t h, uint32_t v) -> uint64_t { return fnv1a64_append(h, &v, sizeof(v)); };
    auto fnv1a64_u64 = [&](uint64_t h, uint64_t v) -> uint64_t { return fnv1a64_append(h, &v, sizeof(v)); };

    // Compute per-kind fingerprints from the raw snapshot first (avoids unnecessary rebuilds).
    constexpr uint64_t kOffset = 14695981039346656037ull;
    uint64_t fpToolbar = kOffset;
    uint64_t fpContext = kOffset;
    uint64_t dom = 0;
    if (items && count > 0)
    {
        dom = items[0].domainId;
        fpToolbar = fnv1a64_u64(fpToolbar, dom);
        fpContext = fnv1a64_u64(fpContext, dom);
        fpToolbar = fnv1a64_u32(fpToolbar, count);
        fpContext = fnv1a64_u32(fpContext, count);

        for (uint32_t i = 0; i < count; ++i)
        {
            const EditorMenuItemNative& it = items[i];
            const uint32_t kind = it.kind;
            uint64_t& h = (static_cast<ScriptMenuKind>(kind) == ScriptMenuKind::Toolbar) ? fpToolbar : fpContext;
            h = fnv1a64_u64(h, it.domainId);
            h = fnv1a64_u32(h, it.kind);
            h = fnv1a64_u32(h, it.targetsMask);
            h = fnv1a64_u32(h, static_cast<uint32_t>(it.priority));
            if (it.pathUtf8 && it.pathLen > 0)
                h = fnv1a64_append(h, it.pathUtf8, it.pathLen);
            h = fnv1a64_u32(h, it.pathLen);
            if (it.methodUtf8 && it.methodLen > 0)
                h = fnv1a64_append(h, it.methodUtf8, it.methodLen);
            h = fnv1a64_u32(h, it.methodLen);
        }
    }
    else
    {
        fpToolbar = fnv1a64_u32(fpToolbar, count);
        fpContext = fnv1a64_u32(fpContext, count);
    }

    bool toolbarChanged = true;
    bool contextChanged = true;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        toolbarChanged = (fpToolbar != m_LastToolbarSnapshotFingerprint);
        contextChanged = (fpContext != m_LastContextSnapshotFingerprint);
        if (!toolbarChanged && !contextChanged)
        {
            // Identical snapshot; no-op (avoids redundant native toolbar rebuilds).
            return;
        }
        m_LastToolbarSnapshotFingerprint = fpToolbar;
        m_LastContextSnapshotFingerprint = fpContext;
    }

    std::vector<ScriptToolbarMenuItem> toolbar;
    std::vector<ScriptContextMenuItem> context;
    std::unordered_map<uint32_t, std::pair<uint64_t, std::string>> bindings;

    toolbar.reserve(count);
    context.reserve(count);
    bindings.reserve(count);

    if (items && count > 0)
    {
        // IMPORTANT (Win32):
        // Menu-bar commands arrive via WM_COMMAND and only carry a 16-bit identifier (LOWORD(wParam)).
        // Therefore, toolbar/menu command IDs must fit in 0..0xFFFF to be reliably routed on Windows.
        //
        // Pick a high 16-bit range to avoid colliding with built-in Editor commands (Undo/Redo/etc.).
        constexpr uint32_t kScriptCmdFirst = 0xA000u;
        constexpr uint32_t kScriptCmdLast = 0xEFFFu; // avoid 0xF000..0xFFFF (common reserved/system ranges)
        uint32_t nextCmd = kScriptCmdFirst;

        for (uint32_t i = 0; i < count; ++i)
        {
            if (nextCmd > kScriptCmdLast)
            {
                Logger::Log::Warning("ScriptMenuRegistry: script menu command id range exhausted ({} items); skipping remaining items",
                                     (kScriptCmdLast - kScriptCmdFirst) + 1u);
                break;
            }

            const EditorMenuItemNative& it = items[i];
            const std::string path = Utf8ToString(it.pathUtf8, it.pathLen);
            const std::string method = Utf8ToString(it.methodUtf8, it.methodLen);
            if (path.empty() || method.empty())
                continue;

            const auto kind = static_cast<ScriptMenuKind>(it.kind);
            if (kind == ScriptMenuKind::Toolbar)
            {
                ScriptToolbarMenuItem ti;
                ti.domainId = it.domainId;
                ti.commandId = nextCmd++;
                ti.path = path;
                ti.method = method;
                ti.priority = it.priority;
                bindings[ti.commandId] = std::make_pair(ti.domainId, ti.method);
                toolbar.push_back(std::move(ti));
            }
            else if (kind == ScriptMenuKind::Context)
            {
                ScriptContextMenuItem ci;
                ci.domainId = it.domainId;
                ci.commandId = nextCmd++;
                ci.targetsMask = it.targetsMask;
                ci.path = path;
                ci.method = method;
                ci.priority = it.priority;
                bindings[ci.commandId] = std::make_pair(ci.domainId, ci.method);
                context.push_back(std::move(ci));
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_CurrentDomainId = dom;
        m_ToolbarItems.swap(toolbar);
        m_ContextItems.swap(context);
        m_CommandBindings.swap(bindings);
        if (toolbarChanged)
            m_ToolbarDirty.store(true, std::memory_order_release);
    }
}

std::vector<ScriptToolbarMenuItem> ScriptMenuRegistry::GetToolbarItems() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_ToolbarItems;
}

std::vector<ScriptContextMenuItem> ScriptMenuRegistry::GetContextItems(uint32_t requiredTargetsMask) const
{
    std::vector<ScriptContextMenuItem> out;
    std::lock_guard<std::mutex> lock(m_Mutex);
    out.reserve(m_ContextItems.size());
    for (const auto& it : m_ContextItems)
    {
        if ((it.targetsMask & requiredTargetsMask) != 0)
        {
            out.push_back(it);
        }
    }
    return out;
}

bool ScriptMenuRegistry::TryResolveCommand(uint32_t commandId, uint64_t& outDomainId, std::string& outMethodFqn) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_CommandBindings.find(commandId);
    if (it == m_CommandBindings.end())
    {
        return false;
    }
    outDomainId = it->second.first;
    outMethodFqn = it->second.second;
    return true;
}

bool ScriptMenuRegistry::ConsumeToolbarDirty()
{
    return m_ToolbarDirty.exchange(false, std::memory_order_acq_rel);
}

uint64_t ScriptMenuRegistry::GetCurrentDomainId() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_CurrentDomainId;
}

bool ScriptMenuRegistry::TryRegisterManagedBridge(CoreCLRHost& clrHost)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_BridgeRegistered)
        return true;

    const std::filesystem::path dllPath =
        ScriptingPaths::ResolveEngineManagedDirectory() / "GameEngine.Editor.Managed.dll";
    std::error_code ec;
    if (!std::filesystem::exists(dllPath, ec))
    {
        Logger::Log::Warning("ScriptMenuRegistry: Editor managed bridge not found at '{}'", dllPath.string());
        return false;
    }

    Logger::Log::Info("ScriptMenuRegistry: attempting managed bridge registration from {}", dllPath.string());

    const std::string methodName = "RegisterMenuSnapshotCallback";

    void* fnPtr = clrHost.GetManagedFunction(
        dllPath.string(), "GameEngine.Editor.Managed.EditorMenuBridge, GameEngine.Editor.Managed", methodName);
    if (!fnPtr)
    {
        Logger::Log::Warning("ScriptMenuRegistry: failed to resolve EditorMenuBridge.RegisterMenuSnapshotCallback");
        return false;
    }

    using RegisterFn = int32_t(CORECLR_DELEGATE_CALLTYPE*)(intptr_t callbackFn, intptr_t userData);
    auto fn = reinterpret_cast<RegisterFn>(fnPtr);

    const intptr_t cb = reinterpret_cast<intptr_t>(&EditorMenu_OnSnapshot);
    const intptr_t ud = reinterpret_cast<intptr_t>(this);
    const int32_t rc = fn(cb, ud);
    if (rc != 0)
    {
        Logger::Log::Warning("ScriptMenuRegistry: RegisterMenuSnapshotCallback failed rc={}", rc);
        return false;
    }

    m_BridgeRegistered = true;
    Logger::Log::Info("ScriptMenuRegistry: registered managed menu bridge");
    return true;
}

} // namespace GameEngine::Editor
