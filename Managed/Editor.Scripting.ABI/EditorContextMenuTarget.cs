using System;

namespace GameEngine.Editor.Scripting
{
    /// <summary>
    /// Targets for editor context menu items.
    /// </summary>
    [Flags]
    public enum EditorContextMenuTarget : uint
    {
        None = 0,

        /// <summary>Right-click on an existing asset item.</summary>
        AssetsItem = 1u << 0,
        /// <summary>Right-click on empty space in the Assets panel.</summary>
        AssetsEmpty = 1u << 1,

        /// <summary>Right-click on an existing hierarchy item (scene object).</summary>
        HierarchyItem = 1u << 2,
        /// <summary>Right-click on empty space in the Hierarchy panel.</summary>
        HierarchyEmpty = 1u << 3,

        /// <summary>Right-click inside the Scene View viewport.</summary>
        SceneView = 1u << 4,

        AllAssets = AssetsItem | AssetsEmpty,
        AllHierarchy = HierarchyItem | HierarchyEmpty,
        All = 0xFFFF_FFFFu,
    }
}


