using System;
using System.IO;
using System.Reflection;
using System.Runtime.Loader;
using NUnit.Framework;

using GameEngine.HotReload;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>Focused tests for QueryExportInDomain + InvokeByToken, including domain==0 fallback.</summary>
    internal class TokenApiTests
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

        [Test]
        public void Token_Query_And_Invoke_With_Domain0_Fallback_Works()
        {
            EnsureHrmLoaded();
            var cbType = GetCoreBridgeType();
            cbType.GetMethod("Demo_InitializeManaged", BindingFlags.Public | BindingFlags.Static)!.Invoke(null, null);

            // Build tiny assembly in memory
            var (asmBytes, _) = ScriptCompileHelper.CompileAssemblyReturning(7);

            // Load compiled bytes -> sets current domain
            var hrm = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: true)!;
            var miLoadBytes = hrm.GetMethod("LoadCompiledAssembly", BindingFlags.Public | BindingFlags.Static, binder: null, types: new[] { typeof(byte[]) }, modifiers: null)!;
            int loadRc = (int)miLoadBytes.Invoke(null, new object[] { asmBytes })!;
            Assert.That(loadRc, Is.GreaterThan(0));

            // Query + invoke via HotReloadHelper (domain==0 fallback)
            ulong token;
            int qrc = HotReloadHelper.TryQuery("HotReloadTest.TestMethod", out token);
            Assert.That(qrc, Is.EqualTo(0));
            Assert.That(token, Is.GreaterThan(0UL));
            int resultVal = HotReloadHelper.TryInvoke(token);
            Assert.That(resultVal, Is.EqualTo(7));
        }
    }
}

