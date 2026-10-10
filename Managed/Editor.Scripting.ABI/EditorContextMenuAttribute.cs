using System;

namespace GameEngine.Editor.Scripting
{
    /// <summary>
    /// Declares an item to be added to one or more editor context menus.
    /// </summary>
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = true, Inherited = false)]
    public sealed class EditorContextMenuAttribute : Attribute
    {
        public EditorContextMenuAttribute(string path)
        {
            Path = path ?? string.Empty;
        }

        /// <summary>Menu path, e.g. "Assets/Create/MyAsset".</summary>
        public string Path { get; }

        /// <summary>Which context menus this item should appear in.</summary>
        public EditorContextMenuTarget Targets { get; set; } = EditorContextMenuTarget.AssetsItem;

        /// <summary>Optional sort priority (lower first).</summary>
        public int Priority { get; set; } = 0;
    }
}


