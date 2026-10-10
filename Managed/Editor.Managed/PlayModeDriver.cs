using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using GameEngine.ECS;
using GameEngine.Scripting.Runtime;

namespace GameEngine.Editor.Managed
{
    /// <summary>
    /// Editor-only Play Mode hook discovery and dispatch.
    /// Called from native via cached unmanaged-callable exports (see <see cref="PlayModeDriverExports"/>).
    /// </summary>
    public static class PlayModeDriver
    {
        private const string EnterAttr = "GameEngine.Scripting.PlayModeEnterAttribute";
        private const string ExitAttr = "GameEngine.Scripting.PlayModeExitAttribute";
        private const string TickAttr = "GameEngine.Scripting.PlayModeTickAttribute";

        private static readonly object s_lock = new();
        private static bool s_built;
        private static ulong s_builtForDomainId;

        private static Func<int>[] s_enters = Array.Empty<Func<int>>();
        private static Func<int>[] s_exits = Array.Empty<Func<int>>();
        private static Func<float, int>[] s_ticks = Array.Empty<Func<float, int>>();

        // Cached delegate to HotReloadManager.GetCurrentDomainId() for
        // detecting assembly swaps without per-frame reflection overhead.
        private static Func<ulong>? s_getDomainIdFunc;
        private static bool s_getDomainIdResolved;

        /// <summary>
        /// Returns the currently active user assemblies, avoiding stale collectible ALCs
        /// that are pending GC after hot-reload. Uses HotReloadManager via reflection
        /// (Editor.Managed cannot directly reference HotReload). Returns null if the
        /// reflection fails, in which case callers should fall back to
        /// AppDomain.CurrentDomain.GetAssemblies() with dedup.
        /// </summary>
        private static Assembly[]? TryGetActiveUserAssemblies()
        {
            try
            {
                var hrAsm = AppDomain.CurrentDomain.GetAssemblies()
                    .FirstOrDefault(a => a.GetName().Name == "GameEngine.HotReload");
                if (hrAsm != null)
                {
                    var method = hrAsm.GetType("GameEngine.HotReload.HotReloadManager")
                        ?.GetMethod("GetActiveUserAssemblies", BindingFlags.Static | BindingFlags.Public);
                    return method?.Invoke(null, null) as Assembly[];
                }
            }
            catch { /* reflection failed */ }
            return null;
        }

        public static int OnEnter()
        {
            // Failures here are non-fatal (play mode must still enter), but never
            // silent: a swallowed throw below means every C# GameSystem stays dead
            // for the whole Play session with no visible cause.
            try { EnsureBuilt(); }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[PlayModeDriver] OnEnter: hook discovery failed: {ex.GetBaseException()}");
            }

            try
            {
                // Generated [ModuleInitializer] registrations already ran eagerly at
                // load/swap (HotReloadManager) and land in this same GameSystemRunner:
                // the native host binds all bridge assemblies into the Default ALC, so
                // there is a single statics universe (B4) and no re-discovery is needed.
                var worldHandle = Ecs.PrimaryWorld.Handle;
                GameSystemRunner.Initialize(worldHandle);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(
                    $"[PlayModeDriver] OnEnter: GameSystemRunner.Initialize failed — no C# GameSystem will run this Play session: {ex.GetBaseException()}");
            }

            foreach (var fn in s_enters)
            {
                try
                {
                    int rc = fn();
                    if (rc != 0) return rc;
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine($"[PlayModeDriver] Enter hook threw: {ex.GetBaseException()}");
                }
            }
            return 0;
        }

        public static int OnExit()
        {
            try
            {
                EnsureBuilt();

                try
                {
                    GameSystemRunner.Shutdown();
                }
                catch (Exception ex)
                {
                    // GameSystemRunner failure must not prevent play mode hooks from running.
                    Console.Error.WriteLine($"[PlayModeDriver] OnExit: GameSystemRunner.Shutdown failed: {ex.GetBaseException()}");
                }

                foreach (var fn in s_exits)
                {
                    try
                    {
                        int rc = fn();
                        if (rc != 0) return rc;
                    }
                    catch (Exception ex)
                    {
                        Console.Error.WriteLine(
                            $"[PlayModeDriver] Exit hook threw: {ex.GetType().Name}: {ex.Message}");
                    }
                }
                lock (s_lock)
                {
                    s_built = false;
                    s_enters = Array.Empty<Func<int>>();
                    s_exits = Array.Empty<Func<int>>();
                    s_ticks = Array.Empty<Func<float, int>>();
                }
                return 0;
            }
            catch { return -1; }
        }

        public static int Tick(float deltaSeconds)
        {
            try
            {
                try { EnsureBuilt(); }
                catch (Exception ex) { LogTickError("hook discovery", ex); }

                try { GameSystemRunner.Tick(deltaSeconds); }
                catch (Exception ex) { LogTickError("GameSystemRunner.Tick", ex); }

                foreach (var fn in s_ticks)
                {
                    try
                    {
                        int rc = fn(deltaSeconds);
                        if (rc != 0) return rc;
                    }
                    catch (Exception ex) { LogTickError("tick hook", ex); }
                }
                return 0;
            }
            catch (Exception ex)
            {
                // Log the first few failures so they can be diagnosed.
                if (s_tickErrorCount < 3)
                {
                    s_tickErrorCount++;
                    var msg = $"[PlayModeDriver] Tick exception #{s_tickErrorCount}: {ex.GetType().Name}: {ex.Message}";
                    if (ex.InnerException != null)
                        msg += $"\n  Inner: {ex.InnerException.GetType().Name}: {ex.InnerException.Message}";
                    msg += $"\n  Stack: {ex.StackTrace}";
                    // Write to both stderr and stdout to maximize visibility
                    Console.Error.WriteLine(msg);
                    Console.WriteLine(msg);
                    System.Diagnostics.Trace.WriteLine(msg);
                }
                return -1;
            }
        }
        private static int s_tickErrorCount;

        /// <summary>
        /// Per-frame failure diagnostics, capped so a persistently broken hook cannot
        /// flood the log: the first few occurrences carry the full exception.
        /// </summary>
        private static void LogTickError(string stage, Exception ex)
        {
            if (s_tickErrorCount >= 3)
                return;
            s_tickErrorCount++;
            Console.Error.WriteLine(
                $"[PlayModeDriver] Tick {stage} failed (#{s_tickErrorCount}, further failures suppressed at 3): {ex.GetBaseException()}");
        }

        /// <summary>
        /// Returns the current HotReloadManager domain ID via a cached delegate.
        /// The domain ID increments on each assembly swap, allowing detection of
        /// hot-reloads without expensive per-frame reflection.
        /// </summary>
        private static ulong GetDomainId()
        {
            if (!s_getDomainIdResolved)
            {
                try
                {
                    var hrAsm = AppDomain.CurrentDomain.GetAssemblies()
                        .FirstOrDefault(a => a.GetName().Name == "GameEngine.HotReload");
                    var method = hrAsm?.GetType("GameEngine.HotReload.HotReloadManager")
                        ?.GetMethod("GetCurrentDomainId", BindingFlags.Static | BindingFlags.Public);
                    if (method != null)
                    {
                        s_getDomainIdFunc = (Func<ulong>)Delegate.CreateDelegate(typeof(Func<ulong>), method);
                        s_getDomainIdResolved = true;
                    }
                }
                catch { }
            }
            return s_getDomainIdFunc?.Invoke() ?? 0;
        }

        private static void EnsureBuilt()
        {
            lock (s_lock)
            {
                ulong domainId = GetDomainId();
                bool domainChanged = s_built && s_builtForDomainId != 0
                    && domainId != 0 && domainId != s_builtForDomainId;

                if (s_built && !domainChanged) return;

                if (domainChanged)
                {
                    // Hot-reload during play mode. The runner already absorbed the
                    // swap: the new domain's [ModuleInitializer] registrations ran
                    // eagerly at swap publish (HotReloadManager) and hot-added fresh
                    // instances into this same GameSystemRunner (single statics
                    // universe, B4), while the unloading ALC's entries were purged.
                    // A Shutdown+Initialize here would create and OnCreate a second
                    // instance of every system on the same publish — reconcile only
                    // picks up reflection-discovered systems the generated
                    // registrations don't cover.
                    try
                    {
                        GameSystemRunner.ReconcileAfterDomainSwap();
                    }
                    catch (Exception)
                    {
                        // GameSystemRunner failure must not prevent hook rebuild.
                    }
                }

                BuildHooks();
                s_builtForDomainId = domainId;
                s_built = true;
            }
        }

        private static void BuildHooks()
        {
            // Last-wins dictionaries: NEWER assemblies (iterated later) overwrite hooks
            // from OLDER stale assemblies still pending GC after hot-reload.
            var enterMap = new Dictionary<string, Func<int>>(StringComparer.Ordinal);
            var exitMap = new Dictionary<string, Func<int>>(StringComparer.Ordinal);
            var tickMap = new Dictionary<string, Func<float, int>>(StringComparer.Ordinal);

            static bool IsCandidateAssembly(Assembly a)
            {
                try
                {
                    var name = a.GetName().Name ?? "";
                    if (name.StartsWith("System.", StringComparison.Ordinal)) return false;
                    if (name.StartsWith("Microsoft.", StringComparison.Ordinal)) return false;
                    if (name.StartsWith("netstandard", StringComparison.Ordinal)) return false;
                    if (name.StartsWith("mscorlib", StringComparison.Ordinal)) return false;

                    // Skip engine/editor infrastructure assemblies.
                    if (name.StartsWith("GameEngine.CoreBridge", StringComparison.Ordinal)) return false;
                    if (name.StartsWith("GameEngine.Editor.Managed", StringComparison.Ordinal)) return false;
                    if (name.StartsWith("GameEngine.HotReload", StringComparison.Ordinal)) return false;
                    if (name.StartsWith("GameEngine.Scripting.Runtime", StringComparison.Ordinal)) return false;
                    if (name.EndsWith(".ABI", StringComparison.Ordinal)) return false;

                    return true;
                }
                catch { return false; }
            }

            // Primary path: use HotReloadManager.GetActiveUserAssemblies() which returns
            // only the currently live assemblies (no stale ALCs pending GC).
            var activeAsms = TryGetActiveUserAssemblies();

            // Build the candidate list. When active user assemblies are available,
            // use them (latest versions, no stale ALCs) and also include any other
            // AppDomain assemblies not in the active set (e.g., Editor hooks, tools).
            var candidates = new List<Assembly>();
            if (activeAsms != null && activeAsms.Length > 0)
            {
                var activeNames = new HashSet<string>(StringComparer.Ordinal);
                foreach (var a in activeAsms)
                {
                    if (a != null && IsCandidateAssembly(a))
                    {
                        candidates.Add(a);
                        activeNames.Add(a.GetName().Name ?? "");
                    }
                }
                // Include non-overlapping AppDomain assemblies so hooks in
                // non-script assemblies (Editor, tools) are still discovered.
                foreach (var a in AppDomain.CurrentDomain.GetAssemblies())
                {
                    if (a == null || !IsCandidateAssembly(a)) continue;
                    var name = a.GetName().Name ?? "";
                    if (!activeNames.Contains(name))
                        candidates.Add(a);
                }
            }
            else
            {
                // Fallback: sort so Editor comes before Scripts (Scripts overwrites via
                // last-wins), and within the same name newest (by load order) comes last.
                var allAsms = AppDomain.CurrentDomain.GetAssemblies();
                var indexed = new List<(Assembly asm, int order)>();
                for (int i = 0; i < allAsms.Length; i++)
                {
                    var a = allAsms[i];
                    if (a != null && IsCandidateAssembly(a))
                        indexed.Add((a, i));
                }
                indexed.Sort((a, b) =>
                {
                    bool aIsScripts = (a.asm.GetName().Name ?? "") == "GameEngine.Scripts";
                    bool bIsScripts = (b.asm.GetName().Name ?? "") == "GameEngine.Scripts";
                    if (aIsScripts != bIsScripts)
                        return aIsScripts ? 1 : -1;
                    return a.order.CompareTo(b.order);
                });
                foreach (var (a, _) in indexed)
                    candidates.Add(a);
            }

            foreach (var asm in candidates)
            {
                Type[] types;
                try { types = asm.GetTypes(); }
                catch (ReflectionTypeLoadException e)
                {
                    var src = e.Types ?? Array.Empty<Type?>();
                    var tmp = new List<Type>(src.Length);
                    for (int i = 0; i < src.Length; i++)
                    {
                        var tt = src[i];
                        if (tt != null) tmp.Add(tt);
                    }
                    types = tmp.ToArray();
                }
                catch { continue; }

                foreach (var t in types)
                {
                    if (t == null) continue;

                    MethodInfo[] methods;
                    try { methods = t.GetMethods(BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic); }
                    catch { continue; }

                    foreach (var m in methods)
                    {
                        if (m == null) continue;

                        bool hasEnter = HasAttrByName(m, EnterAttr);
                        bool hasExit = HasAttrByName(m, ExitAttr);
                        bool hasTick = HasAttrByName(m, TickAttr);

                        if (!hasEnter && !hasExit && !hasTick) continue;

                        var methodKey = $"{t.FullName ?? t.Name}::{m.Name}";

                        try
                        {
                            var pars = m.GetParameters();

                            if (hasEnter && pars.Length == 0)
                            {
                                if (m.ReturnType == typeof(void))
                                {
                                    var d = (Action)m.CreateDelegate(typeof(Action));
                                    enterMap[methodKey] = () => { d(); return 0; };
                                }
                                else if (m.ReturnType == typeof(int))
                                {
                                    enterMap[methodKey] = (Func<int>)m.CreateDelegate(typeof(Func<int>));
                                }
                            }

                            if (hasExit && pars.Length == 0)
                            {
                                if (m.ReturnType == typeof(void))
                                {
                                    var d = (Action)m.CreateDelegate(typeof(Action));
                                    exitMap[methodKey] = () => { d(); return 0; };
                                }
                                else if (m.ReturnType == typeof(int))
                                {
                                    exitMap[methodKey] = (Func<int>)m.CreateDelegate(typeof(Func<int>));
                                }
                            }

                            if (hasTick && pars.Length == 1 && pars[0].ParameterType == typeof(float))
                            {
                                if (m.ReturnType == typeof(void))
                                {
                                    var d = (Action<float>)m.CreateDelegate(typeof(Action<float>));
                                    tickMap[methodKey] = (dt) => { d(dt); return 0; };
                                }
                                else if (m.ReturnType == typeof(int))
                                {
                                    tickMap[methodKey] = (Func<float, int>)m.CreateDelegate(typeof(Func<float, int>));
                                }
                            }
                        }
                        catch
                        {
                            // Ignore invalid signatures/binding failures; keep play mode robust.
                        }
                    }
                }
            }

            s_enters = enterMap.Values.ToArray();
            s_exits = exitMap.Values.ToArray();
            s_ticks = tickMap.Values.ToArray();
        }

        private static bool HasAttrByName(MethodInfo m, string fullName)
        {
            try
            {
                foreach (var cad in m.CustomAttributes)
                {
                    var t = cad.AttributeType;
                    if (t != null && string.Equals(t.FullName, fullName, StringComparison.Ordinal))
                        return true;
                }
            }
            catch { }
            return false;
        }
    }
}

