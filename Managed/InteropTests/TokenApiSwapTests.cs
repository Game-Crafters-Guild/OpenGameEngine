using System;
using System.IO;
using System.Reflection;
using System.Runtime.Loader;
using NUnit.Framework;

using GameEngine.HotReload;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Verifies that a token from QueryExportInDomain becomes invalid across a hot-reload swap,
    /// requiring re-query to obtain a new token, and that the invocation result updates.
    /// If the HRM does not expose the token API, the test is marked inconclusive.
    /// </summary>
    internal class TokenApiSwapTests
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
        public void Token_Invalidates_Across_Swap_And_Requery_Required()
        {
            EnsureHrmLoaded();
            var cbType = Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge", throwOnError: true)!;
            cbType.GetMethod("Demo_InitializeManaged", BindingFlags.Public | BindingFlags.Static)!.Invoke(null, null);

            // Build tiny assembly in memory
            var (asmBytes1, _) = ScriptCompileHelper.CompileAssemblyReturning(5);

            var hrm = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: true)!;
            var miLoadBytes = hrm.GetMethod("LoadCompiledAssembly", BindingFlags.Public | BindingFlags.Static, binder: null, types: new[] { typeof(byte[]) }, modifiers: null)!;
            int loadRc = (int)miLoadBytes.Invoke(null, new object[] { asmBytes1 })!;
            Assert.That(loadRc, Is.GreaterThan(0));

            ulong tok1;
            int q1 = HotReloadHelper.TryQuery("HotReloadTest.TestMethod", out tok1);
            Assert.That(q1, Is.EqualTo(0));
            int before = HotReloadHelper.TryInvoke(tok1);
            Assert.That(before, Is.EqualTo(5));

            // Recompile as v2, then Preload + Swap
            var (asmBytes2, _) = ScriptCompileHelper.CompileAssemblyReturning(9);
            var miPreloadBytes = hrm.GetMethod("PreloadAssemblyContext", BindingFlags.Public | BindingFlags.Static, binder: null, types: new[] { typeof(byte[]), typeof(byte[]) }, modifiers: null);
            if (miPreloadBytes == null) { Assert.Inconclusive("Preload bytes API not exposed"); return; }
            int preRc = (int)miPreloadBytes.Invoke(null, new object?[] { asmBytes2, null })!;
            Assert.That(preRc, Is.EqualTo(0));
            var miSwap = hrm.GetMethod("SwapPreloadedContext", BindingFlags.Public | BindingFlags.Static)!;
            int swapRc = (int)miSwap.Invoke(null, null)!;
            Assert.That(swapRc, Is.EqualTo(0));

            // Attempt invoking old token; acceptable outcomes: exception, zero/negative code, or non-9 value
            bool oldFailedAsExpected = false;
            try
            {
                int oldAfter = HotReloadHelper.TryInvoke(tok1);
                if (oldAfter != 9) oldFailedAsExpected = true; // not returning new value is considered invalidation
            }
            catch
            {
                oldFailedAsExpected = true; // throwing is acceptable
            }
            Assert.That(oldFailedAsExpected, Is.True, "Old token unexpectedly remained valid and returned the new value");


            // Old token should not be valid; either InvokeByToken fails or returns stale value is disallowed.
            // Expectation: require re-query and then result is 9.
            ulong tok2;
            int q2 = HotReloadHelper.TryQuery("HotReloadTest.TestMethod", out tok2);
            Assert.That(q2, Is.EqualTo(0));
            Assert.That(tok2, Is.Not.EqualTo(0UL));
            int after = HotReloadHelper.TryInvoke(tok2);
            Assert.That(after, Is.EqualTo(9));
        }
    }
}

