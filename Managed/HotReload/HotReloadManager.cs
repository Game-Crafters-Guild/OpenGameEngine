using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Threading;
using System.Threading.Tasks;
using System.Collections.Generic;
using System.Linq;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;

namespace GameEngine.HotReload
{
    /// <summary>
    /// Core hot-reload manager using collectible AssemblyLoadContext
    /// This class is in a separate core assembly that is NEVER unloaded,
    /// ensuring safe and reliable hot-reload operations for user scripts.
    ///
    /// Based on Strathweb approach: https://www.strathweb.com/2019/01/collectible-assemblies-in-net-core-3-0/
    /// and GitHub issue: https://github.com/dotnet/runtime/issues/85862
    /// </summary>
    public static class HotReloadManager
    {
        private static CollectibleAssemblyContext? m_CurrentContext;
        private static Assembly? m_CurrentAssembly;

        private static readonly object m_Lock = new object();
        private static bool m_IsInitialized = false;
        private static string m_LastLoadedAssemblyPath = "";
        private static string? m_LastUserScriptsPath = null;

        // Domain identification (stable across native boundary)
        private static ulong m_CurrentDomainId = 0; // expose for CoreBridge reflection
        private static ulong m_NextDomainId = 1;
        public static ulong GetCurrentDomainId() => m_CurrentDomainId;


        // Multi-domain support
        private static readonly Dictionary<ulong, (CollectibleAssemblyContext Ctx, Assembly Asm)> m_Domains = new();

        // Token API maps (per-domain)
        private static readonly Dictionary<ulong, Dictionary<string, ulong>> m_NameToTokenByDomain = new();
        private static readonly Dictionary<ulong, Dictionary<ulong, Func<int>>> m_TokenToMethodByDomain = new();
        private static ulong m_NextToken = 1;
        // Fast lookup indices of exports per domain (built at load/swap)
        private static readonly Dictionary<ulong, Dictionary<string, Func<int>>> m_MethodIndexFullByDomain = new();
        private static readonly Dictionary<ulong, Dictionary<string, Func<int>>> m_MethodIndexSimpleByDomain = new();


        // OPTIMIZATION: Pre-loaded assembly bytes for instant swapping
        private static byte[]? m_PreloadedAssemblyBytes = null;
        private static string m_PreloadedAssemblyPath = "";

	        // Two-phase optimization: Preload context and assembly on background thread

	        // Deduplication: track invoked InitializeOnLoad methods per (MVID, IoL generation) to avoid double
	        // invocations during a single swap while still allowing future swaps/hot-reloads to re-run IoL.
	        private static readonly Dictionary<(Guid Mvid, long Generation), HashSet<int>> m_InvokedInitOnLoadByMvid = new();

			// Optional file-based trace for IoL debugging (enabled via GE_HOTRELOAD_IOL_TRACE_FILE).
			// This is intentionally very lightweight and only active when the env var is set; it bypasses
			// Console redirection so we can debug IoL behavior even if EngineLogWriter is not installed.
			private static readonly string? s_iolTracePath = Environment.GetEnvironmentVariable("GE_HOTRELOAD_IOL_TRACE_FILE");
			#if DEBUG
			private static int s_iolTraceInitLogged;
			#endif
			static HotReloadManager()
			{
			    // One-time diagnostic to ensure we understand the environment inside the managed runtime.
			    try
			    {
			        string env = s_iolTracePath ?? "<null>";
			        Console.WriteLine($"[HotReload] GE_HOTRELOAD_IOL_TRACE_FILE at HotReloadManager load: '{env}'");
			    }
			    catch { }
			}
			private static void IoLTrace(string message)
			{
			    if (string.IsNullOrEmpty(s_iolTracePath)) return;
			    try
			    {
			        // Log the resolved path once in DEBUG builds to help diagnose environment issues.
			        #if DEBUG
			        if (System.Threading.Interlocked.Exchange(ref s_iolTraceInitLogged, 1) == 0)
			        {
			            try
			            {
			                Console.WriteLine($"[HotReload] IoLTrace enabled path='{s_iolTracePath}'");
			            }
			            catch { }
			        }
			        #endif
			
			        string path = s_iolTracePath!;
			        string? dir = Path.GetDirectoryName(path);
			        if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
			        {
			            Directory.CreateDirectory(dir);
			        }
			
			        string line = DateTime.Now.ToString("HH:mm:ss.fff ") + message + Environment.NewLine;
			        File.AppendAllText(path, line);
			    }
			    catch (Exception ex)
			    {
#if !DEBUG
			        _ = ex;
#endif
			        #if DEBUG
			        try
			        {
			            Console.WriteLine($"[HotReload] IoLTrace failed path='{s_iolTracePath}': {ex.GetType().Name}: {ex.Message}");
			        }
			        catch { }
			        #endif
			    }
			}


        // Internal carrier for Stage 3 -> Stage 4 handoff
        private sealed class PreloadResult
        {
            public CollectibleAssemblyContext Context;
            public Assembly Assembly;
            public string? ContextPath;
            public Dictionary<string, Func<int>> FullIndex;
            public Dictionary<string, Func<int>> SimpleIndex;
            public List<System.Reflection.MethodInfo> InitOnLoadMethods;
            public PreloadResult(CollectibleAssemblyContext ctx, Assembly asm, string? path,
                                 (Dictionary<string, Func<int>> Full, Dictionary<string, Func<int>> Simple) indices,
                                 List<System.Reflection.MethodInfo> init)
            {
                Context = ctx;
                Assembly = asm;
                ContextPath = path;
                FullIndex = indices.Full;
                SimpleIndex = indices.Simple;
                InitOnLoadMethods = init;
            }
        }

        // Single field to hold the prepared context between Preload and Swap
        private static PreloadResult? m_Preloaded;

        // Gating for InitializeOnLoad invocation tasks to avoid duplicate logs/runs from overlapping swaps
        private static long m_IoLEnqueueGen = 0;            // monotonically increasing id for IoL tasks
        private static long m_LatestScheduledIoLGen = 0;    // last scheduled generation id


        // Ensure only one swap runs at a time to avoid duplicate IoL scheduling under overlap
        private static int m_SwapActive = 0;

        // Stage 4 completion signaling for InitializeOnLoad
        [ThreadStatic]
        private static long sCurrentIoLGen;

        // Awaitable runtime hook: waiters per IoL generation
        private static readonly System.Collections.Concurrent.ConcurrentDictionary<long, System.Threading.Tasks.TaskCompletionSource<bool>> s_iolWaiters = new System.Collections.Concurrent.ConcurrentDictionary<long, System.Threading.Tasks.TaskCompletionSource<bool>>();
        // Track the most recently completed IoL generation to avoid waiter races
        private static long m_LatestCompletedIoLGen = 0;


        // Event fired after InitializeOnLoad completes for a generation: (generation, invokedCount, skippedCount)
        public static event Action<long, int, int>? InitializeOnLoadCompleted;

        // -------------------------------------------------------------------
        // Engine main-thread marshaling (B5): when the native host registers a
        // pump (CoreBridge.SetMainThreadPumpCallback), [InitializeOnLoad] work is
        // queued here and drained on the engine main thread (ScriptManager's
        // per-frame ProcessMainThreadTasks) instead of running on a thread-pool
        // thread concurrent with rendering. Without a registered pump (standalone
        // dotnet-test hosting) the work falls back to Task.Run as before.
        //
        // Ordering guarantees with a pump registered:
        //  - IoL for swap generation N runs on the engine main thread, after
        //    SwapPreloadedContext(N) returns, at the next main-thread pump.
        //  - Generations drain FIFO; IoL(N) completes before IoL(N+1) starts.
        //  - WaitForInitializeOnLoadAsync semantics are unchanged (never await it
        //    ON the main thread while a pump is registered — the work needs that
        //    thread to run).
        private static readonly System.Collections.Concurrent.ConcurrentQueue<Action> s_mainThreadWork = new();
        private static Action? s_requestMainThreadPump;

        /// <summary>
        /// Registers (or clears, with null) the native main-thread pump request. The
        /// callback must be safe to invoke from any thread; it should schedule a call
        /// to <see cref="DrainMainThreadWork"/> on the engine main thread. Invoked by
        /// CoreBridge via reflection.
        /// </summary>
        public static int SetMainThreadPump(Action? requestPump)
        {
            System.Threading.Volatile.Write(ref s_requestMainThreadPump, requestPump);
            Info($"[HotReload] Main-thread pump {(requestPump != null ? "registered" : "cleared")}");
            return 0;
        }

        /// <summary>
        /// Drains queued main-thread work (IoL invocations). Called on the engine main
        /// thread by CoreBridge.PumpHotReloadMainThread. Returns the number of items run.
        /// </summary>
        public static int DrainMainThreadWork()
        {
            int ran = 0;
            while (s_mainThreadWork.TryDequeue(out var work))
            {
                try { work(); } catch (Exception ex) { Console.Error.WriteLine($"[HotReload] Main-thread work threw: {ex.Message}"); }
                ran++;
            }
            return ran;
        }

        // A host can register the pump and then never drain it (no live frame loop,
        // e.g. gtest hosts that initialize the engine without running Update). The
        // watchdog drains on the thread pool after this delay so IoL can never be
        // stranded; in the editor the main thread drains well before it fires.
        private const int kMainThreadDrainFallbackMs = 3000;

        /// <summary>
        /// Queues work for the engine main thread when a pump is registered; returns
        /// false when the caller must fall back to thread-pool execution.
        /// </summary>
        private static bool TryDispatchToMainThread(Action work)
        {
            var requestPump = System.Threading.Volatile.Read(ref s_requestMainThreadPump);
            if (requestPump == null)
                return false;
            s_mainThreadWork.Enqueue(work);
            try
            {
                requestPump();
                _ = System.Threading.Tasks.Task.Delay(kMainThreadDrainFallbackMs).ContinueWith(static _ =>
                {
                    if (!s_mainThreadWork.IsEmpty)
                    {
                        // By-design fallback (hosts without a frame loop never pump);
                        // informational, not an error — stdout, never stderr.
                        Console.WriteLine(
                            $"[HotReload] Main-thread pump not drained within {kMainThreadDrainFallbackMs}ms (host without a frame loop); draining on thread pool");
                        DrainMainThreadWork();
                    }
                });
                return true;
            }
            catch (Exception ex)
            {
                // A pump request can fail before the host side is fully wired
                // (startup install order); the thread-pool drain below IS the
                // designed fallback, so report it as information, not an error.
                Console.WriteLine(
                    $"[HotReload] Main-thread pump unavailable ({ex.GetType().Name}); draining on thread pool");
                _ = System.Threading.Tasks.Task.Run(static () => DrainMainThreadWork());
                return true;
            }
        }

        // (breadcrumb flags removed)

        // Minimal metrics (guarded by usage; no logging here)
        private static long m_LastCompileMs = 0;
        private static long m_LastSwapMs = 0;
        private static int m_TotalCompiles = 0;
        private static int m_TotalSwaps = 0;
        private static int m_TotalUnloads = 0;
        // ALC leak tracking: incremented when unload verification exhausts its budget (B1)
        private static int m_LeakedAlcCount = 0;
        private static long m_LastLeakedDomainId = 0;
#if DEBUG
        private static long m_LastSwapCriticalSectionUs = 0;
#endif

        public readonly struct HotReloadStatus
        {
            public string? CurrentAssemblyName { get; init; }
            public string? LastLoadedAssemblyPath { get; init; }
            public long LastCompileMs { get; init; }
            public long LastSwapMs { get; init; }
            public long LastSwapCriticalSectionUs { get; init; }
            public int TotalCompiles { get; init; }
            public int TotalSwaps { get; init; }
            public int TotalUnloads { get; init; }
            public int LeakedAlcCount { get; init; }
            public ulong LastLeakedDomainId { get; init; }
            public long LatestScheduledIoLGen { get; init; }
            public long LatestCompletedIoLGen { get; init; }
        }

        public static HotReloadStatus GetStatus()
        {
            long csUs = 0;
#if DEBUG
            csUs = System.Threading.Interlocked.Read(ref m_LastSwapCriticalSectionUs);
#endif
            return new HotReloadStatus
            {
                CurrentAssemblyName = m_CurrentAssembly?.FullName,
                LastLoadedAssemblyPath = m_LastLoadedAssemblyPath,
                LastCompileMs = System.Threading.Interlocked.Read(ref m_LastCompileMs),
                LastSwapMs = System.Threading.Interlocked.Read(ref m_LastSwapMs),
                LastSwapCriticalSectionUs = csUs,
                TotalCompiles = System.Threading.Volatile.Read(ref m_TotalCompiles),
                TotalSwaps = System.Threading.Volatile.Read(ref m_TotalSwaps),
                TotalUnloads = System.Threading.Volatile.Read(ref m_TotalUnloads),
                LeakedAlcCount = System.Threading.Volatile.Read(ref m_LeakedAlcCount),
                LastLeakedDomainId = unchecked((ulong)System.Threading.Interlocked.Read(ref m_LastLeakedDomainId)),
                LatestScheduledIoLGen = System.Threading.Volatile.Read(ref m_LatestScheduledIoLGen),
                LatestCompletedIoLGen = System.Threading.Volatile.Read(ref m_LatestCompletedIoLGen),
            };
        }

        /// <summary>
        /// Returns the set of currently active user assemblies (one per domain).
        /// Useful for tooling that needs to avoid stale collectible ALCs pending GC.
        /// </summary>
        public static Assembly[] GetActiveUserAssemblies()
        {
            lock (m_Lock)
            {
                var list = new List<Assembly>(m_Domains.Count + 1);
                foreach (var (_, pair) in m_Domains)
                {
                    if (pair.Asm != null)
                        list.Add(pair.Asm);
                }
                if (m_CurrentAssembly != null && !list.Contains(m_CurrentAssembly))
                    list.Add(m_CurrentAssembly);
                return list.ToArray();
            }
        }

#if DEBUG
        public static Assembly? GetCurrentAssembly() => m_CurrentAssembly;
        public static Assembly? GetPreloadedAssemblyForTests() => m_Preloaded?.Assembly;
#endif

        /// <summary>
        /// Return minimal metrics for editor/debug surfacing. Returns 0 on success.
        /// </summary>
        public static int GetMetrics(out long lastCompileMs, out long lastSwapMs, out int totalCompiles, out int totalSwaps)
        {
            lastCompileMs = m_LastCompileMs;
            lastSwapMs = m_LastSwapMs;
            totalCompiles = m_TotalCompiles;
            totalSwaps = m_TotalSwaps;
            return 0;
        }

        /// <summary>
        /// Extended metrics including unloads. Returns 0 on success.
        /// (Signature is fixed: CoreBridge.GetHotReloadMetricsEx reflects it with five out-args.)
        /// </summary>
        public static int GetMetricsEx(out long lastCompileMs, out long lastSwapMs, out int totalCompiles, out int totalSwaps, out int totalUnloads)
        {
            lastCompileMs = m_LastCompileMs;
            lastSwapMs = m_LastSwapMs;
            totalCompiles = m_TotalCompiles;
            totalSwaps = m_TotalSwaps;
            totalUnloads = m_TotalUnloads;
            return 0;
        }

        /// <summary>
        /// ALC leak metrics: how many unloaded contexts survived their bounded GC verification,
        /// and the domain id of the most recent leak. Returns 0 on success.
        /// </summary>
        public static int GetLeakMetrics(out int leakedAlcCount, out ulong lastLeakedDomainId)
        {
            leakedAlcCount = System.Threading.Volatile.Read(ref m_LeakedAlcCount);
            lastLeakedDomainId = unchecked((ulong)System.Threading.Interlocked.Read(ref m_LastLeakedDomainId));
            return 0;
        }


        // Editor-only metrics (opt-in consumers): last first-invoke latency after a swap/load
        private static long m_LastFirstInvokeMs = 0;
        private static readonly HashSet<ulong> m_FirstInvokePending = new HashSet<ulong>();

#if DEBUG
        // Stage guard for InitializeOnLoad invocation (debug-only).
        [ThreadStatic]
        private static bool sIoLInvokeAllowed;
#endif

        public static int GetEditorMetrics(out long lastFirstInvokeMs)
        {
            lastFirstInvokeMs = System.Threading.Interlocked.Read(ref m_LastFirstInvokeMs);
            return 0;
        }
        /// <summary>
        /// Editor-only convenience: packed 64-bit value with [63..32]=lastSwapMs (clamped), [31..0]=lastFirstInvokeMs (clamped).
        /// </summary>
        public static long GetEditorMetricsPacked64()
        {
            try
            {
                long swap = System.Threading.Interlocked.Read(ref m_LastSwapMs);
                long first = System.Threading.Interlocked.Read(ref m_LastFirstInvokeMs);
                if (swap < 0) swap = 0; if (first < 0) first = 0;
                ulong hi = (ulong)Math.Min((ulong)uint.MaxValue, (ulong)swap);
                ulong lo = (ulong)Math.Min((ulong)uint.MaxValue, (ulong)first);
                ulong packed = (hi << 32) | lo;
                return unchecked((long)packed);
            }
            catch { return 0L; }
        }



        // Runtime hooks to await InitializeOnLoad completion without polling
        public static long GetLatestInitializeOnLoadGeneration()
        {
            return System.Threading.Volatile.Read(ref m_LatestScheduledIoLGen);
        }

        public static async System.Threading.Tasks.Task<bool> WaitForInitializeOnLoadAsync(long generation, int timeoutMs = 5000)
        {
            if (generation <= 0)
                return true; // nothing pending

            // If this generation already completed, return immediately
            if (generation <= System.Threading.Volatile.Read(ref m_LatestCompletedIoLGen))
                return true;

            // Get or create a waiter for this generation to avoid race with the scheduler
            var tcs = s_iolWaiters.GetOrAdd(generation, _ => new System.Threading.Tasks.TaskCompletionSource<bool>(System.Threading.Tasks.TaskCreationOptions.RunContinuationsAsynchronously));

            var completed = await System.Threading.Tasks.Task.WhenAny(tcs.Task, System.Threading.Tasks.Task.Delay(timeoutMs)).ConfigureAwait(false);
            return completed == tcs.Task && tcs.Task.IsCompleted && tcs.Task.Result;
        }

        public static System.Threading.Tasks.Task<bool> WaitForPendingInitializeOnLoadAsync(int timeoutMs = 5000)
        {
            var gen = System.Threading.Volatile.Read(ref m_LatestScheduledIoLGen);
            if (gen == 0) return System.Threading.Tasks.Task.FromResult(true);
            return WaitForInitializeOnLoadAsync(gen, timeoutMs);
        }
#if DEBUG
        private static void DebugLogConsoleState(string where)
        {
            if (!s_verbose) return;
            try
            {
                Console.WriteLine($"[HotReload] {where}: Console.Out={Console.Out?.GetType().FullName ?? "<null>"} Console.Error={Console.Error?.GetType().FullName ?? "<null>"}");
            }
            catch { }
        }
#endif



        /// <summary>Reset metrics counters to zero. Returns 0 on success.</summary>
        public static int ResetMetrics()
        {
            System.Threading.Interlocked.Exchange(ref m_LastCompileMs, 0);
            System.Threading.Interlocked.Exchange(ref m_LastSwapMs, 0);
            System.Threading.Interlocked.Exchange(ref m_TotalCompiles, 0);
            System.Threading.Interlocked.Exchange(ref m_TotalSwaps, 0);
            System.Threading.Interlocked.Exchange(ref m_TotalUnloads, 0);
            return 0;
        }

        private static readonly bool s_verbose = string.Equals(Environment.GetEnvironmentVariable("GE_HOTRELOAD_VERBOSE"), "1", StringComparison.Ordinal);
        private static void Info(string msg) { if (s_verbose) Console.WriteLine(msg); }
        // Reflection.Metadata fallback: detect InitializeOnLoad without loading attribute assembly
        private static bool HasInitializeOnLoadAttributeViaMetadata(string assemblyPath, string? typeFullName, string methodName)
        {
            try
            {
                if (string.IsNullOrEmpty(assemblyPath) || string.IsNullOrEmpty(typeFullName)) return false;
                using var fs = File.OpenRead(assemblyPath);
                using var pe = new PEReader(fs);
                var md = pe.GetMetadataReader();

                int lastDot = typeFullName.LastIndexOf('.');
                string ns = lastDot >= 0 ? typeFullName.Substring(0, lastDot) : string.Empty;
                string tn = lastDot >= 0 ? typeFullName.Substring(lastDot + 1) : typeFullName;

                foreach (var tDefHandle in md.TypeDefinitions)
                {
                    var tDef = md.GetTypeDefinition(tDefHandle);
                    if (!string.Equals(md.GetString(tDef.Name), tn, StringComparison.Ordinal)) continue;
                    if (!string.Equals(md.GetString(tDef.Namespace), ns, StringComparison.Ordinal)) continue;

                    foreach (var mDefHandle in tDef.GetMethods())
                    {
                        var mDef = md.GetMethodDefinition(mDefHandle);
                        if (!string.Equals(md.GetString(mDef.Name), methodName, StringComparison.Ordinal)) continue;

                        foreach (var caHandle in mDef.GetCustomAttributes())
                        {
                            var ca = md.GetCustomAttribute(caHandle);
                            string? attrName = null;
                            string? attrNs = null;
                            switch (ca.Constructor.Kind)
                            {
                                case HandleKind.MemberReference:
                                    {
                                        var mr = md.GetMemberReference((MemberReferenceHandle)ca.Constructor);
                                        var parent = mr.Parent;
                                        if (parent.Kind == HandleKind.TypeReference)
                                        {
                                            var tr = md.GetTypeReference((TypeReferenceHandle)parent);
                                            attrName = md.GetString(tr.Name);
                                            attrNs = md.GetString(tr.Namespace);
                                        }
                                        else if (parent.Kind == HandleKind.TypeDefinition)
                                        {
                                            var td = md.GetTypeDefinition((TypeDefinitionHandle)parent);
                                            attrName = md.GetString(td.Name);
                                            attrNs = md.GetString(td.Namespace);
                                        }
                                        break;
                                    }
                                case HandleKind.MethodDefinition:
                                    {
                                        var declM = md.GetMethodDefinition((MethodDefinitionHandle)ca.Constructor);
                                        var declType = md.GetTypeDefinition(declM.GetDeclaringType());
                                        attrName = md.GetString(declType.Name);
                                        attrNs = md.GetString(declType.Namespace);
                                        break;
                                    }
                            }
                            if (attrName == null) continue;
                            if (string.Equals(attrName, "InitializeOnLoadAttribute", StringComparison.Ordinal) ||
                                string.Equals(attrName, "InitializeOnLoad", StringComparison.Ordinal) ||
                                (string.Equals(attrNs, "GameEngine.Scripting", StringComparison.Ordinal) && string.Equals(attrName, "InitializeOnLoadAttribute", StringComparison.Ordinal)))
                            {
                                return true;
                            }
                        }
                    }
                }
            }
            catch { }
            return false;
        }



        private static void TryInstallConsoleRedirect()
        {
            try
            {
                var outWriter = Console.Out;
		    	        var typeName = outWriter?.GetType()?.FullName ?? string.Empty;
		    	        IoLTrace($"[IoL] Console redirect: current Console.Out={typeName}");
		    	        if (typeName.Contains("EngineLogWriter", StringComparison.Ordinal))
		    	        {
		    	            Info("[HotReload] TryInstallConsoleRedirect: Console.Out is already EngineLogWriter; skipping.");
		    	            IoLTrace("[IoL] Console redirect: Console.Out already EngineLogWriter; skipping.");
		    	            return;
		    	        }

		    	        Info($"[HotReload] TryInstallConsoleRedirect: current Console.Out={typeName}");

		    	        var cb = AppDomain.CurrentDomain.GetAssemblies().FirstOrDefault(a => string.Equals(a.GetName().Name, "GameEngine.CoreBridge", StringComparison.Ordinal));
		    	        if (cb == null)
		    	        {
		    	            Info("[HotReload] TryInstallConsoleRedirect: GameEngine.CoreBridge assembly not found in AppDomain.");
		    	            IoLTrace("[IoL] Console redirect: GameEngine.CoreBridge assembly not found in AppDomain.");
		    	            return;
		    	        }

		    	        var t = cb.GetType("GameEngine.CoreBridge.EngineLogWriter");
		    	        if (t == null)
		    	        {
		    	            Info("[HotReload] TryInstallConsoleRedirect: EngineLogWriter type not found.");
		    	            IoLTrace("[IoL] Console redirect: EngineLogWriter type not found.");
		    	            return;
		    	        }

		    	        var m = t.GetMethod("Install", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static);
		    	        if (m == null)
		    	        {
		    	            Info("[HotReload] TryInstallConsoleRedirect: Install method not found on EngineLogWriter.");
		    	            IoLTrace("[IoL] Console redirect: Install method not found on EngineLogWriter.");
		    	            return;
		    	        }

		    	        var parameters = m.GetParameters();
		    	        Info($"[HotReload] TryInstallConsoleRedirect: invoking Install (parameters={parameters.Length}).");
		    	        IoLTrace($"[IoL] Console redirect: invoking EngineLogWriter.Install (parameters={parameters.Length}).");
		    	        if (parameters.Length == 1 && parameters[0].ParameterType == typeof(bool))
		    	        {
		    	            m.Invoke(null, new object?[] { true });
		    	        }
		    	        else
		    	        {
		    	            m.Invoke(null, null);
		    	        }

		    	        var afterType = Console.Out?.GetType()?.FullName ?? "<null>";
		    	        Info($"[HotReload] TryInstallConsoleRedirect: after invoke, Console.Out={afterType}");
		    	        IoLTrace($"[IoL] Console redirect: after install, Console.Out={afterType}");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[HotReload] TryInstallConsoleRedirect failed: {ex.Message}");
            }
        }

        /// <summary>
        /// Initialize the core hot reload manager
        /// This method is called once during engine startup and the manager stays loaded
        /// </summary>
        /// Returns ScriptingOpResult.Ok (0) on success; negative values map to ScriptingOpResult codes.

        public static int Initialize()
        {
            try
            {
                lock (m_Lock)
                {
                    if (m_IsInitialized)
                    {
                        // Already initialized; no-op
                        return 0;
                    }

                    Info("[HotReload] Initializing Core Hot-Reload Manager");
                    EnsureSharedAssembliesInDefault();
                    m_IsInitialized = true;
                    Info("[HotReload] Core hot-reload manager initialized");
                    return 0;
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[HotReload] Failed to initialize: {ex.Message}");
                return -1;
            }
        }

        /// <summary>
        /// Load user scripts assembly using collectible AssemblyLoadContext with stream-based loading
        /// Returns ScriptingOpResult.Ok (0) on success; negative values map to ScriptingOpResult codes.

        /// This prevents file locking and enables true hot-reload
        /// </summary>
        public static int LoadUserScriptsAssembly(string assemblyPath)
        {
            try
            {
                if (string.IsNullOrWhiteSpace(assemblyPath)) return -2; // InvalidArg
                if (!File.Exists(assemblyPath)) return -3;             // NotFound

                // Ensure initialized before load
                if (!m_IsInitialized)
                {
                    var rcInit = Initialize();
                    if (rcInit != 0) return rcInit;
                }

                // Detach any existing current assembly/domain under lock; unload outside (B3).
                CollectibleAssemblyContext? oldContext;
                ulong oldDomainId;
                string? oldAssemblyName;
                lock (m_Lock)
                {
                    (oldContext, oldDomainId, oldAssemblyName) = DetachCurrentAssemblyLocked();

                    // Track the last loaded path for potential reload semantics
                    m_LastUserScriptsPath = assemblyPath;
                }
                if (oldContext != null)
                {
                    UnloadContext(oldContext, oldDomainId, oldAssemblyName);
                }

                // Load via bytes to avoid file locking and keep behavior consistent with domain APIs
                byte[] assemblyBytes = File.ReadAllBytes(assemblyPath);
                int dom = LoadCompiledAssembly(assemblyBytes);
                if (dom <= 0) return dom; // propagate negative codes from LoadCompiledAssembly

                // For legacy API, treat the most recently loaded domain as "current"
                lock (m_Lock)
                {
                    m_CurrentDomainId = (ulong)dom;
                }

                return 0;
            }
            catch
            {
                return -1;
            }
        }

        /// Returns ScriptingOpResult.Ok (0) on success; negative values map to ScriptingOpResult codes.

        /// <summary>
        /// Unload current user scripts assembly and dispose collectible context.
        /// Legacy helper that delegates to UnloadDomain for the current domain.
        /// </summary>
        public static int UnloadUserScriptsAssembly()
        {
            try
            {
                ulong dom;
                lock (m_Lock)
                {
                    if (!m_IsInitialized && m_CurrentAssembly == null) return -4; // NotInitialized

                    if (m_CurrentDomainId == 0)
                    {
                        // Nothing loaded; treat as success for legacy callers
                        return 0;
                    }

                    dom = m_CurrentDomainId;
                }

                // UnloadDomain runs user teardown via ALC.Unloading — never call it
                // while holding m_Lock (B3). It clears the current pointers itself.
                return UnloadDomain(dom);
            }
            catch
            {
                return -1;
            }
        }


        /// <summary>
        /// Unload a specific domain by id without relying on m_CurrentDomainId. Returns 0 on success.
        /// </summary>
        public static int UnloadDomain(ulong domain)
        {
            try
            {
                CollectibleAssemblyContext toUnload;
                string? assemblyName = null;
                lock (m_Lock)
                {
                    if (!m_IsInitialized && m_CurrentAssembly == null) return -1;
                    if (domain == 0) return -2;
                    if (!m_Domains.TryGetValue(domain, out var tuple)) return -2;

                    try { assemblyName = tuple.Asm?.GetName().Name; } catch { }

                    // Remove indices and token maps for this domain
                    m_NameToTokenByDomain.Remove(domain);
                    m_TokenToMethodByDomain.Remove(domain);
                    m_MethodIndexFullByDomain.Remove(domain);
                    m_MethodIndexSimpleByDomain.Remove(domain);
                    m_Domains.Remove(domain);

                    // If this was the active domain, clear current pointers
                    if (m_CurrentDomainId == domain)
                    {
                        m_CurrentDomainId = 0;
                        m_CurrentAssembly = null;
                        m_CurrentContext = null;
                    }

                    toUnload = tuple.Ctx;
                }

                // Unload outside m_Lock (B3): Unload() synchronously raises
                // ALC.Unloading, which runs user teardown and registration purges.
                UnloadContext(toUnload, domain, assemblyName);
                return 0;
            }
            catch { return -1; }
        }

        /// <summary>
        /// Eagerly runs [ModuleInitializer]s for every module in the given context (or
        /// the single assembly when no context is available). The runtime only runs
        /// module constructors on first code execution, so without this a freshly
        /// loaded/swapped assembly's generated registrations (component schemas,
        /// GameSystemRunner entries) stay invisible until play-enter, and the previous
        /// domain's stale registrations keep pinning its ALC in the meantime.
        /// User init code — must be called without m_Lock held (B3).
        /// </summary>
        private static void RunModuleConstructors(CollectibleAssemblyContext? context, Assembly? primary)
        {
            var sw = System.Diagnostics.Stopwatch.StartNew();
            int moduleCount = 0;
            try
            {
                IEnumerable<Assembly> assemblies;
                if (context != null)
                    assemblies = context.Assemblies;
                else if (primary != null)
                    assemblies = new[] { primary };
                else
                    return;

                foreach (var asm in assemblies)
                {
                    Module[] modules;
                    try { modules = asm.GetModules(); }
                    catch { continue; }
                    foreach (var module in modules)
                    {
                        try
                        {
                            System.Runtime.CompilerServices.RuntimeHelpers.RunModuleConstructor(module.ModuleHandle);
                            moduleCount++;
                        }
                        catch (Exception ex)
                        {
                            Console.Error.WriteLine(
                                $"[HotReload] Module initializer for '{asm.GetName().Name}' threw: {ex.GetBaseException().Message}");
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[HotReload] RunModuleConstructors failed: {ex.Message}");
            }
            sw.Stop();
            Info($"[HotReload] Ran module initializers for {moduleCount} module(s) in {sw.ElapsedMilliseconds}ms");
        }

        /// <summary>
        /// Initiates unload of a detached collectible context and schedules its
        /// bounded collection verification. Must be called without m_Lock held.
        /// Unload protocol (B5): [OnScriptsUnload] handlers run first (time-boxed,
        /// in the unloading domain), then ALC.Unload() fires Unloading subscribers
        /// (registration purges), then collection is verified off-thread.
        /// </summary>
        private static void UnloadContext(CollectibleAssemblyContext context, ulong domainId, string? assemblyName)
        {
            InvokeScriptsUnloadHandlers(context, domainId);
            try
            {
                context.Unload();
                System.Threading.Interlocked.Increment(ref m_TotalUnloads);
                ScheduleUnloadVerification(context, domainId, assemblyName);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[HotReload] UnloadContext({domainId}): Unload threw {ex.GetType().Name}: {ex.Message}");
            }
        }

        private const string kOnScriptsUnloadAttributeFullName = "GameEngine.Scripting.OnScriptsUnloadAttribute";
        private const int kScriptsUnloadBudgetMs = 1000;

        /// <summary>
        /// Per-domain teardown broadcast: invokes every static parameterless
        /// [OnScriptsUnload] method in the unloading context so user code can stop
        /// threads/timers and unsubscribe from pinned events before the ALC unloads.
        /// Time-boxed: once the budget is exhausted, remaining handlers are skipped
        /// with a diagnostic (a running handler cannot be aborted). Exceptions are
        /// swallowed per handler. User code — never call with m_Lock held (B3).
        /// </summary>
        private static void InvokeScriptsUnloadHandlers(CollectibleAssemblyContext context, ulong domainId)
        {
            try
            {
                var sw = System.Diagnostics.Stopwatch.StartNew();
                int invoked = 0, skipped = 0;
                foreach (var asm in context.Assemblies)
                {
                    Type[] types;
                    try { types = asm.GetTypes(); }
                    catch (ReflectionTypeLoadException ex) { types = ex.Types.Where(t => t != null).Cast<Type>().ToArray(); }
                    catch { continue; }

                    foreach (var t in types)
                    {
                        if (t == null) continue;
                        foreach (var mi in t.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static))
                        {
                            bool hasAttr = false;
                            try
                            {
                                foreach (var cad in mi.GetCustomAttributesData())
                                {
                                    if (string.Equals(cad.AttributeType?.FullName, kOnScriptsUnloadAttributeFullName, StringComparison.Ordinal))
                                    {
                                        hasAttr = true;
                                        break;
                                    }
                                }
                            }
                            catch { }
                            if (!hasAttr) continue;
                            if (mi.GetParameters().Length != 0) { skipped++; continue; }

                            if (sw.ElapsedMilliseconds > kScriptsUnloadBudgetMs)
                            {
                                skipped++;
                                Console.Error.WriteLine(
                                    $"[HotReload] Domain {domainId}: OnScriptsUnload budget ({kScriptsUnloadBudgetMs}ms) exhausted; skipping '{t.FullName}.{mi.Name}'");
                                continue;
                            }

                            try { mi.Invoke(null, null); invoked++; }
                            catch (Exception ex)
                            {
                                Console.Error.WriteLine(
                                    $"[HotReload] OnScriptsUnload '{t.FullName}.{mi.Name}' threw: {ex.GetBaseException().Message}");
                            }
                        }
                    }
                }
                if (invoked > 0 || skipped > 0)
                {
                    Console.WriteLine($"[HotReload] Domain {domainId}: OnScriptsUnload handlers invoked={invoked} skipped={skipped} in {sw.ElapsedMilliseconds}ms");
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[HotReload] InvokeScriptsUnloadHandlers({domainId}) failed: {ex.Message}");
            }
        }


        /// <summary>
        /// Reload the user scripts assembly (unload + load) using last known path, if any.
        /// </summary>
        /// Returns ScriptingOpResult.Ok (0) on success; negative values map to ScriptingOpResult codes.

        public static int ReloadUserScriptsAssembly()
        {
            try
            {
                string? path = null;
                lock (m_Lock)
                {
                    if (!m_IsInitialized && m_CurrentAssembly == null) return -4; // NotInitialized
                    path = m_LastUserScriptsPath;
                }

                if (string.IsNullOrWhiteSpace(path))
                {
                    // No persisted path available; legacy semantics map this to InvalidArg
                    return -2;
                }

                // Delegate to LoadUserScriptsAssembly for unified behavior
                return LoadUserScriptsAssembly(path);
            }
            catch
            {
                return -1;
            }
        }

        /// <summary>
        /// Call a method in the current loaded user scripts assembly
        /// </summary>
        public static int CallMethodInUserScripts(string methodName)
        {
            return CallMethodInUserScripts(methodName, (object[]?)null);
        }

        public static int CallMethodInUserScripts(string methodName, object[]? args)
        {
            try
            {
                // Snapshot state under lock; scan and invoke outside (B3) so arbitrary
                // user code never runs while m_Lock is held.
                List<Assembly> assemblies;
                lock (m_Lock)
                {
                    if (!m_IsInitialized)
                        return -1;
                    if (m_CurrentAssembly == null)
                        return -2;

                    // Build assembly set to search
                    assemblies = new List<Assembly> { m_CurrentAssembly };
                    if (m_CurrentContext != null) assemblies.AddRange(m_CurrentContext.Assemblies);
                }
                {
                    foreach (var a in AppDomain.CurrentDomain.GetAssemblies())
                        if (string.Equals(a.GetName().Name, "GameEngine.Scripts", StringComparison.OrdinalIgnoreCase)) assemblies.Add(a);
                    var uniqueAssemblies = new HashSet<Assembly>(assemblies);

                    // Local helper to invoke with signature checking
                    int? TryInvoke(MethodInfo mi)
                    {
                        var ps = mi.GetParameters();
                        try
                        {
                            object? res = null;
                            if ((args == null || args.Length == 0))
                            {
                                if (ps.Length == 0 && mi.IsStatic && (mi.ReturnType == typeof(int) || mi.ReturnType == typeof(void)))
                                {
                                    res = mi.Invoke(null, null);
                                    if (mi.ReturnType == typeof(void))
                                        return 0;
                                    return res is int ir ? ir : 0;
                                }
                                // Not a zero-arg int static; keep scanning other candidates
                                return null;
                            }
                            else if (args != null && ps.Length == args.Length)
                            {
                                bool match = true;
                                for (int i = 0; i < ps.Length; ++i)
                                {
                                    var pt = ps[i].ParameterType; var av = args[i];
                                    if (av is int && (pt == typeof(int) || pt == typeof(System.Int32))) continue;
                                    match = false; break;
                                }
                                if (!match) return null;
                                res = mi.Invoke(null, args);
                                return res is int ir ? ir : 0;
                            }
                            // Length mismatch; keep scanning
                            return null;
                        }
                        catch (TargetInvocationException) { return -5; }
                        catch { return -1; }
                    }

                    // Qualified 'Type.Method'
                    int dot = methodName.LastIndexOf('.');
                    if (dot > 0 && dot < methodName.Length - 1)
                    {
                        string className = methodName.Substring(0, dot);
                        string simpleName = methodName.Substring(dot + 1);
                        foreach (var asm in uniqueAssemblies)
                        {
                            foreach (var t in asm.GetTypes())
                            {
                                var tn = t.Name; var tfn = t.FullName ?? tn;
                                if (tn.Equals(className, StringComparison.OrdinalIgnoreCase) || tfn.EndsWith("." + className, StringComparison.Ordinal))
                                {
                                    var mi = t.GetMethod(simpleName, BindingFlags.Public | BindingFlags.Static | BindingFlags.IgnoreCase);
                                    if (mi != null)
                                    {
                                        var r = TryInvoke(mi);
                                        if (r.HasValue) return r.Value;
                                    }
                                }
                            }
                        }
                    }

                    // Fallback: by simple method name across all public static
                    string simple = methodName;
                    int dot2 = methodName.LastIndexOf('.');
                    if (dot2 >= 0 && dot2 < methodName.Length - 1) simple = methodName.Substring(dot2 + 1);
                    bool anyNameMatch = false;

                    foreach (var asm in uniqueAssemblies)
                    {
                        foreach (var type in asm.GetTypes())
                        {
                            foreach (var mi in type.GetMethods(BindingFlags.Public | BindingFlags.Static))
                            {
                                if (!mi.Name.Equals(simple, StringComparison.OrdinalIgnoreCase) && !mi.Name.Equals(methodName, StringComparison.OrdinalIgnoreCase))
                                    continue;
                                anyNameMatch = true;
                                var r = TryInvoke(mi);
                                if (r.HasValue) return r.Value;
                            }
                        }
                    }

                    return anyNameMatch ? -4 : -3;
                }
            }
            catch { return -1; }
        }

        /// <summary>
        /// Get information about the currently loaded user scripts assembly
        /// </summary>
        public static int GetUserScriptsAssemblyInfo()
        {
            try
            {
                lock (m_Lock)
                {
                    if (!m_IsInitialized)
                    {
                        Console.WriteLine("[HotReload] Not initialized");
                        return -1;
                    }

                    if (m_CurrentAssembly == null)
                    {
                        // No user scripts assembly loaded
                        return 0;
                    }

                    var types = m_CurrentAssembly.GetTypes();

                    return types.Length;
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[HotReload] GetUserScriptsAssemblyInfo failed: {ex.Message}");
                return -1;
            }
        }

        // Bounded async unload verification: after Unload() is initiated, poll a WeakReference to
        // the context with gentle GC passes on a thread-pool thread until it is collected or the
        // attempt budget runs out. Replaces the forced blocking gen2 GC storms that used to run on
        // the (frame/swap-critical) caller thread.
        private const int kUnloadVerifyMaxAttempts = 10;
        private const int kUnloadVerifyDelayMs = 100;
        private const int kUnloadVerifyFinalizerWaitMs = 500;

        /// <summary>
        /// Schedule background verification that an already-unloading collectible context actually
        /// gets collected. The background closure roots only a WeakReference, never the context.
        /// A context that survives the attempt budget is recorded as a leaked ALC (B1).
        /// </summary>
        private static void ScheduleUnloadVerification(CollectibleAssemblyContext unloadedContext, ulong domainId, string? assemblyName)
        {
            var weakRef = new WeakReference(unloadedContext);
            unloadedContext = null!;
            _ = System.Threading.Tasks.Task.Run(async () =>
            {
                // A hung finalizer in leaked user code blocks GC.WaitForPendingFinalizers
                // forever; an unbounded wait here wedged this verification AND starved every
                // later one of thread-pool threads. Bound the wait and stop waiting on
                // finalizers for the rest of this verification once it times out.
                bool finalizersResponsive = true;
                for (int attempt = 1; attempt <= kUnloadVerifyMaxAttempts; attempt++)
                {
                    try { GC.Collect(); } catch { return; }
                    if (finalizersResponsive)
                    {
                        try
                        {
                            var waiter = System.Threading.Tasks.Task.Run(static () => GC.WaitForPendingFinalizers());
                            finalizersResponsive = waiter.Wait(kUnloadVerifyFinalizerWaitMs);
                            if (!finalizersResponsive)
                            {
                                Console.Error.WriteLine(
                                    $"[HotReload] Domain {domainId} ('{assemblyName ?? "<unknown>"}'): finalizer queue unresponsive after {kUnloadVerifyFinalizerWaitMs}ms; continuing verification without finalizer waits");
                            }
                        }
                        catch { finalizersResponsive = false; }
                    }
                    if (!weakRef.IsAlive)
                    {
                        // Positive verdict logs unconditionally: leak reports are loud, so the
                        // all-clear must be visible without GE_HOTRELOAD_VERBOSE.
                        Console.WriteLine($"[HotReload] Domain {domainId} ('{assemblyName ?? "<unknown>"}') ALC collected after {attempt} GC pass(es)");
                        return;
                    }
                    try { await System.Threading.Tasks.Task.Delay(kUnloadVerifyDelayMs).ConfigureAwait(false); } catch { return; }
                }

                OnAlcLeakDetected(domainId, assemblyName);
            });
        }

        private static void OnAlcLeakDetected(ulong domainId, string? assemblyName)
        {
            int leakedCount = System.Threading.Interlocked.Increment(ref m_LeakedAlcCount);
            System.Threading.Interlocked.Exchange(ref m_LastLeakedDomainId, unchecked((long)domainId));
            try
            {
                int approxMs = kUnloadVerifyMaxAttempts * kUnloadVerifyDelayMs;
                Console.Error.WriteLine(
                    $"[HotReload] ***** ALC LEAK: domain {domainId} (assembly '{assemblyName ?? "<unknown>"}', swap generation {System.Threading.Volatile.Read(ref m_TotalSwaps)}) " +
                    $"is still alive after {kUnloadVerifyMaxAttempts} GC passes (~{approxMs}ms). " +
                    "Something is pinning the old AssemblyLoadContext (static field, event subscription, timer, or running thread in user code); " +
                    $"its assemblies and statics remain resident. leakedAlcCount={leakedCount}. *****");
            }
            catch { }
        }

        /// <summary>
        /// Detaches the current user scripts context from all bookkeeping and returns it
        /// for unloading. Caller must hold m_Lock and must initiate the actual unload via
        /// UnloadContext AFTER releasing the lock (B3: Unload runs user teardown).
        /// </summary>
        private static (CollectibleAssemblyContext? Context, ulong DomainId, string? AssemblyName) DetachCurrentAssemblyLocked()
        {
            if (m_CurrentContext == null)
                return (null, 0, null);

            var context = m_CurrentContext;
            var domainId = m_CurrentDomainId;
            string? assemblyName = null;
            try { assemblyName = m_CurrentAssembly?.GetName().Name; } catch { }

            // Clear token maps for current domain
            if (m_CurrentDomainId != 0)
            {
                m_NameToTokenByDomain.Remove(m_CurrentDomainId);
                m_TokenToMethodByDomain.Remove(m_CurrentDomainId);
                m_MethodIndexFullByDomain.Remove(m_CurrentDomainId);
                m_MethodIndexSimpleByDomain.Remove(m_CurrentDomainId);
                m_Domains.Remove(m_CurrentDomainId);
                m_CurrentDomainId = 0;
            }

            m_CurrentAssembly = null;
            m_CurrentContext = null;
            return (context, domainId, assemblyName);
        }


        /// <summary>
        /// Call a method in the specified user scripts domain (per-ALC routing)
        /// </summary>
        public static int CallMethodInDomain(ulong domain, string methodName)
        {
            try
            {
                // Resolve the target assembly under lock; reflection scan and user-code
                // invocation happen outside (B3).
                Assembly asm;
                lock (m_Lock)
                {
                    if (!m_IsInitialized && m_CurrentAssembly == null) return -1;

                    if (domain == 0 || !m_Domains.TryGetValue(domain, out var tuple))
                    {
                        // Fallback: use current loaded assembly if specific domain not found
                        if (m_CurrentDomainId != 0 && m_Domains.TryGetValue(m_CurrentDomainId, out var cur))
                        {
                            asm = cur.Asm;
                        }
                        else if (m_CurrentAssembly != null)
                        {
                            asm = m_CurrentAssembly;
                        }
                        else
                        {
                            Console.WriteLine($"[HotReload] Domain not found: {domain} (domains={m_Domains.Count}) and no current assembly");
                            return -2;
                        }
                    }
                    else
                    {
                        asm = tuple.Asm;
                    }
                }

                {

#if DEBUG
                        try { Console.WriteLine($"[HRM DBG] asm='{asm.FullName}', types={asm.GetTypes().Length}"); } catch { }
#endif


#if DEBUG
                        try
                        {
                            var dbgType = asm.GetType("HotReloadTest", throwOnError: false, ignoreCase: true);
                            Console.WriteLine($"[HRM DBG] Invoke search: asm='{asm.FullName}' hasHotReloadTest={(dbgType!=null)}");
                        }
                        catch { }
#endif


                    // Resolve 'Namespace.Type.Method' or simple 'Method'
                    string simple = methodName;
                    string? className = null;
                    int dot = methodName.LastIndexOf('.');
                    if (dot > 0)
                    {
                        className = methodName.Substring(0, dot);
                        simple = methodName.Substring(dot + 1);
                    }

                    int? TryInvoke(MethodInfo mi)
                    {
                        var ps = mi.GetParameters();
                        if (ps.Length == 0 && mi.IsStatic && (mi.ReturnType == typeof(int) || mi.ReturnType == typeof(void)))
                        {
                            var r = mi.Invoke(null, null);
                            if (mi.ReturnType == typeof(void))
                                return 0;
                            return r is int i ? i : -1;
                        }
                        return null;
                    }

                    static IEnumerable<Type> SafeGetTypes(Assembly a)
                    {
                        try { return a.GetTypes(); }
                        catch (ReflectionTypeLoadException ex) { return ex.Types.Where(t => t != null)!; }
                        catch { return Array.Empty<Type>(); }
                    }


                    // 1) Direct fully-qualified type lookup (case-sensitive, then insensitive), then robust scan
                    if (!string.IsNullOrEmpty(className))
                    {
                        var type = asm.GetType(className, throwOnError: false, ignoreCase: false)
                                   ?? asm.GetType(className, throwOnError: false, ignoreCase: true);
                        if (type == null)
                        {
                            foreach (var t in SafeGetTypes(asm))
                            {
                                if (t == null || !t.IsPublic) continue;
                                var fn = t.FullName ?? t.Name;
                                if (string.Equals(fn, className, StringComparison.Ordinal) || string.Equals(fn, className, StringComparison.OrdinalIgnoreCase))
                                { type = t; break; }
                            }
                        }
                        if (type != null && type.IsPublic)
                        {
                            var mi0 = type.GetMethod(simple, BindingFlags.Public | BindingFlags.Static);
                            var r0 = mi0 != null ? TryInvoke(mi0) : null;
                            if (r0.HasValue) return r0.Value;

                            var mi1 = type.GetMethod(simple, BindingFlags.Public | BindingFlags.Static | BindingFlags.IgnoreCase);
                            var r1 = mi1 != null ? TryInvoke(mi1) : null;
                            if (r1.HasValue) return r1.Value;
                        }
                    }

                    // 2) Fallback: scan for exact type-name match
                    if (!string.IsNullOrEmpty(className))
                    {
                        foreach (var t in SafeGetTypes(asm))
                        {
                            if (t == null || !t.IsPublic) continue;
                            var tn = t.FullName ?? t.Name;
                            if (!string.Equals(tn, className, StringComparison.Ordinal)) continue;
                            var mi = t.GetMethod(simple, BindingFlags.Public | BindingFlags.Static);
                            if (mi != null)
                            {
                                var r = TryInvoke(mi);
                                if (r.HasValue) return r.Value;
                            }
                        }
                    }
                    else
                    {
                        // 3) No class specified: scan all public static int Methods by name
                        foreach (var t in SafeGetTypes(asm))
                        {
                            if (t == null || !t.IsPublic) continue;
                            foreach (var mi in t.GetMethods(BindingFlags.Public | BindingFlags.Static))
                            {
                                if (!mi.Name.Equals(simple, StringComparison.Ordinal) && !mi.Name.Equals(methodName, StringComparison.Ordinal))
                                    continue;
                                var r = TryInvoke(mi);
                                if (r.HasValue) return r.Value;
                            }
                        }
                    }

                    // Not found — single diagnostic to aid native flow debugging
                    try { Console.WriteLine($"[HotReload] CallMethodInDomain not found: '{methodName}' in asm='{asm.FullName}'"); } catch { }
                    return -1;
                }
            }
            catch { return -1; }
        }

        /// <summary>
        /// Query an export (public static int with no args) by name within a domain. Returns an int ScriptingOpResult code: Ok (0) on success and assigns a stable token; negative values map to failure codes.
        /// Common return codes: Ok(0), Fail(-1), InvalidArg(-2).
        /// </summary>
        public static int QueryExportInDomain(ulong domain, string methodName, ref ulong token)
        {
            try
            {
#if DEBUG
                try { Console.WriteLine($"[HRM Token] Query: dom={domain} name='{methodName}'"); } catch { }
#endif
                lock (m_Lock)
                {
                    if (!m_IsInitialized && m_CurrentAssembly == null) return -1;

                    // Ensure method indices exist for this domain (can be cleared during other operations).
                    ulong domIdEnsure = (domain != 0) ? domain : (m_CurrentDomainId != 0 ? m_CurrentDomainId : 0UL);
                    if (domIdEnsure != 0 && !m_MethodIndexFullByDomain.ContainsKey(domIdEnsure))
                    {
                        if (m_Domains.TryGetValue(domIdEnsure, out var tupEnsure))
                        {
                            try { BuildDomainMethodIndex(domIdEnsure, tupEnsure.Asm); } catch { }
                        }
                    }

                    // Resolve target assembly
                    Assembly asm;
                    if (domain == 0 || !m_Domains.TryGetValue(domain, out var tuple))
                    {
                        if (m_CurrentDomainId != 0 && m_Domains.TryGetValue(m_CurrentDomainId, out var cur)) asm = cur.Asm;
                        else if (m_CurrentAssembly != null) asm = m_CurrentAssembly;
                        else return -2;
                    }
                    else { asm = tuple.Asm; }

                    // Resolve 'Namespace.Type.Method' or simple 'Method'
                    string simple = methodName;
                    string? className = null;
                    int dot = methodName.LastIndexOf('.');
                    if (dot > 0) { className = methodName.Substring(0, dot); simple = methodName.Substring(dot + 1); }


                    // Fast-path: use prebuilt index when available
                    ulong domIdFast = (domain != 0) ? domain : (m_CurrentDomainId != 0 ? m_CurrentDomainId : 0UL);
                    if (domIdFast != 0 &&
                        m_MethodIndexFullByDomain.TryGetValue(domIdFast, out var fullIdxFast) &&
                        m_MethodIndexSimpleByDomain.TryGetValue(domIdFast, out var simpleIdxFast))
                    {
                        bool hit;
                        Func<int>? delFast;
                        if (!string.IsNullOrEmpty(className)) { hit = fullIdxFast.TryGetValue(methodName, out delFast); }
                        else { hit = simpleIdxFast.TryGetValue(simple, out delFast); }
                        if (hit)
                        {
                            if (!m_NameToTokenByDomain.TryGetValue(domIdFast, out Dictionary<string, ulong>? nameMapFast)) { nameMapFast = new Dictionary<string, ulong>(StringComparer.Ordinal); m_NameToTokenByDomain[domIdFast] = nameMapFast; }
                            if (!m_TokenToMethodByDomain.TryGetValue(domIdFast, out Dictionary<ulong, Func<int>>? tokMapFast)) { tokMapFast = new Dictionary<ulong, Func<int>>(); m_TokenToMethodByDomain[domIdFast] = tokMapFast; }
                            if (!nameMapFast.TryGetValue(methodName, out var tokFast)) { tokFast = m_NextToken++; nameMapFast[methodName] = tokFast; }
                            tokMapFast[tokFast] = delFast!;
                            token = tokFast;
                            return 0;
                        }
                    }

                    static IEnumerable<Type> SafeGetTypes(Assembly a)
                    {
                        try { return a.GetTypes(); }
                        catch (ReflectionTypeLoadException ex) { return ex.Types.Where(t => t != null)!; }
                        catch { return Array.Empty<Type>(); }
                    }

                    static MethodInfo? PickMethod(Type t, string simpleName, string originalName)
                    {
                        var mi = t.GetMethod(simpleName, BindingFlags.Public | BindingFlags.Static)
                              ?? t.GetMethod(simpleName, BindingFlags.Public | BindingFlags.Static | BindingFlags.IgnoreCase);
                        if (mi == null && !string.IsNullOrEmpty(originalName))
                        {
                            // Allow full-name equality as a last resort
                            foreach (var m in t.GetMethods(BindingFlags.Public | BindingFlags.Static))
                            {
                                if (m.Name.Equals(originalName, StringComparison.Ordinal)) { mi = m; break; }
                            }
                        }
                        if (mi != null)
                        {
                            var ps = mi.GetParameters();
                            if (ps.Length == 0 && mi.IsStatic && (mi.ReturnType == typeof(int) || mi.ReturnType == typeof(void))) return mi;
                        }
                        return null;
                    }

                    MethodInfo? found = null;
                    if (!string.IsNullOrEmpty(className))
                    {
                        var type = asm.GetType(className, throwOnError: false, ignoreCase: false)
                               ?? asm.GetType(className, throwOnError: false, ignoreCase: true);
                        if (type == null)
                        {
                            foreach (var t in SafeGetTypes(asm))
                            {
                                if (t == null || !t.IsPublic) continue;
                                var fn = t.FullName ?? t.Name;
                                if (string.Equals(fn, className, StringComparison.Ordinal) || string.Equals(fn, className, StringComparison.OrdinalIgnoreCase)) { type = t; break; }
                            }
                        }
                        if (type != null && type.IsPublic) { found = PickMethod(type, simple, methodName); }
                    }
                    if (found == null)
                    {
                        // Fallback: no class specified or not found — scan all public types
                        foreach (var t in SafeGetTypes(asm))
                        {
                            if (t == null || !t.IsPublic) continue;
                            var mi = PickMethod(t, simple, methodName);
                            if (mi != null) { found = mi; break; }
                        }
                    }

                    if (found == null) { token = 0UL; return -1; }

                    // Ensure domain ID for token tables
                    ulong domId = (domain != 0) ? domain : (m_CurrentDomainId != 0 ? m_CurrentDomainId : 0UL);
                    if (domId == 0) { token = 0UL; return -2; }

                    if (!m_NameToTokenByDomain.TryGetValue(domId, out var nameMap)) { nameMap = new Dictionary<string, ulong>(StringComparer.Ordinal); m_NameToTokenByDomain[domId] = nameMap; }
                    if (!m_TokenToMethodByDomain.TryGetValue(domId, out var tokMap)) { tokMap = new Dictionary<ulong, Func<int>>(); m_TokenToMethodByDomain[domId] = tokMap; }

                    if (!nameMap.TryGetValue(methodName, out var tok))
                    {
                        tok = m_NextToken++;
                        nameMap[methodName] = tok;
                        Func<int> del;
                        if (found.ReturnType == typeof(void))
                        {
                            try
                            {
                                var a = (Action)Delegate.CreateDelegate(typeof(Action), found);
                                del = () => { a(); return 0; };
                            }
                            catch
                            {
                                del = () => { found.Invoke(null, null); return 0; };
                            }
                        }
                        else
                        {
                            try { del = (Func<int>)Delegate.CreateDelegate(typeof(Func<int>), found); }
                            catch { del = () => { var r = found.Invoke(null, null); return r is int i ? i : -1; }; }
                        }
                        tokMap[tok] = del;
                    }
                    token = tok;
#if DEBUG
                    try { Console.WriteLine($"[HRM Token] → dom={domId} token={token}"); } catch { }
#endif
                    return 0;
                }
            }

            catch { token = 0UL; return -1; }
        }

        /// <summary>Invoke a previously queried export by token within a domain. Returns the method's int result on success; on failure returns a negative ScriptingOpResult code.
        // Build fast method indices for a domain: full name (Namespace.Type.Method) and simple name (Method)
        private static void BuildDomainMethodIndex(ulong domId, Assembly asm)
        {
            try
            {
                var fullIdx = new Dictionary<string, Func<int>>(StringComparer.Ordinal);
                var simpleIdx = new Dictionary<string, Func<int>>(StringComparer.Ordinal);

                static IEnumerable<Type> SafeGetTypes(Assembly a)
                {
                    try { return a.GetExportedTypes(); }
                    catch (ReflectionTypeLoadException ex) { return ex.Types.Where(t => t != null && t.IsPublic)!; }
                    catch { return Array.Empty<Type>(); }
                }

                // Scan main scripts assembly plus optional Editor scripts assembly loaded in the same ALC.
                var toScan = new List<Assembly>(2) { asm };
                try
                {
                    var alc = AssemblyLoadContext.GetLoadContext(asm);
                    if (alc != null)
                    {
                        foreach (var a in alc.Assemblies)
                        {
                            if (a == null || ReferenceEquals(a, asm))
                                continue;
                            string? n = null;
                            try { n = a.GetName().Name; } catch { n = null; }
                            if (string.Equals(n, "GameEngine.Editor", StringComparison.OrdinalIgnoreCase))
                            {
                                toScan.Add(a);
                            }
                        }
                    }
                }
                catch { }

                foreach (var a in toScan)
                {
                    foreach (var t in SafeGetTypes(a))
                    {
                        if (t == null || !t.IsPublic) continue;
                        foreach (var mi in t.GetMethods(BindingFlags.Public | BindingFlags.Static))
                        {
                            if (mi.GetParameters().Length != 0 || (mi.ReturnType != typeof(int) && mi.ReturnType != typeof(void))) continue;
                            Func<int> del;
                            if (mi.ReturnType == typeof(void))
                            {
                                try
                                {
                                    var act = (Action)Delegate.CreateDelegate(typeof(Action), mi);
                                    del = () => { act(); return 0; };
                                }
                                catch
                                {
                                    del = () => { mi.Invoke(null, null); return 0; };
                                }
                            }
                            else
                            {
                                try { del = (Func<int>)Delegate.CreateDelegate(typeof(Func<int>), mi); }
                                catch { del = () => { var r = mi.Invoke(null, null); return r is int i ? i : -1; }; }
                            }
                            var fullName = ((t.FullName ?? t.Name) + "." + mi.Name);
                            if (!fullIdx.ContainsKey(fullName)) fullIdx[fullName] = del;
                            if (!simpleIdx.ContainsKey(mi.Name)) simpleIdx[mi.Name] = del; // first wins
                        }
                    }
                }

                m_MethodIndexFullByDomain[domId] = fullIdx;
                m_MethodIndexSimpleByDomain[domId] = simpleIdx;
                Info($"[HotReload] Indexed {fullIdx.Count} exports for domain {domId}");


            }
            catch { /* best-effort only */ }
        }

        /// Common return codes: Fail(-1), InvalidArg(-2).</summary>
        public static int InvokeByToken(ulong domain, ulong token)
        {
            try
            {
                Func<int>? del;
                bool firstInvoke;
                // Resolve under lock, invoke outside (B3): user code that blocks on a
                // thread needing HotReloadManager would otherwise deadlock, and
                // background preloads would contend with per-frame invokes.
                lock (m_Lock)
                {
                    ulong domId = (domain != 0) ? domain : (m_CurrentDomainId != 0 ? m_CurrentDomainId : 0UL);
                    if (domId == 0) return -2;
                    if (!m_TokenToMethodByDomain.TryGetValue(domId, out var tokMap)) return -1;
                    if (!tokMap.TryGetValue(token, out del) || del == null) return -1;
                    firstInvoke = m_FirstInvokePending.Remove(domId);
                }
                if (firstInvoke)
                {
                    var sw = System.Diagnostics.Stopwatch.StartNew();
                    int r = del();
                    sw.Stop();
                    System.Threading.Interlocked.Exchange(ref m_LastFirstInvokeMs, sw.ElapsedMilliseconds);
                    return r;
                }
                return del();
            }
            catch { return -1; }
        }





        /// <summary>
        /// OPTIMIZATION: Pre-load assembly context and assembly on background thread
        /// This performs all heavy operations (file I/O, context creation, assembly loading)
        /// on a background thread to minimize main thread blocking during swap
        /// </summary>
        /// Returns ScriptingOpResult.Ok (0) on success; negative values map to ScriptingOpResult codes.


        private static void PrewarmDelegates(Dictionary<string, Func<int>> full, Dictionary<string, Func<int>> simple)
        {
            try
            {
                foreach (var kv in full)
                {
                    var d = kv.Value;
                    try { System.Runtime.CompilerServices.RuntimeHelpers.PrepareDelegate(d); } catch { }
                    try { var mh = d.Method.MethodHandle; System.Runtime.CompilerServices.RuntimeHelpers.PrepareMethod(mh); } catch { }
                }
                // simple map shares same delegates; nothing extra required
            }
            catch { }
        }

        private static (Dictionary<string, Func<int>> Full, Dictionary<string, Func<int>> Simple) BuildMethodIndicesForAssembly(Assembly asm)
        {
            var fullIdx = new Dictionary<string, Func<int>>(StringComparer.Ordinal);
            var simpleIdx = new Dictionary<string, Func<int>>(StringComparer.Ordinal);

            static IEnumerable<Type> SafeGetTypes(Assembly a)


            {
                try { return a.GetExportedTypes(); }
                catch (ReflectionTypeLoadException ex) { return ex.Types.Where(t => t != null && t.IsPublic)!; }
                catch { return Array.Empty<Type>(); }
            }

            foreach (var t in SafeGetTypes(asm))
            {
                if (t == null || !t.IsPublic) continue;
                foreach (var mi in t.GetMethods(BindingFlags.Public | BindingFlags.Static))
                {
                    if (mi.GetParameters().Length != 0 || (mi.ReturnType != typeof(int) && mi.ReturnType != typeof(void))) continue;
                    Func<int> del;
                    if (mi.ReturnType == typeof(void))
                    {
                        try
                        {
                            var a = (Action)Delegate.CreateDelegate(typeof(Action), mi);
                            del = () => { a(); return 0; };
                        }
                        catch
                        {
                            del = () => { mi.Invoke(null, null); return 0; };
                        }
                    }
                    else
                    {
                        try { del = (Func<int>)Delegate.CreateDelegate(typeof(Func<int>), mi); }
                        catch { del = () => { var r = mi.Invoke(null, null); return r is int i ? i : -1; }; }
                    }
                    var fullName = ((t.FullName ?? t.Name) + "." + mi.Name);
                    if (!fullIdx.ContainsKey(fullName)) fullIdx[fullName] = del;
                    if (!simpleIdx.ContainsKey(mi.Name)) simpleIdx[mi.Name] = del; // first wins


                }
            }

            return (fullIdx, simpleIdx);
        }



        /// <summary>
        /// Loads every DLL in <paramref name="dir"/> (top level only, name-sorted for
        /// determinism) into the given collectible context via LoadFromStream (never
        /// locks the file), with its PDB when present. One bad assembly is logged and
        /// skipped; the rest still load. Assemblies loaded here resolve each other by
        /// simple name from the context's own cache, so cross-package references work
        /// without any resolver hook (compile order already guaranteed dependency DLLs
        /// exist).
        /// </summary>
        private static void LoadPackageAssemblies(CollectibleAssemblyContext context, string dir,
                                                  List<(Assembly Asm, string Path)> results)
        {
            try
            {
                if (!Directory.Exists(dir))
                    return;
                foreach (var dll in Directory.EnumerateFiles(dir, "*.dll", SearchOption.TopDirectoryOnly)
                                             .OrderBy(p => p, StringComparer.OrdinalIgnoreCase))
                {
                    try
                    {
                        byte[] bytes = File.ReadAllBytes(dll);
                        Assembly asm;
                        string pdbPath = Path.ChangeExtension(dll, ".pdb");
                        using var asmStream = new MemoryStream(bytes);
                        if (File.Exists(pdbPath))
                        {
                            using var pdbStream = new MemoryStream(File.ReadAllBytes(pdbPath));
                            asm = context.LoadFromStream(asmStream, pdbStream);
                        }
                        else
                        {
                            asm = context.LoadFromStream(asmStream);
                        }
                        results.Add((asm, dll));
                        Info($"[HotReload] Loaded package assembly '{Path.GetFileName(dll)}'");
                    }
                    catch (Exception ex)
                    {
                        Console.Error.WriteLine($"[HotReload] Failed to load package assembly '{dll}': {ex.Message}");
                    }
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[HotReload] Package assembly scan of '{dir}' failed: {ex.Message}");
            }
        }

        public static int PreloadAssemblyContext(string assemblyPath)
        {
            try
            {
                if (!File.Exists(assemblyPath))
                {
                    Console.WriteLine($"[HotReload] PreloadAssemblyContext: File not found: {assemblyPath}");
                    // Normalize to InvalidArg (-2) so native maps to GE_Result_InvalidArg for invalid path
                    return -2;
                }



                // Step 1: Read assembly bytes (I/O operation - done on background thread)
                byte[] assemblyBytes = File.ReadAllBytes(assemblyPath);

                // Step 2: Validate assembly bytes
                if (!ValidateAssemblyBytes(assemblyBytes, assemblyPath))
                {
                    Console.WriteLine("[HotReload] Assembly validation failed during preload");
                    return -1;
                }

                // Step 3: Create new collectible context (can be done on background thread)
                var newContext = new CollectibleAssemblyContext();

                // Step 4: Load assembly from stream (can be done on background thread)
                Assembly newAssembly;
                using (var stream = new MemoryStream(assemblyBytes))
                {
                    newAssembly = newContext.LoadFromStream(stream);
                }

                // Step 4a: Call out type-less assemblies loudly. A raced or
                // superseded compile can produce a valid ~4KB PE that defines no
                // types; swapping it in "succeeds" while every system, component
                // and [InitializeOnLoad] silently vanishes. The compile server
                // rejects that race at the source (GE0002); a 0-type assembly
                // reaching preload is still legal — a project with no scripts
                // compiles to exactly this — so warn rather than fail.
                if (CountDefinedTypes(newAssembly) == 0)
                {
                    Console.Error.WriteLine($"[HotReload] PreloadAssemblyContext: '{assemblyPath}' defines no types — nothing will run after this swap (empty project, or a stale/raced compile output)");
                }

                // Optional: preload the Editor scripts assembly into the same collectible context so:
                // - editor menu methods can be indexed/invoked
                // - InitializeOnLoad can run for editor-provided scripts
                Assembly? editorAsm = null;
                string? editorPath = null;
                try
                {
                    string baseDir = string.Empty;
                    try { baseDir = AppContext.BaseDirectory; } catch { baseDir = string.Empty; }
                    string cwd = string.Empty;
                    try { cwd = Directory.GetCurrentDirectory(); } catch { cwd = string.Empty; }
                    string procDir = string.Empty;
                    try
                    {
                        var pp = Environment.ProcessPath;
                        if (!string.IsNullOrEmpty(pp))
                            procDir = Path.GetDirectoryName(pp) ?? string.Empty;
                    }
                    catch { procDir = string.Empty; }

                    static string? TryResolveIn(string root, string fileName)
                    {
                        if (string.IsNullOrWhiteSpace(root))
                            return null;
                        try
                        {
                            var candidate = Path.Combine(root, fileName);
                            if (File.Exists(candidate))
                                return candidate;
                        }
                        catch { }
                        return null;
                    }

                    editorPath =
                        TryResolveIn(baseDir, "GameEngine.Editor.dll") ??
                        TryResolveIn(procDir, "GameEngine.Editor.dll") ??
                        TryResolveIn(cwd, "GameEngine.Editor.dll");

                    if (!string.IsNullOrEmpty(editorPath))
                    {
                        byte[] editorBytes = File.ReadAllBytes(editorPath);
                        using var s2 = new MemoryStream(editorBytes);
                        editorAsm = newContext.LoadFromStream(s2);
                    }
                }
                catch { editorAsm = null; editorPath = null; }

                // Step 4b (P1 packages): package assemblies load into the SAME collectible
                // context (whole-graph swap keeps type identity trivial). Runtime-kind
                // assemblies live in Packages/ next to the scripts assembly; Editor-kind
                // ones in Packages/Editor/ and load only when the editor scripts assembly
                // is present — mirroring how GameEngine.Editor.dll itself is handled
                // (presence-based; a Player build stages neither). ALL are loaded before
                // any type enumeration below so cross-package references resolve from the
                // context's own assembly cache.
                var packageAssemblies = new List<(Assembly Asm, string Path)>();
                try
                {
                    string scriptsDir = Path.GetDirectoryName(assemblyPath) ?? string.Empty;
                    if (!string.IsNullOrEmpty(scriptsDir))
                    {
                        string packagesDir = Path.Combine(scriptsDir, "Packages");
                        LoadPackageAssemblies(newContext, packagesDir, packageAssemblies);
                        if (editorAsm != null)
                            LoadPackageAssemblies(newContext, Path.Combine(packagesDir, "Editor"), packageAssemblies);
                    }
                }
                catch { }

                // Step 5: Precompute method indices for instant publish on swap
                var indices = BuildMethodIndicesForAssembly(newAssembly);
                void MergeIndices(Assembly extra)
                {
                    var idx2 = BuildMethodIndicesForAssembly(extra);
                    foreach (var kv in idx2.Full)
                    {
                        if (!indices.Full.ContainsKey(kv.Key))
                            indices.Full[kv.Key] = kv.Value;
                    }
                    foreach (var kv in idx2.Simple)
                    {
                        if (!indices.Simple.ContainsKey(kv.Key))
                            indices.Simple[kv.Key] = kv.Value;
                    }
                }
                if (editorAsm != null)
                    MergeIndices(editorAsm);
                foreach (var (pkgAsm, _) in packageAssemblies)
                    MergeIndices(pkgAsm);

                // Step 5b: Discover [InitializeOnLoad] methods now (background thread)
                var prebuiltInit = DiscoverInitializeOnLoadMethods(newAssembly, assemblyPath);
                if (editorAsm != null)
                {
                    try
                    {
                        var init2 = DiscoverInitializeOnLoadMethods(editorAsm, editorPath ?? "GameEngine.Editor.dll");
                        if (init2.Count > 0) prebuiltInit.AddRange(init2);
                    }
                    catch { }
                }
                foreach (var (pkgAsm, pkgPath) in packageAssemblies)
                {
                    try
                    {
                        var initP = DiscoverInitializeOnLoadMethods(pkgAsm, pkgPath);
                        if (initP.Count > 0) prebuiltInit.AddRange(initP);
                    }
                    catch { }
                }

                // Step 6: Store preloaded context, indices and prebuilt init list atomically
                lock (m_Lock)
                {
                    // Clean up any existing preloaded context
                    if (m_Preloaded != null)
                    {
                        Info("[HotReload] Cleaning up previous preloaded context");
                        try { m_Preloaded.Context.Unload(); } catch { }
                        m_Preloaded = null;
                    }

                    // Optionally prewarm delegates for faster first-invoke after swap
                    if (!string.Equals(Environment.GetEnvironmentVariable("GE_SKIP_PREWARM"), "1", StringComparison.Ordinal))
                    {
                        PrewarmDelegates(indices.Full, indices.Simple);
                    }

                    // Publish new preloaded package as a single unit
                    m_Preloaded = new PreloadResult(newContext, newAssembly, assemblyPath, indices, prebuiltInit);


                }



                return 0;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[HotReload] PreloadAssemblyContext failed: {ex.Message}");
                return -1;
            }
        }


        /// <summary>
        /// Number of types the assembly defines (partial count when some types
        /// fail to load). Zero means a stub assembly — nothing to run.
        /// </summary>
        private static int CountDefinedTypes(Assembly asm)
        {
            try { return asm.GetTypes().Length; }
            catch (ReflectionTypeLoadException ex) { return ex.Types.Count(t => t != null); }
            catch { return -1; } // unknown — never misreport as a stub
        }

	        // Discover [InitializeOnLoad] methods without invoking them (used during preload)
	        // Uses reflection first, with a metadata-only fallback (no assembly resolution required)
	        private static List<System.Reflection.MethodInfo> DiscoverInitializeOnLoadMethods(System.Reflection.Assembly asm, string? assemblyPath = null)
	        {
	            const string attrFullName = "GameEngine.Scripting.InitializeOnLoadAttribute";
	            var results = new List<System.Reflection.MethodInfo>();
	            Type[] types;
	            try { types = asm.GetTypes(); }
	            catch (ReflectionTypeLoadException ex) { types = ex.Types.Where(t => t != null)!.Cast<Type>().ToArray(); }
	            catch { types = Array.Empty<Type>(); }
	
	            try
	            {
	                var asmName = asm.GetName().Name ?? "<null>";
	                IoLTrace($"[IoL] Discover start asm='{asmName}' path='{assemblyPath ?? "<null>"}' typeCount={types.Length}");
	            }
	            catch { }
	
	            foreach (var t in types)
	            {
	                if (t == null) continue;
	                foreach (var mi in t.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static))
	                {
	                    bool hasAttr = false;
	                    try
	                    {
	                        foreach (var cad in mi.GetCustomAttributesData())
	                        {
	                            var full = cad.AttributeType?.FullName;
	                            var name = cad.AttributeType?.Name;
	                            if (string.Equals(full, attrFullName, StringComparison.Ordinal) ||
	                                string.Equals(name, "InitializeOnLoadAttribute", StringComparison.Ordinal) ||
	                                string.Equals(name, "InitializeOnLoad", StringComparison.Ordinal))
	                            { hasAttr = true; break; }
	                        }
	                    }
	                    catch
	                    {
	                        // Fallback: metadata-only attribute detection (no assembly resolution)
	                        try
	                        {
	                            if (!hasAttr && !string.IsNullOrEmpty(assemblyPath))
	                            {
	                                bool metaHas = HasInitializeOnLoadAttributeViaMetadata(assemblyPath, t.FullName ?? t.Name, mi.Name);
	                                if (metaHas) hasAttr = true;
	                            }
	                        }
	                        catch { }
	                    }
	
			            if (!hasAttr)
			            {
			                // Secondary fallback if reflection succeeded but attributes were unresolved
			                if (!string.IsNullOrEmpty(assemblyPath))
			                {
			                    try
			                    {
			                        bool metaHas2 = HasInitializeOnLoadAttributeViaMetadata(assemblyPath, t.FullName ?? t.Name, mi.Name);
			                        if (!metaHas2) continue; // skip
			                    }
			                    catch { continue; }
			                }
			                else
			                {
			                    continue;
			                }
			            }
			            results.Add(mi);
			            try
			            {
			                var qual = $"{(mi.DeclaringType?.FullName ?? mi.DeclaringType?.Name ?? "<Unknown>")}.{mi.Name}";
			                IoLTrace($"[IoL] Discover hit {qual}");
			            }
			            catch { }
			        }
		    }
	
		    try
		    {
		        var asmName = asm.GetName().Name ?? "<null>";
		        var path = assemblyPath ?? "<null>";
		        Info($"[HotReload] IoL discovery: assembly='{asmName}' path='{path}' types={types.Length} initOnLoadCount={results.Count}");
		        IoLTrace($"[IoL] Discover done asm='{asmName}' path='{path}' initOnLoadCount={results.Count}");
		    }
		    catch { }
				
			    return results;
	        }

	        // Invoke InitializeOnLoad methods, deduplicated per (assembly MVID, IoL generation)
        private static void InvokeInitializeOnLoadMethods(List<System.Reflection.MethodInfo>? prebuilt, System.Reflection.Assembly assembly)
        {
#if DEBUG
            if (!sIoLInvokeAllowed)
            {
                System.Diagnostics.Debug.Assert(false, "[HotReload] InitializeOnLoad must only run during Stage 4 (swap)");
                return;
            }
#endif
			        try
			        {
			            var methods = prebuilt ?? DiscoverInitializeOnLoadMethods(assembly, m_LastLoadedAssemblyPath);
			            var found = new List<string>();
			            int invoked = 0, skipped = 0;
			            IoLTrace($"[IoL] Invoke start gen={sCurrentIoLGen} asm='{assembly?.GetName().Name ?? "<null>"}' methodCount={methods.Count}");
		
			            foreach (var mi in methods)
			            {
			                if (mi == null) continue;
			                if (mi.GetParameters().Length != 0) { skipped++; continue; }
		
			                var mvid = mi.Module.ModuleVersionId;
			                var token = mi.MetadataToken;
			                bool shouldInvoke = false;
			                lock (m_Lock)
			                {
			                    long genKey = sCurrentIoLGen;
			                    var key = (mvid, genKey);
			                    if (!m_InvokedInitOnLoadByMvid.TryGetValue(key, out var set))
			                    {
			                        set = new HashSet<int>();
			                        m_InvokedInitOnLoadByMvid[key] = set;
			                    }
			                    if (!set.Contains(token))
			                    {
			                        set.Add(token);
			                        shouldInvoke = true;
			                    }
			                }
			                if (!shouldInvoke) { skipped++; continue; }
		
			                var qual = $"{(mi.DeclaringType?.FullName ?? mi.DeclaringType?.Name ?? "<Unknown>")}.{mi.Name}";
			                found.Add(qual);
			                try { mi.Invoke(null, null); invoked++; }
			                catch (Exception invEx) { Console.WriteLine($"[HotReload] InitializeOnLoad '{qual}' threw: {invEx.Message}"); }
			            }
		
			            try
			            {
			                var asmName = assembly?.GetName().Name ?? "<null>";
			                Info($"[HotReload] IoL invoke: assembly='{asmName}' methods={methods.Count} invoked={invoked} skipped={skipped}");
			                IoLTrace($"[IoL] Invoke done gen={sCurrentIoLGen} asm='{asmName}' invoked={invoked} skipped={skipped}");
			                if (found.Count > 0)
			                {
			                    Info("[HotReload] IoL methods: " + string.Join(", ", found));
			                    IoLTrace("[IoL] Methods: " + string.Join(", ", found));
			                }
			            }
			            catch { }
		
			            // Signal completion to waiters and raise event (no noisy Info logs)
			            var gen = sCurrentIoLGen;
			            if (gen != 0)
			            {
			                // Mark completion before signaling waiters to handle late subscribers
			                System.Threading.Volatile.Write(ref m_LatestCompletedIoLGen, gen);
			                if (s_iolWaiters.TryRemove(gen, out var tcs)) tcs.TrySetResult(true);
			                var handler = InitializeOnLoadCompleted;
			                if (handler != null) { try { handler(gen, invoked, skipped); } catch { /* swallow */ } }
			            }
		
			        }
			        catch (Exception ex)
			        {
			            Console.WriteLine($"[HotReload] InitializeOnLoad invoke failed: {ex.Message}");
			            IoLTrace($"[IoL] Invoke failed gen={sCurrentIoLGen} error='{ex.GetType().Name}: {ex.Message}'");
			        }
        }

        /// <summary>
        /// Pre-load an assembly context directly from assembly bytes (and optional PDB bytes).
        /// Heavy lifting happens here; SwapPreloadedContext performs a near-instant swap on the main thread.
        /// </summary>
        /// Returns ScriptingOpResult.Ok (0) on success; negative values map to ScriptingOpResult codes.

        public static int PreloadAssemblyContext(byte[] assemblyBytes, byte[]? pdbBytes)
        {
            try
            {
                if (assemblyBytes == null || assemblyBytes.Length == 0)
                {
                    Console.WriteLine("[HotReload] PreloadAssemblyContext(bytes): no assembly bytes provided");
                    return -1;
                }

                // Create new collectible context and load from provided bytes
                var newContext = new CollectibleAssemblyContext();
                Assembly newAssembly;
                if (pdbBytes != null && pdbBytes.Length > 0)
                {
                    using (var asmStream = new MemoryStream(assemblyBytes))
                    using (var pdbStream = new MemoryStream(pdbBytes))
                    {
                        newAssembly = newContext.LoadFromStream(asmStream, pdbStream);
                    }
                }
                else
                {
                    using (var asmStream = new MemoryStream(assemblyBytes))
                    {
                        newAssembly = newContext.LoadFromStream(asmStream);
                    }
                }

                // Call out type-less assemblies loudly (see PreloadAssemblyContext(string)).
                if (CountDefinedTypes(newAssembly) == 0)
                {
                    Console.Error.WriteLine("[HotReload] PreloadAssemblyContext(bytes): assembly defines no types — nothing will run after this swap (empty project, or a stale/raced compile output)");
                }

                // Precompute indices for instant publish on swap
                var indices = BuildMethodIndicesForAssembly(newAssembly);

                // Discover [InitializeOnLoad] methods now (background thread)
                var prebuiltInit = DiscoverInitializeOnLoadMethods(newAssembly, m_PreloadedAssemblyPath);

                // Atomically publish the preloaded context, indices, and prebuilt init list
                lock (m_Lock)
                {
                    if (m_Preloaded != null)
                    {
                        Info("[HotReload] Cleaning up previous preloaded context");
                        try { m_Preloaded.Context.Unload(); } catch { }
                        m_Preloaded = null;
                    }
                    if (!string.Equals(Environment.GetEnvironmentVariable("GE_SKIP_PREWARM"), "1", StringComparison.Ordinal))
                    {
                        PrewarmDelegates(indices.Full, indices.Simple);
                    }
                    m_Preloaded = new PreloadResult(newContext, newAssembly, null, indices, prebuiltInit);


                }



                return 0;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[HotReload] PreloadAssemblyContext(bytes) failed: {ex.Message}");
                return -1;
            }
        }


        /// <summary>
        /// OPTIMIZATION: Pre-load assembly bytes in background to enable instant swapping
        /// This method should be called from a background thread during compilation
        /// </summary>
        public static int PreloadAssemblyBytes(string assemblyPath)
        {
            try
            {
                if (!File.Exists(assemblyPath))
                {
                    Console.WriteLine($"[HotReload] PreloadAssemblyBytes: File not found: {assemblyPath}");
                    return -1;
                }



                // Read assembly bytes in background thread (no lock needed for this operation)
                byte[] assemblyBytes = File.ReadAllBytes(assemblyPath);

                // Update preloaded data atomically
                lock (m_Lock)
                {
                    m_PreloadedAssemblyBytes = assemblyBytes;
                    m_PreloadedAssemblyPath = assemblyPath;
                }


                return 0;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[HotReload] PreloadAssemblyBytes failed: {ex.Message}");
                return -1;
            }
        }

        /// <summary>
        /// OPTIMIZATION: Perform atomic swap of preloaded assembly context
        /// This is designed for main thread execution with <1ms blocking time
        /// All heavy operations (file I/O, context creation, assembly loading) are done in PreloadAssemblyContext
        /// </summary>
        /// Returns ScriptingOpResult.Ok (0) on success; negative values map to ScriptingOpResult codes.

        public static int SwapPreloadedContext()
        {
            try
            {
                // Prevent overlapping swaps; ensures single IoL scheduling
                if (System.Threading.Interlocked.CompareExchange(ref m_SwapActive, 1, 0) != 0)
                {
                    return -9; // busy
                }

                var sw = System.Diagnostics.Stopwatch.StartNew();
                int rc = 0;

                // Locals captured outside lock for post-swap work
                CollectibleAssemblyContext? oldContext = null;
                CollectibleAssemblyContext? publishedContext = null;
                ulong oldDomainId = 0;
                string? oldAssemblyName = null;
                System.Reflection.Assembly? asmRef = null;
                List<System.Reflection.MethodInfo>? toInvoke = null;
                long myGen = 0;

	                // Minimal critical section: pointer swap + bookkeeping only
	                lock (m_Lock)
	                {
#if DEBUG
                    long csStart = System.Diagnostics.Stopwatch.GetTimestamp();
#endif
	                    if (!m_IsInitialized)
	                    {
	                        var initRc = Initialize();
	                        if (initRc != 0)
	                        {
	                            IoLTrace("[IoL] SwapPreloadedContext: Initialize() failed");
	                            return -1;
	                        }
	                    }
	
	                    if (m_Preloaded == null)
	                    {
	                        IoLTrace("[IoL] SwapPreloadedContext: m_Preloaded is null (no preloaded context)");
	                        return -2;
	                    }

	                    // Store old context for cleanup. Its bookkeeping must be purged here:
	                    // the entries in m_Domains and the token/index maps hold strong references
	                    // that would otherwise keep the unloaded ALC alive forever.
	                    oldContext = m_CurrentContext;
	                    if (oldContext != null)
	                    {
	                        oldDomainId = m_CurrentDomainId;
	                        try { oldAssemblyName = m_CurrentAssembly?.GetName().Name; } catch { }
	                        if (oldDomainId != 0 && m_Domains.TryGetValue(oldDomainId, out var oldTuple) && ReferenceEquals(oldTuple.Ctx, oldContext))
	                        {
	                            m_NameToTokenByDomain.Remove(oldDomainId);
	                            m_TokenToMethodByDomain.Remove(oldDomainId);
	                            m_MethodIndexFullByDomain.Remove(oldDomainId);
	                            m_MethodIndexSimpleByDomain.Remove(oldDomainId);
	                            m_Domains.Remove(oldDomainId);
	                        }
	                    }

	                    // ATOMIC SWAP: Just pointer assignments (extremely fast)
	                    m_CurrentContext = m_Preloaded.Context;
	                    m_CurrentAssembly = m_Preloaded.Assembly;
	                    m_LastLoadedAssemblyPath = m_Preloaded.ContextPath ?? m_PreloadedAssemblyPath ?? "";

                    // Assign new domain id and register mapping
                    m_CurrentDomainId = m_NextDomainId++;
                    m_Domains[m_CurrentDomainId] = (m_CurrentContext, m_CurrentAssembly);

                    // Mark first-invoke timing pending for this new domain and reset last measurement
                    m_FirstInvokePending.Add(m_CurrentDomainId);
                    System.Threading.Interlocked.Exchange(ref m_LastFirstInvokeMs, 0);

                    // Publish prebuilt fast export indices for this domain (if available); fallback to building now
                    if (m_Preloaded.FullIndex != null && m_Preloaded.SimpleIndex != null)
                    {
                        m_MethodIndexFullByDomain[m_CurrentDomainId] = m_Preloaded.FullIndex;
                        m_MethodIndexSimpleByDomain[m_CurrentDomainId] = m_Preloaded.SimpleIndex;
                    }
                    else
                    {
                        BuildDomainMethodIndex(m_CurrentDomainId, m_CurrentAssembly);
                    }

	                    // Capture IoL list and consume it, then clear preloaded refs
	                    toInvoke = m_Preloaded.InitOnLoadMethods;
	                    asmRef = m_CurrentAssembly;
	                    publishedContext = m_CurrentContext;
	
	                    // Clear preloaded references (they're now current)
	                    m_Preloaded = null;
	
	                    // Record generation for IoL scheduling
	                    myGen = System.Threading.Interlocked.Increment(ref m_IoLEnqueueGen);
	                    System.Threading.Volatile.Write(ref m_LatestScheduledIoLGen, myGen);

	                    // Prune IoL dedup entries from older generations; dedup keys embed the
	                    // generation, so past-generation entries can never be consulted again and
	                    // previously grew by one entry set per swap forever.
	                    if (m_InvokedInitOnLoadByMvid.Count > 0)
	                    {
	                        List<(Guid Mvid, long Generation)>? stale = null;
	                        foreach (var key in m_InvokedInitOnLoadByMvid.Keys)
	                        {
	                            if (key.Generation < myGen)
	                                (stale ??= new List<(Guid, long)>()).Add(key);
	                        }
	                        if (stale != null)
	                        {
	                            foreach (var key in stale)
	                                m_InvokedInitOnLoadByMvid.Remove(key);
	                        }
	                    }
	
	                    // Pre-create a waiter to avoid race if callers await after swap returns
	                    s_iolWaiters.GetOrAdd(myGen, _ => new System.Threading.Tasks.TaskCompletionSource<bool>(System.Threading.Tasks.TaskCreationOptions.RunContinuationsAsynchronously));

#if DEBUG
                    long csEnd = System.Diagnostics.Stopwatch.GetTimestamp();
                    long us = (long)((csEnd - csStart) * 1000000.0 / System.Diagnostics.Stopwatch.Frequency);
                    System.Threading.Interlocked.Exchange(ref m_LastSwapCriticalSectionUs, us);
#endif

                    rc = 0;
                }

                // Run the new assemblies' [ModuleInitializer]s eagerly, before the old
                // context unloads: generated registrations (component schemas, system
                // registrations) become visible at publish instead of at first code
                // execution (play enter), and same-name dedup replaces the old domain's
                // entries before its purge runs.
                RunModuleConstructors(publishedContext, asmRef);

                // Unload the old context (cheap initiation) and verify collection off-thread
                if (oldContext != null)
                {
                    UnloadContext(oldContext, oldDomainId, oldAssemblyName);
                    oldContext = null;
                }

	                // After atomic swap, invoke [InitializeOnLoad] methods asynchronously outside the critical section
	
		#if DEBUG
	                DebugLogConsoleState($"IoL enqueue gen={myGen}");
		#endif
	
	                if (asmRef != null)
	                {
	                    // Ensure console redirection is installed before any InitializeOnLoad logging
	                    TryInstallConsoleRedirect();
	
	                    var genCopy = myGen;
	                    IoLTrace($"[IoL] Enqueue invoke gen={genCopy} asm='{asmRef.GetName().Name}' prebuiltCount={(toInvoke?.Count ?? 0)}");
	                    // Marshal to the engine main thread when the host registered a pump
	                    // (B5); fall back to the thread pool otherwise. Ordering guarantees
	                    // are documented on s_mainThreadWork.
	                    Action iolWork = () =>
	                    {
	                        // Only the newest scheduled invocation should run/log
	                        // Always run for the scheduled generation; scheduler ensures only one active swap
	                        // (If multiple swaps occur quickly, later generations will overwrite state and still signal completion.)
	                        try
	                        {
#if DEBUG
	                            sIoLInvokeAllowed = true;
#endif
	                            sCurrentIoLGen = genCopy;
	                            IoLTrace($"[IoL] Invoke task start gen={genCopy} asm='{asmRef.GetName().Name}'");
	                            InvokeInitializeOnLoadMethods(toInvoke, asmRef);
	                            IoLTrace($"[IoL] Invoke task end gen={genCopy} asm='{asmRef.GetName().Name}'");
	                        }
	                        finally
	                        {
#if DEBUG
	                            sIoLInvokeAllowed = false;
#endif
	                            sCurrentIoLGen = 0;
	                        }
	                    };
	                    if (!TryDispatchToMainThread(iolWork))
	                    {
	                        _ = System.Threading.Tasks.Task.Run(iolWork);
	                    }
	                }

                sw.Stop();
                System.Threading.Interlocked.Exchange(ref m_LastSwapMs, sw.ElapsedMilliseconds);
                System.Threading.Interlocked.Increment(ref m_TotalSwaps);
                return rc;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[HotReload] SwapPreloadedContext failed: {ex.Message}");
                return -1;
            }
            finally
            {
                System.Threading.Interlocked.Exchange(ref m_SwapActive, 0);
            }
        }



        /// <summary>
        /// Handle initialization of collectible context for async hot-reload
        /// </summary>
        private static int HandleInitCollectibleContext()
        {
            try
            {
                Info("[HotReload] Initializing collectible context for async hot-reload");
                // Context creation is handled in the async pipeline
                return 0;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[HotReload] HandleInitCollectibleContext failed: {ex.Message}");
                return -1;
            }
        }

        /// <summary>
        /// Handle loading assembly from stream for async hot-reload
        /// </summary>
        private static int HandleLoadAssemblyFromStream(string command)
        {
            try
            {
                Info("[HotReload] Loading assembly from stream for async pipeline");

                // Extract assembly path from command (format: "LOAD_ASSEMBLY_FROM_STREAM:path")
                string assemblyPath = "";
                if (command.Contains(':'))
                {
                    int idx = command.IndexOf(':');
                    assemblyPath = idx >= 0 && idx + 1 < command.Length ? command[(idx + 1)..] : string.Empty;
                }
                else
                {
                    Console.WriteLine("[HotReload] No assembly path provided in command");
                    return -1;
                }

                Info($"[HotReload] Preparing to load assembly: {assemblyPath}");

                // For the async pipeline, we prepare the assembly for loading but don't actually load it yet
                // The actual loading happens in the swap phase
                if (File.Exists(assemblyPath))
                {

                    return 0;
                }
                else
                {
                    Console.WriteLine($"[HotReload] Assembly file not found: {assemblyPath}");
                    return -1;
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[HotReload] HandleLoadAssemblyFromStream failed: {ex.Message}");
                return -1;
            }
        }

        /// <summary>
        /// Handle assembly context swap for async hot-reload
        /// Uses pre-loaded assembly bytes to minimize main-thread blocking
        /// </summary>
        private static int HandleSwapAssemblyContext(string command)
        {
            try
            {
                Info("[HotReload] Swapping assembly context for async pipeline");

                // Extract assembly path from command (format: "SWAP_ASSEMBLY_CONTEXT:path")
                string assemblyPath = "";
                if (command.Contains(':'))
                {
                    int idx = command.IndexOf(':');
                    assemblyPath = idx >= 0 && idx + 1 < command.Length ? command[(idx + 1)..] : string.Empty;
                }
                else
                {
                    Console.WriteLine("[HotReload] No assembly path provided in command");
                    return -1;
                }

                Info($"[HotReload] Performing assembly swap: {assemblyPath}");

                // Unified path: preload + atomic swap
                if (m_PreloadedAssemblyBytes != null && m_PreloadedAssemblyPath == assemblyPath)
                {

                    var pre = PreloadAssemblyContext(m_PreloadedAssemblyBytes, null);
                    if (pre != 0) return pre;
                    return SwapPreloadedContext();
                }
                else
                {

                    var pre = PreloadAssemblyContext(assemblyPath);
                    if (pre != 0) return pre;
                    return SwapPreloadedContext();
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[HotReload] HandleSwapAssemblyContext failed: {ex.Message}");
                return -1;
            }
        }

        /// <summary>
        /// Handle cleanup of old context for async hot-reload
        /// </summary>
        private static int HandleCleanupOldContext(string assemblyPath)
        {
            try
            {
                Info($"[HotReload] Cleaning up old context: {assemblyPath}");
                // Context cleanup is handled in the async pipeline
                return 0;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[HotReload] HandleCleanupOldContext failed: {ex.Message}");
                return -1;
            }
        }

        /// <summary>
        /// Async version of ReloadUserScriptsAssembly for async hot-reload pipeline
        /// </summary>
        public static async Task<int> ReloadUserScriptsAssemblyAsync(string assemblyPath, CancellationToken cancellationToken)
        {
            try
            {
                Info($"[HotReload] Starting async reload for: {assemblyPath}");

                // Run the reload operation in a background task
                return await Task.Run(() =>
                {
                    try
                    {
                        cancellationToken.ThrowIfCancellationRequested();

                        // Use the existing synchronous reload method
                        int result = LoadUserScriptsAssembly(assemblyPath);

                        if (result == 0)
                        {

                        }
                        else
                        {
                            Console.WriteLine($"[HotReload] Async reload failed with result: {result}");
                        }

                        return result;
                    }
                    catch (OperationCanceledException)
                    {

                        return -1;
                    }
                    catch (Exception ex)
                    {
                        Console.WriteLine($"[HotReload] Async reload exception: {ex.Message}");
                        return -1;
                    }
                }, cancellationToken);
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[HotReload] ReloadUserScriptsAssemblyAsync failed: {ex.Message}");
                return -1;
            }
        }

        /// <summary>
        /// Initialize a new collectible AssemblyLoadContext for hot-reload
        /// Called by async hot-reload pipeline Stage 3
        /// </summary>
        [UnmanagedCallersOnly]
        public static int InitializeCollectibleContext()
        {
            try
            {
                Info("[HotReload] Initializing new collectible AssemblyLoadContext");

                // In a full implementation, this would create a new collectible context
                // For now, we'll just prepare for the upcoming assembly load

                return 0;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[HotReload] Failed to initialize collectible context: {ex.Message}");
                return -1;
            }
        }

        /// <summary>
        /// Cleanup old collectible AssemblyLoadContext after hot-reload
        /// Called by async hot-reload pipeline cleanup
        /// </summary>
        [UnmanagedCallersOnly]
        public static int CleanupOldContext()
        {
            try
            {
                // Old contexts are unloaded and their collection verified asynchronously by
                // SwapPreloadedContext/UnloadDomain; no blocking GC is needed here.
                Info("[HotReload] CleanupOldContext: old context cleanup is handled by unload verification");
                return 0;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[HotReload] Failed to cleanup old context: {ex.Message}");
                return -1;
            }
        }

        /// <summary>
        /// Load compiled assembly bytes into the hot reload system
        /// </summary>
        /// Returns ScriptingOpResult.Ok (0) on success; negative values map to ScriptingOpResult codes.

        // Temporarily public for CoreBridge reflection in tests; can be internal but flagged NonPublic via reflection
        public static int LoadCompiledAssembly(byte[] assemblyBytes)
        {
            try
            {
                if (assemblyBytes == null || assemblyBytes.Length == 0)
                {
                    Console.WriteLine("[HotReload] No assembly bytes to load");
                    return -1;
                }

                Info($"[HotReload] Loading compiled assembly: {assemblyBytes.Length} bytes");

                CollectibleAssemblyContext newContext;
                Assembly newAssembly;
                ulong newDomainId;
                lock (m_Lock)
                {
                    // Multi-domain: do not unload previous assembly here. Each call creates a new collectible context/domain.
                    // Previous domains remain active until explicitly unloaded by the engine.

                    // Create new collectible context
                    newContext = new CollectibleAssemblyContext();

                    // Load assembly from bytes
                    using (var stream = new MemoryStream(assemblyBytes))
                    {
                        newAssembly = newContext.LoadFromStream(stream);
                    }

                    m_CurrentContext = newContext;
                    m_CurrentAssembly = newAssembly;
                    m_CurrentDomainId = m_NextDomainId++;
                    newDomainId = m_CurrentDomainId;
                    m_Domains[m_CurrentDomainId] = (m_CurrentContext, m_CurrentAssembly);
                    Info($"[HotReload] CurrentDomainId={m_CurrentDomainId}");
                    // Build fast export indices for this domain
                    BuildDomainMethodIndex(m_CurrentDomainId, m_CurrentAssembly);
                }

                // Eagerly run [ModuleInitializer]s so generated registrations are
                // visible immediately (user code — outside m_Lock).
                RunModuleConstructors(newContext, newAssembly);

                // Return the domain id as positive int for interop callers
                return newDomainId <= int.MaxValue ? (int)newDomainId : int.MaxValue;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[HotReload] LoadCompiledAssembly failed: {ex.Message}");
                return -1;
            }
        }

        /// <summary>
        /// Validate assembly bytes before loading to prevent crashes
        /// </summary>
        private static bool ValidateAssemblyBytes(byte[] assemblyBytes, string assemblyPath)
        {
            try
            {
                // Basic validation
                if (assemblyBytes == null || assemblyBytes.Length == 0)
                {
                    Console.WriteLine("[HotReload] Assembly bytes are null or empty");
                    return false;
                }

                // Check minimum size for .NET assembly
                if (assemblyBytes.Length < 64)
                {
                    Console.WriteLine($"[HotReload] Assembly too small ({assemblyBytes.Length} bytes) - likely corrupted");
                    return false;
                }

                // Check PE header (DOS signature)
                if (assemblyBytes[0] != 0x4D || assemblyBytes[1] != 0x5A) // "MZ"
                {
                    Console.WriteLine("[HotReload] Invalid PE header - not a valid .NET assembly");
                    return false;
                }

                // Check file size matches expected
                var fileInfo = new FileInfo(assemblyPath);
                if (fileInfo.Length != assemblyBytes.Length)
                {
                    Console.WriteLine($"[HotReload] File size mismatch - Expected: {fileInfo.Length}, Got: {assemblyBytes.Length}");
                    return false;
                }

                // Robust: do not reject small assemblies by size alone; rely on explicit ReferenceAssembly marker detection below
                var scanLen = Math.Min(assemblyBytes.Length, 8192);
                try
                {
                    var head = System.Text.Encoding.ASCII.GetString(assemblyBytes, 0, scanLen);
                    if (head.Contains("ReferenceAssembly", StringComparison.Ordinal))
                    {
                        Console.WriteLine("[HotReload] Detected 'ReferenceAssembly' marker - implementation assembly expected");
                        return false;
                    }
                }
                catch { /* best-effort */ }


                return true;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[HotReload] Assembly validation failed: {ex.Message}");
                return false;
            }
        }

        /// <summary>
        /// Handle test command for hot reload testing
        /// </summary>
        private static int HandleTestCommand(string command)
        {
            try
            {
                Info("[HotReload] Test command executed");
                return 42; // Magic test number
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[HotReload] Test command failed: {ex.Message}");
                return -1;
            }
        }



        /// <summary>
        /// Handle test command after reload for hot reload testing
        /// </summary>
        private static int HandleTestCommandAfterReload(string command)
        {
            try
            {
                Info("[HotReload] Test command after reload executed");
                return 84; // Magic test number (42 * 2)
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[HotReload] Test command after reload failed: {ex.Message}");
                return -1;
            }
        }

        /// <summary>
        /// Call a specific method in a specific class in the user scripts assembly
        /// </summary>
        private static int CallMethodInUserScripts(string className, string methodName)
        {
            try
            {
                // Snapshot the context under lock; reflection and invocation outside (B3).
                CollectibleAssemblyContext? context;
                lock (m_Lock)
                {
                    context = m_CurrentContext;
                }
                if (context == null)
                {
                    Console.WriteLine("[HotReload] No user scripts assembly loaded");
                    return -1;
                }

                // Find the first assembly in the context (typically only one)
                Assembly? assembly = null;
                foreach (var a in context.Assemblies) { assembly = a; break; }
                if (assembly == null)
                {
                    Console.WriteLine("[HotReload] No assemblies found in user scripts context");
                    return -1;
                }

                // Find the class and method
                var type = assembly.GetType($"GameEngine.Scripts.{className}");
                if (type == null)
                {
                    Console.WriteLine($"[HotReload] Class not found: GameEngine.Scripts.{className}");
                    return -1;
                }

                var method = type.GetMethod(methodName, BindingFlags.Public | BindingFlags.Static);
                if (method == null)
                {
                    Console.WriteLine($"[HotReload] Method not found: {methodName}");
                    return -1;
                }

                if (method.GetParameters().Length != 0)
                {
                    Console.WriteLine($"[HotReload] Unsupported method signature for: {methodName}");
                    return -1;
                }

                var result = method.Invoke(null, null);
                if (result is int intResult) return intResult;
                return 0;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[HotReload] CallMethodInUserScripts failed: {ex.Message}");
                return -1;
            }
        }

        // -------------------------------------------------------------------
        // Shared assembly pre-loading
        // -------------------------------------------------------------------

        /// <summary>
        /// Assemblies whose types must have a single identity across all ALCs.
        /// Pre-loaded into AssemblyLoadContext.Default at startup so that
        /// CollectibleAssemblyContext.Load() can simply return null for them.
        /// </summary>
        private static readonly string[] s_SharedAssemblyNames =
        {
            "GameEngine.Scripting.ABI",
            "GameEngine.ECS.ABI",
            "GameEngine.Input.ABI",
            "GameEngine.Physics.ABI",
            "GameEngine.Platform.ABI",
            "GameEngine.Editor.Scripting.ABI",
            "GameEngine.Scripting.Runtime",
            "GameEngine.CoreBridge",
            "GameEngine.HotReload",
        };

        /// <summary>
        /// Pre-loads all engine-shared assemblies into the default ALC so that collectible
        /// ALCs resolve them from Default (via returning null from Load). This guarantees
        /// type identity for GameSystem, IComponent, GameSystemRunner, etc. across ALCs.
        /// Called once from Initialize(), before any collectible context is created.
        /// </summary>
        private static void EnsureSharedAssembliesInDefault()
        {
            // Probe order matters: this assembly's own directory IS the staged managed
            // directory wherever the platform puts it (Contents/Resources/Managed in a
            // macOS bundle, next to the exe on Windows). Environment.ProcessPath points
            // at the host executable's directory, which on macOS contains no managed
            // DLLs — probing it first used to make this preload a silent no-op there.
            var probeDirs = new List<string>();
            void AddProbeDir(Func<string?> resolve)
            {
                try
                {
                    var dir = resolve();
                    if (!string.IsNullOrWhiteSpace(dir) && !probeDirs.Contains(dir!, StringComparer.Ordinal))
                        probeDirs.Add(dir!);
                }
                catch { }
            }
            AddProbeDir(static () => Path.GetDirectoryName(typeof(HotReloadManager).Assembly.Location));
            AddProbeDir(static () => Path.GetDirectoryName(Environment.ProcessPath));
            AddProbeDir(static () => AppContext.BaseDirectory);
            AddProbeDir(static () => Directory.GetCurrentDirectory());

            if (probeDirs.Count == 0)
            {
                Console.Error.WriteLine("[HotReload] EnsureSharedAssembliesInDefault: could not determine any probe directory");
                return;
            }

            var alreadyLoaded = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var asm in AssemblyLoadContext.Default.Assemblies)
            {
                var n = asm.GetName().Name;
                if (!string.IsNullOrEmpty(n))
                    alreadyLoaded.Add(n);
            }

            List<string>? missing = null;
            foreach (var simpleName in s_SharedAssemblyNames)
            {
                if (alreadyLoaded.Contains(simpleName))
                    continue;

                string? dllPath = null;
                foreach (var dir in probeDirs)
                {
                    var candidate = Path.Combine(dir, simpleName + ".dll");
                    if (File.Exists(candidate)) { dllPath = candidate; break; }
                }
                if (dllPath == null)
                {
                    (missing ??= new List<string>()).Add(simpleName);
                    continue;
                }

                try
                {
                    AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.GetFullPath(dllPath));
                }
                catch (Exception ex)
                {
                    // InvalidOperationException if already loaded from a different path — benign.
                    Console.WriteLine($"[HotReload] EnsureSharedAssembliesInDefault: {simpleName}: {ex.GetType().Name}: {ex.Message}");
                }
            }

            // Loading an assembly is not running it. A module initializer is guaranteed only to
            // run at-or-before first ACCESS to one of the module's members, so a shared assembly
            // that is loaded here and then never touched has not initialized — and these are
            // precisely the assemblies nothing is obliged to touch, because they exist to be
            // referenced by user scripts rather than called by the host. RunModuleConstructors
            // gives collectible user assemblies this same treatment at publish; this is the
            // Default-ALC half of it, and without it "eagerly loaded for type identity" stops
            // one step short of the initialization that makes the load useful.
            foreach (var asm in AssemblyLoadContext.Default.Assemblies)
            {
                string? name = null;
                try { name = asm.GetName().Name; } catch { }
                if (name == null || Array.IndexOf(s_SharedAssemblyNames, name) < 0)
                    continue;

                try
                {
                    foreach (var module in asm.GetModules())
                        System.Runtime.CompilerServices.RuntimeHelpers.RunModuleConstructor(module.ModuleHandle);
                }
                catch (Exception ex)
                {
                    // A throwing module initializer must not take the host down with it: the
                    // assembly stays loaded and merely uninitialized, which is the same state
                    // this method produced before it forced anything.
                    Console.WriteLine($"[HotReload] EnsureSharedAssembliesInDefault: module initializer for {name} failed: {ex.GetType().Name}: {ex.Message}");
                }
            }

            if (missing != null)
            {
                // Not necessarily fatal — CoreBridge's Default-ALC Resolving handler can
                // still resolve these lazily — but a miss here means the eager type-identity
                // guarantee is running on its fallback, which is worth a visible line.
                Console.WriteLine(
                    $"[HotReload] EnsureSharedAssembliesInDefault: not found in any probe dir ({string.Join(", ", probeDirs)}): {string.Join(", ", missing)}");
            }
        }
    }

    /// <summary>
    /// Collectible AssemblyLoadContext for user scripts hot-reload
    /// This context can be created and disposed safely while the core manager remains loaded
    /// </summary>
    public class CollectibleAssemblyContext : AssemblyLoadContext
    {
        public CollectibleAssemblyContext() : base(isCollectible: true)
        {
            try { if (string.Equals(Environment.GetEnvironmentVariable("GE_HOTRELOAD_VERBOSE"), "1", StringComparison.Ordinal)) Console.WriteLine("[HotReload] Created collectible AssemblyLoadContext for user scripts"); } catch { }
        }

        protected override Assembly? Load(AssemblyName assemblyName)
        {
            // All engine-shared assemblies (ABI modules, CoreBridge, Scripting.Runtime,
            // HotReload) are pre-loaded into AssemblyLoadContext.Default by
            // HotReloadManager.EnsureSharedAssembliesInDefault() at startup.
            //
            // Returning null causes the runtime to fall back to Default, giving a single
            // shared type identity (GameSystem, IComponent, etc.) across all collectible
            // contexts.  This is essential for IsSubclassOf, casting, and [ModuleInitializer]
            // static state to work correctly between engine code and user scripts.
            //
            // We intentionally do NOT probe the filesystem here.  Loading arbitrary DLLs
            // into this collectible ALC was the root cause of type duplication bugs.
            // User script dependencies are resolved transitively when the scripts DLL is
            // loaded via LoadFromStream; framework assemblies resolve from Default via TPA.
            return null;
        }
    }
}
