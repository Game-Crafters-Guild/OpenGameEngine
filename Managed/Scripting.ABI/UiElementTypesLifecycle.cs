using System;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace GameEngine.Scripting
{
    // The two seams the load context announces: it is unloading, and a reload finished. Both run
    // on whichever thread the runtime raises them on, which is why neither touches the engine
    // beyond the entry points documented as safe from one.
    internal static partial class UiElementTypes
    {
        /// <summary>
        /// One loaded copy of <c>GameEngine.HotReload</c>, and the two members we need from it.
        /// <para>
        /// <b>The assembly accessor is deliberately <c>GetActiveUserAssemblies</c>, not
        /// <c>GetCurrentAssembly</c>.</b> The latter is compiled out of Release
        /// (<c>#if DEBUG</c>), so binding to it — as the EditorMenuBridge precedent does, and it
        /// hard-fails the whole bind when the member is missing — would silently disable C#
        /// element types in exactly the configuration that ships. The former is unguarded, and
        /// returns every live user assembly rather than one, which is also the right answer here:
        /// several collectible contexts can be loaded at once and each may define element types.
        /// </para>
        /// </summary>
        private sealed class HotReloadBinding
        {
            internal readonly Type HrmType;
            internal readonly MethodInfo GetActiveUserAssemblies;

            internal HotReloadBinding(Type hrmType, MethodInfo getActiveUserAssemblies)
            {
                HrmType = hrmType;
                GetActiveUserAssemblies = getActiveUserAssemblies;
            }
        }

        /// <summary>
        /// The unloading context's elements must let go of their managed instances NOW: this is
        /// the last moment those handles are valid. The engine releases them inline and marks
        /// the elements Pending; the tree-visible half of that transition is marshalled onto the
        /// UI thread by the engine, because this callback runs on whichever thread called
        /// Unload.
        /// </summary>
        private static void OnContextUnloading(AssemblyLoadContext context)
        {
            ulong owner;
            lock (s_Lock)
            {
                // BEFORE the ownership lookup below, and unconditionally: a batch staged for this
                // context must not survive it, whether or not the context ever reached a window.
                // A surviving batch would be registered later against a dead assembly, and since
                // this unload is the only thing that ever releases its tags, they would be held
                // for the rest of the process.

                s_Staged.RemoveAll(b => ReferenceEquals(b.Context, context));

                if (!s_OwnerByContext.TryGetValue(context, out StrongBox<ulong>? box))
                    return;
                owner = box!.Value;
                s_OwnerByContext.Remove(context);
                // Held live until the reload verdict — see s_AwaitingReload.
                s_AwaitingReload.Add(owner);

                // The types themselves are gone with the context; their tag ids stay Pending on
                // the engine side until a reload either re-registers them or faults them.
                var doomed = new List<ulong>();
                foreach (KeyValuePair<ulong, TypeBinding> kv in s_ByTagId)
                {
                    if (AssemblyLoadContext.GetLoadContext(kv.Value.Type.Assembly) == context)
                        doomed.Add(kv.Key);
                }
                foreach (ulong tagId in doomed)
                    s_ByTagId.Remove(tagId);
            }

            try { GE_UI_OrphanElementTypes(owner, out _); }
            catch { /* nothing may cross the boundary out of an unload callback */ }
        }

        /// <summary>
        /// Bind to <c>HotReloadManager.InitializeOnLoadCompleted</c> by reflection over the
        /// loaded assemblies — no ProjectReference in either direction.
        /// <para>
        /// Mirrors the shipped <c>EditorMenuBridge.EnsureHotReloadBindings</c>, including its two
        /// hazards: bind to EVERY loaded copy rather than assuming one, and filter by generation
        /// so a stale or replayed event cannot fault a context whose reload already succeeded.
        /// </para>
        /// </summary>
        private static void EnsureReloadCompletionBound()
        {
            lock (s_Lock)
            {
                Assembly[] assemblies;
                try { assemblies = AppDomain.CurrentDomain.GetAssemblies(); }
                catch { return; }

                foreach (Assembly asm in assemblies)
                {
                    AssemblyName? an = null;
                    try { an = asm.GetName(); } catch { }
                    if (an == null || !string.Equals(an.Name, "GameEngine.HotReload",
                                                     StringComparison.OrdinalIgnoreCase))
                        continue;

                    Type? hrmType = null;
                    try
                    {
                        hrmType = asm.GetType("GameEngine.HotReload.HotReloadManager",
                                              throwOnError: false, ignoreCase: false);
                    }
                    catch { }
                    if (hrmType == null)
                        continue;

                    bool already = false;
                    foreach (HotReloadBinding bound in s_IoLBindings)
                    {
                        if (ReferenceEquals(bound.HrmType, hrmType)) { already = true; break; }
                    }
                    if (already)
                        continue;

                    EventInfo? iol = null;
                    try
                    {
                        iol = hrmType.GetEvent("InitializeOnLoadCompleted",
                                               BindingFlags.Public | BindingFlags.Static);
                    }
                    catch { }
                    if (iol == null)
                        continue;

                    // The assembly accessor is required, not optional: without it the event is a
                    // bare "something reloaded" with nothing to register from. Deliberately
                    // GetActiveUserAssemblies and NOT GetCurrentAssembly — see HotReloadBinding.
                    MethodInfo? getActive = null;
                    try
                    {
                        getActive = hrmType.GetMethod("GetActiveUserAssemblies",
                                                      BindingFlags.Public | BindingFlags.Static);
                    }
                    catch { }
                    if (getActive == null)
                    {
                        UiElementTypeLog.Report("GameEngine.HotReload exposes no GetActiveUserAssemblies; C#-defined " +
                               "element types cannot be discovered from this copy.");
                        continue;
                    }

                    try
                    {
                        var del = (Action<long, int, int>)OnInitializeOnLoadCompleted;
                        iol.AddEventHandler(null, del);
                        s_IoLBindings.Add(new HotReloadBinding(hrmType, getActive));
                    }
                    catch { /* keep scanning other copies */ }
                }
            }
        }

        private static void OnInitializeOnLoadCompleted(long generation, int invoked, int skipped)
        {
            try
            {
                lock (s_Lock)
                {
                    if (generation > 0 && generation <= s_LastSeenIoLGen)
                        return;
                    if (generation > 0)
                        s_LastSeenIoLGen = generation;
                }

                // THIS HANDLER TOUCHES THE ENGINE EXACTLY ONCE, AND ONLY TO ASK FOR A SLOT.
                // It runs on whichever thread completed the reload, which is a thread-pool one
                // whenever the managed pump's watchdog fires (HotReloadManager.cs:233-243) — and
                // registering there would write the factory registry underneath a document build.
                // So it discovers (reflection, no ABI) and stages, marks the verdict due, and asks
                // for a window. The window does the rest, in order, on the main thread.
                StageFromActiveAssemblies();

                lock (s_Lock)
                {
                    s_VerdictDue = true;
                }

                RequestRegistrationWindow();
            }
            catch { }
        }
    }
}
