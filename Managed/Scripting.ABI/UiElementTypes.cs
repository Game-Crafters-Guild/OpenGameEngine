using System;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace GameEngine.Scripting
{
    /// <summary>
    /// C#-defined element types: discovery, registration, and the load-context lifecycle that
    /// decides what happens to their live elements across a hot reload.
    /// <para>
    /// This type lives in <c>GameEngine.Scripting.ABI</c>, which is pre-loaded into the default
    /// context and never unloaded, so its tables and the function pointers it hands native
    /// outlive every reload. Only the GCHandles it mints are perishable, and those are released
    /// at the unload seam while they are still valid.
    /// </para>
    /// <para>
    /// <b>Owner ids are the engine's, not ours.</b> Native mints one per load context and every
    /// entry point validates it. Nothing here may invent, guess or reuse one — the whole point
    /// is that a script cannot name a context it does not own.
    /// </para>
    /// </summary>
    /// <remarks>
    /// Split across four files by responsibility, all one partial class because they share the
    /// tables and the single lock above them: this file owns the STAGING pipeline and the state
    /// itself, <c>UiElementTypesInterop.cs</c> the C ABI and the element trampolines, and
    /// <c>UiElementTypesLifecycle.cs</c> the load-context and reload seams. The reflection scan
    /// and the value conversion have no shared state at all and are separate classes
    /// (<c>UiElementTypeDiscovery</c>, <c>UiAttributeBinder</c>).
    /// </remarks>
    internal static partial class UiElementTypes
    {
        private static readonly object s_Lock = new object();

        // tag id -> the managed type to build, and the members its .uxml attributes bind to.
        private static readonly Dictionary<ulong, TypeBinding> s_ByTagId =
            new Dictionary<ulong, TypeBinding>();

        // Load contexts we have minted an engine owner id for.
        //
        // A ConditionalWeakTable, NOT a Dictionary: a dictionary key is a STRONG reference, so a
        // context left in it can never be collected and the engine's ALC-leak detector reports it
        // forever. Correctness must not depend on every removal path running — this table is the
        // one place a missed removal is permanent and silent.
        private static readonly ConditionalWeakTable<AssemblyLoadContext, StrongBox<ulong>>
            s_OwnerByContext = new ConditionalWeakTable<AssemblyLoadContext, StrongBox<ulong>>();

        // Owners whose context has unloaded and whose reload verdict is not in yet.
        //
        // The id CANNOT be released at Unloading, which is the obvious place for it: the proxies
        // orphaned there still carry that owner, and deciding they are Faulted is exactly what
        // NotifyReloadCompleted does — with the same id — after the reload finishes. Releasing
        // early would retire the id and make the engine refuse the very call that closes the
        // three-state machine. So the id lives until the verdict, and is released with it.
        private static readonly HashSet<ulong> s_AwaitingReload = new HashSet<ulong>();

        // Batches discovered but not yet registered. Discovery is reflection and runs wherever the
        // reload handler runs; registration is main-thread-only and runs in the window.
        //
        // A BATCH MUST NOT OUTLIVE ITS CONTEXT. Staging and the window are separated by at least
        // one main-thread task, and during a hot reload the drain budget is one task per frame, so
        // a second swap can unload a staged context before its window ever runs. Registering that
        // batch would claim tags for an assembly that no longer exists — and, because the unload
        // that would have released them has already happened, hold them for the rest of the
        // session. OnContextUnloading purges the context's batches for exactly that reason.
        private static readonly List<StagedBatch> s_Staged = new List<StagedBatch>(4);

        // A reload announced itself and the owners in s_AwaitingReload are owed a verdict. Set at
        // the announcement, consumed by the window, so the verdict cannot be issued before the
        // registrations that decide it.
        private static bool s_VerdictDue;

        private static bool s_CallbacksInstalled;
        private static bool s_Armed;
        private static long s_LastSeenIoLGen;
        private static readonly List<HotReloadBinding> s_IoLBindings = new List<HotReloadBinding>(4);

        /// <summary>
        /// One assembly's discovered element types, waiting for a main-thread window to be
        /// registered in. Holds the <see cref="Type"/> objects themselves, which is what the
        /// registration loop needs and what keeps the batch meaningful; it is drained within a
        /// frame, so it is not a lifetime the context can notice.
        /// </summary>
        private sealed class StagedBatch
        {
            internal readonly AssemblyLoadContext Context;
            internal readonly List<(string Tag, Type Type)> Found;
            // Minted at STAGE time, where the context was just observed alive. Carried rather than
            // looked up again so the window registers under the id the unload seam will sweep.
            internal readonly ulong Owner;

            internal StagedBatch(AssemblyLoadContext context, List<(string Tag, Type Type)> found,
                                 ulong owner)
            {
                Context = context;
                Found = found;
                Owner = owner;
            }
        }

        private sealed class TypeBinding
        {
            internal readonly Type Type;
            // Attribute name (lowercase) -> the member it sets. Built once per type.
            internal readonly Dictionary<string, MemberInfo> Members;

            internal TypeBinding(Type type, Dictionary<string, MemberInfo> members)
            {
                Type = type;
                Members = members;
            }
        }

        /// <summary>
        /// Install the native callbacks, arm the unload and reload-completion seams, and stage
        /// every <see cref="UiElementAttribute"/> type already loaded for the first registration
        /// window. Runs once per process; safe to call again.
        /// </summary>
        internal static void Arm()
        {
            lock (s_Lock)
            {
                if (s_Armed)
                    return;
                s_Armed = true;
            }

            EnsureCallbacksInstalled();
            EnsureReloadCompletionBound();

            // Catch up on whatever is already loaded. This is what makes arm ORDER irrelevant:
            // the reload event is not replayed to late subscribers, so a bind that happens after
            // a publish would otherwise miss it entirely — the hazard EditorMenuBridge answers
            // with an unconditional refresh at registration. Whichever of (arm, publish) happens
            // first, the other covers it.
            StageFromActiveAssemblies();
            RequestRegistrationWindow();
        }

        /// <summary>
        /// Ask the engine for a main-thread slot in which to drain what has been staged.
        /// <para>
        /// Safe from any thread, and that is the point: nothing else here may touch the engine
        /// from a reload thread. A failure is swallowed — the batch stays staged and the next
        /// request drains it too.
        /// </para>
        /// </summary>
        private static void RequestRegistrationWindow()
        {
            try { GE_UI_RequestTypeRegistrationWindow(); }
            catch { }
        }

        /// <summary>
        /// Discover element types in every user assembly the orchestrator currently considers
        /// active, recovering each one's load context from the assembly itself, and stage them
        /// for the next registration window.
        /// <para>
        /// Reflective and allocation-light by design: it runs on the reload event, never per
        /// frame. Re-scanning assemblies already registered is harmless — the owner is reused per
        /// context, and the engine's factory registry accepts re-registration by the SAME owner
        /// while refusing a different one.
        /// </para>
        /// <para>
        /// <b>Calls only ANY-THREAD entry points across the ABI</b> — <c>GE_UI_AcquireTypeOwner</c>
        /// (mutex-protected, documented any-thread) to own each context where it is observably
        /// alive, and nothing else. It touches no element, claims no tag and reads no registry, so
        /// it still runs safely on whichever thread the reload handler landed on, including a
        /// thread-pool one when the managed pump's watchdog fires. The main-thread-only calls stay
        /// where they were, in the window.
        /// </para>
        /// </summary>
        private static void StageFromActiveAssemblies()
        {
            HotReloadBinding[] bindings;
            lock (s_Lock)
            {
                if (s_IoLBindings.Count == 0)
                    return;
                bindings = s_IoLBindings.ToArray();
            }

            foreach (HotReloadBinding binding in bindings)
            {
                Assembly[]? active = null;
                try
                {
                    active = binding.GetActiveUserAssemblies.Invoke(null, null) as Assembly[];
                }
                catch { }
                if (active == null)
                    continue;

                foreach (Assembly asm in active)
                {
                    if (asm == null)
                        continue;
                    AssemblyLoadContext? alc = null;
                    try { alc = AssemblyLoadContext.GetLoadContext(asm); }
                    catch { }
                    if (alc == null)
                        continue;
                    StageTypesFrom(asm, alc);
                }
            }
        }

        /// <summary>
        /// Discover <paramref name="assembly"/>'s element types and queue them for registration.
        /// Reflection only — see <see cref="StageFromActiveAssemblies"/> on why that matters.
        /// </summary>
        internal static void StageTypesFrom(Assembly assembly, AssemblyLoadContext context)
        {
            if (assembly == null || context == null)
                return;

            EnsureCallbacksInstalled();
            EnsureReloadCompletionBound();

            List<(string Tag, Type Type)> found = UiElementTypeDiscovery.Discover(assembly);
            if (found.Count == 0)
                return;

            // THE OWNER AND THE UNLOAD SUBSCRIPTION ARE TAKEN HERE, NOT IN THE WINDOW, and the
            // reason is a runtime semantic rather than a preference: Unloading fires synchronously
            // inside Unload(), and subscribing after that point SUCCEEDS SILENTLY and never runs.
            // Subscribing in the window — which is at least one main-thread task later — would
            // therefore attach a dead handler to an already-unloaded context, whose owner is then
            // never released and whose tags are never swept. Here the context has just been
            // handed to us as an active assembly's, so it is alive.
            ulong owner;
            lock (s_Lock)
            {
                if (s_OwnerByContext.TryGetValue(context, out StrongBox<ulong>? existing))
                {
                    owner = existing!.Value;
                }
                else
                {
                    if (GE_UI_AcquireTypeOwner(out owner) != 0 || owner == 0)
                        return;
                    s_OwnerByContext.AddOrUpdate(context, new StrongBox<ulong>(owner));
                    // The default context never unloads, so subscribing to it would be a handler
                    // that never runs.
                    if (context.IsCollectible)
                        context.Unloading += OnContextUnloading;
                }
                s_Staged.Add(new StagedBatch(context, found, owner));
            }
        }

        /// <summary>
        /// Claim one staged batch's tags. Runs in the registration window, on the main thread.
        /// <para>
        /// <b>It never mints an owner.</b> The owner was taken at stage time, where the context
        /// was observably alive; if it is gone from the ownership table by the time the window
        /// runs, the context unloaded in between and this batch is stale. Claiming for it would
        /// take tags on behalf of an assembly that no longer exists, under an id whose unload —
        /// the only thing that would ever release those tags — has already happened.
        /// </para>
        /// </summary>
        private static void RegisterStaged(StagedBatch batch)
        {
            ulong owner = batch.Owner;
            lock (s_Lock)
            {
                if (!s_OwnerByContext.TryGetValue(batch.Context, out StrongBox<ulong>? current) ||
                    current!.Value != owner)
                {
                    // The unload seam has already run for this context (it removes the entry), so
                    // its batches are void. They are normally purged there; this is the residual
                    // window where the window had already taken its snapshot.
                    return;
                }
            }

            foreach ((string tag, Type type) in batch.Found)
            {
                if (GE_UI_RegisterElementType(owner, tag, out ulong tagId) != 0 || tagId == 0)
                {
                    // Refused: the tag belongs to the engine or to another context, or this call
                    // arrived off the main thread and the engine turned the batch away. The engine
                    // logged which and why; this type simply does not become available.
                    continue;
                }
                lock (s_Lock)
                {
                    s_ByTagId[tagId] = new TypeBinding(type, UiElementTypeDiscovery.BuildMemberMap(type));
                }
            }
        }

        /// <summary>
        /// The registration window. The engine calls this on the main thread after any thread
        /// asked for a slot, and it is the ONLY place the registration and verdict exports are
        /// called from.
        /// <para>
        /// <b>The order inside it is the whole protocol.</b> Registration claims tags and hands
        /// back their ids, touching no element; only the verdict decides what happens to the
        /// orphans. So every tag this reload brought back must be claimed — and its binding
        /// stored in <c>s_ByTagId</c> — before the verdict asks the engine to build anything,
        /// because the engine can only ask us by tag id and we can only answer for an id we have
        /// already been handed. Both halves run in this one main-thread slot, so no element
        /// construction can land between them.
        /// </para>
        /// <para>
        /// <b>Returning from here is itself a signal.</b> The engine treats the return as the
        /// batch being complete and issues its own verdict for every owner that claimed a tag
        /// inside — which is what recovers a restored type, whose reload unloads no owner and so
        /// announces no verdict of its own. So <c>s_ByTagId</c> must be filled for every id
        /// claimed BEFORE this returns; deferring any of it past the return would hand the engine
        /// a tag we cannot yet build.
        /// </para>
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void PerformRegistrationsTrampoline()
        {
            try
            {
                StagedBatch[] batches;
                bool verdictDue;
                lock (s_Lock)
                {
                    batches = s_Staged.ToArray();
                    s_Staged.Clear();
                    verdictDue = s_VerdictDue;
                    s_VerdictDue = false;
                }

                foreach (StagedBatch batch in batches)
                {
                    try { RegisterStaged(batch); }
                    catch (Exception ex) { UiElementTypeLog.Report($"registering a staged batch failed: {ex}"); }
                }

                if (!verdictDue)
                    return;

                // A reload finished. Every owner still awaiting a verdict gets one: its elements
                // whose tag came back re-attach a fresh instance, and whatever is still absent is
                // now Faulted rather than merely waiting. Only after this is the id retired.
                ulong[] pending;
                lock (s_Lock)
                {
                    pending = new ulong[s_AwaitingReload.Count];
                    s_AwaitingReload.CopyTo(pending);
                    s_AwaitingReload.Clear();
                }

                foreach (ulong owner in pending)
                {
                    try
                    {
                        GE_UI_NotifyReloadCompleted(owner);
                        GE_UI_ReleaseTypeOwner(owner);
                    }
                    catch { }
                }
            }
            catch (Exception ex)
            {
                // Nothing may cross back into native, and a window that threw must not take the
                // engine with it.
                UiElementTypeLog.Report($"the UI element type registration window failed: {ex}");
            }
        }

    }
}
