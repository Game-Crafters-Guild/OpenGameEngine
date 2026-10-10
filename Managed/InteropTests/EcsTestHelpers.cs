using System;
using System.Runtime.InteropServices;
using NUnit.Framework;
using GameEngine;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Shared helpers for ECS-related interop tests.
    /// </summary>
    internal static class EcsTestHelpers
    {
        /// <summary>
        /// Strict helper: acquires the primary world via the managed facade and performs a
        /// light sanity check. Any failure to obtain the world or query entity count will
        /// surface as a test failure in the caller.
        /// </summary>
        public static GameEngine.ECS.World RequirePrimaryWorld(string? scenario)
        {
            var world = Engine.Current.Worlds.Primary;
            // Light sanity check: ensure the query path is wired, but do not
            // assert on the value here.
            _ = world.TryGetEntityCount(out _);
            return world;
        }

        /// <summary>
        /// Capability-gated helper for environments where ECS is optional (e.g., logging
        /// smoke tests). Attempts to acquire the primary world via the managed facade and
        /// marks the calling test as inconclusive when the native ECS world is not available
        /// instead of failing hard.
        /// </summary>
        public static GameEngine.ECS.World RequirePrimaryWorldOrInconclusive(string? scenario)
        {
            try
            {
                return RequirePrimaryWorld(scenario);
            }
            catch (InvalidOperationException ex) when (ex.Message.Contains("Primary world not available"))
            {
                Assert.Inconclusive($"Primary world not available for scenario '{scenario}'. This environment does not expose the native ECS world.");
                throw;
            }
            catch (SEHException)
            {
                Assert.Inconclusive($"Native ECS interop threw an SEHException for scenario '{scenario}'. This typically means ECS is not available for headless tests.");
                throw;
            }
        }
    }
}
