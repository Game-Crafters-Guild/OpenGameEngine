using System;
using System.Reflection;
using NUnit.Framework;
using GameEngine;
using GameEngine.Interop;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// ECS façade covering both strict and Try* accessors.
    /// </summary>
    public class ECSFacadeTests
    {
        /// <summary>
        /// Property should not throw and return a non-negative count.
        /// </summary>
        [Test]
        public void PrimaryWorld_EntityCount_Accessible()
        {
            var world = EcsTestHelpers.RequirePrimaryWorld("ECSFacade.PrimaryWorld_EntityCount_Accessible");
            Assert.That(world.EntityCount, Is.GreaterThanOrEqualTo(0));
        }

        /// <summary>
        /// Try pattern should succeed and return a non-negative count.
        /// </summary>
        [Test]
        public void PrimaryWorld_TryGetEntityCount_Succeeds()
        {
            var world = EcsTestHelpers.RequirePrimaryWorld("ECSFacade.PrimaryWorld_TryGetEntityCount_Succeeds");
            Assert.That(world.TryGetEntityCount(out var count), Is.True);
            Assert.That(count, Is.GreaterThanOrEqualTo(0));
        }

        /// <summary>
        /// When the native binding reports failure, Primary should throw with a clear message.
        /// This exercises the negative path without depending on a particular host configuration
        /// by injecting a stub EngineNativeBinding via reflection.
        /// </summary>
        [Test]
        public void PrimaryWorld_Throws_WhenWorldUnavailable()
        {
            // Arrange a stub binding that always fails to provide a world handle.
            var binding = new EngineNativeBinding
            {
                ECS_GetWorldHandle = (out ulong world) =>
                {
                    world = 0;
                    return (int)GeResult.Fail;
                }
            };

            // Create a context and inject the stub binding without loading any native DLL.
            var ctx = new EngineInstanceContext(nativePath: "does_not_matter.dll");
            var bindingField = typeof(EngineInstanceContext).GetField("m_binding", BindingFlags.Instance | BindingFlags.NonPublic);
            Assert.That(bindingField, Is.Not.Null, "Expected EngineInstanceContext.m_binding field to exist");
            bindingField!.SetValue(ctx, binding);

            // Construct an EngineContext bound to our stub context via the internal constructor.
            var engineContextCtor = typeof(EngineContext).GetConstructor(
                BindingFlags.Instance | BindingFlags.NonPublic,
                binder: null,
                types: new[] { typeof(EngineInstanceContext) },
                modifiers: null);
            Assert.That(engineContextCtor, Is.Not.Null, "Expected EngineContext(EngineInstanceContext) ctor to exist");
            var engine = (EngineContext)engineContextCtor!.Invoke(new object[] { ctx });

            // Act / Assert: accessing Primary should throw a clear InvalidOperationException.
            var ex = Assert.Throws<InvalidOperationException>(() =>
            {
                var _ = engine.Worlds.Primary;
            });
            Assert.That(ex!.Message, Is.EqualTo("Primary world not available"));
        }
    }
}


