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
    /// B2: registrations in GameSystemRunner are stamped with their owning ALC and
    /// purged when that ALC unloads, so a deleted or renamed system's stale delegate
    /// cannot pin the old collectible context forever.
    /// </summary>
    public class GameSystemRegistrationPurgeTests
    {
        private const string kSources = @"
public static class PurgeSystems
{
    public static void ExecuteA(ulong world, float dt) { }
    public static void DestroyA() { }
    public static void ExecuteB(ulong world, float dt) { }
    public static void DestroyB() { }
}";

        /// <summary>Register system A + B from a collectible ALC, unload, assert both purged and the ALC dies.</summary>
        [Test]
        public void UnloadedDomain_PurgesRegistrationsAndReleasesAlc()
        {
            ScriptCompileHelper.EnsureHrmLoaded();

            string nameA = "PurgeTest.SystemA." + Guid.NewGuid().ToString("N");
            string nameB = "PurgeTest.SystemB." + Guid.NewGuid().ToString("N");

            var (domain, weakAlc) = LoadAndRegister(nameA, nameB);
            Assert.That(domain, Is.GreaterThan(0));
            Assert.That(GameSystemRunner.IsSystemRegistered(nameA), Is.True, "system A not registered");
            Assert.That(GameSystemRunner.IsSystemRegistered(nameB), Is.True, "system B not registered");

            Assert.That(HotReloadManager.UnloadDomain((ulong)domain), Is.EqualTo(0));

            // Purge fires synchronously from ALC.Unloading during Unload().
            Assert.That(GameSystemRunner.IsSystemRegistered(nameA), Is.False, "system A survived domain unload");
            Assert.That(GameSystemRunner.IsSystemRegistered(nameB), Is.False, "system B survived domain unload");

            // With the registrations gone, nothing pins the context: its WeakReference must die.
            for (int i = 0; i < 30 && weakAlc.IsAlive; i++)
            {
                GC.Collect();
                GC.WaitForPendingFinalizers();
                Thread.Sleep(50);
            }
            Assert.That(weakAlc.IsAlive, Is.False, "collectible ALC still alive after registration purge");
        }

        /// <summary>
        /// Loads the test assembly into a fresh domain and registers two entity systems
        /// with delegates that point into the collectible ALC. NoInlining keeps strong
        /// locals (assembly, delegates) out of the caller's frame so collection can occur.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        private static (int domain, WeakReference weakAlc) LoadAndRegister(string nameA, string nameB)
        {
            var (asm, _) = ScriptCompileHelper.CompileCustomSources(
                kSources, assemblyName: "PurgeTestScripts_" + Guid.NewGuid().ToString("N"));
            int domain = HotReloadManager.LoadCompiledAssembly(asm);
            Assert.That(domain, Is.GreaterThan(0));

            Assembly? loaded = null;
            foreach (var a in HotReloadManager.GetActiveUserAssemblies())
            {
                if (a.GetType("PurgeSystems", throwOnError: false) != null)
                {
                    loaded = a;
                    break;
                }
            }
            Assert.That(loaded, Is.Not.Null, "PurgeSystems type not found in loaded user assemblies");

            var type = loaded!.GetType("PurgeSystems")!;
            var execA = (Action<ulong, float>)Delegate.CreateDelegate(typeof(Action<ulong, float>), type.GetMethod("ExecuteA")!);
            var destA = (Action)Delegate.CreateDelegate(typeof(Action), type.GetMethod("DestroyA")!);
            var execB = (Action<ulong, float>)Delegate.CreateDelegate(typeof(Action<ulong, float>), type.GetMethod("ExecuteB")!);
            var destB = (Action)Delegate.CreateDelegate(typeof(Action), type.GetMethod("DestroyB")!);

            GameSystemRunner.RegisterEntitySystem(nameA, 0, execA, destA);
            GameSystemRunner.RegisterEntitySystem(nameB, 0, execB, destB);

            var alc = AssemblyLoadContext.GetLoadContext(loaded);
            Assert.That(alc, Is.Not.Null);
            Assert.That(alc!.IsCollectible, Is.True, "test assembly did not load into a collectible ALC");
            return (domain, new WeakReference(alc));
        }
    }
}
