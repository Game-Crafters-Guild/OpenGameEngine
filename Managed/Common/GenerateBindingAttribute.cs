using System;

namespace GameEngine.Interop
{
    /// <summary>
    /// Marks a managed method as a candidate for auto-generated native binding registration.
    /// The InteropGenerator will use this metadata to emit registration code and wrapper stubs.
    /// </summary>
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = false, Inherited = false)]
    public sealed class GenerateBindingAttribute : Attribute
    {
        /// <summary>
        /// The numeric callback ID. It must match the GE_CB_* value in ScriptingABI.h.
        /// </summary>
        public uint Id { get; }
        /// <summary>
        /// Optional logical subsystem name to help with ID allocation and documentation.
        /// </summary>
        public string? Subsystem { get; set; }
        /// <summary>
        /// Create a new GenerateBindingAttribute with the specified callback ID.
        /// </summary>
        public GenerateBindingAttribute(uint id) => Id = id;
    }
}

