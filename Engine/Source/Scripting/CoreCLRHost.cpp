#include "Scripting/CoreCLRHost.h"
#include "Logger/Logger.h"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include <filesystem>
#include <fstream>

#include <atomic>
#include <cstdint>

#include <cstdlib> // getenv/atoi for configurable async delay
#include <cstring> // strlen for UCO name lengths

#include "Scripting/PathResolver.h"

#include "Scripting/ScriptingABI.h"

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#endif

#if defined(NETHOST_AVAILABLE)
// coreclr_delegates.h/hostfxr.h/nethost.h are pulled in from CoreCLRHost.h
#if !defined(PLATFORM_WINDOWS)
#include <dlfcn.h>
#endif
#endif

#ifdef PLATFORM_WINDOWS
static std::string Utf8FromWide(const std::wstring& w)
{
    if (w.empty())
        return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return std::string();
    std::string s((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), size, nullptr, nullptr);
    return s;
}
#endif

#ifdef PLATFORM_WINDOWS
static void Host_DebugWrite(const char* msg)
{
    try
    {
        auto p = std::filesystem::current_path() / "coreclrhost-debug.txt";
        std::ofstream ofs(p, std::ios::app | std::ios::out);
        ofs << msg << "\n";
    }
    catch (...)
    {
    }
}
#endif

#ifndef PLATFORM_WINDOWS
static inline void Host_DebugWrite(const char* msg)
{
    (void)msg;
}
#endif

static std::atomic<int32_t> g_dbgUnloadDomainCount{0};

#if defined(NETHOST_AVAILABLE)
static std::filesystem::path ResolveCoreBridgePath(const std::filesystem::path& preferred)
{
    // Hardened resolver: try preferred via centralized resolver, then default resolver, then CWD
    namespace SP = GameEngine::ScriptingPaths;
    std::filesystem::path p = SP::ResolveCoreBridgeDll(preferred);
    if (!p.empty() && std::filesystem::exists(p))
        return p;

    // Fallback: try without preferred (default probes)
    p = SP::ResolveCoreBridgeDll();
    if (!p.empty() && std::filesystem::exists(p))
        return p;

    // Final fallback: current working directory
    std::filesystem::path cwdCand = std::filesystem::current_path() / "GameEngine.CoreBridge.dll";
    if (std::filesystem::exists(cwdCand))
        return cwdCand;

    return p; // may be empty
}

namespace
{
// Process-wide shared CoreCLR hosting state (single initialization, cached delegates)
static std::mutex g_coreclrMutex;
static void* g_hostfxrLib = nullptr;
static hostfxr_initialize_for_runtime_config_fn g_initForConfig = nullptr;
static hostfxr_get_runtime_delegate_fn g_getRuntimeDelegate = nullptr;
static hostfxr_close_fn g_close = nullptr;
static load_assembly_and_get_function_pointer_fn g_loadAssemblyAndGetFunctionPointer = nullptr;
static get_function_pointer_fn g_getFunctionPointer = nullptr;
static load_assembly_fn g_loadAssemblyIntoDefault = nullptr;
static hostfxr_handle g_hostfxrContext = nullptr;
static int g_runtimeRefCount = 0;

// Assemblies already pushed into the Default ALC via hdt_load_assembly (keyed by
// absolute path). Loading the same path twice is benign but noisy; skip it.
static std::mutex g_defaultLoadedMutex;
static std::unordered_set<std::string> g_defaultLoadedAssemblies;
} // namespace
#endif

#if defined(NETHOST_AVAILABLE)
namespace
{
static std::vector<std::string> GetCoreBridgeAssemblyNameCandidates(const std::filesystem::path& bridgeAssemblyPath)
{
    std::vector<std::string> names;
    names.emplace_back("GameEngine.CoreBridge");

    const std::string stem = bridgeAssemblyPath.stem().string();
    if (!stem.empty() && stem != names.front())
        names.emplace_back(stem);

    return names;
}
} // namespace
#endif

#include <atomic>
#include <limits>

#ifdef _DEBUG
static std::atomic<int32_t> g_CoreBridgeInitOverride{(std::numeric_limits<int32_t>::max)()};
namespace GameEngine
{
void SetCoreBridgeInitResultOverrideForTests(int32_t code)
{
    g_CoreBridgeInitOverride.store(code);
}
} // namespace GameEngine
#endif

namespace GameEngine
{

CoreCLRHost::CoreCLRHost()
    : m_Initialized(false)
#ifdef NETHOST_AVAILABLE
      ,
      m_HostfxrLib(nullptr), m_InitForConfig(nullptr), m_GetRuntimeDelegate(nullptr), m_Close(nullptr), m_LoadAssemblyAndGetFunctionPointer(nullptr), m_HostfxrContext(nullptr)
#endif
{
}

CoreCLRHost::~CoreCLRHost()
{
    if (m_Initialized)
    {
        Shutdown();
    }
}

bool CoreCLRHost::Initialize(const std::filesystem::path& runtimeConfigPath)
{
    if (m_Initialized)
    {
        Logger::Log::Warning("CoreCLRHost already initialized");
        return true;
    }

    Logger::Log::Info("Initializing CoreCLR Host");
    Logger::Log::Info("Runtime config: {}", runtimeConfigPath.string());

    // Avoid calling std::filesystem::absolute on an empty runtimeConfigPath
    // (it throws filesystem_error). This happens in stub builds where no
    // runtimeconfig.json is staged next to the executable (e.g. EditorTests),
    // so ResolveScriptsRuntimeConfig() returns an empty path. The debug trace
    // is only needed for platforms where we have a concrete path.
#ifdef PLATFORM_WINDOWS
    if (!runtimeConfigPath.empty())
    {
        Host_DebugWrite((std::string("Initialize: runtimeconfig=") + std::filesystem::absolute(runtimeConfigPath).string()).c_str());
    }
#endif

    m_RuntimeConfigPath = runtimeConfigPath;

#if defined(NETHOST_AVAILABLE)
    {
        std::lock_guard<std::mutex> lock(g_coreclrMutex);
        if (g_runtimeRefCount == 0)
        {
            // First-time initialization in this process
            if (!LoadHostfxr())
            {
                Logger::Log::Error("Failed to load hostfxr library");
                return false;
            }
            if (!InitializeHostfxr(runtimeConfigPath))
            {
                Logger::Log::Error("Failed to initialize hostfxr");
                return false;
            }
            if (!GetRuntimeDelegate())
            {
                Logger::Log::Error("Failed to get runtime delegate");
                return false;
            }
            // Cache process-wide state
            g_hostfxrLib = m_HostfxrLib;
            g_initForConfig = m_InitForConfig;
            g_getRuntimeDelegate = m_GetRuntimeDelegate;
            g_close = m_Close;
            g_loadAssemblyAndGetFunctionPointer = m_LoadAssemblyAndGetFunctionPointer;
            g_getFunctionPointer = m_GetFunctionPointer;
            g_loadAssemblyIntoDefault = m_LoadAssemblyIntoDefault;
            Logger::Log::Debug("CoreCLRHost: cached process-wide load-assembly delegate {} (default-ALC pair: getFn={} loadAsm={})",
                               (void*)g_loadAssemblyAndGetFunctionPointer,
                               (void*)g_getFunctionPointer,
                               (void*)g_loadAssemblyIntoDefault);
            g_hostfxrContext = m_HostfxrContext;
            g_runtimeRefCount = 1;
        }
        else
        {
            // Reuse cached state
            m_HostfxrLib = g_hostfxrLib;
            m_InitForConfig = g_initForConfig;
            m_GetRuntimeDelegate = g_getRuntimeDelegate;
            m_Close = g_close;
            m_LoadAssemblyAndGetFunctionPointer = g_loadAssemblyAndGetFunctionPointer;
            m_GetFunctionPointer = g_getFunctionPointer;
            m_LoadAssemblyIntoDefault = g_loadAssemblyIntoDefault;
            m_HostfxrContext = g_hostfxrContext;
            ++g_runtimeRefCount;
        }
    }
#else
    Logger::Log::Warning("CoreCLR hosting not available (NETHOST_AVAILABLE not defined)");
    return false;
#endif

    m_Initialized = true;
    Logger::Log::Info("CoreCLR Host initialized successfully");
    if (m_HostSwitches != 0 && !PushHostSwitches())
        Logger::Log::Error("CoreCLRHost: host switches 0x{:X} set before startup did not reach CoreBridge", m_HostSwitches);
    return true;
}

void CoreCLRHost::SetNativeLibraryOverride(const std::string& nativeLibraryPathUtf8)
{
    m_NativePathOverride = nativeLibraryPathUtf8;
}

void CoreCLRHost::SetHostSwitches(uint32 switches)
{
    m_HostSwitches = switches;
    if (m_Initialized && !PushHostSwitches())
        Logger::Log::Error("CoreCLRHost: host switches 0x{:X} did not reach CoreBridge", switches);
}

bool CoreCLRHost::PushHostSwitches()
{
    using SetSwitchesFn = int32(CORECLR_DELEGATE_CALLTYPE*)(uint32);
    auto setSwitches = reinterpret_cast<SetSwitchesFn>(GetCoreBridgeExport(GE_HOST_STR("SetHostSwitches")));
    return setSwitches != nullptr && setSwitches(m_HostSwitches) == 0;
}

void CoreCLRHost::Shutdown()
{
    if (!m_Initialized)
    {
        return;
    }

    // Do NOT call into managed (UCO) during shutdown/destructor. The CLR may be tearing down.

    Logger::Log::Info("Shutting down CoreCLR Host");

#if defined(NETHOST_AVAILABLE)
    {
        std::lock_guard<std::mutex> lock(g_coreclrMutex);
        // Keep process-wide hostfxr alive across tests/process lifetime to avoid re-init issues.
        // Only clear this instance's pointers.
        m_HostfxrContext = nullptr;
        m_HostfxrLib = nullptr;
        m_InitForConfig = nullptr;
        m_GetRuntimeDelegate = nullptr;
        m_Close = nullptr;
        m_LoadAssemblyAndGetFunctionPointer = nullptr;
        m_GetFunctionPointer = nullptr;
        m_LoadAssemblyIntoDefault = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(m_BridgeExportCacheMutex);
        m_BridgeExportsByName.clear();
    }
#endif

    // Clear loaded assembly path
    m_LoadedAssemblyPath.clear();

    // Reset static state to allow proper reinitialization
    ResetStaticState();

    m_Initialized = false;
#if defined(NETHOST_AVAILABLE)
    Logger::Log::Info("CoreCLRHost state before Initialize: initialized={} hostfxrContext={}", m_Initialized, (void*)m_HostfxrContext);
#else
    Logger::Log::Info("CoreCLRHost state before Initialize: initialized={} hostfxrContext={}", m_Initialized, (void*)nullptr);
#endif

    Logger::Log::Info("CoreCLR Host shutdown complete");
}

void CoreCLRHost::ResetStaticState()
{
    // Reset process-wide hosting state so tests (and controlled reinitialization
    // scenarios) can start from a clean slate. This is primarily intended for
    // test environments and normal engine shutdown.
#if defined(NETHOST_AVAILABLE)
    {
        std::lock_guard<std::mutex> lock(g_coreclrMutex);
        if (g_hostfxrContext && g_close)
        {
            g_close(g_hostfxrContext);
        }
        g_hostfxrContext = nullptr;
        g_hostfxrLib = nullptr;
        g_initForConfig = nullptr;
        g_getRuntimeDelegate = nullptr;
        g_close = nullptr;
        g_loadAssemblyAndGetFunctionPointer = nullptr;
        g_getFunctionPointer = nullptr;
        g_loadAssemblyIntoDefault = nullptr;
        g_runtimeRefCount = 0;
    }
    {
        std::lock_guard<std::mutex> lock(g_defaultLoadedMutex);
        g_defaultLoadedAssemblies.clear();
    }
#endif
    Logger::Log::Debug("CoreCLR static state reset requested for test reinitialization");
}

// Diagnostics callbacks for managed → native events
extern "C" void GE_CDECL Core_OnCompilationEvent(GE_CompilationStage stage,
                                                 float progress01,
                                                 const GE_Diagnostic* diags,
                                                 uint32_t diagCount,
                                                 void* userData)
{
    (void)userData;
    if (stage == GE_Comp_Started)
    {
        Logger::Log::Info("[Scripts] Compilation started");
    }
    else if (stage == GE_Comp_Progress)
    {
        Logger::Log::Debug("[Scripts] Compilation progress: {:.0f}%", progress01 * 100.0f);
    }
    else if (stage == GE_Comp_Completed)
    {
        Logger::Log::Info("[Scripts] Compilation completed ({} diagnostics)", (int)diagCount);
        if (diags && diagCount > 0)
        {
            for (uint32_t i = 0; i < diagCount; ++i)
            {
                const GE_Diagnostic& d = diags[i];
                const char* sev = d.severity == GE_Diag_Error ? "Error" : (d.severity == GE_Diag_Warning ? "Warning" : "Info");
                Logger::Log::Info("[{}] {} ({}:{}:{})",
                                  sev,
                                  d.message.data ? d.message.data : "",
                                  d.file.data ? d.file.data : "",
                                  d.line,
                                  d.column);
            }
        }
    }
}

extern "C" void GE_CDECL Core_OnReloadEvent(GE_ReloadStage stage,
                                            const char* reasonUtf8,
                                            void* userData)
{
    (void)userData;
    switch (stage)
    {
    case GE_Reload_Started:
        Logger::Log::Info("[Scripts] Reload started: {}", reasonUtf8 ? reasonUtf8 : "");
        break;
    case GE_Reload_Swapping:
        Logger::Log::Info("[Scripts] Reload swapping");
        break;
    case GE_Reload_Completed:
        Logger::Log::Info("[Scripts] Reload completed");
        break;
    case GE_Reload_Failed:
        Logger::Log::Error("[Scripts] Reload failed: {}", reasonUtf8 ? reasonUtf8 : "");
        break;
    default:
        break;
    }
}

bool CoreCLRHost::InitializeDiagnosticsSink()
{
#if defined(NETHOST_AVAILABLE)
    if (!m_LoadAssemblyAndGetFunctionPointer)
        return false;

    using RegisterSinkFn = int32(CORECLR_DELEGATE_CALLTYPE*)(void* onComp, void* onReload, void* userData);
    RegisterSinkFn reg = nullptr;

    std::filesystem::path bridgePath = ResolveCoreBridgePath(m_CoreBridgeAssemblyPath);
    if (bridgePath.empty() || !std::filesystem::exists(bridgePath))
    {
        Logger::Log::Warning("Diagnostics sink: CoreBridge.dll not found via PathResolver");
        return false;
    }

    void* regPtr = nullptr;
    int32 result = ResolveManagedUco(bridgePath,
                                     GE_HOST_STR("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge"),
                                     GE_HOST_STR("RegisterDiagnosticsSink"),
                                     &regPtr);
    reg = reinterpret_cast<RegisterSinkFn>(regPtr);

    if (result != 0 || !reg)
    {
        Logger::Log::Warning("Failed to resolve CoreBridge.RegisterDiagnosticsSink: {}", result);
        return false;
    }

    int32 r = reg((void*)&Core_OnCompilationEvent, (void*)&Core_OnReloadEvent, nullptr);
    if (r != 0)
    {
        Logger::Log::Warning("RegisterDiagnosticsSink returned error: {}", r);
        return false;
    }

#ifdef _DEBUG
    // Early test hook: bypass initialization requirements and force a result
    {
        int32_t forced = g_CoreBridgeInitOverride.load();
        if (forced != (std::numeric_limits<int32_t>::max)())
        {
            m_LastCoreBridgeInitResult = forced;
            return false;
        }
    }
#endif

    Logger::Log::Info("Diagnostics sink registered with CoreBridge");
    return true;
#else
    return false;
#endif
}

bool CoreCLRHost::InitializeCoreBridge(const std::filesystem::path& coreBridgeAssemblyPath)
{
#if !defined(NETHOST_AVAILABLE)
    (void)coreBridgeAssemblyPath;
    Logger::Log::Warning("CoreBridge.Initialize requested, but .NET hosting is not available on this platform (NETHOST_AVAILABLE not defined)");
    m_LastCoreBridgeInitResult = -1;
    return false;
#else
#ifdef _DEBUG
    // Test hook: allow forcing a specific CoreBridge.Initialize result code early
    {
        int32_t forced = g_CoreBridgeInitOverride.load();
        if (forced != (std::numeric_limits<int32_t>::max)())
        {
            m_LastCoreBridgeInitResult = forced;
            return false;
        }
    }
#endif

    if (!m_Initialized || !m_LoadAssemblyAndGetFunctionPointer)
    {
        Logger::Log::Error("CoreCLRHost not initialized or load function unavailable");
        return false;
    }

    // Idempotency: avoid re-invoking CoreBridge.Initialize multiple times
    if (m_CoreBridgeInitialized)
    {
        // Preserve the original successful initialization; record the path if empty
        if (m_CoreBridgeAssemblyPath.empty())
        {
            m_CoreBridgeAssemblyPath = coreBridgeAssemblyPath;
        }
        m_LastCoreBridgeInitResult = 0;
        return true;
    }

    // Resolve CoreBridge.Initialize, tolerating alternate assembly names some build
    // pipelines produce (the assembly-qualified type name must match the real name).
    using BridgeInitFn = int32(CORECLR_DELEGATE_CALLTYPE*)();
    BridgeInitFn init = nullptr;

    const std::filesystem::path asmPathFs = std::filesystem::absolute(coreBridgeAssemblyPath);

#ifdef PLATFORM_WINDOWS
    using HostString = std::wstring;
#else
    using HostString = std::string;
#endif
    HostString chosenTypeName;

    Host_DebugWrite("InitializeCoreBridge: resolving CoreBridge.Initialize");
    int32 result = -1;
    for (const auto& asmName : GetCoreBridgeAssemblyNameCandidates(asmPathFs))
    {
        const std::string typeNameUtf8 = std::string("GameEngine.CoreBridge.CoreBridge, ") + asmName;
#ifdef PLATFORM_WINDOWS
        HostString typeName(typeNameUtf8.begin(), typeNameUtf8.end());
#else
        const HostString& typeName = typeNameUtf8;
#endif
        void* tmp = nullptr;
        result = ResolveManagedUco(asmPathFs, typeName.c_str(), GE_HOST_STR("Initialize"), &tmp);
        if (result == 0 && tmp != nullptr)
        {
            init = reinterpret_cast<BridgeInitFn>(tmp);
            chosenTypeName = typeName;
            break;
        }
    }

    if (result != 0 || !init)
    {
        Host_DebugWrite("InitializeCoreBridge: failed to resolve CoreBridge.Initialize");
        Logger::Log::Error("Failed to get CoreBridge.Initialize function pointer: {} (0x{:X})", result, (uint32)result);
        m_LastCoreBridgeInitResult = -1; // resolution failure (not a managed return)
        return false;
    }

    Host_DebugWrite("InitializeCoreBridge: invoking CoreBridge.Initialize");
    int32 initResult = init();
    Host_DebugWrite("InitializeCoreBridge: CoreBridge.Initialize returned");
    m_LastCoreBridgeInitResult = initResult;
    if (initResult != 0)
    {
        Logger::Log::Error("CoreBridge.Initialize returned error: {}", initResult);
        return false;
    }
    // Success
    m_LastCoreBridgeInitResult = 0;

    // Store the resolved CoreBridge path for later delegate lookups
    m_CoreBridgeAssemblyPath = coreBridgeAssemblyPath;

    // Mark initialized to prevent duplicate Initialize calls
    m_CoreBridgeInitialized = true;

    // Inform CoreBridge of the exact HotReload.dll directory to avoid env vars and stale copies
    {
        using SetHrmDirFn = int32(CORECLR_DELEGATE_CALLTYPE*)(void*, uint32);
        void* tmpSet = nullptr;
        (void)ResolveManagedUco(asmPathFs, chosenTypeName.c_str(), GE_HOST_STR("SetHotReloadDirectory"), &tmpSet);
        if (tmpSet)
        {
            auto setDir = reinterpret_cast<SetHrmDirFn>(tmpSet);
            namespace SP = GameEngine::ScriptingPaths;
            std::filesystem::path hrmPath = SP::ResolveHotReloadDll();
            if (!hrmPath.empty() && std::filesystem::exists(hrmPath))
            {
                std::filesystem::path hrmDir = std::filesystem::absolute(hrmPath).parent_path();
                std::string hrmDirUtf8 = hrmDir.string();
                Host_DebugWrite("InitializeCoreBridge: calling SetHotReloadDirectory");
                (void)setDir((void*)hrmDirUtf8.c_str(), (uint32)hrmDirUtf8.size());
            }
        }
    }

    // Register this process's engine instance (the native library managed code binds to)
    // using the CoreBridge UCO.
    //
    // IMPORTANT: This must complete before any InitializeOnLoad methods execute so that
    // Console redirection (EngineLogWriter) and the engine instance binding are fully
    // wired when user scripts log during IoL. To guarantee this, we perform registration
    // synchronously the first time CoreBridge is initialized instead of relying on a
    // background thread with timing-sensitive behaviour.
    {
        using RegisterInstanceFn = int32(CORECLR_DELEGATE_CALLTYPE*)(void* path, uint32 pathLen, uint32 abiOverride);
        void* tmpRegister = nullptr;
        (void)ResolveManagedUco(asmPathFs, chosenTypeName.c_str(), GE_HOST_STR("RegisterEngineInstance"), &tmpRegister);
        auto reg = reinterpret_cast<RegisterInstanceFn>(tmpRegister);

        if (reg && !m_EngineInstanceRegistered)
        {
            m_EngineInstanceRegistered = true;

            std::string nativePathUtf8;
            if (!m_NativePathOverride.empty())
            {
                nativePathUtf8 = m_NativePathOverride;
            }
            else
            {
                namespace SP = GameEngine::ScriptingPaths;
                std::filesystem::path nativePathFs = SP::ResolveNativeLibraryPath();
                if (nativePathFs.empty())
                {
                    // No probed directory holds the library: register the directory the resolver
                    // settles on (the executable directory); the managed loader then probes its
                    // own candidates.
                    nativePathFs = SP::ResolveNativeLibraryDirectory();
                }
                nativePathUtf8 = std::filesystem::absolute(nativePathFs).string();
            }

            Host_DebugWrite("CoreCLRHost: registering engine instance synchronously");
            int32 rc = reg((void*)nativePathUtf8.c_str(), (uint32)nativePathUtf8.size(), 0);
            Logger::Log::Debug("CoreCLRHost: RegisterEngineInstance rc={} path={}", (int)rc, nativePathUtf8);

            if (rc != 0)
            {
                Logger::Log::Warning("CoreBridge engine instance registration returned non-zero (rc={})", (int)rc);
            }
        }

        if (!reg)
        {
            Logger::Log::Warning("CoreBridge UCO RegisterEngineInstance missing; managed code has no engine instance binding");
        }
    }

    // Hand managed HotReloadManager a main-thread pump so [InitializeOnLoad] (and
    // other queued managed work) runs on the engine main thread instead of a
    // thread-pool thread racing the renderer (B5).
    InstallMainThreadPump(asmPathFs, chosenTypeName.c_str());

    Logger::Log::Info("CoreBridge initialized and callbacks registered");
    return true;
#endif // NETHOST_AVAILABLE
}

void CoreCLRHost::SetMainThreadDispatcher(std::function<void(std::function<void()>)> dispatcher)
{
    std::lock_guard<std::mutex> lock(m_MainThreadDispatcherMutex);
    m_MainThreadDispatcher = std::move(dispatcher);
}

void CoreCLRHost::InstallMainThreadPump(const std::filesystem::path& bridgePath, const char_t* typeName)
{
#if !defined(NETHOST_AVAILABLE)
    (void)bridgePath;
    (void)typeName;
#else
    {
        std::lock_guard<std::mutex> lock(m_MainThreadDispatcherMutex);
        if (!m_MainThreadDispatcher)
        {
            Logger::Log::Debug("CoreCLRHost: no main-thread dispatcher set; managed IoL stays on the thread pool");
            return;
        }
    }

    using SetPumpFn = int32 (CORECLR_DELEGATE_CALLTYPE*)(void*, void*);
    void* tmpSet = nullptr;
    (void)ResolveManagedUco(bridgePath, typeName, GE_HOST_STR("SetMainThreadPumpCallback"), &tmpSet);
    void* tmpPump = nullptr;
    (void)ResolveManagedUco(bridgePath, typeName, GE_HOST_STR("PumpHotReloadMainThread"), &tmpPump);
    if (!tmpSet || !tmpPump)
    {
        Logger::Log::Warning("CoreCLRHost: main-thread pump exports unavailable; managed IoL stays on the thread pool");
        return;
    }

    m_PumpMainThreadFn = reinterpret_cast<PumpMainThreadFn>(tmpPump);
    auto setPump = reinterpret_cast<SetPumpFn>(tmpSet);
    int32 rc = setPump(reinterpret_cast<void*>(&CoreCLRHost::MainThreadPumpThunk), this);
    if (rc != 0)
    {
        Logger::Log::Warning("CoreCLRHost: SetMainThreadPumpCallback returned {}", rc);
        m_PumpMainThreadFn = nullptr;
        return;
    }
    Logger::Log::Info("CoreCLRHost: managed main-thread pump registered (IoL runs on the engine main thread)");
#endif
}

void GE_CDECL CoreCLRHost::MainThreadPumpThunk(void* user)
{
    auto* self = static_cast<CoreCLRHost*>(user);
    if (!self)
        return;
    std::function<void(std::function<void()>)> dispatcher;
    {
        std::lock_guard<std::mutex> lock(self->m_MainThreadDispatcherMutex);
        dispatcher = self->m_MainThreadDispatcher;
    }
    auto pumpFn = self->m_PumpMainThreadFn;
    if (!dispatcher || !pumpFn)
        return;
    dispatcher([pumpFn]()
               { (void)pumpFn(); });
}

bool CoreCLRHost::PreloadAndSwapFromPath(const std::filesystem::path& assemblyPath)
{
#if !defined(NETHOST_AVAILABLE)
    (void)assemblyPath;
    Logger::Log::Warning("PreloadAndSwapFromPath is not available because .NET hosting is not enabled on this platform (NETHOST_AVAILABLE not defined)");
    return false;
#else
    if (!m_Initialized)
    {
        Logger::Log::Error("CoreCLRHost not initialized");
        return false;
    }
    if (!m_LoadAssemblyAndGetFunctionPointer)
    {
        Logger::Log::Error("Load assembly function pointer not available");
        return false;
    }

    // Ensure CoreBridge is initialized (idempotent)
    std::filesystem::path bridgePath = ResolveCoreBridgePath(m_CoreBridgeAssemblyPath);
    if (bridgePath.empty() || !std::filesystem::exists(bridgePath))
    {
        Logger::Log::Error("CoreBridge.dll not found via PathResolver (Preload/Swap)");
        return false;
    }
    if (!InitializeCoreBridge(bridgePath))
    {
        Logger::Log::Warning("CoreBridge.Initialize failed or already initialized; continuing");
    }

    // Resolve CoreBridge.PreloadAssemblyContext and CoreBridge.SwapPreloadedContext.
    // These unmanaged-callable methods are implemented via [UnmanagedCallersOnly] and
    // are callable with the default C calling convention on all supported platforms.
    using PreloadFn = int32(CORECLR_DELEGATE_CALLTYPE*)(void*, uint32);
    using SwapFn = int32(CORECLR_DELEGATE_CALLTYPE*)();

    PreloadFn preFn = nullptr;
    SwapFn swapFn = nullptr;

    const std::filesystem::path asmPathFs = std::filesystem::absolute(bridgePath);
#ifdef PLATFORM_WINDOWS
    using HostString = std::wstring;
#else
    using HostString = std::string;
#endif

    void* tmp = nullptr;
    int32 hrPre = -1;
    HostString chosenTypeName;
    for (const auto& asmName : GetCoreBridgeAssemblyNameCandidates(asmPathFs))
    {
        const std::string typeNameUtf8 = std::string("GameEngine.CoreBridge.CoreBridge, ") + asmName;
#ifdef PLATFORM_WINDOWS
        HostString typeName(typeNameUtf8.begin(), typeNameUtf8.end());
#else
        const HostString& typeName = typeNameUtf8;
#endif
        tmp = nullptr;
        hrPre = ResolveManagedUco(asmPathFs, typeName.c_str(), GE_HOST_STR("PreloadAssemblyContext"), &tmp);
        if (hrPre == 0 && tmp != nullptr)
        {
            chosenTypeName = typeName;
            break;
        }
    }
    if (hrPre != 0 || tmp == nullptr)
    {
        Logger::Log::Error("Failed to resolve CoreBridge.PreloadAssemblyContext: {}", hrPre);
        m_LastManagedErrorCode = (hrPre != 0) ? hrPre : -1;
        return false;
    }
    preFn = reinterpret_cast<PreloadFn>(tmp);

    tmp = nullptr;
    int32 hrSwap = ResolveManagedUco(asmPathFs, chosenTypeName.c_str(), GE_HOST_STR("SwapPreloadedContext"), &tmp);
    if (hrSwap != 0 || tmp == nullptr)
    {
        Logger::Log::Error("Failed to resolve CoreBridge.SwapPreloadedContext: {}", hrSwap);
        m_LastManagedErrorCode = (hrSwap != 0) ? hrSwap : -1;
        return false;
    }
    swapFn = reinterpret_cast<SwapFn>(tmp);

    // Call Preload with UTF-8 path to the compiled scripts assembly.
    std::string pathUtf8 = assemblyPath.string();
    int32 preRc = -1;
    try
    {
        preRc = preFn((void*)pathUtf8.c_str(), (uint32)pathUtf8.size());
    }
    catch (const std::exception& ex)
    {
        Logger::Log::Error("PreloadAssemblyContext threw C++ exception: {}", ex.what());
        m_LastManagedErrorCode = -1;
        return false;
    }
    catch (...)
    {
        // NOTE: Some environments can surface HRESULT-based exceptions from the hosting/AMSI pipeline.
        // They are typically first-chance and handled internally, but if one escapes here, fail safely.
        Logger::Log::Error("PreloadAssemblyContext threw unknown C++ exception.");
        m_LastManagedErrorCode = -1;
        return false;
    }
    if (preRc != 0)
    {
        Logger::Log::Error("PreloadAssemblyContext failed: {} (path={})", preRc, assemblyPath.string());
        m_LastManagedErrorCode = preRc;
        return false;
    }

    int32 swapRc = -1;
    try
    {
        swapRc = swapFn();
    }
    catch (const std::exception& ex)
    {
        Logger::Log::Error("SwapPreloadedContext threw C++ exception: {}", ex.what());
        m_LastManagedErrorCode = -1;
        return false;
    }
    catch (...)
    {
        Logger::Log::Error("SwapPreloadedContext threw unknown C++ exception.");
        m_LastManagedErrorCode = -1;
        return false;
    }
    if (swapRc != 0)
    {
        Logger::Log::Error("SwapPreloadedContext failed: {}", swapRc);
        m_LastManagedErrorCode = swapRc;
        return false;
    }

    m_LastManagedErrorCode = 0;
    return true;
#endif
}

int32 CoreCLRHost::ResolveManagedUco(const std::filesystem::path& assemblyPath,
                                     const char_t* typeName,
                                     const char_t* methodName,
                                     void** outFn)
{
#if !defined(NETHOST_AVAILABLE)
    (void)assemblyPath;
    (void)typeName;
    (void)methodName;
    (void)outFn;
    return -1;
#else
    if (!outFn)
        return -1;
    *outFn = nullptr;

    auto loadFn = m_LoadAssemblyIntoDefault ? m_LoadAssemblyIntoDefault : g_loadAssemblyIntoDefault;
    auto getFn = m_GetFunctionPointer ? m_GetFunctionPointer : g_getFunctionPointer;
    const std::filesystem::path absPath = std::filesystem::absolute(assemblyPath);
#ifdef PLATFORM_WINDOWS
    const std::wstring pathNative = absPath.wstring();
#else
    const std::string pathNative = absPath.string();
#endif

    if (loadFn && getFn)
    {
        // Single statics universe (B4): push the assembly into the Default ALC once,
        // then bind the export there. Managed dependencies resolve into Default via
        // CoreBridge's Default-ALC Resolving handler, so generated [ModuleInitializer]
        // registrations and the code that consumes them share one set of statics.
        const std::string pathKey = absPath.string();
        bool needsLoad = false;
        {
            std::lock_guard<std::mutex> lock(g_defaultLoadedMutex);
            needsLoad = g_defaultLoadedAssemblies.insert(pathKey).second;
        }
        if (needsLoad)
        {
            int32 loadRc = loadFn(pathNative.c_str(), nullptr, nullptr);
            if (loadRc != 0)
            {
                // A same-identity assembly loaded from a different path reports failure
                // here but may still resolve by name; let get_function_pointer decide.
                Logger::Log::Debug("CoreCLRHost: hdt_load_assembly rc=0x{:X} for '{}' (continuing; type may already resolve in Default)",
                                   (uint32)loadRc, pathKey);
            }
        }
        int32 rc = getFn(typeName, methodName, UNMANAGEDCALLERSONLY_METHOD, nullptr, nullptr, outFn);
        if (rc != 0 || *outFn == nullptr)
        {
            // Allow a later call to retry the Default-ALC load.
            std::lock_guard<std::mutex> lock(g_defaultLoadedMutex);
            g_defaultLoadedAssemblies.erase(pathKey);
        }
        return rc;
    }

    // Legacy fallback (runtimes without the Default-ALC delegates): isolated component
    // context per assembly path — statics are duplicated across universes.
    auto legacy = m_LoadAssemblyAndGetFunctionPointer ? m_LoadAssemblyAndGetFunctionPointer : g_loadAssemblyAndGetFunctionPointer;
    if (!legacy)
        return -1;
    return legacy(pathNative.c_str(), typeName, methodName, UNMANAGEDCALLERSONLY_METHOD, nullptr, outFn);
#endif
}

void* CoreCLRHost::GetCoreBridgeExport(const char_t* methodName)
{
#if !defined(NETHOST_AVAILABLE)
    (void)methodName;
    return nullptr;
#else
    {
        std::lock_guard<std::mutex> lock(m_BridgeExportCacheMutex);
        auto it = m_BridgeExportsByName.find(methodName);
        if (it != m_BridgeExportsByName.end())
            return it->second;
    }

    if (!m_Initialized || !m_LoadAssemblyAndGetFunctionPointer)
        return nullptr;

    std::filesystem::path bridgePath = ResolveCoreBridgePath(m_CoreBridgeAssemblyPath);
    if (bridgePath.empty() || !std::filesystem::exists(bridgePath))
    {
        Logger::Log::Error("CoreBridge.dll not found via PathResolver (resolving CoreBridge export)");
        return nullptr;
    }

    void* fn = nullptr;
    int32 hr = ResolveManagedUco(bridgePath, GE_HOST_STR("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge"), methodName, &fn);
    if (hr != 0 || fn == nullptr)
        return nullptr;

    std::lock_guard<std::mutex> lock(m_BridgeExportCacheMutex);
    m_BridgeExportsByName.emplace(methodName, fn);
    return fn;
#endif
}

int32 CoreCLRHost::CallUserScriptsMethod(const String& methodName)
{
#if defined(NETHOST_AVAILABLE)
    if (!m_Initialized || !m_LoadAssemblyAndGetFunctionPointer)
        return -1;
    // Resolve CoreBridge.CallUserScriptsMethod
    using call_fn = int32 (CORECLR_DELEGATE_CALLTYPE*)(void*, uint32);
    call_fn fn = nullptr;
    std::filesystem::path bridgePath = ResolveCoreBridgePath(m_CoreBridgeAssemblyPath);
    if (!std::filesystem::exists(bridgePath))
    {
        Logger::Log::Error("CoreBridge.dll not found via PathResolver (CallUserScriptsMethod)");
        return -1;
    }
    void* tmp = nullptr;
    int32 hr = ResolveManagedUco(bridgePath,
                                 GE_HOST_STR("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge"),
                                 GE_HOST_STR("CallUserScriptsMethod"),
                                 &tmp);
    if (hr != 0 || tmp == nullptr)
        return -1;
    fn = reinterpret_cast<call_fn>(tmp);
    std::string nameUtf8 = methodName.c_str();
    return fn((void*)nameUtf8.c_str(), (uint32)nameUtf8.size());
#else
    (void)methodName;
    return -1;
#endif
}

int32 CoreCLRHost::InvokeInDomain(uint64 domain, const char* methodNameUtf8, uint32 methodNameLen, int32* outResult)
{
#if defined(NETHOST_AVAILABLE)
    if (!m_Initialized || !m_LoadAssemblyAndGetFunctionPointer)
        return -1;
    if (!outResult || !methodNameUtf8 || methodNameLen == 0)
        return -2;

    // Compute safe pass length and a string key for caching
    uint32 safeLen = methodNameUtf8 ? (uint32)strlen(methodNameUtf8) : 0u;
    uint32 passLen = (safeLen > 0u) ? (std::min)(methodNameLen, safeLen) : methodNameLen;
    std::string nameKey = (methodNameUtf8 && passLen > 0u) ? std::string(methodNameUtf8, passLen) : std::string();

    // 1) Try cached token fast path
    uint64 token = 0ULL;
    {
        std::lock_guard<std::mutex> _lock(m_TokenCacheMutex);
        auto itDom = m_TokenCache.find(domain);
        if (itDom != m_TokenCache.end())
        {
            auto itName = itDom->second.find(nameKey);
            if (itName != itDom->second.end())
                token = itName->second;
        }
    }
    if (token != 0ULL)
    {
        int32 rcTok = InvokeByToken(domain, token, outResult);
        if (rcTok >= 0)
            return rcTok; // success via token
    }

    // 2) Query token then invoke by token; cache on success
    uint64 newToken = 0ULL;
    int32 qrc = QueryExport(domain, methodNameUtf8, passLen, &newToken);
    if (qrc >= 0 && newToken != 0ULL)
    {
        {
            std::lock_guard<std::mutex> _lock(m_TokenCacheMutex);
            m_TokenCache[domain][nameKey] = newToken;
            m_TokenReverseCache[domain][newToken] = nameKey;
        }
        int32 rcTok = InvokeByToken(domain, newToken, outResult);
        if (rcTok >= 0)
            return rcTok; // success via token after query
    }

    // 3) Fallback to name-based invoke (slow path)
    using invoke_fn = int32 (CORECLR_DELEGATE_CALLTYPE*)(uint64, void*, uint32, int32*);

    // Optional diagnostics: dump HRM state only if GE_DUMP_HRM=1
    if (std::getenv("GE_DUMP_HRM") && std::strcmp(std::getenv("GE_DUMP_HRM"), "0") != 0)
    {
        using dump_fn = int32 (CORECLR_DELEGATE_CALLTYPE*)();
        if (void* tmpDump = GetCoreBridgeExport(GE_HOST_STR("DebugDumpHotReloadState")))
        {
            reinterpret_cast<dump_fn>(tmpDump)();
        }
    }

    void* tmp = GetCoreBridgeExport(GE_HOST_STR("InvokeInDomain"));
    if (tmp == nullptr)
    {
        Logger::Log::Error("CoreBridge.InvokeInDomain function pointer resolve failed");
        return -3;
    }
    invoke_fn fn = reinterpret_cast<invoke_fn>(tmp);
    Logger::Log::Trace("CoreCLRHost: name-based InvokeInDomain domain={} method='{}'", domain, nameKey);
    int32 ret = fn(domain, (void*)methodNameUtf8, passLen, outResult);
    Logger::Log::Trace("CoreCLRHost: InvokeInDomain rc={} outResult={}", ret, outResult ? *outResult : 0);
    return ret;
#else
    (void)domain;
    (void)methodNameUtf8;
    (void)methodNameLen;
    (void)outResult;
    return -1;
#endif
}

int32 CoreCLRHost::QueryExport(uint64 domain, const char* methodNameUtf8, uint32 methodNameLen, uint64* outToken)
{
#if defined(NETHOST_AVAILABLE)
    if (!m_Initialized || !m_LoadAssemblyAndGetFunctionPointer)
        return -1;
    if (!outToken || !methodNameUtf8 || methodNameLen == 0)
        return -2;
    using query_fn = int32 (CORECLR_DELEGATE_CALLTYPE*)(uint64, void*, uint32, uint64*);
    void* tmp = GetCoreBridgeExport(GE_HOST_STR("QueryExport"));
    if (tmp == nullptr)
    {
        Logger::Log::Error("CoreBridge.QueryExport function pointer resolve failed");
        return -3;
    }

    query_fn fn = reinterpret_cast<query_fn>(tmp);
    int32 rc = fn(domain, (void*)methodNameUtf8, methodNameLen, outToken);
    Logger::Log::Trace("CoreCLRHost: QueryExport rc={} outToken={}", rc, outToken ? *outToken : 0ULL);
    if (rc == 0 && outToken && *outToken != 0ULL)
    {
        // Cache name<->token for repair path
        const uint32 passLen = methodNameLen;
        const char* p = methodNameUtf8;
        uint32 n = 0;
        for (; n < passLen; ++n)
        {
            if (p[n] == '\0')
                break;
        }
        const uint32 effectiveLen = (passLen > 0) ? (n > 0 ? n : passLen) : 0u;
        std::string nameKey = (p && effectiveLen > 0) ? std::string(p, effectiveLen) : std::string();
        if (!nameKey.empty())
        {
            std::lock_guard<std::mutex> _lock(m_TokenCacheMutex);
            m_TokenCache[domain][nameKey] = *outToken;
            m_TokenReverseCache[domain][*outToken] = nameKey;
        }
    }
    return rc;
#else
    (void)domain;
    (void)methodNameUtf8;
    (void)methodNameLen;
    (void)outToken;
    return -1;
#endif
}

int32 CoreCLRHost::InvokeByToken(uint64 domain, uint64 token, int32* outResult)
{
#if defined(NETHOST_AVAILABLE)
    if (!m_Initialized || !m_LoadAssemblyAndGetFunctionPointer)
        return -1;
    if (!outResult)
        return -2;
    using invoke_by_token_fn = int32 (CORECLR_DELEGATE_CALLTYPE*)(uint64, uint64, int32*);
    void* tmp = GetCoreBridgeExport(GE_HOST_STR("InvokeByToken"));
    if (tmp == nullptr)
        return -3;
    invoke_by_token_fn fn = reinterpret_cast<invoke_by_token_fn>(tmp);
    int32 rc = fn(domain, token, outResult);
    Logger::Log::Trace("CoreCLRHost: InvokeByToken domain={} token={} rc={} out={}", domain, token, rc, outResult ? *outResult : 0);
    if (rc < 0)
    {
        // Attempt lightweight repair: look up name by token and rebind via QueryExport
        std::string nameFromToken;
        {
            std::lock_guard<std::mutex> _lock(m_TokenCacheMutex);
            auto itDom = m_TokenReverseCache.find(domain);
            if (itDom != m_TokenReverseCache.end())
            {
                auto itName = itDom->second.find(token);
                if (itName != itDom->second.end())
                    nameFromToken = itName->second;
            }
        }
        if (!nameFromToken.empty())
        {
            uint64 newTok = 0ULL;
            int32 qrc = QueryExport(domain, nameFromToken.c_str(), (uint32)nameFromToken.size(), &newTok);
            if (qrc == 0 && newTok != 0ULL)
            {
                // Update reverse cache
                {
                    std::lock_guard<std::mutex> _lock(m_TokenCacheMutex);
                    m_TokenReverseCache[domain][newTok] = nameFromToken;
                }
                Logger::Log::Debug("CoreCLRHost: repaired token via QueryExport (old={} new={})", token, newTok);
                rc = fn(domain, newTok, outResult);
                Logger::Log::Debug("CoreCLRHost: InvokeByToken (repaired) rc={} out={}", rc, outResult ? *outResult : 0);
            }
        }
    }
    return rc;
#else
    (void)domain;
    (void)token;
    (void)outResult;
    return -1;
#endif
}

int32 CoreCLRHost::UnloadDomain(uint64 domain)
{
#if defined(NETHOST_AVAILABLE)
    if (!m_Initialized || !m_LoadAssemblyAndGetFunctionPointer)
        return -1;
    using unload_domain_fn = int32 (CORECLR_DELEGATE_CALLTYPE*)(uint64);
    void* tmp = GetCoreBridgeExport(GE_HOST_STR("UnloadDomain"));
    if (tmp == nullptr)
        return -3;
    unload_domain_fn fn = reinterpret_cast<unload_domain_fn>(tmp);
    int32 rc = fn(domain);
    if (rc >= 0)
    {
        std::lock_guard<std::mutex> _lock(m_TokenCacheMutex);
        m_TokenCache.erase(domain);
        g_dbgUnloadDomainCount.fetch_add(1, std::memory_order_relaxed);
    }
    return rc;
#else
    (void)domain;
    return -1;
#endif
}

int32 CoreCLRHost::AcceptPackage(const GE_PackageEntry* entries,
                                 uint32 entryCount,
                                 uint32 flags,
                                 uint64* outDomain)
{
#if defined(NETHOST_AVAILABLE)
    if (!m_Initialized || !m_LoadAssemblyAndGetFunctionPointer)
        return -1;
    if (!entries || entryCount == 0 || !outDomain)
        return -2;
    using accept_pkg_fn = int32 (CORECLR_DELEGATE_CALLTYPE*)(const GE_PackageEntry*, uint32, uint32, uint64*);
    using accept_pkg_packed_fn = long long (CORECLR_DELEGATE_CALLTYPE*)(const GE_PackageEntry*, uint32, uint32);

    // In Debug, prefer the packed variant to avoid out-params across UCO.
#ifdef _DEBUG
    // 1) Try AcceptAssembly_Packed first to avoid any struct pointer marshalling across UCO
    if (void* tmpAsmPacked = GetCoreBridgeExport(GE_HOST_STR("AcceptAssembly_Packed")))
    {
        using accept_asm_packed_fn = long long (CORECLR_DELEGATE_CALLTYPE*)(const void*, uint32, uint32);
        auto fnAsmPacked = reinterpret_cast<accept_asm_packed_fn>(tmpAsmPacked);
        const void* asmPtr = (const void*)entries[0].assembly.data;
        uint32 asmLen = (uint32)entries[0].assembly.length;
        long long packed = fnAsmPacked(asmPtr, asmLen, flags);
        int16 rc16 = (int16)((packed >> 48) & 0xFFFF);
        *outDomain = (uint64)(packed & 0x0000FFFFFFFFFFFFULL);
        Logger::Log::Debug("CoreCLRHost: AcceptAssembly_Packed rc={} outDomain={}", (int)rc16, *outDomain);
        return (int32)rc16;
    }
    // 2) Fallback: AcceptPackage_Packed with struct pointer
    if (void* tmpPacked = GetCoreBridgeExport(GE_HOST_STR("AcceptPackage_Packed")))
    {
        auto fnPacked = reinterpret_cast<accept_pkg_packed_fn>(tmpPacked);
        long long packed = fnPacked(entries, entryCount, flags);
        int16 rc16 = (int16)((packed >> 48) & 0xFFFF);
        *outDomain = (uint64)(packed & 0x0000FFFFFFFFFFFFULL);
        Logger::Log::Debug("CoreCLRHost: AcceptPackage_Packed rc={} outDomain={}", (int)rc16, *outDomain);
        return (int32)rc16;
    }
#endif

    // Fallback to original AcceptPackage with out-param
    void* tmp = GetCoreBridgeExport(GE_HOST_STR("AcceptPackage"));
    if (tmp == nullptr)
    {
        // CoreBridge may not be initialized yet on this path; initialize it and retry once.
        std::filesystem::path bridgePath = ResolveCoreBridgePath(m_CoreBridgeAssemblyPath);
        if (!bridgePath.empty() && std::filesystem::exists(bridgePath))
        {
            (void)InitializeCoreBridge(bridgePath);
            tmp = GetCoreBridgeExport(GE_HOST_STR("AcceptPackage"));
        }
        if (tmp == nullptr)
        {
            Logger::Log::Error("CoreBridge.AcceptPackage function pointer resolve failed");
            return -3;
        }
    }
    auto fn = reinterpret_cast<accept_pkg_fn>(tmp);
    int32 rc = fn(entries, entryCount, flags, outDomain);
    Logger::Log::Debug("CoreCLRHost: AcceptPackage rc={} outDomain={}", rc, outDomain ? *outDomain : 0ULL);
    return rc;
#else
    (void)entries;
    (void)entryCount;
    (void)flags;
    (void)outDomain;
    return -1;
#endif
}

uint64 CoreCLRHost::GetCurrentRuntimeDomainId()
{
#if defined(NETHOST_AVAILABLE)
    if (!m_Initialized || !m_LoadAssemblyAndGetFunctionPointer)
        return 0ULL;
    using get_domain_id_fn = uint64 (CORECLR_DELEGATE_CALLTYPE*)();
    void* tmp = GetCoreBridgeExport(GE_HOST_STR("GetCurrentRuntimeDomainId"));
    if (tmp == nullptr)
    {
        Logger::Log::Error("CoreBridge.GetCurrentRuntimeDomainId function pointer resolve failed");
        return 0ULL;
    }
    return reinterpret_cast<get_domain_id_fn>(tmp)();
#else
    return 0ULL;
#endif
}

void* CoreCLRHost::GetManagedFunction(const String& assemblyName,
                                      const String& typeName,
                                      const String& methodName)
{
#if !defined(NETHOST_AVAILABLE)
    (void)assemblyName;
    (void)typeName;
    (void)methodName;
    return nullptr;
#else
    if (!m_Initialized || !m_LoadAssemblyAndGetFunctionPointer)
        return nullptr;

    // Resolve CoreBridge path; allow explicit assembly path override if provided
    std::filesystem::path bridgePath = ResolveCoreBridgePath(m_CoreBridgeAssemblyPath);
    if (!assemblyName.empty())
    {
        std::filesystem::path cand = assemblyName;
        if (std::filesystem::exists(cand))
            bridgePath = cand;
    }
    if (!std::filesystem::exists(bridgePath))
        return nullptr;

#ifdef PLATFORM_WINDOWS
    std::wstring typeW = typeName.empty()
                             ? std::wstring(L"GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge")
                             : std::wstring(typeName.begin(), typeName.end());
    std::wstring methodW = std::wstring(methodName.begin(), methodName.end());
    const char_t* typeNamePtr = typeW.c_str();
    const char_t* methodNamePtr = methodW.c_str();
#else
    std::string typeNarrow = typeName.empty()
                                 ? std::string("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge")
                                 : std::string(typeName.c_str());
    std::string methodNarrow = std::string(methodName.c_str());
    const char_t* typeNamePtr = typeNarrow.c_str();
    const char_t* methodNamePtr = methodNarrow.c_str();
#endif

    void* fn = nullptr;
    int32 hr = ResolveManagedUco(bridgePath, typeNamePtr, methodNamePtr, &fn);
    if (hr != 0 || fn == nullptr)
        return nullptr;
    return fn;
#endif
}

String CoreCLRHost::GetRuntimeVersion() const
{
    // TODO: Implement runtime version retrieval
    return "Unknown";
}

bool CoreCLRHost::LoadHostfxr()
{
#if !defined(NETHOST_AVAILABLE)
    return false;
#else
    // Use nethost library to discover the correct hostfxr path (cross-platform)
    char_t hostfxrPath[1024];
    size_t bufferSize = sizeof(hostfxrPath) / sizeof(char_t);

    int result = get_hostfxr_path(hostfxrPath, &bufferSize, nullptr);
    if (result != 0)
    {
        Logger::Log::Error("Failed to get hostfxr path using nethost, error code: {}", result);
        Host_DebugWrite("LoadHostfxr: get_hostfxr_path failed");
        return false;
    }

    // Helper: platform-specific native library load & symbol lookup
    auto LoadNativeLibrary = [](const char_t* path) -> void*
    {
#ifdef PLATFORM_WINDOWS
        return (void*)LoadLibraryW(path);
#else
        return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
    };

    auto GetExport = [](void* handle, const char* name) -> void*
    {
#ifdef PLATFORM_WINDOWS
        return (void*)GetProcAddress((HMODULE)handle, name);
#else
        return dlsym(handle, name);
#endif
    };

    m_HostfxrLib = LoadNativeLibrary(hostfxrPath);
    if (!m_HostfxrLib)
    {
        Logger::Log::Error("Failed to load hostfxr library");
        return false;
    }

    m_InitForConfig = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
        GetExport(m_HostfxrLib, "hostfxr_initialize_for_runtime_config"));
    m_GetRuntimeDelegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
        GetExport(m_HostfxrLib, "hostfxr_get_runtime_delegate"));
    m_Close = reinterpret_cast<hostfxr_close_fn>(
        GetExport(m_HostfxrLib, "hostfxr_close"));
    m_SetErrorWriter = reinterpret_cast<hostfxr_set_error_writer_fn>(
        GetExport(m_HostfxrLib, "hostfxr_set_error_writer"));

    if (!m_InitForConfig || !m_GetRuntimeDelegate || !m_Close)
    {
        Logger::Log::Error("Failed to get hostfxr function pointers");
        return false;
    }

    if (m_SetErrorWriter)
    {
        m_SetErrorWriter([](const char_t* msg)
                         {
#ifdef PLATFORM_WINDOWS
                             Logger::Log::Error("[hostfxr] {}", Utf8FromWide(std::wstring(msg)));
#else
                             Logger::Log::Error("[hostfxr] {}", msg);
#endif
                         });
    }

    return true;
#endif
}

bool CoreCLRHost::InitializeHostfxr(const std::filesystem::path& runtimeConfigPath)
{
#if !defined(NETHOST_AVAILABLE)
    return false;
#else
    // Use cached process-wide context if available
    if (g_hostfxrContext)
    {
        m_HostfxrContext = g_hostfxrContext;
        return true;
    }

    if (!m_InitForConfig)
    {
        Host_DebugWrite("InitializeHostfxr: m_InitForConfig is null");
        return false;
    }

    // An empty path reaches here in stub builds that don't stage a runtimeconfig.json
    // next to the executable (e.g. EditorTests): ResolveScriptsRuntimeConfig() returns
    // empty and scripting is simply unavailable. std::filesystem::absolute throws on an
    // empty path, so treat it as "config not found" and let callers degrade gracefully.
    if (runtimeConfigPath.empty())
    {
        Host_DebugWrite("InitializeHostfxr: empty runtimeconfig path");
        Logger::Log::Warning("InitializeHostfxr: no runtimeconfig.json available; CLR hosting disabled");
        return false;
    }

    std::filesystem::path actualConfigPath = std::filesystem::absolute(runtimeConfigPath);
    if (!std::filesystem::exists(actualConfigPath))
    {
        Host_DebugWrite("InitializeHostfxr: runtimeconfig not found");
        Logger::Log::Error("InitializeHostfxr: runtimeconfig not found: {}", actualConfigPath.string());
        return false;
    }
    const auto configPath = actualConfigPath;

    // Try null parameters first (hostfxr can resolve defaults)
    int32 result = m_InitForConfig(configPath.c_str(), nullptr, &m_HostfxrContext);

    if (result != 0 || !m_HostfxrContext)
    {
        // Retry with explicit host_path
        hostfxr_initialize_parameters params{};
        params.size = sizeof(hostfxr_initialize_parameters);
        params.host_path = nullptr;
        params.dotnet_root = nullptr; // let hostfxr resolve using defaults/DOTNET_ROOT

        m_HostfxrContext = nullptr;
        result = m_InitForConfig(configPath.c_str(), &params, &m_HostfxrContext);
    }

    if (result < 0)
    {
        Host_DebugWrite("InitializeHostfxr: hostfxr_initialize_for_runtime_config failed");
        Logger::Log::Error("InitializeHostfxr: hostfxr_initialize_for_runtime_config failed: hr={} ctxNull={} cfg={}", result, (m_HostfxrContext == nullptr), std::filesystem::absolute(actualConfigPath).string());
        // hostfxr's FrameworkMissingFailure: no installed runtime satisfies the config.
        constexpr int32 kFrameworkMissingFailure = static_cast<int32>(0x80008096);
        if (result == kFrameworkMissingFailure)
            Logger::Log::Error("InitializeHostfxr: the engine hosts .NET 10 and no .NET 10 runtime is installed; "
                               "install the .NET 10 SDK (https://dotnet.microsoft.com/download/dotnet/10.0), "
                               "which includes the runtime, and restart");
        return false;
    }
    // Success: result can be 0 (new context) or >0 (already initialized, context may be null)
    return true;
#endif
}

bool CoreCLRHost::GetRuntimeDelegate()
{
#if !defined(NETHOST_AVAILABLE)
    return false;
#else
    // Use cached delegates if available
    if (g_loadAssemblyAndGetFunctionPointer && g_hostfxrContext)
    {
        m_LoadAssemblyAndGetFunctionPointer = g_loadAssemblyAndGetFunctionPointer;
        m_GetFunctionPointer = g_getFunctionPointer;
        m_LoadAssemblyIntoDefault = g_loadAssemblyIntoDefault;
        return true;
    }

    if (!m_GetRuntimeDelegate || !m_HostfxrContext)
    {
        return false;
    }

    void* delegate = nullptr;
    int32 result = m_GetRuntimeDelegate(m_HostfxrContext,
                                        hdt_load_assembly_and_get_function_pointer,
                                        &delegate);

    if (result != 0 || !delegate)
    {
        return false;
    }

    // hostfxr_get_runtime_delegate returns the delegate via a void* out parameter;
    // reinterpret_cast is required here because C++ does not permit static_cast
    // directly from void* to a function pointer type.
    m_LoadAssemblyAndGetFunctionPointer =
        reinterpret_cast<load_assembly_and_get_function_pointer_fn>(delegate);

    // Default-ALC binding pair (B4). Available since .NET 5 (hdt_get_function_pointer)
    // and .NET 8 (hdt_load_assembly); absence just re-enables the legacy fallback in
    // ResolveManagedUco, so failures here are non-fatal.
    void* getFnDelegate = nullptr;
    if (m_GetRuntimeDelegate(m_HostfxrContext, hdt_get_function_pointer, &getFnDelegate) == 0 && getFnDelegate)
    {
        m_GetFunctionPointer = reinterpret_cast<get_function_pointer_fn>(getFnDelegate);
    }
    void* loadAsmDelegate = nullptr;
    if (m_GetRuntimeDelegate(m_HostfxrContext, hdt_load_assembly, &loadAsmDelegate) == 0 && loadAsmDelegate)
    {
        m_LoadAssemblyIntoDefault = reinterpret_cast<load_assembly_fn>(loadAsmDelegate);
    }
    if (!m_GetFunctionPointer || !m_LoadAssemblyIntoDefault)
    {
        Logger::Log::Warning(
            "CoreCLRHost: Default-ALC hosting delegates unavailable (getFn={}, loadAsm={}); "
            "falling back to isolated component contexts — managed statics will be duplicated",
            (void*)m_GetFunctionPointer, (void*)m_LoadAssemblyIntoDefault);
    }

    return true;
#endif
}

#ifdef _DEBUG
int32 CoreCLRHost::ResetPerfCounters()
{
#ifdef PLATFORM_WINDOWS
    // Prefer process-wide cached delegate; fall back to instance pointer
    auto loadFn = g_loadAssemblyAndGetFunctionPointer;
    if (!loadFn)
        loadFn = m_LoadAssemblyAndGetFunctionPointer;
    if (!loadFn)
        return -1;
    using reset_perf_fn = int32(CORECLR_DELEGATE_CALLTYPE*)();
    reset_perf_fn fn = nullptr;
    std::filesystem::path bridgePath = ResolveCoreBridgePath(m_CoreBridgeAssemblyPath);
    if (!std::filesystem::exists(bridgePath))
        return -3;

    std::wstring typeW = L"GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge";
    std::wstring methodW = L"ResetPerfCounters";
    void* tmp = nullptr;
    int32 hr = ResolveManagedUco(bridgePath, typeW.c_str(), methodW.c_str(), &tmp);
    if (hr != 0 || tmp == nullptr)
        return -3;
    fn = reinterpret_cast<reset_perf_fn>(tmp);
    return fn();
#else
    return -1;
#endif
}
#endif

#ifdef _DEBUG
int32 CoreCLRHost::GetUnloadCounter()
{
#ifdef PLATFORM_WINDOWS
    // Prefer process-wide cached delegate; fall back to instance pointer
    auto loadFn = g_loadAssemblyAndGetFunctionPointer;
    if (!loadFn)
        loadFn = m_LoadAssemblyAndGetFunctionPointer;
    if (!loadFn)
        return (int32)g_dbgUnloadDomainCount.load(std::memory_order_relaxed);
    // Prefer native fallback if present
    int32 native = (int32)g_dbgUnloadDomainCount.load(std::memory_order_relaxed);
    if (native > 0)
        return native;

    // Try extended HRM metrics as authoritative source for total unloads
    using get_metrics_ex_fn = int32(CORECLR_DELEGATE_CALLTYPE*)(uint64_t*, uint64_t*, int32*, int32*, int32*);
    void* tmpEx = nullptr;
    std::filesystem::path bridgePath = ResolveCoreBridgePath(m_CoreBridgeAssemblyPath);
    if (!std::filesystem::exists(bridgePath))
        return native;

    std::wstring typeW = L"GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge";
    std::wstring methodExW = L"GetHotReloadMetricsEx";
    int32 hr = ResolveManagedUco(bridgePath, typeW.c_str(), methodExW.c_str(), &tmpEx);
    if (hr == 0 && tmpEx != nullptr)
    {
        auto fnEx = reinterpret_cast<get_metrics_ex_fn>(tmpEx);
        uint64_t lastCompile = 0, lastSwap = 0;
        int32 totalCompiles = 0, totalSwaps = 0, totalUnloads = 0;
        int32 rc = fnEx(&lastCompile, &lastSwap, &totalCompiles, &totalSwaps, &totalUnloads);
        if (rc == 0 && totalUnloads >= 0)
            return totalUnloads;
    }

    // Fallback to dedicated debug counter if available in CoreBridge
    using get_unload_fn = int32(CORECLR_DELEGATE_CALLTYPE*)();
    void* tmp = nullptr;
    std::wstring methodW = L"GetUnloadCounter";
    hr = ResolveManagedUco(bridgePath, typeW.c_str(), methodW.c_str(), &tmp);
    if (hr != 0 || tmp == nullptr)
        return native;
    auto fn = reinterpret_cast<get_unload_fn>(tmp);
    int32 val = fn();
    if (val < 0)
        return native;
    return val;
#else
    return -1;
#endif
}

int32 CoreCLRHost::ResetUnloadCounter()
{
#ifdef PLATFORM_WINDOWS
    // Prefer process-wide cached delegate; fall back to instance pointer
    auto loadFn = g_loadAssemblyAndGetFunctionPointer;
    if (!loadFn)
        loadFn = m_LoadAssemblyAndGetFunctionPointer;
    if (!loadFn)
    {
        g_dbgUnloadDomainCount.store(0, std::memory_order_relaxed);
        return 0;
    }
    using reset_unload_fn = int32(CORECLR_DELEGATE_CALLTYPE*)();
    reset_unload_fn fn = nullptr;
    std::filesystem::path bridgePath = ResolveCoreBridgePath(m_CoreBridgeAssemblyPath);
    if (!std::filesystem::exists(bridgePath))
    {
        g_dbgUnloadDomainCount.store(0, std::memory_order_relaxed);
        return 0;
    }

    std::wstring typeW = L"GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge";
    std::wstring methodW = L"ResetUnloadCounter";
    void* tmp = nullptr;
    int32 hr = ResolveManagedUco(bridgePath, typeW.c_str(), methodW.c_str(), &tmp);
    if (hr != 0 || tmp == nullptr)
    {
        g_dbgUnloadDomainCount.store(0, std::memory_order_relaxed);
        return 0;
    }
    fn = reinterpret_cast<reset_unload_fn>(tmp);
    g_dbgUnloadDomainCount.store(0, std::memory_order_relaxed);
    return fn();
#else
    return -1;
#endif
}

void CoreCLRHost::DebugBumpUnloadCounterNative()
{
#ifdef PLATFORM_WINDOWS
    g_dbgUnloadDomainCount.fetch_add(1, std::memory_order_relaxed);
#else
    // Non-Windows: counter not tracked; no-op
#endif
}

#endif

#ifdef _DEBUG
int32 CoreCLRHost::GetInvokeByTokenPath()
{
#ifdef PLATFORM_WINDOWS
    if (!m_Initialized || !m_LoadAssemblyAndGetFunctionPointer)
        return -1;
    typedef int32(__cdecl * get_path_fn)();
    get_path_fn fn = nullptr;
    std::filesystem::path bridgePath = ResolveCoreBridgePath(m_CoreBridgeAssemblyPath);
    if (!std::filesystem::exists(bridgePath))
        return -3;

    std::wstring typeW = L"GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge";
    std::wstring methodW = L"GetInvokeByTokenPath";
    void* tmp = nullptr;
    int32 hr = ResolveManagedUco(bridgePath, typeW.c_str(), methodW.c_str(), &tmp);
    if (hr != 0 || tmp == nullptr)
        return -3;
    fn = reinterpret_cast<get_path_fn>(tmp);
    return fn();

#else
    return -1;
#endif
}
#endif

#ifdef _DEBUG
int32 CoreCLRHost::GetPerfCounters(int32* outDelegateQuery, int32* outReflectionQuery, int32* outDelegateInvoke, int32* outReflectionInvoke)
{
#ifdef PLATFORM_WINDOWS
    // Use process-wide cached delegate to avoid touching instance state
    auto loadFn = g_loadAssemblyAndGetFunctionPointer;
    if (!loadFn)
        loadFn = m_LoadAssemblyAndGetFunctionPointer;
    if (!loadFn)
        return -1;
    if (!outDelegateQuery || !outReflectionQuery || !outDelegateInvoke || !outReflectionInvoke)
        return -2;
    using get_perf_fn = int32(CORECLR_DELEGATE_CALLTYPE*)(int32*, int32*, int32*, int32*);
    get_perf_fn fn = nullptr;
    // Resolve CoreBridge path without relying on instance field to avoid any 'this' deref
    std::filesystem::path bridgePath = ResolveCoreBridgePath(m_CoreBridgeAssemblyPath);
    if (!std::filesystem::exists(bridgePath))
        return -3;

    std::wstring typeW = L"GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge";
    // Prefer packed variant (no pointer marshalling) for extra stability; fallback to pointer-based API
    using get_perf_packed_fn = long long(CORECLR_DELEGATE_CALLTYPE*)();
    void* tmp = nullptr;
    std::wstring methodPacked = L"GetPerfCountersPacked";
    int32 hr = ResolveManagedUco(bridgePath, typeW.c_str(), methodPacked.c_str(), &tmp);
    if (hr == 0 && tmp != nullptr)
    {
        auto fp = reinterpret_cast<get_perf_packed_fn>(tmp);
        long long packed = fp();
        Logger::Log::Debug("CoreCLRHost: GetPerfCountersPacked returned {}", packed);
        if (packed < 0)
        {
            return -3;
        }
        uint64 u = (uint64)packed;
        *outDelegateQuery = (int32)(u & 0xFFFF);
        *outReflectionQuery = (int32)((u >> 16) & 0xFFFF);
        *outDelegateInvoke = (int32)((u >> 32) & 0xFFFF);
        *outReflectionInvoke = (int32)((u >> 48) & 0xFFFF);
        return 0;
    }

    // Fallback: pointer-based API
    std::wstring methodW = L"GetPerfCounters";
    tmp = nullptr;
    hr = ResolveManagedUco(bridgePath, typeW.c_str(), methodW.c_str(), &tmp);
    if (hr != 0 || tmp == nullptr)
        return -3;
    fn = reinterpret_cast<get_perf_fn>(tmp);
    int32 ret = fn(outDelegateQuery, outReflectionQuery, outDelegateInvoke, outReflectionInvoke);
    return ret;
#else
    (void)outDelegateQuery;
    (void)outReflectionQuery;
    (void)outDelegateInvoke;
    (void)outReflectionInvoke;
    return -1;
#endif
}
#endif

#ifdef _DEBUG
int32 CoreCLRHost::DebugInvokeByTokenNoOut(uint64 domain, uint64 token)
{
#ifdef PLATFORM_WINDOWS
    if (!m_Initialized || !m_LoadAssemblyAndGetFunctionPointer)
        return -1;
    typedef int32(__cdecl * debug_invoke_no_out_fn)(uint64, uint64);
    std::filesystem::path bridgePath = ResolveCoreBridgePath(m_CoreBridgeAssemblyPath);
    if (!std::filesystem::exists(bridgePath))
        return -3;

    std::wstring typeW = L"GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge";
    std::wstring methodW = L"Debug_InvokeByToken_NoOut";
    void* tmp = nullptr;
    int32 hr = ResolveManagedUco(bridgePath, typeW.c_str(), methodW.c_str(), &tmp);
    if (hr != 0 || tmp == nullptr)
        return -3;
    auto fn = reinterpret_cast<debug_invoke_no_out_fn>(tmp);
    int32 rc = fn(domain, token);
    Logger::Log::Debug("CoreCLRHost: Debug_InvokeByToken_NoOut rc={}", rc);
    return rc;
#else
    (void)domain;
    (void)token;
    return -1;
#endif
}
#endif

#ifdef _DEBUG
int32 CoreCLRHost::DebugInvokeByNameNoOut(uint64 domain, const char* methodNameUtf8, uint32 methodNameLen)
{
#ifdef PLATFORM_WINDOWS
    if (!m_Initialized)
        return -1;
    if (!methodNameUtf8 || methodNameLen == 0u)
        return -2;
    // Safe path: use QueryExport + InvokeByToken to avoid UCO signature drift in debug helper
    uint64 tok = 0ULL;
    int32 qrc = QueryExport(domain, methodNameUtf8, methodNameLen, &tok);
    if (qrc == 0 && tok != 0ULL)
    {
        int32 outRes = -1;
        int32 irc = InvokeByToken(domain, tok, &outRes);
        if (irc == 0 && outRes >= 0)
            return 0;
        return -1;
    }
    return -1;
#else
    (void)domain;
    (void)methodNameUtf8;
    (void)methodNameLen;
    return -1;
#endif // PLATFORM_WINDOWS
}
#endif // _DEBUG

#ifdef _DEBUG
int32 CoreCLRHost::DebugNoopAfterAccept()
{
    Logger::Log::Debug("CoreCLRHost: DebugNoopAfterAccept entered");
    return 0;
}
#endif // _DEBUG

#ifdef _DEBUG
uint64 CoreCLRHost::GetPerfCountersPacked64()
{
#ifdef PLATFORM_WINDOWS
    // Use global delegate exclusively to avoid dereferencing 'this'
    auto loadFn = g_loadAssemblyAndGetFunctionPointer;
    if (!loadFn)
        return ~0ULL;
    using get_packed_fn = long long(CORECLR_DELEGATE_CALLTYPE*)();
    std::filesystem::path bridgePath = ResolveCoreBridgePath(m_CoreBridgeAssemblyPath);
    if (!std::filesystem::exists(bridgePath))
        return ~0ULL;

    std::wstring typeW = L"GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge";
    std::wstring methodW = L"GetPerfCountersPacked";
    void* tmp = nullptr;
    int32 hr = ResolveManagedUco(bridgePath, typeW.c_str(), methodW.c_str(), &tmp);
    if (hr != 0 || tmp == nullptr)
        return ~0ULL;
    auto fn = reinterpret_cast<get_packed_fn>(tmp);
    long long packed = fn();
    Logger::Log::Debug("CoreCLRHost: GetPerfCountersPacked returned {}", packed);
    return static_cast<uint64>(packed);
#else
    return ~0ULL;
#endif
}
#endif

#ifdef _DEBUG
int32 CoreCLRHost::DebugTickleCounters()
{
#ifdef PLATFORM_WINDOWS
    if (!m_Initialized || !m_LoadAssemblyAndGetFunctionPointer)
        return -1;
    typedef int32(__cdecl * tickle_fn)();
    std::filesystem::path bridgePath = ResolveCoreBridgePath(m_CoreBridgeAssemblyPath);
    if (!std::filesystem::exists(bridgePath))
        return -3;

    std::wstring typeW = L"GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge";
    std::wstring methodW = L"Debug_TickleCounters";
    void* tmp = nullptr;
    int32 hr = ResolveManagedUco(bridgePath, typeW.c_str(), methodW.c_str(), &tmp);
    if (hr != 0 || tmp == nullptr)
        return -3;
    auto fn = reinterpret_cast<tickle_fn>(tmp);
    return fn();
#else
    return -1;
#endif
}
#endif

} // namespace GameEngine
