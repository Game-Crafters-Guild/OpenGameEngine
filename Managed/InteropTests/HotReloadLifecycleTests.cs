using System;
using System.Reflection;
using System.Runtime.Loader;
using NUnit.Framework;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>Lifecycle tests for HRM domains and tokens.</summary>

    public class HotReloadLifecycleTests
    {
        private static Type GetHrm()
        {
            var t = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: false);
            if (t == null)
            {
                // Try to load from bin if not resolved
                var baseDir = AppContext.BaseDirectory ?? Environment.CurrentDirectory;
                var path = System.IO.Path.Combine(baseDir, "GameEngine.HotReload.dll");
                if (!System.IO.File.Exists(path)) path = System.IO.Path.GetFullPath(System.IO.Path.Combine(Environment.CurrentDirectory, "HotReload", "bin", "Debug", "net10.0", "GameEngine.HotReload.dll"));
                Assert.That(System.IO.File.Exists(path), Is.True, $"HotReload assembly not found: {path}");
                AssemblyLoadContext.Default.LoadFromAssemblyPath(path);
                t = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: true)!;
            }
            return t;
        }

        private static void Initialize()
        {
            var hrm = GetHrm();
            hrm.GetMethod("Initialize", BindingFlags.Public | BindingFlags.Static)!.Invoke(null, null);
        }

        private static (byte[] asm, byte[]? pdb) CompileAssemblyReturning(int value)
        {
            return ScriptCompileHelper.CompileAssemblyReturning(value);
        }
        /// <summary>Query token, unload current domain, then verify token invocation fails.</summary>


        [Test]
        public void TokenInvalidated_AfterUnload()
        {
            Initialize();
            var hrm = GetHrm();

            // Load first assembly and get domain id
            var (asm1, _) = CompileAssemblyReturning(1);
            var miLoadFromBytes = hrm.GetMethod("LoadCompiledAssembly", BindingFlags.Public | BindingFlags.Static, binder: null, types: new[]{ typeof(byte[]) }, modifiers: null)!;
            int dom = (int)miLoadFromBytes.Invoke(null, new object?[]{ asm1 })!;
            Assert.That(dom, Is.GreaterThan(0));

            // Query token for TestMethod (use byref via args array)
            var miQuery = hrm.GetMethod("QueryExportInDomain", BindingFlags.Public | BindingFlags.Static)!;
            object[] qargs = new object[] { (ulong)dom, "HotReloadTest.TestMethod", 0UL };
            int qrc = (int)miQuery.Invoke(null, qargs)!;
            Assert.That(qrc, Is.EqualTo(0));
            ulong tok = (ulong)qargs[2];

            // Unload and ensure token no longer invokable
            var miUnload = hrm.GetMethod("UnloadUserScriptsAssembly", BindingFlags.Public | BindingFlags.Static)!;
            int urc = (int)miUnload.Invoke(null, null)!;
            Assert.That(urc, Is.EqualTo(0));

            var miInvoke = hrm.GetMethod("InvokeByToken", BindingFlags.Public | BindingFlags.Static, binder: null, types: new[]{ typeof(ulong), typeof(ulong) }, modifiers: null)!;
            int rc = (int)miInvoke.Invoke(null, new object?[]{ (ulong)dom, tok })!;
            Assert.That(rc, Is.LessThan(0));
        }
    }
}

