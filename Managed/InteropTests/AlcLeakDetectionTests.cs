using System;
using System.Diagnostics;
using System.Threading;
using NUnit.Framework;
using GameEngine.HotReload;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Verifies the bounded unload-verification loop: a pinned ALC is reported as a leak
    /// (loud log + GetLeakMetrics), while a clean unload is collected without one.
    /// </summary>
    public class AlcLeakDetectionTests
    {
        private const int kLeakBudgetMs = 3000;   // verification budget is ~1s (10 x 100ms)
        private const int kLeakPollTimeoutMs = 15000;

        private static (byte[] asm, byte[]? pdb) CompilePinnable(int value)
        {
            return ScriptCompileHelper.CompileCustomSources(
                $"public static class HotReloadTest {{ public static int TestMethod() => {value}; }}");
        }

        /// <summary>A user-held Type pins the unloaded ALC; the leak must be counted and attributed.</summary>
        [Test]
        public void PinnedContext_IsReportedAsLeak()
        {
            ScriptCompileHelper.EnsureHrmLoaded();
            HotReloadManager.GetLeakMetrics(out int baselineCount, out _);

            var (asm, _) = CompilePinnable(11);
            int domain = HotReloadManager.LoadCompiledAssembly(asm);
            Assert.That(domain, Is.GreaterThan(0));

            // Pin the ALC the way user code does: hold a Type from the loaded assembly.
            Type? pinned = null;
            foreach (var a in HotReloadManager.GetActiveUserAssemblies())
            {
                pinned = a.GetType("HotReloadTest", throwOnError: false);
                if (pinned != null) break;
            }
            Assert.That(pinned, Is.Not.Null, "test type not found in loaded user assemblies");

            Assert.That(HotReloadManager.UnloadDomain((ulong)domain), Is.EqualTo(0));

            var sw = Stopwatch.StartNew();
            int count = baselineCount;
            ulong lastDomain = 0;
            while (sw.ElapsedMilliseconds < kLeakPollTimeoutMs)
            {
                HotReloadManager.GetLeakMetrics(out count, out lastDomain);
                if (count > baselineCount) break;
                Thread.Sleep(100);
            }

            Assert.That(count, Is.GreaterThan(baselineCount), "pinned ALC was not reported as leaked");
            Assert.That(lastDomain, Is.EqualTo((ulong)domain), "last leaked domain id mismatch");
            GC.KeepAlive(pinned);
        }

        /// <summary>An unpinned context is collected inside the verification budget; no leak is recorded.</summary>
        [Test]
        [Retry(2)]
        public void CleanUnload_IsNotReportedAsLeak()
        {
            ScriptCompileHelper.EnsureHrmLoaded();

            var (asm, _) = CompilePinnable(12);
            int domain = HotReloadManager.LoadCompiledAssembly(asm);
            Assert.That(domain, Is.GreaterThan(0));

            HotReloadManager.GetLeakMetrics(out int baselineCount, out _);
            Assert.That(HotReloadManager.UnloadDomain((ulong)domain), Is.EqualTo(0));

            // Give the verification loop its full budget plus slack, then confirm no new leak
            // was attributed to this domain (other fixtures may legitimately leak in parallel).
            Thread.Sleep(kLeakBudgetMs);
            HotReloadManager.GetLeakMetrics(out int afterCount, out ulong lastDomain);
            bool leakedThisDomain = afterCount > baselineCount && lastDomain == (ulong)domain;
            Assert.That(leakedThisDomain, Is.False, "clean unload was reported as leaked");
        }
    }
}
