namespace GameEngine.Scripting
{
    public static partial class Ui
    {
        /// <summary>
        /// A native &lt;label&gt;. <see cref="FindElement"/> returns this type for one.
        /// </summary>
        public class Label : Element
        {
            internal Label(ulong instanceId)
                : base(instanceId)
            {
            }

            /// <summary>Build a custom control over an existing label.</summary>
            protected Label(Label source)
                : base(source)
            {
            }

            /// <summary>
            /// Set the label's text. False if the element no longer resolves.
            /// <para>
            /// A .uxml hot-reload re-applies the authored <c>text</c> attribute, so a one-shot
            /// write is lost across one while a per-frame driver self-heals in a frame. Style
            /// written through the other calls here survives, because that is a different bag
            /// from the inline-style attribute.
            /// </para>
            /// </summary>
            public bool SetText(string text)
                => GE_UIElement_SetLabelText(InstanceId, text ?? string.Empty) == 0;
        }
    }
}
