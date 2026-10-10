using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;

namespace GameEngine.Interop
{
    /// <summary>
    /// Resolves and loads the GameEngine.Native shim. Every managed assembly that binds the
    /// native ABI links this one file (see the csproj Compile Links), so they all probe the
    /// same locations in the same order and load the same module. On macOS the shim is
    /// libGameEngine.Native.dylib in Contents/Frameworks, never GameEngine.Native.dll. A miss
    /// on every candidate throws with the probed locations, so a hosting failure is
    /// diagnosable instead of surfacing as a bare DllNotFoundException deep inside a hook.
    /// </summary>
    internal static class NativeEngineLibrary
    {
        /// <summary>The DllImport library name of the shim.</summary>
        internal const string kLogicalName = "GameEngine.Native";

        private const string kNativeDirEnv = "GE_NATIVE_DIR";

        /// <summary>This platform's file name of the shim (see PlatformFileName).</summary>
        internal static readonly string FileName = PlatformFileName();

        /// <summary>
        /// Loads the shim. Probes <paramref name="explicitPath"/> first, then this platform's
        /// file name in the directories ProbeDirectories lists, then the OS loader search path.
        /// <paramref name="loadedFrom"/> is the file that loaded, or the logical name when the OS
        /// loader found it.
        /// </summary>
        internal static nint Load(string? explicitPath, out string loadedFrom)
        {
            string? nativeDir = null;
            try { nativeDir = Environment.GetEnvironmentVariable(kNativeDirEnv); } catch { }

            var probed = new List<string>();
            Exception? lastLoadError = null;
            foreach (var candidate in Candidates(explicitPath, nativeDir))
            {
                probed.Add(candidate);
                try
                {
                    if (!File.Exists(candidate))
                        continue;
                    loadedFrom = candidate;
                    return NativeLibrary.Load(candidate);
                }
                catch (Exception ex) when (ex is DllNotFoundException or BadImageFormatException or EntryPointNotFoundException)
                {
                    // A wrong-architecture, damaged or unloadable file: try the next candidate.
                    lastLoadError = ex;
                }
            }

            // OS loader search paths (covers hosts that pre-load the shim or set library paths).
            if (NativeLibrary.TryLoad(kLogicalName, out nint handle))
            {
                loadedFrom = kLogicalName;
                return handle;
            }

            throw new DllNotFoundException(
                $"{kLogicalName} not found ({kNativeDirEnv}='{nativeDir ?? "<unset>"}'). Probed: {string.Join("; ", probed)}. " +
                "Pass the library's path explicitly (EngineNativeBinding.LoadFrom, or GE_ScriptingConfig.nativeLibraryPath for the engine instance), " +
                $"place {FileName} in one of the probed directories, or set {kNativeDirEnv} to the directory that holds it " +
                "as the test harnesses do.",
                lastLoadError);
        }

        /// <summary>
        /// The files Load probes, in order: <paramref name="explicitPath"/>, then this platform's file
        /// name in each directory ProbeDirectories returns for this process (its platform, base
        /// directory, executable directory from Environment.ProcessPath, and this assembly's directory).
        /// </summary>
        internal static IEnumerable<string> Candidates(string? explicitPath, string? nativeDir)
        {
            // A relative explicit path is relative to the working directory.
            string? explicitFullPath = null;
            if (!string.IsNullOrWhiteSpace(explicitPath))
            {
                try { explicitFullPath = Path.GetFullPath(explicitPath!); } catch { }
            }
            if (explicitFullPath != null)
                yield return explicitFullPath;

            string? assemblyDir = null;
            try { assemblyDir = Path.GetDirectoryName(typeof(NativeEngineLibrary).Assembly.Location); } catch { }

            string? executableDir = null;
            try { executableDir = Path.GetDirectoryName(Environment.ProcessPath); } catch { }

            bool isMacOS = RuntimeInformation.IsOSPlatform(OSPlatform.OSX);
            foreach (var root in ProbeDirectories(isMacOS, AppContext.BaseDirectory, executableDir, assemblyDir, nativeDir))
            {
                string candidate;
                try { candidate = Path.Combine(root, FileName); } catch { continue; }
                yield return candidate;
            }
        }

        /// <summary>
        /// The directories probed for the shim's file name, in order: AppContext.BaseDirectory;
        /// on macOS, Contents/Frameworks of the executable's bundle (the executable's directory
        /// joined with ../Frameworks), where the Editor, Player and exported game bundles ship
        /// the shim and where the native resolver (ScriptingPaths::ResolveNativeLibraryDirectoryFrom)
        /// looks after the executable's directory; the directory of the assembly this file is
        /// compiled into; GE_NATIVE_DIR (set by test harnesses, and by EngineNativeBinding once it
        /// has loaded the shim). Unset directories are skipped. The executable's location wins
        /// over GE_NATIVE_DIR: test hosts stage a test shim beside the test assembly while
        /// GE_NATIVE_DIR names the real library.
        /// </summary>
        internal static IEnumerable<string> ProbeDirectories(bool isMacOS, string? baseDirectory, string? executableDirectory,
                                                             string? assemblyDirectory, string? nativeDir)
        {
            if (!string.IsNullOrWhiteSpace(baseDirectory))
                yield return baseDirectory!;

            if (isMacOS && !string.IsNullOrWhiteSpace(executableDirectory))
            {
                string? frameworksDir = null;
                try { frameworksDir = Path.GetFullPath(Path.Combine(executableDirectory!, "..", "Frameworks")); } catch { }
                if (frameworksDir != null)
                    yield return frameworksDir;
            }

            if (!string.IsNullOrWhiteSpace(assemblyDirectory))
                yield return assemblyDirectory!;
            if (!string.IsNullOrWhiteSpace(nativeDir))
                yield return nativeDir!;
        }

        // The file name CMake gives the GameEngine.Native target on Windows, Linux and macOS
        // (lib prefix on Unix); any other platform probes the bare logical name.
        private static string PlatformFileName()
        {
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                return "GameEngine.Native.dll";
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
                return "libGameEngine.Native.so";
            if (RuntimeInformation.IsOSPlatform(OSPlatform.OSX))
                return "libGameEngine.Native.dylib";
            return kLogicalName;
        }
    }
}
