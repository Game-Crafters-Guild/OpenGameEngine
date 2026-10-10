using System;

namespace GameEngine.Scripting
{
    public static partial class Ui
    {
        /// <summary>
        /// A native &lt;toggle&gt; or &lt;checkbox&gt; — both are the same bool-valued control
        /// family natively (ToggleBase), so <see cref="FindElement"/> returns this type for
        /// either tag.
        /// </summary>
        public class Toggle : Element
        {
            internal Toggle(ulong instanceId)
                : base(instanceId)
            {
            }

            /// <summary>Build a custom control over an existing toggle.</summary>
            protected Toggle(Toggle source)
                : base(source)
            {
            }

            /// <summary>
            /// The checked state. Setting it goes through the control's own SetValue, so the
            /// checked-class sync happens natively and the change is ANNOUNCED — members and
            /// subscribers hear it exactly as if the user clicked. Reads false when the
            /// element no longer resolves; a write on a dead element is a no-op.
            /// </summary>
            public bool IsChecked
            {
                get => GE_UIElement_GetValueBool(InstanceId, out int value) == 0 && value != 0;
                set => GE_UIElement_SetValueBool(InstanceId, value ? 1 : 0, 1);
            }

            /// <summary>
            /// Set the checked state and tell NOBODY — no member callback, no event, no script
            /// subscriber — while the value and its visuals still land. The API for driving a
            /// toggle from the state it reflects, so the write is never mistaken for the user
            /// changing it. Same name and contract as Unity's. False if the element no longer
            /// resolves.
            /// </summary>
            public bool SetValueWithoutNotify(bool value)
                => GE_UIElement_SetValueBool(InstanceId, value ? 1 : 0, 0) == 0;

            /// <summary>
            /// The toggle's value changed — by the user, or by a script write through
            /// <see cref="IsChecked"/>; never by <see cref="SetValueWithoutNotify"/>. One
            /// callback with the whole event; read <see cref="ValueArgs.AsBool"/>.
            /// <para>
            /// Subscription accounting, exception isolation and load-context rules are shared
            /// with every event here — see <see cref="Element.PointerEntered"/>.
            /// </para>
            /// </summary>
            public event Action<ValueArgs> ValueChanged
            {
                add => EventSubscription.Add(InstanceId, EventNames.ValueChanged, value);
                remove => EventSubscription.Remove(InstanceId, EventNames.ValueChanged, value);
            }
        }
    }
}
