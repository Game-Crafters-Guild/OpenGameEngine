using System;
using System.Reflection;
using NUnit.Framework;
using GameEngine.HotReload;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// B5: before a scripts domain unloads, static [OnScriptsUnload] methods in that
    /// domain are invoked (time-boxed) so user code can stop threads/timers and
    /// unsubscribe from pinned events.
    /// </summary>
    public class OnScriptsUnloadTests
    {
        /// <summary>Handlers fire exactly once, before the ALC unload is initiated.</summary>
        [Test]
        public void UnloadBroadcast_InvokesHandlers()
        {
            ScriptCompileHelper.EnsureHrmLoaded();

            var src =
                "using System;\n" +
                "namespace GameEngine.Scripting { [AttributeUsage(AttributeTargets.Method)] public sealed class OnScriptsUnloadAttribute : Attribute { } }\n" +
                "namespace TestScripts { public static class UnloadProbe { public static int Marker; public static int ThrowerRan;\n" +
                "  [GameEngine.Scripting.OnScriptsUnload] internal static void Down() { Marker++; }\n" +
                "  [GameEngine.Scripting.OnScriptsUnload] public static void Thrower() { ThrowerRan++; throw new InvalidOperationException(\"boom\"); } } }";
            var (asm, _) = ScriptCompileHelper.CompileCustomSources(src,
                assemblyName: "OnScriptsUnloadScripts_" + Guid.NewGuid().ToString("N"));

            int domain = HotReloadManager.LoadCompiledAssembly(asm);
            Assert.That(domain, Is.GreaterThan(0));

            Type? probe = null;
            foreach (var a in HotReloadManager.GetActiveUserAssemblies())
            {
                probe = a.GetType("TestScripts.UnloadProbe", throwOnError: false);
                if (probe != null) break;
            }
            Assert.That(probe, Is.Not.Null);

            Assert.That(ReadInt(probe!, "Marker"), Is.EqualTo(0));
            Assert.That(HotReloadManager.UnloadDomain((ulong)domain), Is.EqualTo(0));

            // The broadcast ran synchronously during UnloadDomain, before ALC.Unload;
            // the held Type keeps the (unloading) assembly readable for the assert.
            Assert.That(ReadInt(probe!, "Marker"), Is.EqualTo(1), "OnScriptsUnload handler did not run");
            Assert.That(ReadInt(probe!, "ThrowerRan"), Is.EqualTo(1), "throwing handler should still have been invoked");
        }

        private static int ReadInt(Type t, string fieldName)
        {
            var f = t.GetField(fieldName, BindingFlags.Public | BindingFlags.Static)!;
            return (int)(f.GetValue(null) ?? -1);
        }
    }
}
