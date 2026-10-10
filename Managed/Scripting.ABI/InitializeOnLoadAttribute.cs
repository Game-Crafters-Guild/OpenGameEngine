using System;

namespace GameEngine.Scripting
{
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = false, Inherited = false)]
    public sealed class InitializeOnLoadAttribute : Attribute
    {
    }
}

