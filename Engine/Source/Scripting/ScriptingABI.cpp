#include "Scripting/ScriptingABI.h"
#include <atomic>

#include "AbiHandles.h"
#include "Core/DebugMetrics.h"
#include "Core/Engine.h"
#include "DllEngineBootstrap.h"
#include "Logger/FileSink.h"
#include "Logger/Logger.h"
#include "Mathematics/Vector2.h"
#include "Scripting/PathResolver.h"
#include "Scripting/ScriptManager.h"
#include "Input/InputSystem.h"
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "ECS/Entity.h"
#include "Scripting/ScriptingService.h"
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#endif

#if LOGGER_ENABLE_FILE_LOGGING
static bool gNativeLoggerConfiguredFromEnv = false;
#endif

// Per-domain token cache to accelerate GE_Invoke after the first lookup
static std::mutex g_tokenCacheMutex;
static std::unordered_map<uint64_t, std::unordered_map<std::string, uint64_t>> g_tokenByDomainAndName;
static void PurgeTokenCacheForDomain(uint64_t domain)
{
    std::lock_guard<std::mutex> _lock(g_tokenCacheMutex);
    g_tokenByDomainAndName.erase(domain);
}

#if defined(_DEBUG)
static std::atomic<int> g_dbgQueryExportCount{0};
static std::atomic<int> g_dbgInvokeByTokenCount{0};
static std::atomic<int> g_dbgInvokeNameSlowCount{0};
extern "C"
{
    GE_API int32_t GE_CDECL GE_Debug_ResetScriptingCounters()
    {
        g_dbgQueryExportCount.store(0);
        g_dbgInvokeByTokenCount.store(0);
        g_dbgInvokeNameSlowCount.store(0);
        return 0;
    }
    GE_API int32_t GE_CDECL GE_Debug_GetScriptingCounter(int32_t which)
    {
        switch (which)
        {
        case 0:
            return (int32_t)g_dbgQueryExportCount.load();
        case 1:
            return (int32_t)g_dbgInvokeByTokenCount.load();
        case 2:
            return (int32_t)g_dbgInvokeNameSlowCount.load();
        default:
            return -1;
        }
    }
}
#endif

// Shared bootstrap (DllEngineBootstrap.h): an engine that is running, or whose
// Initialize is running on this thread, means nothing to do; an engine some caller in
// this process has already driven is never started from here; otherwise the standalone
// (dotnet test) path initializes it, and concurrent first callers wait for that
// Initialize instead of starting a second one.
static GE_Result EnsureEngineInitialized()
{
    return GameEngine::DllBootstrap::EnsureEngineInitialized();
}

static void SA_DebugWrite(const char* msg)
{
#if defined(GE_VERBOSE)
    try
    {
        auto p = std::filesystem::current_path() / "scriptingabi-debug.txt";
        std::ofstream ofs(p, std::ios::app | std::ios::out);
        ofs << msg << "\n";
    }
    catch (...)
    {
    }
#else
    (void)msg;
#endif
}

// Centralized mapping from managed CoreBridge error codes to GE_Result
// Managed convention: 0=success; negatives=failure
// Standardized mapping:
//   -2 => GE_Result_InvalidArg
//   -3 => GE_Result_NotFound
//   -4 => GE_Result_NotInitialized
//   any other negative => GE_Result_Fail
static GE_Result MapManagedRcToGeResult(int rc)
{
    if (rc == 0)
        return GE_Result_Ok;
    switch (rc)
    {
    case -2:
        return GE_Result_InvalidArg;
    case -3:
        return GE_Result_NotFound;
    case -4:
        return GE_Result_NotInitialized;
    default:
        return GE_Result_Fail;
    }
}

#if 0
// Resolve a candidate path that exists from a list of relative candidates
static std::filesystem::path ResolveFirstExisting(const std::initializer_list<std::filesystem::path>& candidates)
{
    for (const auto& rel : candidates) {
        auto abs = std::filesystem::absolute(rel);
        if (std::filesystem::exists(abs)) return abs;
    }
    return {};
}
#endif

// Ensure that the logger instance inside GameEngine.Native is configured to
// write to the same logfile as the host Editor when one is present.
//
// Background: the Logger module is built as a static library and linked
// into both the Editor executable and the GameEngine.Native DLL. This
// means each binary gets its own Logger::Log::LoggerState instance. The
// Editor configures its instance (console + a FileSink) in
// Apps/Editor/Source/main.cpp, but the GameEngine.Native copy would remain
// unconfigured by default, causing messages routed through GE_Log from the
// managed side (e.g., EngineLogWriter / Console.WriteLine from
// InitializeOnLoad) to miss the Editor's logfile.
//
// To bridge this, we lazily initialize the GameEngine.Native logger on
// first GE_Log call, using the GE_LOGFILE environment variable the Editor
// exports for every run — the explicit -logfile path when one was given, and
// a per-process default under the user cache directory otherwise. This keeps
// responsibilities clear (Editor owns the log path; the native DLL mirrors it
// when needed) while avoiding any dependency on Editor-specific types here.
static void EnsureNativeLoggerConfiguredFromEnv()
{
#if LOGGER_ENABLE_FILE_LOGGING
    if (gNativeLoggerConfiguredFromEnv)
    {
        return;
    }

    // If sinks are already present, assume configuration was done elsewhere
    // (for example, in a unit test harness). Emit a one-shot diagnostic so
    // Editor runs can see that we intentionally skipped GE_LOGFILE wiring.
    const size_t sinkCount = Logger::Log::GetSinkCount();
    if (sinkCount > 0)
    {
        Logger::Log::Debug("[ScriptingABI] GameEngine.Native logger already configured; sinkCount={}", sinkCount);
        gNativeLoggerConfiguredFromEnv = true;
        return;
    }

    Logger::Log::Config cfg;
    cfg.GlobalMinLevel = Logger::LogLevel::Debug;
    Logger::Log::Initialize(cfg);

    const char* logFile = std::getenv("GE_LOGFILE");
    if (logFile && logFile[0] != '\0')
    {
        Logger::FileSink::Config fileCfg;
        fileCfg.filename = logFile;
        fileCfg.append = true;
        fileCfg.minLevel = Logger::Log::GetLogLevel();
        fileCfg.includeTimestamp = true;
#if LOGGER_ENABLE_SOURCE_LOCATION
        fileCfg.includeSource = true;
#endif
        Logger::Log::AddSink(Logger::MakeUnique<Logger::FileSink>(fileCfg));
        Logger::Log::Info("GameEngine.Native logger attached to GE_LOGFILE: {}", fileCfg.filename);
    }
    else
    {
        // This should not normally happen in the Editor, which exports
        // GE_LOGFILE on every run. Log once so IoL investigations can see
        // that the bridge was skipped.
        Logger::Log::Warning("[ScriptingABI] GE_LOGFILE not set; GE_Log will use preconfigured sinks only.");
    }

    gNativeLoggerConfiguredFromEnv = true;
#endif
}

static GE_Result EnsureClrInitialized()
{
    using namespace std::filesystem;
    auto& engine = GameEngine::EngineCore::GetInstance();
    auto& clr = engine.GetScriptManager().GetCLRHost();
    if (clr.IsInitialized())
        return GE_Result_Ok;

    // Prefer centrally resolved Scripts/CoreBridge/HotReload runtimeconfig
    path chosen;
    if (auto rc = GameEngine::ScriptingPaths::ResolveScriptsRuntimeConfig(); !rc.empty())
    {
#if defined(GE_VERBOSE)
        Logger::Log::Info("EnsureClrInitialized: central runtimeconfig candidate='{}' exists={}", rc.string(), exists(rc) ? "Y" : "N");
#endif
        if (exists(rc))
        {
            if (clr.Initialize(rc) || clr.IsInitialized())
            {
                chosen = rc;
            }
        }
    }

    if (chosen.empty())
    {
        SA_DebugWrite("EnsureClrInitialized: CLR Initialize failed for all candidates");
        Logger::Log::Error("EnsureClrInitialized: could not initialize CLR for any runtimeconfig candidate");
        return GE_Result_Fail;
    }

    SA_DebugWrite("EnsureClrInitialized: CLR initialized");
#if defined(GE_VERBOSE)
    Logger::Log::Info("EnsureClrInitialized: CLR initialized with {}", chosen.string());
#endif

    // Initialize CoreBridge using centralized resolver
    path coreBridge = GameEngine::ScriptingPaths::ResolveCoreBridgeDll();
    SA_DebugWrite((std::string("EnsureClrInitialized: chosen CoreBridge='") + coreBridge.string() + "' exists=" + (std::filesystem::exists(coreBridge) ? "Y" : "N")).c_str());
#if defined(GE_VERBOSE)
    Logger::Log::Info("EnsureClrInitialized: probing CoreBridge, candidate='{}' exists={}", coreBridge.string(), std::filesystem::exists(coreBridge) ? "Y" : "N");
#endif
    if (!coreBridge.empty())
    {
        SA_DebugWrite("EnsureClrInitialized: calling InitializeCoreBridge");
        bool coreBridgeOk = clr.InitializeCoreBridge(coreBridge);
        if (!coreBridgeOk)
        {
            Logger::Log::Error("EnsureClrInitialized: InitializeCoreBridge failed for {}", coreBridge.string());
            return GE_Result_Fail;
        }
        (void)clr.InitializeDiagnosticsSink();
#if defined(GE_VERBOSE)
        Logger::Log::Info("EnsureClrInitialized: CoreBridge initialized from {}", coreBridge.string());
#endif
        // Proactively bind managed delegates for HRM fast paths (best-effort)
        try
        {
            auto& eng2 = GameEngine::EngineCore::GetInstance();
            auto& clr2 = eng2.GetScriptManager().GetCLRHost();
            void* ptr = clr2.GetManagedFunction(
                std::string(),
                std::string("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge"),
                std::string("Debug_ForceRebindDelegates"));
            if (ptr)
            {
                int (*fn)() = reinterpret_cast<int (*)()>(ptr);
                int mask = fn();
                SA_DebugWrite((std::string("EnsureClrInitialized: Debug_ForceRebindDelegates mask=") + std::to_string(mask)).c_str());
            }
            else
            {
                SA_DebugWrite("EnsureClrInitialized: Debug_ForceRebindDelegates not found");
            }
        }
        catch (...)
        { /* best-effort */
        }
    }
    else
    {
        Logger::Log::Warning("EnsureClrInitialized: CoreBridge.dll not found near test output; proceeding without it");
    }
    return GE_Result_Ok;
}

#if defined(_DEBUG)
namespace GameEngine
{
GE_API void ResetNativeLoggerBridgeForTests()
{
#if LOGGER_ENABLE_FILE_LOGGING
    // Clear any sinks and reset the lazy GE_LOGFILE wiring state so tests can
    // exercise both the "no sinks" and "preconfigured sinks" paths
    Logger::Log::Shutdown();
    Logger::Log::ClearSinks();
    gNativeLoggerConfiguredFromEnv = false;
#endif
}
} // namespace GameEngine
#endif

// Storage for managed callbacks registered by CoreBridge (id -> function pointer)
static std::mutex g_cbMutex;
static std::unordered_map<uint32_t, void*> g_managedCallbacks;

// Cached managed callbacks registered by CoreBridge
static std::mutex g_hotReloadCbMutex;
static int (*g_cbPreloadAssemblyContext)(const char*, uint32_t) = nullptr;
static int (*g_cbSwapPreloadedContext)() = nullptr;
static int (*g_cbCleanupOldContext)(const char*, uint32_t) = nullptr;
static int (*g_cbClearCompilerCache)() = nullptr;
static int (*g_cbGetCompilerStats)() = nullptr;
static int (*g_cbGetHotReloadMetrics)(uint64_t*, uint64_t*, int32_t*, int32_t*) = nullptr;
static int (*g_cbGetHotReloadMetricsEx)(uint64_t*, uint64_t*, int32_t*, int32_t*, int32_t*) = nullptr;
static int (*g_cbResetHotReloadMetrics)() = nullptr;
static int (*g_cbGetHotReloadEditorMetrics)(uint64_t*) = nullptr;

static int (*g_cbQueryExport)(uint64_t, const char*, uint32_t, uint64_t*) = nullptr;
static int (*g_cbInvokeByToken)(uint64_t, uint64_t, int32_t*) = nullptr;
static int (*g_cbUnloadDomain)(uint64_t) = nullptr;

static void* GetManagedCallbackPtr(uint32_t id)
{
    std::lock_guard<std::mutex> lock(g_cbMutex);
    auto it = g_managedCallbacks.find(id);
    return (it != g_managedCallbacks.end()) ? it->second : nullptr;
}

static void EnsureTokenCallbacksResolved()
{
    std::lock_guard<std::mutex> lock(g_hotReloadCbMutex);
    if (!g_cbQueryExport)
        g_cbQueryExport = reinterpret_cast<int (*)(uint64_t, const char*, uint32_t, uint64_t*)>(GetManagedCallbackPtr(GE_CB_QueryExport));
    if (!g_cbInvokeByToken)
        g_cbInvokeByToken = reinterpret_cast<int (*)(uint64_t, uint64_t, int32_t*)>(GetManagedCallbackPtr(GE_CB_InvokeByToken));
    if (!g_cbUnloadDomain)
        g_cbUnloadDomain = reinterpret_cast<int (*)(uint64_t)>(GetManagedCallbackPtr(GE_CB_UnloadDomain));
}

static void EnsureHotReloadCallbacksResolved()
{
    std::lock_guard<std::mutex> lock(g_hotReloadCbMutex);
    if (!g_cbPreloadAssemblyContext)
        g_cbPreloadAssemblyContext = reinterpret_cast<int (*)(const char*, uint32_t)>(GetManagedCallbackPtr(GE_CB_PreloadAssemblyContext));
    if (!g_cbSwapPreloadedContext)
        g_cbSwapPreloadedContext = reinterpret_cast<int (*)()>(GetManagedCallbackPtr(GE_CB_SwapPreloadedContext));
    if (!g_cbCleanupOldContext)
        g_cbCleanupOldContext = reinterpret_cast<int (*)(const char*, uint32_t)>(GetManagedCallbackPtr(GE_CB_CleanupOldContext));
    if (!g_cbClearCompilerCache)
        g_cbClearCompilerCache = reinterpret_cast<int (*)()>(GetManagedCallbackPtr(GE_CB_ClearCompilerCache));
    if (!g_cbGetHotReloadMetrics)
        g_cbGetHotReloadMetrics = reinterpret_cast<int (*)(uint64_t*, uint64_t*, int32_t*, int32_t*)>(GetManagedCallbackPtr(GE_CB_GetHotReloadMetrics));
    if (!g_cbGetHotReloadMetricsEx)
        g_cbGetHotReloadMetricsEx = reinterpret_cast<int (*)(uint64_t*, uint64_t*, int32_t*, int32_t*, int32_t*)>(GetManagedCallbackPtr(GE_CB_GetHotReloadMetricsEx));
    if (!g_cbGetHotReloadEditorMetrics)
        g_cbGetHotReloadEditorMetrics = reinterpret_cast<int (*)(uint64_t*)>(GetManagedCallbackPtr(GE_CB_GetHotReloadEditorMetrics));

    if (!g_cbResetHotReloadMetrics)
        g_cbResetHotReloadMetrics = reinterpret_cast<int (*)()>(GetManagedCallbackPtr(GE_CB_ResetHotReloadMetrics));

    if (!g_cbGetCompilerStats)
        g_cbGetCompilerStats = reinterpret_cast<int (*)()>(GetManagedCallbackPtr(GE_CB_GetCompilerStats));
}

extern "C"
{

    // Simple storage for diagnostics sink registration
    static std::mutex g_diagMutex;
    static GE_DiagnosticsSink g_diagSink{};
    static bool g_diagSinkSet = false;

    static constexpr uint32_t kAbiVersion = GE_ABI_VERSION_CURRENT; // mirror header; avoid drift

    // Representative engine API: asset count via AssetManager
    GE_API GE_Result GE_CDECL GE_GetAssetCount(int32_t* outCount)
    {
        try
        {
            if (!outCount)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto& eng = GameEngine::EngineCore::GetInstance();
            int32_t count = static_cast<int32_t>(eng.GetAssetManager().GetRegistry().GetAssetCount());
            *outCount = count;
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    // ECS getters wired to Engine-owned primary world (Phase 1)
    GE_API GE_Result GE_CDECL GE_ECS_GetWorldHandle(GE_Handle* outWorld)
    {
        try
        {
            if (!outWorld)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto& eng = GameEngine::EngineCore::GetInstance();
            auto* world = eng.EnsurePrimaryWorld();
            *outWorld = static_cast<GE_Handle>(reinterpret_cast<uintptr_t>(world));
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }
    GE_API GE_Result GE_CDECL GE_ECS_GetEntityCount(GE_Handle world, int32_t* outCount)
    {
        try
        {
            if (!outCount)
                return GE_Result_InvalidArg;
            if (world == 0)
                return GE_Result_InvalidArg;
            auto* w = GameEngine::ScriptingAbi::WorldFromHandle(world);
            *outCount = static_cast<int32_t>(w->GetEntityCount());
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    // -------- Input --------
    GE_API GE_Result GE_CDECL GE_Input_GetActionState(uint64_t actionId, GE_InputActionState* outState)
    {
        try
        {
            if (!outState)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;

            auto* input = GameEngine::EngineCore::GetInstance().GetRuntimeInput();
            if (!input)
                return GE_Result_NotInitialized;

            const GameEngine::Input::ActionState s = input->GetActionState((GameEngine::Input::ActionId)actionId);
            outState->pressed = s.pressed ? 1u : 0u;
            outState->justPressed = s.justPressed ? 1u : 0u;
            outState->justReleased = s.justReleased ? 1u : 0u;
            outState->_pad0 = 0u;
            outState->value = s.value;
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_Input_IsKeyDown(int32_t key, int32_t* outDown)
    {
        try
        {
            if (!outDown)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto* input = GameEngine::EngineCore::GetInstance().GetRuntimeInput();
            if (!input)
                return GE_Result_NotInitialized;
            *outDown = input->IsKeyDown((GameEngine::Input::KeyCode)key) ? 1 : 0;
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_Input_WasKeyPressed(int32_t key, int32_t* outPressed)
    {
        try
        {
            if (!outPressed)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto* input = GameEngine::EngineCore::GetInstance().GetRuntimeInput();
            if (!input)
                return GE_Result_NotInitialized;
            *outPressed = input->WasKeyPressed((GameEngine::Input::KeyCode)key) ? 1 : 0;
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_Input_WasKeyReleased(int32_t key, int32_t* outReleased)
    {
        try
        {
            if (!outReleased)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto* input = GameEngine::EngineCore::GetInstance().GetRuntimeInput();
            if (!input)
                return GE_Result_NotInitialized;
            *outReleased = input->WasKeyReleased((GameEngine::Input::KeyCode)key) ? 1 : 0;
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_Input_GetMousePosition(float* outX, float* outY)
    {
        try
        {
            if (!outX || !outY)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto* input = GameEngine::EngineCore::GetInstance().GetRuntimeInput();
            if (!input)
                return GE_Result_NotInitialized;
            const GameEngine::Mathematics::Vector2 position = input->GetMousePosition();
            *outX = position.x;
            *outY = position.y;
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_Input_IsPointerInWindow(int32_t* outInWindow)
    {
        try
        {
            if (!outInWindow)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto* input = GameEngine::EngineCore::GetInstance().GetRuntimeInput();
            if (!input)
                return GE_Result_NotInitialized;
            *outInWindow = input->IsPointerInWindow() ? 1 : 0;
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_Input_IsWindowFocused(int32_t* outFocused)
    {
        try
        {
            if (!outFocused)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto* input = GameEngine::EngineCore::GetInstance().GetRuntimeInput();
            if (!input)
                return GE_Result_NotInitialized;
            *outFocused = input->IsWindowFocused() ? 1 : 0;
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_Input_IsMouseButtonDown(int32_t button, int32_t* outDown)
    {
        try
        {
            if (!outDown)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto* input = GameEngine::EngineCore::GetInstance().GetRuntimeInput();
            if (!input)
                return GE_Result_NotInitialized;
            *outDown = input->IsMouseButtonDown((GameEngine::Input::MouseCode)button) ? 1 : 0;
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_Input_WasMouseButtonPressed(int32_t button, int32_t* outPressed)
    {
        try
        {
            if (!outPressed)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto* input = GameEngine::EngineCore::GetInstance().GetRuntimeInput();
            if (!input)
                return GE_Result_NotInitialized;
            *outPressed = input->WasMouseButtonPressed((GameEngine::Input::MouseCode)button) ? 1 : 0;
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_Input_WasMouseButtonReleased(int32_t button, int32_t* outReleased)
    {
        try
        {
            if (!outReleased)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto* input = GameEngine::EngineCore::GetInstance().GetRuntimeInput();
            if (!input)
                return GE_Result_NotInitialized;
            *outReleased = input->WasMouseButtonReleased((GameEngine::Input::MouseCode)button) ? 1 : 0;
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_Input_RegisterAction(uint64_t contextId, uint64_t actionId, int32_t isAxis)
    {
        try
        {
            if (contextId == 0 || actionId == 0)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;

            auto* input = GameEngine::EngineCore::GetInstance().GetRuntimeInput();
            if (!input)
                return GE_Result_NotInitialized;

            GameEngine::Input::ActionDesc desc{};
            desc.id = (GameEngine::Input::ActionId)actionId;
            desc.isAxis = (isAxis != 0);
            input->RegisterAction((GameEngine::Input::ContextId)contextId, desc);
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_Input_RemoveAction(uint64_t contextId, uint64_t actionId)
    {
        try
        {
            if (contextId == 0 || actionId == 0)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto* input = GameEngine::EngineCore::GetInstance().GetRuntimeInput();
            if (!input)
                return GE_Result_NotInitialized;
            input->RemoveAction((GameEngine::Input::ContextId)contextId, (GameEngine::Input::ActionId)actionId);
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_Input_ClearBindings(uint64_t contextId, uint64_t actionId)
    {
        try
        {
            if (contextId == 0 || actionId == 0)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto* input = GameEngine::EngineCore::GetInstance().GetRuntimeInput();
            if (!input)
                return GE_Result_NotInitialized;
            input->ClearBindings((GameEngine::Input::ContextId)contextId, (GameEngine::Input::ActionId)actionId);
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_Input_Bind(uint64_t contextId,
                                            uint64_t actionId,
                                            int32_t deviceType,
                                            int32_t code,
                                            float scale,
                                            int32_t requiredMods,
                                            int32_t forbiddenMods)
    {
        try
        {
            if (contextId == 0 || actionId == 0)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto* input = GameEngine::EngineCore::GetInstance().GetRuntimeInput();
            if (!input)
                return GE_Result_NotInitialized;

            GameEngine::Input::ActionBinding b{};
            b.device = (GameEngine::Input::DeviceType)deviceType;
            b.code = code;
            b.scale = scale;
            b.requiredMods = requiredMods;
            b.forbiddenMods = forbiddenMods;
            input->BindKey((GameEngine::Input::ContextId)contextId, (GameEngine::Input::ActionId)actionId, b);
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_Input_PushContext(uint64_t contextId)
    {
        try
        {
            if (contextId == 0)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto* input = GameEngine::EngineCore::GetInstance().GetRuntimeInput();
            if (!input)
                return GE_Result_NotInitialized;
            input->PushContext((GameEngine::Input::ContextId)contextId);
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_Input_PopContext(uint64_t contextId)
    {
        try
        {
            if (contextId == 0)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto* input = GameEngine::EngineCore::GetInstance().GetRuntimeInput();
            if (!input)
                return GE_Result_NotInitialized;
            input->PopContext((GameEngine::Input::ContextId)contextId);
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_Input_SetContextEnabled(uint64_t contextId, int32_t enabled)
    {
        try
        {
            if (contextId == 0)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto* input = GameEngine::EngineCore::GetInstance().GetRuntimeInput();
            if (!input)
                return GE_Result_NotInitialized;
            input->SetContextEnabled((GameEngine::Input::ContextId)contextId, enabled != 0);
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_Capabilities(uint32_t* outMask)
    {
        if (!outMask)
            return GE_Result_InvalidArg;
        // During transition: declare DomainSwap present; package loader and invoke by name pending
        uint32_t mask = 0;
        mask |= GE_Cap_HasDomainSwap;
        // Future: mask |= GE_Cap_HasPackageLoader;
        // Future: mask |= GE_Cap_HasInvokeByName;
        *outMask = mask;
        return GE_Result_Ok;
    }

    // brace cleanup

    GE_API uint32_t GE_CDECL GE_ScriptingGetAbiVersion(void)
    {
        return kAbiVersion;
    }

    GE_API GE_Result GE_CDECL GE_Log(GE_LogLevel level, const char* msg, uint32_t msgLen)
    {
        try
        {
            // Ensure the logger instance inside GameEngine.Native is wired up
            // to the same logfile as the Editor (when available) so that
            // Console.WriteLine/EngineLogWriter messages from managed code
            // appear in the Editor log.
            EnsureNativeLoggerConfiguredFromEnv();

#if LOGGER_ENABLE_FILE_LOGGING
            // Emit a one-shot diagnostic the first time GE_Log is hit so we
            // can confirm IoL / Console.WriteLine are actually reaching this
            // bridge in Editor runs.
            static bool sReportedFirstCall = false;
            if (!sReportedFirstCall)
            {
                sReportedFirstCall = true;
                Logger::Log::Debug("[ScriptingABI] GE_Log first call: level={} len={} bytes", static_cast<int>(level), static_cast<unsigned int>(msgLen));
            }
#endif

            // The managed side passes an explicit byte length and does not guarantee
            // that the buffer is null-terminated. Use a view constructed from
            // (ptr,len) instead of treating msg as a C-string.
            std::string_view text;
            if (msg != nullptr && msgLen > 0u)
            {
                text = std::string_view(msg, static_cast<size_t>(msgLen));
            }

            switch (level)
            {
            case GE_Log_Trace:
            case GE_Log_Debug:
                Logger::Log::Debug("{}", text);
                break;
            case GE_Log_Info:
                Logger::Log::Info("{}", text);
                break;
            case GE_Log_Warn:
                Logger::Log::Warning("{}", text);
                break;
            case GE_Log_Error:
                Logger::Log::Error("{}", text);
                break;
            case GE_Log_Fatal:
                Logger::Log::Critical("{}", text);
                break;
            default:
                Logger::Log::Info("{}", text);
                break;
            }
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    // -------- Managed Callback Registry --------
    GE_API GE_Result GE_CDECL GE_RegisterManagedCallback(GE_CallbackId id, void* fnPtr)
    {
        try
        {
            if (id == 0 || fnPtr == nullptr)
                return GE_Result_InvalidArg;
            std::lock_guard<std::mutex> lock(g_cbMutex);
            g_managedCallbacks[static_cast<uint32_t>(id)] = fnPtr;
            if (id == GE_CB_PreloadAssemblyContext || id == GE_CB_SwapPreloadedContext)
            {
                Logger::Log::Info("Registered HRM callback id={} ptr={}", (uint32_t)id, fnPtr);
            }
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_GetManagedCallback(GE_CallbackId id, void** outFnPtr)
    {
        try
        {
            if (!outFnPtr)
                return GE_Result_InvalidArg;
            std::lock_guard<std::mutex> lock(g_cbMutex);
            auto it = g_managedCallbacks.find(static_cast<uint32_t>(id));
            if (it == g_managedCallbacks.end())
            {
                *outFnPtr = nullptr;
                return GE_Result_NotFound;
            }
            *outFnPtr = it->second;
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_SubscribeDiagnostics(const GE_DiagnosticsSink* sink)
    {
        std::lock_guard<std::mutex> lock(g_diagMutex);
        if (!sink)
            return GE_Result_InvalidArg;
        g_diagSink = *sink; // shallow copy of function pointers and userData
        g_diagSinkSet = true;
#if defined(GE_VERBOSE)
        Logger::Log::Debug("Diagnostics sink subscribed");
#endif
        return GE_Result_Ok;
    }

    GE_API GE_Result GE_CDECL GE_UnsubscribeDiagnostics(const GE_DiagnosticsSink* sink)
    {
        std::lock_guard<std::mutex> lock(g_diagMutex);
        if (!sink)
            return GE_Result_InvalidArg;
        g_diagSink = GE_DiagnosticsSink{};
        g_diagSinkSet = false;
#if defined(GE_VERBOSE)
        Logger::Log::Debug("Diagnostics sink unsubscribed");
#endif
        return GE_Result_Ok;
    }

    // NOTE: Domain handle implementation is TBD; these are placeholders to complete the ABI surface
    GE_API GE_Result GE_CDECL GE_NotifyCompilationEvent(GE_CompilationStage stage,
                                                        float progress01,
                                                        const GE_Diagnostic* diags,
                                                        uint32_t diagCount)
    {
        std::lock_guard<std::mutex> lock(g_diagMutex);
        if (!g_diagSinkSet || !g_diagSink.onCompilationEvent)
            return GE_Result_NotInitialized;
        g_diagSink.onCompilationEvent(stage, progress01, diags, diagCount, g_diagSink.userData);
        return GE_Result_Ok;
    }

    GE_API GE_Result GE_CDECL GE_NotifyReloadEvent(GE_ReloadStage stage,
                                                   const char* reasonUtf8)
    {
        std::lock_guard<std::mutex> lock(g_diagMutex);
        if (!g_diagSinkSet || !g_diagSink.onReloadEvent)
            return GE_Result_NotInitialized;
        g_diagSink.onReloadEvent(stage, reasonUtf8, g_diagSink.userData);
        return GE_Result_Ok;
    }

    GE_API GE_Result GE_CDECL GE_ScriptingInitialize(const GE_ScriptingConfig* config)
    {
        try
        {
            if (!config)
                return GE_Result_InvalidArg;

            // ABI handshake (major compatibility)
            uint32_t requestedAbi = GE_ABI_VERSION_CURRENT;
            (void)requestedAbi;

            // Acquire CLR host through Engine's ScriptManager
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto& engine = GameEngine::EngineCore::GetInstance();

            // ... CLR host acquired above

            auto& scriptMgr = engine.GetScriptManager();
            auto& clr = scriptMgr.GetCLRHost();

            // Initialize CoreCLR with provided runtimeconfig, if any
            std::filesystem::path runtimeConfigPath;
            if (config->runtimeConfigPath.data && config->runtimeConfigPath.length > 0)
            {
                runtimeConfigPath = std::filesystem::path(std::string(config->runtimeConfigPath.data, config->runtimeConfigPath.length));
            }
            if (!runtimeConfigPath.empty())
            {
                if (!clr.Initialize(runtimeConfigPath))
                {
                    Logger::Log::Error("GE_ScriptingInitialize: CoreCLR initialization failed for {}", runtimeConfigPath.string());
                    return GE_Result_Fail;
                }
            }

            // Initialize CoreBridge and diagnostics, if path provided
            std::filesystem::path coreBridgePath;
            if (config->coreBridgeAssembly.data && config->coreBridgeAssembly.length > 0)
            {
                coreBridgePath = std::filesystem::path(std::string(config->coreBridgeAssembly.data, config->coreBridgeAssembly.length));
            }

            // If an explicit native library is provided, set it before CoreBridge init
            if (config->nativeLibraryPath.data && config->nativeLibraryPath.length > 0)
            {
                std::string nativePath(config->nativeLibraryPath.data, config->nativeLibraryPath.length);
                clr.SetNativeLibraryOverride(nativePath);
            }

            if (!coreBridgePath.empty())
            {
                if (!std::filesystem::exists(coreBridgePath))
                {
                    Logger::Log::Warning("GE_ScriptingInitialize: CoreBridge not found at {}", coreBridgePath.string());
                }
                else if (!clr.InitializeCoreBridge(coreBridgePath))
                {
                    int32_t code = clr.GetLastCoreBridgeInitResult();
                    // Treat managed ABI incompatibility as Fail (fail-fast behavior)
                    if (code == -3)
                    {
                        Logger::Log::Error("GE_ScriptingInitialize: CoreBridge init reported ABI incompatibility (code -3) for {}", coreBridgePath.string());
                        return GE_Result_Fail;
                    }
                    Logger::Log::Error("GE_ScriptingInitialize: InitializeCoreBridge failed (code {}) for {}", code, coreBridgePath.string());
                    return GE_Result_Fail;
                }
            }
            else
            {
                // Fallback: attempt centralized resolution for CLR/CoreBridge when not explicitly provided
                GE_Result er = EnsureClrInitialized();
                if (er != GE_Result_Ok)
                    return er;
            }

            // Subscribe diagnostics routing (no-op if CLR not fully initialized)
            clr.InitializeDiagnosticsSink();

            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_ScriptingShutdown(void)
    {
        try
        {
            // Placeholder: when CoreCLRHost wiring moves here, perform teardown
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_GetCurrentRuntimeDomain(GE_DomainHandle* outDomain)
    {
        return GameEngine::ScriptingService::Get().GetCurrentRuntimeDomain(outDomain);
    }

    GE_API GE_Result GE_CDECL GE_GetCurrentEditorDomain(GE_DomainHandle* outDomain)
    {
        return GameEngine::ScriptingService::Get().GetCurrentEditorDomain(outDomain);
    }

    GE_API GE_Result GE_CDECL GE_ScriptsDomainSwap(GE_DomainHandle newDomain)
    {
        auto& svc = GameEngine::ScriptingService::Get();
        svc.NotifyBeforeUnload();
        GE_Result r = svc.SwapRuntimeDomain(newDomain);
        svc.NotifyAfterLoad();
        return r;
    }

    GE_API GE_Result GE_CDECL GE_GetInterface(uint32_t abiVersion, const void** outTable, uint32_t* outSizeBytes)
    {
        try
        {
            if (!outTable || !outSizeBytes)
                return GE_Result_InvalidArg;
            uint32_t major = (abiVersion >> 16) & 0xFFFFu;
            if (major != ((kAbiVersion >> 16) & 0xFFFFu))
            {
                *outTable = nullptr;
                *outSizeBytes = 0;
                return GE_Result_Fail;
            }
            static GE_Interface_v1 s_Iface;
            s_Iface.sizeBytes = sizeof(GE_Interface_v1);
            s_Iface.abiVersion = kAbiVersion;
            s_Iface.Log = reinterpret_cast<GE_Log_Fn>(&GE_Log);
            s_Iface.RegisterManagedCallback = reinterpret_cast<GE_RegisterManagedCallback_Fn>(&GE_RegisterManagedCallback);
            s_Iface.GetManagedCallback = reinterpret_cast<GE_GetManagedCallback_Fn>(&GE_GetManagedCallback);
            s_Iface.NotifyCompilationEvent = reinterpret_cast<GE_NotifyCompilationEvent_Fn>(&GE_NotifyCompilationEvent);
            s_Iface.NotifyReloadEvent = reinterpret_cast<GE_NotifyReloadEvent_Fn>(&GE_NotifyReloadEvent);

            s_Iface.SubscribeDiagnostics = reinterpret_cast<GE_SubscribeDiagnostics_Fn>(&GE_SubscribeDiagnostics);
            s_Iface.UnsubscribeDiagnostics = reinterpret_cast<GE_UnsubscribeDiagnostics_Fn>(&GE_UnsubscribeDiagnostics);
            s_Iface.GetAssetCount = reinterpret_cast<GE_GetAssetCount_Fn>(&GE_GetAssetCount);
            s_Iface.ScriptsDomainSwap = reinterpret_cast<GE_ScriptsDomainSwap_Fn>(&GE_ScriptsDomainSwap);
            s_Iface.ScriptsDomainUnload = reinterpret_cast<GE_ScriptsDomainUnload_Fn>(&GE_ScriptsDomainUnload);
            s_Iface.ECS_GetWorldHandle = reinterpret_cast<GE_ECS_GetWorldHandle_Fn>(&GE_ECS_GetWorldHandle);
            s_Iface.ECS_GetEntityCount = reinterpret_cast<GE_ECS_GetEntityCount_Fn>(&GE_ECS_GetEntityCount);
            *outTable = &s_Iface;
            *outSizeBytes = sizeof(GE_Interface_v1);
            return GE_Result_Ok;
        }
        catch (...)
        {
            if (outTable)
                *outTable = nullptr;
            if (outSizeBytes)
                *outSizeBytes = 0;
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_ScriptsDomainCreateFromPackage(const GE_PackageEntry* entries,
                                                                uint32_t entryCount,
                                                                uint32_t flags,
                                                                GE_DomainHandle* outDomain)
    {
        try
        {
            if (!entries || entryCount == 0 || !outDomain)
                return GE_Result_InvalidArg;
            const GE_PackageEntry& primary = entries[0];
            if (!primary.assembly.data || primary.assembly.length == 0)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            if (EnsureClrInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto& eng = GameEngine::EngineCore::GetInstance();
            // Load bytes via CLR host (prefer CoreBridge fast path; fallback to temp file + HRM path)
            auto& clr = eng.GetScriptManager().GetCLRHost();
            uint64_t id = 0ULL;
#if defined(GE_VERBOSE)
            fprintf(stderr, "[DEBUG] GE_ScriptsDomainCreateFromPackage: entryCount=%u firstLen=%u flags=0x%08X\n", entryCount, entryCount > 0 ? entries[0].assembly.length : 0u, flags);
#endif
            SA_DebugWrite("ScriptingABI: calling AcceptPackage");
            int32_t acceptRc = clr.AcceptPackage(entries, entryCount, flags, outDomain);
            SA_DebugWrite("ScriptingABI: after AcceptPackage");
#if defined(GE_VERBOSE)
            fprintf(stderr, "[DEBUG] AcceptPackage managed rc=%d outDomain=%llu\n", acceptRc, (unsigned long long)(outDomain ? *outDomain : 0ull));
#endif
            if (acceptRc >= 0)
            {
                // Prefer the domain id reported by managed AcceptPackage (same ALC/type identity)
                if (outDomain && *outDomain != 0ULL)
                {
                    id = *outDomain;
                }
                else
                {
                    id = clr.GetCurrentRuntimeDomainId();
                }
#if defined(GE_VERBOSE)
                fprintf(stderr, "[DEBUG] AcceptPackage: current managed domain id=%llu\n", (unsigned long long)id);
#endif
            }
            else
            {
                // If managed reported capability missing (-3), propagate as NotFound without fallback
                if (acceptRc == GE_Result_NotFound)
                {
                    return GE_Result_NotFound;
                }
                // Fallback: write bytes to temp file and use path-based loader
                std::string pidPart;
#if defined(_WIN32)
                pidPart = std::to_string(static_cast<unsigned long>(::GetCurrentProcessId()));
#else
                // On non-Windows platforms, derive a reasonably unique suffix from the current thread id
                auto tidHash = std::hash<std::thread::id>{}(std::this_thread::get_id());
                pidPart = std::to_string(static_cast<unsigned long>(tidHash));
#endif
                std::filesystem::path tmp = std::filesystem::temp_directory_path() /
                                            std::filesystem::path("GE_Scripts_" + pidPart + ".dll");
#if defined(GE_VERBOSE)
                fprintf(stderr, "[DEBUG] AcceptPackage: falling back to temp path load: %s\n", tmp.string().c_str());
#endif
                try
                {
                    std::ofstream ofs(tmp, std::ios::binary);
                    ofs.write(reinterpret_cast<const char*>(primary.assembly.data), primary.assembly.length);
                    ofs.close();
                    bool ok = clr.PreloadAndSwapFromPath(tmp);
#if defined(GE_VERBOSE)
                    fprintf(stderr, "[DEBUG] Fallback PreloadAndSwapFromPath => %s\n", ok ? "OK" : "FAIL");
#endif
                    if (!ok)
                        return GE_Result_Fail;
                    id = clr.GetCurrentRuntimeDomainId();
                    // Cleanup temp file now that we have loaded it successfully
                    {
                        std::error_code ec;
                        std::filesystem::remove(tmp, ec);
                    }
                }
                catch (...)
                {
#if defined(GE_VERBOSE)
                    fprintf(stderr, "[DEBUG] AcceptPackage: exception writing/loading temp file\n");
#endif
                    {
                        std::error_code ec;
                        std::filesystem::remove(tmp, ec);
                    }

                    return GE_Result_Fail;
                }
            }
            if (id == 0ULL)
            {
                static std::atomic<uint64_t> s_Next{1};
                id = s_Next.fetch_add(1, std::memory_order_relaxed);
            }
            // Make the new domain the current one; future invocations route to managed using this handle
            auto& svc = GameEngine::ScriptingService::Get();
            // Mark as live for token validity checks
            (void)svc.MarkDomainLive(id);
            const bool isEditor = (flags & GE_Pkg_Editor) != 0;
            if (isEditor)
            {
                svc.SwapEditorDomain(id);
            }
            else
            {
                svc.SwapRuntimeDomain(id);
            }
            if (outDomain)
                *outDomain = id;
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_Invoke(GE_DomainHandle domain, const char* methodNameUtf8, uint32_t methodNameLen, int32_t* outResult)
    {
        try
        {
            if (!outResult)
                return GE_Result_InvalidArg;
            // Be tolerant to over-reported lengths: clamp to first null within the provided range
            uint32_t n = 0;
            if (methodNameUtf8 && methodNameLen > 0)
            {
                const char* p = methodNameUtf8;
                for (; n < methodNameLen; ++n)
                {
                    if (p[n] == '\0')
                        break;
                }
            }
            const uint32_t effectiveLen = (methodNameUtf8 && methodNameLen > 0) ? (n > 0 ? n : methodNameLen) : 0u;
            std::string method = (methodNameUtf8 && effectiveLen > 0) ? std::string(methodNameUtf8, effectiveLen) : std::string();
            if (method.empty())
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            auto& eng = GameEngine::EngineCore::GetInstance();
            auto& clr = eng.GetScriptManager().GetCLRHost();
            if (!clr.IsInitialized())
            {
                // Do not force CLR bootstrap on this slow-path invoke; keep call safe and return Fail
                return GE_Result_Fail;
            }

            // 0) Fast path: if we have a cached token for (domain, method), use it first
            {
                uint64_t dom2 = (uint64_t)domain;
                if (dom2 == 0ULL)
                {
                    dom2 = clr.GetCurrentRuntimeDomainId();
                }
                if (dom2 != 0ULL)
                {
                    uint64_t cached = 0ULL;
                    {
                        std::lock_guard<std::mutex> _lock(g_tokenCacheMutex);
                        auto itDom = g_tokenByDomainAndName.find(dom2);
                        if (itDom != g_tokenByDomainAndName.end())
                        {
                            auto itTok = itDom->second.find(method);
                            if (itTok != itDom->second.end())
                                cached = itTok->second;
                        }
                    }
                    if (cached != 0ULL)
                    {
                        int32_t fastRes = 0;
                        GE_Result irc = GE_InvokeByToken(dom2, cached, &fastRes);
                        if (irc == GE_Result_Ok)
                        {
                            *outResult = fastRes;
                            return GE_Result_Ok;
                        }
                        // token likely invalid (domain swapped/unloaded) → purge and continue
                        {
                            std::lock_guard<std::mutex> _lock(g_tokenCacheMutex);
                            auto itDom = g_tokenByDomainAndName.find(dom2);
                            if (itDom != g_tokenByDomainAndName.end())
                                itDom->second.erase(method);
                        }
                    }
                }
            }

            int32_t keep = *outResult; // capture to verify clobbering behavior
#if defined(GE_VERBOSE)
            /* debug removed to reduce hot-path logs */
#endif
            int32_t rc = clr.InvokeInDomain(domain, method.c_str(), effectiveLen, outResult);
#if defined(GE_VERBOSE)
            /* debug removed to reduce hot-path logs */
#endif
            if (rc == 0)
            {
                // Best-effort: cache token for future invokes to hit fast path
                uint64_t dom2 = (uint64_t)domain;
                if (dom2 == 0ULL)
                {
                    dom2 = clr.GetCurrentRuntimeDomainId();
                }
                if (dom2 != 0ULL)
                {
                    uint64_t token2 = 0ULL;
                    GE_Result q2 = GE_QueryExport(dom2, method.c_str(), effectiveLen, &token2);
                    if (q2 == GE_Result_Ok && token2 != 0ULL)
                    {
                        std::lock_guard<std::mutex> _lock(g_tokenCacheMutex);
                        g_tokenByDomainAndName[dom2][method] = token2;
                    }
                }
                return GE_Result_Ok;
            }

#if defined(_DEBUG)
            g_dbgInvokeNameSlowCount.fetch_add(1);
#endif
            // 1) Query token and cache it, then invoke by token
            uint64_t token = 0ULL;
            GE_Result qerc = GE_QueryExport(domain, method.c_str(), effectiveLen, &token);
            if (qerc == GE_Result_Ok && token != 0ULL)
            {
                uint64_t dom2 = (uint64_t)domain;
                if (dom2 == 0ULL)
                {
                    dom2 = clr.GetCurrentRuntimeDomainId();
                }
                if (dom2 != 0ULL)
                {
                    // cache token for future calls
                    {
                        std::lock_guard<std::mutex> _lock(g_tokenCacheMutex);
                        g_tokenByDomainAndName[dom2][method] = token;
                    }
                    int32_t alt = 0;
                    GE_Result irc = GE_InvokeByToken(dom2, token, &alt);
#if defined(GE_VERBOSE)
                    /* debug removed to reduce hot-path logs */
#endif
                    if (irc == GE_Result_Ok)
                    {
                        *outResult = alt;
                        return GE_Result_Ok;
                    }
                }
            }

            // managed failed: ensure outResult unchanged
            *outResult = keep;
            // Additional diagnostics: report current runtime domain id from managed
            uint64_t curDom = clr.GetCurrentRuntimeDomainId();
            (void)curDom;

#if defined(GE_VERBOSE)
            Logger::Log::Debug("GE_Invoke: failure; current managed runtime domain id={} (requested domain={})", curDom, (uint64_t)domain);
#endif
            return GE_Result_Fail;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_QueryExport(GE_DomainHandle domain, const char* methodNameUtf8, uint32_t methodNameLen, uint64_t* outToken)
    {
        try
        {
            if (!outToken)
                return GE_Result_InvalidArg;
            // Clamp to NUL within reported range
            uint32_t n = 0;
            if (methodNameUtf8 && methodNameLen > 0)
            {
                const char* p = methodNameUtf8;
                for (; n < methodNameLen; ++n)
                {
                    if (p[n] == '\0')
                        break;
                }
            }
            const uint32_t effectiveLen = (methodNameUtf8 && methodNameLen > 0) ? (n > 0 ? n : methodNameLen) : 0u;
            std::string method = (methodNameUtf8 && effectiveLen > 0) ? std::string(methodNameUtf8, effectiveLen) : std::string();
            if (method.empty())
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            if (EnsureClrInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            int32_t rc = -1;
            EnsureTokenCallbacksResolved();
            auto& eng = GameEngine::EngineCore::GetInstance();
            auto& clr = eng.GetScriptManager().GetCLRHost();
            uint64_t dom2 = (uint64_t)domain;
            if (dom2 == 0ULL)
            {
                dom2 = clr.GetCurrentRuntimeDomainId();
            }
#if defined(_DEBUG)
            g_dbgQueryExportCount.fetch_add(1);
#endif
            if (g_cbQueryExport)
            {
                rc = g_cbQueryExport(dom2, method.c_str(), effectiveLen, outToken);
            }
            else
            {
                rc = clr.QueryExport(dom2, method.c_str(), effectiveLen, outToken);
            }
            return MapManagedRcToGeResult(rc);
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_InvokeByToken(GE_DomainHandle domain, uint64_t token, int32_t* outResult)
    {
        try
        {
            if (!outResult)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            if (EnsureClrInitialized() != GE_Result_Ok)
                return GE_Result_Fail;

            // Enforce that target domain is currently loaded/valid
            if (domain == 0 || !GameEngine::ScriptingService::Get().IsDomainLoaded(domain))
            {
                return GE_Result_Fail;
            }

            int32_t rc = -1;
            EnsureTokenCallbacksResolved();
            if (g_cbInvokeByToken)
            {
                rc = g_cbInvokeByToken((uint64_t)domain, token, outResult);
            }
            else
            {
                auto& eng = GameEngine::EngineCore::GetInstance();
                auto& clr = eng.GetScriptManager().GetCLRHost();
                rc = clr.InvokeByToken(domain, token, outResult);
            }
#if defined(_DEBUG)
            if (rc == 0)
                g_dbgInvokeByTokenCount.fetch_add(1);
#endif
            return MapManagedRcToGeResult(rc);
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_DebugInvokeByNameNoOut(GE_DomainHandle domain, const char* methodNameUtf8, uint32_t methodNameLen)
    {
        try
        {
            // Clamp to NUL within reported range
            uint32_t n = 0;
            if (methodNameUtf8 && methodNameLen > 0)
            {
                const char* p = methodNameUtf8;
                for (; n < methodNameLen; ++n)
                {
                    if (p[n] == '\0')
                        break;
                }
            }
            const uint32_t effectiveLen = (methodNameUtf8 && methodNameLen > 0) ? (n > 0 ? n : methodNameLen) : 0u;
            if (effectiveLen == 0u)
                return GE_Result_InvalidArg;
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            if (EnsureClrInitialized() != GE_Result_Ok)
                return GE_Result_Fail;

            // Prefer managed callbacks if available (keeps counters consistent), else use CoreCLRHost directly
            EnsureTokenCallbacksResolved();
            auto& eng = GameEngine::EngineCore::GetInstance();
            auto& clr = eng.GetScriptManager().GetCLRHost();
            uint64_t dom2 = (uint64_t)domain;
            if (dom2 == 0ULL)
            {
                dom2 = clr.GetCurrentRuntimeDomainId();
            }

            uint64_t tok = 0ULL;
            int32_t qrc = -1;
            if (g_cbQueryExport)
            {
                qrc = g_cbQueryExport(dom2, methodNameUtf8, effectiveLen, &tok);
            }
            else
            {
                qrc = clr.QueryExport(dom2, methodNameUtf8, effectiveLen, &tok);
            }
            if (qrc != 0)
                return MapManagedRcToGeResult(qrc);
            if (tok == 0ULL)
                return GE_Result_NotFound;

            int32_t outRes = -1;
            int32_t irc = -1;
            if (g_cbInvokeByToken)
            {
                irc = g_cbInvokeByToken(dom2, tok, &outRes);
            }
            else
            {
                irc = clr.InvokeByToken(dom2, tok, &outRes);
            }
            if (irc == 0 && outRes >= 0)
                return GE_Result_Ok;
            return MapManagedRcToGeResult(irc != 0 ? irc : -1);
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    // Phase 1 (Engine SHARED): renamed from GE_PreloadAssemblyContext to break the
    // Engine.dll ↔ GameEngine.Native.dll link cycle. Engine.dll now exports a thin
    // GE_PreloadAssemblyContext dispatcher (Engine/Source/HotReloadNativeStubs.cpp)
    // whose function pointer is installed via the auto-register at the bottom of
    // this file. The body below stays as-is; only the symbol name changes (no
    // GE_API decoration = file-internal linkage = not exported from
    // GameEngine.Native.dll).
    static GE_Result GE_CDECL Impl_PreloadAssemblyContext(const char* assemblyPathUtf8, uint32_t pathLen)
    {
        try
        {
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            if (EnsureClrInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            // Determine if the provided path is missing up-front (for post-call normalization only)
            uint32_t n = 0;
            if (assemblyPathUtf8 && pathLen > 0)
            {
                const char* p = assemblyPathUtf8;
                for (; n < pathLen; ++n)
                {
                    if (p[n] == '\0')
                        break;
                }
            }
            const uint32_t effectiveLen = (assemblyPathUtf8 && pathLen > 0) ? (n > 0 ? n : pathLen) : 0u;
            std::string asmPath = (assemblyPathUtf8 && effectiveLen > 0) ? std::string(assemblyPathUtf8, effectiveLen) : std::string();
            bool pathMissing = asmPath.empty();
            if (!pathMissing)
            {
                std::error_code ec;
                pathMissing = !std::filesystem::exists(std::filesystem::absolute(asmPath), ec);
            }

            EnsureHotReloadCallbacksResolved();
            if (!g_cbPreloadAssemblyContext)
            {
                // Fallback: directly resolve the UCO from CoreBridge and cache it
                auto& eng = GameEngine::EngineCore::GetInstance();
                auto& clr = eng.GetScriptManager().GetCLRHost();
                void* ptr = clr.GetManagedFunction(
                    /*assemblyPath*/ std::string(),
                    /*typeName*/ std::string("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge"),
                    /*methodName*/ std::string("PreloadAssemblyContext"));
                if (ptr)
                    g_cbPreloadAssemblyContext = reinterpret_cast<int (*)(const char*, uint32_t)>(ptr);
            }
            if (!g_cbPreloadAssemblyContext)
                return GE_Result_NotFound;
            int rc = g_cbPreloadAssemblyContext(assemblyPathUtf8, pathLen);
            GE_Result r = MapManagedRcToGeResult(rc);
            // Normalize invalid-path case to GE_Result_InvalidArg even if managed surfaced a generic Fail
            if (pathMissing && r == GE_Result_Fail)
                r = GE_Result_InvalidArg;
            // Ensure a reload-failed diagnostic is raised in invalid-path cases even if managed binding was not ready
            if (pathMissing)
            {
                try
                {
                    std::string reason = std::string("Preload failed (rc=") + std::to_string(rc) + ") path='" + asmPath + "'";
                    (void)GE_NotifyReloadEvent(GE_Reload_Failed, reason.c_str());
                }
                catch (...)
                { /* best-effort only */
                }
            }
            return r;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    static GE_Result GE_CDECL Impl_SwapPreloadedContext(void)
    {
        try
        {
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            if (EnsureClrInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            EnsureHotReloadCallbacksResolved();
            if (!g_cbSwapPreloadedContext)
            {
                auto& eng = GameEngine::EngineCore::GetInstance();
                auto& clr = eng.GetScriptManager().GetCLRHost();
                void* ptr = clr.GetManagedFunction(
                    std::string(),
                    std::string("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge"),
                    std::string("SwapPreloadedContext"));
                if (ptr)
                    g_cbSwapPreloadedContext = reinterpret_cast<int (*)()>(ptr);
            }
            if (!g_cbSwapPreloadedContext)
                return GE_Result_NotFound;
            int rc = g_cbSwapPreloadedContext();
            GE_Result r = MapManagedRcToGeResult(rc);
            if (r == GE_Result_Ok)
            {
                // Synchronize native legacy singleton with the new managed current domain
                auto& eng = GameEngine::EngineCore::GetInstance();
                auto& clr = eng.GetScriptManager().GetCLRHost();
                uint64_t dom = clr.GetCurrentRuntimeDomainId();
                auto& svc = GameEngine::ScriptingService::Get();
                (void)svc.MarkDomainLive(dom);
                (void)svc.SwapRuntimeDomain(dom);
            }
            return r;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    static GE_Result GE_CDECL Impl_CleanupOldContext(const char* assemblyPathUtf8, uint32_t pathLen)
    {
        try
        {
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            if (EnsureClrInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            EnsureHotReloadCallbacksResolved();
            if (!g_cbCleanupOldContext)
            {
                auto& eng = GameEngine::EngineCore::GetInstance();
                auto& clr = eng.GetScriptManager().GetCLRHost();
                void* ptr = clr.GetManagedFunction(
                    std::string(),
                    std::string("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge"),
                    std::string("CleanupOldContext"));
                if (ptr)
                    g_cbCleanupOldContext = reinterpret_cast<int (*)(const char*, uint32_t)>(ptr);
            }
            if (!g_cbCleanupOldContext)
                return GE_Result_NotFound;
            int rc = g_cbCleanupOldContext(assemblyPathUtf8, pathLen);
            return MapManagedRcToGeResult(rc);
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    static GE_Result GE_CDECL Impl_ClearCompilerCache(void)
    {
        try
        {
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            if (EnsureClrInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            EnsureHotReloadCallbacksResolved();
            if (!g_cbClearCompilerCache)
                return GE_Result_NotFound;
            int rc = g_cbClearCompilerCache();
            return MapManagedRcToGeResult(rc);
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    static GE_Result GE_CDECL Impl_GetCompilerStats(void)
    {
        try
        {
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            if (EnsureClrInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            EnsureHotReloadCallbacksResolved();
            if (!g_cbGetCompilerStats)
                return GE_Result_NotFound;
            int rc = g_cbGetCompilerStats();
            return MapManagedRcToGeResult(rc);
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_GetHotReloadMetrics(uint64_t* lastCompileMs, uint64_t* lastSwapMs, int32_t* totalCompiles, int32_t* totalSwaps)
    {
        try
        {
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            if (EnsureClrInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            EnsureHotReloadCallbacksResolved();
            if (!g_cbGetHotReloadMetrics)
            {
                // Fallback: directly resolve the UCO from CoreBridge and cache it
                auto& eng = GameEngine::EngineCore::GetInstance();
                auto& clr = eng.GetScriptManager().GetCLRHost();
                void* ptr = clr.GetManagedFunction(
                    std::string(),
                    std::string("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge"),
                    std::string("GetHotReloadMetrics"));
                if (ptr)
                    g_cbGetHotReloadMetrics = reinterpret_cast<int (*)(uint64_t*, uint64_t*, int32_t*, int32_t*)>(ptr);
            }
            if (!g_cbGetHotReloadMetrics)
                return GE_Result_NotFound;
            int rc = g_cbGetHotReloadMetrics(lastCompileMs, lastSwapMs, totalCompiles, totalSwaps);
            return MapManagedRcToGeResult(rc);
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_ResetHotReloadMetrics(void)
    {
        try
        {
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            if (EnsureClrInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            EnsureHotReloadCallbacksResolved();
            if (!g_cbResetHotReloadMetrics)
            {
                auto& eng = GameEngine::EngineCore::GetInstance();
                auto& clr = eng.GetScriptManager().GetCLRHost();
                void* ptr = clr.GetManagedFunction(
                    std::string(),
                    std::string("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge"),
                    std::string("ResetHotReloadMetrics"));
                if (ptr)
                    g_cbResetHotReloadMetrics = reinterpret_cast<int (*)()>(ptr);
            }
            if (!g_cbResetHotReloadMetrics)
                return GE_Result_NotFound;
            int rc = g_cbResetHotReloadMetrics();
            return MapManagedRcToGeResult(rc);
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_GetHotReloadMetricsEx(uint64_t* lastCompileMs, uint64_t* lastSwapMs, int32_t* totalCompiles, int32_t* totalSwaps, int32_t* totalUnloads)
    {
        try
        {
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            if (EnsureClrInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            EnsureHotReloadCallbacksResolved();
            if (!g_cbGetHotReloadMetricsEx)
            {
                auto& eng = GameEngine::EngineCore::GetInstance();
                auto& clr = eng.GetScriptManager().GetCLRHost();
                void* ptr = clr.GetManagedFunction(
                    std::string(),
                    std::string("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge"),
                    std::string("GetHotReloadMetricsEx"));
                if (ptr)
                    g_cbGetHotReloadMetricsEx = reinterpret_cast<int (*)(uint64_t*, uint64_t*, int32_t*, int32_t*, int32_t*)>(ptr);
            }
            if (g_cbGetHotReloadMetricsEx)
            {
                int rc = g_cbGetHotReloadMetricsEx(lastCompileMs, lastSwapMs, totalCompiles, totalSwaps, totalUnloads);
                return MapManagedRcToGeResult(rc);
            }
            // Fallback to base metrics; report 0 for totalUnloads
            if (!g_cbGetHotReloadMetrics)
            {
                auto& eng = GameEngine::EngineCore::GetInstance();
                auto& clr = eng.GetScriptManager().GetCLRHost();
                void* ptr2 = clr.GetManagedFunction(
                    std::string(),
                    std::string("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge"),
                    std::string("GetHotReloadMetrics"));
                if (ptr2)
                    g_cbGetHotReloadMetrics = reinterpret_cast<int (*)(uint64_t*, uint64_t*, int32_t*, int32_t*)>(ptr2);
            }
            if (!g_cbGetHotReloadMetrics)
                return GE_Result_NotFound;
            int rc2 = g_cbGetHotReloadMetrics(lastCompileMs, lastSwapMs, totalCompiles, totalSwaps);
            if (totalUnloads)
                *totalUnloads = 0;
            return MapManagedRcToGeResult(rc2);
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_ScriptsDomainUnload(GE_DomainHandle domain)
    {
        try
        {
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            if (EnsureClrInitialized() != GE_Result_Ok)
                return GE_Result_Fail;

            // Notify native listeners that the domain is about to unload so they can
            // invalidate any cached managed pointers/tokens immediately.
            {
                auto& svcPre = GameEngine::ScriptingService::Get();
                svcPre.NotifyDomainWillUnload(domain);
            }

            // Invoke managed to unload the domain (prefer callback; fallback to CoreBridge UCO)
            EnsureTokenCallbacksResolved();
            int rc = -1;
            if (g_cbUnloadDomain)
            {

                rc = g_cbUnloadDomain((uint64_t)domain);
            }
            else
            {
                auto& eng = GameEngine::EngineCore::GetInstance();
                auto& clr = eng.GetScriptManager().GetCLRHost();
                rc = clr.UnloadDomain((uint64_t)domain);
            }

            // Regardless of managed path, update native bookkeeping so tokens are invalidated
            auto& svc = GameEngine::ScriptingService::Get();
            svc.NotifyBeforeUnload();
            GE_Result r = svc.UnloadDomain(domain);
            svc.NotifyAfterLoad();
            // Purge any cached tokens for this domain
            PurgeTokenCacheForDomain((uint64_t)domain);

#ifdef _DEBUG
            // Bump native debug counter and tickle managed counters to reflect unload in debug builds
            {
                auto& eng2 = GameEngine::EngineCore::GetInstance();
                auto& clr2 = eng2.GetScriptManager().GetCLRHost();
                clr2.DebugBumpUnloadCounterNative();
                (void)clr2.DebugTickleCounters();
            }
#endif

            (void)rc;
            return r;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    // Debug perf counters: cached UCO pointer
    static long long (*g_cbGetPerfCountersPacked64)() = nullptr;

    GE_API long long GE_CDECL GE_DebugGetPerfCountersPacked64()
    {
        try
        {
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return -1;
            if (EnsureClrInitialized() != GE_Result_Ok)
                return -1;
            if (!g_cbGetPerfCountersPacked64)
            {
                auto& eng = GameEngine::EngineCore::GetInstance();
                auto& clr = eng.GetScriptManager().GetCLRHost();

                void* ptr = clr.GetManagedFunction(
                    std::string(),
                    std::string("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge"),
                    std::string("GetPerfCountersPacked"));
                if (ptr)
                    g_cbGetPerfCountersPacked64 = reinterpret_cast<long long (*)()>(ptr);
            }
            if (!g_cbGetPerfCountersPacked64)
                return -1;
            long long packed = g_cbGetPerfCountersPacked64();
            return packed;
        }
        catch (...)
        {
            return -1;
        }
    }

    // Debug path mask: cached UCO pointer
    static int (*g_cbGetPathMask)() = nullptr;

    GE_API int GE_CDECL GE_DebugGetPathMask()
    {
        try
        {
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return -1;
            if (EnsureClrInitialized() != GE_Result_Ok)
                return -1;
            if (!g_cbGetPathMask)
            {
                auto& eng = GameEngine::EngineCore::GetInstance();

                auto& clr = eng.GetScriptManager().GetCLRHost();
                void* ptr = clr.GetManagedFunction(
                    std::string(),
                    std::string("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge"),
                    std::string("Debug_GetPathMask"));
                if (ptr)
                    g_cbGetPathMask = reinterpret_cast<int (*)()>(ptr);
            }
            if (!g_cbGetPathMask)
                return -1;
            int mask = g_cbGetPathMask();
            return mask;
        }
        catch (...)
        {
            return -1;
        }
    }

    GE_API GE_Result GE_CDECL GE_GetHotReloadEditorMetrics(uint64_t* lastFirstInvokeMs)
    {
        try
        {
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            if (EnsureClrInitialized() != GE_Result_Ok)
                return GE_Result_Fail;
            EnsureHotReloadCallbacksResolved();
            if (!g_cbGetHotReloadEditorMetrics)
                return GE_Result_NotFound;
            int rc = g_cbGetHotReloadEditorMetrics(lastFirstInvokeMs);
            return MapManagedRcToGeResult(rc);
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    // Editor packed metrics: cached UCO pointer
    static unsigned long long (*g_cbGetEditorMetricsPacked64)() = nullptr;

    GE_API uint64_t GE_CDECL GE_GetHotReloadEditorMetricsPacked64()
    {
        try
        {
            if (EnsureEngineInitialized() != GE_Result_Ok)
                return 0ULL;
            if (EnsureClrInitialized() != GE_Result_Ok)
                return 0ULL;
            if (!g_cbGetEditorMetricsPacked64)
            {
                auto& eng = GameEngine::EngineCore::GetInstance();
                auto& clr = eng.GetScriptManager().GetCLRHost();
                void* ptr = clr.GetManagedFunction(
                    std::string(),
                    std::string("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge"),
                    std::string("GetEditorMetricsPacked64"));
                if (ptr)
                    g_cbGetEditorMetricsPacked64 = reinterpret_cast<unsigned long long (*)()>(ptr);
            }
            if (!g_cbGetEditorMetricsPacked64)
                return 0ULL;
            return (uint64_t)g_cbGetEditorMetricsPacked64();
        }
        catch (...)
        {
            return 0ULL;
        }
    }

    // -------- Host state / ScriptsConfig propagation --------

    GE_API GE_Result GE_CDECL GE_SetHostLoggerState(void* loggerState)
    {
        Logger::Log::RedirectToSharedState(
            static_cast<Logger::Log::LoggerState*>(loggerState));
        Logger::Log::Info("GE_SetHostLoggerState: DLL logger now drains the host state");
        return GE_Result_Ok;
    }

    GE_API GE_Result GE_CDECL GE_SetOnDomainWillUnload(GE_DomainWillUnloadFn callback, void* user)
    {
        // Runs in the module that executes GE_ScriptsDomainUnload (GameEngine.Native.dll
        // when the host links the import library), so the callback lands on the
        // ScriptingService instance that actually fires NotifyDomainWillUnload.
        auto& svc = GameEngine::ScriptingService::Get();
        if (callback)
        {
            svc.SetOnDomainWillUnload([callback, user](GE_DomainHandle domain)
                                      { callback(domain, user); });
        }
        else
        {
            svc.SetOnDomainWillUnload({});
        }
        return GE_Result_Ok;
    }

    GE_API GE_Result GE_CDECL GE_SetHostScriptsConfig(const GE_HostScriptsConfig* config)
    {
        if (!config)
            return GE_Result_InvalidArg;

        GameEngine::ScriptsConfig sc;
        sc.enableHotReload = config->enableHotReload != 0;
        sc.enableAsyncHotReload = config->enableAsyncHotReload != 0;
        sc.enableAutoProjectGeneration = config->enableAutoProjectGeneration != 0;
        sc.deferInitialLoad = config->deferInitialLoad != 0;
        sc.disableClr = config->disableClr != 0;
        GameEngine::DllBootstrap::SetHostScriptsConfig(sc);
        return GE_Result_Ok;
    }

    // -------- Debug Metrics --------
    GE_API GE_Result GE_CDECL GE_DebugMetrics_RegisterMonitor(const char* nameUtf8,
                                                              uint32_t nameLen,
                                                              int32_t type,
                                                              const char* unitUtf8,
                                                              uint32_t unitLen)
    {
        try
        {
            if (!nameUtf8 || nameLen == 0)
                return GE_Result_InvalidArg;
            if (type < 0 || type > static_cast<int32_t>(GameEngine::Debug::MonitorType::Percent))
                return GE_Result_InvalidArg;
            std::string_view nameView(nameUtf8, nameLen);
            std::string_view unitView = (unitUtf8 && unitLen > 0)
                                            ? std::string_view(unitUtf8, unitLen)
                                            : std::string_view{};
            GameEngine::Debug::DebugMetrics::Get().RegisterMonitor(
                nameView, static_cast<GameEngine::Debug::MonitorType>(type), unitView);
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

    GE_API GE_Result GE_CDECL GE_DebugMetrics_PushSample(const char* nameUtf8,
                                                         uint32_t nameLen,
                                                         float value)
    {
        try
        {
            if (!nameUtf8 || nameLen == 0)
                return GE_Result_InvalidArg;
            GameEngine::Debug::DebugMetrics::Get().PushSample(
                std::string_view(nameUtf8, nameLen), value);
            return GE_Result_Ok;
        }
        catch (...)
        {
            return GE_Result_Fail;
        }
    }

} // extern "C"

// Phase 1: GameEngine.Native registers its hot-reload implementations with
// Engine.dll's dispatcher table at module load. Engine.dll's exported
// GE_PreloadAssemblyContext / GE_SwapPreloadedContext / GE_CleanupOldContext /
// GE_ClearCompilerCache / GE_GetCompilerStats forward to these pointers; absent
// registration they fall through to no-op stubs (Player / headless test path).
//
// The static initializer runs after Engine.dll's static init (the OS DLL load
// graph guarantees parent loads first), so the call into Engine.dll's exported
// GE_RegisterHotReloadCallbacks resolves safely.
namespace {
struct HotReloadAutoRegister {
    HotReloadAutoRegister() {
        GE_HotReloadCallbacks cb{};
        cb.PreloadAssemblyContext = &Impl_PreloadAssemblyContext;
        cb.SwapPreloadedContext   = &Impl_SwapPreloadedContext;
        cb.CleanupOldContext      = &Impl_CleanupOldContext;
        cb.ClearCompilerCache     = &Impl_ClearCompilerCache;
        cb.GetCompilerStats       = &Impl_GetCompilerStats;
        GE_RegisterHotReloadCallbacks(&cb);
    }
};
HotReloadAutoRegister g_hotReloadAutoRegister;
} // namespace
