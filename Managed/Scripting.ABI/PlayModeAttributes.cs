using System;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Marks a static method as a Play Mode enter hook.
    /// Supported signatures:
    /// - void Method()
    /// - int Method()  (0 = success; negative = failure)
    /// </summary>
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = true, Inherited = false)]
    public sealed class PlayModeEnterAttribute : Attribute { }

    /// <summary>
    /// Marks a static method as a Play Mode exit hook.
    /// Supported signatures:
    /// - void Method()
    /// - int Method()  (0 = success; negative = failure)
    /// </summary>
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = true, Inherited = false)]
    public sealed class PlayModeExitAttribute : Attribute { }

    /// <summary>
    /// Marks a static method as a Play Mode tick hook.
    /// Supported signatures:
    /// - void Method(float deltaSeconds)
    /// - int Method(float deltaSeconds) (0 = success; negative = failure)
    /// </summary>
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = true, Inherited = false)]
    public sealed class PlayModeTickAttribute : Attribute { }
}

