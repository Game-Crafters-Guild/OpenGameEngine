using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Loader;
using NUnit.Framework;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>Minimal tests for HRM.PreloadAssemblyContext(byte[], byte[]?)</summary>
    public class HotReloadPreloadBytesTests
    {
        private static Type GetCoreBridgeType()
        {
            var type = Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge", throwOnError: false);
            Assert.That(type, Is.Not.Null, "CoreBridge type not found");
            return type!;
        }

        private static void EnsureHrmLoaded()
        {
            ScriptCompileHelper.EnsureHrmLoaded();
        }

        private static (byte[] asm, byte[]? pdb) CompileSimpleAssemblyReturning(int value)
        {
            return ScriptCompileHelper.CompileAssemblyReturning(value);
        }

        private static int CallScript(string fqn)


        {
            var cbType = GetCoreBridgeType();
            var mi = cbType.GetMethod("ManagedCallUserScriptsMethod", BindingFlags.NonPublic | BindingFlags.Static)!;
            return (int)mi.Invoke(null, new object[]{ fqn })!;
        }

        /// <summary>Preload from bytes without PDB then swap; method should execute.</summary>

        [Test]
        public void PreloadBytes_ThenSwap_InvokesMethod()
        {
            EnsureHrmLoaded();
            var cbType = GetCoreBridgeType();
            cbType.GetMethod("Demo_InitializeManaged", BindingFlags.Public | BindingFlags.Static)!.Invoke(null, null);

            var (asm, _) = CompileSimpleAssemblyReturning(7);
            var hrm = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: true)!;
            var miPreload = hrm.GetMethod("PreloadAssemblyContext", BindingFlags.Public | BindingFlags.Static, binder: null, types: new[]{ typeof(byte[]), typeof(byte[]) }, modifiers: null)!;
            var miSwap = hrm.GetMethod("SwapPreloadedContext", BindingFlags.Public | BindingFlags.Static)!;
            int preRc = (int)miPreload.Invoke(null, new object?[]{ asm, null })!;
            Assert.That(preRc, Is.EqualTo(0));
            int swapRc = (int)miSwap.Invoke(null, null)!;
            Assert.That(swapRc, Is.EqualTo(0));
            Assert.That(CallScript("HotReloadTest.TestMethod"), Is.EqualTo(7));
        }

        /// <summary>Preload from bytes with PDB then swap; method should execute.</summary>

        [Test]
        public void PreloadBytes_WithPdb_ThenSwap_InvokesMethod()
        {
            EnsureHrmLoaded();
            var cbType = GetCoreBridgeType();
            cbType.GetMethod("Demo_InitializeManaged", BindingFlags.Public | BindingFlags.Static)!.Invoke(null, null);

            var (asm, pdb) = CompileSimpleAssemblyReturning(9);
            var hrm = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: true)!;
            var miPreload = hrm.GetMethod("PreloadAssemblyContext", BindingFlags.Public | BindingFlags.Static, binder: null, types: new[]{ typeof(byte[]), typeof(byte[]) }, modifiers: null)!;
            var miSwap = hrm.GetMethod("SwapPreloadedContext", BindingFlags.Public | BindingFlags.Static)!;
            int preRc = (int)miPreload.Invoke(null, new object?[]{ asm, pdb })!;
            Assert.That(preRc, Is.EqualTo(0));
            int swapRc = (int)miSwap.Invoke(null, null)!;
            Assert.That(swapRc, Is.EqualTo(0));
            Assert.That(CallScript("HotReloadTest.TestMethod"), Is.EqualTo(9));
        }
    }
}

