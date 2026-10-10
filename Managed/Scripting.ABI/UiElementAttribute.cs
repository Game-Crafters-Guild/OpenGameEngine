using System;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Declares a C# class as a UI element type the engine can build from a <c>.uxml</c> tag.
    /// <para>
    /// The class must derive from <see cref="Ui.Element"/> and have a public parameterless
    /// constructor. Once its assembly is loaded, <c>&lt;HealthBar/&gt;</c> in a document mints a
    /// native element and constructs one of these over it:
    /// <code>
    /// [UiElement("HealthBar")]
    /// public class HealthBar : Ui.Element
    /// {
    ///     [UiAttribute] public float Percent { set => SetWidthPercent(value * 100f); }
    /// }
    /// </code>
    /// The constructor runs at exactly the moment the element is created, so it is the
    /// construction hook — there is nothing virtual to override, and behaviour comes from
    /// subscribing to events (see <see cref="Ui.Element"/>).
    /// </para>
    /// <para>
    /// <b>The tag cannot take a name the engine already owns.</b> Registering
    /// <c>[UiElement("Button")]</c> is refused and logged, because replacing a built-in control
    /// process-wide and then deleting it when this assembly unloads is not a thing a script may
    /// do. Two scripts contending for one tag are refused the same way: first registration wins.
    /// </para>
    /// <para>
    /// <b>What happens on a hot reload.</b> Elements of this type are not destroyed when the
    /// assembly unloads. The native element stays in the tree with its children, layout and
    /// scroll position intact but inert, and a fresh instance of this class is attached when the
    /// type comes back. If a reload completes and the type is gone — deleted, renamed, or its
    /// assembly failed to compile — the element stays put and a developer build marks it. It is
    /// never deleted automatically, because it may hold authored children.
    /// </para>
    /// </summary>
    [AttributeUsage(AttributeTargets.Class, AllowMultiple = false, Inherited = false)]
    public sealed class UiElementAttribute : Attribute
    {
        /// <summary>The tag as written in <c>.uxml</c>. Matching is case-insensitive.</summary>
        public string Tag { get; }

        public UiElementAttribute(string tag)
        {
            Tag = tag ?? string.Empty;
        }
    }
}
