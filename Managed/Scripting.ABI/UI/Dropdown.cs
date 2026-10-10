using System;

namespace GameEngine.Scripting
{
    public static partial class Ui
    {
        /// <summary>
        /// A native &lt;dropdown&gt;. <see cref="FindElement"/> returns this type for one.
        /// <para>
        /// A dropdown has three readable identities per selection: the INDEX, the option's
        /// VALUE string (the field value — what <see cref="SelectedValueChanged"/> carries),
        /// and the option's LABEL (what the header shows the user). Scripts drive selection by
        /// index; the option list itself is authored, not script-owned.
        /// </para>
        /// </summary>
        public class Dropdown : Element
        {
            internal Dropdown(ulong instanceId)
                : base(instanceId)
            {
            }

            /// <summary>Build a custom control over an existing dropdown.</summary>
            protected Dropdown(Dropdown source)
                : base(source)
            {
            }

            /// <summary>
            /// The selected option's index; -1 when nothing is selected, or when the element
            /// no longer resolves. Setting it goes through the control's own selection path —
            /// header label and item classes sync natively — and is ANNOUNCED. A write on a
            /// dead element is a no-op.
            /// </summary>
            public int SelectedIndex
            {
                get => GE_UIElement_GetDropdownSelectedIndex(InstanceId, out int index) == 0 ? index : -1;
                set => GE_UIElement_SetDropdownSelectedIndex(InstanceId, value, 1);
            }

            /// <summary>
            /// Select by index and tell NOBODY, while the header and item classes still sync.
            /// The API for programmatic refresh paths (mirroring the native
            /// SetSelectedIndexWithoutNotify). False if the element no longer resolves.
            /// </summary>
            public bool SetSelectedIndexWithoutNotify(int index)
                => GE_UIElement_SetDropdownSelectedIndex(InstanceId, index, 0) == 0;

            /// <summary>
            /// The selected option's VALUE string. Empty when nothing is selected or the
            /// element no longer resolves.
            /// </summary>
            public string SelectedValue => ReadElementText(InstanceId, TextSource.FieldValue);

            /// <summary>
            /// The selected option's LABEL — the text the header shows. Empty when nothing is
            /// selected or the element no longer resolves.
            /// </summary>
            public string SelectedLabel => ReadElementText(InstanceId, TextSource.DropdownLabel);

            /// <summary>
            /// The selection changed — by the user, or by a script write through
            /// <see cref="SelectedIndex"/>; never by <see cref="SetSelectedIndexWithoutNotify"/>.
            /// The payload is the option's VALUE string as a dispatch-window view (compare in
            /// the handler; <c>args.Text.ToString()</c> to keep it); read
            /// <see cref="SelectedIndex"/> or <see cref="SelectedLabel"/> for the other two
            /// identities.
            /// <para>
            /// Subscription accounting, exception isolation and load-context rules are shared
            /// with every event here — see <see cref="Element.PointerEntered"/>.
            /// </para>
            /// </summary>
            public event Action<TextValueArgs> SelectedValueChanged
            {
                add => EventSubscription.Add(InstanceId, EventNames.ValueChanged, value);
                remove => EventSubscription.Remove(InstanceId, EventNames.ValueChanged, value);
            }
        }
    }
}
