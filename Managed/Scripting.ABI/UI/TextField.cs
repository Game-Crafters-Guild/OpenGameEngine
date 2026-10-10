using System;

namespace GameEngine.Scripting
{
    public static partial class Ui
    {
        /// <summary>
        /// A native &lt;TextField&gt; (tags <c>textfield</c> and the legacy <c>input</c>).
        /// <see cref="FindElement"/> returns this type for one.
        /// </summary>
        public class TextField : Element
        {
            internal TextField(ulong instanceId)
                : base(instanceId)
            {
            }

            /// <summary>Build a custom control over an existing text field.</summary>
            protected TextField(TextField source)
                : base(source)
            {
            }

            /// <summary>
            /// The field's text. Reads empty when the element no longer resolves; a write on a
            /// dead element is a no-op.
            /// <para>
            /// KNOWN ASYMMETRY, inherited from the native control and pinned by its tests: a
            /// programmatic write here is a silent sync — TextFieldBase notifies only from
            /// user editing paths — so assigning this property does NOT raise
            /// <see cref="TextChanged"/> today, unlike <see cref="Slider.Value"/> and
            /// <see cref="Toggle.IsChecked"/>, which announce. If the native contract ever
            /// changes to announce, this note goes with it.
            /// </para>
            /// </summary>
            public string Text
            {
                get => ReadElementText(InstanceId, TextSource.FieldValue);
                set => GE_UIElement_SetValueText(InstanceId, value ?? string.Empty, 1);
            }

            /// <summary>
            /// Set the text with the explicit no-announcement contract — the write that must
            /// never be mistaken for the user typing, whatever the native notify contract of
            /// plain SetValue is or becomes. Same name and contract as Unity's. False if the
            /// element no longer resolves.
            /// </summary>
            public bool SetValueWithoutNotify(string text)
                => GE_UIElement_SetValueText(InstanceId, text ?? string.Empty, 0) == 0;

            /// <summary>
            /// The text is changing — fired per edit while the user types. The payload is a
            /// dispatch-window view: compare or copy it in the handler, and call
            /// <c>args.Text.ToString()</c> if you keep it.
            /// </summary>
            public event Action<TextValueArgs> TextChanging
            {
                add => EventSubscription.Add(InstanceId, EventNames.ValueChanging, value);
                remove => EventSubscription.Remove(InstanceId, EventNames.ValueChanging, value);
            }

            /// <summary>
            /// The text was committed — Enter, or blur after an edit. The payload rules are
            /// <see cref="TextChanging"/>'s; the subscription accounting, exception isolation
            /// and load-context rules are shared with every event here — see
            /// <see cref="Element.PointerEntered"/>.
            /// </summary>
            public event Action<TextValueArgs> TextChanged
            {
                add => EventSubscription.Add(InstanceId, EventNames.ValueChanged, value);
                remove => EventSubscription.Remove(InstanceId, EventNames.ValueChanged, value);
            }
        }
    }
}
