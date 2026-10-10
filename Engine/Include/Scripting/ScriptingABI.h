#pragma once

// ScriptingABI.h - Stable C ABI for GameEngine native <-> managed interop
// This header defines the minimal, platform-stable interface between the engine (C++)
// and the managed scripting environment (.NET). It intentionally avoids C++ types and
// uses a C ABI (cdecl) for maximum portability and tooling compatibility.

#ifdef __cplusplus
extern "C" {
#endif

// -------- Platform / Export Macros --------
#if defined(_WIN32) || defined(_WIN64)
  #define GE_CDECL __cdecl
  // Export when building a DLL, import when consuming the DLL.
  // When linking statically against Engine (no DLL), define GE_SCRIPTING_STATIC to disable import/export decoration.
  #if defined(GE_SCRIPTING_BUILD)
    #define GE_API __declspec(dllexport)
  #elif defined(GE_SCRIPTING_STATIC)
    #define GE_API
  #else
    #define GE_API __declspec(dllimport)
  #endif
#else
  #define GE_CDECL
  #if defined(GE_SCRIPTING_BUILD)
    #define GE_API __attribute__((visibility("default")))
  #elif defined(GE_SCRIPTING_STATIC)
    #define GE_API
  #else
    #define GE_API __attribute__((visibility("default")))
  #endif
#endif

#include <stdint.h>
#include <stddef.h>

// -------- Basic Types --------
typedef uint64_t GE_Handle;          // Opaque handle for engine objects
typedef uint64_t GE_DomainHandle;    // Opaque handle for a managed domain (ALC)

typedef enum GE_Result {
    GE_Result_Ok = 0,
    GE_Result_Fail = -1,
    GE_Result_InvalidArg = -2,
    GE_Result_NotFound = -3,
    GE_Result_NotInitialized = -4,
    GE_Result_AlreadyInitialized = -5,
    GE_Result_BufferFull = -6
} GE_Result;

typedef enum GE_LogLevel {
    GE_Log_Trace = 0,
    GE_Log_Debug = 1,
    GE_Log_Info  = 2,
    GE_Log_Warn  = 3,
    GE_Log_Error = 4,
    GE_Log_Fatal = 5
} GE_LogLevel;

typedef enum GE_DiagnosticSeverity {
    GE_Diag_Info = 0,
    GE_Diag_Warning = 1,
    GE_Diag_Error = 2
} GE_DiagnosticSeverity;

// UTF-8, non-owning string view
typedef struct GE_StringView {
    const char* data; // may be NULL when length == 0
    uint32_t    length;
} GE_StringView;

// Blittable span of bytes
typedef struct GE_ByteSpan {
    const uint8_t* data; // non-owning
    uint32_t       length;
} GE_ByteSpan;

// -------- Configuration --------
typedef struct GE_ScriptingConfig {
    GE_StringView runtimeConfigPath;   // e.g., path to *.runtimeconfig.json
    GE_StringView coreBridgeAssembly;  // e.g., "GameEngine.CoreBridge.dll"
    int           enableEditorDomain;  // 0/1
    // Optional explicit native library binding (avoids CWD heuristics)
    GE_StringView nativeLibraryPath;   // optional, absolute path to the native engine library
} GE_ScriptingConfig;

// -------- Diagnostics --------
typedef struct GE_Diagnostic {
    GE_DiagnosticSeverity severity;
    int32_t code;          // compiler code if available, else 0
    GE_StringView message; // human readable message (UTF-8)
    GE_StringView file;    // source file path (UTF-8)
    int32_t line;          // 1-based, or 0 if N/A
    int32_t column;        // 1-based, or 0 if N/A
} GE_Diagnostic;

typedef enum GE_CompilationStage {
    GE_Comp_Started = 0,
    GE_Comp_Progress = 1,
    GE_Comp_Completed = 2
} GE_CompilationStage;

typedef enum GE_ReloadStage {
    GE_Reload_Started = 0,
    GE_Reload_Swapping = 1,
    GE_Reload_Completed = 2,
    GE_Reload_Failed = 3
} GE_ReloadStage;

// Callback for structured diagnostics/events into native editor/runtime UI
// All callbacks are optional; userData is passed through verbatim.
typedef void (GE_CDECL *GE_OnCompilationEventFn)(GE_CompilationStage stage,
                                                 float progress01, // 0..1 for Progress, 0/1 otherwise
                                                 const GE_Diagnostic* diags, // array for Completed
                                                 uint32_t diagCount,
                                                 void* userData);

typedef void (GE_CDECL *GE_OnReloadEventFn)(GE_ReloadStage stage,
                                            const char* reasonUtf8, // may be NULL
                                            void* userData);

typedef struct GE_DiagnosticsSink {
    GE_OnCompilationEventFn onCompilationEvent; // may be NULL
    GE_OnReloadEventFn      onReloadEvent;      // may be NULL
    void*                   userData;           // opaque
} GE_DiagnosticsSink;

// -------- Managed Callback Registration --------
// Managed code can export function pointers (via [UnmanagedCallersOnly])
// and register them with the engine under a stable ID. Signatures are determined by ID.
typedef uint32_t GE_CallbackId; // stable semantic IDs

GE_API GE_Result GE_CDECL GE_RegisterManagedCallback(GE_CallbackId id, void* fnPtr);
GE_API GE_Result GE_CDECL GE_GetManagedCallback(GE_CallbackId id, void** outFnPtr);

// Example well-known callback IDs (reserve 0..1023 for core)
enum {
    GE_CB_LogForwarder = 1,    // void(LogLevel level, const char* msg, uint32_t len)
    GE_CB_OnBeforeUnload = 2,  // void()
    GE_CB_OnAfterLoad = 3,     // void()
    // HotReload typed endpoints (managed UCOs)
    GE_CB_PreloadAssemblyContext = 10,      // int(const char* path, uint32_t len)
    GE_CB_SwapPreloadedContext   = 11,      // int()
    GE_CB_CleanupOldContext      = 12,      // int(const char* path, uint32_t len)
    GE_CB_ClearCompilerCache     = 13,      // int()
    GE_CB_GetCompilerStats       = 14,      // int()
    GE_CB_GetHotReloadMetrics   = 15,      // int(uint64* lastCompileMs, uint64* lastSwapMs, int32* totalCompiles, int32* totalSwaps)
    GE_CB_ResetHotReloadMetrics = 16,      // int()
    GE_CB_GetHotReloadMetricsEx = 17,      // int(uint64* lastCompileMs, uint64* lastSwapMs, int32* totalCompiles, int32* totalSwaps, int32* totalUnloads)
    GE_CB_GetHotReloadEditorMetrics = 18,  // int(uint64* lastFirstInvokeMs)

    // Token fast path
    GE_CB_QueryExport            = 20,      // int(uint64 domain, const char* name, uint32 len, uint64* outToken)
    GE_CB_InvokeByToken          = 21,      // int(uint64 domain, uint64 token, int32* outResult)
    // Domain management
    GE_CB_UnloadDomain           = 22,      // int(uint64 domain)

    // Editor Play Mode lifecycle (V1 scaffolding)
    GE_CB_PlayMode_OnEnter       = 30,      // int()
    GE_CB_PlayMode_OnExit        = 31,      // int()
    GE_CB_PlayMode_Tick          = 32       // int(float deltaSeconds)
};

// -------- Initialization / Shutdown --------

// -------- ABI Version --------
// Returns ABI version encoded as 0xMMMMmmmm (M=major, m=minor)
GE_API uint32_t GE_CDECL GE_ScriptingGetAbiVersion(void);


    // -------- Capabilities --------
    typedef uint32_t GE_CapabilitiesMask;
    enum {
        GE_Cap_HasPackageLoader   = 1u << 0,
        GE_Cap_HasDomainSwap      = 1u << 1,
        GE_Cap_HasInvokeByName    = 1u << 2,
    };
    GE_API GE_Result GE_CDECL GE_Capabilities(uint32_t* outMask);

// -------- Versioned Function Table Bootstrap --------
// Major is bumped on breaking changes (a changed layout, a removed export). Minor for additive changes.
#define GE_ABI_VERSION_ENCODE(major, minor)   ((((major) & 0xFFFFu) << 16) | ((minor) & 0xFFFFu))
#define GE_ABI_VERSION_MAJOR(v)               (((v) >> 16) & 0xFFFFu)
#define GE_ABI_VERSION_MINOR(v)               ((v) & 0xFFFFu)

// Current ABI version exposed via GE_GetInterface
// 1.2 adds value/scrollX/scrollY to GE_UIEventData — additive, so a minor bump by this
// header's own rule. Engine and managed ship together from this repo, so the practical
// requirement is that both are rebuilt.
// 1.3 adds text/textLen to GE_UIEventData plus the typed element value accessors
// (GE_UIElement_GetValueFloat and friends) — additive, minor bump for the same reason.
// 2.0 removes the per-project domain exports (GE_ScriptsDomainSwapForProject,
// GE_ScriptsDomainUnloadForProject, GE_GetCurrentRuntimeDomainForProject,
// GE_GetCurrentEditorDomainForProject) and the project id of
// GE_ScriptsDomainCreateFromPackage: one process hosts one project. A removal is
// breaking, so the major moves; engine and managed are rebuilt together.
// 3.0 removes projectIdUtf8 from GE_ScriptingConfig: the host names only the native
// library its one engine instance binds. A changed layout is breaking, so the major moves.
// 3.1 adds GE_Model_GetExtras (ModelABI.h) — additive, minor bump.
// 3.2 adds GE_Animator_PollEventsOnEntity and GE_AnimationEventRecord (AnimatorABI.h) — additive, minor bump.
static const uint32_t GE_ABI_VERSION_CURRENT = GE_ABI_VERSION_ENCODE(3u, 2u);

// Forward declarations for function pointer types used in the interface
typedef GE_Result (GE_CDECL *GE_Log_Fn)(GE_LogLevel level, const char* msg, uint32_t msgLen);
typedef GE_Result (GE_CDECL *GE_RegisterManagedCallback_Fn)(GE_CallbackId id, void* fnPtr);
typedef GE_Result (GE_CDECL *GE_GetManagedCallback_Fn)(GE_CallbackId id, void** outFnPtr);
typedef GE_Result (GE_CDECL *GE_SubscribeDiagnostics_Fn)(const GE_DiagnosticsSink* sink);
typedef GE_Result (GE_CDECL *GE_UnsubscribeDiagnostics_Fn)(const GE_DiagnosticsSink* sink);
    // Allow raising diagnostics through the subscribed sink via the interface table (no DllImport required)
    typedef GE_Result (GE_CDECL *GE_NotifyCompilationEvent_Fn)(GE_CompilationStage stage, float progress01, const GE_Diagnostic* diags, uint32_t diagCount);
    typedef GE_Result (GE_CDECL *GE_NotifyReloadEvent_Fn)(GE_ReloadStage stage, const char* reasonUtf8);

typedef GE_Result (GE_CDECL *GE_GetAssetCount_Fn)(int32_t* outCount);
// Domain control
typedef GE_Result (GE_CDECL *GE_ScriptsDomainSwap_Fn)(GE_DomainHandle newDomain);
typedef GE_Result (GE_CDECL *GE_ScriptsDomainUnload_Fn)(GE_DomainHandle domain);


	// HotReload fast path API (typed, zero-string): optional endpoints
	typedef GE_Result (GE_CDECL *GE_PreloadAssemblyContext_Fn)(const char* assemblyPathUtf8, uint32_t pathLen);
	typedef GE_Result (GE_CDECL *GE_SwapPreloadedContext_Fn)();
        typedef GE_Result (GE_CDECL *GE_GetHotReloadMetrics_Fn)(uint64_t* lastCompileMs, uint64_t* lastSwapMs, int32_t* totalCompiles, int32_t* totalSwaps);
        typedef GE_Result (GE_CDECL *GE_ResetHotReloadMetrics_Fn)();
        // Extended metrics (additive, optional): includes totalUnloads
        typedef GE_Result (GE_CDECL *GE_GetHotReloadMetricsEx_Fn)(uint64_t* lastCompileMs, uint64_t* lastSwapMs, int32_t* totalCompiles, int32_t* totalSwaps, int32_t* totalUnloads);
        typedef GE_Result (GE_CDECL *GE_GetHotReloadEditorMetrics_Fn)(uint64_t* lastFirstInvokeMs);

	typedef GE_Result (GE_CDECL *GE_CleanupOldContext_Fn)(const char* assemblyPathUtf8, uint32_t pathLen);
	typedef GE_Result (GE_CDECL *GE_ClearCompilerCache_Fn)();
	typedef GE_Result (GE_CDECL *GE_GetCompilerStats_Fn)();

typedef GE_Result (GE_CDECL *GE_ECS_GetWorldHandle_Fn)(GE_Handle* outWorld);
typedef GE_Result (GE_CDECL *GE_ECS_GetEntityCount_Fn)(GE_Handle world, int32_t* outCount);

typedef struct GE_Interface_v1 {
    uint32_t sizeBytes; // sizeof(GE_Interface_v1)
    uint32_t abiVersion; // GE_ABI_VERSION_CURRENT
    // Core
    GE_Log_Fn                        Log;
    GE_RegisterManagedCallback_Fn    RegisterManagedCallback;
        // Emit diagnostics/events through the subscribed sink
        GE_NotifyCompilationEvent_Fn    NotifyCompilationEvent;
        GE_NotifyReloadEvent_Fn         NotifyReloadEvent;

    GE_GetManagedCallback_Fn         GetManagedCallback;
    // Diagnostics
    GE_SubscribeDiagnostics_Fn       SubscribeDiagnostics;
    GE_UnsubscribeDiagnostics_Fn     UnsubscribeDiagnostics;
    // Assets
    GE_GetAssetCount_Fn              GetAssetCount;
    // Domains
    GE_ScriptsDomainSwap_Fn          ScriptsDomainSwap;
    GE_ScriptsDomainUnload_Fn        ScriptsDomainUnload;
    // ECS
    GE_ECS_GetWorldHandle_Fn         ECS_GetWorldHandle;
    GE_ECS_GetEntityCount_Fn         ECS_GetEntityCount;
} GE_Interface_v1;

// Bootstrap entry: returns a pointer to the requested interface version
// If abiVersion is incompatible, returns GE_Result_Fail and nulls outTable.
GE_API GE_Result GE_CDECL GE_GetInterface(uint32_t abiVersion, const void** outTable, uint32_t* outSizeBytes);

GE_API GE_Result GE_CDECL GE_ScriptingInitialize(const GE_ScriptingConfig* config);
GE_API GE_Result GE_CDECL GE_ScriptingShutdown(void);

// -------- ScriptsConfig for the standalone bootstrap --------
// Standalone hosting only: when managed code loads GameEngine.Native without a native
// host (e.g. `dotnet test`), the first ABI call initializes the engine and applies this
// config to it. A no-op in a process whose host initializes the engine itself: that
// host configures the engine before Initialize, and the ABI never starts an engine
// such a process has run.
typedef struct GE_HostScriptsConfig
{
    uint8_t enableHotReload;
    uint8_t enableAsyncHotReload;
    uint8_t enableAutoProjectGeneration;
    uint8_t deferInitialLoad;
    uint8_t disableClr;
    uint8_t _pad[3];
} GE_HostScriptsConfig;

GE_API GE_Result GE_CDECL GE_SetHostScriptsConfig(const GE_HostScriptsConfig* config);

// Adopt the host module's logger state so the DLL-local Logger (Engine links
// static -> per-module singleton) drains into the same sinks/ring as the host.
// Without this, DLL-side logs are invisible to the host's console/MCP log tail.
// Pass the host's GameEngine::GetEngineLoggerState(), at host startup before any
// DLL-side logging matters.
GE_API GE_Result GE_CDECL GE_SetHostLoggerState(void* loggerState);

// -------- Domain-unload notification --------
// Registers a host callback fired immediately before a managed scripting domain is
// unloaded (GE_ScriptsDomainUnload), so the host
// can invalidate cached managed export pointers. This MUST be registered through the
// ABI, not via ScriptingService::SetOnDomainWillUnload directly: ScriptingService is
// an inline-static singleton, so the host EXE and GameEngine.Native.dll each have
// their own instance — the unload runs in the DLL, which never sees a callback set
// on the EXE's copy. Pass NULL to unregister. May fire from the unloading thread.
typedef void(GE_CDECL* GE_DomainWillUnloadFn)(GE_DomainHandle domain, void* user);
GE_API GE_Result GE_CDECL GE_SetOnDomainWillUnload(GE_DomainWillUnloadFn callback, void* user);

// -------- Input (optional; returns NotInitialized if host has no InputSystem) --------
typedef struct GE_InputActionState
{
    uint8_t pressed;
    uint8_t justPressed;
    uint8_t justReleased;
    uint8_t _pad0;
    float value;
} GE_InputActionState;

GE_API GE_Result GE_CDECL GE_Input_GetActionState(uint64_t actionId, GE_InputActionState* outState);
GE_API GE_Result GE_CDECL GE_Input_IsKeyDown(int32_t key, int32_t* outDown);
GE_API GE_Result GE_CDECL GE_Input_WasKeyPressed(int32_t key, int32_t* outPressed);
GE_API GE_Result GE_CDECL GE_Input_WasKeyReleased(int32_t key, int32_t* outReleased);
// Where the last move placed the pointer, in play-surface pixels; (0,0) before
// any move, and standing after the pointer leaves the window or the window
// loses focus. A reader gates on the two states below rather than on the
// position: hover-style reads want the pointer in the window, and gestures that
// act on the world (edge scrolling, drags, wheel zoom) want the window focused
// as well.
GE_API GE_Result GE_CDECL GE_Input_GetMousePosition(float* outX, float* outY);
GE_API GE_Result GE_CDECL GE_Input_IsPointerInWindow(int32_t* outInWindow);
GE_API GE_Result GE_CDECL GE_Input_IsWindowFocused(int32_t* outFocused);
GE_API GE_Result GE_CDECL GE_Input_IsMouseButtonDown(int32_t button, int32_t* outDown);
// Frame-snapshot edge queries (mouse mirror of WasKeyPressed/WasKeyReleased).
// Both report true for a press+release pair completing inside one frame — the
// case an IsMouseButtonDown poll silently loses.
GE_API GE_Result GE_CDECL GE_Input_WasMouseButtonPressed(int32_t button, int32_t* outPressed);
GE_API GE_Result GE_CDECL GE_Input_WasMouseButtonReleased(int32_t button, int32_t* outReleased);

// User-defined action/context authoring from managed code (optional).
// These operate on the runtime input sink (Engine::GetRuntimeInput) — in the
// editor the game's own InputSystem, elsewhere the application's — and are
// intended for gameplay scripts. An action registered in an enabled context is
// readable through GE_Input_GetActionState without pushing anything; pushing
// only orders priority between contexts.
GE_API GE_Result GE_CDECL GE_Input_RegisterAction(uint64_t contextId, uint64_t actionId, int32_t isAxis);
GE_API GE_Result GE_CDECL GE_Input_RemoveAction(uint64_t contextId, uint64_t actionId);
GE_API GE_Result GE_CDECL GE_Input_ClearBindings(uint64_t contextId, uint64_t actionId);
GE_API GE_Result GE_CDECL GE_Input_Bind(uint64_t contextId,
                                        uint64_t actionId,
                                        int32_t deviceType,
                                        int32_t code,
                                        float scale,
                                        int32_t requiredMods,
                                        int32_t forbiddenMods);
GE_API GE_Result GE_CDECL GE_Input_PushContext(uint64_t contextId);
GE_API GE_Result GE_CDECL GE_Input_PopContext(uint64_t contextId);
GE_API GE_Result GE_CDECL GE_Input_SetContextEnabled(uint64_t contextId, int32_t enabled);

// -------- Game UI --------
// Element access for gameplay scripts, split by layer.
//
// GE_GameUI_* is the DOCUMENT layer and the only place an entity id appears: mapping an
// entity to a mounted UIDocument subtree is what the gameplay GameUIHost is for. It has
// exactly one function — resolve an element and report its instance id.
//
// GE_UIElement_* is the ELEMENT layer: an instance id and nothing else, no entity, no
// document, no ECS. Every call re-resolves the live element through the published host's
// UIManager (an O(1) index probe plus a root-reachability filter), so a destroyed or
// detached element just reports not-found — the ABI never hands managed code a raw
// element pointer. Instance ids are process-wide and never reused, so an id can never
// come to name a different element.
//
// GE_UI_* is the third and smallest layer: the UI module itself, addressing neither an
// entity nor an element. It exists for the one thing a caller must ask before it holds
// any element — what a tag NAME hashes to — so nothing outside the engine has to
// re-implement the hash.
//
// See UIElementABI.cpp for the safety contract. Managed counterpart:
// GameEngine.Scripting.Ui.

// What a UI event carries across the boundary. Blittable and passed by pointer; the
// callee must not retain it past the call. Deliberately narrow — the fields every
// event this ABI exposes can actually fill. `mods` mirrors UIEvent::Mods; `x`/`y` are
// the pointer position in the host's UI space and are ZERO for events that have no
// position (kEventButtonClick from keyboard activation, focus events).
typedef struct GE_UIEventData
{
    uint64_t elementInstanceId; // the element whose handler is running (CurrentTarget)
    uint64_t eventId;           // the hashed EventId, so one callback can serve several events
    float x;
    float y;
    int32_t button;
    int32_t mods;
    // The control's new value, for UI.ValueChanging / UI.ValueChanged. A bool control
    // reports 0 or 1. Zero for every event that carries no value.
    float value;
    // UI.Scroll: the wheel delta. UI.ScrollOffsetChanged: the settled scroll offset.
    // Zero for events that involve no scrolling.
    float scrollX;
    float scrollY;
    // The control's new value as UTF-8 bytes with an explicit length — NOT NUL-terminated —
    // for UI.ValueChanging / UI.ValueChanged on string-valued controls (a text field's text,
    // a dropdown's selected option VALUE). Null/0 for every event that carries no text.
    // VALID ONLY FOR THE DURATION OF THE CALLBACK: it points at a dispatch-owned copy that
    // is stable however a handler mutates the control (ScopedValueText). A binding that
    // keeps the text must copy it before the callback returns.
    const char* text;
    uint32_t textLen;
} GE_UIEventData;

// THE LAYOUT IS THE CONTRACT. The managed mirror (GameEngine.Scripting.Ui.NativeEventData,
// Managed/Scripting.ABI/UI/Ui.cs) is bound as `NativeEventData*` and read by RUNTIME LAYOUT —
// no marshalling runs — so inserting, reordering or resizing a field here does not fail to
// compile on the managed side, it silently re-points every managed read. These pins turn that
// into a build break. The mirror's own half is UiEventDataLayoutTests (Managed/InteropTests);
// the two are edited together or not at all.
//
// The tail is the one place pointer width shows. On 64-bit targets `scrollY` ends at 44 and
// `text` aligns up to 48, leaving four implicit bytes before it and four more after `textLen`.
// On wasm32 the 4-byte pointer packs straight in at 44 and the struct rounds up to 56 on the
// uint64 fields' alignment. The managed mirror's `nint Text` shifts identically, so the two
// sides agree at either width — the pins below just have to know both.
//
// THE BLIND SPOT IS THAT SAME TAIL PADDING, and it is the one change these pins cannot see.
// On 64-bit, `textLen` ends at 60 and the struct rounds up to 64, so a field of up to four
// bytes appended after it lands at offset 60 inside padding that already existed: sizeof stays
// 64, every offset above is unchanged, and both halves of the pin stay green while the two
// sides have stopped agreeing about what byte 60 means. (wasm32 has the same four bytes at
// 52.) Appending to this struct is therefore a change that must be mirrored by hand and pinned
// by hand — add the offset assert for the new field on both sides, or the next four bytes are
// silent.
#ifdef __cplusplus
static_assert(offsetof(GE_UIEventData, elementInstanceId) == 0, "elementInstanceId moved");
static_assert(offsetof(GE_UIEventData, eventId) == 8, "eventId moved");
static_assert(offsetof(GE_UIEventData, x) == 16, "x moved");
static_assert(offsetof(GE_UIEventData, y) == 20, "y moved");
static_assert(offsetof(GE_UIEventData, button) == 24, "button moved");
static_assert(offsetof(GE_UIEventData, mods) == 28, "mods moved");
static_assert(offsetof(GE_UIEventData, value) == 32, "value moved");
static_assert(offsetof(GE_UIEventData, scrollX) == 36, "scrollX moved");
static_assert(offsetof(GE_UIEventData, scrollY) == 40, "scrollY moved");
#if UINTPTR_MAX == 0xFFFFFFFFu
static_assert(sizeof(GE_UIEventData) == 56, "GE_UIEventData size changed");
static_assert(offsetof(GE_UIEventData, text) == 44, "text moved");
static_assert(offsetof(GE_UIEventData, textLen) == 48, "textLen moved");
#else
static_assert(sizeof(GE_UIEventData) == 64, "GE_UIEventData size changed");
static_assert(offsetof(GE_UIEventData, text) == 48, "text moved");
static_assert(offsetof(GE_UIEventData, textLen) == 56, "textLen moved");
#endif
#endif

typedef void(GE_CDECL* GE_UIEventCallback)(const GE_UIEventData* ev, void* user);

// entityId == 0 searches every mounted document (lowest entity id wins); a specific id
// scopes to that one UIDocument. GE_Result_NotFound until the subtree has mounted.
GE_API GE_Result GE_CDECL GE_GameUI_FindElement(uint64_t entityId, const char* id, uint64_t* outInstanceId);

GE_API GE_Result GE_CDECL GE_UIElement_SetWidthPercent(uint64_t instanceId, float pct);
GE_API GE_Result GE_CDECL GE_UIElement_SetLabelText(uint64_t instanceId, const char* text);
GE_API GE_Result GE_CDECL GE_UIElement_SetClass(uint64_t instanceId, const char* className, int32_t on);
// Liveness as a QUERY, not as a failure: Ok with *outAlive 0 or 1. A caller asking
// "is this still there" is not making a mistake when the answer is no.
GE_API GE_Result GE_CDECL GE_UIElement_IsAlive(uint64_t instanceId, int32_t* outAlive);
// The element's registered tag id — the identity a binding maps to its own type.
// *outTagId is 0 for a live element whose C++ type was never registered with a tag.
GE_API GE_Result GE_CDECL GE_UIElement_GetTagId(uint64_t instanceId, uint64_t* outTagId);
// Tag NAME -> tag id, so a binding never re-implements the hash. 0 for an unknown tag.
GE_API GE_Result GE_CDECL GE_UI_GetTagId(const char* tagLower, uint64_t* outTagId);
// Subscribe to one event by NAME ("UI.ButtonClick", "UI.MouseMove", ...). The name is
// hashed natively, so there is one source of truth for event ids and no binding-side
// enum to drift; an unknown name is rejected rather than becoming a subscription that
// can never fire. The (eventName, token) pair identifies the subscription: handler keys
// are unique within an element's lifetime but not across event ids.
GE_API GE_Result GE_CDECL GE_UIElement_RegisterEvent(uint64_t instanceId, const char* eventName,
                                                     GE_UIEventCallback cb, void* user, uint64_t* outToken);
GE_API GE_Result GE_CDECL GE_UIElement_UnregisterEvent(uint64_t instanceId, const char* eventName, uint64_t token);

// -------- Typed element value accessors --------
// One export per value SHAPE, resolved against the control's Field<T> instantiation, so a
// binding surfaces them as ordinary typed properties with no string parse on the path.
// Implemented in UIElementValueABI.cpp;
// the type rules and the notify contract are documented there.
//
// `notify` on every setter: non-zero routes the control's own SetValue (its clamping,
// quantisation and its own notification contract), zero routes SetValueWithoutNotify.
GE_API GE_Result GE_CDECL GE_UIElement_GetValueFloat(uint64_t instanceId, float* outValue);
GE_API GE_Result GE_CDECL GE_UIElement_SetValueFloat(uint64_t instanceId, float value, int32_t notify);
GE_API GE_Result GE_CDECL GE_UIElement_GetValueBool(uint64_t instanceId, int32_t* outValue);
GE_API GE_Result GE_CDECL GE_UIElement_SetValueBool(uint64_t instanceId, int32_t value, int32_t notify);
// Copies min(*outLen, bufferLen) UTF-8 bytes into buffer — no NUL terminator — and always
// reports the value's full byte length in *outLen; a caller whose buffer was too small
// retries with one that is at least *outLen. buffer may be null only when bufferLen is 0.
GE_API GE_Result GE_CDECL GE_UIElement_GetValueText(uint64_t instanceId, char* buffer, int32_t bufferLen,
                                                    int32_t* outLen);
GE_API GE_Result GE_CDECL GE_UIElement_SetValueText(uint64_t instanceId, const char* text, int32_t notify);
GE_API GE_Result GE_CDECL GE_UIElement_GetScrollOffset(uint64_t instanceId, float* outX, float* outY);
GE_API GE_Result GE_CDECL GE_UIElement_SetScrollX(uint64_t instanceId, float x);
GE_API GE_Result GE_CDECL GE_UIElement_SetScrollY(uint64_t instanceId, float y);
GE_API GE_Result GE_CDECL GE_UIElement_GetDropdownSelectedIndex(uint64_t instanceId, int32_t* outIndex);
GE_API GE_Result GE_CDECL GE_UIElement_SetDropdownSelectedIndex(uint64_t instanceId, int32_t index, int32_t notify);
// Same buffer contract as GE_UIElement_GetValueText. The LABEL is what the header shows;
// the option's VALUE is the field value, read through GE_UIElement_GetValueText.
GE_API GE_Result GE_CDECL GE_UIElement_GetDropdownSelectedLabel(uint64_t instanceId, char* buffer,
                                                                int32_t bufferLen, int32_t* outLen);

// -------- Element TYPES defined outside C++ --------
// The lifecycle layer: who may define element types, which tags they own, and what happens
// to the live elements when their defining load context unloads. Addresses owners and tags,
// never an element — the per-element surface above is a different layer.
//
// Owner ids are MINTED here and validated on every call that takes one, because a script can
// declare its own DllImport and pass any number. An id that was never minted, or that has
// already been released, is refused with GE_Result_InvalidArg and reported.

// A fresh owner id for one managed load context. Never 0. The caller must hand it back
// through GE_UI_ReleaseTypeOwner when its context unloads — and not before the reload
// verdict, because GE_UI_NotifyReloadCompleted needs the same id still live.
GE_API GE_Result GE_CDECL GE_UI_AcquireTypeOwner(uint64_t* outOwner);
// Retire an owner id, permanently. GE_Result_InvalidArg if it was not live, which is what a
// double release or a forged id looks like from here.
GE_API GE_Result GE_CDECL GE_UI_ReleaseTypeOwner(uint64_t owner);
// Install the calls into the defining language. Process-wide and owner-LESS by design: it is
// installed once from a context that is never unloaded, so the pointers cannot dangle across
// a reload. Only the handles they hand back are perishable.
//
// performRegistrations is the registration window's callback: the engine invokes it on the main
// thread in answer to GE_UI_RequestTypeRegistrationWindow, and it is the only context from which
// GE_UI_RegisterElementType and GE_UI_NotifyReloadCompleted may be called.
GE_API GE_Result GE_CDECL GE_UI_SetElementTypeCallbacks(
    intptr_t (GE_CDECL* create)(uint64_t tagId, uint64_t instanceId),
    void (GE_CDECL* applyAttribute)(intptr_t handle, const char* name, const char* value),
    void (GE_CDECL* release)(intptr_t handle),
    void (GE_CDECL* performRegistrations)());
// Ask for a main-thread slot to register in. Callable from ANY thread — it is what a reload
// handler calls instead of registering from wherever it happens to be running.
GE_API GE_Result GE_CDECL GE_UI_RequestTypeRegistrationWindow(void);
// Register one tag for this owner. *outTagId is 0 when the tag was REFUSED — it belongs to
// the engine or to another context — and that is GE_Result_Ok, not an error: one bad tag
// among many must not fail the whole registration pass.
//
// MAIN THREAD ONLY: call it from inside the performRegistrations callback. Off-main the whole
// batch is refused (every *outTagId 0) and the refusal is logged.
GE_API GE_Result GE_CDECL GE_UI_RegisterElementType(uint64_t owner, const char* tagName, uint64_t* outTagId);
// The owner's context is unloading. Releases its instances, marks its elements Pending and
// unregisters its tags. *outOrphaned receives how many elements went Pending.
GE_API GE_Result GE_CDECL GE_UI_OrphanElementTypes(uint64_t owner, uint64_t* outOrphaned);
// A reload finished: whatever of this owner's elements did not come back is now Faulted
// rather than merely waiting. Call it while the owner is STILL LIVE, then release the id.
//
// MAIN THREAD ONLY, and after the batch — same window, same call, registrations first.
GE_API GE_Result GE_CDECL GE_UI_NotifyReloadCompleted(uint64_t owner);

// -------- Domains (AssemblyLoadContext) --------
// NOTE: Deprecated in favor of GE_ScriptsDomainCreateFromPackage. Reserved for future implementation.
// GE_API GE_Result GE_CDECL GE_ScriptsDomainLoadFromMemory(const uint8_t* asmBytes,
//                                                          uint32_t asmLen,
//                                                          const uint8_t* pdbBytes,
//                                                          uint32_t pdbLen,
//                                                          GE_DomainHandle* outDomain);

GE_API GE_Result GE_CDECL GE_ScriptsDomainSwap(GE_DomainHandle newDomain);
GE_API GE_Result GE_CDECL GE_ScriptsDomainUnload(GE_DomainHandle domain);

// Optional: obtain the process's current runtime/editor domains (one process hosts one project)
GE_API GE_Result GE_CDECL GE_GetCurrentRuntimeDomain(GE_DomainHandle* outDomain);
GE_API GE_Result GE_CDECL GE_GetCurrentEditorDomain(GE_DomainHandle* outDomain);

// -------- Diagnostics / Events --------
GE_API GE_Result GE_CDECL GE_SubscribeDiagnostics(const GE_DiagnosticsSink* sink);
GE_API GE_Result GE_CDECL GE_UnsubscribeDiagnostics(const GE_DiagnosticsSink* sink);
// Optional helpers to raise events from managed or native test harness
GE_API GE_Result GE_CDECL GE_NotifyCompilationEvent(GE_CompilationStage stage,
                                                    float progress01,
                                                    const GE_Diagnostic* diags,
                                                    uint32_t diagCount);
GE_API GE_Result GE_CDECL GE_NotifyReloadEvent(GE_ReloadStage stage,
                                               const char* reasonUtf8);

// -------- Representative Engine APIs (typed C ABI) --------
// Logging (UTF-8 message)
GE_API GE_Result GE_CDECL GE_Log(GE_LogLevel level, const char* msg, uint32_t msgLen);

// Assets
GE_API GE_Result GE_CDECL GE_GetAssetCount(int32_t* outCount);

// ECS (examples; operate on opaque handles and blittable arrays)
GE_API GE_Result GE_CDECL GE_ECS_GetWorldHandle(GE_Handle* outWorld);
GE_API GE_Result GE_CDECL GE_ECS_GetEntityCount(GE_Handle world, int32_t* outCount);



// -------- Package loader and invocation (v1.2 additions) --------
// C-compatible package entry matching managed layout
typedef struct GE_PackageEntry {
    GE_ByteSpan assembly; // required
    GE_ByteSpan pdb;      // optional
} GE_PackageEntry;

// Flags for domain creation
enum {
    GE_Pkg_Collectible = 1u << 0,
    GE_Pkg_MemoryOnly  = 1u << 1,
    GE_Pkg_Editor      = 1u << 2,
};

// -------- Host switches for the managed bridge --------
// The native host's selectors for CoreBridge, delivered by CoreCLRHost::SetHostSwitches.
// Each one is a path selector or a fault injection for the scripting-ABI suites; a
// production host leaves the word clear.
enum {
    GE_HostSwitch_DisableHrmDelegates       = 1u << 0, // reach HotReloadManager through reflection, not typed delegates
    GE_HostSwitch_ForceHrmCapabilityFailure = 1u << 1, // behave as if HotReloadManager were missing (entry points return -3)
    GE_HostSwitch_ForceAbiMismatch          = 1u << 2, // CoreBridge.Initialize reports a managed ABI mismatch (-3)
};

// HotReload typed endpoints
GE_API GE_Result GE_CDECL GE_GetHotReloadMetrics(uint64_t* lastCompileMs, uint64_t* lastSwapMs, int32_t* totalCompiles, int32_t* totalSwaps);
GE_API GE_Result GE_CDECL GE_GetHotReloadMetricsEx(uint64_t* lastCompileMs, uint64_t* lastSwapMs, int32_t* totalCompiles, int32_t* totalSwaps, int32_t* totalUnloads);
GE_API GE_Result GE_CDECL GE_GetHotReloadEditorMetrics(uint64_t* lastFirstInvokeMs);
GE_API GE_Result GE_CDECL GE_ResetHotReloadMetrics(void);

GE_API GE_Result GE_CDECL GE_PreloadAssemblyContext(const char* assemblyPathUtf8, uint32_t pathLen);
GE_API GE_Result GE_CDECL GE_SwapPreloadedContext(void);
GE_API GE_Result GE_CDECL GE_CleanupOldContext(const char* assemblyPathUtf8, uint32_t pathLen);
GE_API GE_Result GE_CDECL GE_ClearCompilerCache(void);
GE_API GE_Result GE_CDECL GE_GetCompilerStats(void);

// HotReload callback registration
//
// Engine.dll exports the five GE_PreloadAssemblyContext / GE_SwapPreloadedContext /
// GE_CleanupOldContext / GE_ClearCompilerCache / GE_GetCompilerStats entry points
// above. By default these dispatch to no-op stubs (used by Player builds and
// tests with the CLR disabled). GameEngine.Native.dll holds the real CoreCLR-
// backed implementations and, at module load, calls GE_RegisterHotReloadCallbacks
// to wire its function pointers into Engine.dll's dispatcher table so editor /
// player hot-reload flows through the actual CLR domain swap.
//
// This indirection exists because Engine.dll's HotReloadTasks / IncrementalCompilation
// Task call these entry points internally — if Engine.dll PRIVATE-linked them from
// GameEngine.Native (the pre-Phase-1 arrangement), the two SHARED libraries form a
// link cycle. With the dispatcher pattern, GameEngine.Native depends on Engine.dll
// but Engine.dll does not depend on GameEngine.Native, and the link graph is acyclic.
typedef struct GE_HotReloadCallbacks {
    GE_PreloadAssemblyContext_Fn PreloadAssemblyContext;
    GE_SwapPreloadedContext_Fn   SwapPreloadedContext;
    GE_CleanupOldContext_Fn      CleanupOldContext;
    GE_ClearCompilerCache_Fn     ClearCompilerCache;
    GE_GetCompilerStats_Fn       GetCompilerStats;
} GE_HotReloadCallbacks;

// Install hot-reload implementations into Engine.dll's dispatcher table. Pass nullptr
// to reset all five back to the no-op default (useful during shutdown). Safe to call
// multiple times; the last registration wins.
//
// Engine.dll owns this symbol; GameEngine.Native consumes it via the import lib. We
// use GE_ENGINE_API (Engine/Include/Core/EngineAPI.h) instead of ScriptingABI.h's
// GE_API because the former tracks GAMEENGINE_BUILD_DLL (Engine.dll's flag) rather
// than GE_SCRIPTING_BUILD (GameEngine.Native's flag) — i.e., it gives the correct
// dllexport-when-building-Engine / dllimport-when-consuming behavior regardless of
// which target includes this header. GE_API would have GameEngine.Native re-export
// the symbol, clashing with Engine.dll's export.
#if defined(_WIN32) || defined(_WIN64)
  #if defined(GAMEENGINE_BUILD_DLL)
    #define GE_ENGINE_HOSTAPI __declspec(dllexport)
  #elif defined(GE_SCRIPTING_STATIC)
    // Statically linked (e.g. test targets that compile the stub definitions
    // directly): no import/export decoration, or the definition's linkage would
    // disagree with this declaration (C4273).
    #define GE_ENGINE_HOSTAPI
  #else
    #define GE_ENGINE_HOSTAPI __declspec(dllimport)
  #endif
#elif defined(GE_SCRIPTING_STATIC)
  #define GE_ENGINE_HOSTAPI
#else
  #define GE_ENGINE_HOSTAPI __attribute__((visibility("default")))
#endif

GE_ENGINE_HOSTAPI void GE_CDECL GE_RegisterHotReloadCallbacks(const GE_HotReloadCallbacks* callbacks);

GE_API GE_Result GE_CDECL GE_ScriptsDomainCreateFromPackage(const GE_PackageEntry* entries,
                                                            uint32_t entryCount,
                                                            uint32_t flags,
                                                            GE_DomainHandle* outDomain);

// Invoke a public static method by name within a specified domain
GE_API GE_Result GE_CDECL GE_Invoke(GE_DomainHandle domain,
                                    const char* methodNameUtf8,
                                    uint32_t methodNameLen,
                                    int32_t* outResult);

// Token-based export API (fast path)
// Query a method once to obtain a per-domain opaque token; valid until the domain unloads
GE_API GE_Result GE_CDECL GE_QueryExport(GE_DomainHandle domain,
                                         const char* methodNameUtf8,
                                         uint32_t methodNameLen,
                                         uint64_t* outToken);
// Invoke previously queried token without string lookup
GE_API GE_Result GE_CDECL GE_InvokeByToken(GE_DomainHandle domain,
                                           uint64_t token,
                                           int32_t* outResult);

// Debug-only helper: perform QueryExport+InvokeByToken fully inside ABI without out-params crossing boundary
// Returns GE_Result_Ok on success; negative on failure
GE_API GE_Result GE_CDECL GE_DebugInvokeByNameNoOut(GE_DomainHandle domain,
                                                   const char* methodNameUtf8,
                                                   uint32_t methodNameLen);

// Debug-only helper: read perf counters via CoreBridge UCO returning packed 4x16-bit fields
// Returns -1 on failure
GE_API long long GE_CDECL GE_DebugGetPerfCountersPacked64();

// Debug-only helper: return CoreBridge call-path mask for token API
// Bits: 0x1=Typed Query, 0x2=Typed Invoke, 0x4=Wrapper Query, 0x8=Wrapper Invoke, 0x10=Reflection Query, 0x20=Reflection Invoke
GE_API int GE_CDECL GE_DebugGetPathMask();


// Editor-only helper: packed metrics [63..32]=lastSwapMs, [31..0]=lastFirstInvokeMs
GE_API uint64_t GE_CDECL GE_GetHotReloadEditorMetricsPacked64(void);

// -------- Debug Metrics (custom monitor publishing for scripts) --------
// Type values mirror GameEngine::Debug::MonitorType:
//   0=Quantity, 1=Memory (bytes), 2=TimeMs, 3=Percent.
GE_API GE_Result GE_CDECL GE_DebugMetrics_RegisterMonitor(const char* nameUtf8,
                                                          uint32_t nameLen,
                                                          int32_t type,
                                                          const char* unitUtf8,
                                                          uint32_t unitLen);
GE_API GE_Result GE_CDECL GE_DebugMetrics_PushSample(const char* nameUtf8,
                                                     uint32_t nameLen,
                                                     float value);


#ifdef __cplusplus
} // extern "C"
#endif

