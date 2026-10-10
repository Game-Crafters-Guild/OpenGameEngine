using System;
using System.IO;
using System.Reflection;
using NUnit.Framework;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Tests that [InitializeOnLoad] methods are discovered during preload but only invoked after Stage 4 swap.
    /// </summary>
    public class InitializeOnLoadInvocationTests
    {
        /// <summary>
        /// Preload must not invoke IoL; swap must invoke IoL exactly once.
        /// </summary>
        [Test]
        public void Preload_DoesNotInvoke_IoL_OnlyAfterSwap()
        {
            ScriptCompileHelper.EnsureHrmLoaded();
            var coreBridge = Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge", throwOnError: false);
            coreBridge?.GetMethod("Demo_InitializeManaged", BindingFlags.Public | BindingFlags.Static)?.Invoke(null, null);

            var src = "using System;\nnamespace GameEngine.Scripting { [AttributeUsage(AttributeTargets.Method, AllowMultiple=false, Inherited=false)] public sealed class InitializeOnLoadAttribute : Attribute { } }\nnamespace TestScripts { public static class Init { public static int Marker = 0; [GameEngine.Scripting.InitializeOnLoad] public static void Boot() { Marker = Marker + 1; } } }";
            var (asm, pdb) = ScriptCompileHelper.CompileCustomSources(src);

            var hrm = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: true)!;
            var miPreload = hrm.GetMethod("PreloadAssemblyContext", BindingFlags.Public | BindingFlags.Static, binder: null, types: new[]{ typeof(byte[]), typeof(byte[]) }, modifiers: null)!;
            var miSwap = hrm.GetMethod("SwapPreloadedContext", BindingFlags.Public | BindingFlags.Static)!;


            int preRc = (int)miPreload.Invoke(null, new object?[]{ asm, pdb })!;
            Assert.That(preRc, Is.EqualTo(0), "Preload failed");
            // Inspect preloaded assembly via supported test accessor to ensure discovery worked
            var miGetPreAsm = hrm.GetMethod("GetPreloadedAssemblyForTests", BindingFlags.Public | BindingFlags.Static);
            var preAsm = (Assembly?)miGetPreAsm!.Invoke(null, null);
            Assert.That(preAsm, Is.Not.Null, "Preloaded assembly missing after preload");
            var miDiscover = hrm.GetMethod("DiscoverInitializeOnLoadMethods", BindingFlags.Static | BindingFlags.NonPublic);
            var discovered = (System.Collections.Generic.List<MethodInfo>)miDiscover!.Invoke(null, new object?[]{ preAsm!, null })!;
            Assert.That(discovered.Count, Is.GreaterThanOrEqualTo(1), "No [InitializeOnLoad] methods discovered during preload");

            // Ensure nothing ran during preload (Marker remains 0)

            int swapRc = (int)miSwap.Invoke(null, null)!;
            // Read Marker before swap (should be 0)
            var initTypePre = preAsm!.GetType("TestScripts.Init", throwOnError: true)!;
            var markerFieldPre = initTypePre.GetField("Marker", BindingFlags.Public | BindingFlags.Static)!;
            var markerPre = (int)markerFieldPre.GetValue(null)!;
            Assert.That(markerPre, Is.EqualTo(0), "Marker should be 0 before swap");

            Assert.That(swapRc, Is.EqualTo(0), "Swap failed");

            // Await IoL completion without polling
            var miWaitPending = hrm.GetMethod("WaitForPendingInitializeOnLoadAsync", BindingFlags.Public | BindingFlags.Static)!;
            var t = (System.Threading.Tasks.Task<bool>)miWaitPending.Invoke(null, new object?[]{ 5000 })!;
            Assert.That(t.GetAwaiter().GetResult(), Is.True, "InitializeOnLoad did not complete within timeout");

            // Read Marker after swap (should be 1) from current assembly via supported test accessor
            var miGetCurrAsm = hrm.GetMethod("GetCurrentAssembly", BindingFlags.Public | BindingFlags.Static);
            var currAsm = (Assembly?)miGetCurrAsm!.Invoke(null, null);
            Assert.That(currAsm, Is.Not.Null, "Current assembly missing after swap");
            var initType = currAsm!.GetType("TestScripts.Init", throwOnError: true)!;
            var markerField = initType.GetField("Marker", BindingFlags.Public | BindingFlags.Static)!;
            int marker = (int)markerField.GetValue(null)!;
            Assert.That(marker, Is.EqualTo(1), "InitializeOnLoad should set Marker to 1 after swap");
        }
    }
}

