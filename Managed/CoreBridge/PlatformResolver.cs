using System;
using System.Reflection;
using System.Runtime.InteropServices;
using GameEngine.Interop;

namespace GameEngine.CoreBridge;

// Resolves CoreBridge's [DllImport("GameEngine.Native")] through the shared shim loader.
// Installed from CoreBridge.Initialize, never from a static constructor: no resolver may be
// installed while the CLR bootstraps.
internal static class PlatformResolver
{
    internal static void EnsureInstalled()
    {
        try { NativeLibrary.SetDllImportResolver(typeof(PlatformResolver).Assembly, Resolve); }
        catch { }
    }

    // The runtime binds each P/Invoke method once, so this runs once per distinct DllImport
    // method, not per call.
    private static IntPtr Resolve(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (!string.Equals(libraryName, NativeEngineLibrary.kLogicalName, StringComparison.Ordinal))
            return IntPtr.Zero;

        // Zero hands the import back to the runtime's default probing.
        try { return NativeEngineLibrary.Load(null, out _); }
        catch (DllNotFoundException) { return IntPtr.Zero; }
    }
}
