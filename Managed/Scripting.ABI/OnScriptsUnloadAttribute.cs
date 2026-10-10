using System;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Marks a static, parameterless method to be invoked in the unloading scripts
    /// domain immediately before its AssemblyLoadContext unloads (hot-reload swap or
    /// explicit domain unload). Use it to stop threads and timers, cancel pending
    /// tasks, and unsubscribe from pinned static events — anything that would
    /// otherwise keep the old context alive and running against the live world.
    /// Handlers are time-boxed and exceptions are swallowed; the unload proceeds
    /// regardless.
    /// </summary>
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = false, Inherited = false)]
    public sealed class OnScriptsUnloadAttribute : Attribute
    {
    }
}
