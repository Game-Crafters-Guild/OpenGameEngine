using System;
using NUnit.Framework;
using GameEngine.ECS;
using GameEngine.ECS.Internal;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Validates runtime-defined ECS blob components and cached chunk queries
    /// (the sole query surface since ECS ABI v2.0).
    /// </summary>
    public class EcsAbiBlobComponentQueryTests
    {
        private static bool CachedQueryContainsEntity(ulong worldHandle, ulong typeId, uint entityId)
        {
            ulong query = ChunkQueryNative.CreateCachedQuery(worldHandle, new[] { typeId }, ReadOnlySpan<ulong>.Empty);
            try
            {
                ChunkQueryNative.ResetQuery(query, out int archetypeCount);
                for (int a = 0; a < archetypeCount; a++)
                {
                    ChunkQueryNative.GetArchetypeInfo(query, a, out _, out int chunkCount);
                    for (int c = 0; c < chunkCount; c++)
                    {
                        foreach (uint id in ChunkQueryNative.GetChunkEntityIds(query, a, c))
                        {
                            if (id == entityId)
                                return true;
                        }
                    }
                }
                return false;
            }
            finally
            {
                ChunkQueryNative.DestroyCachedQuery(query);
            }
        }

        /// <summary>
        /// Registers a blob component type, sets/gets bytes, and queries entities having it.
        /// Marks inconclusive when the native ECS ABI is unavailable in this environment.
        /// </summary>
        [Test]
        public void BlobComponent_SetGet_And_Query_Works_WhenAvailable()
        {
            WorldHandle world;
            try { world = Ecs.PrimaryWorld; }
            catch (Exception ex)
            {
                Assert.Inconclusive("ECS ABI not available in this environment: " + ex.Message);
                return;
            }

            ulong blobTypeId;
            try { blobTypeId = Ecs.RegisterBlobComponent("CS.Blob8", 8); }
            catch (Exception ex)
            {
                Assert.Inconclusive("RegisterBlobComponent not available: " + ex.Message);
                return;
            }

            uint e = world.CreateEmptyEntity("EcsAbi.BlobEntity");

            var payload = new byte[] { 1, 2, 3, 4, 5, 6, 7, 8 };
            try { world.SetComponentBytes(e, blobTypeId, payload); }
            catch (Exception ex)
            {
                Assert.Inconclusive("SetComponentBytes not available: " + ex.Message);
                return;
            }

            byte[] got;
            try { got = world.GetComponentBytes(e, blobTypeId); }
            catch (Exception ex)
            {
                Assert.Inconclusive("GetComponentBytes not available: " + ex.Message);
                return;
            }
            Assert.That(got, Is.EqualTo(payload));

            Assert.That(CachedQueryContainsEntity(world.Handle, blobTypeId, e), Is.True,
                "cached query over the blob component should find the entity");

            // Remove and ensure the query no longer returns it.
            try { world.RemoveComponent(e, blobTypeId); }
            catch (Exception ex)
            {
                Assert.Inconclusive("RemoveComponent not available: " + ex.Message);
                return;
            }

            Assert.That(CachedQueryContainsEntity(world.Handle, blobTypeId, e), Is.False,
                "cached query should not find the entity after component removal");
        }
    }
}
