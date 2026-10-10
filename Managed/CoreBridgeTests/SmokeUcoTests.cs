using System;
using NUnit.Framework;

namespace GameEngine.CoreBridgeTests;

/// <summary>Smoke tests to ensure UCO signatures can be resolved from CoreBridge.</summary>

public class SmokeUcoTests
{
    /// <summary>Ensures CoreBridge exposes required unmanaged entry points with expected names.</summary>
    [Test]
    public void CanResolveCoreBridgeUcoSignatures()
    {
        // CoreBridge is a project reference, so the build copies it beside the tests.
        string coreBridgePath = System.IO.Path.Combine(AppContext.BaseDirectory, "GameEngine.CoreBridge.dll");
        Assert.That(System.IO.File.Exists(coreBridgePath), $"CoreBridge not found at {coreBridgePath}");
        var asm = System.Runtime.Loader.AssemblyLoadContext.Default.LoadFromAssemblyPath(coreBridgePath);
        var type = asm.GetType("GameEngine.CoreBridge.CoreBridge", throwOnError: true)!;

        // Verify the method CoreCLRHost resolves by name exists
        var register = type.GetMethod("RegisterEngineInstance", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
        Assert.That(register, Is.Not.Null);
    }
}

