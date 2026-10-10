using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.Loader;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Emit;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Compiles tiny BCL-only test scripts in memory with Roslyn. Production script
    /// compilation is owned by CompileServerHost; this helper exists purely so hot-reload
    /// tests can produce assembly bytes without shelling out.
    /// </summary>
    internal static class ScriptCompileHelper
    {
        private static readonly string[] s_BclReferenceNames =
        {
            "System.Runtime.dll",
            "System.Console.dll",
            "System.Collections.dll",
            "System.Linq.dll",
            "System.Threading.dll",
            "System.Threading.Tasks.dll",
            "System.IO.dll",
            "System.Runtime.Extensions.dll",
            "netstandard.dll",
        };

        public static (byte[] asm, byte[]? pdb) CompileAssemblyReturning(int value)
        {
            return CompileCustomSources($"public static class HotReloadTest {{ public static int TestMethod() => {value}; }}");
        }

        public static (byte[] asm, byte[]? pdb) CompileCustomSources(string csSource,
            string assemblyName = "TestScripts", IEnumerable<string>? extraReferencePaths = null)
        {
            var tree = CSharpSyntaxTree.ParseText(csSource);

            var refs = new List<MetadataReference>
            {
                MetadataReference.CreateFromFile(typeof(object).Assembly.Location)
            };
            var runtimeDir = Path.GetDirectoryName(typeof(object).Assembly.Location) ?? string.Empty;
            foreach (var name in s_BclReferenceNames)
            {
                var path = Path.Combine(runtimeDir, name);
                if (File.Exists(path))
                    refs.Add(MetadataReference.CreateFromFile(path));
            }
            if (extraReferencePaths != null)
            {
                foreach (var path in extraReferencePaths)
                    refs.Add(MetadataReference.CreateFromFile(path));
            }

            var compilation = CSharpCompilation.Create(
                assemblyName: assemblyName,
                syntaxTrees: new[] { tree },
                references: refs,
                options: new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, optimizationLevel: OptimizationLevel.Debug));

            using var peStream = new MemoryStream();
            using var pdbStream = new MemoryStream();
            var emit = compilation.Emit(peStream, pdbStream,
                options: new EmitOptions(debugInformationFormat: DebugInformationFormat.PortablePdb));
            if (!emit.Success)
            {
                var errors = string.Join("; ", emit.Diagnostics
                    .Where(d => d.Severity == DiagnosticSeverity.Error)
                    .Select(d => d.ToString()));
                throw new InvalidOperationException($"Compilation failed: {errors}");
            }
            return (peStream.ToArray(), pdbStream.ToArray());
        }

        public static void EnsureHrmLoaded()
        {
            var t = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: false);
            if (t != null) return;
            var baseDir = AppContext.BaseDirectory ?? Environment.CurrentDirectory;
            var built = Path.Combine(baseDir, "GameEngine.HotReload.dll");
            if (!File.Exists(built)) built = Path.GetFullPath(Path.Combine(Environment.CurrentDirectory, "HotReload", "bin", "Debug", "net10.0", "GameEngine.HotReload.dll"));
            if (!File.Exists(built)) throw new FileNotFoundException("HotReload assembly not found", built);
            AssemblyLoadContext.Default.LoadFromAssemblyPath(built);
        }
    }
}
