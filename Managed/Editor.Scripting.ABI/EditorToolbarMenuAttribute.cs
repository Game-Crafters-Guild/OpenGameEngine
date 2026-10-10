using System;

namespace GameEngine.Editor.Scripting
{
    /// <summary>
    /// Declares a menu item under the native Editor toolbar/menu bar.
    /// </summary>
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = true, Inherited = false)]
    public sealed class EditorToolbarMenuAttribute : Attribute
    {
        public EditorToolbarMenuAttribute(string path)
        {
            Path = path ?? string.Empty;
        }

        /// <summary>Menu path, e.g. "Tools/MyAction".</summary>
        public string Path { get; }

        /// <summary>Optional sort priority (lower first).</summary>
        public int Priority { get; set; } = 0;
    }

    /// <summary>
    /// Alias for <see cref="EditorToolbarMenuAttribute"/> (convenience).
    /// </summary>
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = true, Inherited = false)]
    public sealed class EditorToolbarItemAttribute : Attribute
    {
        public EditorToolbarItemAttribute(string path)
        {
            Path = path ?? string.Empty;
        }

        /// <summary>Menu path, e.g. "Tools/MyAction".</summary>
        public string Path { get; }

        /// <summary>Optional sort priority (lower first).</summary>
        public int Priority { get; set; } = 0;
    }
}


