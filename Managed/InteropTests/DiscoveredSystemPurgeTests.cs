using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;
using System.Threading;
using NUnit.Framework;
using GameEngine.HotReload;
using GameEngine.Scripting.Runtime;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// A GameSystem the source generator never registered (no partial declaration, so no
    /// generated [ModuleInitializer]) reaches the runner through reflection discovery. It
    /// must be stamped with its owning ALC exactly like a registered one: when that scripts
    /// context unloads, the live instance is destroyed and stops pinning the context.
    /// Without the stamp the entry outlives its context — the ALC leaks, and the entry keeps
    /// holding the system's name, which makes the post-swap discovery pass skip the
    /// replacement type so a mid-play hot reload never restarts the system.
    /// </summary>
    public class DiscoveredSystemPurgeTests
    {
        /// <summary>
        /// Discover a system from a collectible ALC, unload that ALC, assert the instance
        /// received OnDestroy and the context is collected.
        /// </summary>
        [Test]
        public void UnloadedDomain_DestroysDiscoveredSystemAndReleasesAlc()
        {
            ScriptCompileHelper.EnsureHrmLoaded();

            string typeName = "DiscoveredProbe_" + Guid.NewGuid().ToString("N");
            AppDomain.CurrentDomain.SetData(CreateKey(typeName), 0);
            AppDomain.CurrentDomain.SetData(DestroyKey(typeName), 0);

            // Initialize() is re-entry guarded, so the runner has to be down before this test
            // can drive a discovery pass of its own.
            GameSystemRunner.Shutdown();

            var (domain, weakAlc) = LoadProbeAssembly(typeName);
            try
            {
                GameSystemRunner.Initialize(0);
                Assert.That(Count(CreateKey(typeName)), Is.EqualTo(1),
                    "discovery must instantiate and OnCreate the probe system");

                Assert.That(HotReloadManager.UnloadDomain((ulong)domain), Is.EqualTo(0));

                // The purge fires synchronously from ALC.Unloading during Unload().
                Assert.That(Count(DestroyKey(typeName)), Is.EqualTo(1),
                    "a discovered system must be destroyed when its scripts context unloads");

                for (int i = 0; i < 30 && weakAlc.IsAlive; i++)
                {
                    GC.Collect();
                    GC.WaitForPendingFinalizers();
                    Thread.Sleep(50);
                }
                Assert.That(weakAlc.IsAlive, Is.False,
                    "collectible ALC still pinned after unload (discovered system instance survived)");
            }
            finally
            {
                GameSystemRunner.Shutdown();
            }
        }

        private static string CreateKey(string typeName) => typeName + ".create";
        private static string DestroyKey(string typeName) => typeName + ".destroy";

        private static int Count(string key)
            => AppDomain.CurrentDomain.GetData(key) is int i ? i : 0;

        /// <summary>
        /// Compiles and loads a scripts assembly holding one GameSystem subclass with no
        /// generated registration — the shape reflection discovery exists for. Lifecycle
        /// counts go through AppDomain data so the probe reports across the ALC boundary
        /// without a static that would itself pin something. NoInlining keeps the strong
        /// assembly/type locals out of the caller's frame so the context can be collected.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        private static (int domain, WeakReference weakAlc) LoadProbeAssembly(string typeName)
        {
            string source = $@"
using GameEngine.Scripting;
public class {typeName} : GameSystem
{{
    public override void OnCreate() => Bump(""{CreateKey(typeName)}"");
    public override void OnDestroy() => Bump(""{DestroyKey(typeName)}"");
    private static void Bump(string key)
    {{
        object v = System.AppDomain.CurrentDomain.GetData(key);
        System.AppDomain.CurrentDomain.SetData(key, (v is int i ? i : 0) + 1);
    }}
}}";

            var (asm, _) = ScriptCompileHelper.CompileCustomSources(
                source,
                assemblyName: "DiscoveredProbeScripts_" + Guid.NewGuid().ToString("N"),
                extraReferencePaths: new[] { typeof(GameEngine.Scripting.GameSystem).Assembly.Location });

            int domain = HotReloadManager.LoadCompiledAssembly(asm);
            Assert.That(domain, Is.GreaterThan(0));

            Assembly? loaded = null;
            foreach (var a in HotReloadManager.GetActiveUserAssemblies())
            {
                if (a.GetType(typeName, throwOnError: false) != null)
                {
                    loaded = a;
                    break;
                }
            }
            Assert.That(loaded, Is.Not.Null, $"{typeName} not found in loaded user assemblies");

            var alc = AssemblyLoadContext.GetLoadContext(loaded!);
            Assert.That(alc, Is.Not.Null);
            Assert.That(alc!.IsCollectible, Is.True, "probe assembly did not load into a collectible ALC");
            return (domain, new WeakReference(alc));
        }
    }
}
