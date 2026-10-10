using System;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using NUnit.Framework;
using GameEngine.Interop;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Tests that EngineLogWriter routes via EngineNativeBinding.Log when an engine
    /// instance is registered, and falls back to GE_Log when no binding is present.
    /// </summary>
    public class EngineLogWriterRoutingTests
    {
        private static Type GetCoreBridgeType()
        {
            // CoreBridge is a static class inside the GameEngine.CoreBridge namespace and
            // assembly. We resolve it explicitly by name to avoid namespace/type confusion.
            var type = Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge",
                throwOnError: true)!;
            return type;
        }

        private static Type GetLogWriterType()
        {
            var coreBridgeType = GetCoreBridgeType();
            var logWriterType = coreBridgeType.Assembly.GetType(
                "GameEngine.CoreBridge.EngineLogWriter",
                throwOnError: true)!;
            return logWriterType;
        }

        private static void CallLogWriterInstall(Type logWriterType)
        {
            var install = logWriterType.GetMethod("Install", BindingFlags.Static | BindingFlags.NonPublic);
            install!.Invoke(null, new object?[] { true });
        }

        private static void CallLogWriterUninstall(Type logWriterType)
        {
            var uninstall = logWriterType.GetMethod("Uninstall", BindingFlags.Static | BindingFlags.NonPublic);
            uninstall!.Invoke(null, null);
        }

        private static FieldInfo GetEngineInstanceField()
        {
            var field = GetCoreBridgeType().GetField("s_engineInstance", BindingFlags.Static | BindingFlags.NonPublic);
            Assert.That(field, Is.Not.Null);
            return field!;
        }

        /// <summary>
        /// When an engine instance with an EngineNativeBinding is registered, EngineLogWriter
        /// should route logs via the binding.Log delegate (no native fallback).
        /// </summary>
        [Test]
        public void RoutesViaBindingLog_WhenEngineInstanceRegistered()
        {
            var logWriterType = GetLogWriterType();

            // Ensure a clean install state
            CallLogWriterUninstall(logWriterType);

            // Reset fallback flag so we can assert it remains unused
            var reportedFallbackField = logWriterType.GetField("s_reportedFallback", BindingFlags.Static | BindingFlags.NonPublic);
            Assert.That(reportedFallbackField, Is.Not.Null);
            reportedFallbackField!.SetValue(null, false);

            // A synthetic engine instance with a stub binding
            var ctx = new EngineInstanceContext(nativePath: "unused-native-path");

            // Inject a stub EngineNativeBinding with a recording Log delegate
            var binding = new EngineNativeBinding();
            int callCount = 0;
            string? lastMessage = null;
            EngineNativeBinding.GE_Log_Delegate del = (level, msg, len) =>
            {
                if (msg != 0 && len > 0)
                {
                    var bytes = new byte[len];
                    Marshal.Copy(msg, bytes, 0, (int)len);
                    lastMessage = Encoding.UTF8.GetString(bytes, 0, (int)len);
                }
                callCount++;
                return 0;
            };
            binding.Log = del;

            var bindingField = typeof(EngineInstanceContext).GetField("m_binding", BindingFlags.Instance | BindingFlags.NonPublic);
            Assert.That(bindingField, Is.Not.Null);
            bindingField!.SetValue(ctx, binding);

            // Make it CoreBridge's registered engine instance
            var instanceField = GetEngineInstanceField();
            var prevInstance = instanceField.GetValue(null);
            instanceField.SetValue(null, ctx);

            try
            {
                CallLogWriterInstall(logWriterType);

                const string marker = "[RoutingTest] Binding route";
                Console.WriteLine(marker);

                Assert.That(callCount, Is.GreaterThan(0), "Expected binding.Log delegate to be invoked.");
                Assert.That(lastMessage, Does.Contain(marker));

                bool fallbackReported = (bool)(reportedFallbackField.GetValue(null) ?? false);
                Assert.That(fallbackReported, Is.False, "Fallback path should not be used when binding is present.");
            }
            finally
            {
                CallLogWriterUninstall(logWriterType);
                instanceField.SetValue(null, prevInstance);
                reportedFallbackField.SetValue(null, false);
            }
        }

        /// <summary>
        /// When no engine instance is registered, EngineLogWriter should fall back to calling
        /// GE_Log via the native P/Invoke path and mark the one-time fallback flag.
        /// </summary>
        [Test]
        public void FallsBackToGeLog_WhenNoBindingPresent()
        {
            var logWriterType = GetLogWriterType();

            // Ensure a clean install state and reset fallback flag
            CallLogWriterUninstall(logWriterType);
            var reportedFallbackField = logWriterType.GetField("s_reportedFallback", BindingFlags.Static | BindingFlags.NonPublic);
            Assert.That(reportedFallbackField, Is.Not.Null);
            reportedFallbackField!.SetValue(null, false);

            // Clear the registered engine instance so no binding is available
            var instanceField = GetEngineInstanceField();
            var prevInstance = instanceField.GetValue(null);
            instanceField.SetValue(null, null);

            try
            {
                CallLogWriterInstall(logWriterType);

                const string marker = "[RoutingTest] Native fallback";
                Console.WriteLine(marker);

                bool fallbackReported = (bool)(reportedFallbackField.GetValue(null) ?? false);
                Assert.That(fallbackReported, Is.True, "Expected native fallback path to be used when no binding is available.");
            }
            finally
            {
                CallLogWriterUninstall(logWriterType);
                instanceField.SetValue(null, prevInstance);
                reportedFallbackField.SetValue(null, false);
            }
        }
    }
}

