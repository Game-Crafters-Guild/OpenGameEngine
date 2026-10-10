using System;
using System.IO;
using System.Linq;
using GameEngine.Interop;
using NUnit.Framework;

namespace GameEngine.CoreBridgeTests;

/// <summary>
/// The directories the shared native loader probes for GameEngine.Native. The bundles ship the
/// macOS shim in Contents/Frameworks, beside the executable's Contents/MacOS.
/// </summary>
public class NativeEngineLibraryProbeTests
{
    private static readonly string s_BundleContents = Path.Combine(Path.GetTempPath(), "Game.app", "Contents");
    private static readonly string s_MacOSDir = Path.Combine(s_BundleContents, "MacOS");
    private static readonly string s_FrameworksDir = Path.Combine(s_BundleContents, "Frameworks");
    private static readonly string s_ManagedDir = Path.Combine(s_BundleContents, "Resources", "Managed");
    private static readonly string s_AssemblyDir = Path.Combine(s_MacOSDir, "SDK", "managed");
    private static readonly string s_NativeDir = Path.Combine(Path.GetTempPath(), "native");

    /// <summary>
    /// A NativeAOT game in a macOS bundle: the scripts library sits beside the executable in
    /// Contents/MacOS and has no assembly location, so Contents/Frameworks is the probe that finds the shim.
    /// </summary>
    [Test]
    public void MacBundleProbesContentsFrameworksAfterTheExecutableDirectory()
    {
        var probed = NativeEngineLibrary.ProbeDirectories(isMacOS: true, baseDirectory: s_MacOSDir,
            executableDirectory: s_MacOSDir, assemblyDirectory: null, nativeDir: null).ToArray();

        Assert.That(probed, Is.EqualTo(new[] { s_MacOSDir, s_FrameworksDir }));
    }

    /// <summary>
    /// A CoreCLR-hosted bundle whose base directory is not the executable's: Contents/Frameworks is
    /// anchored on the executable, and like the executable's directory it precedes both the
    /// assembly's directory and GE_NATIVE_DIR.
    /// </summary>
    [Test]
    public void MacBundleAnchorsFrameworksOnTheExecutableAndProbesItBeforeTheAssemblyDirectoryAndNativeDir()
    {
        var probed = NativeEngineLibrary.ProbeDirectories(isMacOS: true, baseDirectory: s_ManagedDir,
            executableDirectory: s_MacOSDir, assemblyDirectory: s_AssemblyDir, nativeDir: s_NativeDir).ToArray();

        Assert.That(probed, Is.EqualTo(new[] { s_ManagedDir, s_FrameworksDir, s_AssemblyDir, s_NativeDir }));
    }

    /// <summary>
    /// The loader's own inputs on a macOS host: the platform is detected and Contents/Frameworks is
    /// anchored on Environment.ProcessPath, not on AppContext.BaseDirectory. The test host's
    /// executable (the dotnet muxer) is not in the test output directory, so the two anchors differ here.
    /// </summary>
    [Test]
    public void OnMacOSTheLoaderProbesFrameworksBesideTheRunningExecutable()
    {
        if (!OperatingSystem.IsMacOS())
            Assert.Ignore("Probes Contents/Frameworks on macOS hosts only.");

        string executableDir = Path.GetDirectoryName(Environment.ProcessPath)!;
        string expected = Path.Combine(Path.GetFullPath(Path.Combine(executableDir, "..", "Frameworks")),
            NativeEngineLibrary.FileName);

        Assert.That(NativeEngineLibrary.Candidates(explicitPath: null, nativeDir: null), Does.Contain(expected));
    }

    /// <summary>Windows and Linux probe the base, assembly and GE_NATIVE_DIR directories only.</summary>
    [Test]
    public void OtherPlatformsProbeNoFrameworksDirectory()
    {
        var probed = NativeEngineLibrary.ProbeDirectories(isMacOS: false, baseDirectory: s_MacOSDir,
            executableDirectory: s_MacOSDir, assemblyDirectory: s_ManagedDir, nativeDir: s_NativeDir).ToArray();

        Assert.That(probed, Is.EqualTo(new[] { s_MacOSDir, s_ManagedDir, s_NativeDir }));
    }
}
