using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using GameEngine.Interop;

namespace GameEngine.CoreBridge;

/// <summary>
/// A raw pointer + length span used for passing byte buffers across the ABI.
/// Pointer may be null when length is 0.
/// </summary>

[StructLayout(LayoutKind.Sequential)]
public unsafe struct GE_ByteSpan
{
    /// <summary>Pointer to the first byte; may be null when length is 0.</summary>
    public byte* data;
    /// <summary>Number of bytes in the span.</summary>
    public uint length;
}

/// <summary>
/// A compiled package entry containing assembly bytes and optional PDB bytes.
/// </summary>

[StructLayout(LayoutKind.Sequential)]
public struct GE_PackageEntry
{
    /// <summary>Assembly bytes.</summary>
    public GE_ByteSpan assembly;
    /// <summary>Optional PDB bytes (may be empty).</summary>
    public GE_ByteSpan pdb;
}

/// <summary>
/// CoreBridge is a small, stable managed assembly loaded in the default ALC.
/// It exposes unmanaged-callable entry points and registers callbacks for the engine.
/// Also provides managed-friendly wrappers for scripting control.
/// </summary>
public static unsafe partial class CoreBridge
{
    // Callback IDs (must match ScriptingABI.h)
// Ensure resolver is initialized
// static CoreBridge() { _ = typeof(PlatformResolver); }

    private const uint GE_CB_LogForwarder = 1;
    private const uint GE_CB_OnBeforeUnload = 2;
    private const uint GE_CB_OnAfterLoad = 3;
    // HotReload typed endpoints (must match ScriptingABI.h)
    private const uint GE_CB_PreloadAssemblyContext = 10;
    private const uint GE_CB_SwapPreloadedContext = 11;
    private const uint GE_CB_CleanupOldContext = 12;
    private const uint GE_CB_ClearCompilerCache = 13;
    private const uint GE_CB_GetCompilerStats = 14;

    private const uint GE_CB_GetHotReloadMetrics = 15;
    private const uint GE_CB_ResetHotReloadMetrics = 16;
    private const uint GE_CB_GetHotReloadMetricsEx = 17;

    // The engine instance this process's managed code talks to, set once by the native host
    // through RegisterEngineInstance. Null until then.
    private static GameEngine.Interop.EngineInstanceContext? s_engineInstance;
    // Late-binding: cache diagnostics sink until a binding is available, then auto-subscribe
    private static GameEngine.Interop.GE_DiagnosticsSink? s_pendingDiagSink;

    // Token API callback IDs (must match ScriptingABI.h)
    private const uint GE_CB_QueryExport = 20;
    private const uint GE_CB_InvokeByToken = 21;
    private const uint GE_CB_UnloadDomain = 22; // int(ulong domain)

    // Track which native bindings have had managed callbacks registered (one entry per binding)
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<nint, byte> s_registeredBindings = new();

    private static void EnsureManagedCallbacksRegistered()
    {
        var binding = GetCurrentBinding();
        if (binding == null) return;
        EnsureManagedCallbacksRegisteredFor(binding);
    }

    private static void EnsureManagedCallbacksRegisteredFor(GameEngine.Interop.EngineNativeBinding binding)
    {
        if (!s_registeredBindings.TryAdd(binding.Handle, 1)) return;
        try
        {

            // Auto-generated managed callback registration (from InteropGenerator)
            AutoRegisterGeneratedBindings(binding);
            try { DebugWrite($"Auto-registered managed callbacks for binding 0x{binding.Handle:X}"); } catch { }
        }

        catch { }
    }

    private static void TryFlushPendingDiagnostics()
    {
        try
        {
            var binding = GetCurrentBinding();
            if (binding == null) return;
            if (s_pendingDiagSink.HasValue)
            {
                var sink = s_pendingDiagSink.Value;
                int rc = binding.TrySubscribeDiagnostics(in sink);
                if (rc == 0) s_pendingDiagSink = null;
            }
        }
        catch { }
    }





	    // Verbose diagnostics gate (shared with HotReloadManager)
	    private static readonly bool s_verbose = Environment.GetEnvironmentVariable("GE_VERBOSE") == "1";

        // Explicit override for HotReload.dll directory, configured by native host (preferred over env vars)
        private static string? s_hrmDirOverride;

        // The native host's switches (GE_HostSwitch_* in ScriptingABI.h), delivered by CoreCLRHost::SetHostSwitches.
        [Flags]
        private enum HostSwitches : uint
        {
            None = 0,
            DisableHrmDelegates = 1u << 0,
            ForceHrmCapabilityFailure = 1u << 1,
            ForceAbiMismatch = 1u << 2,
        }
        private static HostSwitches s_hostSwitches;
        private static bool HrmDelegatesDisabled => (s_hostSwitches & HostSwitches.DisableHrmDelegates) != 0;



    // Appends to corebridge-debug.txt in the working and base directories.
    // Opt-in with GE_VERBOSE=1: the file lands wherever the host runs (repo root,
    // app bundle), so a default run must never write it.
    private static void DebugWrite(string msg)
    {
        if (!s_verbose)
            return;
        try
        {
            string stamp = DateTime.Now.ToString("HH:mm:ss.fff ") + msg + Environment.NewLine;
            // Write to current directory
            try {
                var p = System.IO.Path.Combine(System.IO.Directory.GetCurrentDirectory(), "corebridge-debug.txt");
                System.IO.File.AppendAllText(p, stamp);
            } catch { }
            // Also write to AppContext.BaseDirectory
            try {
                var p2 = System.IO.Path.Combine(AppContext.BaseDirectory ?? "", "corebridge-debug.txt");
                System.IO.File.AppendAllText(p2, stamp);
            } catch { }
        }
        catch { }
    }

    private static GameEngine.Interop.EngineNativeBinding? GetCurrentBinding() => s_engineInstance?.Binding;

    /// <summary>
    /// The engine instance the native host registered, or null before registration.
    /// </summary>
    internal static GameEngine.Interop.EngineInstanceContext? GetEngineInstance() => s_engineInstance;

    // Domain lifecycle control (to be implemented natively)
    // Domain calls will be routed via EngineNativeBinding

    // Diagnostics registration now routes via EngineNativeBinding

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
		    [GenerateBinding(GE_CB_LogForwarder)]

    private static void LogForwarder(int level, byte* msg, uint len)
    {
        try
        {
            string text = len == 0 || msg == null ? string.Empty : System.Text.Encoding.UTF8.GetString(msg, (int)len);
            // Placeholder: LogForwarder can be used if native wants to push logs into managed; keep silent by default
        }
        catch { /* swallow to avoid propagating across boundary */ }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
		    [GenerateBinding(GE_CB_OnBeforeUnload)]

    private static void OnBeforeUnload()
    {
        try
        {
            // Invalidate façade caches before unload/swap
            var ctx = s_engineInstance;
            if (ctx != null)
            {
                GameEngine.WorldsProvider.Invalidate(ctx);
            }
        }
        catch { }
    }



    /// <summary>
    /// Registers diagnostics callbacks (compilation and reload events) from native. Returns 0 on success.
    /// </summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int RegisterDiagnosticsSink(IntPtr onCompilationEvent, IntPtr onReloadEvent, IntPtr userData)
    {
        try
        {
            var sink = new GameEngine.Interop.GE_DiagnosticsSink(onCompilationEvent, onReloadEvent, userData);
            var binding = GetCurrentBinding();
            if (binding != null)
            {
                return binding.TrySubscribeDiagnostics(in sink);
            }
            // Late-binding path: cache the sink and report success; we'll auto-subscribe when a binding becomes available
            s_pendingDiagSink = sink;
            return 0;
        }
        catch { return -1; }
    }

        /// <summary>
        /// Swap the process's current runtime domain to <paramref name="newDomain"/>.
        /// Returns 0 on success; non-zero on failure.
        /// </summary>

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int SwapRuntimeDomain(ulong newDomain)
    {
        try { var b = GetCurrentBinding(); return b?.TryScriptsDomainSwap(newDomain) ?? -1; }
        catch { return -1; }
    }

    /// <summary>
    /// Unload a managed scripting domain identified by domain id. Returns 0 on success.
    /// </summary>
    [GenerateBinding(GE_CB_UnloadDomain)]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int UnloadDomain(ulong domain)
    {
        try {
#if DEBUG
            System.Threading.Interlocked.Increment(ref s_dbgUnloadDomain);
#endif
            if (!EnsureHotReloadTypeLoaded()) return -3;
            if (s_miUnloadDomain == null) return -3;
            var ret = s_miUnloadDomain.Invoke(null, new object?[] { domain });
            return ret is int i ? i : 0;
        }
        catch { return -1; }
    }


        /// <summary>
        /// Registers this process's engine instance: the native engine library at nativePath, bound
        /// lazily on first use with an optional ABI override (0 = the current ABI). Then installs the
        /// DllImport resolver and Console redirection and registers the managed callbacks. The native
        /// host calls this once, after Initialize.
        /// Returns 0 on success; 1 if an instance is already registered (the first one stays);
        /// -2 if nativePath is empty; -1 on failure.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int RegisterEngineInstance(byte* nativePathUtf8, uint nativePathLen, uint abiOverride)
        {
            try
            {
                string nativePath = (nativePathUtf8 == null || nativePathLen == 0) ? string.Empty : System.Text.Encoding.UTF8.GetString(nativePathUtf8, (int)nativePathLen);
                if (string.IsNullOrWhiteSpace(nativePath)) return -2;
                uint? abi = abiOverride == 0 ? null : abiOverride;
                var ctx = new GameEngine.Interop.EngineInstanceContext(nativePath, abi);
                if (System.Threading.Interlocked.CompareExchange(ref s_engineInstance, ctx, null) != null) return 1;
#if DEBUG
                if (s_verbose) try { Console.WriteLine($"[CoreBridge] Registered engine instance: {nativePath} (abiOverride={abi?.ToString("X8") ?? "null"})"); } catch { }
#endif
                try
                {
                    // Post-bootstrap: safe to install resolver and console redirection
                    string? skip = Environment.GetEnvironmentVariable("GE_SKIP_PLATFORM_RESOLVER");
                    if (string.IsNullOrEmpty(skip) || skip == "0")
                    {
                        try { PlatformResolver.EnsureInstalled(); } catch { }
                    }
                    string? disable = Environment.GetEnvironmentVariable("GE_DISABLE_CONSOLE_REDIRECT");
                    if (string.IsNullOrEmpty(disable) || disable == "0")
                    {
                        try { EngineLogWriter.Install(); } catch { }
                    }
                }
                catch { }

                try { EnsureManagedCallbacksRegistered(); TryFlushPendingDiagnostics(); } catch {}
                return 0;
            }
            catch { return -1; }
        }

        // Managed-friendly wrappers for script load/reload/unload using ScriptingOpResult
        /// <summary>Load user scripts from a file path.</summary>
        public static ScriptingOpResult LoadUserScriptsFromPath(string path)
        {
            try
            {
                if (!EnsureHotReloadTypeLoaded() || s_miLoad == null) return ScriptingOpResult.NotFound;
                var result = s_miLoad.Invoke(null, new object[] { path ?? string.Empty });
                int code = result is int i ? i : -1;
                return ScriptingOp.MapCommon(code);
            }
            catch { return ScriptingOpResult.Fail; }
        }
        /// <summary>Load user scripts from a byte array.</summary>
        public static ScriptingOpResult LoadUserScriptsFromBytes(byte[] bytes)
        {
            try
            {
                if (!EnsureHotReloadTypeLoaded() || s_miLoadFromBytes == null) return ScriptingOpResult.NotFound;
                if (bytes == null || bytes.Length == 0) return ScriptingOpResult.InvalidArg;
                var result = s_miLoadFromBytes.Invoke(null, new object[] { bytes });
                int code = result is int i ? i : -1;
                return ScriptingOp.MapCommon(code);
            }
            catch { return ScriptingOpResult.Fail; }
        }
        /// <summary>Reload the currently loaded user scripts.</summary>
        public static ScriptingOpResult ReloadUserScripts()
        {
            try
            {
                if (!EnsureHotReloadTypeLoaded() || s_miReload == null) return ScriptingOpResult.NotFound;
                var result = s_miReload.Invoke(null, null);
                int code = result is int i ? i : -1;
                return ScriptingOp.MapCommon(code);
            }
            catch { return ScriptingOpResult.Fail; }
        }
        /// <summary>Unload currently loaded user scripts.</summary>
        public static ScriptingOpResult UnloadUserScripts()
        {
            try
            {
                if (!EnsureHotReloadTypeLoaded() || s_miUnload == null) return ScriptingOpResult.NotFound;
                var result = s_miUnload.Invoke(null, null);
                int code = result is int i ? i : -1;
                return ScriptingOp.MapCommon(code);
            }
            catch { return ScriptingOpResult.Fail; }
        }


    private static Type? s_hotReloadType;
    private static System.Reflection.MethodInfo? s_miInitialize;
    private static bool s_hrmInitialized = false;

    private static System.Reflection.MethodInfo? s_miLoad;
    private static System.Reflection.MethodInfo? s_miUnload;
    private static System.Reflection.MethodInfo? s_miUnloadDomain;
    private static System.Reflection.MethodInfo? s_miCallInDomain;
    private static System.Reflection.MethodInfo? s_miQueryExportInDomain;
    // Fast-path delegates (created from MethodInfos)
    private delegate int DCallInDomain(ulong domain, string method);
    private delegate int DQueryExportInDomain(ulong domain, string name, ref ulong token);
    private delegate int DInvokeByToken(ulong domain, ulong token);
    private delegate int DStringArg(string arg);
    private delegate int DNoArgs();

    private static DCallInDomain? s_dCallInDomain;
    private static DQueryExportInDomain? s_dQueryExportInDomain;
    private static DInvokeByToken? s_dInvokeByToken;
    private static DStringArg? s_dPreloadAssemblyContext;
    private static DNoArgs? s_dSwapPreloadedContext;
    private static DStringArg? s_dCleanupOldContext;

        // Track whether the currently bound delegates are wrappers around MethodInfo (not true typed delegates)
        private static bool s_isWrapperQuery = false;
        private static bool s_isWrapperInvoke = false;

    private static System.Reflection.MethodInfo? s_miInvokeByToken;

        // The next EnsureHotReloadTypeLoaded re-resolves HotReloadManager and rebinds every entry
        // point under the current directory override and host switches.
        private static void InvalidateHrmBindings()
        {
            s_hotReloadType = null;
            s_miLoadFromBytes = null;
            ClearHrmDelegates();
        }

        private static void ClearHrmDelegates()
        {
            s_dCallInDomain = null; s_dQueryExportInDomain = null; s_dInvokeByToken = null;
            s_dPreloadAssemblyContext = null; s_dSwapPreloadedContext = null; s_dCleanupOldContext = null;
            s_isWrapperQuery = false; s_isWrapperInvoke = false;
        }

        // Typed delegates over the MethodInfos just bound, or none while the host has delegates
        // disabled: each call then goes through the reflection wrapper or MethodInfo.Invoke.
        private static void BindHrmDelegates()
        {
            ClearHrmDelegates();
            if (HrmDelegatesDisabled) return;
            try
            {
                s_dCallInDomain = s_miCallInDomain?.CreateDelegate<DCallInDomain>();
                s_dQueryExportInDomain = s_miQueryExportInDomain?.CreateDelegate<DQueryExportInDomain>();
                s_dInvokeByToken = s_miInvokeByToken?.CreateDelegate<DInvokeByToken>();
                s_dPreloadAssemblyContext = s_miPreloadAssemblyContext?.CreateDelegate<DStringArg>();
                s_dSwapPreloadedContext = s_miSwapPreloadedContext?.CreateDelegate<DNoArgs>();
                s_dCleanupOldContext = s_miCleanupOldContext?.CreateDelegate<DStringArg>();
            }
            catch (Exception ex)
            {
                // A signature mismatch leaves the remaining entry points on reflection.
                if (s_verbose) { try { DebugWrite($"BindHrmDelegates: {ex.GetType().Name}: {ex.Message}"); } catch { } }
            }
        }

        // First use after a HotReload (re)load or a switch change: the typed delegate, or with
        // delegates disabled or a signature mismatch the wrapper around MethodInfo.Invoke.
        private static void BindQueryExportDelegate()
        {
            var mi = s_miQueryExportInDomain;
            if (mi == null) return;
            if (!HrmDelegatesDisabled)
            {
                try { s_dQueryExportInDomain = mi.CreateDelegate<DQueryExportInDomain>(); s_isWrapperQuery = false; return; }
                catch (Exception ex) { if (s_verbose) { try { DebugWrite($"QueryExport CreateDelegate failed: {ex.GetType().Name}: {ex.Message}"); } catch { } } }
            }
            s_dQueryExportInDomain = (ulong d, string nm, ref ulong tk) =>
            {
                object?[] a = new object?[] { d, nm, 0UL };
                var r = mi.Invoke(null, a);
                tk = a[2] is ulong u2 ? u2 : 0UL;
                return r is int i2 ? i2 : -1;
            };
            s_isWrapperQuery = true;
            try { DebugWrite("QueryExport fallback wrapper bound"); } catch { }
        }

        private static void BindInvokeByTokenDelegate()
        {
            var mi = s_miInvokeByToken;
            if (mi == null) return;
            if (!HrmDelegatesDisabled)
            {
                try { s_dInvokeByToken = mi.CreateDelegate<DInvokeByToken>(); s_isWrapperInvoke = false; return; }
                catch (Exception ex) { if (s_verbose) { try { DebugWrite($"InvokeByToken CreateDelegate failed: {ex.GetType().Name}: {ex.Message}"); } catch { } } }
            }
            s_dInvokeByToken = (ulong d, ulong tk) =>
            {
                var r = mi.Invoke(null, new object?[] { d, tk });
                return r is int i2 ? i2 : -1;
            };
            s_isWrapperInvoke = true;
            try { DebugWrite("InvokeByToken fallback wrapper bound"); } catch { }
        }

#if DEBUG
        // Debug-only perf counters
        private static long s_dbgDelegateQueryExport = 0;
        private static long s_dbgReflectionQueryExport = 0;
        private static long s_dbgDelegateInvokeByToken = 0;
        private static long s_dbgReflectionInvokeByToken = 0;
        private static long s_dbgUnloadDomain = 0;
#endif

    private static System.Reflection.MethodInfo? s_miReload;
    private static System.Reflection.MethodInfo? s_miLoadFromBytes;
        private static System.Reflection.MethodInfo? s_miCallWithArgs;
    private static bool VerifyHrmCapabilities()
    {
        // Only the minimal surface is required up-front; others are optional and resolved lazily.
        bool ok = s_miCallInDomain != null && s_miQueryExportInDomain != null && s_miInvokeByToken != null;
        ok &= s_miGetUserScriptsAssemblyInfo != null;
        if (!ok)
        {
            if (!s_hrmDiagPrinted)
            {
                s_hrmDiagPrinted = true;
                try { System.Console.Error.WriteLine("[CoreBridge] HRM capability verification failed: required core methods missing."); } catch { }
            }
        }
        return ok;
    }

    private static bool IsValidToken(ulong token) => token != 0UL;

    private static class HotReloadApi
    {
    // Note: domains are opaque; do not pre-validate here to preserve existing behavior.

    // Methods below populated later




        public static int CallInDomain(System.Reflection.MethodInfo mi, ulong domain, string method)
        {
            var ret = mi.Invoke(null, new object[] { domain, method });
            return ret is int i ? i : -1;
        }
        public static int QueryExportInDomain(System.Reflection.MethodInfo mi, ulong domain, string name, ref ulong token)
        {
            object?[] args = new object?[] { domain, name, 0UL };
            var ret = mi.Invoke(null, args);
            if (args[2] is ulong u) token = u; else token = 0UL;
            return ret is int i ? i : -1;
        }
        public static int InvokeByToken(System.Reflection.MethodInfo mi, ulong domain, ulong token)
        {
            var ret = mi.Invoke(null, new object?[] { domain, token });
            return ret is int i ? i : -1;
        }
        public static int InvokeNoArgs(System.Reflection.MethodInfo mi)
        {
            var ret = mi.Invoke(null, null);
            return ret is int i ? i : -1;
        }
        public static int InvokeStringArg(System.Reflection.MethodInfo mi, string arg)
        {
            var ret = mi.Invoke(null, new object[] { arg });
            return ret is int i ? i : -1;
        }
    }


    private static System.Reflection.MethodInfo? s_miCall;

    private static System.Reflection.MethodInfo? s_miGetUserScriptsAssemblyInfo;
    private static System.Reflection.MethodInfo? s_miPreloadAssemblyContext;
    private static System.Reflection.MethodInfo? s_miSwapPreloadedContext;
    private static System.Reflection.MethodInfo? s_miCleanupOldContext;

        private static System.Runtime.Loader.AssemblyLoadContext? s_hrmAlc;
        private static bool s_hrmDiagPrinted = false;

        private sealed class HrmLoadContext : System.Runtime.Loader.AssemblyLoadContext
        {
            private readonly System.Runtime.Loader.AssemblyDependencyResolver m_resolver;
            public HrmLoadContext(string mainAssemblyPath) : base(isCollectible: false)
            {
                m_resolver = new System.Runtime.Loader.AssemblyDependencyResolver(mainAssemblyPath);
            }
            protected override System.Reflection.Assembly? Load(System.Reflection.AssemblyName assemblyName)
            {
                var path = m_resolver.ResolveAssemblyToPath(assemblyName);
                return path != null ? LoadFromAssemblyPath(path) : null;
            }
        }

        private static bool TryLoadHrmAt(string path)
        {
            try
            {
                if (!System.IO.File.Exists(path)) return false;
                var alc = s_hrmAlc as HrmLoadContext;
                if (alc == null) { alc = new HrmLoadContext(path); s_hrmAlc = alc; }
                System.Reflection.Assembly? asm = null;
                try {
                    asm = alc.LoadFromAssemblyPath(path);
                } catch (Exception ex) {
                    DebugWrite($"TryLoadHrmAt: ALC load failed {ex.GetType().Name}: {ex.Message}, trying Assembly.LoadFrom");
                    try { asm = System.Reflection.Assembly.LoadFrom(path); } catch (Exception ex2) { DebugWrite($"TryLoadHrmAt: Assembly.LoadFrom failed {ex2.GetType().Name}: {ex2.Message}"); return false; }
                }
                var t = asm.GetType("GameEngine.HotReload.HotReloadManager", throwOnError: false, ignoreCase: false);
                if (t == null) { DebugWrite("TryLoadHrmAt: HotReloadManager type not found in assembly"); return false; }
                s_hotReloadType = t;
                // Cache method infos
                s_miInitialize = t.GetMethod("Initialize", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
                s_miLoad = t.GetMethod("LoadUserScriptsAssembly", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
                s_miLoadFromBytes = t.GetMethod("LoadCompiledAssembly", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static, binder: null, types: new[] { typeof(byte[]) }, modifiers: null);
                s_miUnload = t.GetMethod("UnloadUserScriptsAssembly", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
                s_miUnloadDomain = t.GetMethod("UnloadDomain", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static, binder: null, types: new[] { typeof(ulong) }, modifiers: null);
                s_miReload = t.GetMethod("ReloadUserScriptsAssembly", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
                s_miCall = t.GetMethod("CallMethodInUserScripts", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static, binder: null, types: new[] { typeof(string) }, modifiers: null);
                s_miCallWithArgs = t.GetMethod("CallMethodInUserScripts", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static, binder: null, types: new[] { typeof(string), typeof(object[]) }, modifiers: null);
                // Bind via generated HrmBindings
                var bound = GameEngine.CoreBridge.Generated.HrmBindings.Bind(t);
                s_miCallInDomain = bound.MiCallInDomain;
                s_miQueryExportInDomain = bound.MiQueryExportInDomain;
                s_miInvokeByToken = bound.MiInvokeByToken;
                s_miGetUserScriptsAssemblyInfo = bound.MiGetUserScriptsAssemblyInfo;
                s_miPreloadAssemblyContext = bound.MiPreloadAssemblyContext;
                s_miSwapPreloadedContext = bound.MiSwapPreloadedContext;
                s_miCleanupOldContext = bound.MiCleanupOldContext;

                BindHrmDelegates();

                // Fallback: if generated bindings did not locate required methods, bind via reflection directly
                if (s_miCallInDomain == null)
                    s_miCallInDomain = t.GetMethod("CallMethodInDomain", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static, binder: null, types: new[] { typeof(ulong), typeof(string) }, modifiers: null);
                if (s_miQueryExportInDomain == null)
                    s_miQueryExportInDomain = t.GetMethod("QueryExportInDomain", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static, binder: null, types: new[] { typeof(ulong), typeof(string), typeof(ulong).MakeByRefType() }, modifiers: null);
                if (s_miInvokeByToken == null)
                    s_miInvokeByToken = t.GetMethod("InvokeByToken", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static, binder: null, types: new[] { typeof(ulong), typeof(ulong) }, modifiers: null);
                if (s_miGetUserScriptsAssemblyInfo == null)
                    s_miGetUserScriptsAssemblyInfo = t.GetMethod("GetUserScriptsAssemblyInfo", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);

                if (!VerifyHrmCapabilities()) return false;

                return true;

            }
            catch (Exception ex)
            {
                if (!s_hrmDiagPrinted)
                {
                    try { System.Console.Error.WriteLine($"[CoreBridge] HRM load failed at '{path}': {ex.GetType().Name}: {ex.Message}"); } catch { }
                    s_hrmDiagPrinted = true;
                }
                return false;
            }
        }


    private static bool EnsureHotReloadTypeLoaded()
    {
        // The host's fault injection: behave as if HotReloadManager were absent.
        if ((s_hostSwitches & HostSwitches.ForceHrmCapabilityFailure) != 0) return false;
        if (s_hotReloadType != null) return true;
        try { DebugWrite("EnsureHotReloadTypeLoaded: enter"); } catch { }
        // 1) Try by assembly-qualified name first
        try
        {
            s_hotReloadType = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: false);
            if (s_verbose) { try { DebugWrite($"Type.GetType returned null={(s_hotReloadType==null)}"); } catch { } }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[CoreBridge] Type.GetType failed: {ex.GetType().Name}: {ex.Message}");
        }


        // Preferred: explicit override provided by native host
        if (s_hotReloadType == null && !string.IsNullOrEmpty(s_hrmDirOverride))
        {
            try
            {
                var candidate = System.IO.Path.Combine(s_hrmDirOverride!, "GameEngine.HotReload.dll");
                if (s_verbose) { try { DebugWrite($"Probe override dir='{s_hrmDirOverride}' candidate='{candidate}' exists={System.IO.File.Exists(candidate)}"); } catch { } }
                if (TryLoadHrmAt(candidate)) goto HRM_READY;
            }
            catch { }
        }

        // 2) Try explicit environment hint first (useful for tests/tools)
        if (s_hotReloadType == null && string.IsNullOrEmpty(s_hrmDirOverride))
        {
            try
            {
                string? envPath = null; try { envPath = Environment.GetEnvironmentVariable("GE_HRM_PATH"); } catch {}
                if (s_verbose) { try { DebugWrite($"Probe GE_HRM_PATH='{envPath}'"); } catch { } }
                if (!string.IsNullOrEmpty(envPath) && TryLoadHrmAt(envPath)) goto HRM_READY;
                string? envDir = null; try { envDir = Environment.GetEnvironmentVariable("GE_HRM_DIR"); } catch {}
                if (s_verbose) { try { DebugWrite($"Probe GE_HRM_DIR='{envDir}'"); } catch { } }
                if (!string.IsNullOrEmpty(envDir))
                {
                    var candidate = System.IO.Path.Combine(envDir!, "GameEngine.HotReload.dll");
                    if (s_verbose) { try { DebugWrite($"Probe from GE_HRM_DIR candidate='{candidate}' exists={System.IO.File.Exists(candidate)}"); } catch { } }
                    if (TryLoadHrmAt(candidate)) goto HRM_READY;
                }
            }
            catch { }
        }

        // 3) Try to load from the CoreBridge assembly directory (via AssemblyDependencyResolver-backed ALC)
        if (s_hotReloadType == null)
        {
            try
            {
                var asmPath = typeof(CoreBridge).Assembly.Location;
                var dir = System.IO.Path.GetDirectoryName(asmPath)!;
                var candidate = System.IO.Path.Combine(dir, "GameEngine.HotReload.dll");
                DebugWrite($"Probe CoreBridge dir: '{candidate}', exists={System.IO.File.Exists(candidate)}");
                if (TryLoadHrmAt(candidate)) goto HRM_READY;
            }
            catch { }
        }

        // 3) Try AppContext.BaseDirectory using resolver-backed ALC
        if (s_hotReloadType == null)
        {
            try
            {
                var baseDir = AppContext.BaseDirectory ?? string.Empty;
                if (!string.IsNullOrEmpty(baseDir))
                {
                    var candidate = System.IO.Path.Combine(baseDir, "GameEngine.HotReload.dll");
                    if (s_verbose) { try { DebugWrite($"Probe BaseDirectory candidate='{candidate}' exists={System.IO.File.Exists(candidate)}"); } catch { } }
                    if (TryLoadHrmAt(candidate)) goto HRM_READY;
                }
            }
            catch { }
        }

        // 3b) Try current working directory (native tests copy HRM here)
        if (s_hotReloadType == null)
        {
            try
            {
                var cwd = System.IO.Directory.GetCurrentDirectory();
                var candidate = System.IO.Path.Combine(cwd, "GameEngine.HotReload.dll");
                if (s_verbose) { try { DebugWrite($"Probe CWD candidate='{candidate}' exists={System.IO.File.Exists(candidate)}"); } catch { } }
                if (TryLoadHrmAt(candidate)) goto HRM_READY;
            }
            catch { }
        }

        // 4) final failure — resolution is exe-dir-anchored only (override dir, env
        // hints, CoreBridge assembly dir, AppContext.BaseDirectory, CWD). Walking up
        // to an engine repo root is a shipped-editor bug: it works on a dev machine
        // and breaks the moment the build is relocated (PathResolver is the native
        // precedent; anything the runtime needs is STAGED next to the executable).
        if (s_hotReloadType == null) { try { DebugWrite("EnsureHotReloadTypeLoaded: failure"); } catch { } return false; }

HRM_READY:
        // If we have the type but capability bindings are missing, (re)bind from the existing type without reloading
        if (s_hotReloadType != null && (s_miLoadFromBytes == null || s_miCall == null || (s_miCallInDomain == null && (s_miQueryExportInDomain == null || s_miInvokeByToken == null))))
        {
            try
            {
                var t = s_hotReloadType;
                s_miInitialize = t?.GetMethod("Initialize", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
                s_miLoad = t?.GetMethod("LoadUserScriptsAssembly", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
                s_miLoadFromBytes = t?.GetMethod("LoadCompiledAssembly", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static, binder: null, types: new[] { typeof(byte[]) }, modifiers: null);
                s_miUnload = t?.GetMethod("UnloadUserScriptsAssembly", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
                s_miUnloadDomain = t?.GetMethod("UnloadDomain", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static, binder: null, types: new[] { typeof(ulong) }, modifiers: null);
                s_miReload = t?.GetMethod("ReloadUserScriptsAssembly", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
                // The Type.GetType fast path reaches HRM_READY without going through
                // TryLoadHrmAt, so the invocation entry points must be (re)bound here
                // too or CallUserScriptsMethod permanently fails with -3.
                s_miCall = t?.GetMethod("CallMethodInUserScripts", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static, binder: null, types: new[] { typeof(string) }, modifiers: null) ?? s_miCall;
                s_miCallWithArgs = t?.GetMethod("CallMethodInUserScripts", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static, binder: null, types: new[] { typeof(string), typeof(object[]) }, modifiers: null) ?? s_miCallWithArgs;
                var bound2 = GameEngine.CoreBridge.Generated.HrmBindings.Bind(t!);
                s_miCallInDomain = bound2.MiCallInDomain ?? s_miCallInDomain;
                s_miQueryExportInDomain = bound2.MiQueryExportInDomain ?? s_miQueryExportInDomain;
                s_miInvokeByToken = bound2.MiInvokeByToken ?? s_miInvokeByToken;
                s_miGetUserScriptsAssemblyInfo = bound2.MiGetUserScriptsAssemblyInfo ?? s_miGetUserScriptsAssemblyInfo;
                s_miPreloadAssemblyContext = bound2.MiPreloadAssemblyContext ?? s_miPreloadAssemblyContext;
                s_miSwapPreloadedContext = bound2.MiSwapPreloadedContext ?? s_miSwapPreloadedContext;
                s_miCleanupOldContext = bound2.MiCleanupOldContext ?? s_miCleanupOldContext;
                BindHrmDelegates();
            }
            catch { }
        }
        if (s_verbose) { try { DebugWrite($"EnsureHotReloadTypeLoaded: success asm='{s_hotReloadType?.Assembly?.Location}'"); } catch { } }
        // Capability check: we require LoadCompiledAssembly(byte[]);
        // For invocation, accept either CallInDomain(ulong,string) OR the token pair QueryExportInDomain+InvokeByToken
        bool hasBytesLoader = s_miLoadFromBytes != null;
        bool hasCallInDomain = s_miCallInDomain != null || s_dCallInDomain != null;
        bool hasTokenPair = (s_miQueryExportInDomain != null || s_dQueryExportInDomain != null) && (s_miInvokeByToken != null || s_dInvokeByToken != null);
        if (s_verbose) { try { DebugWrite($"EnsureHotReloadTypeLoaded: caps bytesLoader={hasBytesLoader} callInDomain={hasCallInDomain} tokenPair={hasTokenPair}"); } catch { } }
        bool needsRetry = !hasBytesLoader || !(hasCallInDomain || hasTokenPair);

        if (needsRetry)
        {
            // A resolved type without the required capabilities is a stale/foreign
            // HotReload assembly — clear the bindings and fail; there is no further
            // location to probe (resolution is exe-dir-anchored, see above).
            s_hotReloadType = null;
            s_miInitialize = s_miLoad = s_miUnload = s_miReload = s_miLoadFromBytes = s_miCall = s_miCallWithArgs = null;
            s_miCallInDomain = s_miQueryExportInDomain = s_miInvokeByToken = s_miPreloadAssemblyContext = s_miSwapPreloadedContext = s_miCleanupOldContext = s_miGetUserScriptsAssemblyInfo = null;
            s_dCallInDomain = null;
            s_dQueryExportInDomain = null;
            s_dInvokeByToken = null;
            s_dPreloadAssemblyContext = null;
            s_dSwapPreloadedContext = null;
            s_dCleanupOldContext = null;
            s_isWrapperQuery = false; s_isWrapperInvoke = false;
            return false;
        }

        if (s_verbose) try { Console.WriteLine($"[CoreBridge] HotReloadManager resolved from '{s_hotReloadType?.Assembly?.Location}', LoadFromBytes={(s_miLoadFromBytes!=null)}"); } catch {}
        return true;
    }

    private static int LoadFromBytesCore(byte* bytes, uint len)
    {
        try
        {
            if (!EnsureHotReloadTypeLoaded()) { Console.WriteLine("[CoreBridge] ❌ HRM type not loaded in LoadFromBytesCore"); DebugWrite("LoadFromBytesCore: HRM type not loaded"); return -3; }
            if (s_miLoadFromBytes == null) { Console.WriteLine("[CoreBridge] ❌ s_miLoadFromBytes is null (LoadCompiledAssembly not found)"); DebugWrite("LoadFromBytesCore: s_miLoadFromBytes is null"); return -3; }
            if (bytes == null || len == 0) { Console.WriteLine("[CoreBridge] ❌ LoadFromBytesCore got empty buffer"); DebugWrite("LoadFromBytesCore: empty buffer"); return -2; }
            if (s_verbose) try { Console.WriteLine($"[CoreBridge] LoadFromBytesCore len={len}"); } catch { } if (s_verbose) DebugWrite($"LoadFromBytesCore: len={len}");
            var managed = new ReadOnlySpan<byte>(bytes, (int)len).ToArray();
            var result = s_miLoadFromBytes.Invoke(null, new object[] { managed });
            if (result is int intRes) return intRes;
            return -1;
        }
        catch (Exception ex)
        {
            try
            {
                // Map common managed loader failures to distinct codes for easier native diagnosis
                var tie = ex as System.Reflection.TargetInvocationException;
                var inner = tie?.InnerException ?? ex;
                int code = -1;
                if (inner is System.IO.FileLoadException) code = -201;
                else if (inner is System.BadImageFormatException) code = -202;
                else if (inner is System.TypeLoadException) code = -203;
                else if (inner is System.IO.FileNotFoundException) code = -204;
                Console.WriteLine($"[CoreBridge] ❌ LoadFromBytesCore failed: {inner.GetType().Name}: {inner.Message} (code={code})");
                DebugWrite($"LoadFromBytesCore: exception {inner.GetType().Name}: {inner.Message} (code={code})");
                return code;
            }
            catch { return -1; }
        }
    }

    private static int CallUserScriptsMethodCore(byte* methodNameUtf8, uint len)
    {
        try
        {
            if (!EnsureHotReloadTypeLoaded()) {
                Console.WriteLine("[CoreBridge] ❌ HotReload type not found (EnsureHotReloadTypeLoaded failed)");
                return -3;
            }
            if (s_miCall == null) {
                Console.WriteLine("[CoreBridge] ❌ HotReloadManager.CallMethodInUserScripts method not found");
                return -3;
            }
            string raw = (methodNameUtf8 == null || len == 0) ? string.Empty : System.Text.Encoding.UTF8.GetString(methodNameUtf8, (int)len);
            try
            {
                // Parse CALL:Method?i32=3&i32=4 into method and args
                string methodName = raw;
                object[]? args = null;
                int q = raw.IndexOf('?');
                if (q >= 0)
                {
                    methodName = raw.Substring(0, q);
                    var query = raw.Substring(q + 1);
                    var parts = query.Split('&', StringSplitOptions.RemoveEmptyEntries);
                    var argList = new System.Collections.Generic.List<object>(parts.Length);
                    foreach (var part in parts)
                    {
                        var kv = part.Split('=', 2);
                        if (kv.Length != 2) continue;
                        var key = kv[0]; var val = kv[1];
                        if (key.Equals("i32", StringComparison.OrdinalIgnoreCase))
                        {
                            if (int.TryParse(val, out var iv)) argList.Add(iv);
                        }
                        // future: f32=, str= (percent-encoding)
                    }
                    args = argList.ToArray();
                }

                object? result;
                if (args == null || args.Length == 0)
                {
                    result = s_miCall.Invoke(null, new object[] { methodName });
                }
                else if (s_miCallWithArgs != null)
                {
                    result = s_miCallWithArgs.Invoke(null, new object[] { methodName, args });
                }
                else
                {
                    // Fallback: try to call single-arg version; managed side may route args-less call
                    result = s_miCall.Invoke(null, new object[] { methodName });
                }
                return result is int i ? i : -1;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[CoreBridge] ❌ Invoking HotReloadManager.CallMethodInUserScripts failed: {ex.GetType().Name}: {ex.Message}");
                return -1; // general/unexpected error
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[CoreBridge] ❌ CallUserScriptsMethodCore wrapper failed: {ex.GetType().Name}: {ex.Message}");
            return -1;
        }
    }

/// <summary>
/// Call a method inside the user scripts assembly by fully-qualified name (UTF-8). Returns result or negative on error.
/// </summary>

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int CallUserScriptsMethod(byte* methodNameUtf8, uint len)
    {
        return CallUserScriptsMethodCore(methodNameUtf8, len);
    }

/// <summary>
/// Get information about the currently loaded user scripts assembly. Returns 0 on success, negative on error.
/// </summary>

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int GetUserScriptsAssemblyInfo()
    {
        try
        {
            if (!EnsureHotReloadTypeLoaded()) return -1;
            var mi = s_miGetUserScriptsAssemblyInfo;

            if (mi == null) return -1;
            var ret = mi.Invoke(null, null);
            return ret is int i ? i : 0;
        }
        catch { return -1; }
    }

/// <summary>
/// Lightweight ping for connectivity testing. Returns a constant value.
/// </summary>

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int SilentPing() { return 7; }

/// <summary>
/// Sanity ping that logs when verbose. Returns a constant value (42).
/// </summary>

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int SanityPing()
    {
        if (s_verbose) { try { Console.WriteLine("[CoreBridge] SanityPing - managed invocation working"); } catch { } }
        return 42;
    }


    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
		    [GenerateBinding(GE_CB_OnAfterLoad)]

    private static void OnAfterLoad()
    {
        try
        {
            // Invalidate façade caches on reload completion
            var ctx = s_engineInstance;
            if (ctx != null)
            {
                GameEngine.WorldsProvider.Invalidate(ctx);
                // Minimal warm-up: touch primary world once to re-establish handles lazily
                try { var _ = GameEngine.Engine.Current.Worlds.Primary; } catch { }
            }
        }
        catch { }
    }


    // Internal managed helpers to allow tests and managed callers to simulate lifecycle
    internal static void InternalBeforeUnload()
    {
        try
        {
            var ctx = s_engineInstance;
            if (ctx != null)
            {
                GameEngine.WorldsProvider.Invalidate(ctx);
            }
        }
        catch { }
    }

    internal static void InternalAfterLoad()
    {
        try
        {
            var ctx = s_engineInstance;
            if (ctx != null)
            {
                GameEngine.WorldsProvider.Invalidate(ctx);
                // Minimal warm-up: touch primary world once to re-establish handles lazily
                try { var _ = GameEngine.Engine.Current.Worlds.Primary; } catch { }
            }
        }
        catch { }
    }

    /// <summary>
    /// Initializes CoreBridge and registers essential callbacks with the engine.
    /// Exposed as an unmanaged entry point via function pointer resolution from native.
    /// </summary>
        /// <summary>
        /// Initializes CoreBridge and registers managed callbacks (hot-reload, token API, diagnostics).
        /// Call from native once per process after CoreCLR is available.
        /// Returns 0 on success; non-zero on failure.
        /// </summary>

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int Initialize()
    {
        try
        {
            if (s_verbose) try { Console.WriteLine("[CoreBridge] Initialize starting"); } catch { }
            try { DebugWrite("Initialize: enter"); } catch { }

            // Skipping platform resolver install during CLR bootstrap to avoid loader-lock
            // It will be installed lazily by RegisterEngineInstance.

            // Defer native binding until after CLR is fully initialized.
            // Native binding will be established via engine instance registration (RegisterEngineInstance)
            // that the host calls immediately after this Initialize() returns.
            // The host's fault injection: report a managed ABI mismatch.
            if ((s_hostSwitches & HostSwitches.ForceAbiMismatch) != 0) return -3;
            try { DebugWrite("Initialize: deferring native binding (no NativeLibrary.Load during CLR init)"); } catch { }

            // Do not register managed callbacks here; defer until engine instance registration
            // to avoid loading native bindings during CLR bootstrap (prevents loader-lock in tests/CI).
            // Callbacks will be registered idempotently when RegisterEngineInstance runs.

            // Skipping Console redirection during CoreBridge.Initialize to avoid early interaction with CLR bootstrap
            // It will be installed lazily by RegisterEngineInstance.

            // Skip eager HotReloadManager load during CLR bootstrap to minimize dependencies
            // HRM will be discovered/used on-demand by callers later.

            try { DebugWrite("Initialize: exit"); } catch { }
            return 0;
        }
        catch
        {
            // Avoid noisy console writes here; return generic failure
            try { DebugWrite("Initialize: exception"); } catch { }
            return -1;
        }
    }

        // -------- Demo-friendly managed wrappers (no unmanaged entrypoint) --------
        private static int LoadUserScriptsAssemblyImpl(string path)
        {
            try
            {
                if (!EnsureHotReloadTypeLoaded()) return -1;
                if (s_miLoad == null) return -1;
                var result = s_miLoad.Invoke(null, new object[] { path });
                return result is int i ? i : 0;
            }
            catch { return -1; }
        }

        private static int UnloadUserScriptsAssemblyImpl()
        {
            try
            {
                if (!EnsureHotReloadTypeLoaded()) return -1;
                if (s_miUnload == null) return -1;
                var result = s_miUnload.Invoke(null, null);
                return result is int i ? i : 0;
            }
            catch { return -1; }
        }

        private static int ReloadUserScriptsAssemblyImpl()
        {
            try
            {
                if (!EnsureHotReloadTypeLoaded()) return -1;
                if (s_miReload == null) return -1;
                var result = s_miReload.Invoke(null, null);
                return result is int i ? i : 0;
            }
            catch { return -1; }
        }

        /// <summary>
        /// Managed-only setup used by the isolated demo: redirects Console to GE_Log and calls HotReloadManager.Initialize if present.
        /// </summary>
        public static int Demo_InitializeManaged()
        {
            try
            {
                bool disableRedirect = false; try { disableRedirect = Environment.GetEnvironmentVariable("GE_DISABLE_CONSOLE_REDIRECT") == "1"; } catch { }
                if (!disableRedirect)
                {
                    try { Console.SetOut(new EngineLogWriter()); Console.SetError(new EngineLogWriter()); } catch { }
                }
                if (!EnsureHotReloadTypeLoaded())
                {
                    // Try to bind directly to already-loaded HRM in default ALC
                    var t = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: false);
                    if (t != null)
                    {
                        try
                        {
                            s_hotReloadType = t;
                            // Cache method infos and generated bindings
                            s_miInitialize = t.GetMethod("Initialize", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
                            s_miLoad = t.GetMethod("LoadUserScriptsAssembly", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
                            s_miLoadFromBytes = t.GetMethod("LoadCompiledAssembly", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static, binder: null, types: new[] { typeof(byte[]) }, modifiers: null);
                            s_miUnload = t.GetMethod("UnloadUserScriptsAssembly", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
                            s_miUnloadDomain = t.GetMethod("UnloadDomain", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static, binder: null, types: new[] { typeof(ulong) }, modifiers: null);
                            s_miReload = t.GetMethod("ReloadUserScriptsAssembly", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
                            var bound = GameEngine.CoreBridge.Generated.HrmBindings.Bind(t);
                            s_miCallInDomain = bound.MiCallInDomain;
                            s_miQueryExportInDomain = bound.MiQueryExportInDomain;
                            s_miInvokeByToken = bound.MiInvokeByToken;
                            s_miGetUserScriptsAssemblyInfo = bound.MiGetUserScriptsAssemblyInfo;
                            s_miPreloadAssemblyContext = bound.MiPreloadAssemblyContext;
                            s_miSwapPreloadedContext = bound.MiSwapPreloadedContext;
                            s_miCleanupOldContext = bound.MiCleanupOldContext;
                            s_miCall = t.GetMethod("CallMethodInUserScripts", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static, binder: null, types: new[] { typeof(string) }, modifiers: null);
                            s_miCallWithArgs = t.GetMethod("CallMethodInUserScripts", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static, binder: null, types: new[] { typeof(string), typeof(object[]) }, modifiers: null);
                        }
                        catch { }
                    }
                    else { return 0; }
                }
                if (s_miInitialize != null)
                {
                    var result = s_miInitialize.Invoke(null, null);
                    return result is int i ? i : 0;
                }
                return 0;
            }
            catch { return -1; }
        }

/// <summary>
/// Demo helper: load user scripts assembly from a managed path.
/// </summary>

        public static int Demo_LoadUserScriptsAssembly(string path) => LoadUserScriptsAssemblyImpl(path);
/// <summary>
/// Demo helper: unload the user scripts assembly.
/// </summary>

        public static int Demo_UnloadUserScriptsAssembly() => UnloadUserScriptsAssemblyImpl();
/// <summary>
/// Demo helper: reload the user scripts assembly.
/// </summary>

        public static int Demo_ReloadUserScriptsAssembly() => ReloadUserScriptsAssemblyImpl();

        /// <summary>
        /// Accept a compiled package (assembly + optional PDB) and create/swap a domain. Returns 0 on success.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int AcceptPackage(GE_PackageEntry* entries, uint entryCount, uint flags, ulong* outDomain)
        {
            try
            {
                if (s_verbose) DebugWrite($"AcceptPackage: entryCount={entryCount}");
                if (s_verbose) try { Console.WriteLine($"[CoreBridge] AcceptPackage entryCount={entryCount}"); } catch { }
                if (s_verbose)
                {
                    try
                    {
                        var e1 = Environment.GetEnvironmentVariable("GE_HRM_DIR") ?? "<null>";
                        var e2 = Environment.GetEnvironmentVariable("GE_HRM_PATH") ?? "<null>";
                        DebugWrite($"AcceptPackage: env GE_HRM_DIR='{e1}' GE_HRM_PATH='{e2}'");
                    } catch { }
                }
                if (entries == null || entryCount == 0 || outDomain == null) { Console.WriteLine("[CoreBridge] AcceptPackage invalid args"); DebugWrite("AcceptPackage: invalid args"); return -2; }
                var first = entries[0];
                if (s_verbose) DebugWrite($"AcceptPackage: firstLen={first.assembly.length}");
                if (s_verbose) try { Console.WriteLine($"[CoreBridge] AcceptPackage first.len={first.assembly.length}"); } catch { }
                if (first.assembly.data == null || first.assembly.length == 0) { Console.WriteLine("[CoreBridge] AcceptPackage empty assembly bytes"); DebugWrite("AcceptPackage: empty bytes"); return -2; }
                if (EnsureHotReloadTypeLoaded() && s_miInitialize != null)
                {
                    try { var _ = s_miInitialize.Invoke(null, null); } catch { }
                }
                if (s_verbose) try { Console.WriteLine($"[CoreBridge] HRM type='{s_hotReloadType?.Assembly?.Location}', LoadFromBytes={(s_miLoadFromBytes!=null)}"); } catch {}
                int rc = LoadFromBytesCore(first.assembly.data, first.assembly.length);
                if (s_verbose) try { Console.WriteLine($"[CoreBridge] AcceptPackage LoadFromBytesCore rc={rc}"); } catch { }
                if (s_verbose) DebugWrite($"AcceptPackage: LoadFromBytesCore rc={rc}");
                // Ensure callbacks are registered now that a domain/binding likely exists
                try { EnsureManagedCallbacksRegistered(); } catch { }

                if (rc < 0) return rc;
                if (rc > 0) { *outDomain = (ulong)rc; return 0; }
                // Return the domain id directly from the load result if available
                try
                {
                    if (!EnsureHotReloadTypeLoaded()) { *outDomain = 0UL; return 0; }
                    // Prefer the stable m_CurrentDomainId after load
                    var field = s_hotReloadType!.GetField("m_CurrentDomainId", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static);
                    if (field != null)
                    {
                        var val = field.GetValue(null);
                        *outDomain = val is ulong u ? u : 0UL;
                    }
                    else { *outDomain = 0UL; }
                }
                catch { *outDomain = 0UL; }
                return 0;
            }
            catch (Exception ex) { Console.WriteLine($"[CoreBridge] AcceptPackage exception: {ex.GetType().Name}: {ex.Message}"); return -1; }
        }

        /// <summary>
        /// Debug-only: AcceptPackage without out-params. Returns a 64-bit packed value: [63..48]=rc (signed 16-bit), [47..0]=domain (masked to 48 bits).
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static long AcceptPackage_Packed(GE_PackageEntry* entries, uint entryCount, uint flags)
        {
            try
            {
                if (entries == null || entryCount == 0) return ((long)(short)(-2)) << 48;
                var first = entries[0];
                if (first.assembly.data == null || first.assembly.length == 0) return ((long)(short)(-2)) << 48;

                // Initialize HRM once if available
                if (EnsureHotReloadTypeLoaded() && s_miInitialize != null) { try { var _ = s_miInitialize.Invoke(null, null); } catch { } }

                int rc = LoadFromBytesCore(first.assembly.data, first.assembly.length);
                try { EnsureManagedCallbacksRegistered(); } catch { }
                if (rc < 0)
                {
                    return (((long)(short)rc) << 48);
                }

                ulong domain = 0UL;
                if (rc > 0) domain = (ulong)rc;
                else
                {
                    try
                    {
                        if (!EnsureHotReloadTypeLoaded()) domain = 0UL;
                        else
                        {
                            var field = s_hotReloadType!.GetField("m_CurrentDomainId", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static);
                            var val = field?.GetValue(null);
                            domain = val is ulong u ? u : 0UL;
                        }
                    }
                    catch { domain = 0UL; }
                }
                ulong dom48 = domain & 0x0000FFFFFFFFFFFFUL;
                long packed = (((long)(short)0) << 48) | (long)dom48; // rc==0 success
                return packed;
            }
            catch (Exception ex) { try { Console.WriteLine($"[CoreBridge] AcceptPackage_Packed exception: {ex.GetType().Name}: {ex.Message}"); } catch { } return ((long)(short)(-1)) << 48; }
        }


        /// <summary>
        /// Debug-only: Accept assembly bytes directly (no struct pointers). Returns packed rc/domain like AcceptPackage_Packed.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static long AcceptAssembly_Packed(byte* asmData, uint asmLen, uint flags)
        {
            try
            {
                if (asmData == null || asmLen == 0) return ((long)(short)(-2)) << 48;
                if (EnsureHotReloadTypeLoaded() && s_miInitialize != null) { try { var _ = s_miInitialize.Invoke(null, null); } catch { } }
                int rc = LoadFromBytesCore(asmData, asmLen);
                try { EnsureManagedCallbacksRegistered(); } catch { }
                if (rc < 0) return (((long)(short)rc) << 48);
                ulong domain = 0UL;
                if (rc > 0) domain = (ulong)rc;
                else
                {
                    try
                    {
                        if (!EnsureHotReloadTypeLoaded()) domain = 0UL;
                        else
                        {
                            var field = s_hotReloadType!.GetField("m_CurrentDomainId", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static);
                            var val = field?.GetValue(null);
                            domain = val is ulong u ? u : 0UL;
                        }
                    }
                    catch { domain = 0UL; }
                }
                ulong dom48 = domain & 0x0000FFFFFFFFFFFFUL;
                return (((long)(short)0) << 48) | (long)dom48;
            }
            catch (Exception ex) { try { Console.WriteLine($"[CoreBridge] AcceptAssembly_Packed exception: {ex.GetType().Name}: {ex.Message}"); } catch { } return ((long)(short)(-1)) << 48; }
        }


        /// <summary>
        /// Get the current runtime domain id as tracked by HotReloadManager.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static ulong GetCurrentRuntimeDomainId()
        {
            try
            {
                if (!EnsureHotReloadTypeLoaded()) return 0UL;
                var field = s_hotReloadType!.GetField("m_CurrentDomainId", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static);
                if (field == null) return 0UL;
                var val = field.GetValue(null);
                return val is ulong u ? u : 0UL;
            }
            catch { return 0UL; }
        }

        /// <summary>
        /// Dump HotReloadManager state for diagnostics (verbose). Returns 0 on success.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int DebugDumpHotReloadState()
        {
            try
            {
                if (!EnsureHotReloadTypeLoaded()) { if (s_verbose) try { Console.WriteLine("[CoreBridge] DebugDump: HRM not loaded"); } catch { } return -1; }
                if (s_verbose) try { Console.WriteLine($"[CoreBridge] DebugDump: HRM type asm='{s_hotReloadType!.Assembly.Location}'"); } catch { }
                var fldCur = s_hotReloadType!.GetField("m_CurrentDomainId", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static);
                var fldDomains = s_hotReloadType!.GetField("m_Domains", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static);
                ulong curDom = 0UL;
                if (fldCur != null)
                {
                    var v = fldCur.GetValue(null);
                    if (v is ulong u) curDom = u;
                }
                if (s_verbose) try { Console.WriteLine($"[CoreBridge] DebugDump: m_CurrentDomainId={curDom}"); } catch { }
                if (fldDomains != null)
                {
                    var dict = fldDomains.GetValue(null) as System.Collections.IDictionary;
                    if (dict != null)
                    {
                        Console.WriteLine($"[CoreBridge] DebugDump: domains.Count={dict.Count}");
                        foreach (System.Collections.DictionaryEntry de in dict)
                        {
                            var key = de.Key;
                            var val = de.Value;
                            ulong dom = key is ulong k ? k : 0UL;
                            var asmProp = val?.GetType().GetProperty("Asm");
                            var asmObj = asmProp?.GetValue(val);

                            var asm = asmObj as System.Reflection.Assembly;
                            string loc = asm != null ? (asm.Location ?? asm.FullName ?? "(null)") : "(null)";
                            if (s_verbose) try { Console.WriteLine($"[CoreBridge] DebugDump: domain {dom} asm='{loc}'"); } catch { }
                        }
                    }
                }
                return 0;
            }
            catch (System.Exception ex) { Console.WriteLine($"[CoreBridge] DebugDump exception: {ex.GetType().Name}: {ex.Message}"); return -1; }
        }



        // Token-based export API (Managed wrappers for native to call via UCO)
        /// <summary>
        /// Query a managed export by FQN in the specified domain and return a stable token.
        /// Returns 0 on success; negative values on failure.
        /// </summary>

        [GenerateBinding(GE_CB_QueryExport)]
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int QueryExport(ulong domain, byte* nameUtf8, uint nameLen, ulong* outToken)
        {
            try
            {
                // Late ensure callbacks are registered (idempotent per-binding)
                try { EnsureManagedCallbacksRegistered(); } catch { }

                if (!EnsureHotReloadTypeLoaded()) { if (s_verbose) try { Console.WriteLine("[CoreBridge] QueryExport: HRM not loaded"); } catch { } return -3; }
                if (s_miQueryExportInDomain == null) { if (s_verbose) try { Console.WriteLine("[CoreBridge] QueryExport: HRM.QueryExportInDomain not found"); } catch { } return -3; }
                string name = (nameUtf8 == null || nameLen == 0) ? string.Empty : System.Text.Encoding.UTF8.GetString(nameUtf8, (int)nameLen);
                if (s_verbose) try { Console.WriteLine($"[CoreBridge] QueryExport: domain={domain} name='{name}'"); } catch { }
                if (s_dQueryExportInDomain == null) BindQueryExportDelegate();
                var queryExport = s_dQueryExportInDomain;
                if (queryExport == null) return -3;
                ulong tok = 0UL;
                int rc = queryExport(domain, name, ref tok);
#if DEBUG
                System.Threading.Interlocked.Increment(ref s_dbgDelegateQueryExport);
#endif
                try { if (outToken != null) *outToken = tok; } catch { }
                if (s_verbose) try { Console.WriteLine($"[CoreBridge] QueryExport: rc={rc} outToken={(outToken!=null ? (*outToken).ToString() : "null")}"); } catch { }
                return rc;
            }
            catch (Exception ex) { try { Console.WriteLine($"[CoreBridge] QueryExport exception: {ex.Message}"); } catch { } return -1; }
        }

        /// <summary>Invoke a previously queried token in the specified domain.</summary>
        /// <param name="domain">Target domain ID.</param>
        /// <param name="token">QueryExport token to invoke.</param>
        /// <param name="outResult">Receives the method result on success.</param>
        /// <returns>0 on success; negative on failure.</returns>


        [GenerateBinding(GE_CB_InvokeByToken)]
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int InvokeByToken(ulong domain, ulong token, int* outResult)
        {
            try
            {
                if (outResult == null) return -2;
                // Late ensure callbacks are registered (idempotent per-binding)
                try { EnsureManagedCallbacksRegistered(); } catch { }

                if (!EnsureHotReloadTypeLoaded()) return -3;
                if (s_miInvokeByToken == null) return -3;
                if (s_dInvokeByToken == null) BindInvokeByTokenDelegate();
                var invokeByToken = s_dInvokeByToken;
                if (invokeByToken == null) return -3;
                int rc = invokeByToken(domain, token);
#if DEBUG
                System.Threading.Interlocked.Increment(ref s_dbgDelegateInvokeByToken);
#endif
                if (rc >= 0) { *outResult = rc; return 0; }
                return -1;
            }
            catch { return -1; }
        }


/// <summary>
/// Invoke a method inside a specific domain by name. Writes the result to outResult; returns 0 on success.
/// </summary>

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int InvokeInDomain(ulong domain, byte* methodNameUtf8, uint methodNameLen, int* outResult)
        {
            try
            {
                if (outResult == null) return -2;
                string dbgMethod = (methodNameUtf8 == null || methodNameLen == 0) ? string.Empty : System.Text.Encoding.UTF8.GetString(methodNameUtf8, (int)methodNameLen);
                if (s_verbose) try { Console.WriteLine($"[CoreBridge] InvokeInDomain enter: domain={domain} method='{dbgMethod}'"); } catch { }
                int rc = CallUserScriptsMethodInDomain(domain, methodNameUtf8, methodNameLen);
                if (rc >= 0)
                {


                    *outResult = rc;
                    if (s_verbose) try { Console.WriteLine($"[CoreBridge] InvokeInDomain exit: rc={rc}"); } catch { }
                    return 0;
                }
                // Negative result from managed indicates failure; skip managed token fallback to let native handle it
                if (rc < 0)
                {
                    if (s_verbose) try { Console.WriteLine($"[CoreBridge] InvokeInDomain immediate fail (rc={rc})"); } catch { }
                    return -1;
                }

				// Token fallback when string routing fails
				try {
				    string name = dbgMethod;
				    ulong tok = 0UL;
				    int qrc;
				    if (!EnsureHotReloadTypeLoaded()) return -3;
				    if (s_miQueryExportInDomain != null && s_miInvokeByToken != null)
				    {
				        ulong dom2 = domain;
				        try {
				            if (dom2 == 0 && s_hotReloadType != null)
				            {
				                var f = s_hotReloadType.GetField("m_CurrentDomainId", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static);
				                var v = f?.GetValue(null);
				                if (v is ulong u && u != 0UL) dom2 = u;
				            }
				        } catch { }
				        if (s_dQueryExportInDomain != null) qrc = s_dQueryExportInDomain(dom2, name, ref tok);
				        else qrc = HotReloadApi.QueryExportInDomain(s_miQueryExportInDomain, dom2, name, ref tok);
				        if (qrc != 0 && dom2 == 0 && domain == 0)
				        {
				            // Final fallback: query current domain id via reflection
				            try {
				                var t = s_hotReloadType;
				                var f2 = t?.GetField("m_CurrentDomainId", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static);
				                var v2 = f2?.GetValue(null);
				                if (v2 is ulong u2 && u2 != 0UL) dom2 = u2;
				            } catch { dom2 = 0; }
				            if (dom2 != 0)
				            {
				                if (s_dQueryExportInDomain != null) qrc = s_dQueryExportInDomain(dom2, name, ref tok);
				                else qrc = HotReloadApi.QueryExportInDomain(s_miQueryExportInDomain, dom2, name, ref tok);
				            }
				        }
				        if (qrc == 0 && tok != 0UL)
				        {
				            int res = (s_dInvokeByToken != null) ? s_dInvokeByToken(dom2, tok) : HotReloadApi.InvokeByToken(s_miInvokeByToken, dom2, tok);
				            if (res >= 0)
				            {
				                *outResult = res;
				                if (s_verbose) try { Console.WriteLine($"[CoreBridge] InvokeInDomain exit via token: res={res} tok={tok} dom={dom2}"); } catch { }
				                return 0;
				            }
				            // Negative result from managed indicates failure; preserve existing outResult
				            return -1;
				        }
				    }
				} catch { }

                if (s_verbose) try { Console.WriteLine($"[CoreBridge] InvokeInDomain exit (fail): rc={rc}"); } catch { }
                return -1;
            }
            catch (Exception ex) { try { Console.WriteLine($"[CoreBridge] InvokeInDomain exception: {ex.GetType().Name}: {ex.Message}"); } catch { } return -1; }
        }


		private static int CallUserScriptsMethodInDomain(ulong domain, byte* methodNameUtf8, uint len)
		{
			try
			{
				if (!EnsureHotReloadTypeLoaded()) { try { Console.WriteLine("[CoreBridge] ❌ EnsureHotReloadTypeLoaded returned false in InvokeInDomain"); } catch { } return -3; }
				if (s_verbose) { try { Console.WriteLine($"[CoreBridge] HRM s_hotReloadType='{s_hotReloadType?.Assembly?.Location}'"); } catch { } }

				try { DebugWrite($"InvokeInDomain: HRM loaded alcAsm='{s_hotReloadType?.Assembly?.Location}'"); } catch { }

				var mi = s_miCallInDomain;


				try { DebugWrite($"InvokeInDomain: s_miCallInDomain null={(mi==null)}"); } catch { }

				if (mi == null) { try { Console.WriteLine("[CoreBridge] ❌ HotReloadManager.CallMethodInDomain method not found"); } catch { } return -3; }

				string method = (methodNameUtf8 == null || len == 0) ? string.Empty : System.Text.Encoding.UTF8.GetString(methodNameUtf8, (int)len);
				if (s_verbose) { try { Console.WriteLine($"[CoreBridge] → HRM.CallMethodInDomain(domain={domain}, method='{method}')"); } catch { } }
				try { DebugWrite($"InvokeInDomain: method='{method}' domain={domain}"); } catch { }



					// Sanitize method string (trim at first NUL and whitespace)
					int __nul = method.IndexOf('\0');
					if (__nul >= 0) method = method.Substring(0, __nul);
					method = method.Trim();

					// Further sanitize: keep only [A-Za-z0-9_.] from the start; stop at first invalid char
					int __k = 0;
					while (__k < method.Length)
					{
						char c = method[__k];
						bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
						if (!ok) break;
						__k++;
					}
					if (__k < method.Length) method = method.Substring(0, __k);

					// Minimal: call HRM and return its code without any legacy or diagnostic fallbacks
					if (s_dCallInDomain != null) return s_dCallInDomain(domain, method);
					return HotReloadApi.CallInDomain(mi, domain, method);
			}


			catch (Exception ex) { try { Console.WriteLine($"[CoreBridge] ❌ InvokeInDomain failed: {ex.GetType().Name}: {ex.Message}"); } catch { } return -1; }
		}

			/// <summary>
			/// Preload a new assembly context from a compiled package path. Returns 0 on success.
			/// </summary>
				[GenerateBinding(GE_CB_PreloadAssemblyContext)]

			[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
			public static int PreloadAssemblyContext(byte* pathUtf8, uint len)
			{
				try
				{
					if (!EnsureHotReloadTypeLoaded()) return -3;
					// Ensure HRM is initialized once prior to preload (with fallback resolution)
					if (!s_hrmInitialized)
					{
						try {
							if (s_miInitialize != null) { var _ = s_miInitialize.Invoke(null, null); }
							else { var t = s_hotReloadType; var miInit = t?.GetMethod("Initialize", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static); if (miInit != null) { s_miInitialize = miInit; var _ = miInit.Invoke(null, null); } }
							s_hrmInitialized = true;
						} catch { }
					}

					string path = (pathUtf8 == null || len == 0) ? string.Empty : System.Text.Encoding.UTF8.GetString(pathUtf8, (int)len);
					int rc;
					if (s_dPreloadAssemblyContext != null)
					{
						rc = s_dPreloadAssemblyContext(path);
					}
					else
					{
						var mi = s_miPreloadAssemblyContext;
						if (mi == null) return -3;
						rc = HotReloadApi.InvokeStringArg(mi, path);
					}
					if (rc != 0)
					{
						try
						{
							var binding = GetCurrentBinding();
							if (binding != null)
							{
								string reason = $"Preload failed (rc={rc}) path='{path}'";
								System.IntPtr reasonPtr = System.IntPtr.Zero;
								try { reasonPtr = System.Runtime.InteropServices.Marshal.StringToHGlobalAnsi(reason); /* ABI v1.0: no NotifyReloadEvent; no-op */ _ = 0; }
								finally { if (reasonPtr != System.IntPtr.Zero) System.Runtime.InteropServices.Marshal.FreeHGlobal(reasonPtr); }
							}
						}
						catch { }
					}
					return rc;
				}
				catch { return -1; }
			}

                // Managed-friendly wrappers for tests/internals (avoid UnmanagedCallersOnly restrictions)
		        internal static int ManagedPreloadAssemblyContext(string path)
		        {
		            try
		            {
		                DebugWrite($"ManagedPreloadAssemblyContext enter path='{path}' exists={(System.IO.File.Exists(path))}");
		                bool ok = EnsureHotReloadTypeLoaded();
		                #if DEBUG
		                try { Console.WriteLine($"[CoreBridge] ManagedPreloadAssemblyContext EnsureHotReloadTypeLoaded={ok}"); } catch { }
		                #endif
		                if (!ok) return -3;
						// Ensure HRM is initialized once prior to managed preload (with fallback resolution)
						try
						{
						    if (s_miInitialize != null)
						    {
						        var _ = s_miInitialize.Invoke(null, null);
						    }
						    else
						    {
						        var t = s_hotReloadType;
						        var miInit = t?.GetMethod("Initialize", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
						        if (miInit != null)
						        {
						            s_miInitialize = miInit;
						            var _ = miInit.Invoke(null, null);
						        }
						    }
						}
						catch { }
				
		                // ARCHITECTURE: Prefer the string-based PreloadAssemblyContext(path) API that
		                // HrmBindings is generated for. The byte[] overload is reserved for internal
		                // async/compile-server pipelines where HRM manages cached bytes + paths.
		                if (s_dPreloadAssemblyContext != null)
		                {
		                    #if DEBUG
		                    try { Console.WriteLine("[CoreBridge] ManagedPreloadAssemblyContext: using delegate PreloadAssemblyContext(string)"); } catch { }
		                    #endif
		                    return s_dPreloadAssemblyContext(path);
		                }
		
		                var mi = s_miPreloadAssemblyContext;
		                if (mi != null)
		                {
		                    #if DEBUG
		                    try { Console.WriteLine($"[CoreBridge] ManagedPreloadAssemblyContext: invoking MiPreloadAssemblyContext('{mi.Name}')"); } catch { }
		                    #endif
		                    // Expect the modern signature PreloadAssemblyContext(string path)
		                    if (mi.GetParameters().Length == 1 && mi.GetParameters()[0].ParameterType == typeof(string))
		                    {
		                        return HotReloadApi.InvokeStringArg(mi, path);
		                    }
		                }
		
		                // Fallback: older HRM versions may only expose the byte[] overload. In that case,
		                // use it as a last resort. This path does not attempt to coordinate HRM's internal
		                // m_PreloadedAssemblyPath bytes cache and is intended purely for backwards
		                // compatibility with test harnesses / legacy hosts.
		                var miBytes = s_hotReloadType?.GetMethod(
		                    "PreloadAssemblyContext",
		                    System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static,
		                    binder: null,
		                    types: new[] { typeof(byte[]), typeof(byte[]) },
		                    modifiers: null);
		                #if DEBUG
		                try { Console.WriteLine($"[CoreBridge] ManagedPreloadAssemblyContext fallback miBytes={(miBytes!=null)}"); } catch { }
		                #endif
		                if (miBytes != null && System.IO.File.Exists(path))
		                {
		                    #if DEBUG
		                    try { Console.WriteLine("[CoreBridge] ManagedPreloadAssemblyContext: using fallback PreloadAssemblyContext(byte[])"); } catch { }
		                    #endif
		                    byte[] bytes = System.IO.File.ReadAllBytes(path);
		                    var rcObj = miBytes.Invoke(null, new object?[] { bytes, null });
		                    return rcObj is int i ? i : -1;
		                }
		
		                #if DEBUG
		                try { Console.WriteLine("[CoreBridge] ManagedPreloadAssemblyContext: no suitable preload method found"); } catch { }
		                #endif
		                return -3;
		            }
		            catch (Exception ex)
		            {
#if !DEBUG
		                _ = ex;
#endif
		                #if DEBUG
		                try { Console.WriteLine($"[CoreBridge] ManagedPreloadAssemblyContext: exception {ex.GetType().Name}: {ex.Message}"); } catch { }
		                #endif
		                return -1;
		            }
		        }

                internal static int ManagedSwapPreloadedContext()
                {
                    try
                    {

                        bool ok = EnsureHotReloadTypeLoaded();
                        if (!ok)
                        {
                            try { DebugWrite("ManagedSwapPreloadedContext: HRM type not loaded"); } catch { }
                            return -3;
                        }
                        // Ensure HRM is initialized before swap (with fallback resolution)
                        if (!s_hrmInitialized)
                        {
                            try
                            {
                                if (s_miInitialize != null)
                                {
                                    var _ = s_miInitialize.Invoke(null, null);
                                }
                                else
                                {
                                    var t = s_hotReloadType;
                                    var miInit = t?.GetMethod("Initialize", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
                                    if (miInit != null)
                                    {
                                        s_miInitialize = miInit;
                                        var _ = miInit.Invoke(null, null);
                                    }
                                }
                                s_hrmInitialized = true;
                            }
                            catch { }
                        }
                        if (s_dSwapPreloadedContext != null)
                        {
                            try { DebugWrite("ManagedSwapPreloadedContext: using delegate"); } catch { }
                            return s_dSwapPreloadedContext();
                        }
                        var mi = s_miSwapPreloadedContext;
                        if (mi == null)
                        {
                            // Fallback: try to resolve the managed alias and cache
                            mi = s_hotReloadType != null ? GameEngine.CoreBridge.SwapAliasShim.ResolveSwapAlias(s_hotReloadType) : null;
                            try { DebugWrite($"ManagedSwapPreloadedContext: resolved swap='{(mi?.Name ?? "<null>")}'"); } catch { }
                            if (mi != null) s_miSwapPreloadedContext = mi;
                        }
                        if (mi == null)
                        {
                            try { DebugWrite("ManagedSwapPreloadedContext: swap method unresolved"); } catch { }
                            return -3;
                        }
                        try { DebugWrite($"ManagedSwapPreloadedContext: invoking '{mi?.Name}' on '{mi?.DeclaringType?.FullName}'"); } catch { }
                        int rc = HotReloadApi.InvokeNoArgs(mi!);
                        try { DebugWrite($"ManagedSwapPreloadedContext: invoke rc={rc}"); } catch { }
                        return rc;
                    }
                    catch (Exception ex) { try { DebugWrite($"ManagedSwapPreloadedContext: exception {ex.GetType().Name}: {ex.Message}"); } catch { } return -1; }
                }

                internal static int ManagedCallUserScriptsMethod(string methodName)
                {
                    try
                    {
                        if (!EnsureHotReloadTypeLoaded()) return -3;
                        // Prefer direct facade if available
                        if (s_miCall != null)
                        {
                            var retObj = s_miCall.Invoke(null, new object?[] { methodName });
                            int ret = retObj is int i ? i : -1;
                            // If facade did not find the method, fall back to token path in current domain
                            if (ret >= 0 || (ret != -3 && ret != -4)) return ret;
                        }

                        // Token-path fallback: QueryExportInDomain + InvokeByToken in current domain (0 resolves to current)
                        if ((s_dQueryExportInDomain != null || s_miQueryExportInDomain != null) && (s_dInvokeByToken != null || s_miInvokeByToken != null))
                        {
                            ulong tok = 0UL;
                            int qrc = (s_dQueryExportInDomain != null)
                                ? s_dQueryExportInDomain(0, methodName, ref tok)
                                : HotReloadApi.QueryExportInDomain(s_miQueryExportInDomain!, 0, methodName, ref tok);
                            if (qrc == 0 && tok != 0UL)
                            {
                                int rc = (s_dInvokeByToken != null)
                                    ? s_dInvokeByToken(0, tok)
                                    : HotReloadApi.InvokeByToken(s_miInvokeByToken!, 0, tok);
                                if (rc >= 0) return rc;
                                return -1; // invocation failed
                            }
                            return -3; // not found
                        }

                        return -3;
                    }
                    catch { return -1; }
                }



                /// <summary>Swap the preloaded context into active use. Returns 0 on success.</summary>

				[GenerateBinding(GE_CB_SwapPreloadedContext)]

			[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
			public static int SwapPreloadedContext()
			{
				try
				{


					if (!EnsureHotReloadTypeLoaded()) return -3;
					// Ensure HRM is initialized prior to swap (UCO path) with fallback resolution
					if (!s_hrmInitialized)
						{
							try
							{
								if (s_miInitialize != null)
								{
									var _ = s_miInitialize.Invoke(null, null);
								}
								else
								{
									var t = s_hotReloadType;
									var miInit = t?.GetMethod("Initialize", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
									if (miInit != null)
									{
										s_miInitialize = miInit;
										var _ = miInit.Invoke(null, null);
									}
								}
								s_hrmInitialized = true;
							}
							catch { }
						}

					if (s_dSwapPreloadedContext != null)
					{
						try { DebugWrite("CoreBridge.SwapPreloadedContext: using delegate"); } catch { }

						return s_dSwapPreloadedContext();
					}
					var mi = s_miSwapPreloadedContext;
					if (mi == null)
					{
						// Prefer managed helper which handles alias resolution and diagnostics
						try { DebugWrite("CoreBridge.SwapPreloadedContext: using ManagedSwapPreloadedContext"); } catch { }
						return ManagedSwapPreloadedContext();
					}
					try { DebugWrite($"CoreBridge.SwapPreloadedContext: invoking via MethodInfo '{mi?.Name}' on '{mi?.DeclaringType?.FullName}'"); } catch { }
					int rc = HotReloadApi.InvokeNoArgs(mi!);
					return rc;
				}
				catch { return -1; }
			}

			/// <summary>
			/// Cleanup an old context given a package path; used post-swap. Returns 0 on success.
			/// </summary>
				[GenerateBinding(GE_CB_CleanupOldContext)]

			[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
			public static int CleanupOldContext(byte* pathUtf8, uint len)
			{
				try
				{
					if (!EnsureHotReloadTypeLoaded()) return -3;
					string path = (pathUtf8 == null || len == 0) ? string.Empty : System.Text.Encoding.UTF8.GetString(pathUtf8, (int)len);
					if (s_dCleanupOldContext != null) return s_dCleanupOldContext(path);
					var mi = s_miCleanupOldContext;
					if (mi == null) return -3;
					int rc = HotReloadApi.InvokeStringArg(mi, path);
					return rc;
				}


				catch { return -1; }
			}

			/// <summary>
			/// Legacy compiler-cache hook. The in-process incremental compiler was removed
			/// (CompileServerHost owns compilation); the callback id stays registered for ABI
			/// stability and reports the capability as absent.
			/// </summary>
				[GenerateBinding(GE_CB_ClearCompilerCache)]

			[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
			public static int ClearCompilerCache()
			{
				return -3; // GE_Result_NotFound: capability removed
			}

			/// <summary>
			/// Legacy compiler-stats hook. See <see cref="ClearCompilerCache"/> - the in-process
			/// compiler was removed; reports the capability as absent.
			/// </summary>
				[GenerateBinding(GE_CB_GetCompilerStats)]

			[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
			public static int GetCompilerStats()
			{
				return -3; // GE_Result_NotFound: capability removed
			}


        // Configure HotReload directory explicitly from native (avoids environment-variable reliance)
        /// <summary>
        /// Sets the directory where GameEngine.HotReload.dll must be resolved from. Preferred over env vars.
        /// Called by the native host during initialization. Returns 0 on success.
        /// </summary>

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int SetHotReloadDirectory(byte* dirUtf8, uint len)
        {
            try
            {
                string dir = (dirUtf8 == null || len == 0) ? string.Empty : System.Text.Encoding.UTF8.GetString(dirUtf8, (int)len);
                s_hrmDirOverride = dir;
                InvalidateHrmBindings();
                if (s_verbose) { try { DebugWrite($"SetHotReloadDirectory override='{dir}'"); } catch { } }
                return 0;
            }
            catch { return -1; }
        }

        /// <summary>
        /// The native host's switches (GE_HostSwitch_* in ScriptingABI.h), delivered by
        /// CoreCLRHost::SetHostSwitches whenever they change, from the thread that makes the
        /// managed calls they govern and between those calls (the word is a plain static). A
        /// change to a delegate switch rebinds HotReloadManager's entry points on their next
        /// use. Returns 0.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int SetHostSwitches(uint switches)
        {
            var next = (HostSwitches)switches;
            var changed = s_hostSwitches ^ next;
            s_hostSwitches = next;
            if ((changed & HostSwitches.DisableHrmDelegates) != 0)
                InvalidateHrmBindings();
            return 0;
        }

    /// <summary>
    /// Get minimal HotReload metrics. Receives values via pointers; returns 0 on success.
    /// </summary>
    [GenerateBinding(GE_CB_GetHotReloadMetrics)]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int GetHotReloadMetrics(ulong* lastCompileMs, ulong* lastSwapMs, int* totalCompiles, int* totalSwaps)
    {
        try
        {
            if (!EnsureHotReloadTypeLoaded()) return -3;
            var t = s_hotReloadType;
            if (t == null) return -3;
            var mi = t.GetMethod("GetMetrics", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
            if (mi == null) return -3;
            object?[] args = new object?[] { 0L, 0L, 0, 0 };
            var ret = mi.Invoke(null, args);
            int rc = ret is int i ? i : 0;
            if (rc != 0) return rc;
            if (lastCompileMs != null) *lastCompileMs = (ulong)(args[0] is long a0 ? a0 : 0L);
            if (lastSwapMs != null) *lastSwapMs = (ulong)(args[1] is long a1 ? a1 : 0L);
            if (totalCompiles != null) *totalCompiles = (int)(args[2] is int a2 ? a2 : 0);
            if (totalSwaps != null) *totalSwaps = (int)(args[3] is int a3 ? a3 : 0);
            return 0;
        }
        catch { return -1; }
    }

    /// <summary>
    /// Get extended HotReload metrics including unloads. Returns 0 on success.
    /// </summary>
    [GenerateBinding(GE_CB_GetHotReloadMetricsEx)]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int GetHotReloadMetricsEx(ulong* lastCompileMs, ulong* lastSwapMs, int* totalCompiles, int* totalSwaps, int* totalUnloads)
    {


        try
        {
            if (!EnsureHotReloadTypeLoaded()) return -3;
            var t = s_hotReloadType;
            if (t == null) return -3;
            var mi = t.GetMethod("GetMetricsEx", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
            if (mi == null) return -3;
            object?[] args = new object?[] { 0L, 0L, 0, 0, 0 };
            var ret = mi.Invoke(null, args);
            int rc = ret is int i ? i : 0;
            if (rc != 0) return rc;
            if (lastCompileMs != null) *lastCompileMs = (ulong)(args[0] is long a0 ? a0 : 0L);
            if (lastSwapMs != null) *lastSwapMs = (ulong)(args[1] is long a1 ? a1 : 0L);
            if (totalCompiles != null) *totalCompiles = (int)(args[2] is int a2 ? a2 : 0);
            if (totalSwaps != null) *totalSwaps = (int)(args[3] is int a3 ? a3 : 0);
            if (totalUnloads != null) *totalUnloads = (int)(args[4] is int a4 ? a4 : 0);
            return 0;
        }
        catch { return -1; }
    }

    /// <summary>Reset minimal HotReload metrics counters to zero. Returns 0 on success.</summary>
    [GenerateBinding(GE_CB_ResetHotReloadMetrics)]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int ResetHotReloadMetrics()
    {
        try
        {
            if (!EnsureHotReloadTypeLoaded()) return -3;
            var t = s_hotReloadType;
            if (t == null) return -3;
            var mi = t.GetMethod("ResetMetrics", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
            if (mi == null) return -3;
            var ret = mi.Invoke(null, null);
            return ret is int i ? i : 0;
        }
        catch { return -1; }
    }



#if DEBUG
        /// <summary>
        /// Debug-only: return delegate vs reflection hit counts as four int32 values.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int GetPerfCounters(int* outDelegateQuery, int* outReflectionQuery, int* outDelegateInvoke, int* outReflectionInvoke)
        {
            try
            {
                if (outDelegateQuery == null || outReflectionQuery == null || outDelegateInvoke == null || outReflectionInvoke == null)
                    return -2;
                int dq = (int)System.Threading.Interlocked.Read(ref s_dbgDelegateQueryExport);
                int rq = (int)System.Threading.Interlocked.Read(ref s_dbgReflectionQueryExport);
                int di = (int)System.Threading.Interlocked.Read(ref s_dbgDelegateInvokeByToken);


                int ri = (int)System.Threading.Interlocked.Read(ref s_dbgReflectionInvokeByToken);
                *outDelegateQuery = dq;


                *outReflectionQuery = rq;
                *outDelegateInvoke = di;
                *outReflectionInvoke = ri;
                return 0;
            }
            catch { return -1; }
        }

        /// <summary>
        /// Debug-only: return counters packed into a 64-bit value to avoid pointer marshalling.
        /// Layout: [63..48]=ri, [47..32]=di, [31..16]=rq, [15..0]=dq (each clamped to 16 bits).
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static long GetPerfCountersPacked()
        {
            try
            {
#if DEBUG
                long dq = System.Threading.Interlocked.Read(ref s_dbgDelegateQueryExport);
                long rq = System.Threading.Interlocked.Read(ref s_dbgReflectionQueryExport);
                long di = System.Threading.Interlocked.Read(ref s_dbgDelegateInvokeByToken);
                long ri = System.Threading.Interlocked.Read(ref s_dbgReflectionInvokeByToken);
                dq = dq < 0 ? 0 : dq; rq = rq < 0 ? 0 : rq; di = di < 0 ? 0 : di; ri = ri < 0 ? 0 : ri;
                dq &= 0xFFFF; rq &= 0xFFFF; di &= 0xFFFF; ri &= 0xFFFF;
                long packed = (ri << 48) | (di << 32) | (rq << 16) | dq;
                return packed;
#else
                return -1;
#endif
            }
            catch { return -1; }
        }

        /// <summary>
        /// Debug-only: artificially bump delegate counters to validate the test path without invoking fragile interop.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int Debug_TickleCounters()
        {
            try
            {
#if DEBUG
                System.Threading.Interlocked.Increment(ref s_dbgDelegateQueryExport);
                System.Threading.Interlocked.Increment(ref s_dbgDelegateInvokeByToken);
                return 0;
#else
                return -1;
#endif
            }
            catch { return -1; }
        }


        /// <summary>
        /// Debug-only: reset perf counters to zero.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int ResetPerfCounters()
        {
            try
            {

                System.Threading.Interlocked.Exchange(ref s_dbgDelegateQueryExport, 0);
                System.Threading.Interlocked.Exchange(ref s_dbgReflectionQueryExport, 0);
                System.Threading.Interlocked.Exchange(ref s_dbgDelegateInvokeByToken, 0);
                System.Threading.Interlocked.Exchange(ref s_dbgReflectionInvokeByToken, 0);
                return 0;
            }
            catch { return -1; }
        }

#endif

        /// <summary>
        /// Debug-only: return current active path for InvokeByToken: 0=Unknown, 1=Delegate, 2=Reflection.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int GetInvokeByTokenPath()
        {
            try
            {
                if (!EnsureHotReloadTypeLoaded()) return -3;
                if (s_dInvokeByToken != null) return 1;
                if (s_miInvokeByToken != null) return 2;
                return 0;
            }
            catch { return -1; }
        }


        /// <summary>
        /// Debug: returns a bitmask of current call-path bindings.
        /// Bits: 0x1=Typed Query, 0x2=Typed Invoke, 0x4=Wrapper Query, 0x8=Wrapper Invoke, 0x10=Reflection Query, 0x20=Reflection Invoke
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int Debug_GetPathMask()
        {
            try
            {
                if (!EnsureHotReloadTypeLoaded()) return -3;
                int mask = 0;
                if (s_dQueryExportInDomain != null && !s_isWrapperQuery) mask |= 0x01;
                if (s_dInvokeByToken != null && !s_isWrapperInvoke) mask |= 0x02;
                if (s_dQueryExportInDomain != null && s_isWrapperQuery) mask |= 0x04;
                if (s_dInvokeByToken != null && s_isWrapperInvoke) mask |= 0x08;
                if (s_dQueryExportInDomain == null && s_miQueryExportInDomain != null) mask |= 0x10;
                if (s_dInvokeByToken == null && s_miInvokeByToken != null) mask |= 0x20;
                return mask;
            }
            catch { return -1; }
        }


        /// <summary>
        /// Debug-only helper: attempts to bind CoreBridge's managed fast-path delegates from stored MethodInfos.
        /// Returns a bitmask: 0x1=QueryExport delegate bound, 0x2=InvokeByToken delegate bound.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int Debug_ForceRebindDelegates()
        {
            try
            {
                if (HrmDelegatesDisabled) return -1;
                if (!EnsureHotReloadTypeLoaded()) return -3;
                try { if (s_miQueryExportInDomain != null) { s_dQueryExportInDomain = s_miQueryExportInDomain.CreateDelegate<DQueryExportInDomain>(); s_isWrapperQuery = false; } } catch { }
                try { if (s_miInvokeByToken != null) { s_dInvokeByToken = s_miInvokeByToken.CreateDelegate<DInvokeByToken>(); s_isWrapperInvoke = false; } } catch { }
                int mask = 0;
                if (s_dQueryExportInDomain != null) mask |= 1;
                if (s_dInvokeByToken != null) mask |= 2;
                return mask;
            }
            catch { return -1; }
        }


#if DEBUG
        /// <summary>
        /// Debug-only: get unload counter.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int GetUnloadCounter()
        {
            try { return (int)System.Threading.Interlocked.Read(ref s_dbgUnloadDomain); } catch { return -1; }
        }


        /// <summary>
        /// Debug-only: perform InvokeByToken entirely inside managed, without an out pointer across the UCO boundary.
        /// Returns 0 on success; negative on failure. Increments delegate/reflection invoke counters accordingly.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int Debug_InvokeByToken_NoOut(ulong domain, ulong token)
        {
            try
            {
                try { EnsureManagedCallbacksRegistered(); } catch { }
                if (!EnsureHotReloadTypeLoaded()) return -3;
                if (s_miInvokeByToken == null) return -3;
                if (s_dInvokeByToken != null)
                {
                    int rc = s_dInvokeByToken(domain, token);
    #if DEBUG
                    System.Threading.Interlocked.Increment(ref s_dbgDelegateInvokeByToken);
    #endif
                    return rc >= 0 ? 0 : -1;
                }
                var ret = s_miInvokeByToken.Invoke(null, new object?[] { domain, token });
                int rc2 = ret is int i ? i : -1;
    #if DEBUG
                System.Threading.Interlocked.Increment(ref s_dbgReflectionInvokeByToken);
    #endif
                return rc2 >= 0 ? 0 : -1;
            }
            catch { return -1; }
        }


        /// <summary>
        /// Debug-only: perform QueryExport+InvokeByToken entirely inside managed, using a UTF-8 name.
        /// No out-parameters cross the UCO boundary; returns 0 on success, negative on failure.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe int Debug_InvokeByName_NoOut(ulong domain, byte* nameUtf8, uint nameLen)
        {
            try
            {
                if (nameUtf8 == null || nameLen == 0) return -2;
                try { EnsureManagedCallbacksRegistered(); } catch { }
                if (!EnsureHotReloadTypeLoaded()) return -3;
                var nameSpan = new ReadOnlySpan<byte>(nameUtf8, (int)nameLen);
                string name = System.Text.Encoding.UTF8.GetString(nameSpan);
                if (string.IsNullOrEmpty(name)) return -2;
                if (s_verbose) try { Console.WriteLine($"[CoreBridge] Debug_InvokeByName_NoOut: name='{name}' len={nameLen}"); } catch {}

                ulong tok = 0UL;
                int qrc;
                ulong dom2 = domain;
                if (s_miQueryExportInDomain == null || s_miInvokeByToken == null) return -3;

                try
                {
                    if (dom2 == 0 && s_hotReloadType != null)
                    {
                        var f = s_hotReloadType.GetField("m_CurrentDomainId", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static);
                        var v = f?.GetValue(null);
                        if (v is ulong u && u != 0UL) dom2 = u;
                    }
                }
                catch { }


                if (s_dQueryExportInDomain == null) BindQueryExportDelegate();
                if (s_dInvokeByToken == null) BindInvokeByTokenDelegate();

                if (s_dQueryExportInDomain != null) qrc = s_dQueryExportInDomain(dom2, name, ref tok);
                else if (s_miQueryExportInDomain != null) qrc = HotReloadApi.QueryExportInDomain(s_miQueryExportInDomain, dom2, name, ref tok);
                else qrc = -3;

                if (qrc != 0 && dom2 == 0 && domain == 0)
                {
                    try
                    {
                        var t = s_hotReloadType;
                        var f2 = t?.GetField("m_CurrentDomainId", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static);
                        var v2 = f2?.GetValue(null);
                        if (v2 is ulong u2 && u2 != 0UL) dom2 = u2;
                    }
                    catch { dom2 = 0; }
                    if (dom2 != 0)
                    {
                        if (s_dQueryExportInDomain != null) qrc = s_dQueryExportInDomain(dom2, name, ref tok);
                        else qrc = HotReloadApi.QueryExportInDomain(s_miQueryExportInDomain!, dom2, name, ref tok);
                    }
                }

                if (s_verbose) try { Console.WriteLine($"[CoreBridge] Debug_InvokeByName_NoOut: qrc={qrc} tok={tok} dom2={dom2}"); } catch {}

                if (qrc == 0 && tok != 0UL)
                {
                    int rc = (s_dInvokeByToken != null) ? s_dInvokeByToken(dom2, tok) : HotReloadApi.InvokeByToken(s_miInvokeByToken!, dom2, tok);
#if DEBUG
                    if (s_dQueryExportInDomain != null) System.Threading.Interlocked.Increment(ref s_dbgDelegateQueryExport);
                    else System.Threading.Interlocked.Increment(ref s_dbgReflectionQueryExport);
                    if (s_dInvokeByToken != null) System.Threading.Interlocked.Increment(ref s_dbgDelegateInvokeByToken);
                    else System.Threading.Interlocked.Increment(ref s_dbgReflectionInvokeByToken);
#endif
                    return rc >= 0 ? 0 : -1;
                }

                return -1;
            }
            catch { return -1; }
        }

        /// <summary>
        /// Debug-only: reset unload counter to zero.
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int ResetUnloadCounter()
        {
            try { System.Threading.Interlocked.Exchange(ref s_dbgUnloadDomain, 0); return 0; } catch { return -1; }
        }
#endif

    // Editor-only: expose first-invoke timing metric after a swap/load
    /// <summary>
    /// Editor-only: get first-invoke timing metric after last swap/load. Returns 0 on success; optional binding.
    /// </summary>

    [GenerateBinding(18)]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int GetHotReloadEditorMetrics(ulong* lastFirstInvokeMs)
    {
        try
        {
            if (!EnsureHotReloadTypeLoaded()) return -3;
            var t = s_hotReloadType;
            if (t == null) return -3;
            var mi = t.GetMethod("GetEditorMetrics", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
            if (mi == null) return -3;
            object?[] args = new object?[] { 0L };
            var ret = mi.Invoke(null, args);

            int rc = ret is int i ? i : 0;
            if (rc != 0) return rc;
            if (lastFirstInvokeMs != null) *lastFirstInvokeMs = (ulong)(args[0] is long a0 ? a0 : 0L);
            return 0;
        }
        catch { return -1; }
    }




    /// <summary>
    /// Editor-only: get packed metrics with [63..32]=lastSwapMs and [31..0]=lastFirstInvokeMs.
    /// Returns 0 if HotReloadManager or the method is not available.
    /// </summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static ulong GetEditorMetricsPacked64()
    {
        try
        {
            if (!EnsureHotReloadTypeLoaded()) return 0UL;
            var t = s_hotReloadType;
            if (t == null) return 0UL;
            var mi = t.GetMethod("GetEditorMetricsPacked64", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
            if (mi == null) return 0UL;
            var ret = mi.Invoke(null, null);
            if (ret is long l) return unchecked((ulong)l);
            if (ret is ulong ul) return ul;
            return 0UL;
        }
        catch { return 0UL; }
    }

}



