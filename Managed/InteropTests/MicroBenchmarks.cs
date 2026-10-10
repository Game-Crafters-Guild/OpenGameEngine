using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.Loader;
using NUnit.Framework;
using GameEngine.HotReload;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Explicit micro-benchmarks for HRM Query/Invoke fast paths. Skipped by default.
    /// </summary>
    [TestFixture]
    internal class MicroBenchmarks
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

        private static byte[] CompileSimpleAssembly(int retVal)
        {
            return ScriptCompileHelper.CompileAssemblyReturning(retVal).asm;
        }

        private static void LoadAssemblyBytes(byte[] asmBytes)
        {
            int loadRc = HotReloadHelper.LoadFromBytes(asmBytes);
            Assert.That(loadRc, Is.GreaterThan(0));
        }

        [Test, Explicit("Micro-benchmark: Query warm path")]
        public void Bench_Query_Warm()
        {
            EnsureHrmLoaded();
            var cbType = Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge", throwOnError: true)!;
            cbType.GetMethod("Demo_InitializeManaged", BindingFlags.Public | BindingFlags.Static)!.Invoke(null, null);
            var asmBytes = CompileSimpleAssembly(1);
            LoadAssemblyBytes(asmBytes);

            // Warm-up: one query to ensure mapping exists
            ulong tok;
            Assert.That(HotReloadHelper.TryQuery("HotReloadTest.TestMethod", out tok), Is.EqualTo(0));

            const int iters = 200_000;
            var sw = Stopwatch.StartNew();
            for (int i = 0; i < iters; i++)
            {
                HotReloadHelper.TryQuery("HotReloadTest.TestMethod", out tok);
            }
            sw.Stop();
            TestContext.WriteLine($"Query warm: {iters} iters in {sw.Elapsed.TotalMilliseconds:F2} ms ({iters / Math.Max(1.0, sw.Elapsed.TotalSeconds):F0} ops/s)");
        }

        [Test, Explicit("Micro-benchmark: Invoke warm path")]
        public void Bench_Invoke_Warm()
        {
            EnsureHrmLoaded();
            var cbType = Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge", throwOnError: true)!;
            cbType.GetMethod("Demo_InitializeManaged", BindingFlags.Public | BindingFlags.Static)!.Invoke(null, null);
            var asmBytes = CompileSimpleAssembly(1);
            LoadAssemblyBytes(asmBytes);

            ulong tok;
            Assert.That(HotReloadHelper.TryQuery("HotReloadTest.TestMethod", out tok), Is.EqualTo(0));

            int sum = 0;
            const int iters = 500_000;
            var sw = Stopwatch.StartNew();
            for (int i = 0; i < iters; i++)
            {
                sum += HotReloadHelper.TryInvoke(tok);
            }
            sw.Stop();
            TestContext.WriteLine($"Invoke warm: {iters} iters in {sw.Elapsed.TotalMilliseconds:F2} ms ({iters / Math.Max(1.0, sw.Elapsed.TotalSeconds):F0} ops/s), sum={sum}");
        }
    }
}

