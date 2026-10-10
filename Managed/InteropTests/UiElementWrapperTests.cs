using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using NUnit.Framework;
using GameEngine.Scripting;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// The managed object model's own bookkeeping: one wrapper per live element, typed from the
    /// element's tag, held weakly; and one native handler-table subscription per event however
    /// many C# listeners are on it.
    /// <para>
    /// Driven against the native shim's element test double (Tests/NativeShim/
    /// GameEngineNativeShimUi.cpp), because a dotnet-test host has no window, no device and no
    /// mounted UIDocument, so the real exports could only ever answer not-found here. The double
    /// mirrors the real exports' validation and result codes; what it cannot speak for — whether
    /// a Button dispatches a click, whether a cancelled press becomes one, whether a detached
    /// element still resolves — is pinned in UITests against the real tree.
    /// </para>
    /// </summary>
    public class UiElementWrapperTests
    {
        private const string kLib = "GameEngine.Native";

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_AddElement(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string elementId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string tagLower, out ulong outInstanceId);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_DestroyElement(ulong instanceId);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_SubscriptionCount(ulong instanceId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? eventName, out int outCount);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_Dispatch(ulong instanceId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string eventName, float x, float y, int mods,
            out int outInvoked);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_Reset();

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_GetTagIdCallCount(out ulong outCount);

        // Reached directly, not through a wrapper: the rules they pin are ones the typed
        // managed surface cannot express.
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UIElement_SetLabelText(ulong instanceId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string text);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_RegisterNoopEvent(ulong instanceId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string eventName);

        private const int kOk = 0;
        private const int kInvalidArg = -2;

        private const string kFocusIn = "UI.FocusIn";
        private const string kMouseMove = "UI.MouseMove";

        /// <summary>Each test builds its own elements, so the double starts empty.</summary>
        [SetUp]
        public void Reset() => Assert.That(GE_TestUI_Reset(), Is.EqualTo(0));

        private static ulong AddElement(string elementId, string tagLower)
        {
            Assert.That(GE_TestUI_AddElement(elementId, tagLower, out ulong instanceId), Is.EqualTo(0));
            Assert.That(instanceId, Is.Not.Zero);
            return instanceId;
        }

        private static int SubscriptionCount(ulong instanceId, string? eventName = null)
        {
            Assert.That(GE_TestUI_SubscriptionCount(instanceId, eventName, out int count), Is.EqualTo(0));
            return count;
        }

        private static int Dispatch(ulong instanceId, string eventName, float x = 0f, float y = 0f, int mods = 0)
        {
            Assert.That(GE_TestUI_Dispatch(instanceId, eventName, x, y, mods, out int invoked), Is.EqualTo(0));
            return invoked;
        }

        /// <summary>
        /// ARM 12 — the same live element resolves to the same C# instance. Without this a
        /// subclass's state would evaporate between lookups, which is the whole reason the
        /// wrapper table exists rather than minting per call.
        /// </summary>
        [Test]
        public void FindElement_ReturnsTheSameWrapperInstanceForOneElement()
        {
            ulong instanceId = AddElement("hp-fill", "uielement");

            Ui.Element? first = Ui.FindElement("hp-fill");
            Ui.Element? second = Ui.FindElement("hp-fill");

            Assert.That(first, Is.Not.Null);
            Assert.That(second, Is.Not.Null);
            Assert.That(ReferenceEquals(first, second), Is.True,
                "a second lookup minted a fresh wrapper — any state a subclass held is now lost");
            Assert.That(first!.IsAlive, Is.True);

            // Reference equality means what C# means, and a dead element does NOT compare equal
            // to null: this deliberately does not copy UnityEngine.Object's == overload.
            GE_TestUI_DestroyElement(instanceId);
            Assert.That(first.IsAlive, Is.False);
            Assert.That(first == null, Is.False, "a dead wrapper must not fake-null");
        }

        /// <summary>
        /// The wrapper's TYPE comes from the element's own tag, so a script gets Clicked on a
        /// button and SetText on a label without casting blind. An unmapped tag falls back to
        /// the base wrapper rather than failing.
        /// </summary>
        [Test]
        public void FindElement_TypesTheWrapperFromTheElementsTag()
        {
            AddElement("damage-btn", "button");
            AddElement("hp-label", "label");
            AddElement("hp-panel", "uielement");
            AddElement("odd-one", "somecontrolthisbindinghasnowrapperfor");

            Assert.That(Ui.FindElement("damage-btn"), Is.InstanceOf<Ui.Button>());
            Assert.That(Ui.FindElement("hp-label"), Is.InstanceOf<Ui.Label>());

            Ui.Element? panel = Ui.FindElement("hp-panel");
            Assert.That(panel, Is.Not.Null);
            Assert.That(panel!.GetType(), Is.EqualTo(typeof(Ui.Element)),
                "a plain element must not be typed as a control");

            Ui.Element? unmapped = Ui.FindElement("odd-one");
            Assert.That(unmapped, Is.Not.Null);
            Assert.That(unmapped!.GetType(), Is.EqualTo(typeof(Ui.Element)),
                "an unmapped tag must fall back to the base wrapper, not fail the lookup");
        }

        private sealed class HealthBar : Ui.Element
        {
            public HealthBar(Ui.Element source)
                : base(source)
            {
            }

            public int Hp { get; set; }
        }

        /// <summary>
        /// ARM 12, second half — a user subclass built over an element takes over that element's
        /// identity, so a later lookup hands back the subclass with its state, not a bare
        /// wrapper. This is what makes "subclass for composition" work at all.
        /// </summary>
        [Test]
        public void SubclassClaimsTheElementIdentityAndKeepsItsState()
        {
            AddElement("hp-panel", "uielement");
            Ui.Element? resolved = Ui.FindElement("hp-panel");
            Assert.That(resolved, Is.Not.Null);

            var bar = new HealthBar(resolved!) { Hp = 42 };

            Ui.Element? again = Ui.FindElement("hp-panel");
            Assert.That(again, Is.SameAs(bar),
                "the subclass did not claim the element — a later lookup returned something else");
            Assert.That(((HealthBar)again!).Hp, Is.EqualTo(42),
                "the subclass instance came back but its state did not");

            Assert.That(() => new HealthBar(null!), Throws.ArgumentNullException);
        }

        /// <summary>
        /// ARM 12, third half — the table holds wrappers WEAKLY. A strong table would keep a
        /// user subclass alive, and with it the whole collectible scripts context it was defined
        /// in, merely because something once looked the element up.
        /// </summary>
        [Test]
        public void WrapperTableDoesNotKeepWrappersAlive()
        {
            AddElement("transient", "uielement");
            WeakReference weak = ResolveAndForget();

            for (int i = 0; i < 20 && weak.IsAlive; i++)
            {
                GC.Collect();
                GC.WaitForPendingFinalizers();
                Thread.Sleep(20);
            }

            Assert.That(weak.IsAlive, Is.False,
                "the wrapper survived with no user reference — the identity table is holding it "
                + "strongly, which would pin the load context a user subclass lives in");
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static WeakReference ResolveAndForget() => new WeakReference(Ui.FindElement("transient"));

        /// <summary>
        /// ARM 15 — N managed listeners cost ONE native handler-table entry, and the last
        /// unsubscribe takes it away. Red under the naive one-entry-per-listener implementation
        /// (the count would climb), and red if the last -= leaks the entry — which would keep a
        /// GCHandle rooted and pin the scripts context.
        /// </summary>
        [Test]
        public void ManyListenersShareOneNativeSubscription()
        {
            ulong instanceId = AddElement("hp-panel", "uielement");
            Ui.Element el = Ui.FindElement("hp-panel")!;
            Assert.That(SubscriptionCount(instanceId), Is.EqualTo(0));

            int first = 0;
            int second = 0;
            Action onFirst = () => ++first;
            Action onSecond = () => ++second;

            el.FocusGained += onFirst;
            Assert.That(SubscriptionCount(instanceId, kFocusIn), Is.EqualTo(1));
            el.FocusGained += onSecond;
            Assert.That(SubscriptionCount(instanceId, kFocusIn), Is.EqualTo(1),
                "a second listener created a second native subscription");

            Assert.That(Dispatch(instanceId, kFocusIn), Is.EqualTo(1),
                "native invoked the binding more than once for one occurrence");
            Assert.That(first, Is.EqualTo(1));
            Assert.That(second, Is.EqualTo(1));

            el.FocusGained -= onFirst;
            Assert.That(SubscriptionCount(instanceId, kFocusIn), Is.EqualTo(1),
                "removing one of two listeners tore down the shared native subscription");
            Dispatch(instanceId, kFocusIn);
            Assert.That(first, Is.EqualTo(1), "a removed listener still fired");
            Assert.That(second, Is.EqualTo(2));

            el.FocusGained -= onSecond;
            Assert.That(SubscriptionCount(instanceId, kFocusIn), Is.EqualTo(0),
                "the last unsubscribe leaked the native subscription — its GCHandle would keep "
                + "the listeners, and their load context, rooted forever");
            Assert.That(Dispatch(instanceId, kFocusIn), Is.EqualTo(0));
            Assert.That(second, Is.EqualTo(2));

            // And it can be wired again afterwards.
            el.FocusGained += onFirst;
            Assert.That(SubscriptionCount(instanceId, kFocusIn), Is.EqualTo(1));
            el.FocusGained -= onFirst;
        }

        /// <summary>
        /// The wrapper cache saves the tag-id P/Invoke on a repeat lookup. That is all it does —
        /// identity itself is carried by the re-check behind it — so this counts calls rather
        /// than asserting identity, which no other arm can distinguish.
        /// </summary>
        [Test]
        public void ARepeatLookupDoesNotReAskForTheTagId()
        {
            AddElement("hp-panel", "uielement");

            Ui.Element first = Ui.FindElement("hp-panel")!;
            Assert.That(GE_TestUI_GetTagIdCallCount(out ulong afterFirst), Is.EqualTo(0));
            Assert.That(afterFirst, Is.GreaterThan(0UL),
                "minting a wrapper must ask the engine for the element's tag");

            Ui.Element second = Ui.FindElement("hp-panel")!;
            Assert.That(GE_TestUI_GetTagIdCallCount(out ulong afterSecond), Is.EqualTo(0));
            Assert.That(afterSecond, Is.EqualTo(afterFirst),
                "a repeat lookup re-asked for the tag id — the wrapper cache is not saving the "
                + "P/Invoke it exists for");
            Assert.That(second, Is.SameAs(first));
            GC.KeepAlive(first);
        }

        /// <summary>
        /// The two type rules the real element ABI enforces, asserted against the double that
        /// claims to mirror them: text belongs to a Label, and only a Button dispatches a click.
        /// <para>
        /// Driven through the raw exports rather than the wrappers, because the typed managed
        /// surface makes both rules unspellable — <c>SetText</c> exists only on
        /// <see cref="Ui.Label"/> and <c>Clicked</c> only on <see cref="Ui.Button"/>. That is
        /// the binding doing its job, and it is also why the rules would go unobserved here
        /// unless something reached past it, which another language binding certainly would.
        /// </para>
        /// </summary>
        [Test]
        public void TheDoubleEnforcesTheRealExportsTypeRules()
        {
            ulong panel = AddElement("hp-panel", "uielement");
            ulong label = AddElement("hp-label", "label");
            ulong button = AddElement("damage-btn", "button");

            Assert.That(GE_UIElement_SetLabelText(label, "100 / 100"), Is.EqualTo(kOk));
            Assert.That(GE_UIElement_SetLabelText(button, "nope"), Is.EqualTo(kInvalidArg),
                "SetLabelText on a live non-Label must be InvalidArg, not a silent success");
            Assert.That(GE_UIElement_SetLabelText(panel, "nope"), Is.EqualTo(kInvalidArg));

            Assert.That(GE_TestUI_RegisterNoopEvent(button, "UI.ButtonClick"), Is.EqualTo(kOk));
            Assert.That(GE_TestUI_RegisterNoopEvent(panel, "UI.ButtonClick"), Is.EqualTo(kInvalidArg),
                "a click subscription on a non-Button must be refused — nothing else dispatches "
                + "UI.ButtonClick, so it could never fire");
            Assert.That(GE_TestUI_RegisterNoopEvent(panel, "UI.MouseMove"), Is.EqualTo(kOk),
                "control: the refusal above is about the EVENT, not about this element");
        }

        /// <summary>
        /// ARM 17 — no subscription, no cost. An event nobody subscribed has no native entry at
        /// all, so nothing crosses the boundary when it occurs. Red the day the event surface
        /// auto-wires anything: with zero listeners there must be zero native subscriptions and
        /// therefore zero reverse P/Invokes.
        /// </summary>
        [Test]
        public void AnUnsubscribedEventCostsNothing()
        {
            ulong instanceId = AddElement("hp-panel", "uielement");
            Ui.Element el = Ui.FindElement("hp-panel")!;
            GC.KeepAlive(el);

            Assert.That(SubscriptionCount(instanceId), Is.EqualTo(0),
                "resolving an element subscribed something by itself — the surface is auto-wiring");
            Assert.That(Dispatch(instanceId, kMouseMove), Is.EqualTo(0));
            Assert.That(Dispatch(instanceId, kFocusIn), Is.EqualTo(0));

            // Subscribing to ONE event must not wire the others.
            Action noop = () => { };
            el.FocusGained += noop;
            Assert.That(SubscriptionCount(instanceId, kMouseMove), Is.EqualTo(0));
            Assert.That(SubscriptionCount(instanceId), Is.EqualTo(1));
            el.FocusGained -= noop;
        }

        /// <summary>
        /// ARM 16 — each listener is invoked inside its own try. A throwing listener must
        /// neither stop the others nor cross the cdecl boundary (which would tear the process
        /// down, since a managed exception cannot propagate out of an UnmanagedCallersOnly
        /// frame).
        /// </summary>
        [Test]
        public void AThrowingListenerNeitherStopsTheOthersNorEscapes()
        {
            ulong instanceId = AddElement("hp-panel", "uielement");
            Ui.Element el = Ui.FindElement("hp-panel")!;

            var order = new List<int>();
            Action first = () => { order.Add(1); throw new InvalidOperationException("listener one"); };
            Action second = () => order.Add(2);
            Action third = () => { order.Add(3); throw new InvalidOperationException("listener three"); };

            el.FocusGained += first;
            el.FocusGained += second;
            el.FocusGained += third;

            // If isolation failed, the exception would reach the native dispatch below and this
            // process would not survive to assert anything.
            Assert.That(() => Dispatch(instanceId, kFocusIn), Throws.Nothing);
            Assert.That(order, Is.EqualTo(new[] { 1, 2, 3 }),
                "a throwing listener skipped the ones after it");

            el.FocusGained -= first;
            el.FocusGained -= second;
            el.FocusGained -= third;
        }

        /// <summary>
        /// A listener that unsubscribes ITSELF from inside the dispatch, when it is the only one
        /// left. That takes the native subscription down while the native dispatch of that very
        /// subscription is still on the stack, and frees the GCHandle native handed the callback
        /// — the sharpest ordering this design has. It must not throw, must not fire again, and
        /// must leave the event re-subscribable.
        /// </summary>
        [Test]
        public void AListenerCanRemoveItselfDuringDispatch()
        {
            ulong instanceId = AddElement("hp-panel", "uielement");
            Ui.Element el = Ui.FindElement("hp-panel")!;

            int fired = 0;
            Action? self = null;
            self = () =>
            {
                ++fired;
                el.FocusGained -= self!;
            };
            el.FocusGained += self;
            Assert.That(SubscriptionCount(instanceId, kFocusIn), Is.EqualTo(1));

            Assert.That(() => Dispatch(instanceId, kFocusIn), Throws.Nothing);
            Assert.That(fired, Is.EqualTo(1));
            Assert.That(SubscriptionCount(instanceId, kFocusIn), Is.EqualTo(0),
                "a self-removing last listener left the native subscription behind");

            Dispatch(instanceId, kFocusIn);
            Assert.That(fired, Is.EqualTo(1), "the self-removed listener fired again");

            // Still usable afterwards: the slot was cleared, not poisoned.
            int again = 0;
            Action second = () => ++again;
            el.FocusGained += second;
            Assert.That(SubscriptionCount(instanceId, kFocusIn), Is.EqualTo(1));
            Dispatch(instanceId, kFocusIn);
            Assert.That(again, Is.EqualTo(1));
            el.FocusGained -= second;
        }

        /// <summary>
        /// The event payload survives the crossing. Pointer events carry position and modifiers;
        /// a click deliberately carries only modifiers, because keyboard activation has no
        /// position and there would be no honest value to put there.
        /// </summary>
        [Test]
        public void PointerEventsCarryTheirPayload()
        {
            ulong instanceId = AddElement("hp-panel", "uielement");
            Ui.Element el = Ui.FindElement("hp-panel")!;

            Ui.PointerArgs seen = default;
            int hits = 0;
            Action<Ui.PointerArgs> onMove = a => { seen = a; ++hits; };
            el.MouseMoved += onMove;

            Dispatch(instanceId, kMouseMove, x: 12.5f, y: 34.25f, mods: 3);

            Assert.That(hits, Is.EqualTo(1));
            Assert.That(seen.X, Is.EqualTo(12.5f));
            Assert.That(seen.Y, Is.EqualTo(34.25f));
            Assert.That(seen.Mods, Is.EqualTo(3));

            el.MouseMoved -= onMove;
        }

        /// <summary>
        /// Subscribing to an element that no longer resolves is a no-op, not an exception and
        /// not a subscription that silently never fires. The event stays subscribable so a
        /// re-resolved element can be wired again.
        /// </summary>
        [Test]
        public void SubscribingToADeadElementIsANoOp()
        {
            ulong instanceId = AddElement("hp-panel", "uielement");
            Ui.Element el = Ui.FindElement("hp-panel")!;
            GE_TestUI_DestroyElement(instanceId);
            Assert.That(el.IsAlive, Is.False);

            int fired = 0;
            Action listener = () => ++fired;
            Assert.That(() => el.FocusGained += listener, Throws.Nothing);
            Assert.That(SubscriptionCount(instanceId), Is.EqualTo(0),
                "a subscription was created on an element that does not resolve");
            Assert.That(() => el.FocusGained -= listener, Throws.Nothing);

            // The element-layer writes fail closed the same way, without throwing.
            Assert.That(el.SetWidthPercent(50f), Is.False);
            Assert.That(el.AddClass("low-health"), Is.False);
            Assert.That(fired, Is.EqualTo(0));
        }
    }
}
