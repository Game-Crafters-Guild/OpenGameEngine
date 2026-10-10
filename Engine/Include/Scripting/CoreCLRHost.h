#pragma once

#include "Scripting/ScriptingABI.h"
#include "Types/Types.h"
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef NETHOST_AVAILABLE
// Use actual .NET hosting headers when nethost/hostfxr are available
#include <coreclr_delegates.h>
#include <hostfxr.h>
#include <nethost.h>
#else
// Minimal fallback types when nethost headers are not available (stub builds/tests)
#ifndef CORECLR_DELEGATE_CALLTYPE
#ifdef PLATFORM_WINDOWS
#define CORECLR_DELEGATE_CALLTYPE __cdecl
#else
#define CORECLR_DELEGATE_CALLTYPE
#endif
#endif

#ifdef PLATFORM_WINDOWS
using char_t = wchar_t;
#else
using char_t = char;
#endif

// Forward declarations for compilation when nethost is not available
typedef void* hostfxr_handle;
typedef int32_t (*hostfxr_initialize_for_runtime_config_fn)(const char_t* runtime_config_path, void* parameters, hostfxr_handle* host_context_handle);
typedef int32_t (*hostfxr_get_runtime_delegate_fn)(const hostfxr_handle host_context_handle, int32_t type, void** delegate);
typedef int32_t (*hostfxr_close_fn)(const hostfxr_handle host_context_handle);
typedef int32_t (*load_assembly_and_get_function_pointer_fn)(const char_t* assembly_path,
                                                             const char_t* type_name,
                                                             const char_t* method_name,
                                                             const char_t* delegate_type_name,
                                                             void* reserved,
                                                             void** delegate);
typedef int32_t (*get_function_pointer_fn)(const char_t* type_name,
                                           const char_t* method_name,
                                           const char_t* delegate_type_name,
                                           void* load_context,
                                           void* reserved,
                                           void** delegate);
typedef int32_t (*load_assembly_fn)(const char_t* assembly_path,
                                    void* load_context,
                                    void* reserved);
typedef void (*hostfxr_error_writer_fn)(const char_t*);
typedef void (*hostfxr_set_error_writer_fn)(hostfxr_error_writer_fn);

enum hostfxr_delegate_type
{
    hdt_com_activation,
    hdt_load_in_memory_assembly,
    hdt_winrt_activation,
    hdt_com_register,
    hdt_com_unregister,
    hdt_load_assembly_and_get_function_pointer,
    hdt_get_function_pointer,
    hdt_load_assembly
};
#endif // NETHOST_AVAILABLE

#ifndef UNMANAGEDCALLERSONLY_METHOD
// Match official coreclr_delegates.h: sentinel pointer value, not a string literal
#ifdef PLATFORM_WINDOWS
#define UNMANAGEDCALLERSONLY_METHOD ((const wchar_t*)-1)
#else
#define UNMANAGEDCALLERSONLY_METHOD ((const char_t*)-1)
#endif
#endif

// char_t literal helper: hostfxr uses wide strings on Windows, UTF-8 elsewhere.
#ifdef PLATFORM_WINDOWS
#define GE_HOST_STR(s) L##s
#else
#define GE_HOST_STR(s) s
#endif

namespace GameEngine
{

/**
 * @brief CoreCLR runtime hosting and management
 */
class CoreCLRHost
{
  public:
    CoreCLRHost();
    ~CoreCLRHost();

    /**
     * @brief Initialize CoreCLR runtime
     */
    bool Initialize(const std::filesystem::path& runtimeConfigPath);

    /**
     * @brief Shutdown CoreCLR runtime
     */
    void Shutdown();

    /**
     * @brief Reset static state for test environments
     * This should only be called during shutdown to allow proper reinitialization
     */
    static void ResetStaticState();

    /**
     * @brief Check if runtime is initialized
     */
    bool IsInitialized() const
    {
        return m_Initialized;
    }

    // Native engine library CoreBridge binds this process's engine instance to, overriding the
    // path resolved from the executable directory. Call before InitializeCoreBridge.
    void SetNativeLibraryOverride(const std::string& nativeLibraryPathUtf8);

    /**
     * @brief Hand CoreBridge the host's switches (GE_HostSwitch_* bits, ScriptingABI.h).
     * Delivered at once while the runtime is up, otherwise when it comes up, so a switch
     * set before startup governs CoreBridge.Initialize and one set later takes effect on
     * the next managed call. Call it from the thread that makes the managed calls it
     * governs, between those calls: the word is a plain static on both sides. A production
     * host never sets it; the bits exist for the scripting-ABI suites.
     */
    void SetHostSwitches(uint32 switches);

    /**
     * @brief Check if an assembly is currently loaded
     */
    bool IsAssemblyLoaded() const
    {
        return !m_LoadedAssemblyPath.empty();
    }

    /**
     * @brief Get function pointer from managed assembly
     */
    void* GetManagedFunction(const String& assemblyName,
                             const String& typeName,
                             const String& methodName);

    /**
     * @brief Try calling a native export function
     */
    bool TryCallNativeExport(const String& methodName);

    /**
     * @brief Try calling with delegate signatures (fallback)
     */

    // CoreBridge UCO wrappers
    int32 AcceptPackage(const GE_PackageEntry* entries,
                        uint32 entryCount,
                        uint32 flags,
                        uint64* outDomain);

    // Query current runtime domain identifier from CoreBridge
    uint64 GetCurrentRuntimeDomainId();

    int32 TryCallWithDelegateSignatures(const String& assemblyName,
                                        const String& typeName,
                                        const String& methodName);

    // Invoke a managed method by name in the specified domain (domain routing may be ignored by managed until multi-domain is implemented)
    int32 InvokeInDomain(uint64 domain,
                         const char* methodNameUtf8,
                         uint32 methodNameLen,
                         int32* outResult);

    /**
     * @brief Get runtime information
     */
    String GetRuntimeVersion() const;

    /**
     * @brief Preload and atomically swap a script assembly using CoreBridge UCOs (avoids GE_* ABI re-entrancy)
     * Returns true on success.
     */
    bool PreloadAndSwapFromPath(const std::filesystem::path& assemblyPath);

    int32 GetLastManagedErrorCode() const
    {
        return m_LastManagedErrorCode;
    }
    int32 GetLastCoreBridgeInitResult() const
    {
        return m_LastCoreBridgeInitResult;
    }

    // Invoke a public static method in the currently loaded user scripts assembly via HotReloadManager
    // Returns method result (int) or negative error code
    int32 CallUserScriptsMethod(const String& methodName);

    // Token-based export API
    // Returns 0 on success and sets outToken; negative on failure
    int32 QueryExport(uint64 domain, const char* methodNameUtf8, uint32 methodNameLen, uint64* outToken);
    // Returns 0 on success and writes method result to outResult
    int32 InvokeByToken(uint64 domain, uint64 token, int32* outResult);
    // Managed domain unload (calls CoreBridge.UnloadDomain). Returns 0 on success.
    int32 UnloadDomain(uint64 domain);

#ifdef _DEBUG
    // Debug-only: resolve CoreBridge.GetPerfCounters UCO and invoke it
    int32 GetPerfCounters(int32* outDelegateQuery, int32* outReflectionQuery, int32* outDelegateInvoke, int32* outReflectionInvoke);
    int32 ResetPerfCounters();
    // Debug-only: packed counters (no pointer out-params); returns 64-bit packed value or 0xFFFFFFFFFFFFFFFF on error
    uint64 GetPerfCountersPacked64();
    // Debug-only: helper to invoke by token in managed without out-param crossing boundary
    int32 DebugInvokeByTokenNoOut(uint64 domain, uint64 token);
    // Debug-only: helper to invoke by name entirely inside managed (queries token + invokes), no out-param across boundary
    int32 DebugInvokeByNameNoOut(uint64 domain, const char* methodNameUtf8, uint32 methodNameLen);
    // Debug-only: trivial no-op to validate stack integrity after last interop
    int32 DebugNoopAfterAccept();
    // Debug-only: artificially bump counters without fragile interop
    int32 DebugTickleCounters();
    // Debug-only: unload counter helpers
    int32 GetUnloadCounter();
    int32 ResetUnloadCounter();
    void DebugBumpUnloadCounterNative();
    int32 GetInvokeByTokenPath(); // 0=Unknown, 1=Delegate, 2=Reflection; -3 NotFound if HRM missing
#endif

    /**
     * @brief Resolve an [UnmanagedCallersOnly] export from an assembly loaded into the
     * DEFAULT AssemblyLoadContext (single statics universe, B4). Loads the assembly into
     * Default via hdt_load_assembly on first use, then binds via hdt_get_function_pointer.
     * Falls back to load_assembly_and_get_function_pointer (isolated component context)
     * only when the runtime does not expose the Default-ALC delegates.
     * Public consumers host standalone tool assemblies (e.g. the Unity import
     * converter's EditorHost entry); typeName is assembly-qualified.
     * Returns 0 and writes *outFn on success; nonzero hostfxr code on failure.
     */
    int32 ResolveManagedUco(const std::filesystem::path& assemblyPath,
                            const char_t* typeName,
                            const char_t* methodName,
                            void** outFn);

    /**
     * @brief Initialize CoreBridge managed assembly and register callbacks
     * Loads GameEngine.CoreBridge.dll and calls CoreBridge.Initialize via function pointer
     */
    bool InitializeCoreBridge(const std::filesystem::path& coreBridgeAssemblyPath);

    /**
     * @brief Host-provided dispatcher that schedules a closure onto the engine main
     * thread (ScriptManager wires its main-thread task queue here). When set before
     * CoreBridge initialization, a pump is registered with managed HotReloadManager
     * so [InitializeOnLoad] runs on the engine main thread instead of a thread-pool
     * thread concurrent with rendering (B5).
     */
    void SetMainThreadDispatcher(std::function<void(std::function<void()>)> dispatcher);

    /**
     * @brief Initialize diagnostics sink subscription with CoreBridge
     */
    bool InitializeDiagnosticsSink();

  private:
    /**
     * @brief Resolve (and memoize) an [UnmanagedCallersOnly] export on the CoreBridge type.
     * CoreBridge lives in the default ALC and is never reloaded, so resolved pointers are
     * stable for the process lifetime. Returns nullptr on failure without caching, so a
     * later call can succeed once CoreBridge is available.
     */
    void* GetCoreBridgeExport(const char_t* methodName);

    /// Mirrors m_HostSwitches into CoreBridge.SetHostSwitches; false when CoreBridge is unreachable.
    bool PushHostSwitches();

    /**
     * @brief Registers the native pump-request thunk with managed HotReloadManager
     * when a main-thread dispatcher is set (B5). No-op otherwise.
     */
    void InstallMainThreadPump(const std::filesystem::path& bridgePath, const char_t* typeName);

    /// Invoked by managed code (any thread) when main-thread work is queued.
    static void GE_CDECL MainThreadPumpThunk(void* user);

    /**
     * @brief Load hostfxr library
     */
    bool LoadHostfxr();

    /**
     * @brief Initialize hostfxr
     */
    bool InitializeHostfxr(const std::filesystem::path& runtimeConfigPath);

    /**
     * @brief Get runtime delegate
     */
    bool GetRuntimeDelegate();

  private:
    bool m_Initialized = false;
    std::filesystem::path m_RuntimeConfigPath;
    // intentionally declared once above in public section

    std::filesystem::path m_LoadedAssemblyPath;
    std::filesystem::path m_CoreBridgeAssemblyPath;

    // Idempotency guards
    bool m_CoreBridgeInitialized = false;        // True once CoreBridge.Initialize has been invoked successfully
    bool m_EngineInstanceRegistered = false;     // True once RegisterEngineInstance has been called

    uint32 m_HostSwitches = 0; // GE_HostSwitch_* bits; CoreBridge holds the same word once pushed

    // Last managed error and CoreBridge.Initialize result; valid on all platforms
    int32 m_LastManagedErrorCode = 0;
    int32 m_LastCoreBridgeInitResult = 0;

    // Set via SetNativeLibraryOverride(); when non-empty, InitializeCoreBridge registers this path
    std::string m_NativePathOverride;

#ifdef NETHOST_AVAILABLE
    // CoreCLR hosting handles (valid when nethost/hostfxr are available)
    void* m_HostfxrLib = nullptr;
    hostfxr_initialize_for_runtime_config_fn m_InitForConfig = nullptr;
    hostfxr_get_runtime_delegate_fn m_GetRuntimeDelegate = nullptr;
    hostfxr_close_fn m_Close = nullptr;
    load_assembly_and_get_function_pointer_fn m_LoadAssemblyAndGetFunctionPointer = nullptr;
    // Default-ALC binding pair (single statics universe, B4): load the assembly into
    // AssemblyLoadContext.Default, then bind exports there — never into an
    // IsolatedComponentLoadContext with its own copy of every static.
    get_function_pointer_fn m_GetFunctionPointer = nullptr;
    load_assembly_fn m_LoadAssemblyIntoDefault = nullptr;
    hostfxr_set_error_writer_fn m_SetErrorWriter = nullptr;
    hostfxr_handle m_HostfxrContext = nullptr;
#endif

    // Memoized CoreBridge export pointers (method name → UCO pointer). Cleared on Shutdown.
    std::mutex m_BridgeExportCacheMutex;
    std::unordered_map<std::basic_string<char_t>, void*> m_BridgeExportsByName;

    // Main-thread marshaling (B5): dispatcher supplied by the host (ScriptManager
    // task queue) and the cached managed drain export.
    std::mutex m_MainThreadDispatcherMutex;
    std::function<void(std::function<void()>)> m_MainThreadDispatcher;
    using PumpMainThreadFn = int32 (CORECLR_DELEGATE_CALLTYPE*)();
    PumpMainThreadFn m_PumpMainThreadFn = nullptr;

    // Per-domain token cache for name→token (fast path). Purged on domain unload.
    std::mutex m_TokenCacheMutex;
    std::unordered_map<uint64, std::unordered_map<std::string, uint64>> m_TokenCache;        // name -> token
    std::unordered_map<uint64, std::unordered_map<uint64, std::string>> m_TokenReverseCache; // token -> name

    DISALLOW_COPY_AND_ASSIGN(CoreCLRHost);
};

} // namespace GameEngine
