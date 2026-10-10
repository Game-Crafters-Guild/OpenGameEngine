namespace GameEngine.Scripting
{
    /// <summary>
    /// The kinds of object in a model's source file whose extras the model keeps: a glTF file's scenes,
    /// nodes, meshes, materials and animations. The values are the native <c>GE_ModelObjectKind</c>'s.
    /// </summary>
    public enum ModelObjectKind : uint
    {
        Scene = 0,
        Node = 1,
        Mesh = 2,
        Material = 3,
        Animation = 4
    }
}
