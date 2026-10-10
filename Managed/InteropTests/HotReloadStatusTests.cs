using NUnit.Framework;
using System.Threading.Tasks;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Sanity checks for HotReloadManager.GetStatus() metrics: non-negative and monotonic where applicable.
    /// </summary>
    public class HotReloadStatusTests
    {
        /// <summary>
        /// Validates that metrics are non-negative and that TotalSwaps monotonically increases across swaps.
        /// </summary>
        [Test]
        public void GetStatus_MetricsAreValidAndMonotonic()
        {
            ScriptCompileHelper.EnsureHrmLoaded();

            var status0 = GameEngine.HotReload.HotReloadManager.GetStatus();
            Assert.That(status0.TotalCompiles, Is.GreaterThanOrEqualTo(0));
            Assert.That(status0.TotalSwaps, Is.GreaterThanOrEqualTo(0));
            Assert.That(status0.TotalUnloads, Is.GreaterThanOrEqualTo(0));
            Assert.That(status0.LastCompileMs, Is.GreaterThanOrEqualTo(0));
            Assert.That(status0.LastSwapMs, Is.GreaterThanOrEqualTo(0));
            Assert.That(status0.LastSwapCriticalSectionUs, Is.GreaterThanOrEqualTo(0));

            // Compile two different tiny assemblies and load them to trigger swaps
            var (asm1, pdb1) = ScriptCompileHelper.CompileAssemblyReturning(1);
            var rc1 = GameEngine.HotReload.HotReloadManager.LoadCompiledAssembly(asm1);
            Assert.That(rc1, Is.GreaterThan(0));

            var status1 = GameEngine.HotReload.HotReloadManager.GetStatus();
            Assert.That(status1.TotalSwaps, Is.GreaterThanOrEqualTo(status0.TotalSwaps));

            var (asm2, pdb2) = ScriptCompileHelper.CompileAssemblyReturning(2);
            var rc2 = GameEngine.HotReload.HotReloadManager.LoadCompiledAssembly(asm2);
            Assert.That(rc2, Is.GreaterThan(0));

            var status2 = GameEngine.HotReload.HotReloadManager.GetStatus();
            Assert.That(status2.TotalSwaps, Is.GreaterThanOrEqualTo(status1.TotalSwaps));
            Assert.That(status2.TotalCompiles, Is.GreaterThanOrEqualTo(status0.TotalCompiles));
            Assert.That(status2.TotalUnloads, Is.GreaterThanOrEqualTo(status0.TotalUnloads));

#if DEBUG
            // In DEBUG we collect critical section timing; it should be non-negative
            Assert.That(status2.LastSwapCriticalSectionUs, Is.GreaterThanOrEqualTo(0));
#endif
        }
    }
}

