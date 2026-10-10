using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;
using GameEngine.ECS;
using GameEngine.Scripting;

namespace GameEngine.Scripting.Runtime
{
    /// <summary>
    /// Unified system runner for both GameSystem and IEntitySystem instances.
    /// All systems share a single sorted execution list ordered by <see cref="ISystemEntry.Order"/>.
    /// Called once per frame from native ManagedSystemBridge (builds) or PlayModeDriver (editor).
    /// </summary>
    public static class GameSystemRunner
    {
        // Guards all registration/system list state. Never held while user system
        // code (OnCreate/OnUpdate/...) runs; Tick iterates a published snapshot.
        private static readonly object s_lock = new();

        private static readonly List<ISystemEntry> s_systems = new();
        // Snapshot of s_systems published after every mutation; Tick iterates this
        // without taking s_lock so a purge from an ALC-unload thread cannot race it.
        private static ISystemEntry[] s_executionOrder = Array.Empty<ISystemEntry>();
        // Entity system registrations are preserved across play-mode cycles
        // since [ModuleInitializer] only runs once per assembly load, not per play-mode enter.
        // Each registration is stamped with its owning AssemblyLoadContext so that
        // unloading that context purges the registration (a deleted or renamed system
        // would otherwise pin its old collectible ALC forever via the stale delegate).
        private static readonly List<EntitySystemRegistration> s_entitySystemRegistrations = new();
        private static readonly List<GameSystemRegistration> s_gameSystemRegistrations = new();
        // Collectible ALCs whose Unloading event already has a purge hook attached.
        // ConditionalWeakTable so tracking never pins a context.
        private static readonly ConditionalWeakTable<AssemblyLoadContext, object> s_purgeHookedContexts = new();
        private static ulong s_worldHandle;
        private static bool s_initialized;


        private readonly struct EntitySystemRegistration
        {
            public readonly string Name;
            public readonly int Order;
            public readonly Action<ulong, float> Execute;
            public readonly Action Destroy;
            public readonly string[]? RunAfter;
            public readonly string[]? RunBefore;
            public readonly AssemblyLoadContext? Owner;

            public EntitySystemRegistration(string name, int order,
                Action<ulong, float> execute, Action destroy,
                string[]? runAfter, string[]? runBefore, AssemblyLoadContext? owner)
            {
                Name = name;
                Order = order;
                Execute = execute;
                Destroy = destroy;
                RunAfter = runAfter;
                RunBefore = runBefore;
                Owner = owner;
            }
        }

        private readonly struct GameSystemRegistration
        {
            public readonly string Name;
            public readonly Func<GameSystem> Factory;
            public readonly int Order;
            public readonly string[]? RunAfter;
            public readonly string[]? RunBefore;
            public readonly AssemblyLoadContext? Owner;

            public GameSystemRegistration(string name, Func<GameSystem> factory, int order,
                string[]? runAfter, string[]? runBefore, AssemblyLoadContext? owner)
            {
                Name = name;
                Factory = factory;
                Order = order;
                RunAfter = runAfter;
                RunBefore = runBefore;
                Owner = owner;
            }
        }

        internal interface ISystemEntry
        {
            string Name { get; }
            int Order { get; }
            bool Enabled { get; }
            string[]? RunAfter { get; }
            string[]? RunBefore { get; }
            AssemblyLoadContext? Owner { get; }
            void Execute(ulong worldHandle, float deltaTime);
            void Destroy();
        }

        /// <summary>
        /// Wraps a <see cref="GameSystem"/> instance as a unified system entry.
        /// Manages the full lifecycle: OnCreate, OnEnable, OnUpdate, OnDisable, OnDestroy.
        /// </summary>
        private sealed class GameSystemEntry : ISystemEntry
        {
            private readonly GameSystem m_system;
            private bool m_created;

            public GameSystemEntry(GameSystem system, string[]? runAfter = null, string[]? runBefore = null)
            {
                m_system = system;
                RunAfter = runAfter;
                RunBefore = runBefore;
            }

            public string Name => m_system.GetType().FullName ?? m_system.GetType().Name;
            public int Order => m_system.Order;
            public bool Enabled => m_system.Enabled;
            public string[]? RunAfter { get; }
            public string[]? RunBefore { get; }
            public AssemblyLoadContext? Owner => ResolveOwner(m_system.GetType());

            /// <summary>
            /// Flips the enabled flag. Returns true when the flag actually transitioned on a
            /// created system, i.e. the matching OnEnable/OnDisable callback still needs to run
            /// (outside the runner lock). Before Create() the flag alone is updated — Create()
            /// fires OnEnable itself when the system starts enabled.
            /// </summary>
            public bool SetEnabled(bool enabled)
            {
                if (m_system.Enabled == enabled) return false;
                m_system.Enabled = enabled;
                return m_created;
            }

            /// <summary>Invokes OnEnable/OnDisable for a transition, isolating user-code exceptions.</summary>
            public void FireEnabledTransition(bool enabled)
            {
                try
                {
                    if (enabled)
                        m_system.OnEnable();
                    else
                        m_system.OnDisable();
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine(
                        $"[GameSystemRunner] System '{Name}' {(enabled ? "OnEnable" : "OnDisable")} threw: {ex}");
                }
            }

            public void Create()
            {
                if (m_created) return;
                m_created = true;
                m_system.OnCreate();
                if (m_system.Enabled)
                    m_system.OnEnable();
            }

            public void Execute(ulong worldHandle, float deltaTime)
            {
                m_system.OnUpdate(deltaTime);
            }

            public void Destroy()
            {
                if (!m_created) return;
                try
                {
                    if (m_system.Enabled)
                        m_system.OnDisable();
                }
                catch { }
                try { m_system.OnDestroy(); } catch { }
            }
        }

        /// <summary>
        /// Wraps a source-generated IEntitySystem's static methods as a unified system entry.
        /// Registered by generated [ModuleInitializer] code via <see cref="RegisterEntitySystem"/>.
        /// </summary>
        private sealed class EntitySystemEntry : ISystemEntry
        {
            private readonly string m_name;
            private readonly int m_order;
            private readonly Action<ulong, float> m_execute;
            private readonly Action m_destroy;
            private bool m_enabled = true;

            public EntitySystemEntry(string name, int order, Action<ulong, float> execute, Action destroy,
                AssemblyLoadContext? owner, string[]? runAfter = null, string[]? runBefore = null)
            {
                m_name = name;
                m_order = order;
                m_execute = execute;
                m_destroy = destroy;
                Owner = owner;
                RunAfter = runAfter;
                RunBefore = runBefore;
            }

            public string Name => m_name;
            public int Order => m_order;
            public bool Enabled { get => m_enabled; set => m_enabled = value; }
            public string[]? RunAfter { get; }
            public string[]? RunBefore { get; }
            public AssemblyLoadContext? Owner { get; }

            public void Execute(ulong worldHandle, float deltaTime)
            {
                m_execute(worldHandle, deltaTime);
            }

            public void Destroy()
            {
                try { m_destroy(); } catch { }
            }
        }

        /// <summary>
        /// Registers a source-generated IEntitySystem.
        /// Called by generated [ModuleInitializer] methods when the assembly loads.
        /// </summary>
        public static void RegisterEntitySystem(string name, int order, Action<ulong, float> execute, Action destroy,
            string[]? runAfter = null, string[]? runBefore = null)
        {
            var owner = ResolveOwner(execute);
            lock (s_lock)
            {
                TrackOwnerLocked(owner);
                // Store the registration so it survives Shutdown() / re-Initialize() cycles.
                // [ModuleInitializer] runs once per assembly load, not per play-mode enter.
                // Deduplicate by name to prevent accumulation across hot-reload cycles.
                RemoveRegistrationByName(s_entitySystemRegistrations, name, static r => r.Name);
                s_entitySystemRegistrations.Add(new EntitySystemRegistration(name, order, execute, destroy, runAfter, runBefore, owner));
                // Also add to active list if already initialized (hot-reload during play).
                if (s_initialized)
                {
                    // Remove existing entry with same name to prevent duplicate execution.
                    RemoveActiveSystemByNameLocked(name);
                    s_systems.Add(new EntitySystemEntry(name, order, execute, destroy, owner, runAfter, runBefore));
                    SortSystemsLocked();
                }
            }
        }

        /// <summary>
        /// Registers a GameSystem subclass by factory function.
        /// Called by source-generated [ModuleInitializer] code — avoids reflection-based discovery.
        /// </summary>
        public static void RegisterGameSystem(string name, Func<GameSystem> factory, int order,
            string[]? runAfter = null, string[]? runBefore = null)
        {
            var owner = ResolveOwner(factory);
            GameSystemEntry? toCreate = null;
            lock (s_lock)
            {
                TrackOwnerLocked(owner);
                // Deduplicate by name to prevent accumulation across hot-reload cycles.
                RemoveRegistrationByName(s_gameSystemRegistrations, name, static r => r.Name);
                s_gameSystemRegistrations.Add(new GameSystemRegistration(name, factory, order, runAfter, runBefore, owner));

                // Also add to active list if already initialized (hot-reload during play).
                if (s_initialized)
                {
                    // Remove existing entry with same name to prevent duplicate execution.
                    RemoveActiveSystemByNameLocked(name);
                    var instance = factory();
                    SetWorldHandle(instance, new WorldHandle(s_worldHandle));
                    toCreate = new GameSystemEntry(instance, runAfter, runBefore);
                    s_systems.Add(toCreate);
                    SortSystemsLocked();
                }
            }
            // OnCreate/OnEnable are user code — invoke outside the lock.
            if (toCreate != null)
            {
                try { toCreate.Create(); } catch { }
            }
        }

        /// <summary>
        /// Enables or disables a system by name. Works for both GameSystem and IEntitySystem.
        /// For GameSystem, matches against the class type name.
        /// For IEntitySystem, matches against the registered full name.
        /// A GameSystem transitioning state receives OnDisable/OnEnable (mirroring native
        /// ISystem enable semantics); re-applying the current state is a no-op.
        /// </summary>
        public static bool SetSystemEnabled(string name, bool enabled)
        {
            GameSystemEntry? transitioned = null;
            bool found = false;
            lock (s_lock)
            {
                for (int i = 0; i < s_systems.Count; i++)
                {
                    var entry = s_systems[i];
                    if (entry.Name == name)
                    {
                        if (entry is EntitySystemEntry ese)
                            ese.Enabled = enabled;
                        else if (entry is GameSystemEntry gse && gse.SetEnabled(enabled))
                            transitioned = gse;
                        found = true;
                        break;
                    }
                }
            }
            // OnEnable/OnDisable are user code — invoke outside the lock.
            transitioned?.FireEnabledTransition(enabled);
            return found;
        }

        /// <summary>
        /// True when a stored (persistent) registration with this name exists.
        /// Used by tests to verify domain-unload purging.
        /// </summary>
        public static bool IsSystemRegistered(string name)
        {
            lock (s_lock)
            {
                foreach (var r in s_entitySystemRegistrations)
                    if (r.Name == name) return true;
                foreach (var r in s_gameSystemRegistrations)
                    if (r.Name == name) return true;
            }
            return false;
        }

        private static AssemblyLoadContext? ResolveOwner(Delegate? d)
        {
            if (d == null) return null;
            // Prefer the bound target's type (open-instance delegates created via
            // Delegate.CreateDelegate bind the user instance); fall back to the
            // declaring module of the method body.
            var owner = ResolveOwner(d.Target?.GetType());
            return owner ?? ResolveOwnerOf(d.Method.Module.Assembly);
        }

        private static AssemblyLoadContext? ResolveOwner(Type? t)
            => t == null ? null : ResolveOwnerOf(t.Assembly);

        private static AssemblyLoadContext? ResolveOwnerOf(Assembly asm)
        {
            try
            {
                var alc = AssemblyLoadContext.GetLoadContext(asm);
                return (alc != null && alc.IsCollectible) ? alc : null;
            }
            catch { return null; }
        }

        /// <summary>
        /// Attach a one-time purge hook to a collectible owner context. When the
        /// context begins unloading, every registration and live entry stamped with
        /// it is removed so nothing can pin the ALC (deleted/renamed systems, B2).
        /// Every path that puts an entry in <see cref="s_systems"/> must call this
        /// for that entry's owner, registration-backed or reflection-discovered.
        /// </summary>
        private static void TrackOwnerLocked(AssemblyLoadContext? owner)
        {
            if (owner == null) return;
            if (s_purgeHookedContexts.TryGetValue(owner, out _)) return;
            s_purgeHookedContexts.Add(owner, string.Empty);
            owner.Unloading += static alc => PurgeRegistrations(alc);
        }

        /// <summary>
        /// Removes all registrations and active systems owned by the unloading context.
        /// Runs on the thread that initiated ALC.Unload(); active entries are destroyed
        /// outside the lock.
        /// </summary>
        private static void PurgeRegistrations(AssemblyLoadContext owner)
        {
            List<ISystemEntry>? toDestroy = null;
            lock (s_lock)
            {
                s_entitySystemRegistrations.RemoveAll(r => ReferenceEquals(r.Owner, owner));
                s_gameSystemRegistrations.RemoveAll(r => ReferenceEquals(r.Owner, owner));

                for (int i = s_systems.Count - 1; i >= 0; i--)
                {
                    if (ReferenceEquals(s_systems[i].Owner, owner))
                    {
                        (toDestroy ??= new List<ISystemEntry>()).Add(s_systems[i]);
                        s_systems.RemoveAt(i);
                    }
                }
                if (toDestroy != null)
                    PublishExecutionOrderLocked();
            }
            if (toDestroy != null)
            {
                foreach (var entry in toDestroy)
                {
                    try { entry.Destroy(); } catch { }
                }
            }
        }

        private delegate string NameSelector<T>(T item);

        private static void RemoveRegistrationByName<T>(List<T> list, string name, NameSelector<T> selector)
        {
            for (int i = list.Count - 1; i >= 0; i--)
            {
                if (selector(list[i]) == name)
                {
                    list.RemoveAt(i);
                    break;
                }
            }
        }

        private static void RemoveActiveSystemByNameLocked(string name)
        {
            for (int i = s_systems.Count - 1; i >= 0; i--)
            {
                if (s_systems[i].Name == name)
                {
                    try { s_systems[i].Destroy(); } catch { }
                    s_systems.RemoveAt(i);
                    break;
                }
            }
        }

        private static void PublishExecutionOrderLocked()
        {
            System.Threading.Volatile.Write(ref s_executionOrder, s_systems.ToArray());
        }

        /// <summary>
        /// Called on play-mode enter or standalone app startup.
        /// Discovers GameSystem subclasses, sorts all systems by order, and calls OnCreate/OnEnable.
        /// </summary>
        public static void Initialize(ulong worldHandle)
        {
            ISystemEntry[] created;
            lock (s_lock)
            {
                // Re-entry guard: a second Initialize without an intervening
                // Shutdown() would instantiate every stored registration again
                // (duplicate instances, duplicate OnCreate). All legitimate
                // re-initialization flows go through Shutdown() first.
                if (s_initialized) return;

                s_worldHandle = worldHandle;

                // Rebuild entity systems from stored registrations.
                // These were registered by [ModuleInitializer] on assembly load and survive
                // Shutdown() calls between play-mode cycles.
                foreach (var reg in s_entitySystemRegistrations)
                {
                    s_systems.Add(new EntitySystemEntry(reg.Name, reg.Order, reg.Execute, reg.Destroy, reg.Owner, reg.RunAfter, reg.RunBefore));
                }

                // Create GameSystem instances from source-generated registrations.
                var world = new WorldHandle(s_worldHandle);
                foreach (var reg in s_gameSystemRegistrations)
                {
                    try
                    {
                        var instance = reg.Factory();
                        SetWorldHandle(instance, world);
                        s_systems.Add(new GameSystemEntry(instance, reg.RunAfter, reg.RunBefore));
                    }
                    catch (Exception ex)
                    {
                        Console.Error.WriteLine(
                            $"[GameSystemRunner] Failed to create registered GameSystem '{reg.Name}': {ex.Message}");
                    }
                }

                DiscoverGameSystems();
                SortSystemsLocked();
                created = s_executionOrder;
                s_initialized = true;
            }

            // One line per play-enter: the absence of systems is the most common
            // scripting failure and must be visible without extra tracing.
            Console.WriteLine($"[GameSystemRunner] Initialized with {created.Length} system(s)");

            // OnCreate/OnEnable are user code — invoke outside the lock.
            for (int i = 0; i < created.Length; i++)
            {
                if (created[i] is GameSystemEntry gse)
                {
                    try { gse.Create(); }
                    catch (Exception ex)
                    {
                        Console.Error.WriteLine(
                            $"[GameSystemRunner] System '{created[i].Name}' OnCreate/OnEnable threw: {ex.GetBaseException()}");
                    }
                }
            }
        }

        /// <summary>
        /// Called when a hot-reload swaps the script domain while play mode is running.
        ///
        /// By the time the swap is visible, the runner has already absorbed it once:
        /// the new domain's [ModuleInitializer] registrations hot-added fresh
        /// instances (RegisterGameSystem/RegisterEntitySystem create + OnCreate them
        /// immediately when <c>s_initialized</c>), and PurgeRegistrations removed
        /// everything owned by the unloading context. Tearing the runner down and
        /// re-initializing here — the previous behavior — created and OnCreate'd a
        /// second instance of every system on the same publish, on the same thread.
        ///
        /// The only systems the hot-add path cannot see are reflection-discovered
        /// stragglers (GameSystem subclasses without generated registrations), so
        /// this reconcile runs discovery for those alone and leaves every already
        /// live entry untouched.
        /// </summary>
        public static void ReconcileAfterDomainSwap()
        {
            List<GameSystemEntry>? created = null;
            lock (s_lock)
            {
                if (!s_initialized) return;

                var before = new HashSet<ISystemEntry>(s_systems);
                DiscoverGameSystems();
                foreach (var entry in s_systems)
                {
                    if (entry is GameSystemEntry gse && !before.Contains(entry))
                        (created ??= new List<GameSystemEntry>()).Add(gse);
                }
                if (created != null)
                    SortSystemsLocked();
            }

            if (created != null)
            {
                // OnCreate/OnEnable are user code — invoke outside the lock.
                foreach (var gse in created)
                {
                    try { gse.Create(); } catch { }
                }
            }
        }

        /// <summary>
        /// Called once per frame from native ManagedSystemBridge or PlayModeDriver.
        /// Executes all enabled systems in order.
        /// Structural changes are deferred during execution and flushed at the end.
        /// </summary>
        public static void Tick(float deltaTime)
        {
            if (!s_initialized) return;

            // Iterate the published snapshot: a purge triggered by an ALC unload on
            // another thread swaps the snapshot atomically instead of mutating it.
            var systems = System.Threading.Volatile.Read(ref s_executionOrder);

            // Defer structural changes so Span<T> pointers remain valid
            // during IEntitySystem chunk iteration.
            // Note: In standalone builds, ManagedSystemBridge also sets deferral before
            // calling Tick. The double-set is idempotent and safe. In editor builds,
            // PlayModeDriver does NOT set deferral, so this is the only fence.
            ECS.Internal.ChunkQueryNative.SetDeferStructuralChanges(s_worldHandle, true);
            try
            {
                for (int i = 0; i < systems.Length; i++)
                {
                    var entry = systems[i];
                    if (!entry.Enabled) continue;
                    try
                    {
                        entry.Execute(s_worldHandle, deltaTime);
                    }
                    catch (Exception ex)
                    {
                        // Isolate per-system exceptions so one broken system
                        // doesn't disable all subsequent systems.
                        Console.Error.WriteLine(
                            $"[GameSystemRunner] System '{entry.Name}' (order={entry.Order}) threw: {ex}");
                    }
                }
            }
            finally
            {
                // Flush deferred commands (entity create/destroy, component add/remove)
                // and reset the deferral flag so structural changes outside Tick work normally.
                ECS.Internal.ChunkQueryNative.FlushDeferredCommands(s_worldHandle);
                ECS.Internal.ChunkQueryNative.SetDeferStructuralChanges(s_worldHandle, false);
            }
        }

        /// <summary>
        /// Called on play-mode exit or standalone app shutdown.
        /// Destroys all systems in reverse order and clears the list.
        /// </summary>
        public static void Shutdown()
        {
            ISystemEntry[] toDestroy;
            lock (s_lock)
            {
                toDestroy = s_systems.ToArray();
                s_systems.Clear();
                // s_entitySystemRegistrations and s_gameSystemRegistrations survive across
                // play-mode cycles since [ModuleInitializer] runs once per assembly load;
                // registrations from an unloading ALC are purged by PurgeRegistrations.
                s_worldHandle = 0;
                s_initialized = false;
                PublishExecutionOrderLocked();
            }
            // Destroy is user code — invoke outside the lock, in reverse order.
            for (int i = toDestroy.Length - 1; i >= 0; i--)
            {
                try { toDestroy[i].Destroy(); } catch { }
            }
        }

        /// <summary>
        /// Scans loaded assemblies for non-abstract subclasses of <see cref="GameSystem"/>,
        /// instantiates them, injects the world handle, and wraps them as ISystemEntry.
        /// </summary>
        // Trim-safety: this reflection fallback only ever discovers types in user
        // script assemblies. Under CoreCLR (editor/hot-reload) nothing is trimmed.
        // Under NativeAOT the BuildPipeline's generated AOT project roots the
        // compiled scripts assembly whole (<TrimmerRootAssembly> — required for the
        // source-generated [ModuleInitializer] registrations to run at all), so
        // every GameSystem subclass and its parameterless constructor is preserved
        // by construction. Not a blind suppression: remove the rooting and the C#
        // never ticks long before this scan could misbehave (the packaged export
        // probe fails the build in that case).
        [UnconditionalSuppressMessage("Trimming", "IL2026",
            Justification = "User script assemblies are fully rooted in AOT builds (TrimmerRootAssembly in the generated csproj); CoreCLR paths never trim.")]
        [UnconditionalSuppressMessage("Trimming", "IL2067",
            Justification = "Activator.CreateInstance targets types from the rooted user scripts assembly; their public parameterless constructors are preserved by the assembly-level root.")]
        private static void DiscoverGameSystems()
        {
            // Build a set of already-registered names so reflection fallback skips them.
            var registeredNames = new HashSet<string>(s_gameSystemRegistrations.Count);
            foreach (var reg in s_gameSystemRegistrations)
            {
                registeredNames.Add(reg.Name);
            }
            // Also skip anything already live: ReconcileAfterDomainSwap runs discovery
            // against a populated system list, and a re-discovered duplicate would
            // OnCreate + tick a second instance of the same system.
            foreach (var entry in s_systems)
            {
                registeredNames.Add(entry.Name);
            }

            var worldHandle = new WorldHandle(s_worldHandle);

            // Last-wins by full name: a stale assembly pending GC after hot-reload can
            // still appear in AppDomain.GetAssemblies() next to its replacement; later
            // load order wins so the fresh copy is the one instantiated.
            var candidates = new Dictionary<string, Type>(StringComparer.Ordinal);

            foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
            {
                if (asm == null) continue;
                if (!IsCandidateAssembly(asm)) continue;

                Type[] types;
                try { types = asm.GetTypes(); }
                catch (ReflectionTypeLoadException e)
                {
                    // Partial type load: the types that DID load are still scanned, but a
                    // GameSystem subclass whose base failed to resolve is in the null set —
                    // surface the loader errors or discovery silently misses user systems.
                    var loaderMessage = (e.LoaderExceptions is { Length: > 0 } && e.LoaderExceptions[0] != null)
                        ? e.LoaderExceptions[0]!.Message
                        : e.Message;
                    Console.Error.WriteLine(
                        $"[GameSystemRunner] Discovery: '{asm.GetName().Name}' loaded partially: {loaderMessage}");
                    var src = e.Types ?? Array.Empty<Type?>();
                    var tmp = new List<Type>(src.Length);
                    for (int i = 0; i < src.Length; i++)
                    {
                        var t = src[i];
                        if (t != null) tmp.Add(t);
                    }
                    types = tmp.ToArray();
                }
                catch (Exception e)
                {
                    Console.Error.WriteLine(
                        $"[GameSystemRunner] Discovery: skipping '{asm.GetName().Name}': {e.GetBaseException().Message}");
                    continue;
                }

                foreach (var type in types)
                {
                    if (type == null) continue;
                    if (type.IsAbstract) continue;
                    if (!type.IsSubclassOf(typeof(GameSystem))) continue;

                    // Skip types already registered by source-generated code.
                    var fullName = type.FullName ?? type.Name;
                    if (registeredNames.Contains(fullName)) continue;

                    candidates[fullName] = type;
                }
            }

            foreach (var (fullName, type) in candidates)
            {
                try
                {
                    var instance = (GameSystem)Activator.CreateInstance(type)!;
                    SetWorldHandle(instance, worldHandle);
                    var entry = new GameSystemEntry(instance);
                    // A discovered system has no registration to carry the purge hook, so it
                    // must attach its own: without it the entry outlives the scripts context
                    // it came from, pinning that ALC and holding the system's name against
                    // the skip set above — so the swapped-in type is never instantiated and
                    // the system stops restarting across a mid-play hot reload.
                    TrackOwnerLocked(entry.Owner);
                    s_systems.Add(entry);
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine(
                        $"[GameSystemRunner] Failed to create GameSystem '{fullName}': {ex.Message}");
                }
            }
        }

        private static void SetWorldHandle(GameSystem system, WorldHandle world)
        {
            // GameSystem.World has an internal setter in Scripting.ABI.
            // Accessible here via InternalsVisibleTo("GameEngine.Scripting.Runtime").
            system.World = world;
        }

        private static bool IsCandidateAssembly(Assembly a)
        {
            try
            {
                var name = a.GetName().Name ?? "";
                if (name.StartsWith("System.", StringComparison.Ordinal)) return false;
                if (name.StartsWith("Microsoft.", StringComparison.Ordinal)) return false;
                if (name.StartsWith("netstandard", StringComparison.Ordinal)) return false;
                if (name.StartsWith("mscorlib", StringComparison.Ordinal)) return false;
                if (name.StartsWith("GameEngine.CoreBridge", StringComparison.Ordinal)) return false;
                if (name.StartsWith("GameEngine.Editor.Managed", StringComparison.Ordinal)) return false;
                if (name.StartsWith("GameEngine.HotReload", StringComparison.Ordinal)) return false;
                if (name.StartsWith("GameEngine.Scripting.Runtime", StringComparison.Ordinal)) return false;
                if (name.EndsWith(".ABI", StringComparison.Ordinal)) return false;
                return true;
            }
            catch { return false; }
        }

        private static void SortSystemsLocked()
        {
            SortSystemListLocked();
            PublishExecutionOrderLocked();
        }

        private static void SortSystemListLocked()
        {
            int count = s_systems.Count;
            if (count <= 1) return;

            // Check if any system has dependency attributes.
            bool hasDependencies = false;
            for (int i = 0; i < count; i++)
            {
                var entry = s_systems[i];
                if ((entry.RunAfter != null && entry.RunAfter.Length > 0) ||
                    (entry.RunBefore != null && entry.RunBefore.Length > 0))
                {
                    hasDependencies = true;
                    break;
                }
            }

            if (!hasDependencies)
            {
                // Fast path: no dependencies, just sort by Order.
                s_systems.Sort((a, b) => a.Order.CompareTo(b.Order));
                return;
            }

            // Build name-to-index mapping.
            var nameToIndex = new Dictionary<string, int>(count);
            for (int i = 0; i < count; i++)
            {
                nameToIndex[s_systems[i].Name] = i;
            }

            // Build adjacency list and in-degree array for Kahn's algorithm.
            // Edge: A -> B means A must run before B.
            var adjacency = new List<int>[count];
            var inDegree = new int[count];
            for (int i = 0; i < count; i++)
            {
                adjacency[i] = new List<int>();
            }

            for (int i = 0; i < count; i++)
            {
                var entry = s_systems[i];

                // [After(typeof(X))] means X -> this (X must run before this).
                if (entry.RunAfter != null)
                {
                    foreach (var depName in entry.RunAfter)
                    {
                        if (nameToIndex.TryGetValue(depName, out int depIndex))
                        {
                            adjacency[depIndex].Add(i);
                            inDegree[i]++;
                        }
                        else
                        {
                            Console.Error.WriteLine(
                                $"[GameSystemRunner] System '{entry.Name}' declares [After(typeof({depName}))] but '{depName}' is not registered.");
                        }
                    }
                }

                // [Before(typeof(X))] means this -> X (this must run before X).
                if (entry.RunBefore != null)
                {
                    foreach (var depName in entry.RunBefore)
                    {
                        if (nameToIndex.TryGetValue(depName, out int depIndex))
                        {
                            adjacency[i].Add(depIndex);
                            inDegree[depIndex]++;
                        }
                        else
                        {
                            Console.Error.WriteLine(
                                $"[GameSystemRunner] System '{entry.Name}' declares [Before(typeof({depName}))] but '{depName}' is not registered.");
                        }
                    }
                }
            }

            // Kahn's algorithm with priority queue (Order, registrationIndex) as tiebreaker.
            // Using a sorted set to simulate a priority queue.
            var ready = new SortedSet<(int Order, int Index)>();
            for (int i = 0; i < count; i++)
            {
                if (inDegree[i] == 0)
                {
                    ready.Add((s_systems[i].Order, i));
                }
            }

            var sorted = new List<ISystemEntry>(count);
            while (ready.Count > 0)
            {
                var min = ready.Min;
                ready.Remove(min);
                int idx = min.Index;
                sorted.Add(s_systems[idx]);

                foreach (int neighbor in adjacency[idx])
                {
                    inDegree[neighbor]--;
                    if (inDegree[neighbor] == 0)
                    {
                        ready.Add((s_systems[neighbor].Order, neighbor));
                    }
                }
            }

            if (sorted.Count < count)
            {
                // Cycle detected — collect the names of systems still in the graph.
                var cycleNames = new List<string>();
                for (int i = 0; i < count; i++)
                {
                    if (inDegree[i] > 0)
                    {
                        cycleNames.Add(s_systems[i].Name);
                    }
                }
                Console.Error.WriteLine(
                    $"[GameSystemRunner] Dependency cycle detected among systems: {string.Join(", ", cycleNames)}. Falling back to Order-based sort.");
                s_systems.Sort((a, b) => a.Order.CompareTo(b.Order));
                return;
            }

            s_systems.Clear();
            s_systems.AddRange(sorted);
        }
    }
}
