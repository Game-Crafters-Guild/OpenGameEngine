using System.IO;
using GameEngine.Interop;
using NUnit.Framework;

namespace GameEngine.CoreBridgeTests;

/// <summary>
/// The GameEngine.Native library these tests register as a project's native path, resolved by
/// the engine's own loader: this platform's file name in the directories
/// NativeEngineLibrary.ProbeDirectories lists (the test output directory and GE_NATIVE_DIR among
/// them). Never the working directory or the repository.
/// </summary>
internal static class NativeLibraryUnderTest
{
    internal static string Resolve()
    {
        NativeEngineLibrary.Load(null, out string loadedFrom);
        Assert.That(File.Exists(loadedFrom), Is.True,
            $"GameEngine.Native loaded as '{loadedFrom}' through the OS search path, not from a file the tests can copy. " +
            "Stage it beside the test assembly or set GE_NATIVE_DIR to the directory that holds it.");
        return loadedFrom;
    }
}
