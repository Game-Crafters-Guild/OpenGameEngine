using System;
using System.IO;
using System.Reflection;
using System.Runtime.Loader;
using NUnit.Framework;

namespace GameEngine.ManagedInteropTests
{
    internal class HotReloadPathPreloadTests
    {
        private static void EnsureHrmLoaded()
        {
            var t = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: false);
            if (t != null) return;
            var baseDir = AppContext.BaseDirectory ?? Environment.CurrentDirectory;
            var built = Path.Combine(baseDir, "GameEngine.HotReload.dll");
            if (!File.Exists(built))
            {
                built = Path.GetFullPath(Path.Combine(Environment.CurrentDirectory, "HotReload", "bin", "Debug", "net10.0", "GameEngine.HotReload.dll"));
            }
            Assert.That(File.Exists(built), Is.True, $"HotReload assembly not found: {built}");
            Environment.SetEnvironmentVariable("GE_HRM_PATH", built);
            AssemblyLoadContext.Default.LoadFromAssemblyPath(built);
        }

        [Test]
        public void Preload_Path_Swap_Invoke_Works()
        {
            EnsureHrmLoaded();
            var cbType = Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge", throwOnError: true)!;
            cbType.GetMethod("Demo_InitializeManaged", BindingFlags.Public | BindingFlags.Static)!.Invoke(null, null);

            // Build small assembly, save bytes to file, then path-preload
            var tempDir = Path.Combine(TestContext.CurrentContext.WorkDirectory, "PathPreloadScripts_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(tempDir);
            var (asmBytes1, _) = ScriptCompileHelper.CompileAssemblyReturning(7);

            var dllPath = Path.Combine(tempDir, "TestScripts.dll");
            File.WriteAllBytes(dllPath, asmBytes1);

            var hrm = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: true)!;
            var miPreloadPath = hrm.GetMethod("PreloadAssemblyContext", BindingFlags.Public | BindingFlags.Static, binder: null, types: new[] { typeof(string) }, modifiers: null)!;
            var miSwap = hrm.GetMethod("SwapPreloadedContext", BindingFlags.Public | BindingFlags.Static)!;
            int preRc = (int)miPreloadPath.Invoke(null, new object?[] { dllPath })!;
            Assert.That(preRc, Is.EqualTo(0));
            int swapRc = (int)miSwap.Invoke(null, null)!;
            Assert.That(swapRc, Is.EqualTo(0));

            var miCall = hrm.GetMethod("CallMethodInDomain", BindingFlags.Public | BindingFlags.Static)!;
            int rc = (int)miCall.Invoke(null, new object?[] { 0UL, "HotReloadTest.TestMethod" })!;
            Assert.That(rc, Is.EqualTo(7));
        }
    }
}

