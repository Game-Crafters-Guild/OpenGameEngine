using System;
using System.IO;
using System.Reflection;
using System.Runtime.Loader;
using NUnit.Framework;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>Focused tests for the CoreBridge facade calling into HRM.</summary>
    internal class HotReloadFacadeTests
    {
        private static Type GetCoreBridgeType()
        {
            var type = Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge", throwOnError: false);
            Assert.That(type, Is.Not.Null, "CoreBridge type not found");
            return type!;
        }

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

        /// <summary>Ensures the facade can call a method in a just-loaded script assembly.</summary>
        [Test]
        public void Facade_Can_Call_Simple_Method_In_Loaded_Scripts()
        {
            EnsureHrmLoaded();
            var cbType = GetCoreBridgeType();
            cbType.GetMethod("Demo_InitializeManaged", BindingFlags.Public | BindingFlags.Static)!.Invoke(null, null);

            // Build a tiny assembly in-memory
            var (asmBytes, _) = ScriptCompileHelper.CompileAssemblyReturning(1);

            // Load via LoadCompiledAssembly(byte[])
            var hrm = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: true)!;
            var miLoadBytes = hrm.GetMethod("LoadCompiledAssembly", BindingFlags.Public | BindingFlags.Static, binder: null, types: new[] { typeof(byte[]) }, modifiers: null)!;
            int loadRc = (int)miLoadBytes.Invoke(null, new object[] { asmBytes })!;
            Assert.That(loadRc, Is.GreaterThan(0)); // returns domain id as positive int

            // Call via CoreBridge façade wrapper
            var miCall = cbType.GetMethod("ManagedCallUserScriptsMethod", BindingFlags.NonPublic | BindingFlags.Static)!;
            int resultVal = (int)miCall.Invoke(null, new object[] { "HotReloadTest.TestMethod" })!;
            Assert.That(resultVal, Is.EqualTo(1));
        }

        /// <summary>Call by simple method name without class prefix using the facade.</summary>
        [Test]
        public void Facade_Can_Call_By_Simple_Method_Name()
        {
            EnsureHrmLoaded();
            var cbType = GetCoreBridgeType();
            cbType.GetMethod("Demo_InitializeManaged", BindingFlags.Public | BindingFlags.Static)!.Invoke(null, null);

            // Build tiny assembly
            var (asmBytes, _) = ScriptCompileHelper.CompileAssemblyReturning(42);

            var hrm = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: true)!;
            var miLoadBytes = hrm.GetMethod("LoadCompiledAssembly", BindingFlags.Public | BindingFlags.Static, binder: null, types: new[] { typeof(byte[]) }, modifiers: null)!;
            int loadRc = (int)miLoadBytes.Invoke(null, new object[] { asmBytes })!;
            Assert.That(loadRc, Is.GreaterThan(0));

            var miCall = cbType.GetMethod("ManagedCallUserScriptsMethod", BindingFlags.NonPublic | BindingFlags.Static)!;
            int resultVal = (int)miCall.Invoke(null, new object[] { "TestMethod" })!;
            Assert.That(resultVal, Is.EqualTo(42));
        }

        /// <summary>End-to-end: compile, call, edit, recompile, hot-reload (preload+swap), call again.</summary>
        [Test]
        public void Facade_Edit_Recompile_HotReload_Updates_Result()
        {
            EnsureHrmLoaded();
            var cbType = GetCoreBridgeType();
            cbType.GetMethod("Demo_InitializeManaged", BindingFlags.Public | BindingFlags.Static)!.Invoke(null, null);

            // Build tiny assembly (v1)
            var (asmBytesV1, _) = ScriptCompileHelper.CompileAssemblyReturning(1);

            // Load v1
            var hrm = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: true)!;
            var miLoadBytes = hrm.GetMethod("LoadCompiledAssembly", BindingFlags.Public | BindingFlags.Static, binder: null, types: new[] { typeof(byte[]) }, modifiers: null)!;
            int loadRc = (int)miLoadBytes.Invoke(null, new object[] { asmBytesV1 })!;
            Assert.That(loadRc, Is.GreaterThan(0));

            var miCall = cbType.GetMethod("ManagedCallUserScriptsMethod", BindingFlags.NonPublic | BindingFlags.Static)!;
            int v1 = (int)miCall.Invoke(null, new object[] { "HotReloadTest.TestMethod" })!;
            Assert.That(v1, Is.EqualTo(1));

            // Edit to v2 and recompile
            var (asmBytesV2, _) = ScriptCompileHelper.CompileAssemblyReturning(2);

            // Preload+swap with bytes
            var miPreloadBytes = hrm.GetMethod("PreloadAssemblyContext", BindingFlags.Public | BindingFlags.Static, binder: null, types: new[] { typeof(byte[]), typeof(byte[]) }, modifiers: null)!;
            int preRc = (int)miPreloadBytes.Invoke(null, new object?[] { asmBytesV2, null })!;
            Assert.That(preRc, Is.EqualTo(0));
            var miSwap = hrm.GetMethod("SwapPreloadedContext", BindingFlags.Public | BindingFlags.Static)!;
            int swapRc = (int)miSwap.Invoke(null, null)!;
            Assert.That(swapRc, Is.EqualTo(0));

            int v2 = (int)miCall.Invoke(null, new object[] { "HotReloadTest.TestMethod" })!;
            Assert.That(v2, Is.EqualTo(2));
        }
    }
}

