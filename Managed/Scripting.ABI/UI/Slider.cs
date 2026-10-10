using System;

namespace GameEngine.Scripting
{
    public static partial class Ui
    {
        /// <summary>
        /// A native &lt;slider&gt;. <see cref="FindElement"/> returns this type for one.
        /// </summary>
        public class Slider : Element
        {
            internal Slider(ulong instanceId)
                : base(instanceId)
            {
            }

            /// <summary>Build a custom control over an existing slider.</summary>
            protected Slider(Slider source)
                : base(source)
            {
            }

            /// <summary>
            /// The slider's value. Setting it goes through the control's own SetValue —
            /// clamped to the domain and quantised to the step natively, then ANNOUNCED, so a
            /// script assigning here gets exactly what a drag produces and reads back the
            /// committed value, not the requested one. Reads 0 when the element no longer
            /// resolves; a write on a dead element is a no-op.
            /// </summary>
            public float Value
            {
                get => GE_UIElement_GetValueFloat(InstanceId, out float value) == 0 ? value : 0f;
                set => GE_UIElement_SetValueFloat(InstanceId, value, 1);
            }

            /// <summary>
            /// Set the value and tell NOBODY, with the same native clamping and quantisation
            /// as <see cref="Value"/>. The API for driving a slider from the state it reflects
            /// (a per-frame HUD refresh), so the write is never mistaken for the user dragging
            /// it. Same name and contract as Unity's. False if the element no longer resolves.
            /// </summary>
            public bool SetValueWithoutNotify(float value)
                => GE_UIElement_SetValueFloat(InstanceId, value, 0) == 0;

            /// <summary>
            /// The value is changing — fired continuously while a drag is in flight, at
            /// pointer rate. Keep the listener cheap; the committed change arrives once, on
            /// <see cref="ValueChanged"/>.
            /// </summary>
            public event Action<ValueArgs> ValueChanging
            {
                add => EventSubscription.Add(InstanceId, EventNames.ValueChanging, value);
                remove => EventSubscription.Remove(InstanceId, EventNames.ValueChanging, value);
            }

            /// <summary>
            /// The value was committed — a drag released, a keyboard nudge, or a script write
            /// through <see cref="Value"/>; never <see cref="SetValueWithoutNotify"/>. The
            /// payload carries the committed (clamped, quantised) value.
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
