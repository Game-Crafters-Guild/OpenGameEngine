using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;
using System.Threading;
using GameEngine.Editor.Scripting;

namespace GameEngine.Editor.Managed
{
    /// <summary>
    /// Editor-only bridge that discovers script-defined editor menu items and syncs them to native.
    /// </summary>
    public static unsafe class EditorMenuBridge
    {
        private enum MenuKind : uint
        {
            Toolbar = 0,
            Context = 1,
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct EditorMenuItemNative
        {
            public ulong domainId;
            public uint kind;        // MenuKind
            public uint targetsMask; // EditorContextMenuTarget (0 for toolbar)
            public int priority;

            public byte* pathUtf8;
            public uint pathLen;

            public byte* methodUtf8;
            public uint methodLen;
        }

        private readonly struct DiscoveredItem
        {
            public DiscoveredItem(ulong domainId,
                                  MenuKind kind,
                                  EditorContextMenuTarget targets,
                                  int priority,
                                  string path,
                                  string method)
            {
                DomainId = domainId;
                Kind = kind;
                Targets = targets;
                Priority = priority;
                Path = path;
                Method = method;
            }

            public ulong DomainId { get; }
            public MenuKind Kind { get; }
            public EditorContextMenuTarget Targets { get; }
            public int Priority { get; }
            public string Path { get; }
            public string Method { get; }
        }

        // Native callback: void(EditorMenuItemNative* items, uint count, void* userData)
        private static nint s_callback;
        private static nint s_userData;
        private static long s_lastSeenIoLGen;

        private static readonly object s_refreshLock = new object();
        private static int s_refreshRunning;
        private static int s_refreshPending;

        private const string kToolbarAttrFullName = "GameEngine.Editor.Scripting.EditorToolbarMenuAttribute";
        private const string kToolbarAliasAttrFullName = "GameEngine.Editor.Scripting.EditorToolbarItemAttribute";
        private const string kContextAttrFullName = "GameEngine.Editor.Scripting.EditorContextMenuAttribute";

        private sealed class HotReloadBinding
        {
            public HotReloadBinding(Type hrmType,
                                    MethodInfo getCurrentAsm,
                                    MethodInfo getCurrentDomain,
                                    EventInfo iolEvent,
                                    Delegate handler)
            {
                HrmType = hrmType;
                GetCurrentAssembly = getCurrentAsm;
                GetCurrentDomainId = getCurrentDomain;
                IoLEvent = iolEvent;
                Handler = handler;
            }

            public Type HrmType { get; }
            public MethodInfo GetCurrentAssembly { get; }
            public MethodInfo GetCurrentDomainId { get; }
            public EventInfo IoLEvent { get; }
            public Delegate Handler { get; }
        }

        private static readonly object s_hrmLock = new object();
        private static readonly List<HotReloadBinding> s_hrmBindings = new List<HotReloadBinding>(4);

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int RegisterMenuSnapshotCallback(nint callbackFn, nint userData)
        {
            try
            {
                if (callbackFn == 0)
                    return -2;

                s_callback = callbackFn;
                s_userData = userData;

                EnsureHotReloadBindings();

                try { Console.WriteLine($"[EditorMenuBridge] Registered callback fn=0x{callbackFn.ToString("X")} user=0x{userData.ToString("X")}"); }
                catch { }

                // Always do an immediate refresh to handle the case where the IoL event fired
                // before the Editor registered this bridge.
                QueueRefresh();
                return 0;
            }
            catch
            {
                return -1;
            }
        }

        private static void OnInitializeOnLoadCompleted(long generation, int invokedCount, int skippedCount)
        {
            _ = invokedCount;
            _ = skippedCount;

            try { Console.WriteLine($"[EditorMenuBridge] IoL completed gen={generation} invoked={invokedCount} skipped={skippedCount}"); } catch { }

            // Ignore older generations (best-effort).
            long prev = Interlocked.Read(ref s_lastSeenIoLGen);
            if (generation > 0 && generation <= prev)
                return;
            if (generation > 0)
                Interlocked.Exchange(ref s_lastSeenIoLGen, generation);

            QueueRefresh();
        }

        private static void EnsureHotReloadBindings()
        {
            lock (s_hrmLock)
            {
                // Attempt to bind to every loaded GameEngine.HotReload instance.
                var assemblies = AppDomain.CurrentDomain.GetAssemblies();
                foreach (var asm in assemblies)
                {
                    AssemblyName? an = null;
                    try { an = asm.GetName(); } catch { }
                    if (an == null || !string.Equals(an.Name, "GameEngine.HotReload", StringComparison.OrdinalIgnoreCase))
                        continue;

                    Type? hrmType = null;
                    try { hrmType = asm.GetType("GameEngine.HotReload.HotReloadManager", throwOnError: false, ignoreCase: false); } catch { }
                    if (hrmType == null)
                        continue;

                    // Skip if already bound.
                    bool already = false;
                    for (int i = 0; i < s_hrmBindings.Count; ++i)
                    {
                        if (ReferenceEquals(s_hrmBindings[i].HrmType, hrmType))
                        {
                            already = true;
                            break;
                        }
                    }
                    if (already)
                        continue;

                    MethodInfo? getAsm = null;
                    MethodInfo? getDom = null;
                    EventInfo? iolEvent = null;
                    try { getAsm = hrmType.GetMethod("GetCurrentAssembly", BindingFlags.Public | BindingFlags.Static); } catch { }
                    try { getDom = hrmType.GetMethod("GetCurrentDomainId", BindingFlags.Public | BindingFlags.Static); } catch { }
                    try { iolEvent = hrmType.GetEvent("InitializeOnLoadCompleted", BindingFlags.Public | BindingFlags.Static); } catch { }
                    if (getAsm == null || getDom == null || iolEvent == null)
                        continue;

                    try
                    {
                        var del = (Action<long, int, int>)OnInitializeOnLoadCompleted;
                        iolEvent.AddEventHandler(null, del);
                        s_hrmBindings.Add(new HotReloadBinding(hrmType, getAsm, getDom, iolEvent, del));
                        try { Console.WriteLine($"[EditorMenuBridge] Bound to HotReloadManager from '{asm.Location}'"); } catch { }
                    }
                    catch
                    {
                        // Ignore failed binds; keep scanning other copies.
                    }
                }
            }
        }

        private static Assembly? TryGetCurrentScriptsAssembly(out ulong domainId)
        {
            domainId = 0;
            EnsureHotReloadBindings();

            lock (s_hrmLock)
            {
                Assembly? bestAsm = null;
                ulong bestDom = 0;

                for (int i = 0; i < s_hrmBindings.Count; ++i)
                {
                    var b = s_hrmBindings[i];
                    try
                    {
                        var asm = b.GetCurrentAssembly.Invoke(null, null) as Assembly;
                        var domObj = b.GetCurrentDomainId.Invoke(null, null);
                        ulong dom = 0;
                        try { dom = domObj != null ? Convert.ToUInt64(domObj) : 0UL; } catch { dom = 0; }

                        if (asm != null && bestAsm == null)
                        {
                            bestAsm = asm;
                            bestDom = dom;
                        }
                        // Prefer non-zero domain id when available.
                        if (asm != null && dom != 0)
                        {
                            bestAsm = asm;
                            bestDom = dom;
                            break;
                        }
                    }
                    catch
                    {
                    }
                }

                domainId = bestDom;
                return bestAsm;
            }
        }

        private static void QueueRefresh()
        {
            if (s_callback == 0)
                return;

            // Mark refresh pending. If a refresh is already running, it will observe this and run again.
            Interlocked.Exchange(ref s_refreshPending, 1);

            // Only allow one worker to run at a time; worker drains pending refresh requests.
            if (Interlocked.CompareExchange(ref s_refreshRunning, 1, 0) != 0)
                return;

            ThreadPool.QueueUserWorkItem(static _ =>
            {
                try
                {
                    // Drain pending refresh requests (e.g., during hot-reload, multiple triggers can arrive
                    // while a scan is in progress). This guarantees we don't miss the "latest" assembly.
                    while (Interlocked.Exchange(ref s_refreshPending, 0) != 0)
                    {
                        RefreshNow();
                    }
                }
                finally
                {
                    Interlocked.Exchange(ref s_refreshRunning, 0);

                    // Race fix: if a refresh request arrived after the last pending check but before
                    // s_refreshRunning was cleared, schedule another worker now.
                    if (Volatile.Read(ref s_refreshPending) != 0)
                    {
                        QueueRefresh();
                    }
                }
            });
        }

        private static void RefreshNow()
        {
            if (s_callback == 0)
                return;

            lock (s_refreshLock)
            {
                try
                {
                    var scriptsAssembly = TryGetCurrentScriptsAssembly(out ulong domainId);
                    if (scriptsAssembly == null)
                    {
                        try { Console.WriteLine("[EditorMenuBridge] RefreshNow: scripts assembly is null (pushing empty snapshot)"); } catch { }
                        PushSnapshot(Array.Empty<DiscoveredItem>());
                        return;
                    }

                    // Discover items from the current scripts assembly plus optional Editor scripts assembly loaded
                    // into the same collectible ALC (GameEngine.Editor.dll).
                    var discovered = new List<DiscoveredItem>(128);
                    discovered.AddRange(DiscoverMenuItems(domainId, scriptsAssembly));
                    try
                    {
                        var alc = AssemblyLoadContext.GetLoadContext(scriptsAssembly);
                        if (alc != null)
                        {
                            foreach (var a in alc.Assemblies)
                            {
                                if (a == null || ReferenceEquals(a, scriptsAssembly))
                                    continue;
                                string? n = null;
                                try { n = a.GetName().Name; } catch { n = null; }
                                if (string.Equals(n, "GameEngine.Editor", StringComparison.OrdinalIgnoreCase))
                                {
                                    discovered.AddRange(DiscoverMenuItems(domainId, a));
                                }
                            }
                        }
                    }
                    catch { }

                    // Dedupe across assemblies (same key format as DiscoverMenuItems).
                    if (discovered.Count > 1)
                    {
                        var dedupe = new HashSet<string>(StringComparer.Ordinal);
                        var filtered = new List<DiscoveredItem>(discovered.Count);
                        foreach (var it in discovered)
                        {
                            string k = $"{(uint)it.Kind}|{(uint)it.Targets}|{it.Priority}|{it.Path}|{it.Method}";
                            if (dedupe.Add(k))
                                filtered.Add(it);
                        }
                        discovered = filtered;
                    }
                    if (discovered.Count > 0)
                    {
                        try
                        {
                            Console.WriteLine($"[EditorMenuBridge] Discovered {discovered.Count} items in '{scriptsAssembly.GetName().Name}'");
                            for (int i = 0; i < Math.Min(discovered.Count, 8); ++i)
                            {
                                var it = discovered[i];
                                Console.WriteLine($"[EditorMenuBridge]  - {(it.Kind == MenuKind.Toolbar ? "Toolbar" : "Context")} path='{it.Path}' method='{it.Method}' targets=0x{((uint)it.Targets):X}");
                            }
                        }
                        catch { }
                    }
                    PushSnapshot(discovered);
                }
                catch
                {
                    // If discovery fails, still push an empty snapshot so native can clear stale menus.
                    try { PushSnapshot(Array.Empty<DiscoveredItem>()); } catch { }
                }
            }
        }

        private static IReadOnlyList<DiscoveredItem> DiscoverMenuItems(ulong domainId, Assembly asm)
        {
            var results = new List<DiscoveredItem>(64);
            var dedupe = new HashSet<string>(StringComparer.Ordinal);

            Type[] types;
            try { types = asm.GetTypes(); }
            catch (ReflectionTypeLoadException ex)
            {
                types = ex.Types?.Where(static t => t != null).Cast<Type>().ToArray() ?? Array.Empty<Type>();
            }
            catch { types = Array.Empty<Type>(); }

            foreach (var t in types)
            {
                if (t == null)
                    continue;

                MethodInfo[] methods;
                try { methods = t.GetMethods(BindingFlags.Public | BindingFlags.Static); }
                catch { continue; }

                foreach (var mi in methods)
                {
                    if (mi == null)
                        continue;
                    if (mi.GetParameters().Length != 0)
                        continue;
                    if (mi.ReturnType != typeof(void) && mi.ReturnType != typeof(int))
                        continue;

                    string methodFqn = (t.FullName ?? t.Name) + "." + mi.Name;

                    IList<CustomAttributeData> attrs;
                    try { attrs = mi.GetCustomAttributesData(); }
                    catch { continue; }

                    foreach (var cad in attrs)
                    {
                        string? attrFullName = null;
                        try { attrFullName = cad.AttributeType?.FullName; } catch { }
                        if (string.IsNullOrEmpty(attrFullName))
                            continue;

                        if (string.Equals(attrFullName, kToolbarAttrFullName, StringComparison.Ordinal) ||
                            string.Equals(attrFullName, kToolbarAliasAttrFullName, StringComparison.Ordinal))
                        {
                            if (!TryReadPathAndPriority(cad, out string path, out int priority))
                                continue;
                            path = SanitizePath(path);
                            if (string.IsNullOrEmpty(path))
                                continue;

                            string key = $"T|{priority}|{path}|{methodFqn}";
                            if (!dedupe.Add(key))
                                continue;

                            results.Add(new DiscoveredItem(domainId, MenuKind.Toolbar, EditorContextMenuTarget.None, priority, path, methodFqn));
                        }
                        else if (string.Equals(attrFullName, kContextAttrFullName, StringComparison.Ordinal))
                        {
                            if (!TryReadContextArgs(cad, out string path, out EditorContextMenuTarget targets, out int priority))
                                continue;
                            path = SanitizePath(path);
                            if (string.IsNullOrEmpty(path))
                                continue;
                            if (targets == EditorContextMenuTarget.None)
                                continue;

                            string key = $"C|{(uint)targets}|{priority}|{path}|{methodFqn}";
                            if (!dedupe.Add(key))
                                continue;

                            results.Add(new DiscoveredItem(domainId, MenuKind.Context, targets, priority, path, methodFqn));
                        }
                    }
                }
            }

            return results;
        }

        private static bool TryReadPathAndPriority(CustomAttributeData cad, out string path, out int priority)
        {
            path = string.Empty;
            priority = 0;

            try
            {
                if (cad.ConstructorArguments.Count > 0)
                {
                    var v = cad.ConstructorArguments[0].Value;
                    if (v is string s)
                        path = s;
                }

                foreach (var na in cad.NamedArguments)
                {
                    if (!string.Equals(na.MemberName, "Priority", StringComparison.Ordinal))
                        continue;
                    try { priority = Convert.ToInt32(na.TypedValue.Value); } catch { priority = 0; }
                }

                return !string.IsNullOrEmpty(path);
            }
            catch
            {
                return false;
            }
        }

        private static bool TryReadContextArgs(CustomAttributeData cad, out string path, out EditorContextMenuTarget targets, out int priority)
        {
            path = string.Empty;
            targets = EditorContextMenuTarget.AssetsItem;
            priority = 0;

            if (!TryReadPathAndPriority(cad, out path, out priority))
                return false;

            try
            {
                foreach (var na in cad.NamedArguments)
                {
                    if (!string.Equals(na.MemberName, "Targets", StringComparison.Ordinal))
                        continue;
                    try
                    {
                        var v = na.TypedValue.Value;
                        if (v != null)
                            targets = (EditorContextMenuTarget)Convert.ToUInt32(v);
                    }
                    catch { /* keep default */ }
                }
                return true;
            }
            catch
            {
                return false;
            }
        }

        private static string SanitizePath(string path)
        {
            if (path == null)
                return string.Empty;

            var p = path.Replace('\\', '/').Trim();
            while (p.StartsWith("/", StringComparison.Ordinal))
                p = p.Substring(1);
            while (p.EndsWith("/", StringComparison.Ordinal))
                p = p.Substring(0, p.Length - 1);
            return p;
        }

        private static void PushSnapshot(IReadOnlyList<DiscoveredItem> items)
        {
            var cb = s_callback;
            if (cb == 0)
                return;

            var onSnapshot = (delegate* unmanaged[Cdecl]<EditorMenuItemNative*, uint, nint, void>)cb;

            if (items == null || items.Count == 0)
            {
                try { Console.WriteLine("[EditorMenuBridge] Pushing empty snapshot"); } catch { }
                onSnapshot(null, 0, s_userData);
                return;
            }

            try { Console.WriteLine($"[EditorMenuBridge] Pushing snapshot count={items.Count}"); } catch { }

            var handles = new List<GCHandle>(items.Count * 2);
            try
            {
                var arr = new EditorMenuItemNative[items.Count];

                for (int i = 0; i < items.Count; ++i)
                {
                    var it = items[i];

                    byte[] pathBytes = Encoding.UTF8.GetBytes(it.Path ?? string.Empty);
                    var hPath = GCHandle.Alloc(pathBytes, GCHandleType.Pinned);
                    handles.Add(hPath);

                    byte[] methodBytes = Encoding.UTF8.GetBytes(it.Method ?? string.Empty);
                    var hMethod = GCHandle.Alloc(methodBytes, GCHandleType.Pinned);
                    handles.Add(hMethod);

                    arr[i] = new EditorMenuItemNative
                    {
                        domainId = it.DomainId,
                        kind = (uint)it.Kind,
                        targetsMask = (uint)it.Targets,
                        priority = it.Priority,
                        pathUtf8 = (byte*)hPath.AddrOfPinnedObject(),
                        pathLen = (uint)pathBytes.Length,
                        methodUtf8 = (byte*)hMethod.AddrOfPinnedObject(),
                        methodLen = (uint)methodBytes.Length,
                    };
                }

                fixed (EditorMenuItemNative* pItems = arr)
                {
                    onSnapshot(pItems, (uint)arr.Length, s_userData);
                }
            }
            finally
            {
                for (int i = 0; i < handles.Count; ++i)
                {
                    try
                    {
                        if (handles[i].IsAllocated)
                            handles[i].Free();
                    }
                    catch { }
                }
            }
        }
    }
}


