using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using NUnit.Framework;
using GameEngine.ECS;
using GameEngine.ECS.Internal;
using GameEngine.Scripting;
using GameEngine.Scripting.Runtime;

namespace GameEngine.ManagedInteropTests
{
    [StructLayout(LayoutKind.Sequential)]
    internal struct VerifyVelocity : IComponent
    {
        public float X, Y, Z;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct VerifyHealth : IComponent
    {
        public int Current, Max;
    }

    namespace NamespaceA
    {
        [StructLayout(LayoutKind.Sequential)]
        internal struct QualifiedNameProbe : IComponent
        {
            public ulong Value;
        }
    }

    namespace NamespaceB
    {
        [StructLayout(LayoutKind.Sequential)]
        internal struct QualifiedNameProbe : IComponent
        {
            public ulong Value;
        }
    }

    /// <summary>
    /// End-to-end verification of the C# entities/systems surface against the live
    /// native ECS: typed component round-trips, 64-bit id integrity, namespace-
    /// qualified registration, deferred structural-change fencing, cached-query
    /// refresh, stale-handle behavior, and GameSystemRunner lifecycle/ordering.
    /// Tests gate on world availability (environment capability) and are strict
    /// beyond that point: an ABI failure is a test failure, not an Inconclusive.
    /// </summary>
    public class EcsAbiSystemsVerificationTests
    {
        private static WorldHandle RequireWorld()
        {
            try
            {
                return Ecs.PrimaryWorld;
            }
            catch (Exception ex)
            {
                Assert.Inconclusive("ECS ABI not available in this environment: " + ex.Message);
                throw;
            }
        }

        /// <summary>Typed ComponentType&lt;T&gt; Set/Get round-trip preserves all fields.</summary>
        [Test]
        public void TypedComponent_SetGet_RoundTrip()
        {
            var world = RequireWorld();
            uint e = world.CreateEmptyEntity("Verify.TypedRoundTrip");

            ulong typeId = ComponentType<VerifyVelocity>.CachedId;
            Assert.That(typeId, Is.Not.Zero);

            var v = new VerifyVelocity { X = 1.5f, Y = -2.25f, Z = 1024f };
            ComponentType<VerifyVelocity>.Set(world, e, v);

            var got = ComponentType<VerifyVelocity>.Get(world, e);
            Assert.That(got.X, Is.EqualTo(v.X));
            Assert.That(got.Y, Is.EqualTo(v.Y));
            Assert.That(got.Z, Is.EqualTo(v.Z));

            Assert.That(world.HasComponent(e, typeId), Is.True);
            world.RemoveComponent(e, typeId);
            Assert.That(world.HasComponent(e, typeId), Is.False);
        }

        /// <summary>
        /// Component ids are the full 64-bit native hash: a registered id must have
        /// non-zero high bits eventually, and must round-trip through set/get. A
        /// truncated 32-bit id cannot match the native registry (the pre-v2.0 bug).
        /// </summary>
        [Test]
        public void ComponentIds_Are64Bit_AndRoundTrip()
        {
            var world = RequireWorld();

            ulong id = Ecs.RegisterBlobComponent("CS.Verify.Id64Probe", 16);
            Assert.That(id, Is.Not.Zero);

            // FNV-1a of any realistic name has entropy in the high 32 bits. If this
            // ever legitimately fails the id was truncated somewhere in the ABI.
            Assert.That(id >> 32, Is.Not.Zero,
                "component id high bits are zero — 64-bit id was truncated at the ABI boundary");

            uint e = world.CreateEmptyEntity("Verify.Id64");
            var payload = new byte[16];
            for (int i = 0; i < payload.Length; i++) payload[i] = (byte)(i + 1);
            world.SetComponentBytes(e, id, payload);
            Assert.That(world.GetComponentBytes(e, id), Is.EqualTo(payload));
        }

        /// <summary>
        /// Two components with the same simple name in different namespaces register
        /// as distinct component types (namespace-qualified registration).
        /// </summary>
        [Test]
        public void QualifiedNames_SameSimpleName_DifferentNamespace_AreDistinct()
        {
            var world = RequireWorld();

            ulong idA = ComponentType<NamespaceA.QualifiedNameProbe>.CachedId;
            ulong idB = ComponentType<NamespaceB.QualifiedNameProbe>.CachedId;
            Assert.That(idA, Is.Not.Zero);
            Assert.That(idB, Is.Not.Zero);
            Assert.That(idA, Is.Not.EqualTo(idB),
                "same simple name in different namespaces must register as distinct components");

            // Both are independently usable on the same entity.
            uint e = world.CreateEmptyEntity("Verify.QualifiedNames");
            ComponentType<NamespaceA.QualifiedNameProbe>.Set(world, e, new NamespaceA.QualifiedNameProbe { Value = 0xAAAAAAAAAAAAAAAA });
            ComponentType<NamespaceB.QualifiedNameProbe>.Set(world, e, new NamespaceB.QualifiedNameProbe { Value = 0xBBBBBBBBBBBBBBBB });
            Assert.That(ComponentType<NamespaceA.QualifiedNameProbe>.Get(world, e).Value, Is.EqualTo(0xAAAAAAAAAAAAAAAA));
            Assert.That(ComponentType<NamespaceB.QualifiedNameProbe>.Get(world, e).Value, Is.EqualTo(0xBBBBBBBBBBBBBBBB));
        }

        /// <summary>
        /// Re-registering an existing component name with a different size is rejected
        /// loudly (native logs an error and the managed call throws).
        /// </summary>
        [Test]
        public void DuplicateName_DifferentSize_RegistrationIsRejected()
        {
            RequireWorld();

            ulong first = Ecs.RegisterBlobComponent("CS.Verify.DupSize", 8);
            Assert.That(first, Is.Not.Zero);

            // Same name + same size: idempotent, returns the same id.
            Assert.That(Ecs.RegisterBlobComponent("CS.Verify.DupSize", 8), Is.EqualTo(first));

            // Same name + different size: rejected.
            Assert.Throws<InvalidOperationException>(
                () => Ecs.RegisterBlobComponent("CS.Verify.DupSize", 16));
        }

        /// <summary>
        /// While structural changes are deferred (the state generated systems run in),
        /// component sets/removes and destroys are queued and only apply on flush.
        /// </summary>
        [Test]
        public void DeferredFencing_StructuralChanges_ApplyOnFlush()
        {
            var world = RequireWorld();
            ulong w = world.Handle;

            uint e = world.CreateEmptyEntity("Verify.Fencing");
            ulong typeId = Ecs.RegisterBlobComponent("CS.Verify.Fence4", 4);
            world.SetComponentBytes(e, typeId, new byte[] { 1, 1, 1, 1 });

            ChunkQueryNative.SetDeferStructuralChanges(w, true);
            try
            {
                // Data set on an EXISTING component defers while the fence is up.
                world.SetComponentBytes(e, typeId, new byte[] { 2, 2, 2, 2 });
                Assert.That(world.GetComponentBytes(e, typeId), Is.EqualTo(new byte[] { 1, 1, 1, 1 }),
                    "deferred set must not be visible before flush");

                // Component add (set on a missing component) defers too.
                ulong addedTypeId = Ecs.RegisterBlobComponent("CS.Verify.FenceAdd4", 4);
                world.SetComponentBytes(e, addedTypeId, new byte[] { 9, 9, 9, 9 });
                Assert.That(world.HasComponent(e, addedTypeId), Is.False,
                    "deferred component add must not be visible before flush");

                // Destroy defers.
                unsafe { ChunkQueryNative.DeferCommand(w, 1, e, 0, null, 0); }
                Assert.That(world.IsEntityValid(e), Is.True,
                    "deferred destroy must not be visible before flush");

                ChunkQueryNative.FlushDeferredCommands(w);

                Assert.That(world.IsEntityValid(e), Is.False, "flushed destroy must apply");
            }
            finally
            {
                // FlushDeferredCommands resets deferral; make it unconditional for test isolation.
                ChunkQueryNative.SetDeferStructuralChanges(w, false);
            }
        }

        /// <summary>
        /// Stale generational handles: after destroy, the handle is invalid, reads
        /// fail, and double-destroy does not corrupt the world.
        /// </summary>
        [Test]
        public void StaleHandle_And_DoubleDestroy_AreSafe()
        {
            var world = RequireWorld();
            ulong w = world.Handle;

            uint e = world.CreateEmptyEntity("Verify.Stale");
            ulong typeId = Ecs.RegisterBlobComponent("CS.Verify.Stale4", 4);
            world.SetComponentBytes(e, typeId, new byte[] { 7, 7, 7, 7 });

            // Deferred double-destroy of the same entity: second is a no-op.
            unsafe
            {
                ChunkQueryNative.DeferCommand(w, 1, e, 0, null, 0);
                ChunkQueryNative.DeferCommand(w, 1, e, 0, null, 0);
            }
            ChunkQueryNative.FlushDeferredCommands(w);

            Assert.That(world.IsEntityValid(e), Is.False);
            Assert.Throws<InvalidOperationException>(() => world.GetComponentBytes(e, typeId),
                "reading a component off a stale handle must fail");
            Assert.Throws<InvalidOperationException>(() => world.SetComponentBytes(e, typeId, new byte[4]),
                "writing a component to a stale handle must fail");

            // The world still works after the stale accesses.
            uint e2 = world.CreateEmptyEntity("Verify.Stale.After");
            Assert.That(world.IsEntityValid(e2), Is.True);
        }

        /// <summary>
        /// Cached queries span multiple archetypes, and Reset() re-evaluates the
        /// archetype list after structural changes (new matching entities appear,
        /// removed ones disappear).
        /// </summary>
        [Test]
        public void CachedQuery_MultiArchetype_And_RefreshOnStructuralChange()
        {
            var world = RequireWorld();
            ulong w = world.Handle;

            ulong tagId = Ecs.RegisterBlobComponent("CS.Verify.QueryTag8", 8);
            ulong extraId = Ecs.RegisterBlobComponent("CS.Verify.QueryExtra8", 8);

            // Archetype 1: {Transform, Name, Tag}; Archetype 2: {Transform, Name, Tag, Extra}.
            uint e1 = world.CreateEmptyEntity("Verify.Query.A");
            world.SetComponentBytes(e1, tagId, new byte[8]);
            uint e2 = world.CreateEmptyEntity("Verify.Query.B");
            world.SetComponentBytes(e2, tagId, new byte[8]);
            world.SetComponentBytes(e2, extraId, new byte[8]);

            ulong query = ChunkQueryNative.CreateCachedQuery(w, new[] { tagId }, ReadOnlySpan<ulong>.Empty);
            try
            {
                var seen = CollectEntities(query);
                Assert.That(seen, Does.Contain(e1), "query must see the {Tag} archetype");
                Assert.That(seen, Does.Contain(e2), "query must see the {Tag, Extra} archetype");

                // Structural change AFTER the query was created: a third matching entity.
                uint e3 = world.CreateEmptyEntity("Verify.Query.C");
                world.SetComponentBytes(e3, tagId, new byte[8]);

                seen = CollectEntities(query);
                Assert.That(seen, Does.Contain(e3), "Reset must refresh cached archetypes after structural change");

                // Removing the tag takes the entity out of the results.
                world.RemoveComponent(e1, tagId);
                seen = CollectEntities(query);
                Assert.That(seen, Does.Not.Contain(e1), "entity without the component must drop out of the query");
                Assert.That(seen, Does.Contain(e2));
            }
            finally
            {
                ChunkQueryNative.DestroyCachedQuery(query);
            }
        }

        private static List<uint> CollectEntities(ulong query)
        {
            var result = new List<uint>();
            ChunkQueryNative.ResetQuery(query, out int archetypeCount);
            for (int a = 0; a < archetypeCount; a++)
            {
                ChunkQueryNative.GetArchetypeInfo(query, a, out _, out int chunkCount);
                for (int c = 0; c < chunkCount; c++)
                {
                    foreach (uint id in ChunkQueryNative.GetChunkEntityIds(query, a, c))
                        result.Add(id);
                }
            }
            return result;
        }

        /// <summary>
        /// Chunk spans read/write component memory directly, matching what the source
        /// generator emits: mutable spans for ref parameters, read-only for in.
        /// </summary>
        [Test]
        public void ChunkSpans_ReadAndWrite_ComponentMemory()
        {
            var world = RequireWorld();
            ulong w = world.Handle;

            uint e = world.CreateEmptyEntity("Verify.ChunkSpan");
            ComponentType<VerifyHealth>.Set(world, e, new VerifyHealth { Current = 10, Max = 100 });

            ulong query = ChunkQueryNative.CreateCachedQuery(
                w, new[] { ComponentType<VerifyHealth>.CachedId }, ReadOnlySpan<ulong>.Empty);
            try
            {
                ChunkQueryNative.ResetQuery(query, out int archetypeCount);
                bool mutated = false;
                for (int a = 0; a < archetypeCount; a++)
                {
                    ChunkQueryNative.GetArchetypeInfo(query, a, out _, out int chunkCount);
                    for (int c = 0; c < chunkCount; c++)
                    {
                        var ids = ChunkQueryNative.GetChunkEntityIds(query, a, c);
                        var span = ChunkQueryNative.GetChunkSpan<VerifyHealth>(
                            query, a, c, ComponentType<VerifyHealth>.CachedId);
                        Assert.That(span.Length, Is.EqualTo(ids.Length),
                            "component span and entity-id span must cover the same chunk rows");
                        for (int i = 0; i < span.Length; i++)
                        {
                            if (ids[i] == e)
                            {
                                Assert.That(span[i].Current, Is.EqualTo(10));
                                span[i].Current = 55; // write through the span
                                mutated = true;
                            }
                        }
                    }
                }
                Assert.That(mutated, Is.True, "the entity must be visible through chunk iteration");
            }
            finally
            {
                ChunkQueryNative.DestroyCachedQuery(query);
            }

            Assert.That(ComponentType<VerifyHealth>.Get(world, e).Current, Is.EqualTo(55),
                "writes through the chunk span must land in component storage");
        }

        /// <summary>
        /// Built-in engine components are readable and writable from C# through the
        /// [BuiltInComponent] mirror (Transform by canonical native name), and the
        /// byte path agrees with the dedicated transform accessors.
        /// </summary>
        [Test]
        public void BuiltInTransform_TypedAccess_MatchesTransformApi()
        {
            var world = RequireWorld();

            ulong transformId;
            try
            {
                transformId = ComponentType<GameEngine.Scripting.Transform>.CachedId;
            }
            catch (Exception ex)
            {
                Assert.Inconclusive("Built-in Transform not registered in this environment: " + ex.Message);
                return;
            }
            Assert.That(transformId, Is.Not.Zero);

            uint e = world.CreateEmptyEntity("Verify.BuiltInTransform");

            var t = GameEngine.Scripting.Transform.FromTRS(2f, 3f, 4f, 0f, 0f, 0f, 1f, 1f, 1f, 1f);
            ComponentType<GameEngine.Scripting.Transform>.Set(world, e, t);

            float[] m = world.GetTransformMatrix(e);
            Assert.That(m[12], Is.EqualTo(2f).Within(0.0001f));
            Assert.That(m[13], Is.EqualTo(3f).Within(0.0001f));
            Assert.That(m[14], Is.EqualTo(4f).Within(0.0001f));

            var read = ComponentType<GameEngine.Scripting.Transform>.Get(world, e);
            Assert.That(read.X, Is.EqualTo(2f).Within(0.0001f));
        }

        /// <summary>
        /// LH FromTRS: +Y 90° sends +Z toward +X. FromTRSRH is the old transpose.
        /// </summary>
        [Test]
        public void BuiltInTransform_FromTRS_YawPositiveSendsZTowardX()
        {
            const float halfPi = 1.57079632679f;
            var q = System.Numerics.Quaternion.CreateFromAxisAngle(
                System.Numerics.Vector3.UnitY, halfPi);
            var t = GameEngine.Scripting.Transform.FromTRS(
                System.Numerics.Vector3.Zero, q, System.Numerics.Vector3.One);
            var forward = t.Forward;
            Assert.That(forward.X, Is.EqualTo(1f).Within(1e-4f));
            Assert.That(forward.Y, Is.EqualTo(0f).Within(1e-4f));
            Assert.That(forward.Z, Is.EqualTo(0f).Within(1e-4f));

            var back = t.GetRotation();
            var tRh = GameEngine.Scripting.Transform.FromTRSRH(
                System.Numerics.Vector3.Zero, q, System.Numerics.Vector3.One);
            Assert.That(tRh.Forward.X, Is.EqualTo(-1f).Within(1e-4f));
            Assert.That(back.X, Is.EqualTo(q.X).Within(1e-4f));
            Assert.That(back.Y, Is.EqualTo(q.Y).Within(1e-4f));
            Assert.That(back.Z, Is.EqualTo(q.Z).Within(1e-4f));
            Assert.That(back.W, Is.EqualTo(q.W).Within(1e-4f));
        }

        /// <summary>
        /// GameSystemRunner lifecycle: systems execute in topologically sorted order
        /// ([After] edges win over Order; Order breaks ties), Destroy runs on
        /// Shutdown, and re-registration replaces rather than duplicates.
        /// </summary>
        [Test]
        public void GameSystemRunner_Ordering_Lifecycle_And_Reregistration()
        {
            var world = RequireWorld();
            var log = new List<string>();

            // C declares [After A] but has the LOWEST order — the dependency must win.
            GameSystemRunner.RegisterEntitySystem("Verify.SysC", 0,
                (w, dt) => log.Add("C"), () => log.Add("C.Destroy"),
                runAfter: new[] { "Verify.SysA" });
            GameSystemRunner.RegisterEntitySystem("Verify.SysA", 5,
                (w, dt) => log.Add("A"), () => log.Add("A.Destroy"));
            GameSystemRunner.RegisterEntitySystem("Verify.SysB", 1,
                (w, dt) => log.Add("B"), () => log.Add("B.Destroy"));

            try
            {
                GameSystemRunner.Initialize(world.Handle);
                GameSystemRunner.Tick(0.016f);

                int a = log.IndexOf("A");
                int b = log.IndexOf("B");
                int c = log.IndexOf("C");
                Assert.That(a, Is.GreaterThanOrEqualTo(0), "system A must execute");
                Assert.That(b, Is.GreaterThanOrEqualTo(0), "system B must execute");
                Assert.That(c, Is.GreaterThanOrEqualTo(0), "system C must execute");
                Assert.That(c, Is.GreaterThan(a), "[After(A)] must order C after A despite lower Order");
                Assert.That(b, Is.LessThan(a), "Order tiebreak: B (1) before A (5)");

                // Hot-reload semantics: re-registering while initialized replaces the entry.
                log.Clear();
                GameSystemRunner.RegisterEntitySystem("Verify.SysB", 1,
                    (w, dt) => log.Add("B2"), () => { });
                GameSystemRunner.Tick(0.016f);
                Assert.That(log.FindAll(s => s == "B2").Count, Is.EqualTo(1),
                    "re-registered system must execute exactly once per tick");
                Assert.That(log.Contains("B"), Is.False, "old registration must be replaced");
            }
            finally
            {
                log.Clear();
                GameSystemRunner.Shutdown();
            }

            Assert.That(log, Does.Contain("A.Destroy"), "Shutdown must destroy systems");
        }

        /// <summary>
        /// The EntityCommands flow generated systems use: create an entity mid-tick
        /// (raw create is legal under the fence), queue component adds, and observe
        /// them only after the flush.
        /// </summary>
        [Test]
        public void EntityCommands_CreateAndAdd_UnderFence()
        {
            var world = RequireWorld();
            ulong w = world.Handle;

            ChunkQueryNative.SetDeferStructuralChanges(w, true);
            uint created;
            try
            {
                created = ChunkQueryNative.CreateEntityRaw(w);
                Assert.That(created, Is.Not.Zero);
                Assert.That(world.IsEntityValid(created), Is.True,
                    "raw entity creation returns a live handle immediately");

                var vel = new VerifyVelocity { X = 3f, Y = 2f, Z = 1f };
                unsafe
                {
                    ChunkQueryNative.DeferCommand(w, 2, created,
                        ComponentType<VerifyVelocity>.CachedId, &vel, (uint)sizeof(VerifyVelocity));
                }
                Assert.That(world.HasComponent(created, ComponentType<VerifyVelocity>.CachedId), Is.False,
                    "deferred add must not be visible before flush");

                ChunkQueryNative.FlushDeferredCommands(w);
            }
            finally
            {
                ChunkQueryNative.SetDeferStructuralChanges(w, false);
            }

            Assert.That(world.HasComponent(created, ComponentType<VerifyVelocity>.CachedId), Is.True);
            Assert.That(ComponentType<VerifyVelocity>.Get(world, created).X, Is.EqualTo(3f));
        }

        private static int CountEntitiesWith(ulong worldHandle, params ulong[] required)
        {
            ulong query = ChunkQueryNative.CreateCachedQuery(worldHandle, required, ReadOnlySpan<ulong>.Empty);
            try
            {
                int total = 0;
                ChunkQueryNative.ResetQuery(query, out int archetypeCount);
                for (int a = 0; a < archetypeCount; a++)
                {
                    ChunkQueryNative.GetArchetypeInfo(query, a, out int entityCount, out _);
                    total += entityCount;
                }
                return total;
            }
            finally
            {
                ChunkQueryNative.DestroyCachedQuery(query);
            }
        }

        private static uint CreateWithVelocity(WorldHandle world)
        {
            uint entity = world.CreateEmptyEntity("EnableState.Mover");
            ComponentType<VerifyVelocity>.Set(world, entity, new VerifyVelocity { X = 1f });
            return entity;
        }

        /// <summary>
        /// A component switched off through the deferred commands is skipped by every
        /// query that requires it, as the native enable model does, keeps its data, and
        /// comes back when switched on again.
        /// </summary>
        [Test]
        public void EntityCommands_SetEnabledOfAComponent_TakesItOutOfQueries()
        {
            var world = RequireWorld();
            ulong w = world.Handle;
            ChunkQueryNative.FlushDeferredCommands(w); // an earlier test's queued destroy is not this count's
            ulong velocityId = ComponentType<VerifyVelocity>.CachedId;
            uint entity = CreateWithVelocity(world);
            int before = CountEntitiesWith(w, velocityId);
            var commands = new EntityCommands { WorldHandle = w };

            ChunkQueryNative.SetDeferStructuralChanges(w, true);
            try
            {
                commands.SetEnabled<VerifyVelocity>(entity, false);
                Assert.That(world.IsEnabled<VerifyVelocity>(entity), Is.True, "not before the flush");
                ChunkQueryNative.FlushDeferredCommands(w);
            }
            finally
            {
                ChunkQueryNative.SetDeferStructuralChanges(w, false);
            }

            Assert.That(world.IsEnabled<VerifyVelocity>(entity), Is.False);
            Assert.That(CountEntitiesWith(w, velocityId), Is.EqualTo(before - 1));
            Assert.That(ComponentType<VerifyVelocity>.Get(world, entity).X, Is.EqualTo(1f),
                "a switched-off component keeps its data");

            ChunkQueryNative.SetDeferStructuralChanges(w, true);
            try
            {
                commands.SetEnabled<VerifyVelocity>(entity, true);
                ChunkQueryNative.FlushDeferredCommands(w);
            }
            finally
            {
                ChunkQueryNative.SetDeferStructuralChanges(w, false);
            }

            Assert.That(world.IsEnabled<VerifyVelocity>(entity), Is.True);
            Assert.That(CountEntitiesWith(w, velocityId), Is.EqualTo(before));
            world.DestroyEntity(entity);
            ChunkQueryNative.FlushDeferredCommands(w); // DestroyEntity queues; apply it here, not in the next test
        }

        /// <summary>
        /// A component with no on/off state (Transform) is always on, and switching it is
        /// refused with a message that names it instead of queuing a tag nothing reads.
        /// </summary>
        [Test]
        public void EntityCommands_SetEnabledOfANotToggleableComponent_IsRefusedAndItStaysOn()
        {
            var world = RequireWorld();
            ulong w = world.Handle;
            ChunkQueryNative.FlushDeferredCommands(w);
            uint entity = CreateWithVelocity(world);
            var commands = new EntityCommands { WorldHandle = w };

            Assert.That(ComponentType<Transform>.DisabledTypeId, Is.EqualTo(0UL));
            Assert.That(world.IsEnabled<Transform>(entity), Is.True);
            var refusal = Assert.Throws<InvalidOperationException>(() => commands.SetEnabled<Transform>(entity, false));
            Assert.That(refusal!.Message, Does.Contain("'Transform' is not toggleable"));

            world.DestroyEntity(entity);
            ChunkQueryNative.FlushDeferredCommands(w); // DestroyEntity queues; apply it here, not in the next test
        }

        /// <summary>
        /// An entity switched off through the deferred commands leaves every query,
        /// including one that does not name any of its components' tags.
        /// </summary>
        [Test]
        public void EntityCommands_SetEnabledOfAnEntity_TakesItOutOfQueries()
        {
            var world = RequireWorld();
            ulong w = world.Handle;
            ChunkQueryNative.FlushDeferredCommands(w); // an earlier test's queued destroy is not this count's
            ulong velocityId = ComponentType<VerifyVelocity>.CachedId;
            uint entity = CreateWithVelocity(world);
            int before = CountEntitiesWith(w, velocityId);
            var commands = new EntityCommands { WorldHandle = w };

            ChunkQueryNative.SetDeferStructuralChanges(w, true);
            try
            {
                commands.SetEnabled(entity, false);
                ChunkQueryNative.FlushDeferredCommands(w);
            }
            finally
            {
                ChunkQueryNative.SetDeferStructuralChanges(w, false);
            }

            Assert.That(CountEntitiesWith(w, velocityId), Is.EqualTo(before - 1));
            Assert.That(world.IsEnabled<VerifyVelocity>(entity), Is.True,
                "the component's own state is separate from the entity's");

            ChunkQueryNative.SetDeferStructuralChanges(w, true);
            try
            {
                commands.SetEnabled(entity, true);
                ChunkQueryNative.FlushDeferredCommands(w);
            }
            finally
            {
                ChunkQueryNative.SetDeferStructuralChanges(w, false);
            }

            Assert.That(CountEntitiesWith(w, velocityId), Is.EqualTo(before));
            world.DestroyEntity(entity);
            ChunkQueryNative.FlushDeferredCommands(w); // DestroyEntity queues; apply it here, not in the next test
        }
    }
}
