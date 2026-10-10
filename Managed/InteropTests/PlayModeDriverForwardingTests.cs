using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using NUnit.Framework;
using GameEngine.Scripting;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Ensures Play Mode lifecycle is handled by the Editor-owned driver and that CoreBridge remains decoupled.
    /// </summary>
    public class PlayModeDriverForwardingTests
    {
        private static int s_enterCount;
        private static int s_exitCount;
        private static int s_tickCount;
        private static float s_lastDt;

        private static class HookHost
        {
            [PlayModeEnter]
            private static void Enter() => s_enterCount++;

            [PlayModeExit]
            private static void Exit() => s_exitCount++;

            [PlayModeTick]
            private static void Tick(float dt)
            {
                s_tickCount++;
                s_lastDt = dt;
            }
        }

        /// <summary>
        /// Verifies Play Mode is editor-owned and exposed via native-callable exports.
        /// </summary>
        [Test]
        public void PlayModeDriver_IsEditorOwned_And_ExportsAreNativeCallable()
        {
            // Ensure hook host is referenced so its type is loaded.
            _ = typeof(HookHost);

            s_enterCount = 0;
            s_exitCount = 0;
            s_tickCount = 0;
            s_lastDt = 0;

            // CoreBridge should not embed Editor policy helpers anymore.
            var coreBridgeType = Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge");
            Assert.That(coreBridgeType, Is.Not.Null);
            Assert.That(coreBridgeType!.GetMethod("TryInvokeEditorPlayModeDriver", BindingFlags.NonPublic | BindingFlags.Static),
                        Is.Null);

            // Managed driver should exist and be callable from managed code.
            var driverType = Type.GetType("GameEngine.Editor.Managed.PlayModeDriver, GameEngine.Editor.Managed");
            Assert.That(driverType, Is.Not.Null);
            var onEnterMi = driverType!.GetMethod("OnEnter", BindingFlags.Public | BindingFlags.Static);
            var onExitMi = driverType!.GetMethod("OnExit", BindingFlags.Public | BindingFlags.Static);
            var tickMi = driverType!.GetMethod("Tick", BindingFlags.Public | BindingFlags.Static);
            Assert.That(onEnterMi, Is.Not.Null);
            Assert.That(onExitMi, Is.Not.Null);
            Assert.That(tickMi, Is.Not.Null);

            // Driver should discover our hooks and run them.
            Assert.That((int)(onEnterMi!.Invoke(null, null) ?? -999), Is.EqualTo(0));
            Assert.That(s_enterCount, Is.EqualTo(1));

            const float dt = 0.016f;
            Assert.That((int)(tickMi!.Invoke(null, new object?[] { dt }) ?? -999), Is.EqualTo(0));
            Assert.That(s_tickCount, Is.EqualTo(1));
            Assert.That(s_lastDt, Is.EqualTo(dt));

            Assert.That((int)(onExitMi!.Invoke(null, null) ?? -999), Is.EqualTo(0));
            Assert.That(s_exitCount, Is.EqualTo(1));

            // Native-callable exports must exist and be marked UnmanagedCallersOnly.
            var exportsType = Type.GetType("GameEngine.Editor.Managed.PlayModeDriverExports, GameEngine.Editor.Managed");
            Assert.That(exportsType, Is.Not.Null);
            var exEnter = exportsType!.GetMethod("OnEnter", BindingFlags.Public | BindingFlags.Static);
            var exExit = exportsType!.GetMethod("OnExit", BindingFlags.Public | BindingFlags.Static);
            var exTick = exportsType!.GetMethod("OnTick", BindingFlags.Public | BindingFlags.Static);
            Assert.That(exEnter, Is.Not.Null);
            Assert.That(exExit, Is.Not.Null);
            Assert.That(exTick, Is.Not.Null);

            static void AssertIsUnmanagedCdecl(MethodInfo mi)
            {
                var uco = mi.GetCustomAttribute<UnmanagedCallersOnlyAttribute>();
                Assert.That(uco, Is.Not.Null, $"{mi.Name} should be [UnmanagedCallersOnly]");
                Assert.That(uco!.CallConvs, Does.Contain(typeof(CallConvCdecl)));
            }

            AssertIsUnmanagedCdecl(exEnter!);
            AssertIsUnmanagedCdecl(exExit!);
            AssertIsUnmanagedCdecl(exTick!);
        }
    }
}

