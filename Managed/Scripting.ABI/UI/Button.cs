using System;

namespace GameEngine.Scripting
{
    public static partial class Ui
    {
        /// <summary>
        /// A native &lt;button&gt;. <see cref="FindElement"/> returns this type for one.
        /// </summary>
        public class Button : Element
        {
            internal Button(ulong instanceId)
                : base(instanceId)
            {
            }

            /// <summary>Build a custom control over an existing button.</summary>
            protected Button(Button source)
                : base(source)
            {
            }

            /// <summary>
            /// The button was clicked — the control's OWN notion of a click, not a bare mouse-up
            /// over it. It fires when the press started on this button and the release is still
            /// inside, or on Space/Enter while focused. A press the pointer stream abandons (the
            /// cursor left the viewport still held down) is cancelled, not released, so it never
            /// becomes a click.
            /// <para>
            /// Subscribing here is additive: it can never displace a built-in control's click
            /// handler or another script's. The first <c>+=</c> creates one native subscription
            /// and the last <c>-=</c> removes it; see <see cref="Element.PointerEntered"/> for
            /// the full accounting, exception isolation and load-context rules, which are the
            /// same for every event here.
            /// </para>
            /// </summary>
            public event Action<ClickArgs> Clicked
            {
                add => EventSubscription.Add(InstanceId, EventNames.ButtonClick, value);
                remove => EventSubscription.Remove(InstanceId, EventNames.ButtonClick, value);
            }
        }
    }
}
