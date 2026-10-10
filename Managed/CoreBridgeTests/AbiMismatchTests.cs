using System;
using NUnit.Framework;
using GameEngine.Interop;

namespace GameEngine.CoreBridgeTests;

/// <summary>Engine instance binding tests.</summary>

public class AbiMismatchTests
{
/// <summary>Verifies an engine instance with a mismatched ABI override fails cleanly on first use.</summary>

    [Test]
    public void AbiMismatchFailsCleanly()
    {
        string path = NativeLibraryUnderTest.Resolve();
        uint badAbi = (EngineNativeBinding.kAbiMajor + 1u) << 16;

        // Binding is lazy BY DESIGN: constructing the instance loads nothing, so a
        // bad ABI override is accepted here.
        using var instance = new EngineInstanceContext(path, badAbi);

        // First use materializes the binding against the forced ABI and must
        // surface GE_GetInterface's rejection cleanly (rc in the message),
        // not crash and not silently bind a mismatched table.
        var ex = Assert.Throws<InvalidOperationException>(() => { _ = instance.Binding; });
        Assert.That(ex!.Message, Does.Contain("GE_GetInterface"));
    }
}
