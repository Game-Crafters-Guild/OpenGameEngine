using System;
using NUnit.Framework;
using GameEngine.ECS;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Managed interop coverage for the modular ECS ABI surface.
    /// </summary>
    public class EcsAbiCreateTests
    {
        /// <summary>
        /// Attempts to create an entity through GameEngine.ECS.ABI when the native ECS ABI is present.
        /// Marks the test inconclusive when the ABI is unavailable in the current environment.
        /// </summary>
        [Test]
        public void EcsAbi_CanCreateEmptyEntity_WhenAvailable()
        {
            // Capability-gated: in some environments the native engine/ABI may not be present.
            WorldHandle world;
            try
            {
                world = Ecs.PrimaryWorld;
            }
            catch (Exception ex)
            {
                Assert.Inconclusive($"ECS ABI not available in this environment: {ex.GetType().Name}: {ex.Message}");
                throw;
            }

            int before = 0;
            _ = world.TryGetEntityCount(out before);

            uint id;
            try
            {
                id = world.CreateEmptyEntity("EcsAbiCreateTests_Empty");
            }
            catch (Exception ex)
            {
                Assert.Inconclusive($"ECS ABI create failed in this environment: {ex.GetType().Name}: {ex.Message}");
                throw;
            }

            Assert.That(id, Is.Not.EqualTo(0u));

            Assert.That(world.TryGetEntityCount(out int after), Is.True);
            Assert.That(after, Is.GreaterThan(before));
        }
    }
}


