using System;
using System.Runtime.InteropServices;
using NUnit.Framework;
using GameEngine.Scripting;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// The control wrappers of the first wave — Toggle, Slider, TextField, Dropdown,
    /// ScrollView: tag-typed minting, the typed properties routing the typed exports, and the
    /// notify seam (a property write announces; SetValueWithoutNotify tells nobody).
    /// <para>
    /// Driven against the native shim's element double, whose typed-accessor doubles mirror
    /// the real exports' type rules and notify semantics (Tests/NativeShim/
    /// GameEngineNativeShimUi.cpp). Whether the REAL controls clamp, announce and stay silent
    /// where documented is pinned in UITests against the real tree
    /// (GameUIElementHandleTests.TypedValueAccessorsRoundTripThroughTheRealExports).
    /// </para>
    /// </summary>
    public class UiControlWrapperTests
    {
        private const string kLib = "GameEngine.Native";

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_AddElement(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string elementId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string tagLower, out ulong outInstanceId);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_DestroyElement(ulong instanceId);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_SetDropdownData(ulong instanceId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string value,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string label);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_DispatchPayload(ulong instanceId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string eventName, float value, float scrollX,
            float scrollY, out int outInvoked);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_Reset();

        /// <summary>Each test builds its own elements, so the double starts empty.</summary>
        [SetUp]
        public void Reset() => Assert.That(GE_TestUI_Reset(), Is.EqualTo(0));

        private static T Find<T>(string elementId, string tagLower) where T : Ui.Element
        {
            Assert.That(GE_TestUI_AddElement(elementId, tagLower, out ulong instanceId), Is.EqualTo(0));
            Assert.That(instanceId, Is.Not.Zero);
            Ui.Element? element = Ui.FindElement(elementId);
            Assert.That(element, Is.Not.Null);
            Assert.That(element, Is.InstanceOf<T>(),
                        $"tag '{tagLower}' must mint {typeof(T).Name}, got {element!.GetType().Name}");
            return (T)element;
        }

        /// <summary>
        /// Every first-wave tag mints its typed wrapper — including <c>checkbox</c>, which is
        /// the same bool-valued native family as <c>toggle</c> and shares its wrapper. An
        /// unmapped tag still falls back to the base wrapper.
        /// </summary>
        [Test]
        public void FindElementMintsTheTypedWrapperPerTag()
        {
            Assert.That(Find<Ui.Toggle>("t", "toggle"), Is.Not.Null);
            Assert.That(Find<Ui.Toggle>("c", "checkbox"), Is.Not.Null);
            Assert.That(Find<Ui.Slider>("s", "slider"), Is.Not.Null);
            Assert.That(Find<Ui.TextField>("f", "textfield"), Is.Not.Null);
            Assert.That(Find<Ui.Dropdown>("d", "dropdown"), Is.Not.Null);
            Assert.That(Find<Ui.ScrollView>("v", "scrollview"), Is.Not.Null);

            Assert.That(GE_TestUI_AddElement("plain", "uielement", out ulong plainId), Is.EqualTo(0));
            Ui.Element? plain = Ui.FindElement("plain");
            Assert.That(plain, Is.Not.Null);
            Assert.That(plain!.GetType(), Is.EqualTo(typeof(Ui.Element)));
            _ = plainId;
        }

        /// <summary>
        /// The notify seam on a Slider: assigning Value announces (the handler hears the
        /// change), SetValueWithoutNotify moves the value silently, and the getter reads back
        /// whatever landed.
        /// </summary>
        [Test]
        public void SliderValueAnnouncesAndWithoutNotifyStaysSilent()
        {
            Ui.Slider slider = Find<Ui.Slider>("vol", "slider");
            float seen = float.NaN;
            int events = 0;
            slider.ValueChanged += a =>
            {
                ++events;
                seen = a.Value;
            };

            slider.Value = 4.0f;
            Assert.That(events, Is.EqualTo(1), "a property write is a user-equivalent change");
            Assert.That(seen, Is.EqualTo(4.0f));
            Assert.That(slider.Value, Is.EqualTo(4.0f));

            Assert.That(slider.SetValueWithoutNotify(7.0f), Is.True);
            Assert.That(events, Is.EqualTo(1), "SetValueWithoutNotify must tell nobody");
            Assert.That(slider.Value, Is.EqualTo(7.0f), "the silent write still lands");

            slider.Value = 7.0f;
            Assert.That(events, Is.EqualTo(1), "an unchanged write announces nothing");
        }

        /// <summary>The same notify seam on a Toggle: the bool travels as 0/1.</summary>
        [Test]
        public void ToggleIsCheckedAnnouncesAndWithoutNotifyStaysSilent()
        {
            Ui.Toggle toggle = Find<Ui.Toggle>("mute", "toggle");
            bool? seen = null;
            int events = 0;
            toggle.ValueChanged += a =>
            {
                ++events;
                seen = a.AsBool;
            };

            toggle.IsChecked = true;
            Assert.That(events, Is.EqualTo(1));
            Assert.That(seen, Is.True);
            Assert.That(toggle.IsChecked, Is.True);

            Assert.That(toggle.SetValueWithoutNotify(false), Is.True);
            Assert.That(events, Is.EqualTo(1));
            Assert.That(toggle.IsChecked, Is.False);
        }

        /// <summary>
        /// TextField.Text round-trips through the UTF-8 buffer contract — including a value
        /// longer than the binding's 256-byte stack buffer, which exercises the exact-size
        /// retry. Programmatic writes announce nothing (the native control's own contract,
        /// mirrored by the double and pinned engine-side).
        /// </summary>
        [Test]
        public void TextFieldTextRoundTripsAndProgrammaticWritesAreSilent()
        {
            Ui.TextField field = Find<Ui.TextField>("name", "textfield");
            int events = 0;
            field.TextChanged += _ => ++events;

            field.Text = "héllo 🎮";
            Assert.That(field.Text, Is.EqualTo("héllo 🎮"));

            string longValue = new string('か', 200); // 600 UTF-8 bytes: past the stack buffer
            Assert.That(field.SetValueWithoutNotify(longValue), Is.True);
            Assert.That(field.Text, Is.EqualTo(longValue), "the exact-size retry path must round-trip");

            Assert.That(events, Is.Zero,
                        "TextFieldBase's programmatic SetValue is a silent sync today; the wrapper "
                        + "docs say so, and the double must not invent an announcement");
        }

        /// <summary>
        /// Index round-trips, the value/label getters read the selection's two string
        /// identities, and the selection event carries the option VALUE string.
        /// </summary>
        [Test]
        public void DropdownSelectionRoundTripsAndAnnouncesTheOptionValue()
        {
            Ui.Dropdown dropdown = Find<Ui.Dropdown>("mode", "dropdown");

            string? seenValue = null;
            int events = 0;
            dropdown.SelectedValueChanged += a =>
            {
                ++events;
                seenValue = a.Text.ToString();
            };

            // Seed what the double's selection resolves to — the real control derives both
            // from its option list, which the double does not carry.
            Assert.That(GE_TestUI_SetDropdownData(FindInstanceId("mode"), "val-b", "Label B"),
                        Is.EqualTo(0));

            dropdown.SelectedIndex = 1;
            Assert.That(events, Is.EqualTo(1));
            Assert.That(seenValue, Is.EqualTo("val-b"), "the event carries the option VALUE string");
            Assert.That(dropdown.SelectedIndex, Is.EqualTo(1));
            Assert.That(dropdown.SelectedValue, Is.EqualTo("val-b"));
            Assert.That(dropdown.SelectedLabel, Is.EqualTo("Label B"));

            Assert.That(dropdown.SetSelectedIndexWithoutNotify(0), Is.True);
            Assert.That(events, Is.EqualTo(1), "the without-notify selection told nobody");
            Assert.That(dropdown.SelectedIndex, Is.EqualTo(0));
        }

        /// <summary>Offsets round-trip per axis, and the settled event carries both.</summary>
        [Test]
        public void ScrollViewOffsetsRoundTripAndTheEventCarriesBoth()
        {
            Ui.ScrollView view = Find<Ui.ScrollView>("list", "scrollview");
            view.ScrollX = 12f;
            view.ScrollY = 340f;
            Assert.That(view.ScrollX, Is.EqualTo(12f));
            Assert.That(view.ScrollY, Is.EqualTo(340f));

            float x = float.NaN;
            float y = float.NaN;
            view.ScrollOffsetChanged += a =>
            {
                x = a.ScrollX;
                y = a.ScrollY;
            };
            // The offset event rides the manager's coalesced flush, which the double does not
            // simulate; raise it through the dispatch driver as the settled offsets.
            Assert.That(GE_TestUI_DispatchPayload(FindInstanceId("list"), "UI.ScrollOffsetChanged",
                                                  0f, 12f, 340f, out int invoked), Is.EqualTo(0));
            Assert.That(invoked, Is.EqualTo(1));
            Assert.That(x, Is.EqualTo(12f));
            Assert.That(y, Is.EqualTo(340f));
        }

        /// <summary>
        /// Dead elements fail closed: getters report defaults, setters report false, and
        /// nothing throws — the wrapper contract everywhere.
        /// </summary>
        [Test]
        public void DeadElementsReadDefaultsAndRefuseWrites()
        {
            Ui.Slider slider = Find<Ui.Slider>("vol", "slider");
            slider.Value = 5f;
            Assert.That(GE_TestUI_DestroyElement(FindInstanceId("vol")), Is.EqualTo(0));

            Assert.That(slider.IsAlive, Is.False);
            Assert.That(slider.Value, Is.EqualTo(0f), "a dead element reads the default, not stale state");
            Assert.That(slider.SetValueWithoutNotify(9f), Is.False);

            Ui.TextField field = Find<Ui.TextField>("name", "textfield");
            field.Text = "hello";
            Assert.That(GE_TestUI_DestroyElement(FindInstanceId("name")), Is.EqualTo(0));
            Assert.That(field.Text, Is.EqualTo(string.Empty));
            Assert.That(field.SetValueWithoutNotify("x"), Is.False);

            Ui.Dropdown dropdown = Find<Ui.Dropdown>("mode", "dropdown");
            Assert.That(GE_TestUI_DestroyElement(FindInstanceId("mode")), Is.EqualTo(0));
            Assert.That(dropdown.SelectedIndex, Is.EqualTo(-1));
            Assert.That(dropdown.SelectedValue, Is.EqualTo(string.Empty));
        }

        // The double's find-by-id, so a test can address the element it just wrapped without
        // reaching into the wrapper's internals.
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_GameUI_FindElement(ulong entityId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string id, out ulong outInstanceId);

        private static ulong FindInstanceId(string elementId)
        {
            Assert.That(GE_GameUI_FindElement(0, elementId, out ulong instanceId), Is.EqualTo(0));
            return instanceId;
        }
    }
}
