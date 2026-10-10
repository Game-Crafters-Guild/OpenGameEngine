using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

namespace GameEngine.Scripting
{
    public static partial class Ui
    {
        /// <summary>
        /// The one native handler-table entry behind one C# event on one element, shared by
        /// every listener on it.
        /// <para>
        /// This is what makes <c>+=</c> cheap: the first listener creates the native
        /// subscription, the rest are combined into an ordinary managed multicast delegate, and
        /// the last <c>-=</c> takes the native entry away again. So N listeners cost one
        /// handler-table entry and one reverse P/Invoke per occurrence — not N of each — and an
        /// event nobody subscribed costs nothing, because there is no subscription to make.
        /// </para>
        /// <para>
        /// <b>Keyed by ELEMENT, never by wrapper.</b> Wrapper identity is per element and
        /// wrappers are held weakly, so a subscription stored on the wrapper disagrees with
        /// identity in two ways the moment a second wrapper exists for one element: a subclass
        /// claiming the element would leave the previous wrapper's listeners unreachable
        /// (<c>-=</c> a silent no-op, <c>+=</c> a second native entry for one event), and a
        /// wrapper collected while a subscription is live would strand that entry forever,
        /// because the GCHandle roots the SUBSCRIPTION, not the wrapper. Keying the table by
        /// instance id makes every wrapper for one element see the same slot, which is the
        /// same rule the identity table follows.
        /// </para>
        /// <para>
        /// The GCHandle rooting this object is what native holds. It roots the listeners too,
        /// and a listener from a collectible scripts context is a root FOR THAT CONTEXT — stale
        /// script code left reachable, assemblies never collected. That is why every
        /// subscription is registered with <see cref="UiSubscriptionRegistry"/>, which revokes
        /// the ones a context leaves behind when it unloads.
        /// </para>
        /// </summary>
        internal sealed class EventSubscription
        {
            // One entry per (element, event). Strong on purpose: the entry must outlive every
            // wrapper for that element, and it is removed the moment the subscription is
            // revoked — which is also when its GCHandle is freed, so the table never holds
            // anything the GCHandle was not already holding.
            private static readonly object s_TableLock = new object();
            private static readonly Dictionary<(ulong InstanceId, string EventName), EventSubscription> s_Table =
                new Dictionary<(ulong InstanceId, string EventName), EventSubscription>();

            private readonly ulong m_InstanceId;
            private readonly string m_EventName;
            private GCHandle m_Gch;
            private ulong m_Token;
            private Delegate? m_Listeners;

            // The one listener, when there is exactly one — which is nearly always. Written
            // only under s_TableLock, immediately after m_Listeners, so Dispatch can decide
            // the fast path from a single read. Null whenever the count is 0 or more than 1.
            //
            // Delegate.Combine(null, d) returns d itself, so with one listener this IS
            // m_Listeners; the field exists to make "is it exactly one" atomic with the value.
            private Delegate? m_Single;
            private int m_ListenerCount;

            // Interlocked, not bool: the owner's last -= and the registry's context-unload
            // revocation are separate callers, and exactly one of them may reach the GCHandle
            // free — freeing an already-freed handle throws.
            private int m_Revoked;

            private EventSubscription(ulong instanceId, string eventName)
            {
                m_InstanceId = instanceId;
                m_EventName = eventName;
            }

            /// <summary>Revoked by a load-context unload, or by its own last unsubscribe.</summary>
            internal bool IsRevoked => Volatile.Read(ref m_Revoked) != 0;

            /// <summary>
            /// Add a listener to this element's subscription for this event, creating the
            /// native subscription if this is the first one. A no-op when the element no longer
            /// resolves: nothing is subscribed and nothing is retained, and the event stays
            /// subscribable so a re-resolved element can be re-wired.
            /// </summary>
            internal static void Add(ulong instanceId, string eventName, Delegate? listener)
            {
                if (listener == null || instanceId == 0)
                    return;

                EventSubscription sub;
                lock (s_TableLock)
                {
                    var key = (instanceId, eventName);
                    // A subscription the registry revoked (its context unloaded) is spent: the
                    // native entry is gone, so combining onto it would silently never fire.
                    if (!s_Table.TryGetValue(key, out EventSubscription? existing) || existing.IsRevoked)
                    {
                        var fresh = new EventSubscription(instanceId, eventName);
                        // Under the lock: the native call cannot re-enter managed code, and two
                        // callers racing here would otherwise each mint a native entry.
                        if (!fresh.Subscribe())
                            return;
                        s_Table[key] = fresh;
                        existing = fresh;
                    }
                    existing.m_Listeners = Delegate.Combine(existing.m_Listeners, listener);
                    // Always increments: the same listener added twice is legitimately two
                    // listeners, and the multicast list holds it twice.
                    ++existing.m_ListenerCount;
                    existing.RefreshSingle();
                    sub = existing;
                }

                // Outside the table lock, and always in this order (table -> registry), so the
                // two locks can never be taken in opposite orders by different threads.
                // Tracked per listener, not per subscription: listeners can come from different
                // contexts, and any one of them unloading must be able to take its own listener
                // out rather than leave a dead delegate rooted.
                UiSubscriptionRegistry.Track(listener, sub);
            }

            /// <summary>
            /// Remove a listener from this element's subscription for this event, taking the
            /// native subscription away with the last one. Removing a listener that was never
            /// added is a no-op, and so is removing one from an element nothing subscribed.
            /// </summary>
            internal static void Remove(ulong instanceId, string eventName, Delegate? listener)
            {
                if (listener == null || instanceId == 0)
                    return;

                EventSubscription? sub;
                lock (s_TableLock)
                {
                    if (!s_Table.TryGetValue((instanceId, eventName), out sub))
                        return;
                }
                sub.RemoveListener(listener);
            }

            /// <summary>
            /// The listeners as they stand, for the registry's per-context sweep. Null when
            /// there are none.
            /// </summary>
            internal Delegate[]? ListenerSnapshot() => m_Listeners?.GetInvocationList();

            /// <summary>
            /// Drop one listener, revoking the native subscription with the last one. True when
            /// the subscription is now spent.
            /// </summary>
            internal bool RemoveListener(Delegate listener)
            {
                lock (s_TableLock)
                {
                    Delegate? before = m_Listeners;
                    m_Listeners = Delegate.Remove(m_Listeners, listener);
                    // Count only what actually left. Delegate.Remove returns the list
                    // unchanged when the listener was never on it, and the same listener may
                    // legitimately appear twice — so identity of the list, not the argument,
                    // is what says a removal happened.
                    if (!ReferenceEquals(before, m_Listeners) && m_ListenerCount > 0)
                        --m_ListenerCount;
                    RefreshSingle();
                    if (m_Listeners != null)
                        return false;
                }
                Revoke();
                return true;
            }

            /// <summary>
            /// Republish the single-listener field from the count. Caller holds s_TableLock,
            /// and must call this after every write to <c>m_Listeners</c> — a stale value here
            /// would either run a dead listener or fall back to the allocating path forever.
            /// </summary>
            private void RefreshSingle() => m_Single = m_ListenerCount == 1 ? m_Listeners : null;

            private unsafe bool Subscribe()
            {
                // Normal (non-pinned) GCHandle: native must be able to find this object, and
                // through it the listener list, for as long as it may invoke the callback.
                m_Gch = GCHandle.Alloc(this);
                int rc = GE_UIElement_RegisterEvent(m_InstanceId, m_EventName, &Trampoline,
                    GCHandle.ToIntPtr(m_Gch), out m_Token);
                if (rc == 0)
                    return true;
                m_Gch.Free();
                m_Gch = default;
                return false;
            }

            /// <summary>
            /// Unregister natively, drop the table entry and release the root. Idempotent, and
            /// safe from either the owning thread's last unsubscribe or the load-context unload
            /// callback.
            /// </summary>
            internal void Revoke()
            {
                if (Interlocked.Exchange(ref m_Revoked, 1) != 0)
                    return;
                lock (s_TableLock)
                {
                    var key = (m_InstanceId, m_EventName);
                    // Only if it is still OURS: a fresh subscription may already have replaced
                    // a revoked one under the same key.
                    if (s_Table.TryGetValue(key, out EventSubscription? current) && ReferenceEquals(current, this))
                        s_Table.Remove(key);
                    m_Listeners = null;
                    m_ListenerCount = 0;
                    RefreshSingle();
                }
                UiSubscriptionRegistry.Untrack(this);
                try
                {
                    // Re-resolves the instance natively — a no-op if the element is already
                    // gone (never a UAF, and never another element's handler, because instance
                    // ids are not reused).
                    GE_UIElement_UnregisterEvent(m_InstanceId, m_EventName, m_Token);
                }
                finally
                {
                    // Unconditional, and NOT because the handle would otherwise pin a load
                    // context — by now m_Listeners is null, so it roots only this object, which
                    // is a default-context type holding two integers and a literal string.
                    //
                    // The reason is the other direction: if the unregister did not resolve the
                    // element, freeing here leaves native holding a handler entry whose `user`
                    // is a freed handle. That is safe ONLY because no element is ever held
                    // detached across a managed call — every TakeChild/AddChild pair in the
                    // tree (the .uxml reconcile, Accordion's re-parent) completes inside one
                    // native call, so managed code never runs while an element is off the tree
                    // and reachable again afterwards. A pooling or Mount-slot caller that
                    // parked a detached element across frames would break that, and
                    // UIElement.h's TakeChild contemplates exactly such a caller: it would need
                    // this to keep the handle instead.
                    if (m_Gch.IsAllocated)
                        m_Gch.Free();
                    m_Gch = default;
                }
            }

            /// <summary>
            /// Invoke every listener, each inside its own try. One listener throwing must
            /// neither stop the others nor reach the cdecl boundary.
            /// </summary>
            internal void Dispatch(in NativeEventData data)
            {
                // The overwhelmingly common case is one listener, and GetInvocationList()
                // allocates a fresh Delegate[] EVERY call — per-frame garbage on a dragged
                // slider or a scrolled view. m_Single is written under the same lock as
                // m_Listeners and is non-null only while exactly one listener is attached, so
                // ONE reference read yields a delegate that was, at some point, the whole list.
                // It can be STALE — a listener added or removed after this read is not
                // reflected, exactly as the single-read rule below already allows for
                // m_Listeners — but it can never be an internally inconsistent PAIR. (A
                // separate listener COUNT could not give even that: count and list are two reads,
                // and a multicast list seen through a count of 1 would run several listeners
                // inside one try — silently breaking the per-listener isolation below.)
                Delegate? single = m_Single;
                if (single != null)
                {
                    try
                    {
                        Invoke(single, data);
                    }
                    catch
                    {
                        // Same isolation as the loop; see the comment there.
                    }
                    return;
                }

                // One read: a listener may unsubscribe during dispatch, and the multicast
                // delegate it replaces is immutable, so the list being walked stays coherent.
                Delegate? listeners = m_Listeners;
                if (listeners == null)
                    return;

                foreach (Delegate listener in listeners.GetInvocationList())
                {
                    try
                    {
                        Invoke(listener, data);
                    }
                    catch
                    {
                        // Isolated per subscriber. Swallowed rather than logged: this runs
                        // inside the UI event dispatch, and a listener throwing every frame
                        // would otherwise flood the log ring it shares with everything else.
                    }
                }
            }

            private static unsafe void Invoke(Delegate listener, in NativeEventData data)
            {
                switch (listener)
                {
                    case Action<PointerArgs> pointer:
                        pointer(new PointerArgs(data.X, data.Y, data.Mods));
                        break;
                    case Action<ClickArgs> click:
                        click(new ClickArgs(data.Mods));
                        break;
                    case Action<ValueArgs> value:
                        value(new ValueArgs(data.Value));
                        break;
                    // A legal generic instantiation over a ref struct: .NET 9's Action<T>
                    // declares `allows ref struct`. The span wraps the native bytes in place —
                    // no copy, no allocation on the dispatch path — and the ref-struct args
                    // keep the view from escaping the handler, which is the lifetime story.
                    case Action<TextValueArgs> text:
                        text(new TextValueArgs(new Utf8Text(
                            data.Text != 0
                                ? new ReadOnlySpan<byte>((void*)data.Text, checked((int)data.TextLen))
                                : ReadOnlySpan<byte>.Empty)));
                        break;
                    case Action<ScrollOffsetArgs> scroll:
                        scroll(new ScrollOffsetArgs(data.ScrollX, data.ScrollY));
                        break;
                    case Action plain:
                        plain();
                        break;
                }
            }

            // Native invokes this on every occurrence. A [UnmanagedCallersOnly] static + &method
            // is the AOT-safe reverse-callback shape (NOT Marshal.GetFunctionPointerForDelegate).
            // Never let an exception cross into native.
            [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
            private static unsafe void Trampoline(NativeEventData* ev, nint user)
            {
                try
                {
                    if (ev != null && GCHandle.FromIntPtr(user).Target is EventSubscription sub)
                        sub.Dispatch(*ev);
                }
                catch
                {
                    // Swallow: a managed exception must never propagate across the cdecl boundary.
                }
            }
        }
    }
}
