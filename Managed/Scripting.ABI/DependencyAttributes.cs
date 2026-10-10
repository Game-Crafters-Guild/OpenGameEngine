using System;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Declares that this system must execute after the specified system type.
    /// Multiple [After] attributes may be applied to declare multiple dependencies.
    /// </summary>
    [AttributeUsage(AttributeTargets.Struct | AttributeTargets.Class, AllowMultiple = true)]
    public sealed class AfterAttribute : Attribute
    {
        public Type SystemType { get; }
        public AfterAttribute(Type systemType) { SystemType = systemType; }
    }

    /// <summary>
    /// Declares that this system must execute before the specified system type.
    /// Multiple [Before] attributes may be applied to declare multiple dependencies.
    /// </summary>
    [AttributeUsage(AttributeTargets.Struct | AttributeTargets.Class, AllowMultiple = true)]
    public sealed class BeforeAttribute : Attribute
    {
        public Type SystemType { get; }
        public BeforeAttribute(Type systemType) { SystemType = systemType; }
    }
}
