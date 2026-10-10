using System;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Binds a property or field of a <see cref="UiElementAttribute"/> type to an attribute
    /// authored on its <c>.uxml</c> tag.
    /// <para>
    /// <c>&lt;HealthBar percent="0.4" label="HP"/&gt;</c> assigns <c>0.4</c> and <c>"HP"</c> to the
    /// members bound below. Names match case-insensitively, and default to the member's own
    /// name, so the common case needs no argument:
    /// <code>
    /// [UiAttribute] public float Percent { get; set; }      // percent="0.4"
    /// [UiAttribute("label")] public string Caption = "";     // label="HP"
    /// </code>
    /// </para>
    /// <para>
    /// Values arrive as the document's raw strings and are converted to the member's type;
    /// <c>string</c>, the built-in numeric types, <c>bool</c> and <c>enum</c> are supported. A
    /// value that does not convert is reported once and the member is left at whatever the
    /// constructor set — a malformed attribute must not take the element down.
    /// </para>
    /// <para>
    /// Assignment happens after the constructor has run, on first build and again on every
    /// <c>.uxml</c> reconcile, which is what makes a hot-edited attribute reach a live instance.
    /// A re-materialized instance is replayed the element's current attributes, so it comes back
    /// configured rather than at its constructor defaults. A setter is therefore called more
    /// than once and must tolerate that; there is no ordering guarantee between members.
    /// </para>
    /// </summary>
    [AttributeUsage(AttributeTargets.Property | AttributeTargets.Field,
                    AllowMultiple = false, Inherited = true)]
    public sealed class UiAttributeAttribute : Attribute
    {
        /// <summary>
        /// The attribute name in the document. Empty means "use the member's name".
        /// </summary>
        public string Name { get; }

        public UiAttributeAttribute()
        {
            Name = string.Empty;
        }

        public UiAttributeAttribute(string name)
        {
            Name = name ?? string.Empty;
        }
    }
}
