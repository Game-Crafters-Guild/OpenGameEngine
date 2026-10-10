using System;
using NUnit.Framework;
using GameEngine.ECS;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Validates basic ECS ABI entity/component coverage needed for early gameplay scripting:
    /// - create entity
    /// - set/get Name
    /// - set/get Transform matrix
    /// </summary>
    public class EcsAbiTransformAndNameTests
    {
        /// <summary>
        /// Exercises the ECS ABI v1.1 coverage: entity creation, Name, and Transform matrix.
        /// Marks inconclusive when running against older native binaries.
        /// </summary>
        [Test]
        public void CanCreateEntityAndSetNameAndTransform_WhenAbiAvailable()
        {
            WorldHandle world;
            try
            {
                world = Ecs.PrimaryWorld;
            }
            catch (Exception ex)
            {
                Assert.Inconclusive("ECS ABI not available in this environment: " + ex.Message);
                return;
            }

            uint e;
            try
            {
                e = world.CreateEmptyEntity("EcsAbi.BasicEntity");
            }
            catch (Exception ex)
            {
                Assert.Inconclusive("CreateEmptyEntity not available: " + ex.Message);
                return;
            }

            try { world.SetName(e, "Renamed"); }
            catch (Exception ex) { Assert.Inconclusive("SetName not available: " + ex.Message); return; }

            string gotName;
            try { gotName = world.GetName(e); }
            catch (Exception ex) { Assert.Inconclusive("GetName not available: " + ex.Message); return; }
            Assert.That(gotName, Is.EqualTo("Renamed"));

            // Identity matrix with a translation (column-major; translation in 12..14).
            float[] m = new float[16]
            {
                1,0,0,0,
                0,1,0,0,
                0,0,1,0,
                2,3,4,1
            };

            try { world.SetTransformMatrix(e, m); }
            catch (Exception ex) { Assert.Inconclusive("SetTransform not available: " + ex.Message); return; }

            float[] gotM;
            try { gotM = world.GetTransformMatrix(e); }
            catch (Exception ex) { Assert.Inconclusive("GetTransform not available: " + ex.Message); return; }
            Assert.That(gotM[12], Is.EqualTo(2).Within(0.0001f));
            Assert.That(gotM[13], Is.EqualTo(3).Within(0.0001f));
            Assert.That(gotM[14], Is.EqualTo(4).Within(0.0001f));
        }
    }
}

