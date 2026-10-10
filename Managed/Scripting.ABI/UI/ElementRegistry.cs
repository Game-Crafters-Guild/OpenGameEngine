using System;
using System.Collections.Generic;

namespace GameEngine.Scripting
{
    public static partial class Ui
    {
        /// <summary>
        /// One C# wrapper per live native element, and the native-tag -> wrapper-type mapping
        /// that decides which wrapper to mint.
        /// <para>
        /// Identity is not a nicety here. A subclass instance carries fields, so if every
        /// <see cref="FindElement"/> minted a fresh wrapper the user's state would evaporate
        /// between lookups — that is the whole reason this table exists.
        /// </para>
        /// <para>
        /// The references are WEAK on purpose. A user subclass lives in a collectible scripts
        /// context, and a strong table entry would keep it — and therefore that whole context —
        /// alive merely because something once looked the element up: exactly the leak the
        /// subscription registry exists to prevent, reintroduced through the back door. Dead
        /// entries are pruned when they are next probed, and swept in bulk as the table grows.
        /// </para>
        /// </summary>
        internal static class ElementRegistry
        {
            private static readonly object s_Lock = new object();
            private static readonly Dictionary<ulong, WeakReference<Element>> s_Wrappers =
                new Dictionary<ulong, WeakReference<Element>>();

            // Entries are only ever dropped when probed, so a table that is written far more
            // often than it is read would grow without bound. Sweep once it has doubled past
            // the last swept size, which keeps the amortized cost proportional to live entries.
            private const int kMinSweepSize = 64;
            private static int s_SweepAt = kMinSweepSize;

            // Tag id -> the wrapper type to mint. Built once, from ids the ENGINE computes:
            // re-implementing the hash managed-side would silently mint the base wrapper for
            // every button the day that hash changed.
            private static readonly Dictionary<ulong, Func<ulong, Element>> s_ByTagId =
                new Dictionary<ulong, Func<ulong, Element>>();
            private static bool s_TagsResolved;

            /// <summary>
            /// The wrapper for a live element id, minting one of the right type on first sight.
            /// Null only for id 0, which is never a live element.
            /// </summary>
            internal static Element? Resolve(ulong instanceId)
            {
                if (instanceId == 0)
                    return null;

                // A pure optimisation, and only that: it saves the GE_UIElement_GetTagId
                // P/Invoke below on the common hit. Correctness lives entirely in the
                // re-check after it — delete this block and every identity assertion still
                // passes, delete the re-check and four of them go red. Keep both: the arm
                // that pins this one counts tag-id calls, not identity.
                lock (s_Lock)
                {
                    if (s_Wrappers.TryGetValue(instanceId, out WeakReference<Element>? weak))
                    {
                        if (weak.TryGetTarget(out Element? existing))
                            return existing;
                        // Collected: the wrapper is gone but the native element may well still
                        // be there, so drop the corpse and mint a fresh one below.
                        s_Wrappers.Remove(instanceId);
                    }
                }

                // Outside the lock: this is a P/Invoke, and nothing above depends on it.
                ulong tagId = 0;
                if (GE_UIElement_GetTagId(instanceId, out ulong resolvedTagId) == 0)
                    tagId = resolvedTagId;

                lock (s_Lock)
                {
                    // Another caller may have raced us to it; theirs wins, so identity holds.
                    if (s_Wrappers.TryGetValue(instanceId, out WeakReference<Element>? raced) &&
                        raced.TryGetTarget(out Element? winner))
                        return winner;

                    // The constructor claims the slot, so nothing is inserted here.
                    return Mint(tagId, instanceId);
                }
            }

            /// <summary>
            /// Take over an element's identity. Called by every wrapper constructor, which is
            /// what makes a user's subclass — not a bare <see cref="Element"/> — the instance
            /// later lookups return.
            /// </summary>
            internal static void Claim(Element wrapper)
            {
                if (wrapper.InstanceId == 0)
                    return;
                lock (s_Lock)
                {
                    s_Wrappers[wrapper.InstanceId] = new WeakReference<Element>(wrapper);
                    if (s_Wrappers.Count >= s_SweepAt)
                        Sweep();
                }
            }

            private static Element Mint(ulong tagId, ulong instanceId)
            {
                EnsureTagsResolved();
                if (tagId != 0 && s_ByTagId.TryGetValue(tagId, out Func<ulong, Element>? factory))
                    return factory(instanceId);
                // An unmapped tag is a valid answer, not a failure: the element is some type
                // this binding has no wrapper for, and the base wrapper does everything the
                // element layer offers.
                return new Element(instanceId);
            }

            private static void EnsureTagsResolved()
            {
                if (s_TagsResolved)
                    return;
                // Built-in controls register during engine start-up, so an early call
                // legitimately answers 0 — leave the map unresolved and try again for the NEXT
                // element rather than caching a half-built one. It does not heal the element
                // that triggered this call: that one is already being minted, and it is minted
                // as the base wrapper. Not reachable in practice, because the built-in controls
                // register at static-init long before any script runs. All-or-nothing for the
                // same reason: the built-ins register together, so one answering 0 means "not
                // yet", never "this one does not exist".
                Span<ulong> tagIds = stackalloc ulong[kBuiltInWrappers.Length];
                for (int i = 0; i < kBuiltInWrappers.Length; ++i)
                {
                    tagIds[i] = TagIdOf(kBuiltInWrappers[i].Tag);
                    if (tagIds[i] == 0)
                        return;
                }
                for (int i = 0; i < kBuiltInWrappers.Length; ++i)
                    s_ByTagId[tagIds[i]] = kBuiltInWrappers[i].Mint;
                s_TagsResolved = true;
            }

            // Adding a wrapper is one row. The tag is the native tag, lower-case, as the engine
            // hashes it.
            private static readonly (string Tag, Func<ulong, Element> Mint)[] kBuiltInWrappers =
            {
                ("button", static id => new Button(id)),
                ("label", static id => new Label(id)),
                ("toggle", static id => new Toggle(id)),
                // A native Checkbox IS a ToggleBase — the same bool value surface and the same
                // events — so it mints the Toggle wrapper rather than a wrapper of its own,
                // until something needs checkbox-specific API.
                ("checkbox", static id => new Toggle(id)),
                ("slider", static id => new Slider(id)),
                ("textfield", static id => new TextField(id)),
                ("dropdown", static id => new Dropdown(id)),
                ("scrollview", static id => new ScrollView(id)),
            };

            private static ulong TagIdOf(string tagLower)
                => GE_UI_GetTagId(tagLower, out ulong tagId) == 0 ? tagId : 0;

            // Caller holds s_Lock.
            private static void Sweep()
            {
                List<ulong>? dead = null;
                foreach (KeyValuePair<ulong, WeakReference<Element>> kv in s_Wrappers)
                {
                    if (!kv.Value.TryGetTarget(out _))
                        (dead ??= new List<ulong>()).Add(kv.Key);
                }
                if (dead != null)
                {
                    foreach (ulong id in dead)
                        s_Wrappers.Remove(id);
                }
                s_SweepAt = Math.Max(kMinSweepSize, s_Wrappers.Count * 2);
            }
        }
    }
}
