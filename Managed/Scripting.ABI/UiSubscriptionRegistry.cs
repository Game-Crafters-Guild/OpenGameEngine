using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Tracks the UI event subscriptions <see cref="Ui.Element"/>'s event accessors handed out
    /// and revokes the ones a scripts AssemblyLoadContext leaves behind when it unloads.
    /// <para>
    /// Why it must exist here: this assembly lives in the DEFAULT load context and is never
    /// unloaded, so it outlives every collectible scripts context and can still run at the
    /// seam. It is also invisible to the hot-reload orchestrator's [OnScriptsUnload] scan,
    /// which only walks the unloading context's own assemblies — user code that forgets to
    /// unsubscribe has nothing else standing between it and a rooted listener.
    /// </para>
    /// <para>
    /// Scope is deliberately narrow. Only subscriptions this assembly minted are tracked, and
    /// only those with a listener belonging to a collectible context. Engine and editor UI
    /// register their own handlers on the very same elements, so revocation keys off the
    /// subscription this API created — never off anything on the element or the UI tree.
    /// </para>
    /// </summary>
    internal static class UiSubscriptionRegistry
    {
        private static readonly object s_Lock = new object();

        // Live subscriptions with at least one listener from a collectible context, paired with
        // it. A subscription whose listeners all come from the default context is never added:
        // nothing can unload them.
        private static readonly List<TrackedSubscription> s_Tracked = new List<TrackedSubscription>();

        // Weak keys — being hooked must not be the thing that keeps a context alive.
        private static readonly ConditionalWeakTable<AssemblyLoadContext, object> s_HookedOwners =
            new ConditionalWeakTable<AssemblyLoadContext, object>();

        private static readonly object s_HookedMarker = new object();

        private readonly struct TrackedSubscription
        {
            internal TrackedSubscription(AssemblyLoadContext owner, Ui.EventSubscription subscription)
            {
                Owner = owner;
                Subscription = subscription;
            }

            internal AssemblyLoadContext Owner { get; }
            internal Ui.EventSubscription Subscription { get; }
        }

        /// <summary>
        /// Records a live subscription against the collectible context one of its listeners
        /// comes from, and subscribes that context's unload once. A listener from the default
        /// context is ignored — it cannot be unloaded, so there is nothing to revoke.
        /// <para>
        /// One subscription can be recorded against several contexts, because listeners on one
        /// event may come from different assemblies. Whichever unloads first takes the native
        /// entry down: revoking early loses a subscription the script can re-create, while
        /// leaving it would pin a context forever.
        /// </para>
        /// </summary>
        internal static void Track(Delegate listener, Ui.EventSubscription subscription)
        {
            AssemblyLoadContext? owner = ResolveCollectibleOwner(listener);
            if (owner == null)
                return;

            lock (s_Lock)
            {
                foreach (TrackedSubscription tracked in s_Tracked)
                {
                    if (ReferenceEquals(tracked.Subscription, subscription) &&
                        ReferenceEquals(tracked.Owner, owner))
                        return;
                }
                s_Tracked.Add(new TrackedSubscription(owner, subscription));
                if (s_HookedOwners.TryGetValue(owner, out _))
                    return;
                s_HookedOwners.Add(owner, s_HookedMarker);
            }
            owner.Unloading += static alc => RevokeOwned(alc);
        }

        /// <summary>Drops a subscription that was revoked by its owner. Safe for untracked ones.</summary>
        internal static void Untrack(Ui.EventSubscription subscription)
        {
            lock (s_Lock)
            {
                for (int i = s_Tracked.Count - 1; i >= 0; i--)
                {
                    if (ReferenceEquals(s_Tracked[i].Subscription, subscription))
                        s_Tracked.RemoveAt(i);
                }
            }
        }

        /// <summary>
        /// Drops the LISTENERS an unloading context owns, and with the last one the native
        /// subscription itself — the unregister plus the GCHandle free that lets the context
        /// actually collect. Runs on the thread that called ALC.Unload(), the same thread the
        /// orchestrator runs user [OnScriptsUnload] teardown on, which is where the UI API
        /// already asks callers to unsubscribe from.
        /// <para>
        /// Per LISTENER rather than per subscription, because several contexts can have
        /// listeners on one event and one of them unloading is no reason to stop delivering to
        /// the others. Only when the last listener is gone does the native entry go.
        /// </para>
        /// </summary>
        private static void RevokeOwned(AssemblyLoadContext owner)
        {
            List<Ui.EventSubscription>? owned = null;
            lock (s_Lock)
            {
                for (int i = s_Tracked.Count - 1; i >= 0; i--)
                {
                    if (!ReferenceEquals(s_Tracked[i].Owner, owner))
                        continue;
                    (owned ??= new List<Ui.EventSubscription>()).Add(s_Tracked[i].Subscription);
                    s_Tracked.RemoveAt(i);
                }
            }
            if (owned == null)
                return;

            foreach (Ui.EventSubscription subscription in owned)
            {
                // An exception here would escape into AssemblyLoadContext.Unload() and take the
                // whole unload down with it, so nothing is allowed to propagate.
                try
                {
                    Delegate[]? listeners = subscription.ListenerSnapshot();
                    if (listeners == null)
                    {
                        subscription.Revoke();
                        continue;
                    }
                    foreach (Delegate listener in listeners)
                    {
                        if (ReferenceEquals(ResolveCollectibleOwner(listener), owner))
                            subscription.RemoveListener(listener);
                    }
                }
                catch
                {
                }
            }
        }

        /// <summary>
        /// The collectible context the listener would keep alive, or null when it belongs to
        /// the default one. A delegate can be multicast and any single collectible target is
        /// enough to pin its context, so the first one found decides.
        /// </summary>
        private static AssemblyLoadContext? ResolveCollectibleOwner(Delegate listener)
        {
            // Registration must never fail because the walk hit an exotic delegate shape
            // (a dynamic method has no ordinary declaring assembly); an unresolved owner
            // simply means untracked, which is no worse than not looking at all.
            try
            {
                foreach (Delegate d in listener.GetInvocationList())
                {
                    // The bound target's type first: a closure instance carries the user
                    // assembly's identity even when the method body was emitted elsewhere.
                    AssemblyLoadContext? owner = CollectibleContextOf(d.Target?.GetType().Assembly)
                                                 ?? CollectibleContextOf(d.Method.Module.Assembly);
                    if (owner != null)
                        return owner;
                }
            }
            catch
            {
            }
            return null;
        }

        private static AssemblyLoadContext? CollectibleContextOf(Assembly? assembly)
        {
            if (assembly == null)
                return null;
            try
            {
                AssemblyLoadContext? alc = AssemblyLoadContext.GetLoadContext(assembly);
                return (alc != null && alc.IsCollectible) ? alc : null;
            }
            catch
            {
                return null;
            }
        }
    }
}
