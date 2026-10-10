using System;
using NUnit.Framework;
using GameEngine.ECS;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Validates the v2.1 blob-component field schema registration end to end from the
    /// managed side: RegisterComponentSchema catalogs + registers, RegisterBlobComponent
    /// funnels the cataloged schema across the ABI, and a re-registration with a changed
    /// layout migrates placed instance bytes by field name (the hot-reload relayout path).
    /// </summary>
    public class EcsAbiComponentSchemaTests
    {
        private static bool TryGetWorld(out WorldHandle world)
        {
            try
            {
                world = Ecs.PrimaryWorld;
                return true;
            }
            catch (Exception ex)
            {
                Assert.Inconclusive("ECS ABI not available in this environment: " + ex.Message);
                world = default;
                return false;
            }
        }

        /// <summary>
        /// Schema registration resolves a stable type id across re-registration, and the
        /// plain RegisterBlobComponent funnel picks the cataloged schema up transparently.
        /// </summary>
        [Test]
        public void RegisterComponentSchema_IsIdempotent_AndFunnelsThroughRegisterBlobComponent()
        {
            if (!TryGetWorld(out var world))
                return;

            var fields = new[]
            {
                new ComponentFieldDesc("A", 0, 4, ComponentFieldType.Float),
                new ComponentFieldDesc("B", 4, 4, ComponentFieldType.Float),
            };

            ulong id = Ecs.RegisterComponentSchema("CS.SchemaProbe", 8, fields);
            if (id == 0)
            {
                Assert.Inconclusive("RegisterComponentSchema not available (older native)");
                return;
            }

            // Re-registration (every assembly load does this) resolves to the same id.
            Assert.That(Ecs.RegisterComponentSchema("CS.SchemaProbe", 8, fields), Is.EqualTo(id));

            // The plain registration funnel picks the cataloged schema up and agrees on the id.
            Assert.That(Ecs.RegisterBlobComponent("CS.SchemaProbe", 8), Is.EqualTo(id));

            // The component still behaves as a normal blob for byte access.
            uint e = world.CreateEmptyEntity("EcsAbi.SchemaProbeEntity");
            var payload = new byte[] { 0, 0, 0x40, 0x40, 0, 0, 0x20, 0x41 }; // {3.0f, 10.0f}
            world.SetComponentBytes(e, id, payload);
            Assert.That(world.GetComponentBytes(e, id), Is.EqualTo(payload));
        }

        /// <summary>
        /// Re-registering a changed layout (the hot-reload path) migrates placed instance
        /// bytes by field name: moved fields carry their values, new fields default.
        /// </summary>
        [Test]
        public void SchemaRelayout_MigratesPlacedInstanceBytesByFieldName()
        {
            if (!TryGetWorld(out var world))
                return;

            ulong id = Ecs.RegisterComponentSchema("CS.SchemaMigrate", 8, new[]
            {
                new ComponentFieldDesc("A", 0, 4, ComponentFieldType.Float),
                new ComponentFieldDesc("B", 4, 4, ComponentFieldType.Float),
            });
            if (id == 0)
            {
                Assert.Inconclusive("RegisterComponentSchema not available (older native)");
                return;
            }

            uint e = world.CreateEmptyEntity("EcsAbi.SchemaMigrateEntity");
            var v1 = new byte[8];
            BitConverter.GetBytes(1.5f).CopyTo(v1, 0); // A = 1.5
            BitConverter.GetBytes(2.5f).CopyTo(v1, 4); // B = 2.5
            world.SetComponentBytes(e, id, v1);

            // Simulate the hot-reload relayout: A removed, B moved to offset 0, C added.
            ulong id2 = Ecs.RegisterComponentSchema("CS.SchemaMigrate", 8, new[]
            {
                new ComponentFieldDesc("B", 0, 4, ComponentFieldType.Float),
                new ComponentFieldDesc("C", 4, 4, ComponentFieldType.Float),
            });
            Assert.That(id2, Is.EqualTo(id));

            var v2 = world.GetComponentBytes(e, id);
            Assert.That(v2.Length, Is.EqualTo(8));
            Assert.That(BitConverter.ToSingle(v2, 0), Is.EqualTo(2.5f), "B carries by name");
            Assert.That(BitConverter.ToSingle(v2, 4), Is.EqualTo(0.0f), "C defaults to zero");
        }
    }
}
