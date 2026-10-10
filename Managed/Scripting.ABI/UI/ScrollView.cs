using System;

namespace GameEngine.Scripting
{
    public static partial class Ui
    {
        /// <summary>
        /// A native &lt;scrollview&gt;. <see cref="FindElement"/> returns this type for one.
        /// </summary>
        public class ScrollView : Element
        {
            internal ScrollView(ulong instanceId)
                : base(instanceId)
            {
            }

            /// <summary>Build a custom control over an existing scroll view.</summary>
            protected ScrollView(ScrollView source)
                : base(source)
            {
            }

            /// <summary>
            /// Horizontal scroll offset in pixels. Writes clamp to the scrollable range and
            /// COALESCE: however many offsets a frame writes, subscribers hear one settled
            /// offset at the engine's flush. Reads 0 when the element no longer resolves.
            /// </summary>
            public float ScrollX
            {
                get => GE_UIElement_GetScrollOffset(InstanceId, out float x, out _) == 0 ? x : 0f;
                set => GE_UIElement_SetScrollX(InstanceId, value);
            }

            /// <summary>Vertical scroll offset in pixels; the rules are <see cref="ScrollX"/>'s.</summary>
            public float ScrollY
            {
                get => GE_UIElement_GetScrollOffset(InstanceId, out _, out float y) == 0 ? y : 0f;
                set => GE_UIElement_SetScrollY(InstanceId, value);
            }

            /// <summary>
            /// The scroll OFFSET settled somewhere new — not the wheel input. One event per
            /// frame however many writes produced it, carrying where the view ended up.
            /// <para>
            /// There is deliberately no SetScrollWithoutNotify: the native deferral already is
            /// one. A handler writing an offset from inside this event re-arms the NEXT flush
            /// instead of re-entering this one, so the feedback loop SetValueWithoutNotify
            /// exists to break cannot form here (ScrollView.h documents the invariant).
            /// </para>
            /// <para>
            /// Subscription accounting, exception isolation and load-context rules are shared
            /// with every event here — see <see cref="Element.PointerEntered"/>.
            /// </para>
            /// </summary>
            public event Action<ScrollOffsetArgs> ScrollOffsetChanged
            {
                add => EventSubscription.Add(InstanceId, EventNames.ScrollOffsetChanged, value);
                remove => EventSubscription.Remove(InstanceId, EventNames.ScrollOffsetChanged, value);
            }
        }
    }
}
