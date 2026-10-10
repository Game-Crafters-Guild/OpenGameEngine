using System;
using System.Reflection;
using NUnit.Framework;
using GameEngine;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Integration-style tests that simulate reload lifecycle by invoking managed callbacks
    /// (OnBeforeUnload and OnAfterLoad) and validating façade behavior across the boundary.
    /// </summary>
    public class ReloadLifecycleTests
    {
        private static MethodInfo? GetCoreBridgeMethod(string name)
        {
            var type = Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge", throwOnError: false);
            Assert.That(type, Is.Not.Null, "CoreBridge type not found");
            var mi = type!.GetMethod(name, BindingFlags.Static | BindingFlags.NonPublic);
            return mi;
        }

        /// <summary>
        /// Simulates a reload by calling OnBeforeUnload then OnAfterLoad and ensures the façade
        /// remains usable (cache invalidated and minimal warm-up does not throw).
        /// </summary>
        [Test]
        public void SimulatedReload_DoesNotBreakFacade()
        {
            // Touch the façade once to populate caches. Failure to obtain the primary
            // world in supported test environments is treated as a real failure.
            var world1 = EcsTestHelpers.RequirePrimaryWorld("ReloadLifecycle.SimulatedReload_DoesNotBreakFacade.PreReload");
            Assert.That(world1.EntityCount, Is.GreaterThanOrEqualTo(0));

            // Simulate unload
            // Use internal managed helpers instead of UnmanagedCallersOnly methods
            var type = Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge", throwOnError: true)!;
            type.GetMethod("InternalBeforeUnload", BindingFlags.Static | BindingFlags.NonPublic)!.Invoke(null, null);

            // Façade should still work (cache will be reacquired on demand).
            var world2 = EcsTestHelpers.RequirePrimaryWorld("ReloadLifecycle.SimulatedReload_DoesNotBreakFacade.PostUnload");
            Assert.That(world2.EntityCount, Is.GreaterThanOrEqualTo(0));

            // Simulate load completion (triggers minimal warm-up)
            type.GetMethod("InternalAfterLoad", BindingFlags.Static | BindingFlags.NonPublic)!.Invoke(null, null);

            // Still functional after warm-up.
            var world3 = EcsTestHelpers.RequirePrimaryWorld("ReloadLifecycle.SimulatedReload_DoesNotBreakFacade.PostLoad");
            Assert.That(world3.EntityCount, Is.GreaterThanOrEqualTo(0));
        }
    }
}

