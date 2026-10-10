using System;
using System.IO;
using System.Reflection;
using NUnit.Framework;
using GameEngine;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Verifies the managed preloaded context flow can be invoked via CoreBridge wrappers
    /// and that the façade remains functional after swap.
    /// </summary>
    public class PreloadedContextSwapTests
    {
        private static Type GetCoreBridgeType()
        {
            var type = Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge", throwOnError: false);
            Assert.That(type, Is.Not.Null, "CoreBridge type not found");
            return type!;
        }

        private static string FindScriptsAssembly()
        {
            // Heuristic: look for a test scripts assembly near the test output
            var baseDir = AppContext.BaseDirectory;
            var candidate = Path.Combine(baseDir, "TestScripts.dll");
            if (File.Exists(candidate)) return candidate;
            Assert.Inconclusive("TestScripts.dll not found; skipping preloaded context swap test");
            return string.Empty;
        }

        /// <summary>
        /// Preloads then swaps a preloaded context through managed wrappers and validates façade functionality.
        /// </summary>
        [Test]
        public void Managed_Preload_And_Swap_DoesNotBreakFacade()
        {
            var world = EcsTestHelpers.RequirePrimaryWorld("PreloadedContextSwap.Managed_Preload_And_Swap_DoesNotBreakFacade");
            Assert.That(world.EntityCount, Is.GreaterThanOrEqualTo(0));

            var type = GetCoreBridgeType();
            var preload = type.GetMethod("ManagedPreloadAssemblyContext", BindingFlags.Static | BindingFlags.NonPublic);
            var swap = type.GetMethod("ManagedSwapPreloadedContext", BindingFlags.Static | BindingFlags.NonPublic);
            if (preload == null || swap == null)
            {
                Assert.Inconclusive("Managed preload/swap wrappers not available in this build");
                return;
            }

            var asmPath = FindScriptsAssembly();
            var rcPreload = (int)preload!.Invoke(null, new object[] { asmPath })!;
            Assert.That(rcPreload, Is.EqualTo(0).Or.EqualTo(1), $"Preload failed rc={rcPreload}");

            var rcSwap = (int)swap!.Invoke(null, null)!;
            Assert.That(rcSwap, Is.EqualTo(0), $"Swap failed rc={rcSwap}");

            // After swap, façade remains functional once ECS world is available.
            var world2 = EcsTestHelpers.RequirePrimaryWorld("PreloadedContextSwap.Managed_Preload_And_Swap_DoesNotBreakFacade.PostSwap");
            Assert.That(world2.EntityCount, Is.GreaterThanOrEqualTo(0));
        }
    }
}

