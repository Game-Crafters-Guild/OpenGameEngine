using System;
using System.Runtime.InteropServices;
using NUnit.Framework;
using GameEngine.Scripting;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// The managed half of the STRING value-event payload: the pointer-and-length bytes
    /// reaching a handler as a <see cref="Ui.Utf8Text"/> view, the view's explicit-copy and
    /// allocation-free-comparison contract, and the dispatch path staying allocation-free —
    /// which is the entire reason the payload is a view and not a marshalled string.
    /// <para>
    /// Driven against the native shim's element double, like every binding test. What the
    /// double cannot speak for — that the REAL dispatch's bytes stay valid while a handler
    /// mutates the field — is pinned in UITests against the real controls and the real
    /// exports (ControlValueEventTests, GameUIElementHandleTests).
    /// </para>
    /// </summary>
    public class UiTextValueEventTests
    {
        private const string kLib = "GameEngine.Native";

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_AddElement(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string elementId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string tagLower, out ulong outInstanceId);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_DispatchTextPayload(ulong instanceId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string eventName,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string text, out int outInvoked);

        // The same export with RAW pointers, for the allocation arms: [MarshalAs(LPUTF8Str)]
        // allocates a native buffer per call, which would swamp what the arm measures. Both
        // strings are marshalled once, outside the window.
        [DllImport(kLib, EntryPoint = "GE_TestUI_DispatchTextPayload", CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_DispatchTextPayloadRaw(ulong instanceId, IntPtr eventName,
            IntPtr text, out int outInvoked);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_Reset();

        private const string kValueChanged = "UI.ValueChanged";

        /// <summary>Each test builds its own elements, so the double starts empty.</summary>
        [SetUp]
        public void Reset() => Assert.That(GE_TestUI_Reset(), Is.EqualTo(0));

        private static ulong AddElement(string elementId, string tagLower)
        {
            Assert.That(GE_TestUI_AddElement(elementId, tagLower, out ulong instanceId), Is.EqualTo(0));
            Assert.That(instanceId, Is.Not.Zero);
            return instanceId;
        }

        private static int RaiseText(ulong instanceId, string text)
        {
            Assert.That(GE_TestUI_DispatchTextPayload(instanceId, kValueChanged, text, out int invoked),
                        Is.EqualTo(0));
            return invoked;
        }

        /// <summary>
        /// The bytes survive the crossing and materialise to the same string — including
        /// multibyte codepoints, because the payload is UTF-8 bytes with an explicit length,
        /// never a NUL-scanned C string.
        /// </summary>
        [Test]
        public void TextChanged_DeliversTheTextToTheHandler()
        {
            ulong field = AddElement("name", "textfield");
            string? seen = null;
            Ui.EventSubscription.Add(field, kValueChanged,
                                     (Action<Ui.TextValueArgs>)(a => seen = a.Text.ToString()));

            const string kPayload = "héllo wörld 你好 🎮";
            Assert.That(RaiseText(field, kPayload), Is.EqualTo(1));
            Assert.That(seen, Is.EqualTo(kPayload));
        }

        /// <summary>An empty value is a VALUE: it arrives as an empty view, never null.</summary>
        [Test]
        public void EmptyTextArrivesEmpty()
        {
            ulong field = AddElement("name", "textfield");
            bool? isEmpty = null;
            string? seen = null;
            Ui.EventSubscription.Add(field, kValueChanged, (Action<Ui.TextValueArgs>)(a =>
            {
                isEmpty = a.Text.IsEmpty;
                seen = a.Text.ToString();
            }));

            Assert.That(RaiseText(field, string.Empty), Is.EqualTo(1));
            Assert.That(isEmpty, Is.True);
            Assert.That(seen, Is.EqualTo(string.Empty));
        }

        /// <summary>
        /// Equals(string) answers without materialising — including across its 64-char
        /// transcode chunks and through surrogate pairs, where a chunk boundary that split a
        /// pair would corrupt both halves into U+FFFD and lie in both directions.
        /// </summary>
        [Test]
        public void Utf8TextEqualsComparesCorrectlyAcrossChunksAndSurrogates()
        {
            ulong field = AddElement("name", "textfield");

            // 63 ASCII chars then a surrogate pair: the pair straddles the 64-char chunk take.
            string straddling = new string('x', 63) + "🎮" + new string('y', 100);
            string different = new string('x', 63) + "🎯" + new string('y', 100);

            bool? equalsSame = null;
            bool? equalsDifferent = null;
            bool? equalsShort = null;
            Ui.EventSubscription.Add(field, kValueChanged, (Action<Ui.TextValueArgs>)(a =>
            {
                equalsSame = a.Text.Equals(straddling);
                equalsDifferent = a.Text.Equals(different);
                equalsShort = a.Text.Equals("x");
            }));

            Assert.That(RaiseText(field, straddling), Is.EqualTo(1));
            Assert.That(equalsSame, Is.True);
            Assert.That(equalsDifferent, Is.False, "one codepoint differs, past the chunk boundary");
            Assert.That(equalsShort, Is.False, "the length gate must reject without comparing");
        }

        /// <summary>
        /// THE ARM for the payload's whole design: a single listener that only READS the view
        /// dispatches with zero managed allocation — no marshal copy, no string, nothing. The
        /// positive control is the same listener calling ToString(), which is the one place a
        /// copy is allowed to happen; it proves the meter reads this path at all.
        /// </summary>
        [Test]
        public void SingleListenerTextDispatchAllocatesNothing()
        {
            ulong field = AddElement("name", "textfield");
            int lengthSink = 0;
            Ui.EventSubscription.Add(field, kValueChanged,
                                     (Action<Ui.TextValueArgs>)(a => lengthSink += a.Text.Utf8Length));

            IntPtr name = Marshal.StringToCoTaskMemUTF8(kValueChanged);
            IntPtr text = Marshal.StringToCoTaskMemUTF8("forty-eight bytes of perfectly ordinary text..");
            try
            {
                long reading = MeasureTextDispatchAllocation(field, name, text);
                Assert.That(reading, Is.Zero,
                            "a handler that only reads the view must not cause a single managed "
                            + "allocation per dispatch — that is the point of the ref-struct payload");
                Assert.That(lengthSink, Is.GreaterThan(0), "control: the handler really ran");

                // Positive control on a fresh element: one listener that MATERIALISES.
                ulong copying = AddElement("copier", "textfield");
                string? last = null;
                Ui.EventSubscription.Add(copying, kValueChanged,
                                         (Action<Ui.TextValueArgs>)(a => last = a.Text.ToString()));
                long materialising = MeasureTextDispatchAllocation(copying, name, text);
                Assert.That(materialising, Is.GreaterThan(0),
                            "control: ToString() allocates, so the zero above is a measurement "
                            + "and not a dead meter");
                Assert.That(last, Is.Not.Null);
            }
            finally
            {
                Marshal.FreeCoTaskMem(name);
                Marshal.FreeCoTaskMem(text);
            }
        }

        private static long MeasureTextDispatchAllocation(ulong instanceId, IntPtr eventName, IntPtr text)
        {
            for (int i = 0; i < 64; ++i)
                Assert.That(GE_TestUI_DispatchTextPayloadRaw(instanceId, eventName, text, out _),
                            Is.EqualTo(0));

            long before = GC.GetAllocatedBytesForCurrentThread();
            for (int i = 0; i < 256; ++i)
                GE_TestUI_DispatchTextPayloadRaw(instanceId, eventName, text, out _);
            return GC.GetAllocatedBytesForCurrentThread() - before;
        }
    }
}
