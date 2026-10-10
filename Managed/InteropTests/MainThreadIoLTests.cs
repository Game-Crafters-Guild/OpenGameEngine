using System;
using System.Reflection;
using System.Threading;
using NUnit.Framework;
using GameEngine.HotReload;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// B5: with a main-thread pump registered, [InitializeOnLoad] is queued and runs on
    /// the thread that drains the pump (the engine main thread in the editor) instead of
    /// a thread-pool thread concurrent with rendering.
    /// </summary>
    public class MainThreadIoLTests
    {
        /// <summary>IoL must not run until the pump thread drains, and must run on that thread.</summary>
        [Test]
        public void IoL_RunsOnPumpDrainThread_WhenPumpRegistered()
        {
            ScriptCompileHelper.EnsureHrmLoaded();

            int pumpRequests = 0;
            Assert.That(HotReloadManager.SetMainThreadPump(() => Interlocked.Increment(ref pumpRequests)), Is.EqualTo(0));
            try
            {
                var src =
                    "using System;\n" +
                    "namespace GameEngine.Scripting { [AttributeUsage(AttributeTargets.Method)] public sealed class InitializeOnLoadAttribute : Attribute { } }\n" +
                    "namespace TestScripts { public static class MainThreadInit { public static int Marker; public static int ThreadId;\n" +
                    "  [GameEngine.Scripting.InitializeOnLoad] public static void Boot() { Marker++; ThreadId = Environment.CurrentManagedThreadId; } } }";
                var (asm, pdb) = ScriptCompileHelper.CompileCustomSources(src,
                    assemblyName: "MainThreadIoLScripts_" + Guid.NewGuid().ToString("N"));

                Assert.That(HotReloadManager.PreloadAssemblyContext(asm, pdb), Is.EqualTo(0));
                Assert.That(HotReloadManager.SwapPreloadedContext(), Is.EqualTo(0));

                var initType = FindType("TestScripts.MainThreadInit");
                Assert.That(initType, Is.Not.Null);

                // Queued, not run: the swap returned but no one drained the pump yet.
                Assert.That(pumpRequests, Is.GreaterThanOrEqualTo(1), "pump was not requested at swap");
                Assert.That(ReadInt(initType!, "Marker"), Is.EqualTo(0), "IoL ran before the pump drained");

                HotReloadManager.DrainMainThreadWork();

                Assert.That(ReadInt(initType!, "Marker"), Is.EqualTo(1), "IoL did not run on drain");
                Assert.That(ReadInt(initType!, "ThreadId"), Is.EqualTo(Environment.CurrentManagedThreadId),
                    "IoL ran on a different thread than the pump drainer");
            }
            finally
            {
                HotReloadManager.SetMainThreadPump(null);
            }
        }

        private static Type? FindType(string fullName)
        {
            foreach (var a in HotReloadManager.GetActiveUserAssemblies())
            {
                var t = a.GetType(fullName, throwOnError: false);
                if (t != null) return t;
            }
            return null;
        }

        private static int ReadInt(Type t, string fieldName)
        {
            var f = t.GetField(fieldName, BindingFlags.Public | BindingFlags.Static)!;
            return (int)(f.GetValue(null) ?? -1);
        }
    }
}
