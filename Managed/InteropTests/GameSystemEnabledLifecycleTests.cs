using System;
using NUnit.Framework;
using GameEngine.ECS;
using GameEngine.Scripting;
using GameEngine.Scripting.Runtime;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// SetSystemEnabled must mirror native ISystem enable semantics for GameSystems:
    /// an actual state transition fires OnDisable/OnEnable, re-applying the current
    /// state is a lifecycle no-op, a disabled system is skipped by Tick, and
    /// exceptions thrown by the lifecycle callbacks are isolated. The lifecycle
    /// half is pure managed runner logic (no native world needed); only the
    /// Tick-skip half gates on world availability like the other ABI tests.
    /// </summary>
    public class GameSystemEnabledLifecycleTests
    {
        private sealed class EnableProbeSystem : GameSystem
        {
            public int CreateCount;
            public int EnableCount;
            public int DisableCount;
            public int DestroyCount;
            public bool ThrowOnEnable;
            public bool ThrowOnDisable;

            public override void OnCreate() => CreateCount++;

            public override void OnEnable()
            {
                EnableCount++;
                if (ThrowOnEnable) throw new InvalidOperationException("OnEnable probe throw");
            }

            public override void OnDisable()
            {
                DisableCount++;
                if (ThrowOnDisable) throw new InvalidOperationException("OnDisable probe throw");
            }

            public override void OnDestroy() => DestroyCount++;
        }

        private sealed class TickProbeSystem : GameSystem
        {
            public int UpdateCount;
            public override void OnUpdate(float deltaTime) => UpdateCount++;
        }

        /// <summary>
        /// Disable fires OnDisable once; re-applying the same state fires nothing;
        /// re-enable fires OnEnable; throwing callbacks neither propagate nor prevent
        /// the flag from landing; Shutdown sees the final flag (no double OnDisable).
        /// Runner-only logic — no native world required.
        /// </summary>
        [Test]
        public void SetSystemEnabled_Transition_FiresLifecycle()
        {
            string name = typeof(EnableProbeSystem).FullName!;
            EnableProbeSystem? probe = null;
            GameSystemRunner.RegisterGameSystem(name, () => probe = new EnableProbeSystem(), order: 0);

            try
            {
                GameSystemRunner.Initialize(0);
                Assert.That(probe, Is.Not.Null, "factory must run on Initialize");
                Assert.That(probe!.CreateCount, Is.EqualTo(1));
                Assert.That(probe.EnableCount, Is.EqualTo(1), "Create fires OnEnable for enabled systems");

                // Disable: transition fires OnDisable exactly once.
                Assert.That(GameSystemRunner.SetSystemEnabled(name, false), Is.True);
                Assert.That(probe.DisableCount, Is.EqualTo(1), "disable transition must fire OnDisable");

                // Re-applying the same state is a lifecycle no-op.
                Assert.That(GameSystemRunner.SetSystemEnabled(name, false), Is.True);
                Assert.That(probe.DisableCount, Is.EqualTo(1), "no transition, no OnDisable");

                // Re-enable: transition fires OnEnable.
                Assert.That(GameSystemRunner.SetSystemEnabled(name, true), Is.True);
                Assert.That(probe.EnableCount, Is.EqualTo(2), "enable transition must fire OnEnable");
                Assert.That(GameSystemRunner.SetSystemEnabled(name, true), Is.True);
                Assert.That(probe.EnableCount, Is.EqualTo(2), "no transition, no OnEnable");

                // Lifecycle exceptions are isolated: the flag still lands and the
                // opposite transition still fires.
                probe.ThrowOnDisable = true;
                Assert.That(() => GameSystemRunner.SetSystemEnabled(name, false), Throws.Nothing);
                Assert.That(probe.DisableCount, Is.EqualTo(2));

                probe.ThrowOnEnable = true;
                Assert.That(() => GameSystemRunner.SetSystemEnabled(name, true), Throws.Nothing);
                Assert.That(probe.EnableCount, Is.EqualTo(3));

                // End disabled so Shutdown's Destroy must NOT re-fire OnDisable.
                probe.ThrowOnDisable = false;
                Assert.That(GameSystemRunner.SetSystemEnabled(name, false), Is.True);
                Assert.That(probe.DisableCount, Is.EqualTo(3));
            }
            finally
            {
                GameSystemRunner.Shutdown();
            }

            Assert.That(probe!.DisableCount, Is.EqualTo(3), "already-disabled system gets no extra OnDisable at Shutdown");
            Assert.That(probe.DestroyCount, Is.EqualTo(1));
        }

        /// <summary>
        /// The flag's Tick-skip semantics are unchanged: a disabled system is not
        /// updated, re-enabling resumes updates. Needs a live world (Tick fences
        /// structural changes through the ABI), so it gates like the EcsAbi tests.
        /// </summary>
        [Test]
        public void SetSystemEnabled_DisabledSystem_IsSkippedByTick()
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

            string name = typeof(TickProbeSystem).FullName!;
            TickProbeSystem? probe = null;
            GameSystemRunner.RegisterGameSystem(name, () => probe = new TickProbeSystem(), order: 0);

            try
            {
                GameSystemRunner.Initialize(world.Handle);
                Assert.That(probe, Is.Not.Null);

                GameSystemRunner.Tick(0.016f);
                Assert.That(probe!.UpdateCount, Is.EqualTo(1));

                Assert.That(GameSystemRunner.SetSystemEnabled(name, false), Is.True);
                GameSystemRunner.Tick(0.016f);
                Assert.That(probe.UpdateCount, Is.EqualTo(1), "disabled system must be skipped");

                Assert.That(GameSystemRunner.SetSystemEnabled(name, true), Is.True);
                GameSystemRunner.Tick(0.016f);
                Assert.That(probe.UpdateCount, Is.EqualTo(2), "re-enabled system must resume");
            }
            finally
            {
                GameSystemRunner.Shutdown();
            }
        }

        /// <summary>Unknown system names report false without firing anything.</summary>
        [Test]
        public void SetSystemEnabled_UnknownName_ReturnsFalse()
        {
            Assert.That(GameSystemRunner.SetSystemEnabled("No.Such.System." + Guid.NewGuid().ToString("N"), true), Is.False);
        }
    }
}
