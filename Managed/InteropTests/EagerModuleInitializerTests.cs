using System;
using NUnit.Framework;
using GameEngine.HotReload;
using GameEngine.Scripting.Runtime;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// The runtime only runs [ModuleInitializer]s on first code execution, so a swapped
    /// assembly's generated registrations used to stay invisible until play-enter (and the
    /// old domain's stale registrations kept its ALC pinned). HotReloadManager now runs
    /// module constructors eagerly at swap publish / load.
    /// </summary>
    public class EagerModuleInitializerTests
    {
        /// <summary>Swap publish must surface [ModuleInitializer] registrations without any invoke into the assembly.</summary>
        [Test]
        public void SwapPublish_RunsModuleInitializers_WithoutInvoke()
        {
            ScriptCompileHelper.EnsureHrmLoaded();

            string name = "EagerInit.System." + Guid.NewGuid().ToString("N");
            string src = $@"
using System.Runtime.CompilerServices;
using GameEngine.Scripting.Runtime;
public static class EagerInitBoot
{{
    [ModuleInitializer]
    internal static void Boot()
    {{
        GameSystemRunner.RegisterEntitySystem(""{name}"", 0, static (w, dt) => {{ }}, static () => {{ }});
    }}
}}";
            var (asm, pdb) = ScriptCompileHelper.CompileCustomSources(src,
                assemblyName: "EagerInitScripts_" + Guid.NewGuid().ToString("N"),
                extraReferencePaths: new[] { typeof(GameSystemRunner).Assembly.Location });

            Assert.That(HotReloadManager.PreloadAssemblyContext(asm, pdb), Is.EqualTo(0));
            Assert.That(GameSystemRunner.IsSystemRegistered(name), Is.False,
                "module initializer ran at preload; it must run at swap publish");

            Assert.That(HotReloadManager.SwapPreloadedContext(), Is.EqualTo(0));
            Assert.That(GameSystemRunner.IsSystemRegistered(name), Is.True,
                "registration not visible after swap without invoking the assembly");
        }

        /// <summary>LoadCompiledAssembly (legacy/test path) surfaces registrations the same way.</summary>
        [Test]
        public void LoadCompiledAssembly_RunsModuleInitializers_WithoutInvoke()
        {
            ScriptCompileHelper.EnsureHrmLoaded();

            string name = "EagerInit.LoadPath." + Guid.NewGuid().ToString("N");
            string src = $@"
using System.Runtime.CompilerServices;
using GameEngine.Scripting.Runtime;
public static class EagerInitLoadBoot
{{
    [ModuleInitializer]
    internal static void Boot()
    {{
        GameSystemRunner.RegisterEntitySystem(""{name}"", 0, static (w, dt) => {{ }}, static () => {{ }});
    }}
}}";
            var (asm, _) = ScriptCompileHelper.CompileCustomSources(src,
                assemblyName: "EagerInitLoadScripts_" + Guid.NewGuid().ToString("N"),
                extraReferencePaths: new[] { typeof(GameSystemRunner).Assembly.Location });

            int domain = HotReloadManager.LoadCompiledAssembly(asm);
            Assert.That(domain, Is.GreaterThan(0));
            Assert.That(GameSystemRunner.IsSystemRegistered(name), Is.True,
                "registration not visible after load without invoking the assembly");

            Assert.That(HotReloadManager.UnloadDomain((ulong)domain), Is.EqualTo(0));
            Assert.That(GameSystemRunner.IsSystemRegistered(name), Is.False,
                "registration survived domain unload (B2 purge)");
        }
    }
}
