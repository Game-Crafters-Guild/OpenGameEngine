#include "Scripting/ManagedSystemBridge.h"

#include "Core/Application.h"
#include "Core/Engine.h"
#include "Scripting/CoreCLRHost.h"
#include "Scripting/NativeAotScriptsLibrary.h"
#include "Scripting/PathResolver.h"
#include "Logger/Logger.h"

#include <filesystem>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace GameEngine
{

static const char* kExportsTypeName =
    "GameEngine.Scripting.Runtime.GameSystemRunnerExports, GameEngine.Scripting.Runtime";

// ---------------------------------------------------------------------------
// NativeAOT loading via platform-specific dynamic library APIs
// ---------------------------------------------------------------------------

bool ManagedSystemBridge::TryLoadNativeAOT()
{
    auto exeDir = PathUtils::GetExecutableDirectory();
    auto nativeLibPath = exeDir / kNativeAotScriptsLibraryName;

    if (!std::filesystem::exists(nativeLibPath))
        return false;

    Logger::Log::Info("[ManagedSystemBridge] Found NativeAOT library at '{}'", nativeLibPath.string());

#ifdef _WIN32
    HMODULE handle = LoadLibraryW(nativeLibPath.wstring().c_str());
    if (!handle)
    {
        Logger::Log::Error("[ManagedSystemBridge] LoadLibrary failed for '{}' (error {})",
                           nativeLibPath.string(), GetLastError());
        return false;
    }
    m_NativeAOTHandle = static_cast<void*>(handle);

    auto* initFn = reinterpret_cast<InitializeFn>(GetProcAddress(handle, "ge_scripts_initialize"));
    auto* tickFn = reinterpret_cast<TickFn>(GetProcAddress(handle, "ge_scripts_tick"));
    auto* shutdownFn = reinterpret_cast<ShutdownFn>(GetProcAddress(handle, "ge_scripts_shutdown"));
#else
    void* handle = dlopen(nativeLibPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle)
    {
        Logger::Log::Error("[ManagedSystemBridge] dlopen failed for '{}': {}",
                           nativeLibPath.string(), dlerror());
        return false;
    }
    m_NativeAOTHandle = handle;

    auto* initFn = reinterpret_cast<InitializeFn>(dlsym(handle, "ge_scripts_initialize"));
    auto* tickFn = reinterpret_cast<TickFn>(dlsym(handle, "ge_scripts_tick"));
    auto* shutdownFn = reinterpret_cast<ShutdownFn>(dlsym(handle, "ge_scripts_shutdown"));
#endif

    if (!initFn || !tickFn || !shutdownFn)
    {
        Logger::Log::Error("[ManagedSystemBridge] NativeAOT library loaded but exports not found "
                           "(init={}, tick={}, shutdown={})",
                           initFn != nullptr, tickFn != nullptr, shutdownFn != nullptr);
        // Clean up the loaded library to avoid leaving an unusable handle.
        // Note: NativeAOT normally doesn't support unloading, but since we never
        // called any managed code, the runtime may not be fully initialized yet.
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(m_NativeAOTHandle));
#else
        dlclose(m_NativeAOTHandle);
#endif
        m_NativeAOTHandle = nullptr;
        return false;
    }

    m_Initialize = initFn;
    m_Tick = tickFn;
    m_Shutdown = shutdownFn;
    m_UsingNativeAOT = true;

    Logger::Log::Info("[ManagedSystemBridge] NativeAOT exports resolved successfully");
    return true;
}

// ---------------------------------------------------------------------------
// CoreCLR resolution (existing path)
// ---------------------------------------------------------------------------

void ManagedSystemBridge::ResolveCoreClr()
{
    auto& clrHost = EngineCore::GetInstance().GetScriptManager().GetCLRHost();
    if (!clrHost.IsInitialized())
    {
        m_ResolveFailed = true;
        Logger::Log::Error("[ManagedSystemBridge] CoreCLR host not initialized");
        return;
    }

    // Locate GameEngine.Scripting.Runtime.dll next to the CoreBridge assembly.
    std::filesystem::path coreBridgePath = ScriptingPaths::ResolveCoreBridgeDll();
    if (coreBridgePath.empty() || !std::filesystem::exists(coreBridgePath))
    {
        m_ResolveFailed = true;
        Logger::Log::Error("[ManagedSystemBridge] CoreBridge assembly not found; "
                           "cannot locate Scripting.Runtime");
        return;
    }

    std::filesystem::path runtimeAssemblyPath =
        coreBridgePath.parent_path() / ScriptingPaths::kScriptingRuntimeAssemblyFileName;
    if (!std::filesystem::exists(runtimeAssemblyPath))
    {
        m_ResolveFailed = true;
        Logger::Log::Error("[ManagedSystemBridge] {} not found at: {}",
                           ScriptingPaths::kScriptingRuntimeAssemblyFileName,
                           runtimeAssemblyPath.string());
        return;
    }

    String asmPath = runtimeAssemblyPath.string();

    auto* initFn = reinterpret_cast<InitializeFn>(
        clrHost.GetManagedFunction(asmPath, kExportsTypeName, "NativeInitialize"));
    auto* tickFn = reinterpret_cast<TickFn>(
        clrHost.GetManagedFunction(asmPath, kExportsTypeName, "NativeTick"));
    auto* shutdownFn = reinterpret_cast<ShutdownFn>(
        clrHost.GetManagedFunction(asmPath, kExportsTypeName, "NativeShutdown"));

    if (!initFn || !tickFn || !shutdownFn)
    {
        m_ResolveFailed = true;
        Logger::Log::Error("[ManagedSystemBridge] Failed to resolve managed exports from {}",
                           asmPath);
        return;
    }

    m_Initialize = initFn;
    m_Tick = tickFn;
    m_Shutdown = shutdownFn;

    Logger::Log::Info("[ManagedSystemBridge] Managed exports resolved from {}",
                      runtimeAssemblyPath.string());
}

// ---------------------------------------------------------------------------
// EnsureResolved -- tries NativeAOT first, falls back to CoreCLR
// ---------------------------------------------------------------------------

void ManagedSystemBridge::EnsureResolved()
{
    if (m_Initialize || m_ResolveFailed)
        return;

    // Try NativeAOT first -- no CoreCLR dependency needed.
    if (TryLoadNativeAOT())
        return;

    // Fall back to CoreCLR-hosted managed resolution.
    ResolveCoreClr();
}

void ManagedSystemBridge::Update(ECS::World& world, float32 deltaTime)
{
    EnsureResolved();
    if (m_ResolveFailed)
        return;

    // Lazily initialize the managed system runner with the world handle.
    if (!m_Initialized)
    {
        auto worldHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&world));
        int result = m_Initialize(worldHandle);
        if (result != 0)
        {
            Logger::Log::Error("[ManagedSystemBridge] NativeInitialize returned {}", result);
            m_ResolveFailed = true;
            return;
        }
        m_Initialized = true;
    }

    // Structural change fencing is handled inside the managed GameSystemRunner.Tick().
    // It sets defer=true, executes all systems, then flushes deferred commands in a
    // finally block. This works identically for both editor (PlayModeDriver) and
    // standalone (ManagedSystemBridge) paths — no need to fence here.
    int result = m_Tick(deltaTime);

    if (result != 0)
    {
        Logger::Log::Error("[ManagedSystemBridge] NativeTick returned {}", result);
    }
    else if (!m_LoggedFirstTick)
    {
        // One-time, deliberate: end-to-end evidence that C# GameSystems are
        // actually ticking in a standalone/packaged run (assembly-load logs
        // alone proved insufficient for E2E verification).
        m_LoggedFirstTick = true;
        Logger::Log::Info("[ManagedSystemBridge] First GameSystemRunner tick completed (mode={})",
                          m_UsingNativeAOT ? "NativeAOT" : "CoreCLR");
    }
}

ManagedSystemBridge::~ManagedSystemBridge()
{
    if (m_Initialized && m_Shutdown)
    {
        int result = m_Shutdown();
        if (result != 0)
            Logger::Log::Warning("[ManagedSystemBridge] NativeShutdown returned {}", result);
    }

    // NativeAOT docs state that unloading a NativeAOT library is unsupported.
    // We null out the handle without calling FreeLibrary/dlclose.
    m_NativeAOTHandle = nullptr;
}

} // namespace GameEngine
