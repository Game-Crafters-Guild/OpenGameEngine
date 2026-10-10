namespace GameEngine.Scripting
{
    /// <summary>
    /// Marker interface for user-defined ECS components.
    /// Must be an unmanaged struct with [StructLayout(LayoutKind.Sequential)].
    /// </summary>
    public interface IComponent { }
}
