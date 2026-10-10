# Scripting ABI Specification

This document describes the stable C ABI between GameEngine (C++) and the managed scripting environment (.NET). It complements Engine/Include/Scripting/ScriptingABI.h.

## Design Principles
- Stability: Keep the ABI small and versioned; avoid frequent breaking changes
- Performance: Use cdecl, blittable POD types, and avoid heap allocations across the boundary
- Safety: Pass opaque handles, not raw pointers to managed objects; avoid reference cycles across domains
- Portability: Pure C ABI (no C++ types); UTF-8 for strings with explicit lengths
- Determinism: Function signatures are fixed per CallbackId; no string-based dispatch paths

## Calling Convention and Types
- All functions use cdecl (`GE_CDECL`) and export macro `GE_API`
- Opaque handles are 64-bit (`GE_Handle`), valid only in the context they were created
- Strings are UTF-8 with length (`const char*`, `uint32_t len`) or `GE_StringView`
- Arrays are represented by pointer+length spans (non-owning)
- Encoding: All strings crossing the boundary MUST be UTF-8. Avoid CharSet.Ansi; pass explicit (byte*, byteLength) pairs.


## Initialization Flow
- Engine instance: CoreBridge holds one managed EngineInstanceContext per process, the binding to the native engine library the host registered.

1. Engine calls `GE_ScriptingInitialize(&config)` providing runtimeconfig.json path and CoreBridge assembly name.
   - Optional: pass `config.nativeLibraryPath` to name the native engine library managed code binds to and avoid CWD heuristics.
2. Managed CoreBridge initializes the runtime control plane and is pinned in the default ALC
3. Engine optionally subscribes to diagnostics via `GE_SubscribeDiagnostics`


## Domain-Qualified Invocation (New)

- Managed exposes a domain-aware invocation that routes by collectible ALC domain handle.
- Native entry: GE_Invoke(GE_DomainHandle domain, const char* methodUtf8, uint32_t len, int32_t* outResult)
- Managed routing:
  - CoreBridge.InvokeInDomain(ulong domain, string methodName) → int
  - Reflects HotReloadManager.CallMethodInDomain(ulong domain, string methodName)
- Supported methods to invoke:
  - Public static methods with no parameters returning int
  - Name resolution supports "Namespace.Type.Method" or simple "Method" (searched across public static methods in the user assembly)
- Error codes (managed side; native maps to GE_Result):
  - -1 = Not initialized / method not found / unexpected exception
  - -2 = Unknown/invalid domain handle
- Notes:
  - Do not call [UnmanagedCallersOnly] from managed. CoreBridge uses reflection to managed methods only.
  - Domain handles are opaque and unique per process; valid only while the domain is alive.
- Standard entrypoint: GameEngine.Scripts.ScriptsEntryPoint.OnAssemblyLoaded (public static int). Legacy ComponentEntryPoint is removed; do not implement or call it.
  The ScriptsEntryPoint type now lives in the scripts assemblies you load (e.g., DomainRoutingTest.dll in tests or the auto-generated GameEngine.Scripts.dll under ScriptAssemblies); it is no longer provided by a hard‑coded Scripts/ScriptsEntryPoint.cs file in the repo.


### Managed reflection and delegate usage (CoreBridge)
- CoreBridge centralizes invocation of HotReloadManager surface via source‑generated bindings (HrmBindings). This reduces duplicated reflection and keeps names/signatures in one place.
- For hot paths, CoreBridge creates typed delegates from MethodInfo (e.g., DCallInDomain, DQueryExportInDomain, DInvokeByToken) and uses them when available; it falls back to MethodInfo.Invoke otherwise. This is transparent to native callers and maintains ABI stability.
- Capability behavior: Missing HRM methods or an unavailable HRM now consistently return -3 (GE_Result_NotFound) from CoreBridge entry points. Older codes (-100/-101) have been removed.
- UCO guidance unchanged: Do not invoke [UnmanagedCallersOnly] from managed; UCO remains strictly for native→managed function pointers.


### HotReload capability endpoints
- Endpoints: `GE_ClearCompilerCache()` and `GE_GetCompilerStats()`
- Capability presence: These may be absent depending on build/profile. In that case calls return `GE_Result_NotFound` (-3). Treat this as non-fatal and simply skip the operation.
- Resolution order:
  - Prefer managed-first callbacks if `iface->GetManagedCallback` resolves the function IDs.
  - Otherwise, attempt the typed C ABI exports if present.
- Performance expectation: When present, both calls should be fast under normal conditions (< a few ms in Debug).
- Testing guidance: Tests accept `GE_Result_NotFound` as a valid outcome and may emit a single informational line when the capability is missing.

## Quick Start

1) Create scripts domain from package (native → managed)
- GE_ScriptsDomainCreateFromPackage(entries, entryCount, flags, &domain)

2) Call standard entrypoint by fully-qualified name
- const char* fq = "GameEngine.Scripts.ScriptsEntryPoint.OnAssemblyLoaded";
- GE_Invoke(domain, fq, (uint32_t)strlen(fq), &rc); // rc == 0 on success

## Token-based export API (fast path)

- Query once by name to get a stable per-domain token, then invoke by token many times without string lookup.
- Tokens are opaque 64-bit values valid until the domain unloads.

Native usage (concept):
- uint64_t token = 0;
- GE_QueryExport(domain, fq, (uint32_t)strlen(fq), &token);
- int rc = 0;
- GE_InvokeByToken(domain, token, &rc);

Unload semantics:
- GE_ScriptsDomainUnload(domain) will:
  1) Call managed CoreBridge.UnloadDomain(domain) when available (UCO via CoreCLRHost)
  2) Update native bookkeeping to remove domain from the live set
- After unload, any GE_InvokeByToken calls for that domain return GE_Result_Fail and must not modify outResult.
- Domain swap does not implicitly mark a domain as live; only creation marks a domain live.

Managed implementation hooks:
- HotReloadManager.QueryExportInDomain(ulong domain, string name, out ulong token) → int (0 on success)
- HotReloadManager.InvokeByToken(ulong domain, ulong token) → int (method’s int result or <0 on failure)
- CoreBridge exposes UCO wrappers for native:
  - QueryExport(ulong domain, byte* nameUtf8, uint len, ulong* outToken) → int
  - InvokeByToken(ulong domain, ulong token, int* outResult) → int (0 on success)
  - UnloadDomain(ulong domain) → int (0 on success)

## Domains
- ScriptsDomain and EditorDomain are collectible AssemblyLoadContexts
- Native hosts create domains with `GE_ScriptsDomainCreateFromPackage(entries, entryCount, flags, &domain)`. The managed loader consumes assembly and PDB bytes to avoid file locks. `GE_ScriptsDomainLoadFromMemory` is a reserved, commented declaration, not a callable export.
- Swaps are performed via `GE_ScriptsDomainSwap` at a safe point (frame boundary)
- Old domains are unloaded via `GE_ScriptsDomainUnload` after lifecycle events run and references are cleared

## Managed Callbacks
- Managed static methods decorated with `[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]` expose an unmanaged function pointer
- Registration is auto-generated by InteropGenerator by scanning `[GenerateBinding(...)]` attributes in CoreBridge; CoreBridge calls `AutoRegisterGeneratedBindings(binding)` the first time an EngineNativeBinding is registered per native handle
- Native caches function pointers and uses them without string parsing


### Important rules
- Never call `[UnmanagedCallersOnly]` methods from managed code. They are entry points for native only and imply unmanaged calling conventions and no managed prolog/epilog assumptions.
- Prefer `[GenerateBinding]` for managed→native callback registration. Do not manually call `TryRegisterManagedCallback` in CoreBridge; rely on `AutoRegisterGeneratedBindings(binding)`.
- Use UTF-8 (byte*, length) across the ABI. Avoid implicit string conversions on hot paths.

### Reserved Callback IDs
- `GE_CB_LogForwarder` → `void(LogLevel level, const char* msg, uint32_t len)`
- `GE_CB_QueryExport` → `int(uint64 domain, const char* name, uint32 len, uint64* outToken)`
- `GE_CB_InvokeByToken` → `int(uint64 domain, uint64 token, int32* outResult)`
- `GE_CB_UnloadDomain` → `int(uint64 domain)`


### Review Notes (2025-09-01)
- Clarified that prior note about GE_GetInterface being mis-nested is historical; current source appears corrected. Keep this section as a reminder for future edits to avoid brace placement regressions.
- Reaffirmed the hard rule: never call UCO from managed. CoreBridge follows this; maintained in docs to avoid future slips.
- Return codes: Standardize CoreBridge managed wrappers to return -3 (NotFound/Incompatible) for missing HotReload capability and -1 for unexpected exceptions. Native maps to GE_Result consistently.
- Domains/tokens: Explicitly note that on failure paths, out pointers (token/result) must remain untouched; this is implemented in native and managed wrappers should preserve it.
- One project per process: the host names the native engine library through `GE_ScriptingConfig.nativeLibraryPath`; nothing is derived from the working directory.

Progress (this commit)
- Added GE_CB_UnloadDomain and native resolver; CoreBridge now registers UnloadDomain callback
- Native unload functions prefer callback, fallback to CoreCLRHost UCO
- Tests: all ScriptingAbiIntegrationTests pass (20/20)

Next steps
- Add managed diagnostics counters around UnloadDomain to validate cleanup path
- Extend InteropGenerator to emit callback IDs and registration code from C# attributes
- Define subsystem ID ranges and a manifest; add CI to prevent collisions


### Review Notes (2025-09-09)

- Implementation status verified (Engine/Source/Scripting/ScriptingABI.cpp):
  - GE_ScriptingGetAbiVersion, GE_RegisterManagedCallback/GE_GetManagedCallback, GE_GetInterface, GE_QueryExport, and GE_InvokeByToken are implemented and wired into GE_Interface_v1.
- Historical mis-nesting note:
  - The earlier warning about GE_GetInterface being mis-nested no longer applies; current native source has it at file scope. Keep the note as historical context only.
- Invocation guidance update:
  - Native callers should prefer the token fast path (GE_QueryExport once, then GE_InvokeByToken) after bootstrap. GE_Invoke(name) may internally leverage the token path but should not be relied upon for hot-loop invocations.
  - Reaffirm: on failure, do not modify out parameters (token/result). Tests should cover this.
- ABI handshake:
  - Ensure GE_ScriptingInitialize performs a strict major-version check (minor additive) and maps to GE_Result_NotFound for incompatibility, GE_Result_Fail for unexpected errors. CoreBridge.Initialize already returns -3 for mismatch; align native mapping accordingly.
- Capability mask and diagnostics:
  - Encourage clients to branch on capabilities via GE_Capabilities and consume structured diagnostics. Keep logs minimal on hot paths.


### Diagnostics: Delegate binding mask and wrapper control
- GE_DebugGetPathMask() (C ABI): Returns a bitmask describing the current binding mechanism used by CoreBridge
  - 0x01 = Typed Query delegate bound
  - 0x02 = Typed Invoke delegate bound
  - 0x04 = Wrapper Query delegate (typed wrapper over reflection)
  - 0x08 = Wrapper Invoke delegate (typed wrapper over reflection)
  - 0x10 = Reflection Query available (no delegate bound, MethodInfo present)
  - 0x20 = Reflection Invoke available (no delegate bound, MethodInfo present)
- When CreateDelegate rejects a HotReloadManager signature, or the host disabled delegates (`GE_HostSwitch_DisableHrmDelegates`), CoreBridge binds the wrapper delegate over MethodInfo.Invoke instead (0x04 / 0x08); `ScriptingAbi_Paths.TypedDelegates_Bound_OnTheFastPath` pins that the typed delegates serve the fast path.
- Verbose diagnostics: GE_VERBOSE=1 (optional) to print CreateDelegate failure details and path decisions.


### Command-line diagnostics tool
- Build: `cmake --build build/<preset> --target ScriptingOnly --config Debug`
- Run: `build/<preset>/bin/Debug/Tools/ScriptingDiag.exe`
  - Prints ABI version, capabilities, delegate path mask, and perf counters
- Optional: `--poke` flag performs a minimal Query+Invoke using the DomainRoutingTest assembly to bump counters, then prints them
  - Example output shows mask 0x03 (typed query+invoke) and non-zero DQ/DI counters after poke

Automated binding generation design (overview)
- Attributes in C# and macros in C++ serve as the declarative source of truth
- InteropGenerator scans both, emits:
  - ScriptingABI additions (IDs, signatures)
  - CoreBridge registration boilerplate and typed wrappers
  - C# stubs for C++ APIs using EngineNativeBinding with function pointers (no DllImport)
- Phased rollout: HRM → ECS → Assets → Rendering; maintain a zero-footprint core

- `GE_CB_OnBeforeUnload` → `void()`
- `GE_CB_OnAfterLoad` → `void()`

## Diagnostics and Events

## Compile Server Transport Abstraction
- The resident compile client in `Jobs/CompileServerClient.h` uses `IHotReloadTransport`; its default implementation is `PipeTransport`.
- Windows uses named pipes; Linux and macOS use Unix domain sockets. See [Compile Server](CompileServer.md) for endpoints, host discovery, and the wire protocol.
- Tests can inject an `IHotReloadTransport`. The production client has no TCP-selection flag or StdIO fallback transport.
- `Scripting/ICompileTransport.h` is a separate prototype interface with a dummy loopback implementation; it is not the resident client's transport contract.

- Engine provides a `GE_DiagnosticsSink` with compilation and reload event handlers
- Managed side reports: CompilationStarted/Progress/Completed (with structured `GE_Diagnostic[]`), ReloadStarted/Swapping/Completed/Failed
- Editor displays diagnostics with clickable file/line mapping

## Representative Engine APIs
- `GE_Log(level, msg, len)` for logging
- `GE_GetAssetCount(outCount)` for asset stats
- ECS examples using opaque handles (`GE_ECS_GetWorldHandle`, `GE_ECS_GetEntityCount`)
- Implemented in native: GE_GetInterface (bootstrap) and GE_GetAssetCount (example)
- Managed EngineNativeBinding consumes GE_GetInterface and exposes typed delegates (Log, RegisterManagedCallback, SubscribeDiagnostics, GetAssetCount)
- CoreCLRHost calls CoreBridge.Initialize and then registers the process's engine instance (RegisterEngineInstance) when the .NET host is available


## Versioning

## Versioned Function Table (Bootstrap)

For clean versioning, the native library exposes a bootstrap entry that returns a typed table of function pointers.

- Bootstrap: GE_GetInterface(uint32_t abiVersion, const void** outTable, uint32_t* outSizeBytes)
- ABI version: 0xMMMMmmmm (major<<16 | minor)
- Current table: GE_Interface_v1
  - Layout: sizeBytes, abiVersion, then function pointers (Log, RegisterManagedCallback, GetManagedCallback, SubscribeDiagnostics, UnsubscribeDiagnostics, GetAssetCount, ScriptsDomainSwap, ScriptsDomainUnload, ECS_GetWorldHandle, ECS_GetEntityCount)
- Policy: Bump major on breaking changes (a changed layout, a removed export); minor for additive changes. Engines may serve multiple table versions during a grace period.

Managed consumption (concept):
- Load the native library once, by absolute path (NativeLibrary.Load)
- Get bootstrap export (NativeLibrary.GetExport)
- Request table for desired ABI; validate sizeBytes
- Marshal delegates and route all calls via the table (no DllImport)

The table carries the ABI handshake that a direct `[DllImport]` call skips; the direct exports remain for the callers that still declare one (see the known limitation under the versioning policy note).



## Capabilities Mask
- GE_Capabilities(outMask)
  - Bit flags indicate feature availability (e.g., HasPackageLoader, HasDomainSwap, HasInvokeById)
  - Managed and native should branch on capabilities to enable/disable features without breaking ABI


## Contributor Notes: Return‑code conventions and error handling

- Return codes
  - 0 = success; negative values indicate failure. Positive values reserved for rare non-error informational results; avoid introducing them.
- Out parameters
  - Do not modify out parameters on failure. Only set them on success after fully computing values.
- Managed→Native mapping (standardized)
  - Managed CoreBridge/UCO returns:
    - 0 → success
    - -2 → InvalidArg (GE_Result_InvalidArg)
    - -3 → NotFound/Incompatible/Missing capability (GE_Result_NotFound)
    - -4 → NotInitialized (GE_Result_NotInitialized)
    - Any other negative → GE_Result_Fail
  - Native ScriptingABI.cpp centrally maps these via MapManagedRcToGeResult(rc). Callers can rely on consistent GE_Result across GE_QueryExport, GE_InvokeByToken, and HotReload helpers.
- Exceptions across boundary
  - Managed catches and converts to negative codes; native never sees managed exceptions across the ABI.


- Guardrails and expected errors
  - Swap without prior preload: `GE_SwapPreloadedContext()` may return InvalidArg (-2) or NotInitialized (-4) depending on the internal path. Both are treated as the expected guard failure (i.e., a clear non-Ok result) and callers should not proceed to swap cleanup.
  - Preload with invalid path: `GE_PreloadAssemblyContext(<invalid path>)` returns InvalidArg (-2). When a diagnostics sink is subscribed, a `GE_Reload_Failed` event is emitted with a short reason string for UI surfacing.

- Managed↔native boundaries
  - Catch exceptions before crossing the unmanaged boundary; return a negative error code instead of throwing.
  - Map managed error codes to GE_Result consistently and centrally.
- UCO guidance
  - Never call [UnmanagedCallersOnly] from managed code. UCO functions exist solely for native→managed invocation.
  - UCO stubs should decode UTF‑8 inputs, validate arguments, delegate to a testable managed helper, and then return a status code.
- UTF‑8
  - All interop strings are UTF‑8 with explicit byte lengths. Avoid CharSet.Ansi and implicit marshaling on hot paths.
- Tokens and domains
  - Validate domain handles and respect token invalidation after unload. Tokens are per-domain and must not be reused across domains.


### Hot Reload Metrics (Extended)
- GE_GetHotReloadMetricsEx(lastCompileMs, lastSwapMs, totalCompiles, totalSwaps, totalUnloads)
  - Returns the same fields as GE_GetHotReloadMetrics plus totalUnloads.
  - Backward-compatible behavior: if the extended managed endpoint is unavailable, the engine falls back to the legacy metrics and reports totalUnloads=0.
- GE_ResetHotReloadMetrics resets all counters, including totalUnloads.

### Swap path minimization (Swap Descriptor)
- Preload vs swap:
  - Preload (background/off-thread): creates collectible ALC, loads the assembly from bytes/stream, and precomputes the fast export indices (full and simple name maps to delegates).
  - Swap (main thread): performs an atomic pointer flip to the preloaded context/assembly, publishes the prebuilt indices, and schedules old-context cleanup on a background thread. No reflection or indexing is performed on the main thread when indices are available.
- Fallback: If indices were not prebuilt, the swap path builds indices synchronously for compatibility.
- Benefit: Minimizes frame hitch during hot swap; the heavy work is done off-thread.

- Introduce a simple ABI version integer exposed via `GE_ScriptingGetAbiVersion()` (to be added) and bump only when breaking changes occur

### Implementation status (mini table)

| Area | Item | Status |
|------|------|--------|
| Metrics | GE_GetHotReloadMetricsEx (with totalUnloads) | Implemented |
| Metrics | GE_GetHotReloadEditorMetrics (first-invoke timing, editor-only) | Implemented (optional binding) |
| Metrics | GE_GetHotReloadEditorMetricsPacked64 ([63..32]=lastSwapMs,[31..0]=lastFirstInvokeMs) | Implemented (optional binding) |
| Swap Descriptor | Precompute indices off-thread, publish on swap | Implemented |
| Swap Descriptor | Old-context cleanup deferred (background) | Implemented |
| Swap Descriptor | Pre-warm/JIT priming of export delegates | Implemented |
| Swap Descriptor | R2R exploration + editor timing hook | Implemented (docs + hook) |
| SLA | Swap duration metric (lastSwapMs) | Implemented |
| SLA | SLA test for swap duration | Implemented (conservative bound) |

- Reserve Callback ID ranges for core vs. user extension

## Examples
### Managed Wrappers vs UCO: Hard Rule and Guidance

- Do not call [UnmanagedCallersOnly] methods from managed code.
  - Rationale: The CLR will throw InvalidProgram if you attempt to invoke UCO methods via reflection or as regular delegates.
  - UCO methods exist solely as unmanaged entry points to be invoked from native code via function pointers.
- Always call the managed-friendly wrappers from managed code (tests, editor tools, runtime scripts), for example CoreBridge.LoadUserScriptsFromPath and CoreBridge.ReloadUserScripts.
- Implementation note:
  - The UCO exports convert UTF‑8 inputs and delegate to the corresponding managed wrappers to keep logic centralized and testable.
- Engine instance binding:
  - CoreBridge holds the one EngineInstanceContext the native host registered; interop calls, `Engine.Current` and EngineLogWriter route through it.
  - EngineNativeBinding is loaded by absolute path and binds to the native function table via GE_GetInterface.

### Registering a Managed Callback (C#)
```csharp

### Engine Instance Registration (UCO)

- CoreBridge unmanaged entry point, called once by CoreCLRHost after CoreBridge.Initialize:
  - RegisterEngineInstance(nativePathUtf8, len, abiOverride) → 0 registered / 1 already registered (the first stays) / -2 empty path / -1 failure
- Routing:
  - Interop calls route via the registered EngineInstanceContext; without one, `Engine.Current` binds the native library beside the application.

[GenerateBinding(GE_CB_LogForwarder)]
[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
public static void LogForwarder(int level, byte* msg, uint len)
{
    var text = Encoding.UTF8.GetString(msg, (int)len);
    EngineLogger.Forward((GE_LogLevel)level, text);
}

// Registration is auto-generated by InteropGenerator via [GenerateBinding]
```

### Calling into Native (Managed binding, no DllImport)
```csharp
var binding = EngineNativeBinding.LoadFrom(null); // prefers app-local then GE_NATIVE_DIR
binding.Log?.Invoke((int)GE_LogLevel.Info, (nint)pUtf8, (uint)len);
```

### Publishing Diagnostics (Managed → Native)
```csharp
_onCompilationEvent?.Invoke(GE_Comp_Completed, 1.0f, diagsPtr, (uint)count, userData);
```

Refer to `ScriptingABI.h` for the exact structures and enums.



## Implementation Status (2025-08-24)

- Implemented
  - GE_ScriptingGetAbiVersion (returns 3.2 today)
  - GE_Log
  - GE_RegisterManagedCallback / GE_GetManagedCallback
  - GE_GetInterface (returns GE_Interface_v1 with pointers to current exports)
  - ECS_GetWorldHandle / ECS_GetEntityCount (Phase 1 minimal ECS)
    - Exposed for simple smoke tests and stats (e.g., `AbiNativeSmokeTest`, managed interop ECS tests).
    - `GE_ECS_GetWorldHandle` is the canonical initializer for the primary ECS world in test harnesses; it brings up the engine via `EnsureEngineInitialized` and then calls `EngineCore::EnsurePrimaryWorld`.
    - Native ECS ABI tests and managed ECS-focused interop tests now treat failure to obtain a primary world in supported hosts as a **hard failure**, not a missing capability.
    - A small number of logging-only tests may still capability-gate ECS presence (marking themselves inconclusive) when ECS is intentionally unavailable.
  - GE_SubscribeDiagnostics / GE_UnsubscribeDiagnostics
  - GE_NotifyCompilationEvent / GE_NotifyReloadEvent (native-to-native routing)

## Token-based Export and Managed-first Callbacks

- Preferred path: Managed registers callbacks for GE_QueryExport and GE_InvokeByToken during CoreBridge initialization and lazily via EnsureManagedCallbacksRegistered.
- Fallback path: If callbacks are absent, native resolves CoreBridge UCO methods via CoreCLRHost and invokes them directly.
- Domain semantics:
  - Domain IDs originate in managed (HotReloadManager). Tokens are per-domain and invalidated on unload.
  - Native keeps one current runtime domain and one current editor domain per process.

### API
- GE_Result GE_QueryExport(GE_DomainHandle domain, const char* utf8Fqn, uint32_t nameLen, uint64_t* outToken)
- GE_Result GE_InvokeByToken(GE_DomainHandle domain, uint64_t token, int32_t* outResult)

### Guidance
- Query once per method name and cache tokens per domain.
- Always include the domain. After domain unload, tokens from that domain are invalid.
- Host switches (`CoreCLRHost::SetHostSwitches`, `GE_HostSwitch_*` in ScriptingABI.h, delivered to `CoreBridge.SetHostSwitches`; the suites' path selectors and fault injections, a production host leaves them clear):
  - `GE_HostSwitch_DisableHrmDelegates` binds HotReloadManager through the reflection wrapper instead of typed delegates
  - `GE_HostSwitch_ForceHrmCapabilityFailure` makes CoreBridge act as if HRM were missing (entry points return -3)
  - `GE_HostSwitch_ForceAbiMismatch` makes CoreBridge.Initialize report a managed ABI mismatch (-3)


### Current-domain getters policy
- `GE_GetCurrentRuntimeDomain` and `GE_GetCurrentEditorDomain` return `GE_Result_Ok` with `*outDomain = 0` when no domain of that kind is current (never loaded, or unloaded). This avoids erroring in polling code.


- Implemented
  - `GE_ScriptsDomainSwap` / `GE_ScriptsDomainUnload`, routed through `ScriptingService`.
  - `GE_GetInterface`, a file-scope export returning the versioned interface table.
  - `GE_ScriptingInitialize` / `GE_ScriptingShutdown`; initialization sets up the runtime and CoreBridge and supports an explicit native library through `GE_ScriptingConfig`.
  - `GE_GetCurrentRuntimeDomain` / `GE_GetCurrentEditorDomain`, routed through `ScriptingService`.
- Reserved, not exported
  - `GE_ScriptsDomainLoadFromMemory`: only a commented declaration remains in the header. Use `GE_ScriptsDomainCreateFromPackage`.


## Host Engine Bootstrapping

Engine is a shared library: the host executable, GameEngine.Native and the managed runtime's callbacks all see the one `EngineCore` singleton. Whoever calls `EngineCore::Initialize` first owns its lifecycle.

**Native host (Editor, Player, native test harnesses):** the host initializes the engine itself. From its first `Initialize` on, the ABI never starts the engine — not while that `Initialize` runs on another thread, not after `Shutdown`, not after a failed run; an export that needs the engine then fails (`GE_Result_Fail` on the main ABI — the bootstrap answers `GE_Result_NotInitialized`, which only the Animator and Timeline exports pass through).

**Standalone (managed code loading GameEngine.Native without a native host, e.g. `dotnet test`):** nobody has initialized the engine when the first ABI call arrives, so `DllBootstrap::EnsureEngineInitialized()` initializes it, configured from `GE_NATIVE_DIR` and `GE_SetHostScriptsConfig()`; concurrent first callers wait for that `Initialize`.

Key functions:
- `GE_SetHostScriptsConfig(const GE_HostScriptsConfig* config)` — scripting flags for the standalone engine
- `DllBootstrap::EnsureEngineInitialized()` — internal helper every engine-dependent export calls first

Implementation: `Engine/Source/Scripting/DllEngineBootstrap.h`

## Domain Handle Origin and Policies

- Domain handle source:
  - When user assemblies are loaded (CreateFromPackage), the managed HotReloadManager assigns a stable 64-bit domain identifier per collectible AssemblyLoadContext and exposes it via CoreBridge.GetCurrentRuntimeDomainId (UCO).
  - Native queries this identifier after a successful load and returns it to callers. If the managed ID is 0 (should not happen on success), native falls back to a process-local generated handle.

## Flow diagrams

```mermaid
%%{init: { 'theme': 'neutral' } }%%
sequenceDiagram
  autonumber
  participant Native as Native Engine
  participant ABI as ScriptingABI
  participant Host as CoreCLRHost
  participant Bridge as CoreBridge (UCO)
  participant HR as HotReloadManager

  Note over Native,HR: CreateFromPackage
  Native->>ABI: GE_ScriptsDomainCreateFromPackage(entries)
  ABI->>Host: LoadUserScriptsAssemblyFromBytes(bytes)
  Host->>Bridge: LoadUserScriptsAssemblyFromBytes(bytes)
  Bridge->>HR: Load from bytes (into collectible ALC)
  HR-->>Bridge: rc >= 0; set _currentDomainId
  Bridge-->>Host: rc
  Host-->>ABI: rc
  ABI->>Host: GetCurrentRuntimeDomainId()
  Host->>Bridge: GetCurrentRuntimeDomainId()
  Bridge-->>Host: domainId
  Host-->>ABI: domainId
  ABI-->>Native: outDomain = domainId (or fallback)

  Note over Native,HR: InvokeInDomain
  Native->>ABI: GE_Invoke(domain, method, outResult)
  ABI->>Host: InvokeInDomain(domain, method, &outResult)
  Host->>Bridge: InvokeInDomain(domain, method, &outResult)
  Bridge->>HR: CallUserScriptsMethod(method) // domain routing TBD
  HR-->>Bridge: rc (>=0 success, <0 fail)
  Bridge-->>Host: 0 on success, -1 on fail
  alt success
    Host-->>ABI: 0; outResult set by managed
    ABI-->>Native: GE_Result_Ok
  else fail
    Host-->>ABI: -1; outResult unchanged by ABI
    ABI-->>Native: GE_Result_Fail
  end
```

  - Rationale: Managed is the source of truth for ALC lifecycle; native transports opaque IDs only.

- One project per process:
  - A process hosts one project. The native host registers that project with CoreBridge once, at startup; opening another project in the editor rebinds its scripts in the same process.
  - A process has one current runtime domain and one current editor domain. Loading a package makes its domain the current one; `GE_GetCurrentRuntimeDomain` and `GE_GetCurrentEditorDomain` report them.
  - To run two projects side by side, run two processes. Two projects in one process are not supported: nothing keeps their scripts apart.

- Notes:
  - Do not call UnmanagedCallersOnly from managed code; CoreBridge reflects the field for managed-side usage only. UCO is only for native→managed.
  - Domain IDs are opaque and unique per process.

- Policy note on versioning
  - Major must match between managed/native. Current native reports 3.2 (3.2 added GE_Animator_PollEventsOnEntity; 3.1 added GE_Model_GetExtras; 3.0 removed projectIdUtf8 from GE_ScriptingConfig; 2.0 removed the per-project domain exports; 1.3 added text/textLen on GE_UIEventData plus the typed element value accessors). The minor is NOT advisory in one direction: a native OLDER than the managed build writes shorter payload structs than managed reads, and those structs (GE_UIEventData and friends) carry no size field, so nothing detects it per call. EngineNativeBinding.LoadFrom therefore refuses nativeMinor < kAbiMinor and says to rebuild; a NEWER native is accepted, because additive fields nobody reads cost nothing.
  - KNOWN LIMITATION: that gate lives in `EngineNativeBinding.LoadFrom`, so it only protects callers that go through the binding. A managed assembly is free to declare its own `[DllImport]` against `GameEngine.Native` and call an export directly — the C# UI binding does exactly this — and such a call never touches the handshake, so a stale native pairs with a new managed struct layout unnoticed. Closing that would need a size or version field on the payload structs themselves; until then, the rule is that engine and managed are rebuilt together, and the binding's refusal is the tripwire that catches the common case rather than a complete guarantee.

- Guidance reaffirmed
  - Do not call [UnmanagedCallersOnly] methods from managed code. Use managed wrappers; UCO methods are for native→managed function pointers only.



## Review Addendum (2025-09-12)

### Implementation gaps and conformance notes
- Initialize/fail-fast expectations at ABI layer
  - GE_ScriptingInitialize should perform strict major ABI validation and return GE_Result_NotFound on incompatibility, GE_Result_Fail on unexpected errors. Current code paths appear to rely on ScriptManager/CoreCLRHost for managed bring-up; move the contract into this ABI entry as the source of truth. Add native tests.
- Token fast path requirements
  - Reiterate that name-based GE_Invoke is only for bootstrap. After first successful name resolution per domain, native code must cache tokens and use GE_InvokeByToken. Verify out parameters remain untouched on failure; add explicit tests.
- UTF-8 and calling conventions
  - Confirm all UCO methods specify CallConvCdecl and all native sites route through GE_CDECL wrappers. Centralize a macro/attribute mapping to prevent regressions. Add a tiny conformance test.
- Native library binding
  - The host passes the native library path via GE_ScriptingConfig.nativeLibraryPath to avoid CWD coupling. Getters should return Ok with out=0 when absent.

### Design clarifications to reduce user pain
- Capability presence and diagnostics
  - Encourage clients to branch using GE_Capabilities and consume structured diagnostics instead of parsing logs. Maintain a zero-footprint default on hot paths.
- Domain/tokens lifecycle
  - Tokens are per-domain and invalid after unload. On Fail, out pointers (token/result) must not be modified (both native and managed). Keep this prominently documented; maintain tests.

### Next steps (ABI-level)
1) Add explicit ABI validation path and tests to GE_ScriptingInitialize; map errors consistently via MapManagedRcToGeResult.
2) Enforce token fast path by policy: cache per-domain tokens; purge on unload/swap; add failure out-param tests.
3) Add a compile-time/static check to validate UCO signatures and calling conventions (debug build unit test acceptable).
4) Finalize capability mask semantics in the header and add CI checks to keep docs/spec and header in sync.
