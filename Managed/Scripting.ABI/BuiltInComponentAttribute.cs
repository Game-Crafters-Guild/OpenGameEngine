using System;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Marks a C# struct as a mirror of a C++ engine component.
    /// The source generator will use GetComponentTypeIdByName to resolve the
    /// native type ID instead of RegisterBlobComponent, and the component will
    /// participate in slice-based iteration that correctly handles native
    /// ComponentArray chunk boundaries.
    /// </summary>
    [AttributeUsage(AttributeTargets.Struct, AllowMultiple = false, Inherited = false)]
    public sealed class BuiltInComponentAttribute : Attribute
    {
        /// <summary>
        /// The C++ component name as registered in ComponentRegistry
        /// (e.g. "GameEngine::components::Position").
        /// </summary>
        public string NativeName { get; }

        public BuiltInComponentAttribute(string nativeName)
        {
            NativeName = nativeName ?? throw new ArgumentNullException(nameof(nativeName));
        }
    }
}
