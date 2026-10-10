using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace GameEngine.Editor.Managed;

/// <summary>
/// Native-callable exports for Play Mode lifecycle.
///
/// The native Editor binds these function pointers via CoreCLRHost's
/// load_assembly_and_get_function_pointer and calls them directly (no reflection per-frame).
/// </summary>
public static class PlayModeDriverExports
{
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int OnEnter()
    {
        return PlayModeDriver.OnEnter();
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int OnExit()
    {
        return PlayModeDriver.OnExit();
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int OnTick(float deltaSeconds)
    {
        return PlayModeDriver.Tick(deltaSeconds);
    }
}

