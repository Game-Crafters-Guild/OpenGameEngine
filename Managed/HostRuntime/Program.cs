using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using GameEngine.CoreBridge;
using GameEngine.Interop;

unsafe class Program
{
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    static void OnComp(int stage, float p, void* diags, uint count, void* user)
    {
        Console.WriteLine($"[Diag] stage={stage} p={p}");
    }
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    static void OnReload(int stage, byte* reason, void* user)
    {
        Console.WriteLine($"[Reload] stage={stage}");
    }

    static int Main()
    {
        // Ensure native shim is on the probing path (copy alongside)
        try
        {
            var exeDir = AppContext.BaseDirectory;
            // Compute repo root: .../Managed/HostRuntime/bin/Debug/net8.0 -> 5 levels up
            var repoRoot = Path.GetFullPath(Path.Combine(exeDir, "..", "..", "..", "..", ".."));
            string[] candidates = new[] {
                Path.Combine(repoRoot, "Sandbox", "CoreBridgeDemo", "build", "Debug", "GameEngine.Native.dll")
            };
            var dest = Path.Combine(exeDir, "GameEngine.Native.dll");
            foreach (var c in candidates)
            {
                if (File.Exists(c)) { File.Copy(c, dest, overwrite: true); break; }
            }
            if (!File.Exists(dest)) { Console.WriteLine("[HostRuntime] Warning: GameEngine.Native.dll not found. Build the demo native shim first."); return 2; }
        } catch (Exception ex) { Console.WriteLine($"[HostRuntime] Copy failed: {ex.Message}"); return 3; }

        // Initialize CoreBridge managed side (Console redirection and HRM.Initialize if available)
        if (CoreBridge.Demo_InitializeManaged() != 0) return 1;

        // Register diagnostics via EngineNativeBinding
        var binding = EngineNativeBinding.LoadFrom(Path.Combine(AppContext.BaseDirectory, "GameEngine.Native.dll"));
        var compPtr = (nint)(delegate* unmanaged[Cdecl]<int, float, void*, uint, void*, void>)&OnComp;
        var reloadPtr = (nint)(delegate* unmanaged[Cdecl]<int, byte*, void*, void>)&OnReload;
        var sink = new GE_DiagnosticsSink(compPtr, reloadPtr, IntPtr.Zero);
        binding.TrySubscribeDiagnostics(in sink);

        // UTF-8 logging test with non-ASCII
        string text = "Hello, 世界";
        var bytes = System.Text.Encoding.UTF8.GetBytes(text);
        fixed (byte* p = bytes) { binding.Log!(1, (nint)p, (uint)bytes.Length); }

        // Load HotReload assembly next to CoreBridge via reflection in CoreBridge
        var scriptPath = System.IO.Path.GetFullPath("Sandbox/CoreBridgeDemo/Scripts/bin/Debug/net8.0/SimpleScript.dll");
        var rc = CoreBridge.Demo_LoadUserScriptsAssembly(scriptPath); // HRM is now the production one
        Console.WriteLine($"LoadUserScriptsAssembly => {rc}");

        var rr = CoreBridge.Demo_ReloadUserScriptsAssembly();
        Console.WriteLine($"ReloadUserScriptsAssembly => {rr}");

        var ur = CoreBridge.Demo_UnloadUserScriptsAssembly();
        Console.WriteLine($"UnloadUserScriptsAssembly => {ur}");
        return 0;
    }
}

