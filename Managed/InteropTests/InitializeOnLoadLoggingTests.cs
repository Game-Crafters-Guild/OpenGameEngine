using System;
using System.Reflection;
using System.Threading.Tasks;
using NUnit.Framework;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Integration tests that verify [InitializeOnLoad] runs with console redirect installed
    /// both on the initial scripts load and on a subsequent hot reload swap.
    /// </summary>
    public class InitializeOnLoadLoggingTests
    {
        /// <summary>
        /// Verifies that [InitializeOnLoad] sees EngineLogWriter on both initial load and hot reload.
        /// </summary>
        [Test]
        public void InitializeOnLoad_SeesEngineLogWriter_OnInitialLoad_AndHotReload()
        {
            // Ensure HotReloadManager is loaded in the current AppDomain
            ScriptCompileHelper.EnsureHrmLoaded();

            // Reset any existing EngineLogWriter installation so we control when it is installed
            var coreBridgeType = Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge", throwOnError: true)!;
            var logWriterType = coreBridgeType.Assembly.GetType("GameEngine.CoreBridge.EngineLogWriter");
            var uninstall = logWriterType?.GetMethod("Uninstall", BindingFlags.Static | BindingFlags.NonPublic);
            uninstall?.Invoke(null, null);

            // At this point Console.Out should not be an EngineLogWriter
            var beforeType = Console.Out.GetType().FullName ?? string.Empty;
            Assert.That(beforeType, Does.Not.Contain("EngineLogWriter"), "Precondition: Console.Out should not already be redirected.");

            // Helper to drive preload+swap+IoL for a given source string and return the Init type
            static Type RunSwapWithSource(string csSource)
            {
                var hrm = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: true)!;
                var miPreload = hrm.GetMethod("PreloadAssemblyContext", BindingFlags.Public | BindingFlags.Static, null, new[] { typeof(byte[]), typeof(byte[]) }, null)!;
                var miSwap = hrm.GetMethod("SwapPreloadedContext", BindingFlags.Public | BindingFlags.Static)!;
                var miWait = hrm.GetMethod("WaitForPendingInitializeOnLoadAsync", BindingFlags.Public | BindingFlags.Static)!;
                var miGetCurrAsm = hrm.GetMethod("GetCurrentAssembly", BindingFlags.Public | BindingFlags.Static)!;

                var (asm, pdb) = ScriptCompileHelper.CompileCustomSources(csSource);

                int preRc = (int)miPreload.Invoke(null, new object?[] { asm, pdb })!;
                Assert.That(preRc, Is.EqualTo(0), "Preload failed");

                int swapRc = (int)miSwap.Invoke(null, null)!;
                Assert.That(swapRc, Is.EqualTo(0), "Swap failed");

                var t = (Task<bool>)miWait.Invoke(null, new object?[] { 5000 })!;
                Assert.That(t.GetAwaiter().GetResult(), Is.True, "InitializeOnLoad did not complete within timeout");

                var currAsm = (System.Reflection.Assembly?)miGetCurrAsm.Invoke(null, null);
                Assert.That(currAsm, Is.Not.Null, "Current assembly missing after swap");
                var initType = currAsm!.GetType("TestScripts.Init", throwOnError: true)!;
                return initType;
            }

            // Script template: defines [InitializeOnLoad] and records the Console.Out type seen inside IoL
            string MakeSource(string buildLabel) =>
                "using System;" +
                "\nnamespace GameEngine.Scripting {" +
                " [AttributeUsage(AttributeTargets.Method, AllowMultiple=false, Inherited=false)] public sealed class InitializeOnLoadAttribute : Attribute { } }" +
                "\nnamespace TestScripts {" +
                " public static class Init {" +
                "   public static int Marker = 0;" +
                "   public static string LastOutType = string.Empty;" +
                "   [GameEngine.Scripting.InitializeOnLoad]" +
                "   public static void Boot() {" +
                "     Marker++;" +
                "     var w = Console.Out;" +
                "     var t = w.GetType();" +
                "     while (t.FullName == \"System.IO.TextWriter+SyncTextWriter\") {" +
                "       var f = t.GetField(\"_out\", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic);" +
                "       if (f == null) break;" +
                "       w = (System.IO.TextWriter)f.GetValue(w);" +
                "       t = w.GetType();" +
                "     }" +
                "     LastOutType = t.FullName ?? string.Empty;" +
                "     Console.WriteLine(\"[IoL] Boot from \" + \"" + buildLabel + "\");" +
                "   }" +
                " }" +
                "}";

            // First load
            var initType1 = RunSwapWithSource(MakeSource("initial"));
            var markerField1 = initType1.GetField("Marker", BindingFlags.Public | BindingFlags.Static)!;
            var lastOutField1 = initType1.GetField("LastOutType", BindingFlags.Public | BindingFlags.Static)!;
            int marker1 = (int)markerField1.GetValue(null)!;
            string lastOut1 = (string)lastOutField1.GetValue(null)!;

            Assert.That(marker1, Is.EqualTo(1), "IoL Boot should have run exactly once on initial load.");
            Assert.That(lastOut1, Does.Contain("EngineLogWriter"), "Console.Out inside IoL on initial load should be EngineLogWriter or a wrapper.");

            // Hot reload (second swap with new assembly)
            var initType2 = RunSwapWithSource(MakeSource("hotreload"));
            var markerField2 = initType2.GetField("Marker", BindingFlags.Public | BindingFlags.Static)!;
            var lastOutField2 = initType2.GetField("LastOutType", BindingFlags.Public | BindingFlags.Static)!;
            int marker2 = (int)markerField2.GetValue(null)!;
            string lastOut2 = (string)lastOutField2.GetValue(null)!;

            Assert.That(marker2, Is.EqualTo(1), "IoL Boot should have run exactly once on hot reload.");
            Assert.That(lastOut2, Does.Contain("EngineLogWriter"), "Console.Out inside IoL on hot reload should be EngineLogWriter or a wrapper.");
        }

	        /// <summary>
	        /// Guardrail: multiple sequential swaps should continue to see EngineLogWriter inside IoL,
	        /// not a bare console writer, to mirror Editor-like long-running sessions.
	        /// </summary>
	        [Test]
	        public void InitializeOnLoad_SeesEngineLogWriter_AcrossMultipleSequentialSwaps()
	        {
	            ScriptCompileHelper.EnsureHrmLoaded();

	            static Type RunSwapWithSource(string csSource)
	            {
	                var hrm = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: true)!;
	                var miPreload = hrm.GetMethod("PreloadAssemblyContext", BindingFlags.Public | BindingFlags.Static, null, new[] { typeof(byte[]), typeof(byte[]) }, null)!;
	                var miSwap = hrm.GetMethod("SwapPreloadedContext", BindingFlags.Public | BindingFlags.Static)!;
	                var miWait = hrm.GetMethod("WaitForPendingInitializeOnLoadAsync", BindingFlags.Public | BindingFlags.Static)!;
	                var miGetCurrAsm = hrm.GetMethod("GetCurrentAssembly", BindingFlags.Public | BindingFlags.Static)!;

	                var (asm, pdb) = ScriptCompileHelper.CompileCustomSources(csSource);
	                int preRc = (int)miPreload.Invoke(null, new object?[] { asm, pdb })!;
	                Assert.That(preRc, Is.EqualTo(0), "Preload failed");

	                int swapRc = (int)miSwap.Invoke(null, null)!;
	                Assert.That(swapRc, Is.EqualTo(0), "Swap failed");

	                var t = (Task<bool>)miWait.Invoke(null, new object?[] { 5000 })!;
	                Assert.That(t.GetAwaiter().GetResult(), Is.True, "InitializeOnLoad did not complete within timeout");

	                var currAsm = (System.Reflection.Assembly?)miGetCurrAsm.Invoke(null, null);
	                Assert.That(currAsm, Is.Not.Null, "Current assembly missing after swap");
	                var initType = currAsm!.GetType("TestScripts.Init", throwOnError: true)!;
	                return initType;
	            }

	            string MakeSource(string label) =>
	                "using System;" +
	                "\nnamespace GameEngine.Scripting {" +
	                " [AttributeUsage(AttributeTargets.Method, AllowMultiple=false, Inherited=false)] public sealed class InitializeOnLoadAttribute : Attribute { } }" +
	                "\nnamespace TestScripts {" +
	                " public static class Init {" +
	                "   public static int Marker = 0;" +
	                "   public static string LastOutType = string.Empty;" +
	                "   [GameEngine.Scripting.InitializeOnLoad]" +
	                "   public static void Boot() {" +
	                "     Marker++;" +
	                "     var w = Console.Out;" +
	                "     var t = w.GetType();" +
	                "     while (t.FullName == \"System.IO.TextWriter+SyncTextWriter\") {" +
	                "       var f = t.GetField(\"_out\", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic);" +
	                "       if (f == null) break;" +
	                "       w = (System.IO.TextWriter)f.GetValue(w);" +
	                "       t = w.GetType();" +
	                "     }" +
	                "     LastOutType = t.FullName ?? string.Empty;" +
	                "     Console.WriteLine(\"[IoL] Boot from \" + \"" + label + "\");" +
	                "   }" +
	                " }" +
	                "}";

	            // Drive several sequential swaps to emulate a longer Editor session.
	            for (int i = 0; i < 3; ++i)
	            {
	                var initType = RunSwapWithSource(MakeSource($"seq_{i}"));
	                var markerField = initType.GetField("Marker", BindingFlags.Public | BindingFlags.Static)!;
	                var lastOutField = initType.GetField("LastOutType", BindingFlags.Public | BindingFlags.Static)!;

	                int marker = (int)markerField.GetValue(null)!;
	                string lastOut = (string)lastOutField.GetValue(null)!;

	                Assert.That(marker, Is.EqualTo(1), $"IoL Boot should run exactly once per swap (iteration {i}).");
	                Assert.That(lastOut, Does.Contain("EngineLogWriter"),
	                    $"Console.Out inside IoL after swap {i} should be EngineLogWriter or a wrapper.");
	            }
	        }
    }
}

