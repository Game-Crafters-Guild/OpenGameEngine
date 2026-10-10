using System;
using NUnit.Framework;
using GameEngine;
using GameEngine.Interop;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Basic smoke covering ABI surface by forcing binding init and ECS query via the façade.
    /// </summary>
    public class AbiSmokeTests
    {
        /// <summary>
        /// Loads the interface table and ensures ECS query returns a non-negative value.
        /// </summary>
        [Test]
        public void CanLoadInterfaceTableAndQueryEcs()
        {
            // Using the managed facade ensures EngineNativeBinding is constructed.
            var world = EcsTestHelpers.RequirePrimaryWorld("AbiSmoke.CanLoadInterfaceTableAndQueryEcs");
            Assert.That(world.EntityCount, Is.GreaterThanOrEqualTo(0));
        }
    }
}

