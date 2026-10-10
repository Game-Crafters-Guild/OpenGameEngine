using System;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;
using System.Threading;

namespace GameEngine.CoreBridge;

/// <summary>
/// Installs an <see cref="AssemblyLoadContext.Default"/>.Resolving handler that probes
/// CoreBridge's own directory for GameEngine.* managed dependencies.
///
/// The native host loads CoreBridge (and every other bridge assembly) into the Default
/// ALC via hostfxr's hdt_load_assembly, which carries no deps.json context — without
/// this handler Default could not resolve GameEngine.Scripting.Runtime / *.ABI /
/// GameEngine.HotReload and the first dependency touch of any UCO would fail. Keeping
/// everything in Default gives one statics universe: generated [ModuleInitializer]
/// registrations and the code that ticks systems share the same statics (B4).
/// </summary>
internal static class DefaultAlcResolver
{
    private static int s_installed;

    [ModuleInitializer]
    internal static void Install()
    {
        try
        {
            // Only meaningful when CoreBridge itself lives in the Default ALC (the
            // native single-universe host path). Isolated/test hosts resolve their own
            // dependencies via deps.json.
            if (AssemblyLoadContext.GetLoadContext(typeof(DefaultAlcResolver).Assembly) != AssemblyLoadContext.Default)
                return;
            if (Interlocked.Exchange(ref s_installed, 1) != 0)
                return;

            string? dir = Path.GetDirectoryName(typeof(DefaultAlcResolver).Assembly.Location);
            if (string.IsNullOrEmpty(dir))
                return; // byte-loaded CoreBridge has no location to probe

            AssemblyLoadContext.Default.Resolving += (alc, name) =>
            {
                try
                {
                    var simple = name.Name;
                    if (string.IsNullOrEmpty(simple) || !simple.StartsWith("GameEngine.", StringComparison.Ordinal))
                        return null;
                    var candidate = Path.Combine(dir!, simple + ".dll");
                    return File.Exists(candidate) ? alc.LoadFromAssemblyPath(candidate) : null;
                }
                catch
                {
                    return null;
                }
            };
        }
        catch
        {
            // Resolution falls back to default probing; failures surface at first bind.
        }
    }
}
