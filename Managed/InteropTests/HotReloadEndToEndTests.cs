using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Loader;
using NUnit.Framework;
using GameEngine;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// End-to-end hot-reload test: compile a script, preload+swap, invoke method, edit script,
    /// recompile, preload+swap again, and verify new method result.
    /// </summary>
    public class HotReloadEndToEndTests
    {
        private static Type GetCoreBridgeType()
        {
            var type = Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge", throwOnError: false);
            Assert.That(type, Is.Not.Null, "CoreBridge type not found");
            return type!;
        }

        private static void EnsureHrmLoaded()
        {
            // If already loaded, nothing to do
            var t = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: false);
            if (t != null) return;

            // Prefer the test output directory (ProjectReference should copy HRM here)
            var baseDir = AppContext.BaseDirectory ?? Environment.CurrentDirectory;
            var built = Path.Combine(baseDir, "GameEngine.HotReload.dll");
            if (!File.Exists(built))
            {
                // Fallback to repo build output
                built = Path.GetFullPath(Path.Combine(Environment.CurrentDirectory, "HotReload", "bin", "Debug", "net10.0", "GameEngine.HotReload.dll"));
            }
            Assert.That(File.Exists(built), Is.True, $"HotReload assembly not found: {built}");

            // Hint CoreBridge with the absolute HRM path to avoid resolver races
            Environment.SetEnvironmentVariable("GE_HRM_PATH", built);

            // Load into default ALC so Type.GetType can resolve by name
            AssemblyLoadContext.Default.LoadFromAssemblyPath(built);

            // Verify
            t = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: false);
            Assert.That(t, Is.Not.Null, "Failed to load GameEngine.HotReload.dll into default ALC");
        }

        private static Type GetHotReloadManagerType()
        {
            EnsureHrmLoaded();
            var type = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: false);
            Assert.That(type, Is.Not.Null, "HotReloadManager type not found");
            return type!;
        }

        private static (string dir, string csproj, string dll, string scriptPath) CreateTempProject()
        {
            var dir = Path.Combine(TestContext.CurrentContext.WorkDirectory, "TempScripts_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(dir);
            var csproj = Path.Combine(dir, "TestScripts.csproj");
            var dll = Path.Combine(dir, "TestScripts.dll");
            var script = Path.Combine(dir, "HotReloadTest.cs");

            // Minimal SDK project to keep tools happy (the helper compiles sources directly).
            File.WriteAllText(csproj, "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup></Project>");

            // Initial script content returning 1
            File.WriteAllText(script,
                "public static class HotReloadTest { public static int TestMethod() => 1; }");

            return (dir, csproj, dll, script);
        }

        private static (byte[] asmBytes, byte[]? pdbBytes) CompileScriptFile(string scriptPath)
        {
            var (asmBytes, pdbBytes) = ScriptCompileHelper.CompileCustomSources(File.ReadAllText(scriptPath));
            Assert.That(asmBytes?.Length, Is.GreaterThan(0));
            return (asmBytes!, pdbBytes);
        }

        private static int ManagedPreloadAndSwap(string dllPath)
        {
            var cbType = GetCoreBridgeType();
            var preload = cbType.GetMethod("ManagedPreloadAssemblyContext", BindingFlags.Static | BindingFlags.NonPublic)!;
            var swap = cbType.GetMethod("ManagedSwapPreloadedContext", BindingFlags.Static | BindingFlags.NonPublic)!;
            int rcPreload = (int)preload.Invoke(null, new object[] { dllPath })!;
            if (rcPreload != 0 && rcPreload != 1) return rcPreload;
            int rcSwap = (int)swap.Invoke(null, null)!;
            return rcSwap;
        }

        private static int CallScriptMethod(string fqn)
        {
            // Use the CoreBridge managed wrapper so tests exercise the façade path
            var cbType = GetCoreBridgeType();
            var mi = cbType.GetMethod("ManagedCallUserScriptsMethod", BindingFlags.NonPublic | BindingFlags.Static)!;
            return (int)mi.Invoke(null, new object[] { fqn })!;
        }

        /// <summary>
        /// Compiles then swaps a script via preloaded context and verifies method result across edits.
        /// </summary>
        [Test]
        public void Compile_Preload_Swap_Invoke_Edit_Recompile_Swap_Invoke_Succeeds()
        {
            // Ensure HRM assembly is loaded and the ECS façade is functional. If the
            // native ECS world is not available in this environment, treat this as a
            // capability gap rather than a hard failure of hot-reload itself.
            EnsureHrmLoaded();
            var world = EcsTestHelpers.RequirePrimaryWorldOrInconclusive("HotReloadEndToEnd.Compile_Preload_Swap_Invoke_Edit_Recompile_Swap_Invoke_Succeeds");
            Assert.That(world.EntityCount, Is.GreaterThanOrEqualTo(0));

            // Disable console redirection so we can see CoreBridge diagnostics in test output
            Environment.SetEnvironmentVariable("GE_DISABLE_CONSOLE_REDIRECT", "1");

            // Initialize HRM via CoreBridge managed demo entry (no native host in tests)
            var cbInit = GetCoreBridgeType().GetMethod("Demo_InitializeManaged", BindingFlags.Public | BindingFlags.Static);
            cbInit?.Invoke(null, null);

            var (dir, csproj, dll, script) = CreateTempProject();

            // Compile initial version
            var (asm1, pdb1) = CompileScriptFile(script);
            File.WriteAllBytes(dll, asm1);

            // Preload + swap initial
            var hrmType = GetHotReloadManagerType();
            var miSwapPublic = hrmType.GetMethod("SwapPreloadedContext", BindingFlags.Public | BindingFlags.Static);
            TestContext.WriteLine($"HRM public SwapPreloadedContext present={miSwapPublic != null}");

            int rc = ManagedPreloadAndSwap(dll);
            TestContext.WriteLine($"ManagedPreloadAndSwap rc={rc}");
            Assert.That(rc, Is.EqualTo(0), $"Initial swap failed rc={rc}");

            // Invoke method, expect 1
            int v1 = CallScriptMethod("HotReloadTest.TestMethod");
            Assert.That(v1, Is.EqualTo(1));

            // Edit script to return 2 and recompile
            File.WriteAllText(script,
                "public static class HotReloadTest { public static int TestMethod() => 2; }");

            var (asm2, pdb2) = CompileScriptFile(script);
            File.WriteAllBytes(dll, asm2);

            // Preload + swap updated
            rc = ManagedPreloadAndSwap(dll);
            Assert.That(rc, Is.EqualTo(0), $"Updated swap failed rc={rc}");

            // Invoke method again, expect 2
            int v2 = CallScriptMethod("HotReloadTest.TestMethod");
            Assert.That(v2, Is.EqualTo(2));
        }
    }
}

