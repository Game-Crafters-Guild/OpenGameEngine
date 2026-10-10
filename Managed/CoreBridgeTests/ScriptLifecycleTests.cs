using System;
using System.IO;
using NUnit.Framework;

namespace GameEngine.CoreBridgeTests
{
    /// <summary>
    /// The supported case: one process hosts one project, whose scripts are loaded, swapped in,
    /// unloaded and loaded again.
    /// </summary>
    public class ScriptLifecycleTests
    {
        private const string kReset = "GameEngine.Scripts.ScriptsEntryPoint.Reset";
        private const string kIncrementAndGet = "GameEngine.Scripts.ScriptsEntryPoint.IncrementAndGet";
        private const string kGetValue = "GameEngine.Scripts.ScriptsEntryPoint.GetValue";

        private static Type CoreBridgeType => Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge", throwOnError: true)!;

        // Built and copied beside the test assembly by the DomainRoutingTest project reference.
        private static string FindDomainRoutingTest()
        {
            var path = Path.Combine(AppContext.BaseDirectory, "DomainRoutingTest.dll");
            Assert.That(File.Exists(path), Is.True, $"DomainRoutingTest.dll is not beside the test assembly at {path}");
            return path;
        }

        private static int ManagedPreload(string dllPath)
        {
            var mi = CoreBridgeType.GetMethod("ManagedPreloadAssemblyContext", System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic)!;
            return (int)mi.Invoke(null, new object?[] { dllPath })!;
        }

        private static int ManagedSwap()
        {
            var mi = CoreBridgeType.GetMethod("ManagedSwapPreloadedContext", System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic)!;
            return (int)mi.Invoke(null, null)!;
        }

        private static int ManagedCall(string fqn)
        {
            var mi = CoreBridgeType.GetMethod("ManagedCallUserScriptsMethod", System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic)!;
            return (int)mi.Invoke(null, new object?[] { fqn })!;
        }

        private static int Unload()
        {
            var mi = CoreBridgeType.GetMethod("Demo_UnloadUserScriptsAssembly", System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.Public)!;
            return (int)mi.Invoke(null, null)!;
        }

        /// <summary>
        /// Load and swap make the scripts callable, unload makes them uncallable, and loading
        /// again starts the scripts from fresh static state.
        /// </summary>
        [Test]
        public void LoadSwapUnloadLoadAgain()
        {
            string scriptDll = FindDomainRoutingTest();

            Assert.That(ManagedPreload(scriptDll), Is.EqualTo(0));
            Assert.That(ManagedSwap(), Is.EqualTo(0));
            Assert.That(ManagedCall(kReset), Is.EqualTo(0));
            Assert.That(ManagedCall(kIncrementAndGet), Is.EqualTo(1));
            Assert.That(ManagedCall(kIncrementAndGet), Is.EqualTo(2));

            Assert.That(Unload(), Is.EqualTo(0));
            Assert.That(ManagedCall(kGetValue), Is.LessThan(0), "Scripts stayed callable after unload");

            Assert.That(ManagedPreload(scriptDll), Is.EqualTo(0));
            Assert.That(ManagedSwap(), Is.EqualTo(0));
            Assert.That(ManagedCall(kGetValue), Is.EqualTo(0), "Loading again kept the unloaded scripts' static state");
            Assert.That(ManagedCall(kIncrementAndGet), Is.EqualTo(1));
        }
    }
}
