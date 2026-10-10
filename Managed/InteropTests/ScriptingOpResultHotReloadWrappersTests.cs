using NUnit.Framework;
using CB = GameEngine.CoreBridge.CoreBridge;
using R = GameEngine.CoreBridge.ScriptingOpResult;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Smoke tests for managed ScriptingOpResult wrappers around HotReload operations.
    /// </summary>
    public class ScriptingOpResultHotReloadWrappersTests
    {
        /// <summary>
        /// Load → Reload → Unload flow via managed wrappers should return Ok when HRM is present.
        /// Test is skipped if HRM assembly is not available in the test run context.
        /// </summary>
        [Test]
        public void LoadReloadUnload_Wrappers_Work_WhenHrmAvailable()
        {
            // Ensure HRM is present or skip
            try { ScriptCompileHelper.EnsureHrmLoaded(); }
            catch { Assert.Ignore("HotReload assembly not available in this environment"); }

            // Reset state: best-effort unload
            var pre = CB.UnloadUserScripts();
            Assert.That(pre == R.Ok || pre == R.NotFound);

            // Initialize CoreBridge managed side
            var cbType = System.Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge", throwOnError: false)!;
            cbType.GetMethod("Demo_InitializeManaged", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static)!.Invoke(null, null);

            // Preload + swap via HRM (consistent with existing HotReload tests)
            var (asm, pdb) = ScriptCompileHelper.CompileAssemblyReturning(7);
            var hrm = System.Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: true)!;
            var miPreload = hrm.GetMethod("PreloadAssemblyContext", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static, binder: null, types: new[]{ typeof(byte[]), typeof(byte[]) }, modifiers: null)!;
            var miSwap = hrm.GetMethod("SwapPreloadedContext", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static)!;
            int preRc = (int)miPreload.Invoke(null, new object?[]{ asm, pdb })!;
            Assert.That(preRc, Is.EqualTo(0));
            int swapRc = (int)miSwap.Invoke(null, null)!;
            Assert.That(swapRc, Is.EqualTo(0));

            // Now exercise managed wrappers on a loaded context
            var rel = CB.ReloadUserScripts();
            // Reload may be InvalidArg if assembly was loaded from bytes without a persisted path
            Assert.That(rel == R.Ok || rel == R.InvalidArg);

            var un = CB.UnloadUserScripts();
            Assert.That(un, Is.EqualTo(R.Ok));
        }

        /// <summary>
        /// A script path with no file behind it is NotFound, from HotReloadManager and through
        /// the CoreBridge wrapper alike.
        /// </summary>
        [Test]
        public void LoadFromPath_MissingFile_IsNotFound()
        {
            string missing = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "ge-missing-" + System.Guid.NewGuid().ToString("N") + ".dll");

            Assert.That(GameEngine.HotReload.HotReloadManager.LoadUserScriptsAssembly(missing), Is.EqualTo((int)R.NotFound));
            Assert.That(CB.LoadUserScriptsFromPath(missing), Is.EqualTo(R.NotFound));
        }
    }
}

