using System;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Specifies component types to exclude from an IEntitySystem's query.
    /// Entities that have any of the specified components will be skipped.
    /// </summary>
    [AttributeUsage(AttributeTargets.Struct, AllowMultiple = false, Inherited = false)]
    public sealed class WithoutAttribute : Attribute
    {
        public Type[] ExcludedTypes { get; }

        public WithoutAttribute(params Type[] excludedTypes)
        {
            ExcludedTypes = excludedTypes ?? Array.Empty<Type>();
        }
    }
}
