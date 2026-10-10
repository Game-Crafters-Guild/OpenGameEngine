using System;
using NUnit.Framework;
using GameEngine;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Interop coverage specific to ECS façade access patterns.
    /// </summary>
    public class EcsInteropTests
    {
        /// <summary>
        /// Accesses entity count via façade to ensure wide path works.
        /// </summary>
        [Test]
        public void PrimaryWorld_EntityCount_Accessible_ViaFacade()
        {
            var world = EcsTestHelpers.RequirePrimaryWorld("EcsInterop.PrimaryWorld_EntityCount_Accessible_ViaFacade");
            Assert.That(world.EntityCount, Is.GreaterThanOrEqualTo(0));
        }
    }
}

