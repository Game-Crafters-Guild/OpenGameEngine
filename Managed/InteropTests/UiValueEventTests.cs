using System;
using System.Runtime.InteropServices;
using NUnit.Framework;
using GameEngine.Scripting;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// The managed half of the control value events: the widened payload reaching the args
    /// structs, and the single-listener fast path that keeps a per-frame event from allocating.
    /// <para>
    /// Driven against the native shim's element double for the same reason the wrapper tests
    /// are (a dotnet-test host has no window, device or mounted document). What the double
    /// cannot speak for — whether a Slider actually dispatches on SetValue, whether the payload
    /// carries the clamped value — is pinned in UITests against the real controls.
    /// </para>
    /// </summary>
    public class UiValueEventTests
    {
        private const string kLib = "GameEngine.Native";

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_AddElement(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string elementId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string tagLower, out ulong outInstanceId);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_DispatchPayload(ulong instanceId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string eventName, float value, float scrollX,
            float scrollY, out int outInvoked);

        // The same export, declared with a RAW pointer for the name. The allocation arms below
        // measure the dispatch, and [MarshalAs(LPUTF8Str)] allocates a native buffer per call —
        // hundreds of bytes that would swamp the Delegate[] they exist to detect. The name is
        // marshalled once, outside the measurement window.
        [DllImport(kLib, EntryPoint = "GE_TestUI_DispatchPayload", CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_DispatchPayloadRaw(ulong instanceId, IntPtr eventName,
            float value, float scrollX, float scrollY, out int outInvoked);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_SubscriptionCount(ulong instanceId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? eventName, out int outCount);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_Reset();

        private const string kValueChanged = "UI.ValueChanged";
        private const string kScrollOffsetChanged = "UI.ScrollOffsetChanged";

        /// <summary>Each test builds its own elements, so the double starts empty.</summary>
        [SetUp]
        public void Reset() => Assert.That(GE_TestUI_Reset(), Is.EqualTo(0));

        private static ulong AddElement(string elementId, string tagLower)
        {
            Assert.That(GE_TestUI_AddElement(elementId, tagLower, out ulong instanceId), Is.EqualTo(0));
            Assert.That(instanceId, Is.Not.Zero);
            return instanceId;
        }

        private static int Raise(ulong instanceId, string eventName, float value = 0f,
                                float scrollX = 0f, float scrollY = 0f)
        {
            Assert.That(GE_TestUI_DispatchPayload(instanceId, eventName, value, scrollX, scrollY,
                                                  out int invoked), Is.EqualTo(0));
            return invoked;
        }

        /// <summary>
        /// The payload the ABI widened for reaches the handler as the value it was given. Without
        /// this the whole widening is unobservable from C#.
        /// </summary>
        [Test]
        public void ValueChanged_DeliversTheValueToTheHandler()
        {
            ulong slider = AddElement("vol", "slider");
            float seen = float.NaN;
            Ui.EventSubscription.Add(slider, kValueChanged, (Action<Ui.ValueArgs>)(a => seen = a.Value));

            Assert.That(Raise(slider, kValueChanged, value: 0.25f), Is.EqualTo(1));
            Assert.That(seen, Is.EqualTo(0.25f));
        }

        /// <summary>A bool control reports 0 or 1, and AsBool is what makes that readable.</summary>
        [Test]
        public void ValueChanged_ReportsABoolAsZeroOrOne()
        {
            ulong toggle = AddElement("mute", "checkbox");
            bool? seen = null;
            Ui.EventSubscription.Add(toggle, kValueChanged, (Action<Ui.ValueArgs>)(a => seen = a.AsBool));

            Raise(toggle, kValueChanged, value: 1f);
            Assert.That(seen, Is.True);

            Raise(toggle, kValueChanged, value: 0f);
            Assert.That(seen, Is.False);
        }

        /// <summary>
        /// A scroll offset is two numbers, and both must survive the crossing — an event that
        /// carried only one axis would be useless to a virtualized list.
        /// </summary>
        [Test]
        public void ScrollOffsetChanged_DeliversBothOffsets()
        {
            ulong view = AddElement("list", "scrollview");
            float x = float.NaN;
            float y = float.NaN;
            Ui.EventSubscription.Add(view, kScrollOffsetChanged, (Action<Ui.ScrollOffsetArgs>)(a =>
            {
                x = a.ScrollX;
                y = a.ScrollY;
            }));

            Assert.That(Raise(view, kScrollOffsetChanged, scrollX: 12f, scrollY: 340f), Is.EqualTo(1));
            Assert.That(x, Is.EqualTo(12f));
            Assert.That(y, Is.EqualTo(340f));
        }

        /// <summary>
        /// THE ARM for the fast path. One listener — the overwhelmingly common case — must
        /// dispatch without allocating, because a dragged slider and a scrolled view raise this
        /// every frame and GetInvocationList() returns a fresh Delegate[] every call.
        /// <para>
        /// The two-listener case is the positive control: it is what proves the meter reads
        /// anything at all, so a zero on the one-listener case cannot be "nothing was measured".
        /// </para>
        /// </summary>
        [Test]
        public void SingleListenerDispatchAllocatesNothing()
        {
            ulong slider = AddElement("vol", "slider");
            float sink = 0f;
            Ui.EventSubscription.Add(slider, kValueChanged, (Action<Ui.ValueArgs>)(a => sink += a.Value));

            IntPtr name = Marshal.StringToCoTaskMemUTF8(kValueChanged);
            try
            {
                long single = MeasureDispatchAllocation(slider, name);

                // Positive control: a second listener puts dispatch back on GetInvocationList().
                Ui.EventSubscription.Add(slider, kValueChanged, (Action<Ui.ValueArgs>)(a => sink -= a.Value));
                long multi = MeasureDispatchAllocation(slider, name);

                Assert.That(single, Is.Zero,
                            "one listener must dispatch with no managed allocation at all");
                Assert.That(multi, Is.GreaterThan(0),
                            "control: the meter does read allocations, so the zero above is a result "
                            + "and not a dead measurement");
            }
            finally
            {
                Marshal.FreeCoTaskMem(name);
            }
        }

        /// <summary>
        /// Bytes allocated by 256 dispatches, after a warm-up that keeps first-call JIT out of the
        /// window. The event name is already marshalled, so what is left is the dispatch.
        /// </summary>
        private static long MeasureDispatchAllocation(ulong instanceId, IntPtr eventName)
        {
            for (int i = 0; i < 64; ++i)
                Assert.That(GE_TestUI_DispatchPayloadRaw(instanceId, eventName, 1f, 0f, 0f, out _),
                            Is.EqualTo(0));

            long before = GC.GetAllocatedBytesForCurrentThread();
            for (int i = 0; i < 256; ++i)
                GE_TestUI_DispatchPayloadRaw(instanceId, eventName, 1f, 0f, 0f, out _);
            return GC.GetAllocatedBytesForCurrentThread() - before;
        }

        /// <summary>
        /// Dropping back to one listener must re-engage the fast path. Without this the first
        /// second listener a HUD ever adds would cost it the allocation-free path permanently.
        /// </summary>
        [Test]
        public void RemovingTheSecondListenerRestoresTheFastPath()
        {
            ulong slider = AddElement("vol", "slider");
            float sink = 0f;
            Action<Ui.ValueArgs> first = a => sink += a.Value;
            Action<Ui.ValueArgs> second = a => sink -= a.Value;
            Ui.EventSubscription.Add(slider, kValueChanged, first);
            Ui.EventSubscription.Add(slider, kValueChanged, second);
            Ui.EventSubscription.Remove(slider, kValueChanged, second);

            IntPtr name = Marshal.StringToCoTaskMemUTF8(kValueChanged);
            try
            {
                Assert.That(MeasureDispatchAllocation(slider, name), Is.Zero);
            }
            finally
            {
                Marshal.FreeCoTaskMem(name);
            }
            Assert.That(GE_TestUI_SubscriptionCount(slider, kValueChanged, out int native), Is.EqualTo(0));
            Assert.That(native, Is.EqualTo(1), "the native entry is still the single shared one");
        }

        /// <summary>
        /// Two listeners still both run, and one throwing must not take the other with it. The
        /// fast path is an allocation change, not a semantics change — this is the arm that
        /// would catch it becoming one.
        /// </summary>
        [Test]
        public void EveryListenerRunsAndAThrowingOneIsIsolated()
        {
            ulong slider = AddElement("vol", "slider");
            int good = 0;
            Ui.EventSubscription.Add(slider, kValueChanged,
                                     (Action<Ui.ValueArgs>)(_ => throw new InvalidOperationException("boom")));
            Ui.EventSubscription.Add(slider, kValueChanged, (Action<Ui.ValueArgs>)(_ => ++good));

            Assert.That(Raise(slider, kValueChanged, value: 1f), Is.EqualTo(1));
            Assert.That(good, Is.EqualTo(1), "the throwing listener must not stop the next one");

            // And alone on the fast path, a throw still must not cross the cdecl boundary.
            ulong toggle = AddElement("mute", "checkbox");
            Ui.EventSubscription.Add(toggle, kValueChanged,
                                     (Action<Ui.ValueArgs>)(_ => throw new InvalidOperationException("boom")));
            Assert.That(Raise(toggle, kValueChanged, value: 1f), Is.EqualTo(1));
        }

        /// <summary>
        /// A subscription that could never fire is refused natively, and the managed side must
        /// leave nothing behind when it is: no native entry, no rooted listener.
        /// </summary>
        [Test]
        public void SubscribingWhereNothingDispatchesLeavesNothingBehind()
        {
            ulong plain = AddElement("plain", "uielement");
            Ui.EventSubscription.Add(plain, kValueChanged, (Action<Ui.ValueArgs>)(_ => { }));

            Assert.That(GE_TestUI_SubscriptionCount(plain, kValueChanged, out int native), Is.EqualTo(0));
            Assert.That(native, Is.Zero, "the native export refused it, so nothing may be registered");
            Assert.That(Raise(plain, kValueChanged, value: 1f), Is.Zero);
        }
    }
}
